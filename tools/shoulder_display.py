# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded execution of the original Z7 companion-MCU shoulder compositor.

Ported from WonderLantern/z6-qemu tools/shoulder_display.py. The Z7 ex1610
image shares the receive (0x080274BC), publish (0x08029854), state
(0x2001822C) and framebuffer (0x20018310) layout; the full compositor is at
0x080289F8.

The host explicitly schedules receive, publish and compose. Event delivery at
0x0802155A is suppressed in this isolated MCU harness; no MCU RTOS is run.
No firmware instructions, fonts or framebuffer data are distributed here.
"""
from pathlib import Path
import hashlib
import struct

FLASH = 0x08000000
FB = 0x20018310
STATE = 0x20030000
STOP = 0x2003EFF0

EXPECTED_HASH = 'be90188211f7899cf974255a9c1527f589a5b5129e4cfc2d61efc545ea8fd42e'

# Z7 startup scatter table at 0x080038CC. Entries are (source, destination,
# length, function); source == 0 terminates the table. The same table is
# parsed from the verified image at run time.
STARTUP_TABLE = 0x080038CC
STARTUP_COPY = 0x08003894
STARTUP_DECOMPRESS = 0x08003838
STARTUP_ZERO = 0x080038B0

# Initialized-data region produced by the table's decompressor entry.
INIT_SOURCE = 0x0804809C
INIT_DEST = 0x2000D000
INIT_LENGTH = 0x1E58
EXPECTED_INIT_SHA256 = 'e925d2bd3cc1e88f35317d54ce9f75b1fad136bcc1e244b7ee306a613f50817b'

# Renderer accessor table: 4-byte Thumb function pointers, followed by the
# display state passed to the compositor. The table contains the state offsets
# 0x00, 0x05, 0x08, 0x0A, 0x0C ... 0x6D.
OBJECT_TABLE = 0x2000EA3C

# Literal pool locations checked for the unmodified accessor-table address.
OBJECT_POINTER_SLOTS = (
    0x08027C98, 0x08027D00, 0x08027DA0, 0x08027E08, 0x08027E84,
    0x08028088, 0x08028120, 0x08028200, 0x080283B8, 0x080286A4,
    0x08028710, 0x08028844, 0x080289B0, 0x08028AD0, 0x0802984C,
)


def load_firmware(firmware):
    path = Path(firmware)
    if path.stat().st_size != 0x80000:
        raise ValueError('Invalid companion MCU image length')
    with path.open('rb') as stream:
        image = stream.read(0x80001)
    if len(image) != 0x80000:
        raise ValueError('Invalid companion MCU image length')
    if hashlib.sha256(image).hexdigest() != EXPECTED_HASH:
        raise ValueError('Unsupported companion firmware SHA-256')
    return image


class ShoulderDisplay:
    def __init__(self, firmware):
        self.uc = None
        self.image = load_firmware(firmware)
        from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS,
                             UC_PROT_READ, UC_PROT_EXEC)
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(FLASH, len(self.image))
        self.uc.mem_write(FLASH, self.image)
        self.uc.mem_protect(FLASH, len(self.image), UC_PROT_READ | UC_PROT_EXEC)
        self.relocations = 0
        self.object_pointer_slots_valid = all(
            struct.unpack_from('<I', self.image, slot - FLASH)[0] == OBJECT_TABLE
            for slot in OBJECT_POINTER_SLOTS)
        if not self.object_pointer_slots_valid:
            raise ValueError('Unexpected accessor pointer literal')
        self.uc.mem_map(0x20000000, 0x40000)
        self.uc.reg_write(UC_ARM_REG_SP, 0x2003B000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.calls = 0
        self.decompressor_source = INIT_SOURCE
        self.startup_entries = self._startup_entries()
        self._run_startup_table()
        self.data = bytes(self.uc.mem_read(INIT_DEST, INIT_LENGTH))
        self.data_sha256 = hashlib.sha256(self.data).hexdigest()
        if self.data_sha256 != EXPECTED_INIT_SHA256:
            raise ValueError('Unexpected native decompressed-data SHA-256')
        self.object_table_valid = self._valid_table(OBJECT_TABLE)
        if not self.object_table_valid:
            raise ValueError('Renderer accessor table at 0x%08X is not valid' % OBJECT_TABLE)
        from unicorn import UC_HOOK_CODE
        from unicorn.arm_const import UC_ARM_REG_PC
        def event_boundary(uc, address, size, user):
            # Native event delivery is outside this isolated function harness.
            if address == 0x0802155A:
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        self.uc.hook_add(UC_HOOK_CODE, event_boundary, begin=0x0802155A, end=0x0802155A)

    def _startup_entries(self):
        """Parse the verified startup scatter table instead of hard-coding it."""
        base = STARTUP_TABLE - FLASH
        entries = []
        for index in range(32):
            src, dst, length, function = struct.unpack_from('<4I', self.image, base + 16 * index)
            if src == 0:
                return entries
            entries.append((src, dst, length, function))
        raise ValueError('Startup scatter table has no terminator')

    def _run_startup_table(self):
        """Execute the original startup copy/decompress/zero routines natively."""
        if (len(self.startup_entries) != 7
                or self.startup_entries[1] != (INIT_SOURCE, INIT_DEST, INIT_LENGTH, STARTUP_DECOMPRESS)
                or any(entry[3] != STARTUP_ZERO for entry in self.startup_entries[3:])):
            raise ValueError('Unexpected startup scatter layout')
        for src, dst, length, function in self.startup_entries:
            if not (0x20000000 <= dst and dst + length <= 0x20040000):
                raise ValueError('Startup destination outside mapped SRAM')
            if function != STARTUP_ZERO and not (FLASH <= src < FLASH + len(self.image)):
                raise ValueError('Startup source outside firmware')
            if function == STARTUP_COPY and src + length > FLASH + len(self.image):
                raise ValueError('Startup copy exceeds firmware')
            if function == STARTUP_COPY:
                self.call(STARTUP_COPY, src, dst, length)
            elif function == STARTUP_DECOMPRESS:
                self.call(STARTUP_DECOMPRESS, self.decompressor_source, dst, length)
            elif function == STARTUP_ZERO:
                self.call(STARTUP_ZERO, 0, dst, length)
            else:
                raise ValueError('Unknown startup function 0x%08X' % function)

    def _valid_table(self, address):
        if not (INIT_DEST <= address <= INIT_DEST + INIT_LENGTH - 4):
            return False
        # This firmware profile has 54 accessor function pointers.
        values = struct.unpack_from('<54I', self.data, address - INIT_DEST)
        return all(value & 1 and FLASH <= (value & ~1) < FLASH + len(self.image)
                   for value in values)

    def call(self, address, *args):
        if self.uc is None:
            raise RuntimeError('Companion renderer is closed')
        if not FLASH <= (address & ~1) < FLASH + len(self.image):
            raise ValueError('Function outside mapped firmware')
        from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                                       UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                                       UC_ARM_REG_PC)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.uc.reg_write(reg, value)
        self.uc.reg_write(UC_ARM_REG_SP, 0x2003B000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        # Unmapped/protected reads, writes and fetches raise UcError by default.
        self.uc.emu_start(address | 1, STOP, timeout=1000000, count=4000000)
        if self.uc.reg_read(UC_ARM_REG_PC) != STOP:
            raise RuntimeError('Companion function exceeded instruction budget')
        self.calls += 1
        return self.uc.reg_read(UC_ARM_REG_R0)

    def compose(self, state):
        if len(state) != 0x72:
            raise ValueError('Native shoulder state must be exactly 0x72 bytes')
        # The panel driver clears the visible buffer before the original
        # compositor runs. This is not drawing; every lit pixel comes from the
        # native compositor.
        self.uc.mem_write(FB, bytes(0x600))
        self.uc.mem_write(STATE, bytes(state))
        self.call(0x080289F8, FB, STATE)
        return bytes(self.uc.mem_read(FB, 0x600))

    def receive(self, payload):
        if len(payload) != 0x70 or payload[0] != 4:
            raise ValueError('Expected native channel8 tag4 112-byte display snapshot')
        request, source = 0x20031000, 0x20031100
        self.uc.mem_write(source, bytes(payload))
        self.uc.mem_write(request, struct.pack('<HHI', len(payload), 0, source))
        self.call(0x080274BC, request)
        self.call(0x08029854, 0x20013A86)
        state = bytes(self.uc.mem_read(0x2001822C, 0x72))
        if state != bytes(payload) + struct.pack('<H', len(payload)):
            raise RuntimeError('Native shoulder ingress/publication did not preserve snapshot')
        return self.compose(state)

    def evidence(self):
        return dict(renderer='original-ex1610-080289f8', width=128, height=96,
                    format='1bpp-row-major-msb-first', native_calls=self.calls,
                    companion_sha256=hashlib.sha256(self.image).hexdigest(),
                    initialized_data_sha256=self.data_sha256,
                    init_source=hex(self.decompressor_source),
                    startup_entries=len(self.startup_entries),
                    object_table=hex(OBJECT_TABLE),
                    object_table_valid=self.object_table_valid,
                    object_pointer_slots=len(OBJECT_POINTER_SLOTS),
                    object_pointer_slots_valid=self.object_pointer_slots_valid,
                    relocations=self.relocations,
                    full_mcu_emulated=False)

    def close(self):
        # Drop the sole owning reference; Unicorn releases its native context.
        self.uc = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def rgb(frame):
    if len(frame) != 0x600:
        raise ValueError('Expected a 1536-byte shoulder framebuffer')
    out = bytearray()
    for value in frame:
        for bit in range(7, -1, -1):
            out.extend(bytes((220, 230, 225) if value & (1 << bit) else (12, 18, 16)))
    return bytes(out)



class ShoulderService:
    """Worker-owned optional renderer. Published status/PNG are replaced atomically.

    HTTP reads copies from Camera under its existing lock; only the camera
    worker calls reset/poll/close. No QMP or RSP operations are performed here.
    """
    def __init__(self, firmware=None):
        self.firmware = firmware
        self.renderer = None
        self.png = None
        self.reader = None
        self.last_payload = None
        self.status = self._status()

    def _status(self):
        return dict(enabled=self.firmware is not None, firmware_valid=False,
                    available=False, frame=0, last_update=None, error=None,
                    renderer='original-ex1610-function-replay',
                    width=128, height=96, stride=16, framebuffer_size=1536,
                    format='1bpp-row-major-msb-first', full_mcu_emulated=False,
                    message='Waiting for companion firmware' if self.firmware else 'Shoulder display disabled')

    def reset(self):
        from shoulder_protocol import CaptureReader
        self.close()
        self.reader = CaptureReader()
        self.last_payload = None
        self.status = self._status()
        if self.firmware is None:
            return
        try:
            load_firmware(self.firmware)
            self.status['firmware_valid'] = True
            self.renderer = ShoulderDisplay(self.firmware)
            self.status.update(firmware_valid=True, message='Waiting for MAIN shoulder snapshot',
                               **self.renderer.evidence())
        except Exception as exc:
            self._fail(exc)

    def _fail(self, exc):
        self.close()
        # Paths and firmware bytes do not belong in HTTP error responses.
        reason = ('Unicorn is not installed' if isinstance(exc, ImportError)
                  else 'Companion firmware is unavailable' if isinstance(exc, OSError)
                  else 'Companion validation or replay failed: ' + type(exc).__name__)
        self.status.update(available=False, error=reason, message=reason)

    def poll(self, path):
        if self.renderer is None:
            return
        try:
            snapshots = self.reader.poll(path)
            if not snapshots:
                return
            payload = snapshots[-1].payload
            if payload == self.last_payload:
                return
            frame = self.renderer.receive(payload)
            import time
            self.png = framebuffer_png(frame)
            self.last_payload = payload
            self.status.update(available=True, frame=self.status['frame'] + 1,
                               last_update=time.time(), error=None,
                               message='Original ex1610 function replay',
                               rejected=self.reader.rejected,
                               **self.renderer.evidence())
        except Exception as exc:
            self._fail(exc)

    def close(self):
        if self.renderer is not None:
            self.renderer.close()
        self.renderer = None
        self.png = None
        if hasattr(self, 'status'):
            self.status['available'] = False


def framebuffer_png(frame):
    import zlib
    pixels = rgb(frame)
    rows = b''.join(b'\0' + pixels[y * 384:(y + 1) * 384] for y in range(96))
    def chunk(kind, payload):
        body = kind + payload
        return struct.pack('>I', len(payload)) + body + struct.pack('>I', zlib.crc32(body))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 128, 96, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
