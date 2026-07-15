"""Async STM32 device gateway built on the SPI mailbox client."""

from firmware.esp32.core.compat import asyncio, sleep_ms, ticks_diff, ticks_ms
from firmware.esp32.transport.frame_codec import unpack_payload
from firmware.esp32.transport.spi_mailbox import MailboxClient
from protocol.generated import protocol_ids


class DeviceCommandError(RuntimeError):
    def __init__(self, message, code=None, rejected=False, timed_out=False):
        RuntimeError.__init__(self, message)
        self.code = code
        self.rejected = bool(rejected)
        self.timed_out = bool(timed_out)


SPI_POLL_INTERVAL_MS = 100
RESOURCE_CHUNK_SIZE = 238
RESOURCE_MAX_PACKAGE_SIZE = 504 * 1024
IGNORED_RESULT_CAPACITY = 16
IGNORED_MOTION_RESULT_CAPACITY = 16


def _crc32(data):
    state = 0xFFFFFFFF
    for value in data:
        state ^= value
        for _bit in range(8):
            state = (state >> 1) ^ (0xEDB88320 if state & 1 else 0)
    return state ^ 0xFFFFFFFF


def _u32(value, label, allow_zero=True):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError("{} must be an integer".format(label))
    minimum = 0 if allow_zero else 1
    if not minimum <= value <= 0xFFFFFFFF:
        raise ValueError("{} is out of range".format(label))
    return value


