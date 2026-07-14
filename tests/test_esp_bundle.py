import json
import tempfile
import unittest
from pathlib import Path

from tools.build_esp_bundle import build_bundle


PROJECT_ROOT = Path(__file__).resolve().parents[1]


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

    def test_mobile_navigation_and_first_run_settings_contract(self):
        html = (PROJECT_ROOT / "web" / "index.html").read_text()
        css = (PROJECT_ROOT / "web" / "styles.css").read_text()
        javascript = (PROJECT_ROOT / "web" / "app.js").read_text()

        for element_id in ("menu-button", "primary-sidebar", "menu-close", "nav-backdrop"):
            self.assertIn(f'id="{element_id}"', html)
        self.assertIn('aria-controls="primary-sidebar"', html)
        self.assertIn('aria-expanded="false"', html)

        self.assertIn("100dvh", css)
        self.assertIn("safe-area-inset-left", css)
        self.assertIn(".sidebar.open", css)

        self.assertIn("function openNavigation", javascript)
        self.assertIn("function closeNavigation", javascript)
        self.assertIn('status.network.mode === "access_point"', javascript)
        self.assertIn('!config.wifi.ssid', javascript)

    def test_chat_context_clear_confirmation_contract(self):
        html = (PROJECT_ROOT / "web" / "index.html").read_text()
        javascript = (PROJECT_ROOT / "web" / "app.js").read_text()

        for element_id in (
            "clear-chat-context",
            "clear-chat-layer",
            "clear-chat-dialog",
            "clear-chat-cancel",
            "clear-chat-confirm",
        ):
            self.assertIn(f'id="{element_id}"', html)
        self.assertIn('role="dialog"', html)
        self.assertIn('aria-modal="true"', html)
        self.assertIn("function openClearChatDialog", javascript)
        self.assertIn("function closeClearChatDialog", javascript)
        self.assertIn('api("/api/v1/chat", { method: "DELETE"', javascript)

    def test_login_progress_and_estop_recovery_contract(self):
        html = (PROJECT_ROOT / "web" / "index.html").read_text()
        javascript = (PROJECT_ROOT / "web" / "app.js").read_text()

        for element_id in (
            "auth-status",
            "auth-submit-icon",
            "estop-recovery",
            "clear-estop-button",
            "clear-estop-layer",
            "clear-estop-dialog",
            "clear-estop-cancel",
            "clear-estop-confirm",
        ):
            self.assertIn(f'id="{element_id}"', html)
        self.assertIn("AUTH_VERIFYING", javascript)
        self.assertIn("AUTH_LOADING", javascript)
        self.assertIn("AbortController", javascript)
        self.assertIn('stateName === "estop"', javascript)
        self.assertIn('api("/api/v1/estop/clear"', javascript)


if __name__ == "__main__":
    unittest.main()
