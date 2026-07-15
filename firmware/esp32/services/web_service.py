"""Local authenticated HTTP API, static server, and WebSocket event stream."""

import binascii
import hashlib
import os
from collections import deque

try:
    import json
except ImportError:
    import ujson as json

from firmware.esp32.core.compat import asyncio, sleep_ms, ticks_diff, ticks_ms
from firmware.esp32.core.security import (
    DEFAULT_PASSWORD_ITERATIONS,
    constant_time_equal,
    random_token,
)
from firmware.esp32.services.resource_service import ResourceServiceError
from protocol.generated import protocol_ids

WEBSOCKET_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_REQUEST_BODY = 8192
MAX_RESOURCE_CHUNK_BODY = 4096
MAX_REQUEST_LINE = 1024
MAX_HEADER_LINE = 1024
MAX_HEADER_BYTES = 4096
MAX_HEADER_COUNT = 32
MAX_HTTP_CLIENTS = 4
MAX_WEBSOCKET_CLIENTS = 2
REQUEST_READ_TIMEOUT_S = 10
WEBSOCKET_WRITE_TIMEOUT_S = 0.25
HTTP_CLOSE_TIMEOUT_S = 0.25
SESSION_IDLE_TTL_MS = 30 * 60 * 1000
SESSION_ABSOLUTE_TTL_MS = 8 * 60 * 60 * 1000
LOGIN_BACKOFF_INITIAL_MS = 250
LOGIN_BACKOFF_MAX_MS = 2000
JSON_CONTENT_TYPE = "application/json"
RESOURCE_CHUNK_CONTENT_TYPE = "application/octet-stream"

RESOURCE_CONFLICT_ERRORS = (
    "aborting",
    "busy",
    "finishing",
    "incomplete",
    "no_session",
    "offset_mismatch",
    "session_mismatch",
    "session_timeout",
    "status_required",
)
RESOURCE_UNAVAILABLE_ERRORS = (
    "device_error",
    "id_exhausted",
    "invalid_status",
    "link_lost",
)

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".woff2": "font/woff2",
}


class ApiError(Exception):
    def __init__(self, status, message, code=None):
        super().__init__(message)
        self.status = status
        self.message = message
        self.code = code

    def payload(self):
        if self.code is None:
            return {"error": self.message}
        return {"error": {"code": self.code, "message": self.message}}


def _json_bytes(value):
    return json.dumps(value).encode("utf-8")


def _state_name(value):
    return {
        protocol_ids.ROBOTSTATE_BOOT: "boot",
        protocol_ids.ROBOTSTATE_SELF_TEST: "self_test",
        protocol_ids.ROBOTSTATE_IDLE: "idle",
        protocol_ids.ROBOTSTATE_MANUAL: "manual",
        protocol_ids.ROBOTSTATE_AI: "ai",
        protocol_ids.ROBOTSTATE_ESTOP: "estop",
        protocol_ids.ROBOTSTATE_FAULT: "fault",
    }.get(value, "unknown")


