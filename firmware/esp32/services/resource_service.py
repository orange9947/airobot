"""Bounded, resumable expression-resource uploads over DeviceService."""

from firmware.esp32.core.compat import ticks_diff, ticks_ms
from protocol.generated import protocol_ids


class ResourceServiceError(RuntimeError):
    def __init__(self, code, message, uncertain=False):
        self.code = code
        self.message = message
        self.uncertain = bool(uncertain)
        RuntimeError.__init__(self, message)

    def as_dict(self):
        return {"code": self.code, "message": self.message}


ERROR_MESSAGES = {
    "aborting": "resource update abort is being reconciled",
    "busy": "another resource operation is active",
    "chunk_too_large": "resource chunk exceeds 4096 bytes",
    "device_error": "resource command failed",
    "finishing": "resource update is being verified",
    "id_exhausted": "resource update identifiers are exhausted",
    "incomplete": "resource upload is incomplete",
    "invalid_request": "invalid resource request",
    "invalid_status": "device returned invalid resource status",
    "link_lost": "STM32 link was lost",
    "no_session": "no resource update is active",
    "offset_mismatch": "resource offset does not match next_offset",
    "package_overflow": "resource chunk exceeds package size",
    "session_mismatch": "resource update session does not match",
    "session_timeout": "resource update session timed out",
    "status_required": "resource status must be refreshed before continuing",
}


