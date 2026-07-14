"""Async STM32 device gateway built on the SPI mailbox client."""

from firmware.esp32.core.compat import asyncio, sleep_ms, ticks_diff, ticks_ms
from firmware.esp32.transport.frame_codec import unpack_payload
from firmware.esp32.transport.spi_mailbox import MailboxClient
from protocol.generated import protocol_ids


class DeviceCommandError(RuntimeError):
    pass


SPI_POLL_INTERVAL_MS = 100


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
        }
        self._results = {}
        self._motion_results = {}
        self._listeners = []
        self._last_state_request = 0
        self._last_valid_rx = 0
        self._last_hello = (ticks_ms() - 1000) & 0xFFFFFFFF
        self._command_id = 0

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

    def _process_slot(self, slot):
        message_type = slot["type"]
        values = slot["values"]
        if message_type == protocol_ids.MSG_HELLO_RSP:
            self.connected = True
            self.status["connected"] = True
            self.status["stm_boot_id"] = values[0]
            self.status["state"] = values[5]
            if values[5] in (
                protocol_ids.MODE_IDLE,
                protocol_ids.MODE_MANUAL,
                protocol_ids.MODE_AI,
            ):
                self.status["selected_mode"] = values[5]
        elif message_type == protocol_ids.MSG_STATE_SNAPSHOT:
            self.status.update(
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
        elif message_type in (protocol_ids.MSG_ACK, protocol_ids.MSG_NACK):
            request_seq, command_id = values[:2]
            self._results[request_seq] = {
                "ok": message_type == protocol_ids.MSG_ACK,
                "command_id": command_id,
                "code": values[2],
            }
        elif message_type == protocol_ids.MSG_MOTION_DONE:
            self._motion_results[values[0]] = {"ok": True, "left_steps": values[1], "right_steps": values[2]}
        elif message_type == protocol_ids.MSG_MOTION_ABORTED:
            self._motion_results[values[0]] = {"ok": False, "reason": values[1]}
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
                self._process_slot(slot)

    def _mark_disconnected(self):
        if not self.connected:
            return
        self.connected = False
        self.status["connected"] = False
        self._emit({"type": "link", "connected": False, "status": self.status})

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
                raise DeviceCommandError("STM32 did not answer HELLO")
            await sleep_ms(20)

    async def command(self, message_type, values, command_id, timeout_ms=600):
        seq = self.mailbox.submit(message_type, values, command_id)
        started = ticks_ms()
        while seq not in self._results:
            if ticks_diff(ticks_ms(), started) >= timeout_ms:
                raise DeviceCommandError("STM32 command timed out")
            await sleep_ms(10)
        result = self._results.pop(seq)
        if not result["ok"]:
            raise DeviceCommandError("STM32 rejected command: {}".format(result["code"]))
        return result

    async def wait_motion(self, command_id, timeout_ms=2500):
        started = ticks_ms()
        while command_id not in self._motion_results:
            if ticks_diff(ticks_ms(), started) >= timeout_ms:
                raise DeviceCommandError("motion result timed out")
            await sleep_ms(10)
        return self._motion_results.pop(command_id)

    async def set_mode(self, mode):
        command_id = self.next_command_id()
        await self.command(protocol_ids.MSG_SET_MODE, (command_id, mode), command_id)
        return command_id

    async def move(self, left_steps, right_steps, rate, accel=600, timeout_ms=2000):
        command_id = self.next_command_id()
        await self.command(
            protocol_ids.MSG_MOVE_WHEELS,
            (command_id, left_steps, right_steps, rate, accel, timeout_ms),
            command_id,
        )
        return await self.wait_motion(command_id, timeout_ms + 600)

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
