# SPDX-License-Identifier: GPL-2.0-or-later
"""Local interactive Z7 GUI. Original firmware keys through diagnostic GDB calls.

Runs under WSL with standard Python only. The browser sees live QMP screenshots.
Only the worker owns QMP/RSP; HTTP requests enqueue bounded, named operations.
"""
from pathlib import Path
import argparse
import hashlib
import http.server
import json
import os
import queue
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import threading
import tempfile
import traceback
import time
import zlib

import probe
from probe import call_fixture, Remote
from capture_diagnostics import action_completed, capture_health, capture_state
from shoulder_display import ShoulderService

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
BUILD_MANIFEST = PROJECT / '.local/build-manifest.json'
FIRMWARE = None
from raw_input import RawSequence
from cmos_config import active_manifest, active_ingress
SYSTEM_REPORT = bytes.fromhex('000100000200000000000000000000000000000002000000000000000000000000000000')
# ROM 10634144, 16-byte AO entries; masks independently checked against the
# ID_SW name table at file 0061ae8c and following entries (40-byte stride).
KEYS = {'right': 34, 'left': 35, 'up': 36, 'down': 37,
        'menu': 67, 'play': 68, 'delete': 69, 'i': 72, 'ok': 74}
LABELS = {'right': '右', 'left': '左 / 返回', 'up': '上', 'down': '下',
          'menu': 'MENU', 'play': '回放', 'delete': '删除', 'i': 'i', 'ok': 'OK'}


def ppm_pixels(data):
    # QEMU's screendump P6 header, followed by packed RGB bytes.
    magic, dimensions, maximum, pixels = data.split(b'\n', 3)
    w, h = map(int, dimensions.split())
    if magic != b'P6' or maximum != b'255' or (w, h) not in ((720, 480), (1024, 682)) or len(pixels) != w*h*3:
        raise ValueError('Unexpected QEMU screenshot format')
    return w, h, pixels


def rgb_png(w, h, pixels):
    def chunk(kind, payload):
        return struct.pack('>I', len(payload))+kind+payload+struct.pack('>I', zlib.crc32(kind+payload))
    rows = b''.join(b'\0'+pixels[y*w*3:(y+1)*w*3] for y in range(h))
    png = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    return png+chunk(b'IDAT', zlib.compress(rows, 1))+chunk(b'IEND', b'')


