import io
import json
import tempfile
import unittest
from pathlib import Path

from tools import resource_format
from tools.upload_resources import (
    HttpResponse,
    HttpStatusError,
    ResourceApiClient,
    ResourceUploader,
    ResponseError,
    TransportError,
    UploadError,
    UploadRestartRequired,
    UploadValidationError,
    _build_parser,
    main,
)


def build_test_package():
    frame = bytes(range(256)) * 4
    return resource_format.build_package(
        (
            resource_format.ClipSource(
                expression_id=resource_format.EXPRESSION_IDS["HAPPY"],
                weight=1,
                frame_interval_ms=100,
                frames=(frame, frame, frame, frame, frame),
            ),
        )
    )


class FakeResourceTransport:
    def __init__(self, password="admin-secret"):
        self.password = password
        self.calls = []
        self.cookie = "robot_session=session-secret"
        self.csrf = "csrf-secret"
        self.update_id = 7
        self.state = 2
        self.next_offset = 0
        self.total_size = 0
        self.generation = 0
        self.error = 0
        self.accepted = bytearray()
        self.fail_first_chunk_after = None
        self.failed_chunk = False
        self.drop_begin_response = False

    @staticmethod
    def _json(payload, status=200, headers=None):
        return HttpResponse(
            status=status,
            headers=headers or {},
            body=json.dumps(payload).encode("utf-8"),
        )

    def _assert_authenticated(self, headers, mutating):
        if headers.get("Cookie") != self.cookie:
            raise AssertionError("missing session cookie")
        if mutating and headers.get("X-CSRF-Token") != self.csrf:
            raise AssertionError("missing CSRF token")

    def _status_payload(self):
        return {
            "update_id": self.update_id if self.total_size else 0,
            "state": self.state,
            "active_bank": 0xFF,
            "generation": self.generation,
            "next_offset": self.next_offset,
            "total_size": self.total_size,
            "error": self.error,
        }

    def request(self, method, path, headers=None, body=b""):
        headers = dict(headers or {})
        body = bytes(body or b"")
        self.calls.append((method, path, headers, body))

        if method == "POST" and path == "/api/v1/session/login":
            request = json.loads(body.decode("utf-8"))
            if request != {"password": self.password}:
                raise AssertionError("unexpected login body")
            return self._json(
                {"ok": True, "csrf": self.csrf},
                headers={
                    "Set-Cookie": self.cookie
                    + "; Path=/; HttpOnly; SameSite=Strict"
                },
            )

        if method == "GET" and path == "/api/v1/resources/status":
            self._assert_authenticated(headers, False)
            if self.state == 3:
                self.state = 4
            elif self.state == 6:
                self.state = 7
            elif self.state == 7:
                self.state = 2
                self.generation = (self.generation + 1) & 0xFFFFFFFF
            return self._json(self._status_payload())

        if method == "POST" and path == "/api/v1/resources/updates":
            self._assert_authenticated(headers, True)
            request = json.loads(body.decode("utf-8"))
            if request["format_version"] != resource_format.FORMAT_VERSION:
                raise AssertionError("wrong format version")
            self.total_size = request["package_size"]
            self.next_offset = 0
            self.accepted = bytearray()
            self.state = 3
            response = self._json(
                {
                    "active": True,
                    "update_id": self.update_id,
                    "next_offset": 0,
                    "total_size": self.total_size,
                    "needs_sync": False,
                },
                status=201,
            )
            if self.drop_begin_response:
                self.drop_begin_response = False
                raise TransportError("simulated lost begin response")
            return response

        prefix = "/api/v1/resources/updates/{}/chunks/".format(self.update_id)
        if method == "PUT" and path.startswith(prefix):
            self._assert_authenticated(headers, True)
            if headers.get("Content-Type") != "application/octet-stream":
                raise AssertionError("wrong chunk content type")
            offset = int(path[len(prefix) :])
            if offset != self.next_offset:
                raise AssertionError("unexpected chunk offset")
            if self.fail_first_chunk_after is not None and not self.failed_chunk:
                accepted = min(self.fail_first_chunk_after, len(body))
                self.accepted.extend(body[:accepted])
                self.next_offset += accepted
                self.state = 5
                self.failed_chunk = True
                raise TransportError("simulated disconnect")
            self.accepted.extend(body)
            self.next_offset += len(body)
            self.state = 5
            return self._json(
                {
                    "active": True,
                    "update_id": self.update_id,
                    "next_offset": self.next_offset,
                    "total_size": self.total_size,
                    "needs_sync": False,
                }
            )

        finish_path = "/api/v1/resources/updates/{}/finish".format(self.update_id)
        if method == "POST" and path == finish_path:
            self._assert_authenticated(headers, True)
            if headers.get("Content-Type") != "application/json":
                raise AssertionError("wrong finish content type")
            if json.loads(body.decode("utf-8")) != {}:
                raise AssertionError("wrong finish body")
            if self.next_offset != self.total_size:
                raise AssertionError("finish before upload completed")
            self.state = 6
            return self._json({"update_id": self.update_id, "accepted": True})

        update_path = "/api/v1/resources/updates/{}".format(self.update_id)
        if method == "DELETE" and path == update_path:
            self._assert_authenticated(headers, True)
            if headers.get("Content-Type") != "application/json":
                raise AssertionError("wrong abort content type")
            if json.loads(body.decode("utf-8")) != {}:
                raise AssertionError("wrong abort body")
            self.state = 8
            return self._json({"update_id": self.update_id, "aborted": True})

        raise AssertionError("unexpected request: {} {}".format(method, path))


