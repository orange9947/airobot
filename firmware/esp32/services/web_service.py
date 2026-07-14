"""Local authenticated HTTP API, static server, and WebSocket event stream."""

import binascii
import hashlib
import os
from collections import deque

try:
    import json
except ImportError:
    import ujson as json

from firmware.esp32.core.compat import asyncio, sleep_ms
from firmware.esp32.core.security import DEFAULT_PASSWORD_ITERATIONS, random_token
from protocol.generated import protocol_ids

WEBSOCKET_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_REQUEST_BODY = 8192

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
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


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
    def __init__(self, config, device, llm, network, static_root="/www"):
        self.config = config
        self.device = device
        self.llm = llm
        self.network = network
        self.static_root = static_root.rstrip("/")
        self.sessions = {}
        self.websocket_clients = []
        self.event_capacity = 32
        self.events = deque((), self.event_capacity)
        self.server = None
        self.auth_busy = False

    def publish(self, event):
        if len(self.events) >= self.event_capacity:
            self.events.popleft()
        self.events.append(event)

    async def start(self, host="0.0.0.0", port=80):
        self.server = await asyncio.start_server(self._handle_client, host, port)
        asyncio.create_task(self._broadcast_loop())
        return self.server

    def _session(self, headers):
        cookie = headers.get("cookie", "")
        for part in cookie.split(";"):
            name, separator, value = part.strip().partition("=")
            if separator and name == "robot_session":
                return self.sessions.get(value)
        return None

    def _require_auth(self, headers, mutating=False):
        session = self._session(headers)
        if session is None:
            raise ApiError(401, "authentication required")
        if mutating and headers.get("x-csrf-token") != session["csrf"]:
            raise ApiError(403, "invalid CSRF token")
        return session

    async def _read_request(self, reader):
        request_line = await reader.readline()
        if not request_line:
            return None
        try:
            method, target, _version = request_line.decode().strip().split(" ", 2)
        except ValueError:
            raise ApiError(400, "invalid request line")
        headers = {}
        while True:
            line = await reader.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            name, value = line.decode().split(":", 1)
            headers[name.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        if length > MAX_REQUEST_BODY:
            raise ApiError(413, "request body too large")
        body = await reader.readexactly(length) if length else b""
        path = target.split("?", 1)[0]
        return method, path, headers, body

    async def _write_response(self, writer, status, body=b"", content_type="application/json; charset=utf-8", extra_headers=None):
        reason = {
            200: "OK", 201: "Created", 204: "No Content", 400: "Bad Request",
            401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
            409: "Conflict", 413: "Payload Too Large", 500: "Internal Server Error",
            503: "Service Unavailable",
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
            return json.loads(body.decode()) if body else {}
        except ValueError:
            raise ApiError(400, "invalid JSON")

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

    async def _login_once(self, body):
        request = self._body_json(body)
        password = request.get("password", "")
        if not self.config.has_admin_password():
            try:
                await self.config.set_admin_password_async(password)
            except (TypeError, ValueError) as exc:
                raise ApiError(400, str(exc))
        else:
            if not await self.config.verify_admin_password_async(password):
                raise ApiError(401, "invalid password")
            if self.config.admin_password_iterations() != DEFAULT_PASSWORD_ITERATIONS:
                await self.config.set_admin_password_async(password)
        if len(self.sessions) >= 4:
            self.sessions.pop(next(iter(self.sessions)))
        token = random_token()
        csrf = random_token(16)
        self.sessions[token] = {"csrf": csrf}
        return 200, {"ok": True, "csrf": csrf}, {
            "Set-Cookie": "robot_session={}; Path=/; HttpOnly; SameSite=Strict".format(token)
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

    async def dispatch_api(self, method, path, headers, body):
        if method == "GET" and path == "/api/v1/bootstrap":
            return 200, {
                "setup_required": not self.config.has_admin_password(),
                "network": self.network.status(),
            }, {}
        if method == "POST" and path == "/api/v1/session/login":
            return await self._login(body)
        if method == "POST" and path == "/api/v1/session/logout":
            self._require_auth(headers, True)
            return 200, {"ok": True}, {"Set-Cookie": "robot_session=; Path=/; Max-Age=0"}

        if method == "GET" and path == "/api/v1/status":
            self._require_auth(headers)
            return 200, self._status(), {}
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
        digest = hashlib.sha1(key.encode() + WEBSOCKET_GUID).digest()
        accept = binascii.b2a_base64(digest).strip().decode()
        writer.write(b"HTTP/1.1 101 Switching Protocols\r\n")
        writer.write(b"Upgrade: websocket\r\nConnection: Upgrade\r\n")
        writer.write("Sec-WebSocket-Accept: {}\r\n\r\n".format(accept).encode())
        await writer.drain()
        self.websocket_clients.append(writer)
        try:
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
                event = self.events.popleft()
                for writer in tuple(self.websocket_clients):
                    try:
                        await self._websocket_send(writer, event)
                    except Exception:
                        if writer in self.websocket_clients:
                            self.websocket_clients.remove(writer)
            await sleep_ms(50)

    async def _handle_client(self, reader, writer):
        try:
            request = await self._read_request(reader)
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
            await self._write_response(writer, exc.status, _json_bytes({"error": exc.message}))
        except Exception as exc:
            await self._write_response(writer, 500, _json_bytes({"error": str(exc)[:160]}))
        finally:
            writer.close()
            if hasattr(writer, "wait_closed"):
                await writer.wait_closed()
