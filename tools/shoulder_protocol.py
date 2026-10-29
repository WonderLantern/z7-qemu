# SPDX-License-Identifier: GPL-2.0-or-later
"""Strict shoulder-only decoding of observed MAIN SPI0 request frames.

Header word +4 is retained as an opaque value, not a proven sequence number.
The routing byte and word +6 have no role in classifying a shoulder snapshot.
"""
from dataclasses import dataclass
import re
import struct

PREFIX = 'Z7_SHOULDER_FRAME source=guest-main-firmware spi=0 data='


@dataclass(frozen=True)
class Snapshot:
    payload: bytes
    physical_spi: int
    channel: int
    tag: int
    field_04: int


def parse_frame(raw, *, source, physical_spi):
    raw = bytes(raw)
    if source != 'guest-main-firmware' or physical_spi != 0:
        raise ValueError('Not a MAIN SPI0 request')
    if len(raw) != 256:
        raise ValueError('Incomplete or oversized wire frame')
    channel, route, length, field_04, field_06 = struct.unpack_from('<BBHHH', raw)
    if channel != 8 or length != 112 or raw[8] != 4:
        raise ValueError('Not a channel 8 / tag 4 / 112-byte snapshot')
    if sum(raw) & 255 != 255:
        raise ValueError('Wire checksum mismatch')
    if not (raw[252] >= raw[253] > 0):
        raise ValueError('Invalid wire trailer')
    return Snapshot(raw[8:120], physical_spi, channel, raw[8], field_04)


class CaptureReader:
    """Bounded incremental reader; truncated lines are never padded or replayed."""
    def __init__(self):
        self.position = 0
        self.tail = b''
        self.identity = None
        self.rejected = 0

    def poll(self, path):
        stat = path.stat()
        identity = (stat.st_dev, stat.st_ino)
        if identity != self.identity or stat.st_size < self.position:
            self.position, self.tail = 0, b''
            self.identity = identity
        with path.open('rb') as stream:
            stream.seek(self.position)
            data = self.tail + stream.read(256 * 1024)
            self.position = stream.tell()
        lines = data.split(b'\n')
        self.tail = lines.pop()
        # An oversized unfinished line cannot be one of our fixed-size records.
        if len(self.tail) > 1024:
            self.tail = b'!'
        snapshots = []
        for line in lines:
            if not line.startswith(PREFIX.encode()):
                continue
            if line.endswith(b'\r'):
                line = line[:-1]
            encoded = line[len(PREFIX):]
            if not re.fullmatch(b'[0-9a-f]{512}', encoded):
                self.rejected += 1
                continue
            try:
                snapshots.append(parse_frame(bytes.fromhex(encoded.decode()),
                                 source='guest-main-firmware', physical_spi=0))
            except ValueError:
                self.rejected += 1
        return snapshots
