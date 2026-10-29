# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently synthesized frames; no camera captures or firmware."""
import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from shoulder_protocol import CaptureReader, PREFIX, parse_frame


def wire(*, channel=8, tag=4, length=112, field_04=19):
    data = bytearray(256)
    data[0:2] = bytes((channel, 2))
    data[2:4] = length.to_bytes(2, 'little')
    data[4:6] = field_04.to_bytes(2, 'little')
    data[8] = tag
    data[9:120] = bytes((i * 17 & 255) for i in range(111))
    data[252:254] = b'\1\1'
    data[255] = (255 - sum(data[:255])) & 255
    return bytes(data)


def parse(data, **kw):
    return parse_frame(data, source=kw.get('source', 'guest-main-firmware'),
                       physical_spi=kw.get('physical_spi', 0))


class ProtocolTests(unittest.TestCase):
    def test_snapshot_and_fields(self):
        s = parse(wire())
        self.assertEqual((s.physical_spi, s.channel, s.tag, s.field_04), (0, 8, 4, 19))
        self.assertEqual(s.payload, wire()[8:120])
        self.assertEqual(len(s.payload), 112)

    def test_length_tag_channel_and_origin(self):
        for data in (wire(length=111), wire(length=113), wire(length=65535),
                     wire(tag=5), wire(channel=7), wire()[:-1], wire() + b'\0'):
            with self.subTest(data_length=len(data)), self.assertRaises(ValueError):
                parse(data)
        for kw in ({'physical_spi': 1}, {'physical_spi': 8}, {'source': 'qemu-test-peer'}):
            with self.subTest(kw=kw), self.assertRaises(ValueError):
                parse(wire(), **kw)

    def test_checksum_and_trailer(self):
        bad = bytearray(wire()); bad[30] ^= 1
        with self.assertRaises(ValueError): parse(bad)
        bad[252] = 0; bad[255] = (255 - sum(bad[:255])) & 255
        with self.assertRaises(ValueError): parse(bad)

    def test_incremental_rejection_and_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace'
            reader = CaptureReader()
            record = PREFIX + wire().hex() + '\n'
            path.write_bytes(('unrelated\n' + record[:200]).encode())
            self.assertEqual(reader.poll(path), [])
            with path.open('ab') as f: f.write(record[200:].encode())
            self.assertEqual(len(reader.poll(path)), 1)
            self.assertEqual(reader.poll(path), [])
            with path.open('ab') as f: f.write((PREFIX + '00\n' + record).encode())
            self.assertEqual(len(reader.poll(path)), 1)
            self.assertEqual(reader.rejected, 1)
            path.unlink(); path.write_bytes(record.encode())
            self.assertEqual(len(reader.poll(path)), 1)


    def test_crlf_and_split_terminator(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace'
            reader = CaptureReader()
            first = wire()
            second = bytearray(wire())
            second[9] ^= 1
            second[255] = (255 - sum(second[:255])) & 255
            record_crlf = (PREFIX + first.hex() + '\r\n').encode()
            record_lf = (PREFIX + second.hex() + '\n').encode()

            path.write_bytes(record_crlf)
            self.assertEqual([s.payload for s in reader.poll(path)], [first[8:120]])
            self.assertEqual(reader.poll(path), [])

            # Truncate, then complete the pending CRLF and add a full LF record.
            path.write_bytes(record_crlf[:-1])
            self.assertEqual(reader.poll(path), [])
            with path.open('ab') as f:
                f.write(b'\n' + record_lf)
            snaps = reader.poll(path)
            self.assertEqual(len(snaps), 2)
            self.assertEqual([s.payload for s in snaps], [first[8:120], second[8:120]])
            self.assertEqual(reader.rejected, 0)
            self.assertEqual(reader.poll(path), [])
            self.assertEqual(reader.rejected, 0)

            path.write_bytes(record_lf)
            self.assertEqual([s.payload for s in reader.poll(path)], [second[8:120]])
            self.assertEqual(reader.poll(path), [])
            self.assertEqual(reader.rejected, 0)

    def test_split_crlf_terminator_only(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace'
            reader = CaptureReader()
            path.write_bytes((PREFIX + wire().hex() + '\r').encode())
            self.assertEqual(reader.poll(path), [])
            self.assertEqual(reader.rejected, 0)
            with path.open('ab') as f:
                f.write(b'\n')
            snaps = reader.poll(path)
            self.assertEqual(len(snaps), 1)
            self.assertEqual(snaps[0].payload, wire()[8:120])
            self.assertEqual(reader.poll(path), [])
            self.assertEqual(reader.rejected, 0)

    def test_rejects_malformed_records(self):
        valid = wire().hex().encode()
        checksum = bytearray(wire()); checksum[30] ^= 1
        trailer = bytearray(wire()); trailer[252] = 0
        trailer[255] = (255 - sum(trailer[:255])) & 255
        records = (
            valid + b'\r\r', valid + b'x', valid + b' ',
            b'g' + valid[1:], valid.upper(), valid[:-2], valid + b'00',
            checksum.hex().encode(), trailer.hex().encode(),
            wire(length=111).hex().encode(), wire(channel=7).hex().encode(),
            wire(tag=5).hex().encode(),
        )
        with tempfile.TemporaryDirectory() as directory:
            for encoded in records:
                with self.subTest(encoded_length=len(encoded), suffix=encoded[-4:]):
                    path = Path(directory) / 'trace'
                    reader = CaptureReader()
                    path.write_bytes(PREFIX.encode() + encoded + b'\n')
                    self.assertEqual(reader.poll(path), [])
                    self.assertEqual(reader.rejected, 1)
                    self.assertEqual(reader.poll(path), [])
                    self.assertEqual(reader.rejected, 1)
                    # A rejected record must not discard the following valid one.
                    with path.open('ab') as f:
                        f.write(PREFIX.encode() + valid + b'\r\n')
                    self.assertEqual([s.payload for s in reader.poll(path)], [wire()[8:120]])
                    self.assertEqual(reader.rejected, 1)
