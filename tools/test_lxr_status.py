#!/usr/bin/env python3
"""Tests for tools/lxr_status.py. Run: python3 tools/test_lxr_status.py

The frames are the same golden vectors as test_status() in
tests/host/test_sysex_codec.c and the example in docs/sysex.md 5.4.

SPDX-License-Identifier: GPL-3.0-only
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lxr_status  # noqa: E402

REPLY = bytes.fromhex("F0 7D 4C 00 01 04 05 02 01 00 7B 00 68 07 50 00 14 00 17 00 03 00 47 F7")
LEGACY = bytes.fromhex("F0 7D 4C 00 01 04 05 02 01 00 03 F7")


class StatusTest(unittest.TestCase):
    def test_request(self):
        self.assertEqual(lxr_status.build_status_request().hex(" ").upper(), "F0 7D 4C 7F 01 03 7D F7")
        self.assertEqual(lxr_status.build_status_request(reset=True).hex(" ").upper(),
                         "F0 7D 4C 7F 01 03 01 7C F7")
        self.assertEqual(lxr_status.build_status_request(device=0).hex(" ").upper(), "F0 7D 4C 00 01 03 02 F7")

    def test_reply(self):
        st = lxr_status.parse_status_reply(REPLY)
        self.assertIsNotNone(st)
        self.assertTrue(st["has_cpu"])
        self.assertEqual((st["kit"], st["pattern"], st["running"], st["echo"]), (5, 2, True, False))
        self.assertAlmostEqual(st["cpu_now"], 12.3)
        self.assertAlmostEqual(st["cpu_peak"], 100.0)
        self.assertAlmostEqual(st["cpu_voices"], 8.0)
        self.assertAlmostEqual(st["cpu_mixer"], 2.0)
        self.assertAlmostEqual(st["cpu_fx"], 2.3)
        self.assertEqual(st["underruns"], 3)
        self.assertIn("cpu  12.3%", lxr_status.format_status(st))
        # mido strips F0/F7
        self.assertEqual(lxr_status.parse_status_reply(REPLY[1:-1]), st)

    def test_legacy_reply(self):
        st = lxr_status.parse_status_reply(LEGACY)
        self.assertIsNotNone(st)
        self.assertFalse(st["has_cpu"])
        self.assertNotIn("cpu_now", st)
        self.assertIn("no CPU meter", lxr_status.format_status(st))

    def test_rejects(self):
        bad = bytearray(REPLY)
        bad[-2] ^= 1
        self.assertIsNone(lxr_status.parse_status_reply(bytes(bad)))
        other = bytearray(LEGACY)
        other[5] = 0x02  # IDENTITY_REPLY
        other[-2] = lxr_status.checksum(other[3:-2])
        self.assertIsNone(lxr_status.parse_status_reply(bytes(other)))
        self.assertIsNone(lxr_status.parse_status_reply(b"\xF0\x41\x10\xF7"))


if __name__ == "__main__":
    unittest.main()
