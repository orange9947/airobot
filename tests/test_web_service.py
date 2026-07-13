import asyncio
import json
import tempfile
import unittest

from firmware.esp32.services.config_service import ConfigService
from firmware.esp32.services.web_service import ApiError, WebService
from protocol.generated import protocol_ids


class FakeDevice:
    def __init__(self):
        self.status = {
            "connected": True,
            "state": protocol_ids.ROBOTSTATE_IDLE,
            "selected_mode": protocol_ids.MODE_IDLE,
            "fault_code": 0,
        }
        self.calls = []

    async def set_mode(self, mode):
        self.calls.append(("mode", mode))
        return 11

    async def move(self, left, right, rate, accel, timeout):
        self.calls.append(("move", left, right, rate, accel, timeout))
        return {"ok": True, "left_steps": left, "right_steps": right}

    async def stop(self):
        self.calls.append(("stop",))
        return 12


class FakeLlm:
    async def chat(self, message):
        return {"text": "answer: " + message, "provider": "deepseek"}

    def clear(self):
        pass


class FakeNetwork:
    def status(self):
        return {"mode": "station", "ip": "192.0.2.5", "setup_ssid": ""}


class FakeWriter:
    def __init__(self):
        self.data = bytearray()

    def write(self, value):
        self.data.extend(value)

    async def drain(self):
        pass


class WebServiceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.config = ConfigService(self.temporary.name)
        self.config.load()
        self.config.set_admin_password("test-password", iterations=2)
        self.config.update_public({"wifi": {"ssid": "lab", "password": "wifi-secret"}})
        self.device = FakeDevice()
        self.web = WebService(self.config, self.device, FakeLlm(), FakeNetwork(), static_root="web")

    def tearDown(self):
        self.temporary.cleanup()

    def login_headers(self):
        async def login():
            status, payload, extra = await self.web.dispatch_api(
                "POST", "/api/v1/session/login", {}, json.dumps({"password": "test-password"}).encode()
            )
            self.assertEqual(status, 200)
            cookie = extra["Set-Cookie"].split(";", 1)[0]
            return {"cookie": cookie, "x-csrf-token": payload["csrf"]}

        return asyncio.run(login())

    def test_bootstrap_is_public_but_status_requires_login(self):
        async def scenario():
            status, payload, _extra = await self.web.dispatch_api("GET", "/api/v1/bootstrap", {}, b"")
            self.assertEqual(status, 200)
            self.assertFalse(payload["setup_required"])
            with self.assertRaisesRegex(ApiError, "authentication"):
                await self.web.dispatch_api("GET", "/api/v1/status", {}, b"")

        asyncio.run(scenario())

    def test_config_redacts_wifi_password_and_api_key(self):
        headers = self.login_headers()
        self.config.set_provider_key("deepseek", "secret-token")

        async def scenario():
            _status, payload, _extra = await self.web.dispatch_api("GET", "/api/v1/config", headers, b"")
            rendered = json.dumps(payload)
            self.assertNotIn("wifi-secret", rendered)
            self.assertNotIn("secret-token", rendered)
            self.assertTrue(payload["wifi"]["password_configured"])
            self.assertTrue(payload["keys_configured"]["deepseek"])

        asyncio.run(scenario())

    def test_mutation_requires_csrf_and_motion_is_bounded(self):
        headers = self.login_headers()

        async def scenario():
            with self.assertRaisesRegex(ApiError, "CSRF"):
                await self.web.dispatch_api(
                    "POST", "/api/v1/mode", {"cookie": headers["cookie"]}, b'{"mode":"manual"}'
                )
            _status, result, _extra = await self.web.dispatch_api(
                "POST",
                "/api/v1/motion",
                headers,
                b'{"direction":"forward","speed_percent":50,"duration_ms":500}',
            )
            self.assertTrue(result["ok"])
            call = self.device.calls[-1]
            self.assertEqual(call[0], "move")
            self.assertEqual(call[1], call[2])
            self.assertLessEqual(call[3], self.config.config["motion"]["soft_rate_sps"])

        asyncio.run(scenario())

    def test_static_path_rejects_traversal(self):
        with self.assertRaises(ApiError):
            self.web._static_path("/../config/secrets.json")

    def test_invalid_public_config_is_a_client_error(self):
        headers = self.login_headers()

        async def scenario():
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "PUT", "/api/v1/config", headers,
                    b'{"motion":{"soft_rate_sps":400,"accel_sps2":600,"hold_ms":9000}}',
                )
            self.assertEqual(context.exception.status, 400)

        asyncio.run(scenario())

    def test_websocket_text_and_pong_frames(self):
        async def scenario():
            text_writer = FakeWriter()
            await self.web._websocket_send(text_writer, {"ok": True})
            self.assertEqual(text_writer.data[0], 0x81)
            self.assertIn(b'"ok": true', text_writer.data)

            pong_writer = FakeWriter()
            await self.web._websocket_send(pong_writer, b"ping", opcode=0x0A)
            self.assertEqual(bytes(pong_writer.data), b"\x8a\x04ping")

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
