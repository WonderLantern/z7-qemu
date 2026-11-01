# SPDX-License-Identifier: GPL-2.0-or-later
"""Public service and framebuffer contracts use synthesized data and mocks."""
import builtins
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shoulder_display as display
from shoulder_protocol import PREFIX
from test_shoulder_protocol import wire


class FakeRenderer:
    def __init__(self, path): self.calls = 0; self.closed = False
    def receive(self, payload):
        self.calls += 1
        return bytes([128]) + bytes(1535)
    def evidence(self): return {}
    def close(self): self.closed = True


class DisplayTests(unittest.TestCase):
    def test_disabled_does_not_construct_renderer(self):
        with patch.object(display, 'ShoulderDisplay', side_effect=AssertionError):
            service = display.ShoulderService()
            service.reset(); service.poll(Path('unused')); service.close()
        self.assertFalse(service.status['enabled'])
        self.assertIsNone(service.status['error'])

    def test_missing_and_wrong_firmware(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'firmware'
            service = display.ShoulderService(path); service.reset()
            self.assertFalse(service.status['firmware_valid'])
            self.assertIsNotNone(service.status['error'])
            path.write_bytes(bytes(0x80000))
            with self.assertRaisesRegex(ValueError, 'SHA-256'): display.ShoulderDisplay(path)
            path.write_bytes(bytes(10))
            with self.assertRaisesRegex(ValueError, 'length'): display.ShoulderDisplay(path)

    def test_missing_unicorn(self):
        original = builtins.__import__
        def no_unicorn(name, *args, **kwargs):
            if name == 'unicorn' or name.startswith('unicorn.'):
                raise ModuleNotFoundError('unicorn')
            return original(name, *args, **kwargs)
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'synthetic'
            path.write_bytes(bytes(0x80000))
            with patch.object(display, 'EXPECTED_HASH', hashlib.sha256(path.read_bytes()).hexdigest()), patch('builtins.__import__', side_effect=no_unicorn):
                service = display.ShoulderService(path); service.reset()
        self.assertTrue(service.status['firmware_valid'])
        self.assertIn('Unicorn', service.status['error'])
        self.assertFalse(service.status['available'])

    def test_duplicate_failure_and_restart(self):
        with tempfile.TemporaryDirectory() as d, patch.object(display, 'load_firmware', return_value=b''), patch.object(display, 'ShoulderDisplay', FakeRenderer):
            path = Path(d) / 'trace'
            record = PREFIX + wire().hex() + '\n'
            path.write_text(record)
            service = display.ShoulderService('local'); service.reset(); service.poll(path)
            renderer = service.renderer
            self.assertTrue(service.status['available'])
            self.assertIsNotNone(service.status['last_update'])
            with path.open('a') as f: f.write(record)
            service.poll(path)
            self.assertEqual(renderer.calls, 1)
            self.assertEqual(service.status['frame'], 1)
            with patch.object(renderer, 'receive', side_effect=RuntimeError('bounded failure')):
                changed = bytearray(wire()); changed[9] ^= 1
                changed[255] = (255 - sum(changed[:255])) & 255
                with path.open('a') as f: f.write(PREFIX + changed.hex() + '\n')
                service.poll(path)
            self.assertTrue(renderer.closed)
            self.assertFalse(service.status['available'])
            self.assertIsNone(service.png)
            service.reset(); service.poll(path)
            self.assertEqual(service.status['frame'], 1)
            service.close()
            self.assertIsNone(service.renderer)

    def test_main_camera_failure_isolation(self):
        import interactive
        with tempfile.TemporaryDirectory() as d, patch.object(interactive, 'PROJECT', Path(d)):
            camera = interactive.Camera(0, companion_firmware=Path(d) / 'absent')
            camera.shoulder.reset()
            self.assertEqual(camera.state['phase'], 'booting')
            self.assertFalse(camera.exmcu_test_device)
            self.assertIsNotNone(camera.shoulder.status['error'])
            camera.close_guest()

    def test_native_call_budget_and_return_contract(self):
        panel = display.ShoulderDisplay.__new__(display.ShoulderDisplay)
        panel.image = bytes(0x80000)
        panel.calls = 0
        names = ('R0', 'R1', 'R2', 'R3', 'SP', 'LR', 'PC')
        constants = {f'UC_ARM_REG_{name}': i for i, name in enumerate(names)}
        registers = SimpleNamespace(**constants)
        writes = {}
        starts = []
        pc = display.STOP
        panel.uc = SimpleNamespace(reg_write=lambda reg, value: writes.update({reg: value}),
                                   reg_read=lambda reg: pc if reg == registers.UC_ARM_REG_PC else 0,
                                   emu_start=lambda *args, **kw: starts.append((args, kw)))
        with patch.dict(sys.modules, {'unicorn.arm_const': registers}):
            panel.call(display.FLASH + 2, 13)
            self.assertEqual(writes[registers.UC_ARM_REG_R0], 13)
            self.assertEqual(starts[0], ((display.FLASH + 3, display.STOP),
                                        {'timeout': 1000000, 'count': 4000000}))
            pc = display.FLASH + 4
            with self.assertRaisesRegex(RuntimeError, 'budget'):
                panel.call(display.FLASH + 2)
            with self.assertRaises(ValueError): panel.call(0)
        panel.close()
        with self.assertRaisesRegex(RuntimeError, 'closed'): panel.call(display.FLASH)

    def test_rgb_and_png_bit_order(self):
        frame = bytes([0x81, 0x40]) + bytes(1534)
        pixels = display.rgb(frame)
        on, off = bytes((220, 230, 225)), bytes((12, 18, 16))
        self.assertEqual(pixels[:24], on + off * 6 + on)
        self.assertEqual(pixels[24:30], off + on)
        png = display.framebuffer_png(frame)
        self.assertEqual(struct.unpack('>II', png[16:24]), (128, 96))
        pos = 8; raw = b''
        while pos < len(png):
            n = int.from_bytes(png[pos:pos+4], 'big'); kind = png[pos+4:pos+8]
            data = png[pos+8:pos+8+n]
            self.assertEqual(zlib.crc32(kind + data), int.from_bytes(png[pos+8+n:pos+12+n], 'big'))
            if kind == b'IDAT': raw += data
            pos += n + 12
        decoded = zlib.decompress(raw)
        self.assertEqual(len(decoded), 96 * 385)
        self.assertEqual(decoded[1:385], pixels[:384])
        with self.assertRaises(ValueError): display.rgb(bytes(1535))


@unittest.skipUnless(os.environ.get('Z7_SHOULDER_PRIVATE_CASES'), 'local firmware and case manifest not supplied')
class PrivateIntegrationTests(unittest.TestCase):
    def test_native_startup_and_reference_frames(self):
        # Manifest remains outside the public tree: firmware path and cases with
        # payload path + expected framebuffer SHA-256. No output files are saved.
        manifest = json.loads(Path(os.environ['Z7_SHOULDER_PRIVATE_CASES']).read_text())
        with display.ShoulderDisplay(manifest['firmware']) as panel:
            self.assertEqual(panel.data_sha256, display.EXPECTED_INIT_SHA256)
            self.assertTrue(panel.object_table_valid)
            self.assertTrue(panel.object_pointer_slots_valid)
            self.assertEqual(panel.relocations, 0)
            for entry in panel.startup_entries[3:]:
                self.assertEqual(entry[3], display.STARTUP_ZERO)
                self.assertEqual(bytes(panel.uc.mem_read(entry[1], entry[2])), bytes(entry[2]))
            for case in manifest['cases']:
                payload = Path(case['payload']).read_bytes()
                frame = panel.receive(payload)
                self.assertEqual(len(frame), 1536)
                self.assertEqual(hashlib.sha256(frame).hexdigest(), case['sha256'])
                self.assertEqual(panel.receive(payload), frame)
