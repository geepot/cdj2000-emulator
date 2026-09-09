# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

"""The link-dump decoder against records built by the repository's own builders."""

from __future__ import annotations

import struct
import unittest

from tools.cdj_gui.build_status_record import build_status_record
from tools.cdj_gui.build_type1_list import build_type1_list
from tools.cdj_gui.decode_link_dump import decode, format_summary, iter_records
from tools.cdj_gui.main_packet import firmware_crc


def framed(*bodies: bytes) -> bytes:
    return b"".join(b"SPRX" + len(body).to_bytes(4, "little") + body for body in bodies)


def announcing(count: int, halfwords: int) -> bytes:
    """A status record that announces a payload: words 29/30 set, CRC in word 31."""

    record = bytearray(build_status_record())
    struct.pack_into("<HH", record, 58, count, halfwords)
    struct.pack_into("<H", record, 62, firmware_crc(bytes(record[:62])))
    return bytes(record)


class DecodeLinkDumpTests(unittest.TestCase):
    def test_records_are_split_on_the_magic_and_length(self) -> None:
        status = build_status_record()
        payload = build_type1_list(["SD", "A"], word_count=24)
        self.assertEqual(list(iter_records(framed(status, payload, status))),
                         [status, payload, status])

    def test_list_rows_and_repeats_are_reported(self) -> None:
        plain = build_status_record()
        payload = build_type1_list(["SD", "Afrohouse", "AAA-Tracks"],
                                   attrs=[(0x0100, 0), (0x0030, 1), (0x0030, 2)],
                                   word_count=48)
        self.assertEqual(len(payload), 96)
        lines, summary = decode(framed(plain, announcing(1, 48), payload, payload, plain))
        text = "\n".join(lines)
        self.assertIn("payload   96 bytes x2", text)
        self.assertIn("'Afrohouse'", text)
        self.assertIn("attr 0x0030/0x0002  'AAA-Tracks'", text)
        self.assertIn("list cursor 1: total 3", text)
        self.assertEqual(summary.records, 5)
        self.assertEqual(summary.lengths[64], 3)
        self.assertEqual(summary.delivered[96], 2)

    def test_an_undelivered_announcement_is_called_out(self) -> None:
        lines, summary = decode(framed(build_status_record(), announcing(1, 448), announcing(1, 448)))
        report = "\n".join(format_summary(summary))
        self.assertIn("(1, 448) =  896 bytes  records 2..3  delivered x0  <-- never delivered", report)
        self.assertTrue(any("w29=1 w30=448" in line for line in lines))

    def test_64_byte_nxs_list_is_payload_not_status(self) -> None:
        status = announcing(1, 32)
        payload = build_type1_list(["SD", "TESTTONE.WAV"], word_count=32)
        lines, summary = decode(framed(status, payload, payload, status))
        self.assertEqual(len(payload), 64)
        self.assertEqual(summary.delivered[64], 2)
        self.assertEqual(summary.announcements, [(1, 32, 1, 4)])
        self.assertIn("'TESTTONE.WAV'", "\n".join(lines))
        self.assertIn("payload   64 bytes x2", "\n".join(lines))
        self.assertNotIn("never delivered", "\n".join(format_summary(summary)))

    def test_nxs_status_prefix_and_64_byte_player_payload(self) -> None:
        status = build_status_record(overrides={1: 0x8800})
        payload = struct.pack('<32H', 0x19, *([0] * 31))
        lines, summary = decode(framed(status, payload))
        self.assertEqual(summary.player_states, 1)
        self.assertEqual(summary.delivered[64], 1)
        self.assertEqual(sum(' status ' in line for line in lines), 1)


if __name__ == "__main__":
    unittest.main()
