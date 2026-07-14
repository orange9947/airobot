import copy
import json
import struct
import unittest

from protocol import generate


class ProtocolGeneratorTests(unittest.TestCase):
    def test_generation_is_deterministic(self):
        schema = generate.load_schema()
        first = generate.desired_outputs(schema)
        second = generate.desired_outputs(copy.deepcopy(schema))
        self.assertEqual(first, second)

    def test_duplicate_message_id_is_rejected(self):
        schema = generate.load_schema()
        schema["messages"][1]["id"] = schema["messages"][0]["id"]
        with self.assertRaisesRegex(ValueError, "duplicate message id"):
            generate.validate_schema(schema)

    def test_slot_size_is_derived_from_payload(self):
        schema = generate.load_schema()
        schema["protocol"]["slot_size"] += 1
        with self.assertRaisesRegex(ValueError, "slot_size"):
            generate.validate_schema(schema)

    def test_fixed_bytes_requires_a_positive_length(self):
        schema = generate.load_schema()
        field = schema["messages"][1]["fields"][0]
        field["type"] = "bytes"
        schema["messages"][1]["sample"][field["name"]] = "00"

        with self.assertRaisesRegex(ValueError, "bytes field requires positive length"):
            generate.validate_schema(schema)

        field["length"] = 0
        with self.assertRaisesRegex(ValueError, "bytes field requires positive length"):
            generate.validate_schema(schema)

    def test_fixed_bytes_sample_must_be_exact_hex(self):
        schema = generate.load_schema()
        field = schema["messages"][1]["fields"][0]
        field.update({"type": "bytes", "length": 2})

        schema["messages"][1]["sample"][field["name"]] = "00zz"
        with self.assertRaisesRegex(ValueError, "invalid bytes sample"):
            generate.validate_schema(schema)

        schema["messages"][1]["sample"][field["name"]] = "00"
        with self.assertRaisesRegex(ValueError, "bytes sample length"):
            generate.validate_schema(schema)

    def test_fixed_bytes_payload_over_limit_is_rejected(self):
        schema = generate.load_schema()
        schema["messages"].append(
            {
                "name": "TOO_LARGE",
                "id": 0x7FFF,
                "fields": [{"name": "data", "type": "bytes", "length": 257}],
                "sample": {"data": "00" * 257},
            }
        )
        with self.assertRaisesRegex(ValueError, "payload too large"):
            generate.validate_schema(schema)

    def test_non_zero_fixed_bytes_tail_is_rejected(self):
        schema = generate.load_schema()
        chunk = next(message for message in schema["messages"] if message["name"] == "RESOURCE_CHUNK")
        chunk["sample"]["data_length"] = 2
        chunk["sample"]["data"] = "010203" + "00" * 235

        with self.assertRaisesRegex(ValueError, "non-zero bytes padding"):
            generate.validate_schema(schema)

    def test_resource_chunk_layout_and_golden_hex_are_stable(self):
        schema = generate.load_schema()
        chunk = next(message for message in schema["messages"] if message["name"] == "RESOURCE_CHUNK")

        self.assertEqual(generate.message_format(chunk), "<IIIHI238s")
        self.assertEqual(struct.calcsize(generate.message_format(chunk)), 256)

        golden = json.loads(generate.render_golden(schema))["cases"]
        golden_chunk = next(case for case in golden if case["name"] == "RESOURCE_CHUNK")
        self.assertIsInstance(golden_chunk["fields"]["data"], str)
        self.assertEqual(golden_chunk["fields"]["data"], chunk["sample"]["data"])
        self.assertEqual(bytes.fromhex(golden_chunk["payload_hex"])[18:22], b"\x01\x02\x03\x04")


if __name__ == "__main__":
    unittest.main()
