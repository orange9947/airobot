#!/usr/bin/env python3
"""Validate and upload expression resources through the authenticated HTTP API."""

import argparse
import getpass
import http.client
import json
import re
import sys
import time
from dataclasses import dataclass
from http.cookies import CookieError, SimpleCookie
from pathlib import Path
from urllib.parse import urlsplit

if __package__:
    from . import resource_format
else:
    import resource_format


MAX_HTTP_RESPONSE_BODY = 16 * 1024
MAX_HTTP_REQUEST_BODY = 8192
HTTP_CHUNK_SIZE = 4096
SAFE_ERROR_CODE = re.compile(r"[A-Za-z0-9_.-]{1,64}\Z")
SAFE_TOKEN = re.compile(r"[A-Za-z0-9._~-]{1,256}\Z")


class UploadError(RuntimeError):
    """Base class for bounded, credential-safe upload failures."""


class UploadValidationError(UploadError):
    pass


class TransportError(UploadError):
    pass


class ResponseError(UploadError):
    pass


class UploadRestartRequired(UploadError):
    pass


class HttpStatusError(UploadError):
    def __init__(self, status, code):
        self.status = status
        self.code = code
        super().__init__("HTTP request failed (status {}, code {})".format(status, code))


@dataclass(frozen=True)
class HttpResponse:
    status: int
    headers: dict
    body: bytes


def _safe_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ResponseError("JSON response contains duplicate fields")
        result[key] = value
    return result


def _header_value(headers, name):
    wanted = name.lower()
    for key, value in headers.items():
        if str(key).lower() == wanted:
            return str(value)
    return ""


def _safe_error_code(payload, status):
    candidate = None
    if isinstance(payload, dict):
        error = payload.get("error")
        if isinstance(error, dict):
            candidate = error.get("code")
        elif isinstance(error, str):
            candidate = error
        if candidate is None:
            candidate = payload.get("code")
    if isinstance(candidate, str) and SAFE_ERROR_CODE.fullmatch(candidate):
        return candidate
    return "http_{}".format(status)


def _bounded_uint(value, label, maximum=0xFFFFFFFF):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ResponseError("{} is invalid".format(label))
    return value


class HttpTransport:
    """One-request-per-connection HTTP transport with bounded response reads."""

    def __init__(self, base_url, timeout=10):
        parsed = urlsplit(base_url)
        if (
            parsed.scheme not in ("http", "https")
            or not parsed.hostname
            or parsed.username is not None
            or parsed.password is not None
            or parsed.query
            or parsed.fragment
            or parsed.path not in ("", "/")
        ):
            raise ValueError("base URL must be an HTTP(S) origin without credentials or a path")
        if (
            isinstance(timeout, bool)
            or not isinstance(timeout, (int, float))
            or not 0 < timeout <= 300
        ):
            raise ValueError("timeout must be in the range 0..300 seconds")
        try:
            port = parsed.port
        except ValueError as error:
            raise ValueError("base URL has an invalid port") from error
        self.scheme = parsed.scheme
        self.host = parsed.hostname
        self.port = port
        self.timeout = timeout

    def request(self, method, path, headers=None, body=b""):
        if method not in ("GET", "POST", "PUT", "DELETE"):
            raise ValueError("unsupported HTTP method")
        if (
            not isinstance(path, str)
            or not path.startswith("/")
            or len(path) > 1024
            or any(ord(character) < 0x20 or character == " " for character in path)
        ):
            raise ValueError("invalid HTTP path")
        if not isinstance(body, (bytes, bytearray, memoryview)):
            raise TypeError("HTTP body must contain bytes")
        body = bytes(body)
        if len(body) > MAX_HTTP_REQUEST_BODY:
            raise ValueError("HTTP request body exceeds 8192 bytes")

        connection_type = (
            http.client.HTTPSConnection
            if self.scheme == "https"
            else http.client.HTTPConnection
        )
        connection = connection_type(self.host, self.port, timeout=self.timeout)
        try:
            connection.request(method, path, body=body, headers=dict(headers or {}))
            response = connection.getresponse()
            response_body = response.read(MAX_HTTP_RESPONSE_BODY + 1)
            response_headers = {}
            for name, value in response.getheaders():
                response_headers[name] = value
            if len(response_body) > MAX_HTTP_RESPONSE_BODY:
                raise ResponseError("HTTP response exceeds 16384 bytes")
            return HttpResponse(response.status, response_headers, response_body)
        except ResponseError:
            raise
        except (OSError, http.client.HTTPException):
            raise TransportError("network request failed") from None
        finally:
            connection.close()


