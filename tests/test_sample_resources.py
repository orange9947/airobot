import tempfile
import unittest
from pathlib import Path

from PIL import Image

from tools import resource_pack
from tools.generate_sample_faces import EXPRESSIONS, generate


class SampleResourcesTests(unittest.TestCase):
    def test_sample_frames_are_deterministic_and_build_a_valid_package(self):
        with tempfile.TemporaryDirectory() as first_root, tempfile.TemporaryDirectory() as second_root:
            first = Path(first_root)
            second = Path(second_root)
            first_manifest = generate(first)
            second_manifest = generate(second)

            self.assertEqual(first_manifest, second_manifest)
            self.assertEqual(len(first_manifest["clips"]), 12)
            self.assertEqual(
                {clip["expression"] for clip in first_manifest["clips"]},
                set(EXPRESSIONS),
            )
            for expression in EXPRESSIONS:
                clips = [
                    clip
                    for clip in first_manifest["clips"]
                    if clip["expression"] == expression
                ]
                self.assertEqual([clip["weight"] for clip in clips], [3, 1])

            first_files = sorted((first / "frames").glob("*.png"))
            second_files = sorted((second / "frames").glob("*.png"))
            self.assertEqual(len(first_files), 36)
            self.assertEqual(
                [path.read_bytes() for path in first_files],
                [path.read_bytes() for path in second_files],
            )
            committed = Path(__file__).resolve().parents[1] / (
                "examples/resources/default_faces"
            )
            committed_files = sorted((committed / "frames").glob("*.png"))
            self.assertEqual(
                [path.name for path in first_files],
                [path.name for path in committed_files],
            )
            self.assertEqual(
                (first / "manifest.json").read_bytes(),
                (committed / "manifest.json").read_bytes(),
            )
            self.assertEqual(
                [path.read_bytes() for path in first_files],
                [path.read_bytes() for path in committed_files],
            )
            for path in first_files:
                with Image.open(path) as image:
                    self.assertEqual(image.size, (128, 64))
                    self.assertEqual(image.format, "PNG")

            package = resource_pack.build_from_manifest(first / "manifest.json")
            parsed = resource_pack.resource_format.verify_package(package)
            self.assertEqual(len(parsed.clips), 12)
            self.assertEqual(len(parsed.frames), 36)


if __name__ == "__main__":
    unittest.main()
