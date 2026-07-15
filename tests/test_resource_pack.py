import binascii
import json
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools import resource_format
from tools import resource_pack


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def _frame(seed):
    random_source = random.Random(seed)
    return bytes(random_source.randrange(256) for _ in range(resource_format.FRAME_SIZE))


def _recalculate_package_crc(package):
    mutable = bytearray(package)
    mutable[resource_format.PACKAGE_CRC32_OFFSET : resource_format.PACKAGE_CRC32_OFFSET + 4] = (
        b"\x00\x00\x00\x00"
    )
    crc = binascii.crc32(mutable) & 0xFFFFFFFF
    struct.pack_into("<I", mutable, resource_format.PACKAGE_CRC32_OFFSET, crc)
    return bytes(mutable)


class ResourceEncodingTests(unittest.TestCase):
    def test_binary_layout_sizes_and_crc_offset_are_fixed(self):
        self.assertEqual(resource_format.HEADER_STRUCT.format, "<4sHHHHHHIIIII28s")
        self.assertEqual(resource_format.CLIP_STRUCT.format, "<BBHHHII")
        self.assertEqual(resource_format.FRAME_STRUCT.format, "<BBHIIIII")
        self.assertEqual(resource_format.HEADER_STRUCT.size, 64)
        self.assertEqual(resource_format.CLIP_STRUCT.size, 16)
        self.assertEqual(resource_format.FRAME_STRUCT.size, 24)
        self.assertEqual(resource_format.PACKAGE_CRC32_OFFSET, 32)

    def test_rle_round_trip_for_representative_frames(self):
        frames = (
            bytes(resource_format.FRAME_SIZE),
            bytes([0xFF]) * resource_format.FRAME_SIZE,
            bytes([0x00, 0xFF]) * (resource_format.FRAME_SIZE // 2),
            _frame(9947),
        )
        for raw in frames:
            with self.subTest(prefix=raw[:8]):
                encoded = resource_format.encode_rle(raw)
                self.assertEqual(resource_format.decode_rle(encoded), raw)

    def test_rle_rejects_truncated_overlong_and_short_output(self):
        invalid_streams = (
            b"\x80",  # Repeat control without its value.
            b"\x02\x01\x02",  # Literal control missing one byte.
            b"\xff\x00",  # Valid run, but decoded output is too short.
            (b"\xff\x00" * 9),  # Decoded output exceeds 1024 bytes.
        )
        for encoded in invalid_streams:
            with self.subTest(encoded=encoded):
                with self.assertRaises(resource_format.ResourceFormatError):
                    resource_format.decode_rle(encoded)

    def test_encoder_uses_rle_only_when_it_is_strictly_shorter(self):
        encoding, encoded = resource_format.encode_frame(bytes(resource_format.FRAME_SIZE))
        self.assertEqual(encoding, resource_format.ENCODING_RLE1)
        self.assertLess(len(encoded), resource_format.FRAME_SIZE)

        incompressible = bytes(range(256)) * 4
        encoding, encoded = resource_format.encode_frame(incompressible)
        self.assertEqual(encoding, resource_format.ENCODING_RAW1)
        self.assertEqual(encoded, incompressible)

    def test_frame_functions_reject_non_bytes_values(self):
        for value in (resource_format.FRAME_SIZE, None, "\x00" * resource_format.FRAME_SIZE):
            with self.subTest(value_type=type(value).__name__):
                with self.assertRaises(resource_format.ResourceFormatError):
                    resource_format.encode_frame(value)


class ResourcePackageTests(unittest.TestCase):
    def setUp(self):
        self.sources = (
            resource_format.ClipSource(
                expression_id=resource_format.EXPRESSION_IDS["NEUTRAL"],
                weight=3,
                frame_interval_ms=120,
                frames=(bytes(resource_format.FRAME_SIZE), _frame(1)),
            ),
            resource_format.ClipSource(
                expression_id=resource_format.EXPRESSION_IDS["HAPPY"],
                weight=1,
                frame_interval_ms=80,
                frames=(bytes([0xFF]) * resource_format.FRAME_SIZE,),
            ),
        )

    def test_build_is_deterministic_and_round_trips(self):
        first = resource_format.build_package(self.sources)
        second = resource_format.build_package(self.sources)
        self.assertEqual(first, second)

        package = resource_format.parse_package(first)
        self.assertEqual(resource_format.verify_package(first), package)
        self.assertEqual(package.version, resource_format.FORMAT_VERSION)
        self.assertEqual(package.width, 128)
        self.assertEqual(package.height, 64)
        self.assertEqual(len(package.clips), 2)
        self.assertEqual(len(package.frames), 3)
        self.assertEqual(package.clips[1].expression_id, resource_format.EXPRESSION_IDS["HAPPY"])
        self.assertEqual(package.clips[0].first_frame_index, 0)
        self.assertEqual(package.clips[1].first_frame_index, 2)
        self.assertEqual(package.decoded_frame(0), bytes(resource_format.FRAME_SIZE))
        self.assertEqual(package.decoded_frame(1), _frame(1))

        stored_crc = struct.unpack_from("<I", first, resource_format.PACKAGE_CRC32_OFFSET)[0]
        mutable = bytearray(first)
        mutable[resource_format.PACKAGE_CRC32_OFFSET : resource_format.PACKAGE_CRC32_OFFSET + 4] = (
            b"\x00\x00\x00\x00"
        )
        self.assertEqual(stored_crc, binascii.crc32(mutable) & 0xFFFFFFFF)

    def test_parser_rejects_package_and_frame_crc_corruption(self):
        package = bytearray(resource_format.build_package(self.sources))
        package[-1] ^= 0x01
        with self.assertRaisesRegex(resource_format.ResourceFormatError, "package CRC32"):
            resource_format.parse_package(package)

        package = bytearray(resource_format.build_package(self.sources))
        frame_table_offset = struct.unpack_from("<I", package, 20)[0]
        struct.pack_into(
            "<I", package, frame_table_offset + resource_format.FRAME_CRC32_OFFSET, 0
        )
        package = _recalculate_package_crc(package)
        with self.assertRaisesRegex(resource_format.ResourceFormatError, "frame 0 CRC32"):
            resource_format.parse_package(package)

    def test_parser_rejects_nonzero_reserved_fields(self):
        original = resource_format.build_package(self.sources)
        clip_table_offset = struct.unpack_from("<I", original, 16)[0]
        frame_table_offset = struct.unpack_from("<I", original, 20)[0]
        for offset in (36, clip_table_offset + 6, clip_table_offset + 12, frame_table_offset + 1,
                       frame_table_offset + 2, frame_table_offset + 20):
            with self.subTest(offset=offset):
                package = bytearray(original)
                package[offset] = 1
                package = _recalculate_package_crc(package)
                with self.assertRaisesRegex(resource_format.ResourceFormatError, "reserved"):
                    resource_format.parse_package(package)

    def test_parser_rejects_overlapping_tables(self):
        package = bytearray(resource_format.build_package(self.sources))
        clip_table_offset = struct.unpack_from("<I", package, 16)[0]
        struct.pack_into("<I", package, 20, clip_table_offset)
        package = _recalculate_package_crc(package)
        with self.assertRaisesRegex(resource_format.ResourceFormatError, "immediately"):
            resource_format.parse_package(package)

    def test_parser_rejects_gaps_between_metadata_regions(self):
        original = resource_format.build_package(self.sources)
        cases = tuple(struct.unpack_from("<I", original, offset)[0]
                      for offset in (16, 20, 24))
        for insert_offset in cases:
            with self.subTest(insert_offset=insert_offset):
                package = bytearray(original)
                package[insert_offset:insert_offset] = b"\xa5"
                struct.pack_into("<I", package, 28, len(package))
                for offset_field in (16, 20, 24):
                    old_offset = struct.unpack_from("<I", package, offset_field)[0]
                    if old_offset >= insert_offset:
                        struct.pack_into("<I", package, offset_field, old_offset + 1)
                frame_table_offset = struct.unpack_from("<I", package, 20)[0]
                for frame_index in range(3):
                    data_offset_field = (
                        frame_table_offset
                        + frame_index * resource_format.FRAME_STRUCT.size
                        + 4
                    )
                    old_offset = struct.unpack_from(
                        "<I", package, data_offset_field
                    )[0]
                    if old_offset >= insert_offset:
                        struct.pack_into(
                            "<I", package, data_offset_field, old_offset + 1
                        )
                package = _recalculate_package_crc(package)
                with self.assertRaisesRegex(
                    resource_format.ResourceFormatError, "immediately"
                ):
                    resource_format.parse_package(package)

    def test_parser_rejects_gap_between_frame_data(self):
        package = bytearray(resource_format.build_package(self.sources))
        frame_table_offset = struct.unpack_from("<I", package, 20)[0]
        second_data_offset = struct.unpack_from(
            "<I", package, frame_table_offset + resource_format.FRAME_STRUCT.size + 4
        )[0]
        package[second_data_offset:second_data_offset] = b"\xa5"
        struct.pack_into("<I", package, 28, len(package))
        for frame_index in range(1, 3):
            data_offset_field = frame_table_offset + frame_index * resource_format.FRAME_STRUCT.size + 4
            old_offset = struct.unpack_from("<I", package, data_offset_field)[0]
            struct.pack_into("<I", package, data_offset_field, old_offset + 1)
        package = _recalculate_package_crc(package)

        with self.assertRaisesRegex(resource_format.ResourceFormatError, "not contiguous"):
            resource_format.parse_package(package)

    def test_parser_rejects_unreferenced_trailing_data(self):
        package = bytearray(resource_format.build_package(self.sources))
        package.append(0xA5)
        struct.pack_into("<I", package, 28, len(package))
        package = _recalculate_package_crc(package)

        with self.assertRaisesRegex(resource_format.ResourceFormatError, "unreferenced trailing"):
            resource_format.parse_package(package)

    def test_builder_rejects_invalid_clip_limits(self):
        invalid = (
            resource_format.ClipSource(6, 1, 100, (bytes(resource_format.FRAME_SIZE),)),
            resource_format.ClipSource(0, 0, 100, (bytes(resource_format.FRAME_SIZE),)),
            resource_format.ClipSource(0, 1, 49, (bytes(resource_format.FRAME_SIZE),)),
            resource_format.ClipSource(0, 1, 2001, (bytes(resource_format.FRAME_SIZE),)),
            resource_format.ClipSource(0, 1, 100, ()),
        )
        for source in invalid:
            with self.subTest(source=source):
                with self.assertRaises(resource_format.ResourceFormatError):
                    resource_format.build_package((source,))

        too_many = tuple(
            resource_format.ClipSource(0, 1, 100, (bytes(resource_format.FRAME_SIZE),))
            for _ in range(resource_format.MAX_CLIPS + 1)
        )
        with self.assertRaisesRegex(resource_format.ResourceFormatError, "clip count"):
            resource_format.build_package(too_many)


class ResourceManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            from PIL import Image  # noqa: F401
        except ImportError:
            cls.pillow_available = False
        else:
            cls.pillow_available = True

    def _write_manifest(self, directory, clips):
        manifest = directory / "manifest.json"
        manifest.write_text(json.dumps({"clips": clips}), encoding="utf-8")
        return manifest

    def test_manifest_rejects_unknown_expression_and_invalid_ranges_without_opening_png(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            cases = (
                {"expression": "ANGRY", "weight": 1, "frame_interval_ms": 100, "frames": ["x.png"]},
                {"expression": "HAPPY", "weight": 0, "frame_interval_ms": 100, "frames": ["x.png"]},
                {"expression": "HAPPY", "weight": 1, "frame_interval_ms": 49, "frames": ["x.png"]},
                {"expression": "HAPPY", "weight": 1, "frame_interval_ms": 100, "frames": []},
            )
            for index, clip in enumerate(cases):
                with self.subTest(index=index):
                    manifest = self._write_manifest(directory, [clip])
                    with self.assertRaises(resource_format.ResourceFormatError):
                        resource_pack.build_from_manifest(manifest)

    def test_png_threshold_and_page_major_conversion(self):
        if not self.pillow_available:
            self.skipTest("Pillow is not installed in this interpreter")
        from PIL import Image

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            image_path = directory / "frame.png"
            image = Image.new("RGBA", (128, 64), (0, 0, 0, 255))
            image.putpixel((0, 0), (255, 255, 255, 255))
            image.putpixel((0, 7), (255, 255, 255, 255))
            image.putpixel((1, 8), (128, 128, 128, 255))
            image.putpixel((2, 8), (255, 255, 255, 127))
            image.putpixel((3, 8), (127, 127, 127, 255))
            image.save(image_path, format="PNG")

            raw = resource_pack.png_to_frame(image_path)
            self.assertEqual(len(raw), resource_format.FRAME_SIZE)
            self.assertEqual(raw[0], 0b10000001)
            self.assertEqual(raw[128 + 1], 0b00000001)
            self.assertEqual(raw[128 + 2], 0)
            self.assertEqual(raw[128 + 3], 0)

    def test_png_rejects_wrong_dimensions(self):
        if not self.pillow_available:
            self.skipTest("Pillow is not installed in this interpreter")
        from PIL import Image

        with tempfile.TemporaryDirectory() as temporary:
            image_path = Path(temporary) / "small.png"
            Image.new("1", (127, 64)).save(image_path, format="PNG")
            with self.assertRaisesRegex(resource_format.ResourceFormatError, "128 x 64"):
                resource_pack.png_to_frame(image_path)

    def test_manifest_build_and_cli_are_deterministic(self):
        if not self.pillow_available:
            self.skipTest("Pillow is not installed in this interpreter")
        from PIL import Image

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            image_path = directory / "face.png"
            Image.new("1", (128, 64), 1).save(image_path, format="PNG")
            manifest = self._write_manifest(
                directory,
                [
                    {
                        "expression": "SLEEPY",
                        "weight": 2,
                        "frame_interval_ms": 250,
                        "frames": [image_path.name],
                    }
                ],
            )

            first = resource_pack.build_from_manifest(manifest)
            second = resource_pack.build_from_manifest(manifest)
            self.assertEqual(first, second)
            parsed = resource_format.parse_package(first)
            self.assertEqual(parsed.clips[0].expression_id, resource_format.EXPRESSION_IDS["SLEEPY"])

            output = directory / "faces.arp"
            result = subprocess.run(
                [sys.executable, str(PROJECT_ROOT / "tools" / "resource_pack.py"), "build", str(manifest), str(output)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_bytes(), first)

            verify = subprocess.run(
                [sys.executable, str(PROJECT_ROOT / "tools" / "resource_pack.py"), "verify", str(output)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(verify.returncode, 0, verify.stderr)
            self.assertIn("valid", verify.stdout.lower())

            inspect = subprocess.run(
                [sys.executable, str(PROJECT_ROOT / "tools" / "resource_pack.py"), "inspect", str(output)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(inspect.returncode, 0, inspect.stderr)
            details = json.loads(inspect.stdout)
            self.assertEqual(details["clip_count"], 1)
            self.assertEqual(details["frame_count"], 1)
            self.assertEqual(details["clips"][0]["expression"], "SLEEPY")


if __name__ == "__main__":
    unittest.main()
