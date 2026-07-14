import json
import struct
import unittest
from pathlib import Path

from protocol.generated import protocol_ids
from firmware.esp32.transport.crc16 import crc16_ccitt
from firmware.esp32.transport.frame_codec import (
    CRC_OFFSET,
    PAYLOAD_OFFSET,
    SlotError,
    decode_slot,
    encode_slot,
    pack_payload,
    unpack_payload,
)

ROOT = Path(__file__).resolve().parents[1]


class ProtocolPythonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.golden = json.loads((ROOT / "protocol/golden/golden_slots.json").read_text())["cases"]

    def test_crc_standard_vector(self):
        self.assertEqual(crc16_ccitt(b"123456789"), 0x29B1)

    def test_clear_estop_command_layout(self):
        self.assertEqual(protocol_ids.MSG_CLEAR_ESTOP, 0x0206)
        self.assertEqual(protocol_ids.MESSAGE_FORMATS[protocol_ids.MSG_CLEAR_ESTOP], "<I")
        self.assertEqual(protocol_ids.MESSAGE_LENGTHS[protocol_ids.MSG_CLEAR_ESTOP], 4)

    def test_resource_message_layouts(self):
        self.assertEqual(protocol_ids.RESOURCESTATE_BOOT_SCAN, 0)
        self.assertEqual(protocol_ids.RESOURCESTATE_FAILED, 9)
        self.assertEqual(protocol_ids.RESOURCEERROR_NONE, 0)
        self.assertEqual(protocol_ids.RESOURCEERROR_INTERNAL, 21)
        self.assertEqual(protocol_ids.MSG_RESOURCE_BEGIN, 0x0402)
        self.assertEqual(protocol_ids.MSG_RESOURCE_CHUNK, 0x0403)
        self.assertEqual(protocol_ids.MSG_RESOURCE_FINISH, 0x0404)
        self.assertEqual(protocol_ids.MSG_RESOURCE_ABORT, 0x0405)
        self.assertEqual(protocol_ids.MSG_GET_RESOURCE_STATUS, 0x0406)
        self.assertEqual(protocol_ids.MSG_RESOURCE_STATUS, 0x0407)
        self.assertEqual(protocol_ids.MESSAGE_FORMATS[protocol_ids.MSG_RESOURCE_CHUNK], "<IIIHI238s")
        self.assertEqual(protocol_ids.MESSAGE_LENGTHS[protocol_ids.MSG_RESOURCE_CHUNK], 256)

    def test_fixed_bytes_pack_and_unpack(self):
        values = (48, 0x11223344, 0, 4, 0xB63CFBCD, b"\x01\x02\x03\x04")
        payload = pack_payload(protocol_ids.MSG_RESOURCE_CHUNK, values)

        self.assertEqual(len(payload), protocol_ids.PAYLOAD_SIZE)
        self.assertEqual(payload[18:22], b"\x01\x02\x03\x04")
        self.assertEqual(payload[22:], b"\x00" * 234)
        self.assertEqual(unpack_payload(protocol_ids.MSG_RESOURCE_CHUNK, payload)[:-1], values[:-1])
        self.assertEqual(unpack_payload(protocol_ids.MSG_RESOURCE_CHUNK, payload)[-1], values[-1] + b"\x00" * 234)

    def test_resource_chunk_golden_keeps_hex_sample(self):
        chunk = next(case for case in self.golden if case["name"] == "RESOURCE_CHUNK")
        self.assertIsInstance(chunk["fields"]["data"], str)
        decoded = decode_slot(bytes.fromhex(chunk["slot_hex"]))
        self.assertEqual(decoded["values"][3], 4)
        self.assertEqual(decoded["values"][-1][:4], b"\x01\x02\x03\x04")
        self.assertEqual(decoded["values"][-1][4:], b"\x00" * 234)

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