class ResourceApiClient:
    def __init__(self, transport):
        if transport is None or not callable(getattr(transport, "request", None)):
            raise TypeError("transport must provide request()")
        self.transport = transport
        self._cookie = None
        self._csrf = None

    @staticmethod
    def _decode_response(response, expected_statuses):
        if not isinstance(response, HttpResponse):
            raise ResponseError("transport returned an invalid response")
        if (
            isinstance(response.status, bool)
            or not isinstance(response.status, int)
            or not 100 <= response.status <= 599
            or not isinstance(response.headers, dict)
            or not isinstance(response.body, (bytes, bytearray, memoryview))
        ):
            raise ResponseError("transport returned an invalid response")
        response_body = bytes(response.body)
        if len(response_body) > MAX_HTTP_RESPONSE_BODY:
            raise ResponseError("HTTP response exceeds 16384 bytes")
        try:
            payload = json.loads(
                response_body.decode("utf-8"), object_pairs_hook=_safe_json_object
            )
        except ResponseError:
            raise
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
            if response.status not in expected_statuses:
                raise HttpStatusError(response.status, "http_{}".format(response.status)) from None
            raise ResponseError("HTTP response is not valid UTF-8 JSON") from None
        if response.status not in expected_statuses:
            raise HttpStatusError(
                response.status, _safe_error_code(payload, response.status)
            )
        if not isinstance(payload, dict):
            raise ResponseError("JSON response root must be an object")
        return payload

    @staticmethod
    def _json_body(payload):
        body = json.dumps(payload, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
        if len(body) > MAX_HTTP_REQUEST_BODY:
            raise UploadValidationError("JSON request exceeds 8192 bytes")
        return body

    def _auth_headers(self, mutating=False):
        if self._cookie is None or self._csrf is None:
            raise ResponseError("client is not authenticated")
        headers = {"Accept": "application/json", "Cookie": self._cookie}
        if mutating:
            headers["X-CSRF-Token"] = self._csrf
        return headers

    def login(self, password):
        if not isinstance(password, str) or not 1 <= len(password.encode("utf-8")) <= 1024:
            raise UploadValidationError("administrator password is invalid")
        body = self._json_body({"password": password})
        response = self.transport.request(
            "POST",
            "/api/v1/session/login",
            headers={"Accept": "application/json", "Content-Type": "application/json"},
            body=body,
        )
        payload = self._decode_response(response, (200,))
        csrf = payload.get("csrf")
        if (
            payload.get("ok") is not True
            or not isinstance(csrf, str)
            or not SAFE_TOKEN.fullmatch(csrf)
        ):
            raise ResponseError("login response has an invalid CSRF token")

        cookie_header = _header_value(response.headers, "Set-Cookie")
        cookie = SimpleCookie()
        try:
            cookie.load(cookie_header)
            token = cookie["robot_session"].value
        except (CookieError, KeyError):
            raise ResponseError("login response has no valid session cookie") from None
        if not SAFE_TOKEN.fullmatch(token):
            raise ResponseError("login response has an invalid session cookie")
        self._cookie = "robot_session={}".format(token)
        self._csrf = csrf
        return {"ok": True}

    @staticmethod
    def _parse_session_status(payload):
        update_id = _bounded_uint(payload.get("update_id"), "update_id")
        next_offset = _bounded_uint(payload.get("next_offset"), "next_offset")
        total_size = _bounded_uint(
            payload.get("total_size"), "total_size", resource_format.MAX_PACKAGE_SIZE
        )
        if next_offset > total_size:
            raise ResponseError("next_offset exceeds total_size")
        active = payload.get("active")
        if active is not None and not isinstance(active, bool):
            raise ResponseError("active is invalid")
        return {
            "active": active,
            "update_id": update_id,
            "next_offset": next_offset,
            "total_size": total_size,
        }

    @staticmethod
    def _parse_device_status(payload):
        result = ResourceApiClient._parse_session_status(payload)
        result.update(
            {
                "state": _bounded_uint(payload.get("state"), "state", 0xFF),
                "active_bank": _bounded_uint(
                    payload.get("active_bank"), "active_bank", 0xFF
                ),
                "generation": _bounded_uint(payload.get("generation"), "generation"),
                "error": _bounded_uint(payload.get("error"), "error", 0xFFFF),
            }
        )
        if result["state"] > 9 or result["active_bank"] not in (0, 1, 0xFF):
            raise ResponseError("resource device status is invalid")
        return result

    def status(self):
        response = self.transport.request(
            "GET",
            "/api/v1/resources/status",
            headers=self._auth_headers(False),
        )
        return self._parse_device_status(self._decode_response(response, (200,)))

    def begin(self, package_size, package_crc32, format_version=resource_format.FORMAT_VERSION):
        package_size = _bounded_uint(
            package_size, "package_size", resource_format.MAX_PACKAGE_SIZE
        )
        package_crc32 = _bounded_uint(package_crc32, "package_crc32")
        if (
            package_size == 0
            or isinstance(format_version, bool)
            or not isinstance(format_version, int)
            or format_version != resource_format.FORMAT_VERSION
        ):
            raise UploadValidationError("resource package metadata is invalid")
        headers = self._auth_headers(True)
        headers["Content-Type"] = "application/json"
        response = self.transport.request(
            "POST",
            "/api/v1/resources/updates",
            headers=headers,
            body=self._json_body(
                {
                    "format_version": format_version,
                    "package_size": package_size,
                    "package_crc32": package_crc32,
                }
            ),
        )
        result = self._parse_session_status(
            self._decode_response(response, (200, 201))
        )
        if (
            result["active"] is not True
            or result["update_id"] == 0
            or result["next_offset"] != 0
            or result["total_size"] != package_size
        ):
            raise ResponseError("begin response does not match the package")
        return result

    def put_chunk(self, update_id, offset, data):
        update_id = _bounded_uint(update_id, "update_id")
        offset = _bounded_uint(offset, "offset")
        if update_id == 0 or not isinstance(data, (bytes, bytearray, memoryview)):
            raise UploadValidationError("resource chunk metadata is invalid")
        data = bytes(data)
        if not 1 <= len(data) <= HTTP_CHUNK_SIZE:
            raise UploadValidationError("resource chunk must contain 1..4096 bytes")
        headers = self._auth_headers(True)
        headers["Content-Type"] = "application/octet-stream"
        response = self.transport.request(
            "PUT",
            "/api/v1/resources/updates/{}/chunks/{}".format(update_id, offset),
            headers=headers,
            body=data,
        )
        result = self._parse_session_status(self._decode_response(response, (200,)))
        if (
            result["active"] is not True
            or result["update_id"] != update_id
            or result["next_offset"] != offset + len(data)
        ):
            raise ResponseError("chunk response has an unexpected offset")
        return result

    def finish(self, update_id):
        update_id = _bounded_uint(update_id, "update_id")
        if update_id == 0:
            raise UploadValidationError("update_id is invalid")
        headers = self._auth_headers(True)
        headers["Content-Type"] = "application/json"
        response = self.transport.request(
            "POST",
            "/api/v1/resources/updates/{}/finish".format(update_id),
            headers=headers,
            body=self._json_body({}),
        )
        payload = self._decode_response(response, (200,))
        if payload.get("accepted") is not True or payload.get("update_id") != update_id:
            raise ResponseError("finish response is invalid")
        return {"update_id": update_id, "accepted": True}

    def abort(self, update_id):
        update_id = _bounded_uint(update_id, "update_id")
        if update_id == 0:
            raise UploadValidationError("update_id is invalid")
        headers = self._auth_headers(True)
        headers["Content-Type"] = "application/json"
        response = self.transport.request(
            "DELETE",
            "/api/v1/resources/updates/{}".format(update_id),
            headers=headers,
            body=self._json_body({}),
        )
        payload = self._decode_response(response, (200,))
        if payload.get("aborted") is not True or payload.get("update_id") != update_id:
            raise ResponseError("abort response is invalid")
        return {"update_id": update_id, "aborted": True}


class ResourceUploader:
    STATE_IDLE = 2
    STATE_ERASING = 3
    STATE_READY = 4
    STATE_RECEIVING = 5
    STATE_VERIFYING = 6
    STATE_COMMITTING = 7
    STATE_ABORTED = 8
    STATE_FAILED = 9

    def __init__(self, client, progress=None, sleep=time.sleep,
                 max_poll_attempts=480, max_recovery_attempts=3,
                 clock=time.monotonic, poll_timeout_seconds=180):
        if not isinstance(client, ResourceApiClient):
            raise TypeError("client must be a ResourceApiClient")
        if progress is not None and not callable(progress):
            raise TypeError("progress must be callable")
        if not callable(sleep):
            raise TypeError("sleep must be callable")
        if not callable(clock):
            raise TypeError("clock must be callable")
        if (
            isinstance(max_poll_attempts, bool)
            or not isinstance(max_poll_attempts, int)
            or max_poll_attempts <= 0
            or isinstance(max_recovery_attempts, bool)
            or not isinstance(max_recovery_attempts, int)
            or max_recovery_attempts < 0
            or isinstance(poll_timeout_seconds, bool)
            or not isinstance(poll_timeout_seconds, (int, float))
            or not 1 <= poll_timeout_seconds <= 600
        ):
            raise ValueError("upload retry limits are invalid")
        self.client = client
        self.progress = progress or (lambda _stage, _current, _total: None)
        self.sleep = sleep
        self.max_poll_attempts = max_poll_attempts
        self.max_recovery_attempts = max_recovery_attempts
        self.clock = clock
        self.poll_timeout_seconds = poll_timeout_seconds

    def _emit(self, stage, current, total):
        self.progress(stage, current, total)

    def _status_with_recovery(self, deadline, failure_message):
        for attempt in range(self.max_recovery_attempts + 1):
            try:
                return self.client.status()
            except (TransportError, HttpStatusError, ResponseError):
                if (
                    attempt >= self.max_recovery_attempts
                    or self.clock() >= deadline
                ):
                    raise UploadRestartRequired(failure_message) from None
                self.sleep(min(0.25, max(0.0, deadline - self.clock())))
        raise UploadRestartRequired(failure_message)

    @staticmethod
    def _matching_active_status(status, update_id, total_size):
        if status["update_id"] != update_id or status["total_size"] != total_size:
            raise UploadRestartRequired("resource update session was lost; restart upload")
        if status["error"] != 0 or status["state"] in (
            ResourceUploader.STATE_ABORTED,
            ResourceUploader.STATE_FAILED,
        ):
            raise UploadError(
                "resource update failed with device error {}".format(status["error"])
            )
        return status

    @staticmethod
    def _generation_is_newer(generation, baseline):
        difference = (generation - baseline) & 0xFFFFFFFF
        return 0 < difference < 0x80000000

    def _preflight_idle(self):
        deadline = self.clock() + self.poll_timeout_seconds
        for attempt in range(self.max_poll_attempts):
            status = self._status_with_recovery(
                deadline,
                "resource preflight status could not be recovered",
            )
            if status["state"] == self.STATE_IDLE:
                return status
            if status["state"] in (
                self.STATE_ERASING,
                self.STATE_READY,
                self.STATE_RECEIVING,
                self.STATE_VERIFYING,
                self.STATE_COMMITTING,
            ):
                raise UploadRestartRequired(
                    "another resource update is active; abort it or wait for completion"
                )
            if status["state"] not in (0, 1, self.STATE_ABORTED, self.STATE_FAILED):
                raise ResponseError("device entered an unexpected preflight state")
            if attempt + 1 < self.max_poll_attempts and self.clock() < deadline:
                self.sleep(min(0.25, max(0.0, deadline - self.clock())))
            if self.clock() >= deadline:
                break
        raise UploadError("timed out waiting for idle resource storage")

    def _recover_begin(self, total_size, baseline_generation):
        deadline = self.clock() + self.poll_timeout_seconds
        status = self._status_with_recovery(
            deadline,
            "resource begin status could not be recovered; restart upload",
        )
        if (
            status["state"]
            not in (self.STATE_ERASING, self.STATE_READY, self.STATE_RECEIVING)
            or status["error"] != 0
            or status["update_id"] == 0
            or status["total_size"] != total_size
            or status["next_offset"] != 0
            or status["generation"] != baseline_generation
        ):
            raise UploadRestartRequired(
                "resource begin could not be confirmed; restart upload"
            )
        return {
            "active": True,
            "update_id": status["update_id"],
            "next_offset": 0,
            "total_size": total_size,
        }

    def _wait_ready(self, update_id, total_size):
        deadline = self.clock() + self.poll_timeout_seconds
        for attempt in range(self.max_poll_attempts):
            status = self._matching_active_status(
                self._status_with_recovery(
                    deadline,
                    "resource erase status could not be recovered; restart upload",
                ),
                update_id,
                total_size,
            )
            if status["state"] in (self.STATE_READY, self.STATE_RECEIVING):
                return status
            if status["state"] == self.STATE_ERASING:
                self._emit("erasing", status["next_offset"], total_size)
            else:
                raise ResponseError("device entered an unexpected pre-upload state")
            if attempt + 1 < self.max_poll_attempts and self.clock() < deadline:
                self.sleep(min(0.25, max(0.0, deadline - self.clock())))
            if self.clock() >= deadline:
                break
        raise UploadError("timed out while erasing the inactive resource bank")

    def _recover_offset(self, update_id, total_size, attempted_offset,
                        attempted_length):
        deadline = self.clock() + self.poll_timeout_seconds
        try:
            status = self._matching_active_status(
                self._status_with_recovery(
                    deadline,
                    "resource upload status could not be recovered; restart upload",
                ),
                update_id,
                total_size,
            )
        except UploadRestartRequired:
            raise
        except UploadError:
            raise UploadRestartRequired(
                "resource upload status could not be recovered; restart upload"
            ) from None
        if status["state"] not in (self.STATE_READY, self.STATE_RECEIVING):
            raise UploadRestartRequired(
                "resource update is no longer receiving data; restart upload"
            )
        next_offset = status["next_offset"]
        if not attempted_offset <= next_offset <= attempted_offset + attempted_length:
            raise UploadRestartRequired(
                "device next_offset is outside the failed chunk; restart upload"
            )
        return next_offset

    def _finish_with_recovery(self, update_id, total_size,
                              baseline_generation):
        deadline = self.clock() + self.poll_timeout_seconds
        for attempt in range(self.max_recovery_attempts + 1):
            try:
                return self.client.finish(update_id)
            except (TransportError, HttpStatusError, ResponseError):
                if attempt >= self.max_recovery_attempts:
                    raise UploadRestartRequired(
                        "resource finish could not be confirmed"
                    ) from None
                try:
                    status = self._status_with_recovery(
                        deadline,
                        "resource finish status could not be recovered",
                    )
                except UploadError:
                    continue
                if (
                    status["state"] == self.STATE_IDLE
                    and status["error"] == 0
                    and status["update_id"] == update_id
                    and status["total_size"] == total_size
                    and status["next_offset"] == total_size
                    and self._generation_is_newer(
                        status["generation"], baseline_generation
                    )
                ):
                    return {"update_id": update_id, "accepted": True}
                self._matching_active_status(status, update_id, total_size)
                if status["next_offset"] != total_size:
                    raise UploadRestartRequired(
                        "resource finish was attempted before all bytes arrived"
                    )
                if status["state"] in (
                    self.STATE_VERIFYING,
                    self.STATE_COMMITTING,
                ):
                    return {"update_id": update_id, "accepted": True}
                if status["state"] not in (
                    self.STATE_READY,
                    self.STATE_RECEIVING,
                ):
                    raise UploadRestartRequired(
                        "resource finish entered an unexpected state"
                    )
        raise UploadRestartRequired("resource finish could not be confirmed")

    def _wait_complete(self, update_id, total_size, baseline_generation):
        deadline = self.clock() + self.poll_timeout_seconds
        for attempt in range(self.max_poll_attempts):
            status = self._status_with_recovery(
                deadline,
                "resource activation status could not be recovered",
            )
            if status["error"] != 0 or status["state"] in (
                self.STATE_ABORTED,
                self.STATE_FAILED,
            ):
                raise UploadError(
                    "resource update failed with device error {}".format(status["error"])
                )
            if status["state"] == self.STATE_IDLE:
                if (
                    status["update_id"] == update_id
                    and status["total_size"] == total_size
                    and status["next_offset"] == total_size
                    and self._generation_is_newer(
                        status["generation"], baseline_generation
                    )
                ):
                    return status
                raise UploadError(
                    "resource activation completed without a new generation"
                )
            self._matching_active_status(status, update_id, total_size)
            if status["state"] == self.STATE_VERIFYING:
                self._emit("verifying", total_size, total_size)
            elif status["state"] == self.STATE_COMMITTING:
                self._emit("activating", total_size, total_size)
            else:
                raise ResponseError("device entered an unexpected completion state")
            if attempt + 1 < self.max_poll_attempts and self.clock() < deadline:
                self.sleep(min(0.25, max(0.0, deadline - self.clock())))
            if self.clock() >= deadline:
                break
        raise UploadError("timed out while activating the resource bank")

    def upload(self, package_bytes, password):
        try:
            package = resource_format.verify_package(package_bytes)
        except (resource_format.ResourceFormatError, TypeError, ValueError):
            raise UploadValidationError("resource package validation failed") from None
        package_bytes = package.package_bytes
        total_size = package.total_size
        self._emit("validated", total_size, total_size)
        self.client.login(password)

        preflight = self._preflight_idle()
        baseline_generation = preflight["generation"]
        try:
            session = self.client.begin(
                total_size, package.package_crc32, package.version
            )
        except (TransportError, ResponseError):
            session = self._recover_begin(total_size, baseline_generation)
        update_id = session["update_id"]
        self._emit("erasing", 0, total_size)
        status = self._wait_ready(update_id, total_size)
        if status["generation"] != baseline_generation:
            raise UploadRestartRequired(
                "resource generation changed while beginning upload"
            )
        offset = status["next_offset"]
        self._emit("uploading", offset, total_size)

        recovery_attempts = 0
        while offset < total_size:
            chunk = package_bytes[offset : min(offset + HTTP_CHUNK_SIZE, total_size)]
            try:
                result = self.client.put_chunk(update_id, offset, chunk)
            except (TransportError, HttpStatusError, ResponseError):
                recovery_attempts += 1
                if recovery_attempts > self.max_recovery_attempts:
                    raise UploadRestartRequired(
                        "resource chunk could not be recovered; restart upload"
                    ) from None
                offset = self._recover_offset(
                    update_id, total_size, offset, len(chunk)
                )
                self._emit("uploading", offset, total_size)
                continue
            if result["total_size"] != total_size:
                raise ResponseError("chunk response total_size changed")
            offset = result["next_offset"]
            recovery_attempts = 0
            self._emit("uploading", offset, total_size)

        self._emit("verifying", total_size, total_size)
        self._finish_with_recovery(
            update_id, total_size, baseline_generation
        )
        status = self._wait_complete(
            update_id, total_size, baseline_generation
        )
        self._emit("complete", total_size, total_size)
        return status


def _build_parser():
    parser = argparse.ArgumentParser(
        description="Upload validated OLED expression resources to the robot"
    )
    commands = parser.add_subparsers(dest="command", required=True)

    upload = commands.add_parser("upload", help="validate and upload an .arp package")
    upload.add_argument("url", help="robot HTTP origin, for example http://192.168.4.1")
    upload.add_argument("package", type=Path)
    upload.add_argument("--timeout", type=float, default=10.0)

    status = commands.add_parser("status", help="read resource storage status")
    status.add_argument("url")
    status.add_argument("--timeout", type=float, default=10.0)

    abort = commands.add_parser("abort", help="abort an active resource update")
    abort.add_argument("url")
    abort.add_argument("update_id", type=int)
    abort.add_argument("--timeout", type=float, default=10.0)
    return parser


def _print_progress(stream, stage, current, total):
    if total > 0:
        percent = current * 100 // total
        print("{}: {}/{} ({}%)".format(stage, current, total, percent), file=stream)
    else:
        print(stage, file=stream)


def main(argv=None, transport_factory=HttpTransport, password_reader=None,
         stdout=None, stderr=None, sleep=time.sleep):
    stdout = stdout or sys.stdout
    stderr = stderr or sys.stderr
    password_reader = password_reader or getpass.getpass
    args = _build_parser().parse_args(argv)

    try:
        package_bytes = None
        if args.command == "upload":
            try:
                package_bytes = args.package.read_bytes()
                resource_format.verify_package(package_bytes)
            except (OSError, resource_format.ResourceFormatError):
                raise UploadValidationError("resource package validation failed") from None

        transport = transport_factory(args.url, timeout=args.timeout)
        client = ResourceApiClient(transport)
        password = password_reader("Administrator password: ")
        if args.command == "upload":
            uploader = ResourceUploader(
                client,
                progress=lambda stage, current, total: _print_progress(
                    stdout, stage, current, total
                ),
                sleep=sleep,
            )
            result = uploader.upload(package_bytes, password)
            print("resource upload complete (state {})".format(result["state"]), file=stdout)
        else:
            client.login(password)
            if args.command == "status":
                print(json.dumps(client.status(), sort_keys=True), file=stdout)
            else:
                result = client.abort(args.update_id)
                print("resource update {} aborted".format(result["update_id"]), file=stdout)
    except UploadError as error:
        print("error: {}".format(error), file=stderr)
        return 1
    except ValueError:
        print("error: resource operation failed", file=stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