class DeviceService:
    def __init__(self, link, boot_id):
        self.mailbox = MailboxClient(link.exchange, boot_id)
        self.connected = False
        self.running = False
        self.status = {
            "connected": False,
            "state": protocol_ids.ROBOTSTATE_BOOT,
            "selected_mode": protocol_ids.MODE_IDLE,
            "degraded_flags": 0,
            "fault_code": 0,
            "active_command_id": 0,
            "rx_errors": 0,
            "flash": None,
            "resource": None,
        }
        self._results = {}
        self._waiting_results = {}
        self._ignored_capacity = IGNORED_RESULT_CAPACITY
        self._ignored_results = {}
        self._motion_results = {}
        self._motion_waiters = {}
        self._ignored_motion_capacity = IGNORED_MOTION_RESULT_CAPACITY
        self._ignored_motion_results = {}
        self._listeners = []
        self._last_state_request = 0
        self._last_valid_rx = 0
        self._last_hello = (ticks_ms() - 1000) & 0xFFFFFFFF
        self._command_id = 0
        self._resource_status_serial = 0
        self._last_resource_status = None
        self._resource_status_results = {}
        self._resource_status_waiters = {}
        self._stm_boot_id = None
        self._stm_uptime_ms = None
        self._controller_session_generation = 0

    def add_listener(self, listener):
        self._listeners.append(listener)

    def _emit(self, event):
        for listener in tuple(self._listeners):
            try:
                listener(event)
            except Exception:
                pass

    def next_command_id(self):
        self._command_id = (self._command_id + 1) & 0xFFFFFFFF
        if self._command_id == 0:
            self._command_id = 1
        return self._command_id

    def _ignore_result(self, request_seq):
        if request_seq in self._ignored_results:
            return
        if len(self._ignored_results) >= self._ignored_capacity:
            oldest = next(iter(self._ignored_results))
            self._ignored_results.pop(oldest, None)
        self._ignored_results[request_seq] = True

    def _consume_ignored_result(self, request_seq):
        return self._ignored_results.pop(request_seq, None) is not None

    def _ignore_motion_result(self, command_id):
        if command_id in self._ignored_motion_results:
            return
        if len(self._ignored_motion_results) >= self._ignored_motion_capacity:
            oldest = next(iter(self._ignored_motion_results))
            self._ignored_motion_results.pop(oldest, None)
        self._ignored_motion_results[command_id] = True

    def _consume_ignored_motion_result(self, command_id):
        return self._ignored_motion_results.pop(command_id, None) is not None

    def _queue_best_effort_stop(self):
        command_id = self.next_command_id()
        return self.mailbox.submit_urgent(
            protocol_ids.MSG_STOP,
            (command_id, protocol_ids.ABORTREASON_STOP),
            command_id,
        )

    @staticmethod
    def _uptime_regressed(previous, current):
        if previous is None or previous == current:
            return False
        backward = (previous - current) & 0xFFFFFFFF
        return 0 < backward < 0x80000000

    def _reset_controller_session(self):
        self.mailbox.reset_session()
        self._results.clear()
        self._waiting_results.clear()
        self._ignored_results.clear()
        self._motion_results.clear()
        self._motion_waiters.clear()
        self._ignored_motion_results.clear()
        self._resource_status_results.clear()
        self._resource_status_waiters.clear()
        self._last_resource_status = None
        self._resource_status_serial += 1
        self._stm_boot_id = None
        self._stm_uptime_ms = None
        self._controller_session_generation += 1
        self._last_state_request = 0
        self._last_hello = (ticks_ms() - 1000) & 0xFFFFFFFF
        self.status.update(
            connected=False,
            stm_boot_id=None,
            stm_uptime_ms=None,
            state=protocol_ids.ROBOTSTATE_BOOT,
            selected_mode=protocol_ids.MODE_IDLE,
            degraded_flags=0,
            fault_code=0,
            active_command_id=0,
            flash=None,
            resource=None,
        )
        self._mark_disconnected(force=True)

    def _process_slot(self, slot, request_seq=None):
        message_type = slot["type"]
        values = slot["values"]
        if message_type == protocol_ids.MSG_HELLO_RSP:
            stm_boot_id = values[0]
            if self._stm_boot_id is not None and self._stm_boot_id != stm_boot_id:
                self._reset_controller_session()
            self._stm_boot_id = stm_boot_id
            self.connected = True
            self.status["connected"] = True
            self.status["stm_boot_id"] = stm_boot_id
            self.status["state"] = values[5]
            if values[5] in (
                protocol_ids.MODE_IDLE,
                protocol_ids.MODE_MANUAL,
                protocol_ids.MODE_AI,
            ):
                self.status["selected_mode"] = values[5]
        elif message_type == protocol_ids.MSG_STATE_SNAPSHOT:
            stm_boot_id = values[0]
            stm_uptime_ms = values[1]
            if not self.connected:
                return
            if (
                self._stm_boot_id is not None
                and (
                    self._stm_boot_id != stm_boot_id
                    or self._uptime_regressed(self._stm_uptime_ms, stm_uptime_ms)
                )
            ):
                self._reset_controller_session()
                return
            self._stm_boot_id = stm_boot_id
            self._stm_uptime_ms = stm_uptime_ms
            self.status.update(
                stm_boot_id=stm_boot_id,
                stm_uptime_ms=stm_uptime_ms,
                state=values[2],
                selected_mode=values[3],
                degraded_flags=values[4],
                fault_code=values[5],
                active_command_id=values[6],
                rx_errors=values[7],
            )
        elif message_type == protocol_ids.MSG_FLASH_INFO:
            self.status["flash"] = {
                "jedec_id": values[0],
                "capacity_bytes": values[1],
                "status": values[2],
            }
        elif message_type == protocol_ids.MSG_RESOURCE_STATUS:
            resource_status = {
                "update_id": values[0],
                "state": values[1],
                "active_bank": values[2],
                "generation": values[3],
                "next_offset": values[4],
                "total_size": values[5],
                "error": values[6],
            }
            self._resource_status_serial += 1
            self._last_resource_status = resource_status
            self.status["resource"] = resource_status
            if (
                request_seq is not None
                and request_seq in self._resource_status_waiters
                and (
                    self._resource_status_waiters[request_seq] == 0
                    or self._resource_status_waiters[request_seq]
                    == resource_status["update_id"]
                )
            ):
                self._resource_status_results[request_seq] = resource_status
        elif message_type in (protocol_ids.MSG_ACK, protocol_ids.MSG_NACK):
            ack_seq, command_id = values[:2]
            if self._consume_ignored_result(ack_seq):
                return
            if self._waiting_results.get(ack_seq) != command_id:
                return
            self._waiting_results.pop(ack_seq, None)
            self._results[ack_seq] = {
                "ok": message_type == protocol_ids.MSG_ACK,
                "command_id": command_id,
                "code": values[2],
            }
        elif message_type == protocol_ids.MSG_MOTION_DONE:
            command_id = values[0]
            if self._consume_ignored_motion_result(command_id):
                return
            if command_id not in self._motion_waiters:
                return
            self._motion_waiters.pop(command_id, None)
            self._motion_results[command_id] = {
                "ok": True,
                "left_steps": values[1],
                "right_steps": values[2],
            }
        elif message_type == protocol_ids.MSG_MOTION_ABORTED:
            command_id = values[0]
            if self._consume_ignored_motion_result(command_id):
                return
            if command_id not in self._motion_waiters:
                return
            self._motion_waiters.pop(command_id, None)
            self._motion_results[command_id] = {
                "ok": False,
                "reason": values[1],
            }
        elif message_type == protocol_ids.MSG_MODE_CHANGED:
            self.status["state"] = values[0]
            self.status["selected_mode"] = values[0]
        elif message_type == protocol_ids.MSG_FAULT_EVENT:
            self.status["fault_code"] = values[0]
        self._emit({"type": "device", "message_type": message_type, "values": values, "status": self.status})

    def _drain_events(self):
        while self.mailbox.events:
            event = self.mailbox.events.popleft()
            slot = event.get("slot")
            if slot:
                self._process_slot(slot, event.get("request_seq"))

    def _mark_disconnected(self, force=False):
        if not self.connected and not force:
            return
        self.connected = False
        self.status["connected"] = False
        self._emit(
            {
                "type": "link",
                "connected": False,
                "status": dict(self.status),
            }
        )

    async def run(self):
        self.running = True
        while self.running:
            now = ticks_ms()
            if (
                not self.connected
                and self.mailbox._pending is None
                and ticks_diff(now, self._last_hello) >= 1000
            ):
                self._last_hello = now
                self.mailbox.hello()
            decoded = self.mailbox.poll(now)
            self.status["rx_errors"] = self.mailbox.rx_errors
            self._drain_events()
            if decoded is not None and self.connected:
                self._last_valid_rx = now
            if self.connected and ticks_diff(now, self._last_valid_rx) > 750:
                self._mark_disconnected()
            if self.connected and ticks_diff(now, self._last_state_request) >= 1000 and self.mailbox._pending is None:
                self._last_state_request = now
                self.mailbox.request(protocol_ids.MSG_GET_STATE, (), protocol_ids.MSG_STATE_SNAPSHOT)
            # The STM32 may be flushing the software-I2C OLED before it can rearm SPI DMA.
            await sleep_ms(SPI_POLL_INTERVAL_MS)

    async def wait_connected(self, timeout_ms=2000):
        started = ticks_ms()
        while not self.connected:
            if ticks_diff(ticks_ms(), started) >= timeout_ms:
                raise DeviceCommandError(
                    "STM32 did not answer HELLO", timed_out=True
                )
            await sleep_ms(20)

    async def command(self, message_type, values, command_id, timeout_ms=600):
        seq = self.mailbox.submit(message_type, values, command_id)
        session_generation = self._controller_session_generation
        self._ignored_results.pop(seq, None)
        self._waiting_results[seq] = command_id & 0xFFFFFFFF
        started = ticks_ms()
        resolved = False
        try:
            while seq not in self._results:
                if session_generation != self._controller_session_generation:
                    raise DeviceCommandError(
                        "STM32 session changed", timed_out=True
                    )
                if ticks_diff(ticks_ms(), started) >= timeout_ms:
                    raise DeviceCommandError(
                        "STM32 command timed out", timed_out=True
                    )
                await sleep_ms(10)
            result = self._results.pop(seq)
            resolved = True
            if not result["ok"]:
                raise DeviceCommandError(
                    "STM32 rejected command: {}".format(result["code"]),
                    code=result["code"],
                    rejected=True,
                )
            return result
        finally:
            self._waiting_results.pop(seq, None)
            self._results.pop(seq, None)
            if not resolved:
                self.mailbox.cancel(seq)
                self._ignore_result(seq)

    async def wait_motion(self, command_id, timeout_ms=2500):
        session_generation = self._controller_session_generation
        self._motion_waiters[command_id] = True
        self._ignored_motion_results.pop(command_id, None)
        started = ticks_ms()
        resolved = False
        try:
            while command_id not in self._motion_results:
                if session_generation != self._controller_session_generation:
                    raise DeviceCommandError(
                        "STM32 session changed", timed_out=True
                    )
                if ticks_diff(ticks_ms(), started) >= timeout_ms:
                    raise DeviceCommandError(
                        "motion result timed out", timed_out=True
                    )
                await sleep_ms(10)
            result = self._motion_results.pop(command_id)
            resolved = True
            return result
        finally:
            self._motion_waiters.pop(command_id, None)
            self._motion_results.pop(command_id, None)
            if not resolved:
                self._ignore_motion_result(command_id)
                if session_generation == self._controller_session_generation:
                    self._queue_best_effort_stop()

    async def set_mode(self, mode):
        command_id = self.next_command_id()
        await self.command(protocol_ids.MSG_SET_MODE, (command_id, mode), command_id)
        return command_id

    async def move(self, left_steps, right_steps, rate, accel=600, timeout_ms=2000):
        command_id = self.next_command_id()
        session_generation = self._controller_session_generation
        self._ignored_motion_results.pop(command_id, None)
        self._motion_waiters[command_id] = True
        try:
            try:
                await self.command(
                    protocol_ids.MSG_MOVE_WHEELS,
                    (command_id, left_steps, right_steps, rate, accel, timeout_ms),
                    command_id,
                )
            except DeviceCommandError as error:
                if (
                    error.timed_out
                    and session_generation == self._controller_session_generation
                ):
                    self._queue_best_effort_stop()
                raise
            except asyncio.CancelledError:
                if session_generation == self._controller_session_generation:
                    self._queue_best_effort_stop()
                raise

            return await self.wait_motion(command_id, timeout_ms + 600)
        finally:
            if (
                command_id in self._motion_waiters
                or command_id in self._motion_results
            ):
                self._motion_waiters.pop(command_id, None)
                self._motion_results.pop(command_id, None)
                self._ignore_motion_result(command_id)

    async def stop(self):
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_STOP,
            (command_id, protocol_ids.ABORTREASON_STOP),
            command_id,
        )
        return command_id

    async def set_expression(self, expression):
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_SET_EXPRESSION,
            (command_id, expression),
            command_id,
        )
        return command_id

    async def clear_estop(self):
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_CLEAR_ESTOP,
            (command_id,),
            command_id,
        )
        self.status.update(
            state=protocol_ids.ROBOTSTATE_IDLE,
            selected_mode=protocol_ids.MODE_IDLE,
            fault_code=0,
            active_command_id=0,
        )
        return command_id

    async def set_runtime_config(self, rate, accel, hold_ms):
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_SET_RUNTIME_CONFIG,
            (command_id, rate, accel, hold_ms, 0),
            command_id,
        )
        return command_id

    async def resource_begin(
        self, update_id, package_size, package_crc32, format_version=1
    ):
        update_id = _u32(update_id, "update id", allow_zero=False)
        package_size = _u32(package_size, "package size", allow_zero=False)
        package_crc32 = _u32(package_crc32, "package CRC32")
        if package_size > RESOURCE_MAX_PACKAGE_SIZE:
            raise ValueError("package size exceeds the resource limit")
        if format_version != 1:
            raise ValueError("unsupported resource format version")
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_RESOURCE_BEGIN,
            (command_id, update_id, package_size, package_crc32, format_version),
            command_id,
            timeout_ms=1200,
        )
        return command_id

    async def resource_chunk(self, update_id, offset, data):
        update_id = _u32(update_id, "update id", allow_zero=False)
        offset = _u32(offset, "resource offset")
        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise ValueError("resource chunk must contain bytes")
        data = bytes(data)
        if not 1 <= len(data) <= RESOURCE_CHUNK_SIZE:
            raise ValueError("resource chunk must contain 1..238 bytes")
        if len(data) > 0xFFFFFFFF - offset:
            raise ValueError("resource chunk range is out of range")
        padded = data + bytes(RESOURCE_CHUNK_SIZE - len(data))
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_RESOURCE_CHUNK,
            (command_id, update_id, offset, len(data), _crc32(data), padded),
            command_id,
            timeout_ms=1200,
        )
        return command_id

    async def resource_finish(self, update_id):
        update_id = _u32(update_id, "update id", allow_zero=False)
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_RESOURCE_FINISH,
            (command_id, update_id),
            command_id,
            timeout_ms=1200,
        )
        return command_id

    async def resource_abort(self, update_id):
        update_id = _u32(update_id, "update id", allow_zero=False)
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_RESOURCE_ABORT,
            (command_id, update_id),
            command_id,
            timeout_ms=1200,
        )
        return command_id

    async def get_resource_status(self, update_id=0, timeout_ms=1200):
        update_id = _u32(update_id, "update id")
        if not self.connected:
            raise DeviceCommandError("STM32 is not connected")
        seq = self.mailbox.request(
            protocol_ids.MSG_GET_RESOURCE_STATUS,
            (update_id,),
            protocol_ids.MSG_RESOURCE_STATUS,
        )
        self._resource_status_results.pop(seq, None)
        self._resource_status_waiters[seq] = update_id
        session_generation = self._controller_session_generation
        started = ticks_ms()
        try:
            while seq not in self._resource_status_results:
                if session_generation != self._controller_session_generation:
                    raise DeviceCommandError(
                        "STM32 session changed", timed_out=True
                    )
                if ticks_diff(ticks_ms(), started) >= timeout_ms:
                    raise DeviceCommandError(
                        "STM32 resource status timed out", timed_out=True
                    )
                await sleep_ms(10)
            return dict(self._resource_status_results[seq])
        finally:
            self.mailbox.cancel(seq)
            self._resource_status_results.pop(seq, None)
            self._resource_status_waiters.pop(seq, None)
