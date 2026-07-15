import asyncio
import json
import tempfile
import unittest

from firmware.esp32.services.config_service import ConfigService
from firmware.esp32.services.resource_service import ResourceServiceError
from firmware.esp32.services.web_service import (
    HTTP_ADMISSION_TIMEOUT_MS,
    MAX_HTTP_CLIENTS,
    MAX_HTTP_WAITERS,
    SESSION_ABSOLUTE_TTL_MS,
    SESSION_IDLE_TTL_MS,
    ApiError,
    WebService,
)
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

    async def clear_estop(self):
        self.calls.append(("clear_estop",))
        return 13


class FakeLlm:
    def __init__(self):
        self.clear_count = 0

    async def chat(self, message):
        return {"text": "answer: " + message, "provider": "deepseek"}

    def clear(self):
        self.clear_count += 1


class FakeNetwork:
    def status(self):
        return {"mode": "station", "ip": "192.0.2.5", "setup_ssid": ""}


class FakeResource:
    def __init__(self):
        self.calls = []
        self.next_error = None

    def _fail_if_requested(self):
        if self.next_error is not None:
            error = self.next_error
            self.next_error = None
            raise error

    async def status(self, update_id=0):
        self._fail_if_requested()
        self.calls.append(("status", update_id))
        return {
            "update_id": 7,
            "state": protocol_ids.RESOURCESTATE_RECEIVING,
            "active_bank": 0xFF,
            "generation": 0,
            "next_offset": 10,
            "total_size": 100,
            "error": protocol_ids.RESOURCEERROR_NONE,
        }

    async def begin(self, package_size, package_crc32, format_version=1):
        self._fail_if_requested()
        self.calls.append(("begin", package_size, package_crc32, format_version))
        return {
            "active": True,
            "update_id": 7,
            "next_offset": 0,
            "total_size": package_size,
            "needs_sync": False,
            "finishing": False,
        }

    async def write_chunk(self, update_id, offset, data):
        self._fail_if_requested()
        self.calls.append(("chunk", update_id, offset, bytes(data)))
        return {
            "active": True,
            "update_id": update_id,
            "next_offset": offset + len(data),
            "total_size": 100,
            "needs_sync": False,
            "finishing": False,
        }

    async def finish(self, update_id):
        self._fail_if_requested()
        self.calls.append(("finish", update_id))
        return {"update_id": update_id, "accepted": True}

    async def abort(self, update_id):
        self._fail_if_requested()
        self.calls.append(("abort", update_id))
        return {"update_id": update_id, "aborted": True}


class FakeWriter:
    def __init__(self):
        self.data = bytearray()
        self.closed = False

    def write(self, value):
        self.data.extend(value)

    async def drain(self):
        pass

    def close(self):
        self.closed = True


class SlowWriter(FakeWriter):
    async def drain(self):
        await asyncio.Event().wait()


class FakeReader:
    def __init__(self, data):
        self.data = bytes(data)
        self.offset = 0

    async def readexactly(self, length):
        end = self.offset + length
        if end > len(self.data):
            raise EOFError()
        result = self.data[self.offset:end]
        self.offset = end
        return result


class BlockingReader:
    async def readexactly(self, _length):
        await asyncio.Event().wait()


class FakeClock:
    def __init__(self, value=0):
        self.value = value

    def __call__(self):
        return self.value

    def advance(self, milliseconds):
        self.value = (self.value + milliseconds) & 0xFFFFFFFF


class WebServiceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.config = ConfigService(self.temporary.name)
        self.config.load()
        self.config.set_admin_password("test-password", iterations=2)
        self.config.update_public({"wifi": {"ssid": "lab", "password": "wifi-secret"}})
        self.device = FakeDevice()
        self.resource = FakeResource()
        self.web = WebService(
            self.config,
            self.device,
            FakeLlm(),
            FakeNetwork(),
            static_root="web",
            resource=self.resource,
        )

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def json_headers(headers=None):
        result = dict(headers or {})
        result["content-type"] = "application/json"
        return result

    def login_headers(self):
        async def login():
            status, payload, extra = await self.web.dispatch_api(
                "POST",
                "/api/v1/session/login",
                self.json_headers(),
                json.dumps({"password": "test-password"}).encode(),
            )
            self.assertEqual(status, 200)
            cookie = extra["Set-Cookie"].split(";", 1)[0]
            return self.json_headers(
                {"cookie": cookie, "x-csrf-token": payload["csrf"]}
            )

        return asyncio.run(login())

    def test_bootstrap_is_public_but_status_requires_login(self):
        async def scenario():
            status, payload, _extra = await self.web.dispatch_api("GET", "/api/v1/bootstrap", {}, b"")
            self.assertEqual(status, 200)
            self.assertFalse(payload["setup_required"])
            self.assertNotIn("setup_nonce", payload)
            with self.assertRaisesRegex(ApiError, "authentication"):
                await self.web.dispatch_api("GET", "/api/v1/status", {}, b"")

        asyncio.run(scenario())

    def test_first_setup_requires_json_and_one_time_bootstrap_nonce(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = ConfigService(temporary)
            config.load()
            delays = []

            async def record_delay(milliseconds):
                delays.append(milliseconds)

            web = WebService(
                config,
                self.device,
                FakeLlm(),
                FakeNetwork(),
                static_root="web",
                sleep=record_delay,
            )

            async def scenario():
                _status, bootstrap, _extra = await web.dispatch_api(
                    "GET", "/api/v1/bootstrap", {}, b""
                )
                nonce = bootstrap.get("setup_nonce")
                self.assertTrue(bootstrap["setup_required"])
                self.assertIsInstance(nonce, str)
                self.assertGreaterEqual(len(nonce), 32)

                body = json.dumps(
                    {"password": "first-password", "setup_nonce": nonce}
                ).encode()
                with self.assertRaises(ApiError) as context:
                    await web.dispatch_api(
                        "POST",
                        "/api/v1/session/login",
                        {"content-type": "text/plain"},
                        body,
                    )
                self.assertEqual(context.exception.status, 415)
                self.assertFalse(config.has_admin_password())

                for request in (
                    {"password": "first-password"},
                    {"password": "first-password", "setup_nonce": "wrong"},
                ):
                    with self.assertRaises(ApiError) as context:
                        await web.dispatch_api(
                            "POST",
                            "/api/v1/session/login",
                            self.json_headers(),
                            json.dumps(request).encode(),
                        )
                    self.assertEqual(context.exception.status, 403)
                    self.assertFalse(config.has_admin_password())

                status, payload, _extra = await web.dispatch_api(
                    "POST",
                    "/api/v1/session/login",
                    {"content-type": "application/json; charset=utf-8"},
                    body,
                )
                self.assertEqual(status, 200)
                self.assertTrue(payload["ok"])
                self.assertTrue(config.has_admin_password())
                self.assertIsNone(web._setup_nonce)

                _status, installed, _extra = await web.dispatch_api(
                    "GET", "/api/v1/bootstrap", {}, b""
                )
                self.assertFalse(installed["setup_required"])
                self.assertNotIn("setup_nonce", installed)

                with self.assertRaises(ApiError) as context:
                    await web.dispatch_api(
                        "POST",
                        "/api/v1/session/login",
                        self.json_headers(),
                        body,
                    )
                self.assertEqual(context.exception.status, 403)
                self.assertEqual(delays, [250, 500, 250])

            asyncio.run(scenario())

    def test_login_failures_back_off_globally_and_success_resets_delay(self):
        delays = []

        async def record_delay(milliseconds):
            delays.append(milliseconds)

        self.web._sleep = record_delay

        async def login(password):
            return await self.web.dispatch_api(
                "POST",
                "/api/v1/session/login",
                self.json_headers(),
                json.dumps({"password": password}).encode(),
            )

        async def scenario():
            for expected in (250, 500, 1000, 2000, 2000):
                with self.assertRaises(ApiError) as context:
                    await login("wrong-password")
                self.assertEqual(context.exception.status, 401)
                self.assertEqual(delays[-1], expected)

            status, _payload, _extra = await login("test-password")
            self.assertEqual(status, 200)
            with self.assertRaises(ApiError):
                await login("wrong-password")
            self.assertEqual(delays[-1], 250)

        asyncio.run(scenario())

    def test_sessions_have_idle_and_absolute_expiry(self):
        clock = FakeClock(1000)
        self.web._clock = clock
        idle_headers = self.login_headers()

        async def scenario():
            idle_token = idle_headers["cookie"].split("=", 1)[1]
            idle_session = self.web.sessions[idle_token]
            self.assertEqual(idle_session["created_ms"], 1000)
            self.assertEqual(idle_session["last_used_ms"], 1000)

            clock.advance(SESSION_IDLE_TTL_MS)
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "GET", "/api/v1/status", idle_headers, b""
                )
            self.assertEqual(context.exception.status, 401)
            self.assertNotIn(idle_token, self.web.sessions)

            _status, payload, extra = await self.web.dispatch_api(
                "POST",
                "/api/v1/session/login",
                self.json_headers(),
                json.dumps({"password": "test-password"}).encode(),
            )
            absolute_headers = self.json_headers(
                {
                    "cookie": extra["Set-Cookie"].split(";", 1)[0],
                    "x-csrf-token": payload["csrf"],
                }
            )
            absolute_token = absolute_headers["cookie"].split("=", 1)[1]
            elapsed = 0
            active_step = SESSION_IDLE_TTL_MS - 1
            while elapsed + active_step < SESSION_ABSOLUTE_TTL_MS:
                clock.advance(active_step)
                elapsed += active_step
                await self.web.dispatch_api(
                    "GET", "/api/v1/status", absolute_headers, b""
                )
            clock.advance(SESSION_ABSOLUTE_TTL_MS - elapsed)
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "GET", "/api/v1/status", absolute_headers, b""
                )
            self.assertEqual(context.exception.status, 401)
            self.assertNotIn(absolute_token, self.web.sessions)

        asyncio.run(scenario())

    def test_login_returns_redacted_initial_console(self):
        self.config.set_provider_key("deepseek", "provider-secret")

        async def scenario():
            status, payload, extra = await self.web.dispatch_api(
                "POST",
                "/api/v1/session/login",
                self.json_headers(),
                json.dumps({"password": "test-password"}).encode(),
            )
            self.assertEqual(status, 200)
            self.assertIn("csrf", payload)
            self.assertEqual(payload["config"], self.config.public_view())
            self.assertEqual(payload["status"]["state_name"], "idle")
            self.assertEqual(payload["status"]["network"]["mode"], "station")

            rendered = json.dumps(payload)
            session_token = extra["Set-Cookie"].split("=", 1)[1].split(";", 1)[0]
            self.assertNotIn("wifi-secret", rendered)
            self.assertNotIn("provider-secret", rendered)
            self.assertNotIn(session_token, rendered)

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
                    "POST",
                    "/api/v1/mode",
                    self.json_headers({"cookie": headers["cookie"]}),
                    b'{"mode":"manual"}',
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

    def test_clear_chat_requires_csrf_and_clears_once(self):
        headers = self.login_headers()

        async def scenario():
            with self.assertRaisesRegex(ApiError, "CSRF"):
                await self.web.dispatch_api(
                    "DELETE",
                    "/api/v1/chat",
                    self.json_headers({"cookie": headers["cookie"]}),
                    b"",
                )
            status, payload, _extra = await self.web.dispatch_api(
                "DELETE", "/api/v1/chat", headers, b""
            )
            self.assertEqual(status, 200)
            self.assertTrue(payload["ok"])
            self.assertEqual(self.web.llm.clear_count, 1)

        asyncio.run(scenario())

    def test_logout_revokes_the_server_side_session(self):
        headers = self.login_headers()

        async def scenario():
            status, payload, _extra = await self.web.dispatch_api(
                "POST", "/api/v1/session/logout", headers, b""
            )
            self.assertEqual(status, 200)
            self.assertTrue(payload["ok"])
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "GET", "/api/v1/status", headers, b""
                )
            self.assertEqual(context.exception.status, 401)

        asyncio.run(scenario())

    def test_clear_estop_requires_csrf_and_dispatches_once(self):
        headers = self.login_headers()

        async def scenario():
            with self.assertRaisesRegex(ApiError, "CSRF"):
                await self.web.dispatch_api(
                    "POST",
                    "/api/v1/estop/clear",
                    self.json_headers({"cookie": headers["cookie"]}),
                    b"{}",
                )
            status, payload, _extra = await self.web.dispatch_api(
                "POST", "/api/v1/estop/clear", headers, b"{}"
            )
            self.assertEqual(status, 200)
            self.assertTrue(payload["ok"])
            self.assertEqual(payload["command_id"], 13)
            self.assertEqual(self.device.calls.count(("clear_estop",)), 1)

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

    def test_json_bodies_reject_invalid_utf8_and_non_objects(self):
        headers = self.login_headers()

        async def scenario():
            for body in (b"\xff", b"[]", b'"text"'):
                with self.subTest(body=body):
                    with self.assertRaises(ApiError) as context:
                        await self.web.dispatch_api(
                            "POST", "/api/v1/mode", headers, body
                        )
                    self.assertEqual(context.exception.status, 400)

        asyncio.run(scenario())

    def test_all_mutating_json_routes_reject_safelisted_content_types(self):
        headers = self.login_headers()

        async def scenario():
            cases = (
                ("POST", "/api/v1/session/login", b'{"password":"test-password"}'),
                ("POST", "/api/v1/resources/updates", b"{}"),
                ("PUT", "/api/v1/config", b"{}"),
                ("POST", "/api/v1/stop", b"{}"),
                ("DELETE", "/api/v1/chat", b"{}"),
            )
            for method, path, body in cases:
                request_headers = dict(headers)
                request_headers["content-type"] = "text/plain"
                with self.subTest(method=method, path=path):
                    with self.assertRaises(ApiError) as context:
                        await self.web.dispatch_api(
                            method, path, request_headers, body
                        )
                    self.assertEqual(context.exception.status, 415)

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

    def test_websocket_limit_and_slow_broadcast_writer_eviction(self):
        headers = self.login_headers()
        headers["sec-websocket-key"] = "dGhlIHNhbXBsZSBub25jZQ=="
        self.web._websocket_write_timeout_s = 0.01

        async def scenario():
            self.web.websocket_clients.extend((FakeWriter(), FakeWriter()))
            with self.assertRaises(ApiError) as context:
                await self.web._upgrade_websocket(
                    FakeReader(b""), FakeWriter(), headers
                )
            self.assertEqual(context.exception.status, 503)

            self.web.websocket_clients.clear()
            slow = SlowWriter()
            fast = FakeWriter()
            self.web.websocket_clients.extend((slow, fast))
            await self.web._broadcast_event({"type": "test"})
            self.assertNotIn(slow, self.web.websocket_clients)
            self.assertTrue(slow.closed)
            self.assertIn(fast, self.web.websocket_clients)
            self.assertIn(b'"type": "test"', fast.data)

        asyncio.run(scenario())

    def test_event_queue_keeps_its_explicit_capacity(self):
        for index in range(40):
            self.web.publish({"index": index})
        self.assertEqual(len(self.web.events), 32)
        self.assertEqual(self.web.events[0]["index"], 8)

    def test_request_parser_bounds_headers_and_content_length(self):
        async def scenario():
            mobile_request = (
                b"GET /api/v1/bootstrap HTTP/1.1\r\n"
                b"Host: robot\r\n"
                b"X-Mobile-Model: phone-\xe9\r\n\r\n"
            )
            method, path, headers, body = await self.web._read_request(
                FakeReader(mobile_request)
            )
            self.assertEqual((method, path, body), ("GET", "/api/v1/bootstrap", b""))
            self.assertEqual(headers["x-mobile-model"], "phone-\xe9")

            malformed_headers = (
                b"GET / HTTP/1.1\r\nX-\xff: value\r\n\r\n",
                b"GET / HTTP/1.1\r\nX-Test: value\x00hidden\r\n\r\n",
                b"GET / HTTP/1.1\r\nX-Test: one\r\nX-Test: two\r\n\r\n",
            )
            for request in malformed_headers:
                with self.subTest(request=request):
                    with self.assertRaises(ApiError) as context:
                        await self.web._read_request(FakeReader(request))
                    self.assertEqual(context.exception.status, 400)

            oversized = (
                b"GET / HTTP/1.1\r\nX-Fill: "
                + b"a" * 1100
                + b"\r\n\r\n"
            )
            with self.assertRaises(ApiError) as context:
                await self.web._read_request(FakeReader(oversized))
            self.assertEqual(context.exception.status, 431)

            too_many = b"GET / HTTP/1.1\r\n" + b"".join(
                "X-{}: a\r\n".format(index).encode() for index in range(33)
            ) + b"\r\n"
            with self.assertRaises(ApiError) as context:
                await self.web._read_request(FakeReader(too_many))
            self.assertEqual(context.exception.status, 431)

            for value, expected in ((b"-1", 400), (b"no", 400), (b"999999", 413)):
                request = (
                    b"POST /api/v1/session/login HTTP/1.1\r\n"
                    b"Content-Length: " + value + b"\r\n\r\n"
                )
                with self.subTest(value=value):
                    with self.assertRaises(ApiError) as context:
                        await self.web._read_request(FakeReader(request))
                    self.assertEqual(context.exception.status, expected)

        asyncio.run(scenario())

    def test_http_waiter_uses_released_slot_and_whole_request_deadline(self):
        self.web._request_timeout_s = 0.05

        async def scenario():
            blocked_writers = [FakeWriter() for _ in range(MAX_HTTP_CLIENTS)]
            blocked = [
                asyncio.create_task(
                    self.web._handle_client(BlockingReader(), writer)
                )
                for writer in blocked_writers
            ]
            await asyncio.sleep(0)
            self.assertEqual(self.web._active_http_clients, MAX_HTTP_CLIENTS)

            queued_writer = FakeWriter()
            queued = asyncio.create_task(
                self.web._handle_client(
                    FakeReader(b"GET / HTTP/1.1\r\n\r\n"), queued_writer
                )
            )
            await asyncio.sleep(0)
            self.assertEqual(self.web._waiting_http_clients, 1)
            self.assertFalse(queued.done())

            blocked[0].cancel()
            with self.assertRaises(asyncio.CancelledError):
                await blocked[0]
            await queued
            self.assertIn(b"200 OK", queued_writer.data)
            self.assertTrue(queued_writer.closed)

            await asyncio.gather(*blocked[1:])
            self.assertEqual(self.web._active_http_clients, 0)
            self.assertEqual(self.web._waiting_http_clients, 0)
            for writer in blocked_writers[1:]:
                self.assertIn(b"408 Request Timeout", writer.data)
                self.assertTrue(writer.closed)

        asyncio.run(scenario())

    def test_http_admission_timeout_returns_503_and_cleans_wait_count(self):
        self.assertEqual(HTTP_ADMISSION_TIMEOUT_MS, 3000)
        self.web._request_timeout_s = 1
        self.web._http_admission_timeout_ms = 1

        async def scenario():
            blocked_writers = [FakeWriter() for _ in range(MAX_HTTP_CLIENTS)]
            blocked = [
                asyncio.create_task(self.web._handle_client(BlockingReader(), writer))
                for writer in blocked_writers
            ]
            await asyncio.sleep(0)

            overflow = FakeWriter()
            await self.web._handle_client(
                FakeReader(b"GET / HTTP/1.1\r\n\r\n"), overflow
            )
            self.assertIn(b"503 Service Unavailable", overflow.data)
            self.assertTrue(overflow.closed)
            self.assertEqual(self.web._waiting_http_clients, 0)

            for task in blocked:
                task.cancel()
            await asyncio.gather(*blocked, return_exceptions=True)
            self.assertEqual(self.web._active_http_clients, 0)

        asyncio.run(scenario())

    def test_http_wait_queue_rejects_ninth_connection_and_cleans_cancellation(self):
        self.web._request_timeout_s = 1
        self.web._http_admission_timeout_ms = 1000

        async def scenario():
            blocked_writers = [FakeWriter() for _ in range(MAX_HTTP_CLIENTS)]
            blocked = [
                asyncio.create_task(self.web._handle_client(BlockingReader(), writer))
                for writer in blocked_writers
            ]
            await asyncio.sleep(0)

            waiting_writers = [FakeWriter() for _ in range(MAX_HTTP_WAITERS)]
            waiting = [
                asyncio.create_task(self.web._handle_client(BlockingReader(), writer))
                for writer in waiting_writers
            ]
            await asyncio.sleep(0)
            self.assertEqual(self.web._waiting_http_clients, MAX_HTTP_WAITERS)

            rejected = FakeWriter()
            await self.web._handle_client(
                FakeReader(b"GET / HTTP/1.1\r\n\r\n"), rejected
            )
            self.assertIn(b"503 Service Unavailable", rejected.data)
            self.assertTrue(rejected.closed)

            for task in blocked + waiting:
                task.cancel()
            await asyncio.gather(*(blocked + waiting), return_exceptions=True)
            self.assertEqual(self.web._active_http_clients, 0)
            self.assertEqual(self.web._waiting_http_clients, 0)
            self.assertTrue(all(writer.closed for writer in blocked_writers + waiting_writers))

        asyncio.run(scenario())

    def test_unexpected_server_error_does_not_echo_exception_text(self):
        async def scenario():
            writer = FakeWriter()

            async def fail_dispatch(*_args):
                raise RuntimeError("private-response-body")

            original = self.web.dispatch_api
            self.web.dispatch_api = fail_dispatch
            try:
                await self.web._handle_client(
                    FakeReader(b"GET /api/v1/status HTTP/1.1\r\n\r\n"),
                    writer,
                )
            finally:
                self.web.dispatch_api = original
            rendered = bytes(writer.data)
            self.assertIn(b"internal server error", rendered)
            self.assertNotIn(b"private-response-body", rendered)

        asyncio.run(scenario())

    def test_login_migrates_iterations_and_rejects_concurrent_work(self):
        self.config.set_admin_password("test-password", iterations=64)

        async def scenario():
            body = json.dumps({"password": "test-password"}).encode()
            first = asyncio.create_task(
                self.web.dispatch_api(
                    "POST", "/api/v1/session/login", self.json_headers(), body
                )
            )
            await asyncio.sleep(0)
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "POST", "/api/v1/session/login", self.json_headers(), body
                )
            self.assertEqual(context.exception.status, 409)
            status, _payload, _extra = await first
            self.assertEqual(status, 200)
            self.assertEqual(self.config.admin_password_iterations(), 2000)

        asyncio.run(scenario())

    def test_resource_status_requires_auth_but_not_csrf(self):
        headers = self.login_headers()

        async def scenario():
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "GET", "/api/v1/resources/status", {}, b""
                )
            self.assertEqual(context.exception.status, 401)

            status, payload, _extra = await self.web.dispatch_api(
                "GET",
                "/api/v1/resources/status",
                {"cookie": headers["cookie"]},
                b"",
            )
            self.assertEqual(status, 200)
            self.assertEqual(payload["update_id"], 7)
            self.assertEqual(self.resource.calls[-1], ("status", 0))

        asyncio.run(scenario())

    def test_resource_begin_requires_csrf_and_returns_created_session(self):
        headers = self.login_headers()
        body = json.dumps(
            {"package_size": 100, "package_crc32": 0x12345678, "format_version": 1}
        ).encode()

        async def scenario():
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "POST",
                    "/api/v1/resources/updates",
                    self.json_headers({"cookie": headers["cookie"]}),
                    body,
                )
            self.assertEqual(context.exception.status, 403)

            status, payload, _extra = await self.web.dispatch_api(
                "POST", "/api/v1/resources/updates", headers, body
            )
            self.assertEqual(status, 201)
            self.assertEqual(payload["update_id"], 7)
            self.assertEqual(
                self.resource.calls[-1], ("begin", 100, 0x12345678, 1)
            )

        asyncio.run(scenario())

    def test_resource_chunk_has_strict_path_content_type_and_size(self):
        headers = self.login_headers()

        async def scenario():
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "PUT",
                    "/api/v1/resources/updates/7/chunks/10",
                    headers,
                    b"abc",
                )
            self.assertEqual(context.exception.status, 415)

            binary_headers = dict(headers)
            binary_headers["content-type"] = "application/octet-stream"
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "PUT",
                    "/api/v1/resources/updates/7/chunks/10",
                    binary_headers,
                    bytes(4097),
                )
            self.assertEqual(context.exception.status, 413)

            status, payload, _extra = await self.web.dispatch_api(
                "PUT",
                "/api/v1/resources/updates/7/chunks/10",
                binary_headers,
                b"abc",
            )
            self.assertEqual(status, 200)
            self.assertEqual(payload["next_offset"], 13)
            self.assertEqual(self.resource.calls[-1], ("chunk", 7, 10, b"abc"))

            invalid_paths = (
                "/api/v1/resources/updates/no/chunks/10",
                "/api/v1/resources/updates/7/chunks/-1",
                "/api/v1/resources/updates/0/chunks/0",
                "/api/v1/resources/updates/4294967296/chunks/0",
                "/api/v1/resources/updates/99999999999999999999/chunks/0",
            )
            for path in invalid_paths:
                with self.subTest(path=path):
                    with self.assertRaises(ApiError) as context:
                        await self.web.dispatch_api(
                            "PUT", path, binary_headers, b"a"
                        )
                    self.assertEqual(context.exception.status, 400)

            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "PUT",
                    "/api/v1/resources/updates/7/chunks/0/extra",
                    binary_headers,
                    b"a",
                )
            self.assertEqual(context.exception.status, 404)

        asyncio.run(scenario())

    def test_resource_finish_and_abort_require_csrf(self):
        headers = self.login_headers()

        async def scenario():
            with self.assertRaises(ApiError) as context:
                await self.web.dispatch_api(
                    "POST",
                    "/api/v1/resources/updates/7/finish",
                    self.json_headers({"cookie": headers["cookie"]}),
                    b"",
                )
            self.assertEqual(context.exception.status, 403)

            status, payload, _extra = await self.web.dispatch_api(
                "POST", "/api/v1/resources/updates/7/finish", headers, b""
            )
            self.assertEqual(status, 200)
            self.assertTrue(payload["accepted"])
            status, payload, _extra = await self.web.dispatch_api(
                "DELETE", "/api/v1/resources/updates/7", headers, b""
            )
            self.assertEqual(status, 200)
            self.assertTrue(payload["aborted"])
            self.assertIn(("finish", 7), self.resource.calls)
            self.assertIn(("abort", 7), self.resource.calls)

        asyncio.run(scenario())

    def test_resource_errors_have_stable_http_mapping_and_payload(self):
        headers = self.login_headers()

        async def scenario():
            cases = (
                ("invalid_request", 400),
                ("busy", 409),
                ("link_lost", 503),
            )
            for code, expected_status in cases:
                with self.subTest(code=code):
                    self.resource.next_error = ResourceServiceError(
                        code, "stable message"
                    )
                    with self.assertRaises(ApiError) as context:
                        await self.web.dispatch_api(
                            "GET", "/api/v1/resources/status", headers, b""
                        )
                    error = context.exception
                    self.assertEqual(error.status, expected_status)
                    self.assertEqual(error.code, code)
                    self.assertEqual(
                        error.payload(),
                        {"error": {"code": code, "message": "stable message"}},
                    )

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
