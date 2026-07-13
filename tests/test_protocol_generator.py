import copy
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


if __name__ == "__main__":
    unittest.main()