class ErrorTransport(FakeResourceTransport):
    def __init__(self, error_body):
        super().__init__()
        self.error_body = error_body

    def request(self, method, path, headers=None, body=b""):
        if method == "POST" and path == "/api/v1/resources/updates":
            self.calls.append((method, path, dict(headers or {}), bytes(body or b"")))
            return HttpResponse(409, {"Content-Type": "application/json"}, self.error_body)
        return super().request(method, path, headers, body)


class UploadResourcesTests(unittest.TestCase):
    def test_upload_uses_authenticated_4096_byte_chunks_and_reports_stages(self):
        package = build_test_package()
        transport = FakeResourceTransport()
        events = []
        uploader = ResourceUploader(
            ResourceApiClient(transport),
            progress=lambda stage, current, total: events.append(
                (stage, current, total)
            ),
            sleep=lambda _seconds: None,
        )

        result = uploader.upload(package, "admin-secret")

        self.assertEqual(bytes(transport.accepted), package)
        put_calls = [call for call in transport.calls if call[0] == "PUT"]
        self.assertEqual([len(call[3]) for call in put_calls], [4096, len(package) - 4096])
        self.assertEqual(
            [call[1] for call in put_calls],
            [
                "/api/v1/resources/updates/7/chunks/0",
                "/api/v1/resources/updates/7/chunks/4096",
            ],
        )
        self.assertEqual(result["state"], ResourceUploader.STATE_IDLE)
        stages = [event[0] for event in events]
        for stage in ("validated", "erasing", "uploading", "verifying", "activating", "complete"):
            self.assertIn(stage, stages)
        rendered = repr(events) + repr(result)
        self.assertNotIn("admin-secret", rendered)
        self.assertNotIn("session-secret", rendered)
        self.assertNotIn("csrf-secret", rendered)

    def test_partial_http_failure_resumes_from_device_next_offset(self):
        package = build_test_package()
        transport = FakeResourceTransport()
        transport.fail_first_chunk_after = 238
        uploader = ResourceUploader(
            ResourceApiClient(transport),
            sleep=lambda _seconds: None,
        )

        uploader.upload(package, "admin-secret")

        self.assertEqual(bytes(transport.accepted), package)
        put_paths = [call[1] for call in transport.calls if call[0] == "PUT"]
        self.assertEqual(put_paths[0], "/api/v1/resources/updates/7/chunks/0")
        self.assertEqual(put_paths[1], "/api/v1/resources/updates/7/chunks/238")
        status_calls = [
            call
            for call in transport.calls
            if call[:2] == ("GET", "/api/v1/resources/status")
        ]
        self.assertGreaterEqual(len(status_calls), 2)

    def test_lost_begin_response_adopts_the_started_session_without_retry(self):
        package = build_test_package()
        transport = FakeResourceTransport()
        transport.drop_begin_response = True
        uploader = ResourceUploader(
            ResourceApiClient(transport),
            sleep=lambda _seconds: None,
        )

        result = uploader.upload(package, "admin-secret")

        begin_calls = [
            call
            for call in transport.calls
            if call[:2] == ("POST", "/api/v1/resources/updates")
        ]
        self.assertEqual(len(begin_calls), 1)
        self.assertEqual(bytes(transport.accepted), package)
        self.assertEqual(result["state"], ResourceUploader.STATE_IDLE)

    def test_chunk_recovery_tolerates_consecutive_status_disconnects(self):
        class ConsecutiveFailureTransport(FakeResourceTransport):
            def __init__(self):
                super().__init__()
                self.status_failures_remaining = 2

            def request(self, method, path, headers=None, body=b""):
                if (
                    method == "GET"
                    and path == "/api/v1/resources/status"
                    and self.failed_chunk
                    and self.status_failures_remaining > 0
                ):
                    self.status_failures_remaining -= 1
                    self.calls.append((method, path, dict(headers or {}), b""))
                    raise TransportError("simulated status disconnect")
                return super().request(method, path, headers, body)

        package = build_test_package()
        transport = ConsecutiveFailureTransport()
        transport.fail_first_chunk_after = 238
        uploader = ResourceUploader(
            ResourceApiClient(transport),
            sleep=lambda _seconds: None,
        )

        uploader.upload(package, "admin-secret")
        self.assertEqual(bytes(transport.accepted), package)
        self.assertEqual(transport.status_failures_remaining, 0)

    def test_poll_deadline_includes_slow_failed_requests(self):
        class FakeClock:
            def __init__(self):
                self.value = 0.0

            def __call__(self):
                return self.value

        class SlowStatusTransport(FakeResourceTransport):
            def __init__(self, clock):
                super().__init__()
                self.clock = clock
                self.status_calls = 0

            def request(self, method, path, headers=None, body=b""):
                if method == "GET" and path == "/api/v1/resources/status":
                    self.status_calls += 1
                    self.clock.value += 100.0
                    raise TransportError("simulated slow timeout")
                return super().request(method, path, headers, body)

        clock = FakeClock()
        transport = SlowStatusTransport(clock)
        uploader = ResourceUploader(
            ResourceApiClient(transport),
            sleep=lambda _seconds: None,
            max_recovery_attempts=10,
            clock=clock,
            poll_timeout_seconds=180,
        )

        with self.assertRaises(UploadRestartRequired):
            uploader.upload(build_test_package(), "admin-secret")
        self.assertEqual(transport.status_calls, 2)

    def test_resume_rejects_next_offset_outside_failed_chunk(self):
        class JumpingOffsetTransport(FakeResourceTransport):
            def request(self, method, path, headers=None, body=b""):
                response = super().request(method, path, headers, body)
                if (
                    method == "GET"
                    and path == "/api/v1/resources/status"
                    and self.failed_chunk
                    and self.state == 5
                ):
                    payload = json.loads(response.body.decode("utf-8"))
                    payload["next_offset"] = 4097
                    return self._json(payload)
                return response

        transport = JumpingOffsetTransport()
        transport.fail_first_chunk_after = 238
        uploader = ResourceUploader(
            ResourceApiClient(transport),
            sleep=lambda _seconds: None,
        )

        with self.assertRaises(UploadRestartRequired):
            uploader.upload(build_test_package(), "admin-secret")

    def test_status_and_abort_use_cookie_and_csrf(self):
        transport = FakeResourceTransport()
        transport.total_size = 100
        transport.next_offset = 20
        transport.state = 5
        client = ResourceApiClient(transport)
        client.login("admin-secret")

        status = client.status()
        self.assertEqual(status["next_offset"], 20)
        result = client.abort(transport.update_id)
        self.assertTrue(result["aborted"])
        delete = [call for call in transport.calls if call[0] == "DELETE"][0]
        self.assertEqual(delete[2]["Cookie"], transport.cookie)
        self.assertEqual(delete[2]["X-CSRF-Token"], transport.csrf)

    def test_status_rejects_offset_beyond_total_size(self):
        transport = FakeResourceTransport()
        transport.total_size = 100
        transport.next_offset = 101
        transport.state = 5
        client = ResourceApiClient(transport)
        client.login("admin-secret")

        with self.assertRaises(ResponseError):
            client.status()

        transport.next_offset = 0
        transport.state = 10
        with self.assertRaises(ResponseError):
            client.status()

        transport.state = 2
        original_payload = transport._status_payload
        transport._status_payload = lambda: dict(
            original_payload(), active_bank=2
        )
        with self.assertRaises(ResponseError):
            client.status()

    def test_idle_without_generation_change_is_not_reported_as_success(self):
        class StaleGenerationTransport(FakeResourceTransport):
            def request(self, method, path, headers=None, body=b""):
                previous_state = self.state
                previous_generation = self.generation
                response = super().request(method, path, headers, body)
                if (
                    method == "GET"
                    and path == "/api/v1/resources/status"
                    and previous_state == 7
                ):
                    self.generation = previous_generation
                    payload = json.loads(response.body.decode("utf-8"))
                    payload["generation"] = previous_generation
                    return self._json(payload)
                return response

        uploader = ResourceUploader(
            ResourceApiClient(StaleGenerationTransport()),
            sleep=lambda _seconds: None,
        )
        with self.assertRaisesRegex(UploadError, "generation"):
            uploader.upload(build_test_package(), "admin-secret")

        self.assertTrue(ResourceUploader._generation_is_newer(0, 0xFFFFFFFF))

    def test_invalid_package_is_rejected_before_any_http_request(self):
        package = bytearray(build_test_package())
        package[-1] ^= 1
        transport = FakeResourceTransport()
        uploader = ResourceUploader(ResourceApiClient(transport))

        with self.assertRaises(UploadValidationError):
            uploader.upload(package, "admin-secret")
        self.assertEqual(transport.calls, [])

    def test_http_error_uses_stable_code_without_echoing_server_body(self):
        secret = b'{"error":{"code":"busy","message":"raw-secret-frame-data"}}'
        transport = ErrorTransport(secret)
        uploader = ResourceUploader(ResourceApiClient(transport))

        with self.assertRaises(HttpStatusError) as context:
            uploader.upload(build_test_package(), "admin-secret")
        rendered = str(context.exception)
        self.assertIn("busy", rendered)
        self.assertNotIn("raw-secret-frame-data", rendered)

    def test_rejects_oversized_or_malformed_json_responses(self):
        class InvalidLoginTransport:
            def __init__(self, body):
                self.body = body

            def request(self, _method, _path, headers=None, body=b""):
                return HttpResponse(200, {"Set-Cookie": "robot_session=x"}, self.body)

        for body in (b"{" + b"x" * 20000, b"[]", b'{"csrf":1}'):
            with self.subTest(length=len(body)):
                client = ResourceApiClient(InvalidLoginTransport(body))
                with self.assertRaises(ResponseError):
                    client.login("admin-secret")

        client = ResourceApiClient(
            InvalidLoginTransport(b'{"ok":true,"csrf":"token"}')
        )
        client.transport.request = lambda *_args, **_kwargs: HttpResponse(
            200, {"Set-Cookie": "wrong_cookie=x"}, b'{"ok":true,"csrf":"token"}'
        )
        with self.assertRaises(ResponseError):
            client.login("admin-secret")

    def test_cli_prompts_once_and_has_no_password_argument(self):
        package = build_test_package()
        transport = FakeResourceTransport()
        prompts = []
        stdout = io.StringIO()
        stderr = io.StringIO()

        with tempfile.TemporaryDirectory() as temporary:
            package_path = Path(temporary) / "faces.arp"
            package_path.write_bytes(package)
            result = main(
                ["upload", "http://robot.local", str(package_path)],
                transport_factory=lambda _url, timeout=10: transport,
                password_reader=lambda prompt: prompts.append(prompt) or "admin-secret",
                stdout=stdout,
                stderr=stderr,
                sleep=lambda _seconds: None,
            )

        self.assertEqual(result, 0, stderr.getvalue())
        self.assertEqual(len(prompts), 1)
        rendered = stdout.getvalue() + stderr.getvalue()
        self.assertNotIn("admin-secret", rendered)
        self.assertNotIn("session-secret", rendered)
        self.assertNotIn("csrf-secret", rendered)
        help_text = _build_parser().format_help()
        self.assertNotIn("--password", help_text)


if __name__ == "__main__":
    unittest.main()
