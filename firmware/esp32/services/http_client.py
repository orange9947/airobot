"""Small bounded async JSON HTTPS client for MicroPython."""

try:
    import json
except ImportError:
    import ujson as json

from firmware.esp32.core.compat import asyncio


class HttpClientError(RuntimeError):
    def __init__(self, message, status=0, body=""):
        super().__init__(message)
        self.status = status
        self.body = body


def parse_url(url):
    if "://" not in url:
        raise ValueError("URL must include a scheme")
    scheme, remainder = url.split("://", 1)
    if scheme not in ("http", "https"):
        raise ValueError("unsupported URL scheme")
    if "/" in remainder:
        authority, path = remainder.split("/", 1)
        path = "/" + path
    else:
        authority, path = remainder, "/"
    if ":" in authority:
        host, port_text = authority.rsplit(":", 1)
        port = int(port_text)
    else:
        host = authority
        port = 443 if scheme == "https" else 80
    return scheme, host, port, path


class AsyncJsonClient:
    def __init__(self, max_body=65536):
        self.max_body = max_body

    async def _read_chunked(self, reader):
        body = bytearray()
        while True:
            size_line = await reader.readline()
            size = int(size_line.split(b";", 1)[0], 16)
            if size == 0:
                await reader.readline()
                break
            if len(body) + size > self.max_body:
                raise HttpClientError("response body too large")
            body.extend(await reader.readexactly(size))
            await reader.readline()
        return bytes(body)

    async def post_json(self, url, payload, headers=None, timeout_s=60, response_parser=None):
        coroutine = self._post_json(url, payload, headers or {}, response_parser)
        if hasattr(asyncio, "wait_for"):
            return await asyncio.wait_for(coroutine, timeout_s)
        return await coroutine

    async def _post_json(self, url, payload, headers, response_parser=None):
        scheme, host, port, path = parse_url(url)
        reader, writer = await asyncio.open_connection(host, port, ssl=(scheme == "https"))
        try:
            encoded = json.dumps(payload).encode("utf-8")
            request_headers = {
                "Host": host,
                "Content-Type": "application/json",
                "Content-Length": str(len(encoded)),
                "Connection": "close",
            }
            request_headers.update(headers)
            writer.write("POST {} HTTP/1.1\r\n".format(path).encode())
            for name, value in request_headers.items():
                writer.write("{}: {}\r\n".format(name, value).encode())
            writer.write(b"\r\n")
            writer.write(encoded)
            await writer.drain()

            status_line = await reader.readline()
            try:
                status = int(status_line.split(b" ", 2)[1])
            except (IndexError, ValueError):
                raise HttpClientError("invalid HTTP response")
            response_headers = {}
            while True:
                line = await reader.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                name, value = line.decode().split(":", 1)
                response_headers[name.strip().lower()] = value.strip().lower()
            if response_headers.get("transfer-encoding") == "chunked":
                body = await self._read_chunked(reader)
            else:
                length = int(response_headers.get("content-length", "0"))
                if length > self.max_body:
                    raise HttpClientError("response body too large", status)
                body = await reader.readexactly(length) if length else await reader.read(self.max_body + 1)
                if len(body) > self.max_body:
                    raise HttpClientError("response body too large", status)
        finally:
            writer.close()
            if hasattr(writer, "wait_closed"):
                await writer.wait_closed()
        text = body.decode("utf-8", "replace")
        if status < 200 or status >= 300:
            raise HttpClientError("HTTP {}".format(status), status, text[:512])
        return self._decode_body(text, status, response_headers, response_parser)

    @staticmethod
    def _decode_body(text, status, response_headers, response_parser=None):
        if response_parser is not None:
            try:
                return response_parser(text, response_headers)
            except HttpClientError:
                raise
            except Exception:
                raise HttpClientError("invalid response payload", status, text[:512])
        try:
            return json.loads(text) if text else {}
        except ValueError:
            raise HttpClientError("invalid JSON response", status, text[:512])