class ResourceService:
    HTTP_CHUNK_SIZE = 4096
    SPI_CHUNK_SIZE = 238
    MAX_PACKAGE_SIZE = 504 * 1024
    SESSION_TIMEOUT_MS = 60000
    FORMAT_VERSION = 1

    _ACTIVE_STATES = (
        protocol_ids.RESOURCESTATE_ERASING,
        protocol_ids.RESOURCESTATE_READY,
        protocol_ids.RESOURCESTATE_RECEIVING,
        protocol_ids.RESOURCESTATE_VERIFYING,
        protocol_ids.RESOURCESTATE_COMMITTING,
    )

    def __init__(self, device, clock=None):
        self.device = device
        self._clock = clock or ticks_ms
        self._session = None
        self._terminal = None
        self._last_update_id = 0
        self._operation_active = False
        add_listener = getattr(device, "add_listener", None)
        if add_listener is not None:
            add_listener(self._on_device_event)

    @property
    def active_update_id(self):
        if self._session is None:
            return None
        return self._session["update_id"]

    def _error(self, code, uncertain=False):
        return ResourceServiceError(
            code, ERROR_MESSAGES[code], uncertain=uncertain
        )

    def _connected(self):
        connected = getattr(self.device, "connected", None)
        if connected is not None:
            return bool(connected)
        status = getattr(self.device, "status", None)
        if isinstance(status, dict):
            return bool(status.get("connected"))
        return False

    def _on_device_event(self, event):
        if not isinstance(event, dict):
            return
        disconnected = event.get("type") == "link" and event.get("connected") is False
        status = event.get("status")
        if isinstance(status, dict) and status.get("connected") is False:
            disconnected = True
        if disconnected and self._session is not None:
            self._terminate("link_lost")

    def _enter_operation(self):
        if self._operation_active:
            raise self._error("busy")
        self._operation_active = True

    def _leave_operation(self):
        self._operation_active = False

    def _terminate(self, code):
        if self._session is None:
            return
        self._terminal = {"update_id": self._session["update_id"], "code": code}
        self._session = None

    def _next_update_id(self):
        if self._last_update_id >= 0xFFFFFFFF:
            raise self._error("id_exhausted")
        self._last_update_id += 1
        return self._last_update_id

    @staticmethod
    def _valid_u32(value):
        return not isinstance(value, bool) and isinstance(value, int) and 0 <= value <= 0xFFFFFFFF

    def _require_session(self, update_id):
        if not self._valid_u32(update_id) or update_id == 0:
            raise self._error("invalid_request")
        if self._session is None:
            if self._terminal is not None and self._terminal["update_id"] == update_id:
                raise self._error(self._terminal["code"])
            raise self._error("no_session")
        if update_id != self._session["update_id"]:
            raise self._error("session_mismatch")
        return self._session

    def _touch(self, session):
        session["last_activity_ms"] = self._clock()

    def _ensure_session_survived(self, session):
        if self._session is session and self._connected():
            return
        if self._session is session:
            self._terminate("link_lost")
        if self._terminal is not None and self._terminal["update_id"] == session["update_id"]:
            raise self._error(self._terminal["code"])
        raise self._error("link_lost")

    def local_status(self):
        if self._session is None:
            return {
                "active": False,
                "update_id": None,
                "next_offset": 0,
                "total_size": 0,
                "needs_sync": False,
                "beginning": False,
                "finishing": False,
                "aborting": False,
            }
        return {
            "active": True,
            "update_id": self._session["update_id"],
            "next_offset": self._session["next_offset"],
            "total_size": self._session["package_size"],
            "needs_sync": self._session["needs_sync"],
            "beginning": self._session["beginning"],
            "finishing": self._session["finishing"],
            "aborting": self._session["aborting"],
        }

    async def _device_call(self, method_name, *args):
        failure = None
        result = None
        try:
            method = getattr(self.device, method_name)
            result = await method(*args)
        except Exception as caught:
            failure = caught
        if failure is not None:
            if not self._connected():
                if self._session is not None:
                    self._terminate("link_lost")
                raise self._error("link_lost")
            if getattr(failure, "rejected", False):
                code = getattr(failure, "code", None)
                if code == protocol_ids.ERRORCODE_LINK_LOST:
                    if self._session is not None:
                        self._terminate("link_lost")
                    raise self._error("link_lost")
                if code in (
                    protocol_ids.ERRORCODE_BAD_STATE,
                    protocol_ids.ERRORCODE_QUEUE_FULL,
                ):
                    raise self._error("busy")
                if code in (
                    protocol_ids.ERRORCODE_BAD_MESSAGE,
                    protocol_ids.ERRORCODE_BAD_PAYLOAD,
                    protocol_ids.ERRORCODE_OUT_OF_RANGE,
                ):
                    raise self._error("invalid_request")
            raise self._error(
                "device_error",
                uncertain=bool(getattr(failure, "timed_out", False)),
            )
        return result

    async def _maintain(self):
        session = self._session
        if session is None:
            return None
        if not self._connected():
            self._terminate("link_lost")
            return "link_lost"
        if ticks_diff(self._clock(), session["last_activity_ms"]) < self.SESSION_TIMEOUT_MS:
            return None

        update_id = session["update_id"]
        self._terminate("session_timeout")
        try:
            await self._device_call("resource_abort", update_id)
        except ResourceServiceError:
            pass
        return "session_timeout"

    async def tick(self):
        self._enter_operation()
        try:
            return await self._maintain()
        finally:
            self._leave_operation()

    async def begin(self, package_size, package_crc32, format_version=FORMAT_VERSION):
        self._enter_operation()
        try:
            await self._maintain()
            if self._session is not None:
                raise self._error("busy")
            if not self._connected():
                raise self._error("link_lost")
            if (
                isinstance(package_size, bool)
                or not isinstance(package_size, int)
                or not 1 <= package_size <= self.MAX_PACKAGE_SIZE
                or isinstance(package_crc32, bool)
                or not isinstance(package_crc32, int)
                or not 0 <= package_crc32 <= 0xFFFFFFFF
                or isinstance(format_version, bool)
                or not isinstance(format_version, int)
                or format_version != self.FORMAT_VERSION
            ):
                raise self._error("invalid_request")

            update_id = self._next_update_id()
            self._session = {
                "update_id": update_id,
                "package_size": package_size,
                "next_offset": 0,
                "last_activity_ms": self._clock(),
                "needs_sync": True,
                "beginning": True,
                "finishing": False,
                "finish_acknowledged": False,
                "aborting": False,
            }
            self._terminal = None
            try:
                await self._device_call(
                    "resource_begin",
                    update_id,
                    package_size,
                    package_crc32,
                    format_version,
                )
            except ResourceServiceError as error:
                if (
                    error.code == "device_error"
                    and error.uncertain
                    and self._session is not None
                ):
                    self._touch(self._session)
                    return self.local_status()
                if self._session is not None:
                    self._session = None
                raise
            if not self._connected():
                raise self._error("link_lost")
            self._session["needs_sync"] = False
            self._session["beginning"] = False
            self._touch(self._session)
            return self.local_status()
        finally:
            self._leave_operation()

    async def write_chunk(self, update_id, offset, data):
        self._enter_operation()
        try:
            reason = await self._maintain()
            if reason is not None:
                raise self._error(reason)
            session = self._require_session(update_id)
            if session["aborting"]:
                raise self._error("aborting")
            if session["finishing"]:
                raise self._error("finishing")
            if session["needs_sync"]:
                raise self._error("status_required")
            if isinstance(offset, bool) or not isinstance(offset, int) or offset < 0:
                raise self._error("invalid_request")
            if offset != session["next_offset"]:
                raise self._error("offset_mismatch")
            try:
                data_view = memoryview(data)
            except TypeError:
                raise self._error("invalid_request")
            data_length = len(data_view)
            if data_length == 0:
                raise self._error("invalid_request")
            if data_length > self.HTTP_CHUNK_SIZE:
                raise self._error("chunk_too_large")
            if offset + data_length > session["package_size"]:
                raise self._error("package_overflow")

            cursor = 0
            while cursor < data_length:
                self._ensure_session_survived(session)
                chunk_end = min(cursor + self.SPI_CHUNK_SIZE, data_length)
                chunk = bytes(data_view[cursor:chunk_end])
                chunk_offset = session["next_offset"]
                try:
                    await self._device_call("resource_chunk", update_id, chunk_offset, chunk)
                except ResourceServiceError as error:
                    if error.code == "device_error" and error.uncertain:
                        session["needs_sync"] = True
                    raise
                self._ensure_session_survived(session)
                session["next_offset"] += len(chunk)
                self._touch(session)
                cursor = chunk_end
            return self.local_status()
        finally:
            self._leave_operation()

    async def finish(self, update_id):
        self._enter_operation()
        try:
            reason = await self._maintain()
            if reason is not None:
                raise self._error(reason)
            session = self._require_session(update_id)
            if session["aborting"]:
                raise self._error("aborting")
            if session["finishing"]:
                raise self._error("finishing")
            if session["needs_sync"]:
                raise self._error("status_required")
            if session["next_offset"] != session["package_size"]:
                raise self._error("incomplete")
            session["finishing"] = True
            session["finish_acknowledged"] = False
            try:
                await self._device_call("resource_finish", update_id)
            except ResourceServiceError as error:
                if error.code == "device_error" and error.uncertain:
                    session["needs_sync"] = True
                raise
            self._ensure_session_survived(session)
            session["finish_acknowledged"] = True
            session["needs_sync"] = False
            self._touch(session)
            return {"update_id": update_id, "accepted": True}
        finally:
            self._leave_operation()

    async def abort(self, update_id):
        self._enter_operation()
        try:
            reason = await self._maintain()
            if reason is not None:
                raise self._error(reason)
            session = self._require_session(update_id)
            if session["aborting"]:
                raise self._error("aborting")
            session["aborting"] = True
            device_error = None
            try:
                await self._device_call("resource_abort", update_id)
            except ResourceServiceError as error:
                device_error = error
            if device_error is not None:
                if device_error.code == "link_lost":
                    raise device_error
                session["needs_sync"] = True
                self._touch(session)
                raise device_error
            self._session = None
            self._terminal = None
            return {"update_id": update_id, "aborted": True}
        finally:
            self._leave_operation()

    def _validated_status(self, status):
        required = (
            "update_id",
            "state",
            "active_bank",
            "generation",
            "next_offset",
            "total_size",
            "error",
        )
        if not isinstance(status, dict):
            raise self._error("invalid_status")
        result = {}
        for name in required:
            value = status.get(name)
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise self._error("invalid_status")
            result[name] = value
        if (
            result["update_id"] > 0xFFFFFFFF
            or result["state"] > protocol_ids.RESOURCESTATE_FAILED
            or result["active_bank"] not in (0, 1, 0xFF)
            or result["generation"] > 0xFFFFFFFF
            or result["next_offset"] > 0xFFFFFFFF
            or result["total_size"] > self.MAX_PACKAGE_SIZE
            or result["next_offset"] > result["total_size"]
            or result["error"] > protocol_ids.RESOURCEERROR_INTERNAL
        ):
            raise self._error("invalid_status")
        return result

    async def status(self, update_id=0):
        self._enter_operation()
        try:
            reason = await self._maintain()
            if reason is not None:
                raise self._error(reason)
            if not self._valid_u32(update_id):
                raise self._error("invalid_request")
            if not self._connected():
                raise self._error("link_lost")
            raw_status = await self._device_call("get_resource_status", update_id)
            result = self._validated_status(raw_status)

            session = self._session
            if session is not None:
                if (
                    result["update_id"] != session["update_id"]
                    or result["error"] == protocol_ids.RESOURCEERROR_SESSION_MISMATCH
                ):
                    self._terminate("session_mismatch")
                    raise self._error("session_mismatch")
                if (
                    result["total_size"] != session["package_size"]
                    or result["next_offset"] > session["package_size"]
                ):
                    session["needs_sync"] = True
                    raise self._error("invalid_status")
                if result["state"] in self._ACTIVE_STATES:
                    session["next_offset"] = result["next_offset"]
                    session["needs_sync"] = False
                    session["beginning"] = False
                    session["aborting"] = False
                    if (
                        session["finishing"]
                        and not session["finish_acknowledged"]
                        and result["state"]
                        in (
                            protocol_ids.RESOURCESTATE_READY,
                            protocol_ids.RESOURCESTATE_RECEIVING,
                        )
                    ):
                        session["finishing"] = False
                    self._touch(session)
                else:
                    self._session = None
            return result
        finally:
            self._leave_operation()
