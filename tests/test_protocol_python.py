import json
import struct
import unittest
from pathlib import Path

from firmware.esp32.transport.crc16 import crc16_ccitt
from firmware.esp32.transport.frame_codec import (
    CRC_OFFSET,
    PAYLOAD_OFFSET,
    SlotError,
    decode_slot,
    encode_slot,
)

ROOT = Path(__file__).resolve().parents[1]


class ProtocolPythonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.golden = json.loads((ROOT / "protocol/golden/golden_slots.json").read_text())["cases"]

    def test_crc_standard_vector(self):
        self.assertEqual(crc16_ccitt(b"123456789"), 0x29B1)

    def test_all_golden_slots_round_trip(self):
        for case in self.golden:
            with self.subTest(case=case["name"]):
                raw = bytes.fromhex(case["slot_hex"])
                decoded = decode_slot(raw)
                self.assertEqual(decoded["type"], case["type"])
                self.assertEqual(decoded["seq"], case["seq"])
                rebuilt = encode_slot(decoded["type"], decoded["seq"], decoded["flags"], decoded["payload"])
                self.assertEqual(rebuilt, raw)

    def test_rejects_bad_size_magic_version_padding_and_crc(self):
        original = bytearray(bytes.fromhex(self.golden[1]["slot_hex"]))
        cases = []
        cases.append(bytes(original[:-1]))

        bad_magic = bytearray(original)
        bad_magic[0] ^= 1
        cases.append(bytes(bad_magic))

        bad_version = bytearray(original)
        bad_version[2] += 1
        cases.append(bytes(bad_version))

        bad_padding = bytearray(original)
        length = struct.unpack_from("<H", bad_padding, 8)[0]
        bad_padding[PAYLOAD_OFFSET + length] = 1
        cases.append(bytes(bad_padding))

        bad_crc = bytearray(original)
        bad_crc[CRC_OFFSET] ^= 1
        cases.append(bytes(bad_crc))

        for raw in cases:
            with self.subTest(raw_len=len(raw)):
                with self.assertRaises(SlotError):
                    decode_slot(raw)


if __name__ == "__main__":
    unittest.main()
