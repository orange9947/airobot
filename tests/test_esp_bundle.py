import json
import tempfile
import unittest
from pathlib import Path

from tools.build_esp_bundle import build_bundle


class EspBundleTests(unittest.TestCase):
    def test_bundle_contains_runtime_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = build_bundle(Path(temporary) / "bundle")
            manifest = json.loads((output / "bundle-manifest.json").read_text())
            self.assertTrue((output / "boot.py").is_file())
            self.assertTrue((output / "main.py").is_file())
            self.assertTrue((output / "www" / "app.js").is_file())
            self.assertTrue((output / "protocol" / "generated" / "protocol_ids.py").is_file())
            self.assertFalse((output / "firmware" / "stm32").exists())
            self.assertFalse(any("__pycache__" in name for name in manifest["files"]))


if __name__ == "__main__":
    unittest.main()