def ppm_png(data):
    w, h, pixels = ppm_pixels(data)
    return rgb_png(w, h, pixels), pixels


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class QMP:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), 5)
        self.file = self.socket.makefile('rwb', buffering=0)
        json.loads(self.file.readline())
        self.command('qmp_capabilities')

    def command(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None:
            request['arguments'] = arguments
        self.file.write(json.dumps(request).encode()+b'\n')
        while True:
            raw = self.file.readline()
            if not raw:
                raise EOFError('QEMU monitor disconnected')
            reply = json.loads(raw)
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            if 'return' in reply:
                return reply['return']

    def close(self):
        self.file.close()
        self.socket.close()


class Camera:
    def __init__(self, port, profile='menu', capture_trace=False, tg_test_pulse=False,
                 exmcu_test_device=False, capture_control_test=False, raw_source=None,
                 exmcu_no_lens=False, orientation_fixture=None, keep_awake=False,
                 tg_periodic_test=False, raw_ingress_test=None, tg_paired_test=False, isp_boundary_test=False,
                 photo_liveview=False, one_insn_per_tb=False, companion_firmware=None):
        # Fail before launching QEMU if any selected input is malformed.
        self.raw_source = RawSequence(raw_source) if raw_source else None
        self.port = port
        self.photo_liveview = photo_liveview
        self.one_insn_per_tb = one_insn_per_tb
        self.device_log_position = 0
        self.device_log_tail = ''
        self.device_evidence = {}
        self.preview_buffers = set()
        if photo_liveview:
            self.image_process_test = self.lcd_scanout_test = True
            self.icount_shift = 3
        self.profile = profile
        self.capture_trace = capture_trace
        self.capture_control_test = capture_control_test
        self.tg_test_pulse = tg_test_pulse or capture_control_test
        self.tg_periodic_test = tg_periodic_test
        self.tg_paired_test = tg_paired_test
        self.isp_boundary_test = isp_boundary_test
        self.raw_ingress_test = Path(raw_ingress_test).resolve(strict=True) if raw_ingress_test else None
        if self.raw_ingress_test and (not tg_periodic_test or self.raw_ingress_test.stat().st_size != 1909440):
            raise ValueError('RAW ingress experiment requires the timing test and a 1909440-byte stimulus')
        if isp_boundary_test and not self.raw_ingress_test:
            raise ValueError('ISP boundary experiment requires the RAW ingress test')
        self.exmcu_test_device = exmcu_test_device or capture_control_test or exmcu_no_lens
        self.exmcu_no_lens = exmcu_no_lens
        if orientation_fixture is not None and orientation_fixture not in range(4):
            raise ValueError('Orientation must be 0, 1, 2 or 3')
        self.orientation_fixture = orientation_fixture
        self.keep_awake = keep_awake
        self.token = secrets.token_urlsafe(24)
        self.lock = threading.Lock()
        self.requests = queue.Queue(maxsize=16)
        self.state = {'phase': 'booting', 'message': '正在启动虚拟相机', 'busy': True,
                      'frame': 0, 'last_key': None, 'completed_action': 0, 'error': None,
                      'no_lens_simulation': self.exmcu_no_lens,
                      'orientation_fixture': self.orientation_fixture,
                      'keep_awake': self.keep_awake, 'idle_timeout_override': None}
        self.shoulder = ShoulderService(companion_firmware)
        self.state['shoulder'] = dict(self.shoulder.status)
        self.shoulder_png = None
        self.png = None
        self.previous_pixels = None
        self.proc = self.remote = self.monitor = self.console = None
        self.running = False
        self.stamp = time.strftime('%Y%m%d-%H%M%S')+'-'+secrets.token_hex(2)
        self.directory = PROJECT/'.local/sessions'/self.stamp
        self.directory.mkdir(parents=True)
        self.audit_path = self.directory/'actions.jsonl'
        self.sequence = 0
        self.boot_count = 0
        self.refresh_samples = self.png_encodes = 0
        self.refresh_seconds = 0.0
        if self.raw_source:
            self.state['raw_input'] = self.raw_source.status()
            self.audit({'kind': 'raw_source_loaded', **self.raw_source.status()})

    def update(self, **values):
        with self.lock:
            self.state.update(values)

    def snapshot(self):
        with self.lock:
            return dict(self.state, queued=self.requests.qsize(), session=self.stamp)

    def audit(self, record):
        with self.audit_path.open('a', encoding='utf-8') as f:
            f.write(json.dumps(dict(time=time.time(), **record), ensure_ascii=False)+'\n')

    def submit(self, action, key=None):
        if action not in ('key', 'pause', 'resume', 'restart', 'shooting', 'menu-profile', 'stop') or (action == 'key' and key not in KEYS):
            raise ValueError('未知按键或操作')
        with self.lock:
            if self.state['phase'] == 'booting':
                raise RuntimeError('相机正在启动，请稍候')
            if action == 'key' and self.state['phase'] != 'running':
                raise RuntimeError('请先恢复运行')
            self.sequence += 1
            item = {'id': self.sequence, 'action': action, 'key': key}
            self.requests.put_nowait(item)
        return item

    def receive_stop(self):
        packet = self.remote.receive()
        if 'thread:' not in packet:
            raise RuntimeError('Guest stopped unexpectedly: '+packet)
        self.remote.command('Hg'+packet.split('thread:')[1].split(';')[0])
        self.running = False
        return packet

    def continue_guest(self):
        """Wait for RSP continue to reach QEMU before querying it as running.

        RSP and QMP use different sockets; an immediate query can still return
        debug (observed 1.5ms after MENU returned). Never issue a second
        continue to conceal an unexpected stop.
        """
        self.remote.send('c')
        until = time.monotonic() + 1
        while True:
            status = self.monitor.command('query-status')
            if status['running']:
                self.running = True
                return
            if time.monotonic() >= until:
                raise RuntimeError('QEMU did not acknowledge continue: ' + status['status'])
            time.sleep(.01)

    def run_to(self, address):
        assert not self.running
        assert self.remote.command(f'Z1,{address:x},2') == 'OK'
        self.remote.send('c'); self.running = True
        packet = self.receive_stop()
        regs = self.remote.registers()
        if int(regs['r15'], 16) != address:
            raise RuntimeError(f'Unexpected stop at {regs["r15"]}; expected {address:#x}')
        assert self.remote.command(f'z1,{address:x},2') == 'OK'
        return packet

    def call(self, packet, name, function, payload, arguments):
        self.update(message=name)
        result = call_fixture(self.remote, packet, function, payload, arguments)
        self.audit({'kind': 'original_firmware_call', 'name': name, 'result': result})
        return 'T05thread:'+result['return_cpu']+';'

    def key(self, packet, name):
        values = [0]*8
        index = KEYS[name]
        values[index//32] = 1 << (index%32)
        values[4] = 10   # Original AO press duration: 10*10 virtual milliseconds.
        return self.call(packet, '按键：'+LABELS[name], 0x105230a6,
                         struct.pack('<8I', *values), [('scratch', 0), 5, ('scratch', 20)])

    def boot(self, enter_menu=True, announce_ready=True):
        if os.environ.get('WSL_DISTRO_NAME') == 'ubuntu-noble':
            raise RuntimeError('Use the explicitly selected Nikon distribution')
        self.frame_spool = Path(tempfile.gettempdir())/'wonderlantern-z7'/self.stamp
        self.frame_spool.mkdir(parents=True, exist_ok=True)
        self.boot_started = time.perf_counter()
        if self.raw_source:
            self.raw_source = RawSequence(self.raw_source.path)
            self.update(raw_input=self.raw_source.status())
        self.shoulder.reset()
        with self.lock:
            self.shoulder_png = None
            self.state['shoulder'] = dict(self.shoulder.status)
        self.boot_count += 1
        self.device_log_position = 0
        self.device_log_tail = ''
        self.preview_buffers.clear()
        self.device_evidence = {'experimental': True, 'bit_accurate': False,
            'raw_width': 2780, 'raw_height': 932, 'raw_stride': 5568,
            'raw_bits': 12, 'raw_cfa': 'RGGB',
            'raw_batches': 0, 'isp_batches': 0, 'lcd_frames_observed': 0,
            'processed_buffer_scanned': False} if self.photo_liveview else {}
        self.update(phase='booting', message='正在加载原固件', busy=True, error=None, frame=0,
                    capture=None, profile=self.profile, idle_timeout_override=None)
        with self.lock:
            self.png = self.previous_pixels = None
        build = json.loads((BUILD_MANIFEST).read_text())
        if hashlib.sha256(Path(build['binary']).read_bytes()).hexdigest() != build['binary_sha256']:
            raise RuntimeError('QEMU binary changed; rebuild first')
        for name, sha in build['sources'].items():
            if hashlib.sha256((PROJECT/name).read_bytes()).hexdigest() != sha:
                raise RuntimeError('QEMU source changed; rebuild first')
        payload = FIRMWARE
        if hashlib.sha256(payload.read_bytes()).hexdigest() != 'e6df1764323fb1c287159dd3ce1c84ac93c72b96ba6eb09c070295c093fecbff':
            raise RuntimeError('Firmware payload hash changed')
        port, qmp = free_port(), free_port()
        command = [build['binary'], '-M', 'nikon-z7-display', '-smp', '2', '-display', 'none',
                   '-serial', 'none', '-monitor', 'none', '-nic', 'none', '-bios', str(payload),
                   '-device', 'loader,addr=0x10020000,cpu-num=0',
                   '-device', 'loader,addr=0x10020000,cpu-num=1',
                   '-S', '-gdb', f'tcp:127.0.0.1:{port}',
                   '-qmp', f'tcp:127.0.0.1:{qmp},server=on,wait=off',
                   '-accel', 'tcg,one-insn-per-tb='+('on' if self.one_insn_per_tb else 'off'), '-icount',
                   f'shift={getattr(self, "icount_shift", 6)},align=off,sleep=off']
        self.audit({'kind': 'boot', 'command': command, 'build': build,
                    'firmware_modified': False, 'guest_state_modified': True, 'camera_access': False})
        self.live_log = self.frame_spool/f'qemu-{self.boot_count}.log'
        self.console = self.live_log.open('w')
        env = dict(os.environ, Z7_MMIO_QUIET='1')
        env.pop('Z7_SHOULDER_OBSERVE', None)
        if self.shoulder.renderer is not None:
            env['Z7_SHOULDER_OBSERVE'] = '1'
        if os.name == 'nt' and build.get('native_runtime_path'):
            env['PATH'] = build['native_runtime_path'] + os.pathsep + env.get('PATH', '')
        env.pop('Z7_IMAGE_CONFIG_RECORD', None)
        env.pop('Z7_IMAGE_PROCESS_TEST', None)
        env.pop('Z7_ISP_DISABLE_CACHE', None)
        env.pop('Z7_VERIFY_CPSR_HFLAGS', None)
        if getattr(self, 'verify_cpsr_hflags', False):
            env['Z7_VERIFY_CPSR_HFLAGS'] = '1'
        if getattr(self, 'isp_disable_cache', False):
            env['Z7_ISP_DISABLE_CACHE'] = '1'
        env.pop('Z7_LCD_SCANOUT_TEST', None)
        if getattr(self, 'lcd_scanout_test', False):
            env['Z7_LCD_SCANOUT_TEST'] = '1'
        if getattr(self, 'image_process_test', False):
            env['Z7_IMAGE_PROCESS_TEST'] = '1'
        if getattr(self, 'image_config_record', False) or getattr(self, 'image_process_test', False):
            env['Z7_IMAGE_CONFIG_RECORD'] = '1'
        env.pop('Z7_CAPTURE_TRACE', None)
        if self.capture_trace:
            env['Z7_CAPTURE_TRACE'] = '1'
        env.pop('Z7_TG_TEST_PULSE', None)
        if self.tg_test_pulse:
            env['Z7_TG_TEST_PULSE'] = '1'
            self.audit({'kind': 'tg-test-pulse', 'diagnostic_only': True, 'raw_frames': 0})
        env.pop('Z7_EXMCU_TEST_DEVICE', None)
        env.pop('Z7_TG_PERIODIC_TEST', None)
        env.pop('Z7_TG_PAIRED_TEST', None)
        env.pop('Z7_TG_PERIOD_NS', None)
        if getattr(self, 'tg_period_ms', None):
            env['Z7_TG_PERIOD_NS'] = str(self.tg_period_ms * 1000000)
        if self.tg_paired_test:
            env['Z7_TG_PAIRED_TEST'] = '1'
        env.pop('Z7_RAW_INGRESS_TEST', None)
        env.pop('Z7_RAW_NATIVE0_TEST', None)
        env.pop('Z7_ISP_BOUNDARY_TEST', None)
        if self.isp_boundary_test:
            env['Z7_ISP_BOUNDARY_TEST'] = '1'
        if self.raw_ingress_test:
            env['Z7_RAW_INGRESS_TEST'] = str(self.raw_ingress_test)
            env['Z7_RAW_NATIVE0_TEST'] = str(active_ingress('native0'))
        if self.tg_periodic_test:
            env['Z7_TG_PERIODIC_TEST'] = '1'
            self.audit({'kind': 'tg-periodic-test', 'diagnostic_only': True,
                        'paired_edges': self.tg_paired_test, 'raw_frames': 0})
        env.pop('Z7_EXMCU_NO_LENS', None)
        env.pop('Z7_EXMCU_CAPTURE_SYNC_TEST', None)
        if self.photo_liveview:
            env['Z7_EXMCU_CAPTURE_SYNC_TEST'] = '1'
            self.audit({'kind': 'exmcu-capture-sync-test', 'diagnostic_only': True,
                        'request_gpio': '0x65', 'completion_gpio': '0x54', 'irq': 65})
        if self.exmcu_no_lens:
            env['Z7_EXMCU_NO_LENS'] = '1'
            self.audit({'kind': 'exmcu-no-lens', 'diagnostic_only': True})
        if self.exmcu_test_device:
            env['Z7_EXMCU_TEST_DEVICE'] = '1'
            self.audit({'kind': 'exmcu-test-device', 'diagnostic_only': True, 'capture_control_only': True})
        env.pop('Z7_GRAPHICS_DUMP_DIR', None)
        env.pop('Z7_GPU_ICON_DUMP_DIR', None)
        if getattr(self, 'gpu_icon_dump', None):
            env['Z7_GPU_ICON_DUMP_DIR'] = str(self.gpu_icon_dump)
        options = dict(creationflags=subprocess.CREATE_NO_WINDOW) if os.name == 'nt' else {}
        self.proc = subprocess.Popen(command, stdout=self.console, stderr=subprocess.STDOUT, env=env, **options)
        self.remote = Remote(port)
        self.remote.command('qSupported:multiprocess+;xmlRegisters=arm')
        self.remote.command('qXfer:features:read:target.xml:0,fff')
        self.remote.command('Hg0'); self.remote.command('?')
        self.remote.sock.settimeout(30)
        self.monitor = QMP(qmp)
        self.update(message='初始化图像内存')
        packet = self.run_to(0x103f9ce2)
        packet = self.call(packet, '初始化图像内存', 0x10123432, bytes(12), [('scratch', 0)])
        packet = self.run_to(0x100531b8)
        packet = self.call(packet, '加载电源报告', 0x101f9cca, bytes(64), [0x43090010, ('scratch', 0)])
        packet = self.call(packet, '初始化系统状态', 0x10058b90, SYSTEM_REPORT, [0x03040002, ('scratch', 0)])
        packet = self.run_to(0x102699ec)
        power = bytearray(self.remote.memory(0xa1f90bf0, 0x30))
        struct.pack_into('<I', power, 0, struct.unpack_from('<I', power)[0] | 0x200)
        packet = self.call(packet, '接通虚拟电源', 0x101b493e, power, [('scratch', 0)])
        packet = self.run_to(0x100531bc)
        packet = self.call(packet, '等待应用服务', 0x1005cbf8, bytes(8), [0xa1819748, 0])
        packet = self.run_to(0x100531bc)
        if enter_menu:
            self.key(packet, 'menu')
        self.continue_guest()
        if announce_ready:
            self.refresh()
            self.update(phase='running', busy=False, message='正在绘制界面')

    def set_fixed_orientation(self, packet):
        """Use the firmware override API; this is not a physical IMU model."""
        value = self.orientation_fixture
        if value is None:
            return packet, None
        fixture = {'orientation': value, 'override_selector': [1, 3, 4, 2][value],
                   'original_api': '101b4224', 'imu_model': False,
                   'initialization_before': self.remote.memory(0xa18b60f8, 4).hex()}
        for name, function, arguments in [
            ('before', 0x101b331a, []),
            ('set', 0x101b4224, [fixture['override_selector']]),
            ('settle', 0x1017081e, [1000]),
            ('after', 0x101b331a, []),
        ]:
            result = call_fixture(self.remote, packet, function, bytes(8), arguments)
            fixture[name] = result
            packet = 'T05thread:'+result['return_cpu']+';'
        fixture['initialization_after'] = self.remote.memory(0xa18b60f8, 4).hex()
        self.audit({'kind': 'fixed-orientation-fixture', **fixture})
        if int(fixture['after']['return_r0'], 16) != value:
            raise RuntimeError('Fixed orientation did not take effect')
        return packet, fixture

    def set_keep_awake(self, packet):
        """Disable the shooting idle timeout through the native session setting.

        10090c7c reads 1021 in this profile; enum 0 selects zero seconds,
        and 10090e30 then declines to arm AppC_Tim. No timer RAM is patched.
        """
        if not self.keep_awake:
            return packet, None
        record = {'setting': '0x1021', 'value': 0, 'persistent_save_requested': False,
                  'scope': 'shooting idle timer; not sensor initialization'}
        for name, function, arguments in [
            ('before', 0x1015ba56, [0x1021]),
            ('set', 0x101d61ee, [0x1021, 0]),
            ('after', 0x1015ba56, [0x1021]),
            ('calculated_seconds', 0x10090c7c, []),
        ]:
            result = call_fixture(self.remote, packet, function, bytes(8), arguments)
            record[name] = result
            packet = 'T05thread:'+result['return_cpu']+';'
        self.audit({'kind': 'shooting-idle-timeout-override', **record})
        if (int(record['set']['return_r0'], 16) != 1 or
                int(record['after']['return_r0'], 16) != 0 or
                int(record['calculated_seconds']['return_r0'], 16) != 0):
            raise RuntimeError('Original shooting idle timeout did not disable')
        self.update(idle_timeout_override={'setting': '0x1021', 'value': 0,
                    'calculated_seconds': 0, 'persistent_save_requested': False})
        return packet, record

    def boot_shooting(self):
        """Original shooting UI with explicitly logged diagnostic startup inputs.

        The optional control test supplies a TG test pulse and two EX MCU
        control replies. It does not supply a complete sensor config or pixels.
        """
        self.boot(enter_menu=False, announce_ready=False)
        self.update(phase='booting', busy=True, message='正在启动拍摄界面')
        self.pause()
        packet = self.run_to(0x100531bc)
        if self.photo_liveview:
            self.remote.sock.settimeout(60)
            # Stationary-photo profile: the auxiliary MVD engine (20201000)
            # is not modeled. Suppress its frame schedule before any capture
            # is allocated, instead of fabricating completion or clearing a
            # live record. Native 102063e0 reads these 16 period/phase pairs;
            # 102064aa defines period=0 as disabled. This is a RAM fixture,
            # not a stock camera setting or an implementation of MVD/AF.
            address = 0xa18fb73c
            before = self.remote.memory(address, 128)
            if hashlib.sha256(before).hexdigest() != '8c9b874aa948b809d1dc44aa1e2b9ce21e5b7a20aad6dc90cc5a3fc780adfc4b':
                raise RuntimeError('MVD schedule differs from the verified C 3.80 table')
            disabled = bytes(len(before))
            assert self.remote.command(f'M{address:x},{len(disabled):x}:' + disabled.hex()) == 'OK'
            assert self.remote.memory(address, len(disabled)) == disabled
            self.audit({'kind': 'stationary-photo-mvd-disabled', 'diagnostic_only': True,
                        'address': hex(address), 'before': before.hex(), 'after': disabled.hex(),
                        'firmware_code_modified': False, 'motion_detection_implemented': False,
                        'applied_before_capture': True})
            for offset in (0x3dc, 0x3dd):
                packet = self.call(packet, '加载取景诊断设置', 0x101c9a5e, bytes(8), [offset, 1])
        packet, _ = self.set_fixed_orientation(packet)
        self.audit({'kind': 'capture_diagnostic_assumptions',
                    'kept_data_ready_without_data': True,
                    'empty_correction_table': True, 'absent_sensor_calibration': True,
                    'photo_liveview_profile': self.photo_liveview,
                    'physical_sensor_configuration_verified': False})
        packet = self.call(packet, '加载校正设置', 0x101c9a2c, bytes(8), [0x29e])
        packet = self.call(packet, '选择空校正表', 0x101c9a5e, bytes(8), [0x29e, 1])
        packet = self.call(packet, '持久数据诊断通知', 0x101e2c4e, bytes(8), [0x03020008, 0])
        packet = self.call(packet, '拍摄模式通知', 0x1005d4c0, bytes(8), [0xa1819748, 0x05040016, 2])
        packet = self.call(packet, '拍摄参数通知', 0x1015ba4a, bytes(8), [0x180c, 1])
        for _ in range(16):
            packet = self.run_to(0x100531c2)
            packet = self.run_to(0x100531bc)
            state = capture_state(self.remote)
            if state['ready'] & 3 == 3 and state['stage'] == 0:
                break
        else:
            raise RuntimeError('拍摄应用未完成就绪处理')
        packet = self.call(packet, '使用缺省校准分支', 0x10122912, bytes(8), [0])
        if self.capture_control_test:
            pointer = int.from_bytes(self.remote.memory(0xa18fa724, 4), 'little')
            original = self.remote.memory(pointer+0xbe014, 2)
            assert self.remote.command(f'M{pointer+0xbe014:x},2:0470') == 'OK'
            self.audit({'kind': 'sensor-version-fixture', 'address': hex(pointer+0xbe014),
                        'original_bytes': original.hex(), 'bytes': '0470',
                        'complete_sensor_configuration': False, 'diagnostic_only': True})
        result = call_fixture(self.remote, packet, 0x10521d84, bytes(32),
                              [('scratch', 0), 0, ('scratch', 16)])
        if action_completed(result):
            # StartLV acknowledgement can precede the original GUI's drawing.
            # Let guest tasks finish their first draw before publishing ready.
            packet = 'T05thread:'+result['return_cpu']+';'
            packet, _ = self.set_keep_awake(packet)
            self.call(packet, '等待原厂界面绘制', 0x1017081e, bytes(8), [5000])
        self.refresh_device_evidence()
        health = capture_health(self.remote, self.device_evidence or None)
        health['control_test'] = self.capture_control_test
        health['complete_sensor_configuration'] = False
        health['start_lv_action_completed'] = action_completed(result)
        self.audit({'kind': 'capture_start', 'result': result, 'health': health})
        if not health['start_lv_action_completed']:
            raise RuntimeError('StartLV 动作未完成；详见本次运行日志')
        self.update(capture=health)
        self.refresh()
        (self.directory/'shooting-entry.png').write_bytes(self.png)
        self.continue_guest()
        self.update(phase='running', busy=False, message=self.running_message())
        self.update(startup_seconds=round(time.perf_counter()-self.boot_started, 3))

    def running_message(self):
        if self.profile == 'shooting':
            if self.photo_liveview:
                e = self.device_evidence
                return f"取景实验 · RAW {e.get('raw_batches', 0)} 批 · 处理 {e.get('isp_batches', 0)} 批"
            return '控制链实验 · RAW 0 帧' if self.capture_control_test else '运行中 · CMOS 未就绪'
        return '运行中'

    def refresh_device_evidence(self):
        """Read new device log lines only; counters never infer physical accuracy."""
        if not self.photo_liveview:
            return
        import re
        with self.live_log.open('r') as f:
            f.seek(self.device_log_position)
            data = self.device_log_tail + f.read()
            self.device_log_position = f.tell()
        lines = data.split('\n')
        self.device_log_tail = lines.pop()
        e = self.device_evidence
        for line in lines:
            m = re.match(r'Z7_RAW_NATIVE0_STREAM seq=(\d+)', line)
            if m: e['raw_batches'] = int(m[1])
            m = re.match(r'Z7_IMAGE_PROCESS_BATCH batches=(\d+)', line)
            if m: e['isp_batches'] = int(m[1])
            m = re.search(r' host_us=(\d+) cache_hits=(\d+) cache_misses=(\d+)', line)
            if m:
                e['isp_host_ms'] = int(m[1])/1000
                e['isp_cache_hits'], e['isp_cache_misses'] = int(m[2]), int(m[3])
            m = re.match(r'Z7_IMAGE_PROCESS_OUTPUT .* channel=4 y=(0x[0-9a-f]+) uv=(0x[0-9a-f]+)', line)
            if m: self.preview_buffers.add((m[1], m[2]))
            m = re.match(r'Z7_LCD_SCANOUT frame=(\d+) y=(0x[0-9a-f]+) uv=(0x[0-9a-f]+)', line)
            if m:
                e['lcd_frames_observed'] = int(m[1])
                e['lcd_y'], e['lcd_uv'] = m[2], m[3]
                if (m[2], m[3]) in self.preview_buffers:
                    e['processed_buffer_scanned'] = True
        values = {'device_evidence': dict(e)}
        if self.raw_source:
            raw = self.raw_source.status()
            raw.update(guest_frames_transferred=e['raw_batches']*4,
                       firmware_frames_consumed=None,
                       functional_isp_input_reads=e['isp_batches']*4,
                       cmos_input_connected=e['raw_batches'] > 0,
                       blocked_by=None if e['raw_batches'] else 'Waiting for native RAW descriptors')
            values['raw_input'] = raw
        if self.state.get('capture'):
            health = dict(self.state['capture'])
            health.update(cmos_input_connected=e['raw_batches'] > 0,
                          cmos_frames_injected=e['raw_batches']*4,
                          live_view_verified=None,
                          processed_source_scanout_observed=e['processed_buffer_scanned'],
                          image_evidence_scope='experimental device log; visual QA recorded separately')
            values['capture'] = health
        self.update(**values)

    def pause(self):
        if self.running:
            self.remote.sock.sendall(b'\x03')
            self.receive_stop()

    def refresh(self):
        refresh_started = time.perf_counter()
        if self.proc.poll() is not None:
            raise RuntimeError(f'QEMU 已退出（{self.proc.returncode}）')
        status = self.monitor.command('query-status')
        if self.running and not status['running']:
            raise RuntimeError('虚拟机已停止：'+status['status'])
        self.refresh_device_evidence()
        self.shoulder.poll(self.live_log)
        with self.lock:
            self.shoulder_png = self.shoulder.png
            self.state['shoulder'] = dict(self.shoulder.status)
        if not self.running and self.state.get('capture') is not None:
            # Publish a fresh paused observation instead of retaining boot state.
            health = dict(self.state['capture'])
            health.update(capture_health(self.remote, self.device_evidence or None))
            self.update(capture=health)
        # High-frequency QMP scratch I/O stays in host-local temporary storage.
        # Only changed PNGs and audit evidence go to the project drive.
        path = self.frame_spool/'screen.ppm'
        self.monitor.command('screendump', {'filename': str(path)})
        w, h, pixels = ppm_pixels(path.read_bytes())
        sha = hashlib.sha256(pixels).hexdigest()
        changed = sha != self.previous_pixels
        png = rgb_png(w, h, pixels) if changed else None
        if changed:
            (self.directory/'screen.png').write_bytes(png)
            self.png_encodes += 1
        self.refresh_samples += 1
        self.refresh_seconds += time.perf_counter()-refresh_started
        with self.lock:
            if changed:
                self.previous_pixels = sha
                self.png = png
                self.state['frame'] += 1
                self.state['frame_sha256'] = sha
                self.state['updated_at'] = time.time()
            self.state['performance'] = dict(one_insn_per_tb=self.one_insn_per_tb,
                refresh_samples=self.refresh_samples, png_encodes=self.png_encodes,
                refresh_mean_ms=round(self.refresh_seconds*1000/self.refresh_samples, 3))
            self.state['heartbeat'] = time.time()
            if self.state['phase'] == 'running' and not self.state['busy']:
                self.state['message'] = self.running_message()

    def close_guest(self):
        self.shoulder.close()
        with self.lock:
            self.shoulder_png = None
            self.state['shoulder'] = dict(self.shoulder.status)
        for handle in (self.remote, self.monitor):
            if handle:
                try:
                    handle.sock.close() if isinstance(handle, Remote) else handle.close()
                except OSError:
                    pass
        self.remote = self.monitor = None
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill(); self.proc.wait()
        self.proc = None
        if self.console:
            self.console.close(); self.console = None
        if hasattr(self, 'live_log'):
            if self.live_log.exists():
                shutil.copyfile(self.live_log, self.directory/self.live_log.name)
                self.live_log.unlink()
            del self.live_log
        self.running = False
        if hasattr(self, 'frame_spool'):
            (self.frame_spool/'screen.ppm').unlink(missing_ok=True)
            self.frame_spool.rmdir()
            del self.frame_spool

    def worker(self):
        restart = True
        while True:
            try:
                if restart:
                    self.close_guest()
                    if self.profile == 'shooting':
                        self.boot_shooting()
                    else:
                        self.boot()
                    restart = False
                item = None
                try:
                    item = self.requests.get(timeout=.08 if self.running else .5)
                except queue.Empty:
                    pass
                if item:
                    self.update(busy=True)
                    self.audit({'kind': 'browser_action', **item})
                    action = item['action']
                    if action == 'restart':
                        restart = True
                    elif action in ('shooting', 'menu-profile'):
                        self.profile = 'shooting' if action == 'shooting' else 'menu'
                        restart = True
                    elif action == 'stop':
                        self.close_guest()
                        self.update(phase='stopped', message='已停止，可以重新启动')
                    elif action == 'pause' and self.running:
                        self.pause(); self.update(phase='paused', message='已暂停')
                    elif action == 'resume' and self.proc and not self.running:
                        self.continue_guest()
                        self.update(phase='running', message='运行中')
                    elif action == 'key' and self.running:
                        self.pause()
                        packet = self.run_to(0x100531bc)
                        self.key(packet, item['key'])
                        self.continue_guest()
                        self.update(last_key=LABELS[item['key']], message='运行中')
                    if restart:
                        self.update(phase='booting', message='正在重新启动')
                    elif self.proc:
                        # Publish the final paused/drawn frame before acknowledging
                        # the action; otherwise clients can observe one stale frame.
                        self.refresh()
                    self.update(busy=restart, completed_action=item['id'])
                if self.proc and not restart and not item:
                    if self.running:
                        self.refresh()
                    elif self.proc.poll() is not None:
                        raise RuntimeError(f'QEMU 已退出（{self.proc.returncode}）')
            except Exception as error:
                self.audit({'kind': 'error', 'error': str(error), 'traceback': traceback.format_exc(), 'incomplete_fixture': probe.ACTIVE_FIXTURE})
                self.close_guest()
                self.update(phase='error', message='运行已停止', busy=False, error=str(error))
                restart = False
                # Keep the UI available so the user can review or restart.


def main():
    global FIRMWARE, BUILD_MANIFEST
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--firmware', type=Path, required=True, help='Your locally obtained extracted Z7 C 3.80 main payload')
    p.add_argument('--companion-firmware', type=Path,
                   help='Optional local Z7 C3.80 ex1610 image for shoulder function replay')
    p.add_argument('--build-manifest', type=Path, default=BUILD_MANIFEST)
    p.add_argument('--port', type=int, default=8787)
    p.add_argument('--one-insn-per-tb', action=argparse.BooleanOptionalAction, default=False,
                   help='Slow diagnostic translation mode; normal blocks are the interactive default')
    p.add_argument('--isp-disable-cache', action='store_true', help='Recompute ISP pixels for diagnostic comparisons')
    p.add_argument('--profile', choices=('menu', 'shooting'), default='shooting')
    p.add_argument('--capture-control-test', action='store_true',
                   help='Enable experimental TG/EX MCU control inputs and a version-only RAM fixture; no RAW input')
    p.add_argument('--no-lens', action='store_true',
                   help='Simulate the lens-absent EX MCU controller; experimental, no CMOS samples')
    p.add_argument('--orientation-fixture', type=int, choices=range(4),
                   help='Fixed orientation through original firmware API; 0 is landscape, not an IMU model')
    p.add_argument('--keep-awake', action='store_true',
                   help='Disable shooting idle timeout through native setting 1021=0; session only')
    p.add_argument('--no-current', action='store_true', help='Do not replace the primary session pointer')
    p.add_argument('--raw-source', type=Path,
                   help='Unpacked Bayer manifest; defaults to active_cmos.json')
    p.add_argument('--raw-ingress-test', action='store_true',
                   help='Transfer active packed CMOS source through the experimental native DMA channel')
    p.add_argument('--isp-boundary-test', action='store_true',
                   help='Experimental ISP interrupt routing after RAW DMA; not complete ISP emulation')
    p.add_argument('--photo-liveview', action='store_true',
                   help='Verified photo experiment: active Bayer input, functional ISP/LCD, no lens, diagnostic calibration')
    a = p.parse_args()
    FIRMWARE = a.firmware.resolve(strict=True)
    BUILD_MANIFEST = a.build_manifest.resolve(strict=True)
    if FIRMWARE.stat().st_size != 33052088 or hashlib.sha256(FIRMWARE.read_bytes()).hexdigest() != 'e6df1764323fb1c287159dd3ce1c84ac93c72b96ba6eb09c070295c093fecbff':
        p.error('Unsupported firmware: requires the extracted Z7 C 3.80 MAIN, not the upgrade package')
    if a.profile == 'menu' and not a.photo_liveview and not a.raw_source:
        raw_default = None
    else:
        raw_default = a.raw_source or active_manifest('native0' if a.photo_liveview else 'unpacked')
    if a.photo_liveview and a.raw_source and a.raw_source.resolve() != active_manifest('native0'):
        p.error('--photo-liveview uses the checked active native0 source; update active_cmos.json to replace it')
    ingress = a.raw_ingress_test or a.photo_liveview
    camera = Camera(a.port, a.profile, capture_control_test=a.capture_control_test or a.photo_liveview,
                    raw_source=raw_default, exmcu_no_lens=a.no_lens or a.photo_liveview,
                    tg_periodic_test=ingress, tg_paired_test=ingress,
                    raw_ingress_test=active_ingress() if ingress else None,
                    isp_boundary_test=a.isp_boundary_test or a.photo_liveview,
                    orientation_fixture=0 if a.photo_liveview and a.orientation_fixture is None else a.orientation_fixture,
                    keep_awake=a.keep_awake or a.photo_liveview, photo_liveview=a.photo_liveview,
                    one_insn_per_tb=a.one_insn_per_tb, companion_firmware=a.companion_firmware)
    camera.isp_disable_cache = a.isp_disable_cache
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, status, content, mime='application/json; charset=utf-8'):
            self.send_response(status)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(content)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            try:
                self.wfile.write(content)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            path = self.path.split('?')[0]
            if path == '/':
                html = (ROOT/'interactive.html').read_text(encoding='utf-8').replace('__TOKEN__', camera.token)
                self.send(200, html.encode(), 'text/html; charset=utf-8')
            elif path == '/api/status':
                self.send(200, json.dumps(camera.snapshot(), ensure_ascii=False).encode())
            elif path == '/api/shoulder':
                with camera.lock:
                    status = dict(camera.state['shoulder'])
                self.send(200, json.dumps(status).encode())
            elif path == '/api/shoulder/frame':
                with camera.lock:
                    png = camera.shoulder_png
                self.send(200 if png else 503, png or b'Shoulder frame unavailable',
                          'image/png' if png else 'text/plain')
            elif path == '/api/frame':
                with camera.lock:
                    png = camera.png
                self.send(200 if png else 503, png or b'Frame not ready', 'image/png' if png else 'text/plain')
            elif path == '/favicon.ico':
                self.send(204, b'')
            else:
                self.send(404, b'Not found', 'text/plain')

        def do_POST(self):
            if self.path != '/api/action' or self.headers.get('X-Z7-Token') != camera.token:
                self.send(403, b'{"error":"Forbidden"}'); return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 1024:
                    raise ValueError('Invalid request size')
                data = json.loads(self.rfile.read(length))
                item = camera.submit(data.get('action'), data.get('key'))
                self.send(202, json.dumps(item).encode())
            except (ValueError, RuntimeError, queue.Full) as error:
                self.send(409, json.dumps({'error': str(error) or '操作队列已满'}, ensure_ascii=False).encode())

    server = http.server.ThreadingHTTPServer(('127.0.0.1', a.port), Handler)
    if not a.no_current:
        (PROJECT/'.local/sessions/current.json').write_text(json.dumps({
            'url': f'http://127.0.0.1:{a.port}', 'pid': os.getpid(), 'session': camera.stamp,
            'directory': str(camera.directory), 'profile': a.profile,
            'wsl_distribution': os.environ.get('WSL_DISTRO_NAME'),
            'raw_source': str(camera.raw_source.path) if camera.raw_source else None,
            'capture_control_test': camera.capture_control_test, 'no_lens_simulation': camera.exmcu_no_lens,
            'orientation_fixture': camera.orientation_fixture, 'keep_awake': camera.keep_awake,
            'photo_liveview': camera.photo_liveview, 'build': json.loads(
                (BUILD_MANIFEST).read_text())}, indent=2))
    threading.Thread(target=camera.worker, daemon=True).start()
    print(f'Z7 interactive UI: http://127.0.0.1:{a.port}', flush=True)
    try:
        server.serve_forever()
    finally:
        camera.close_guest()


if __name__ == '__main__':
    main()