class WebService:
    def __init__(
        self,
        config,
        device,
        llm,
        network,
        static_root="/www",
        resource=None,
        clock=None,
        sleep=None,
        request_timeout_s=REQUEST_READ_TIMEOUT_S,
        websocket_write_timeout_s=WEBSOCKET_WRITE_TIMEOUT_S,
    ):
        self.config = config
        self.device = device
        self.llm = llm
        self.network = network
        self.resource = resource
        self.static_root = static_root.rstrip("/")
        self.sessions = {}
        self.websocket_clients = []
        self.event_capacity = 32
        self.events = deque((), self.event_capacity)
        self.server = None
        self.auth_busy = False
        self._auth_failures = 0
        self._setup_nonce = None
        self._clock = clock or ticks_ms
        self._sleep = sleep or sleep_ms
        self._request_timeout_s = request_timeout_s
        self._websocket_write_timeout_s = websocket_write_timeout_s
        self._active_http_clients = 0

    def publish(self, event):
        if len(self.events) >= self.event_capacity:
            self.events.popleft()
        self.events.append(event)

    async def start(self, host="0.0.0.0", port=80):
        self.server = await asyncio.start_server(self._handle_client, host, port)
        asyncio.create_task(self._broadcast_loop())
        return self.server

    @staticmethod
    def _session_token(headers):
        cookie = headers.get("cookie", "")
        for part in cookie.split(";"):
            name, separator, value = part.strip().partition("=")
            if separator and name == "robot_session":
                return value
        return None

    @staticmethod
    def _session_expired(session, now):
        return (
            ticks_diff(now, session["created_ms"]) >= SESSION_ABSOLUTE_TTL_MS
            or ticks_diff(now, session["last_used_ms"]) >= SESSION_IDLE_TTL_MS
        )

    def _prune_sessions(self, now=None):
        if now is None:
            now = self._clock()
        for token, session in tuple(self.sessions.items()):
            if self._session_expired(session, now):
                self.sessions.pop(token, None)

    def _session(self, headers, now=None):
        if now is None:
            now = self._clock()
        self._prune_sessions(now)
        token = self._session_token(headers)
        if token is not None:
            return self.sessions.get(token)
        return None

    def _require_auth(self, headers, mutating=False):
        now = self._clock()
        session = self._session(headers, now)
        if session is None:
            raise ApiError(401, "authentication required")
        if mutating and headers.get("x-csrf-token") != session["csrf"]:
            raise ApiError(403, "invalid CSRF token")
        session["last_used_ms"] = now
        return session

    @staticmethod
    async def _readline_bounded(reader, limit):
        line = bytearray()
        while len(line) <= limit:
            try:
                value = await reader.readexactly(1)
            except EOFError:
                return bytes(line)
            if not value:
                return bytes(line)
            line.extend(value)
            if value == b"\n":
                return bytes(line)
        raise ApiError(431, "HTTP line is too large")

    async def _read_request(self, reader):
        request_line = await self._readline_bounded(reader, MAX_REQUEST_LINE)
        if not request_line:
            return None
        try:
            method, target, version = request_line.decode("ascii").strip().split(" ", 2)
        except (UnicodeError, ValueError):
            raise ApiError(400, "invalid request line")
        if (
            version not in ("HTTP/1.0", "HTTP/1.1")
            or not target.startswith("/")
            or any(ord(character) < 0x20 for character in target)
        ):
            raise ApiError(400, "invalid request line")
        headers = {}
        header_bytes = 0
        while True:
            line = await self._readline_bounded(reader, MAX_HEADER_LINE)
            if line in (b"\r\n", b"\n", b""):
                break
            header_bytes += len(line)
            if header_bytes > MAX_HEADER_BYTES or len(headers) >= MAX_HEADER_COUNT:
                raise ApiError(431, "HTTP headers are too large")
            try:
                name, value = line.decode("ascii").split(":", 1)
            except (UnicodeError, ValueError):
                raise ApiError(400, "invalid HTTP header")
            name = name.strip().lower()
            if (
                not name
                or any(
                    not (
                        "a" <= character <= "z"
                        or "0" <= character <= "9"
                        or character == "-"
                    )
                    for character in name
                )
                or name in headers
            ):
                raise ApiError(400, "invalid HTTP header")
            value = value.strip()
            if any(ord(character) < 0x20 and character != "\t"
                   for character in value):
                raise ApiError(400, "invalid HTTP header")
            headers[name] = value
        if "transfer-encoding" in headers:
            raise ApiError(400, "transfer encoding is not supported")
        length_text = headers.get("content-length", "0")
        if (
            not length_text
            or len(length_text) > 10
            or any(character < "0" or character > "9" for character in length_text)
        ):
            raise ApiError(400, "invalid content length")
        length = 0
        for character in length_text:
            length = length * 10 + ord(character) - ord("0")
            if length > MAX_REQUEST_BODY:
                raise ApiError(413, "request body too large")
        if length > MAX_REQUEST_BODY:
            raise ApiError(413, "request body too large")
        try:
            body = await reader.readexactly(length) if length else b""
        except EOFError:
            raise ApiError(400, "incomplete request body")
        path = target.split("?", 1)[0]
        return method, path, headers, body

    async def _write_response(self, writer, status, body=b"", content_type="application/json; charset=utf-8", extra_headers=None):
        reason = {
            200: "OK", 201: "Created", 204: "No Content", 400: "Bad Request",
            401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
            408: "Request Timeout", 409: "Conflict", 413: "Payload Too Large",
            415: "Unsupported Media Type", 500: "Internal Server Error",
            431: "Request Header Fields Too Large", 503: "Service Unavailable",
        }.get(status, "Error")
        writer.write("HTTP/1.1 {} {}\r\n".format(status, reason).encode())
        writer.write("Content-Type: {}\r\n".format(content_type).encode())
        writer.write("Content-Length: {}\r\n".format(len(body)).encode())
        writer.write(b"Cache-Control: no-store\r\n")
        writer.write(b"X-Content-Type-Options: nosniff\r\n")
        writer.write(b"Connection: close\r\n")
        for name, value in (extra_headers or {}).items():
            writer.write("{}: {}\r\n".format(name, value).encode())
        writer.write(b"\r\n")
        writer.write(body)
        await writer.drain()

    def _body_json(self, body):
        try:
            value = json.loads(body.decode()) if body else {}
        except (UnicodeError, ValueError):
            raise ApiError(400, "invalid JSON")
        if not isinstance(value, dict):
            raise ApiError(400, "JSON body must be an object")
        return value

    @staticmethod
    def _media_type(headers):
        return headers.get("content-type", "").split(";", 1)[0].strip().lower()

    @classmethod
    def _require_content_type(cls, headers, expected):
        if cls._media_type(headers) != expected:
            raise ApiError(
                415,
                "request Content-Type must be {}".format(expected),
                "unsupported_media_type",
            )

    def _status(self):
        status = dict(self.device.status)
        status["state_name"] = _state_name(status.get("state"))
        status["network"] = self.network.status()
        status["provider"] = self.config.config["active_provider"]
        status["model"] = self.config.config["providers"][status["provider"]].get("model", "")
        return status

    async def _login(self, body):
        if self.auth_busy:
            raise ApiError(409, "authentication is busy")
        self.auth_busy = True
        try:
            return await self._login_once(body)
        finally:
            self.auth_busy = False

    def _current_setup_nonce(self):
        if self.config.has_admin_password():
            self._setup_nonce = None
            return None
        if self._setup_nonce is None:
            self._setup_nonce = random_token(16)
        return self._setup_nonce

    async def _authentication_failed(self, status, message):
        self._auth_failures = min(self._auth_failures + 1, 32)
        exponent = min(self._auth_failures - 1, 3)
        delay_ms = min(
            LOGIN_BACKOFF_INITIAL_MS << exponent, LOGIN_BACKOFF_MAX_MS
        )
        await self._sleep(delay_ms)
        raise ApiError(status, message)

    async def _login_once(self, body):
        request = self._body_json(body)
        password = request.get("password", "")
        if not self.config.has_admin_password():
            expected_nonce = self._current_setup_nonce()
            provided_nonce = request.get("setup_nonce")
            if (
                not isinstance(provided_nonce, str)
                or not constant_time_equal(
                    provided_nonce.encode("utf-8"), expected_nonce.encode("ascii")
                )
            ):
                await self._authentication_failed(403, "invalid setup nonce")
            try:
                await self.config.set_admin_password_async(password)
            except (TypeError, ValueError) as exc:
                raise ApiError(400, str(exc))
            self._setup_nonce = None
        else:
            if "setup_nonce" in request:
                await self._authentication_failed(403, "invalid setup nonce")
            if not await self.config.verify_admin_password_async(password):
                await self._authentication_failed(401, "invalid password")
            if self.config.admin_password_iterations() != DEFAULT_PASSWORD_ITERATIONS:
                await self.config.set_admin_password_async(password)
        self._auth_failures = 0
        now = self._clock()
        self._prune_sessions(now)
        if len(self.sessions) >= 4:
            self.sessions.pop(next(iter(self.sessions)))
        token = random_token()
        csrf = random_token(16)
        self.sessions[token] = {
            "csrf": csrf,
            "created_ms": now,
            "last_used_ms": now,
        }
        return 200, {
            "ok": True,
            "csrf": csrf,
            "config": self.config.public_view(),
            "status": self._status(),
        }, {
            "Set-Cookie": (
                "robot_session={}; Path=/; Max-Age={}; HttpOnly; SameSite=Strict"
            ).format(token, SESSION_ABSOLUTE_TTL_MS // 1000)
        }

    async def _manual_motion(self, request):
        direction = request.get("direction")
        try:
            speed = int(request.get("speed_percent", 40))
            duration = int(request.get("duration_ms", 500))
        except (TypeError, ValueError):
            raise ApiError(400, "invalid motion parameters")
        if direction not in ("forward", "backward", "left", "right") or not 10 <= speed <= 100 or not 100 <= duration <= 2000:
            raise ApiError(400, "invalid motion parameters")
        motion = self.config.config["motion"]
        rate = max(1, int(motion["soft_rate_sps"]) * speed // 100)
        steps = max(1, rate * duration // 1000)
        left, right = {
            "forward": (steps, steps), "backward": (-steps, -steps),
            "left": (-steps, steps), "right": (steps, -steps),
        }[direction]
        return await self.device.move(left, right, rate, int(motion["accel_sps2"]), duration)

    def _require_resource(self):
        if self.resource is None:
            raise ApiError(503, "resource service is unavailable", "service_unavailable")
        return self.resource

    @staticmethod
    def _resource_path_u32(value, allow_zero=False):
        if (
            not value
            or len(value) > 10
            or any(character < "0" or character > "9" for character in value)
        ):
            raise ApiError(400, "invalid resource path", "invalid_request")
        number = 0
        for character in value:
            number = number * 10 + ord(character) - ord("0")
            if number > 0xFFFFFFFF:
                raise ApiError(400, "invalid resource path", "invalid_request")
        minimum = 0 if allow_zero else 1
        if not minimum <= number <= 0xFFFFFFFF:
            raise ApiError(400, "invalid resource path", "invalid_request")
        return number

    async def _resource_call(self, method_name, *args):
        resource = self._require_resource()
        try:
            method = getattr(resource, method_name)
            return await method(*args)
        except ResourceServiceError as error:
            if error.code in RESOURCE_CONFLICT_ERRORS:
                status = 409
            elif error.code in RESOURCE_UNAVAILABLE_ERRORS:
                status = 503
            else:
                status = 400
            raise ApiError(status, error.message, error.code)

    @staticmethod
    def _is_resource_chunk_request(method, path):
        parts = path.split("/")
        return (
            method == "PUT"
            and len(parts) >= 7
            and parts[1:5] == ["api", "v1", "resources", "updates"]
            and parts[6] == "chunks"
        )

    async def dispatch_api(self, method, path, headers, body):
        if method in ("POST", "PUT", "DELETE"):
            expected_content_type = (
                RESOURCE_CHUNK_CONTENT_TYPE
                if self._is_resource_chunk_request(method, path)
                else JSON_CONTENT_TYPE
            )
            self._require_content_type(headers, expected_content_type)
        if method == "GET" and path == "/api/v1/bootstrap":
            setup_required = not self.config.has_admin_password()
            payload = {
                "setup_required": setup_required,
                "network": self.network.status(),
            }
            if setup_required:
                payload["setup_nonce"] = self._current_setup_nonce()
            return 200, payload, {}
        if method == "POST" and path == "/api/v1/session/login":
            return await self._login(body)
        if method == "POST" and path == "/api/v1/session/logout":
            self._require_auth(headers, True)
            self.sessions.pop(self._session_token(headers), None)
            return 200, {"ok": True}, {"Set-Cookie": "robot_session=; Path=/; Max-Age=0"}

        if method == "GET" and path == "/api/v1/status":
            self._require_auth(headers)
            return 200, self._status(), {}
        if method == "GET" and path == "/api/v1/resources/status":
            self._require_auth(headers)
            return 200, await self._resource_call("status", 0), {}
        if method == "POST" and path == "/api/v1/resources/updates":
            self._require_auth(headers, True)
            request = self._body_json(body)
            if not isinstance(request, dict):
                raise ApiError(400, "invalid resource request", "invalid_request")
            result = await self._resource_call(
                "begin",
                request.get("package_size"),
                request.get("package_crc32"),
                request.get("format_version"),
            )
            return 201, result, {}

        resource_prefix = "/api/v1/resources/updates/"
        if path.startswith(resource_prefix):
            parts = path.split("/")
            if method == "PUT" and len(parts) == 8 and parts[6] == "chunks":
                self._require_auth(headers, True)
                update_id = self._resource_path_u32(parts[5])
                offset = self._resource_path_u32(parts[7], allow_zero=True)
                if len(body) > MAX_RESOURCE_CHUNK_BODY:
                    raise ApiError(413, "resource chunk too large", "chunk_too_large")
                return 200, await self._resource_call("write_chunk", update_id, offset, body), {}
            if method == "POST" and len(parts) == 7 and parts[6] == "finish":
                self._require_auth(headers, True)
                update_id = self._resource_path_u32(parts[5])
                return 200, await self._resource_call("finish", update_id), {}
            if method == "DELETE" and len(parts) == 6:
                self._require_auth(headers, True)
                update_id = self._resource_path_u32(parts[5])
                return 200, await self._resource_call("abort", update_id), {}
        if method == "GET" and path == "/api/v1/config":
            self._require_auth(headers)
            return 200, self.config.public_view(), {}
        if method == "PUT" and path == "/api/v1/config":
            self._require_auth(headers, True)
            request = self._body_json(body)
            try:
                if "wifi" in request and not request["wifi"].get("password"):
                    request["wifi"]["password"] = self.config.config.get("wifi", {}).get("password", "")
                self.config.update_public(request)
            except (AttributeError, KeyError, TypeError, ValueError) as exc:
                raise ApiError(400, str(exc))
            return 200, {"ok": True, "restart_required": "wifi" in request}, {}
        if method == "PUT" and path == "/api/v1/secrets":
            self._require_auth(headers, True)
            request = self._body_json(body)
            try:
                self.config.set_provider_key(request.get("provider"), request.get("api_key", ""))
            except (TypeError, ValueError) as exc:
                raise ApiError(400, str(exc))
            return 200, {"ok": True}, {}
        if method == "POST" and path == "/api/v1/mode":
            self._require_auth(headers, True)
            mode_name = self._body_json(body).get("mode")
            mode = {"idle": protocol_ids.MODE_IDLE, "manual": protocol_ids.MODE_MANUAL, "ai": protocol_ids.MODE_AI}.get(mode_name)
            if mode is None:
                raise ApiError(400, "invalid mode")
            try:
                command_id = await self.device.set_mode(mode)
            except RuntimeError as exc:
                raise ApiError(503, str(exc))
            return 200, {"ok": True, "command_id": command_id}, {}
        if method == "POST" and path == "/api/v1/motion":
            self._require_auth(headers, True)
            try:
                result = await self._manual_motion(self._body_json(body))
            except RuntimeError as exc:
                raise ApiError(503, str(exc))
            return 200, result, {}
        if method == "POST" and path == "/api/v1/stop":
            self._require_auth(headers, True)
            try:
                command_id = await self.device.stop()
            except RuntimeError as exc:
                raise ApiError(503, str(exc))
            return 200, {"ok": True, "command_id": command_id}, {}
        if method == "POST" and path == "/api/v1/estop/clear":
            self._require_auth(headers, True)
            try:
                command_id = await self.device.clear_estop()
            except RuntimeError as exc:
                raise ApiError(503, str(exc))
            return 200, {"ok": True, "command_id": command_id}, {}
        if method == "POST" and path == "/api/v1/chat":
            self._require_auth(headers, True)
            try:
                response = await self.llm.chat(self._body_json(body).get("message", ""))
            except ValueError as exc:
                raise ApiError(400, str(exc))
            except RuntimeError as exc:
                raise ApiError(503, str(exc))
            return 200, response, {}
        if method == "DELETE" and path == "/api/v1/chat":
            self._require_auth(headers, True)
            self.llm.clear()
            return 200, {"ok": True}, {}
        raise ApiError(404, "API route not found")

    def _static_path(self, path):
        if path == "/":
            path = "/index.html"
        normalized = path.replace("\\", "/")
        if ".." in normalized or not normalized.startswith("/"):
            raise ApiError(404, "not found")
        return self.static_root + normalized

    async def _serve_static(self, writer, path):
        file_path = self._static_path(path)
        try:
            with open(file_path, "rb") as handle:
                body = handle.read()
        except OSError:
            raise ApiError(404, "not found")
        extension = "." + file_path.rsplit(".", 1)[-1] if "." in file_path else ""
        await self._write_response(writer, 200, body, CONTENT_TYPES.get(extension, "application/octet-stream"))

    async def _upgrade_websocket(self, reader, writer, headers):
        self._require_auth(headers)
        key = headers.get("sec-websocket-key")
        if not key:
            raise ApiError(400, "missing WebSocket key")
        if len(self.websocket_clients) >= MAX_WEBSOCKET_CLIENTS:
            raise ApiError(503, "too many WebSocket clients")
        digest = hashlib.sha1(key.encode() + WEBSOCKET_GUID).digest()
        accept = binascii.b2a_base64(digest).strip().decode()
        self.websocket_clients.append(writer)
        try:
            writer.write(b"HTTP/1.1 101 Switching Protocols\r\n")
            writer.write(b"Upgrade: websocket\r\nConnection: Upgrade\r\n")
            writer.write(
                "Sec-WebSocket-Accept: {}\r\n\r\n".format(accept).encode()
            )
            await writer.drain()
            while True:
                header = await reader.readexactly(2)
                opcode = header[0] & 0x0F
                length = header[1] & 0x7F
                if length == 126:
                    extended = await reader.readexactly(2)
                    length = (extended[0] << 8) | extended[1]
                elif length == 127:
                    extended = await reader.readexactly(8)
                    length = 0
                    for value in extended:
                        length = (length << 8) | value
                if length > MAX_REQUEST_BODY:
                    break
                mask = await reader.readexactly(4) if header[1] & 0x80 else None
                payload = bytearray(await reader.readexactly(length))
                if mask:
                    for index in range(length):
                        payload[index] ^= mask[index & 3]
                if opcode == 0x08:
                    writer.write(b"\x88\x00")
                    await writer.drain()
                    break
                if opcode == 0x09:
                    await self._websocket_send(writer, payload, opcode=0x0A)
        except (EOFError, OSError):
            pass
        finally:
            if writer in self.websocket_clients:
                self.websocket_clients.remove(writer)

    def _discard_websocket(self, writer):
        if writer in self.websocket_clients:
            self.websocket_clients.remove(writer)
        try:
            writer.close()
        except Exception:
            pass

    async def _websocket_send(self, writer, payload, opcode=0x01):
        data = _json_bytes(payload) if opcode == 0x01 else bytes(payload)
        header = bytearray((0x80 | opcode,))
        if len(data) < 126:
            header.append(len(data))
        elif len(data) < 65536:
            header.extend((126, (len(data) >> 8) & 0xFF, len(data) & 0xFF))
        else:
            return
        writer.write(header)
        writer.write(data)
        await writer.drain()

    async def _broadcast_loop(self):
        while True:
            while self.events:
                await self._broadcast_event(self.events.popleft())
            await sleep_ms(50)

    async def _broadcast_event(self, event):
        for writer in tuple(self.websocket_clients):
            try:
                await asyncio.wait_for(
                    self._websocket_send(writer, event),
                    self._websocket_write_timeout_s,
                )
            except Exception:
                self._discard_websocket(writer)

    async def _handle_client(self, reader, writer):
        if self._active_http_clients >= MAX_HTTP_CLIENTS:
            try:
                await asyncio.wait_for(
                    self._write_response(
                        writer,
                        503,
                        _json_bytes({"error": "too many connections"}),
                    ),
                    HTTP_CLOSE_TIMEOUT_S,
                )
            except Exception:
                pass
            finally:
                writer.close()
            return
        self._active_http_clients += 1
        try:
            try:
                request = await asyncio.wait_for(
                    self._read_request(reader), self._request_timeout_s
                )
            except asyncio.TimeoutError:
                raise ApiError(408, "request read timed out")
            if request is None:
                return
            method, path, headers, body = request
            if path == "/api/v1/events" and headers.get("upgrade", "").lower() == "websocket":
                await self._upgrade_websocket(reader, writer, headers)
                return
            if path.startswith("/api/"):
                status, value, extra = await self.dispatch_api(method, path, headers, body)
                await self._write_response(writer, status, _json_bytes(value), extra_headers=extra)
            elif method == "GET":
                await self._serve_static(writer, path)
            else:
                raise ApiError(404, "not found")
        except ApiError as exc:
            await self._write_response(writer, exc.status, _json_bytes(exc.payload()))
        except Exception:
            await self._write_response(
                writer, 500, _json_bytes({"error": "internal server error"})
            )
        finally:
            self._active_http_clients -= 1
            writer.close()
            if hasattr(writer, "wait_closed"):
                try:
                    await asyncio.wait_for(
                        writer.wait_closed(), HTTP_CLOSE_TIMEOUT_S
                    )
                except Exception:
                    pass
