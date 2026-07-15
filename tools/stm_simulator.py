#!/usr/bin/env python3
"""STM32 protocol/state simulator using fixed SPI mailbox slots over TCP."""

import argparse
import binascii
import socket
import sys
import time
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from firmware.esp32.transport.frame_codec import SlotError, decode_slot, encode_slot, pack_payload, unpack_payload
from protocol.generated import protocol_ids
from tools import resource_format


class RobotSimulator:
    HEARTBEAT_TIMEOUT_MS = 750
    RESOURCE_TIMEOUT_MS = 60000
    RESOURCE_MAX_PACKAGE_SIZE = 504 * 1024
    RESOURCE_FORMAT_VERSION = 1
    RESOURCE_BANK_NONE = 0xFF
    RESOURCE_VERIFY_DELAY_MS = 250
    RESOURCE_COMMIT_DELAY_MS = 200
    OUTGOING_CAPACITY = 32
    DEDUP_CAPACITY = 64

    def __init__(self, boot_id=0x53544D31):
        self.boot_id = boot_id
        self.state = protocol_ids.ROBOTSTATE_IDLE
        self.selected_mode = protocol_ids.MODE_IDLE
        self.session = False
        self.esp_boot_id = None
        self.last_heartbeat_ms = 0
        self.now_ms = 0
        self.rx_errors = 0
        self.fault_code = 0
        self.degraded_flags = 0
        self.active = None
        self._tx_seq = 0
        self._outgoing = deque()
        self._dedup = {}
        self.resource_update = None
        self.resource_update_id = 0
        self.resource_state = protocol_ids.RESOURCESTATE_IDLE
        self.resource_error = protocol_ids.RESOURCEERROR_NONE
        self.resource_active_bank = self.RESOURCE_BANK_NONE
        self.resource_generation = 0
        self.resource_next_offset = 0
        self.resource_total_size = 0

    def _next_seq(self):
        self._tx_seq = (self._tx_seq + 1) & 0xFFFF
        if self._tx_seq == 0:
            self._tx_seq = 1
        return self._tx_seq

    def _slot(self, message_type, values=(), flags=0):
        payload = pack_payload(message_type, values)
        return encode_slot(message_type, self._next_seq(), flags, payload)

    def _queue(self, message_type, values=(), flags=None, priority=False):
        if flags is None:
            flags = protocol_ids.SLOTFLAGS_EVENT
        slot = self._slot(message_type, values, flags)
        if priority:
            if len(self._outgoing) >= self.OUTGOING_CAPACITY:
                self._outgoing.pop()
            self._outgoing.appendleft(slot)
        elif len(self._outgoing) < self.OUTGOING_CAPACITY:
            self._outgoing.append(slot)
        return slot

    def _remember_dedup(self, key, response):
        if key in self._dedup:
            self._dedup.pop(key)
        elif len(self._dedup) >= self.DEDUP_CAPACITY:
            self._dedup.pop(next(iter(self._dedup)))
        self._dedup[key] = response

    def _noop(self):
        return self._slot(protocol_ids.MSG_NOOP)

    def _ack(self, request_seq, command_id):
        return self._queue(
            protocol_ids.MSG_ACK,
            (request_seq, command_id, protocol_ids.ERRORCODE_OK),
            protocol_ids.SLOTFLAGS_RESPONSE,
            priority=True,
        )

    def _nack(self, request_seq, command_id, error):
        return self._queue(
            protocol_ids.MSG_NACK,
            (request_seq, command_id, error),
            protocol_ids.SLOTFLAGS_RESPONSE,
            priority=True,
        )

    def _snapshot_values(self):
        command_id = self.active["command_id"] if self.active else 0
        return (
            self.boot_id,
            self.now_ms & 0xFFFFFFFF,
            self.state,
            self.selected_mode,
            self.degraded_flags,
            self.fault_code,
            command_id,
            self.rx_errors,
            min(len(self._outgoing), 255),
            0,
        )

    def _abort_active(self, reason):
        if not self.active:
            return
        active = self.active
        self.active = None
        if active["kind"] == "coil":
            self._queue(
                protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT,
                (
                    active["command_id"],
                    protocol_ids.COILDIAGNOSTICRESULT_ABORTED,
                ),
                priority=True,
            )
        else:
            self._queue(
                protocol_ids.MSG_MOTION_ABORTED,
                (active["command_id"], reason),
                priority=True,
            )

    def _resource_fail(self, error, state=protocol_ids.RESOURCESTATE_FAILED):
        if self.resource_update is None:
            return
        self.resource_update_id = self.resource_update["update_id"]
        self.resource_next_offset = self.resource_update["next_offset"]
        self.resource_total_size = self.resource_update["package_size"]
        self.resource_update = None
        self.resource_state = state
        self.resource_error = error

    def _resource_reject(self, request_seq, command_id, error, protocol_error):
        self.resource_error = error
        return self._nack(request_seq, command_id, protocol_error)

    def _resource_status_values(self, requested_update_id):
        update_id = self.resource_update_id
        error = self.resource_error
        if self.resource_update is not None:
            update_id = self.resource_update["update_id"]
        if requested_update_id not in (0, update_id):
            error = protocol_ids.RESOURCEERROR_SESSION_MISMATCH
        return (
            update_id,
            self.resource_state,
            self.resource_active_bank,
            self.resource_generation,
            self.resource_next_offset,
            self.resource_total_size,
            error,
        )

    def _handle_resource_begin(self, seq, values):
        command_id, update_id, package_size, package_crc32, format_version = values
        if self.resource_update is not None:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_UPDATE_BUSY,
                protocol_ids.ERRORCODE_BAD_STATE,
            )
        if self.state not in (protocol_ids.ROBOTSTATE_IDLE, protocol_ids.ROBOTSTATE_ESTOP) or self.active is not None:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_STATE,
                protocol_ids.ERRORCODE_BAD_STATE,
            )
        self.resource_update_id = update_id
        self.resource_next_offset = 0
        self.resource_total_size = package_size
        if update_id == 0 or format_version != self.RESOURCE_FORMAT_VERSION:
            self.resource_state = protocol_ids.RESOURCESTATE_IDLE
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_FORMAT_VERSION,
                protocol_ids.ERRORCODE_BAD_PAYLOAD,
            )
        if (
            package_size < resource_format.HEADER_STRUCT.size
            or package_size > self.RESOURCE_MAX_PACKAGE_SIZE
        ):
            self.resource_state = protocol_ids.RESOURCESTATE_IDLE
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_PACKAGE_SIZE,
                protocol_ids.ERRORCODE_OUT_OF_RANGE,
            )
        try:
            buffer = bytearray(package_size)
        except MemoryError:
            self.resource_state = protocol_ids.RESOURCESTATE_FAILED
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_INTERNAL,
                protocol_ids.ERRORCODE_INTERNAL,
            )
        self.resource_update = {
            "update_id": update_id,
            "package_size": package_size,
            "package_crc32": package_crc32,
            "format_version": format_version,
            "buffer": buffer,
            "next_offset": 0,
            "last_chunk": None,
            "last_activity_ms": self.now_ms,
        }
        self.resource_state = protocol_ids.RESOURCESTATE_READY
        self.resource_error = protocol_ids.RESOURCEERROR_NONE
        return self._ack(seq, command_id)

    def _handle_resource_chunk(self, seq, values):
        command_id, update_id, offset, data_length, chunk_crc32, fixed_data = values
        update = self.resource_update
        if update is None or update_id != update["update_id"]:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_SESSION_MISMATCH,
                protocol_ids.ERRORCODE_BAD_STATE,
            )
        if self.resource_state not in (
            protocol_ids.RESOURCESTATE_READY,
            protocol_ids.RESOURCESTATE_RECEIVING,
        ):
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_STATE,
                protocol_ids.ERRORCODE_BAD_STATE,
            )
        update["last_activity_ms"] = self.now_ms
        if data_length == 0 or data_length > len(fixed_data):
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_LENGTH,
                protocol_ids.ERRORCODE_OUT_OF_RANGE,
            )
        if any(fixed_data[data_length:]):
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_LENGTH,
                protocol_ids.ERRORCODE_BAD_PAYLOAD,
            )
        data = bytes(fixed_data[:data_length])
        if (binascii.crc32(data) & 0xFFFFFFFF) != chunk_crc32:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_CHUNK_CRC,
                protocol_ids.ERRORCODE_BAD_PAYLOAD,
            )

        last_chunk = update["last_chunk"]
        if offset != update["next_offset"]:
            if last_chunk is not None and last_chunk == (offset, data_length, chunk_crc32, data):
                self.resource_error = protocol_ids.RESOURCEERROR_NONE
                return self._ack(seq, command_id)
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_OFFSET,
                protocol_ids.ERRORCODE_OUT_OF_RANGE,
            )
        end = offset + data_length
        if end > update["package_size"]:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_LENGTH,
                protocol_ids.ERRORCODE_OUT_OF_RANGE,
            )
        update["buffer"][offset:end] = data
        update["next_offset"] = end
        update["last_chunk"] = (offset, data_length, chunk_crc32, data)
        self.resource_next_offset = end
        self.resource_state = protocol_ids.RESOURCESTATE_RECEIVING
        self.resource_error = protocol_ids.RESOURCEERROR_NONE
        return self._ack(seq, command_id)

    def _handle_resource_finish(self, seq, values):
        command_id, update_id = values
        update = self.resource_update
        if update is None or update_id != update["update_id"]:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_SESSION_MISMATCH,
                protocol_ids.ERRORCODE_BAD_STATE,
            )
        update["last_activity_ms"] = self.now_ms
        if update["next_offset"] != update["package_size"]:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_LENGTH,
                protocol_ids.ERRORCODE_BAD_STATE,
            )
        self.resource_state = protocol_ids.RESOURCESTATE_VERIFYING
        self.resource_error = protocol_ids.RESOURCEERROR_NONE
        update["phase_started_ms"] = self.now_ms
        return self._ack(seq, command_id)

    def _handle_resource_abort(self, seq, values):
        command_id, update_id = values
        update = self.resource_update
        if update is None or update_id != update["update_id"]:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_SESSION_MISMATCH,
                protocol_ids.ERRORCODE_BAD_STATE,
            )
        if self.resource_state == protocol_ids.RESOURCESTATE_COMMITTING:
            return self._resource_reject(
                seq,
                command_id,
                protocol_ids.RESOURCEERROR_BAD_STATE,
                protocol_ids.ERRORCODE_BAD_STATE,
            )
        self._resource_fail(protocol_ids.RESOURCEERROR_NONE, protocol_ids.RESOURCESTATE_ABORTED)
        return self._ack(seq, command_id)

    def _tick(self, now_ms):
        self.now_ms = now_ms & 0xFFFFFFFF
        if self.active:
            active = self.active
            if active["kind"] == "coil":
                finished = (
                    (self.now_ms - active["started_ms"]) & 0xFFFFFFFF
                ) >= active["duration_ms"]
            else:
                finished = now_ms >= active["finish_ms"]
            if finished:
                self.active = None
                if active["kind"] == "coil":
                    self._queue(
                        protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT,
                        (
                            active["command_id"],
                            protocol_ids.COILDIAGNOSTICRESULT_DONE,
                        ),
                    )
                else:
                    self._queue(
                        protocol_ids.MSG_MOTION_DONE,
                        (
                            active["command_id"],
                            active["left_steps"],
                            active["right_steps"],
                            0,
                        ),
                    )
        if self.session and (now_ms - self.last_heartbeat_ms) > self.HEARTBEAT_TIMEOUT_MS:
            self.session = False
            self.state = protocol_ids.ROBOTSTATE_ESTOP
            self.selected_mode = protocol_ids.MODE_IDLE
            self.fault_code = protocol_ids.ERRORCODE_LINK_LOST
            self._abort_active(protocol_ids.ABORTREASON_LINK_LOST)
            if self.resource_state != protocol_ids.RESOURCESTATE_COMMITTING:
                self._resource_fail(protocol_ids.RESOURCEERROR_LINK_LOST)
            self._queue(protocol_ids.MSG_FAULT_EVENT, (protocol_ids.ERRORCODE_LINK_LOST, self.HEARTBEAT_TIMEOUT_MS), priority=True)
        elif (
            self.resource_update is not None
            and not self.session
            and self.resource_state != protocol_ids.RESOURCESTATE_COMMITTING
        ):
            self._resource_fail(protocol_ids.RESOURCEERROR_LINK_LOST)
        elif (
            self.resource_update is not None
            and self.resource_state != protocol_ids.RESOURCESTATE_COMMITTING
            and (now_ms - self.resource_update["last_activity_ms"]) > self.RESOURCE_TIMEOUT_MS
        ):
            self._resource_fail(protocol_ids.RESOURCEERROR_BUSY_TIMEOUT)

        update = self.resource_update
        if update is None:
            return
        phase_elapsed = (
            self.now_ms - update.get("phase_started_ms", self.now_ms)
        ) & 0xFFFFFFFF
        if (
            self.resource_state == protocol_ids.RESOURCESTATE_VERIFYING
            and phase_elapsed >= self.RESOURCE_VERIFY_DELAY_MS
        ):
            try:
                package = resource_format.verify_package(bytes(update["buffer"]))
            except (resource_format.ResourceFormatError, TypeError, ValueError):
                self._resource_fail(protocol_ids.RESOURCEERROR_BAD_DIRECTORY)
                return
            if package.package_crc32 != update["package_crc32"]:
                self._resource_fail(protocol_ids.RESOURCEERROR_PACKAGE_CRC)
                return
            self.resource_state = protocol_ids.RESOURCESTATE_COMMITTING
            update["phase_started_ms"] = self.now_ms
        elif (
            self.resource_state == protocol_ids.RESOURCESTATE_COMMITTING
            and phase_elapsed >= self.RESOURCE_COMMIT_DELAY_MS
        ):
            if self.resource_active_bank == self.RESOURCE_BANK_NONE:
                self.resource_active_bank = 0
            else:
                self.resource_active_bank ^= 1
            self.resource_generation = (self.resource_generation + 1) & 0xFFFFFFFF
            self.resource_update_id = update["update_id"]
            self.resource_next_offset = update["next_offset"]
            self.resource_total_size = update["package_size"]
            self.resource_update = None
            self.resource_state = protocol_ids.RESOURCESTATE_IDLE
            self.resource_error = protocol_ids.RESOURCEERROR_NONE

    def _command_id(self, decoded):
        values = unpack_payload(decoded["type"], decoded["payload"])
        return values[0] if values else 0, values

    def _handle_command(self, decoded):
        message_type = decoded["type"]
        seq = decoded["seq"]

        if message_type == protocol_ids.MSG_NOOP:
            return
        if message_type == protocol_ids.MSG_HELLO_REQ:
            esp_boot_id, _capabilities = unpack_payload(message_type, decoded["payload"])
            if self.esp_boot_id is not None and esp_boot_id != self.esp_boot_id:
                self._resource_fail(protocol_ids.RESOURCEERROR_LINK_LOST)
                self._abort_active(protocol_ids.ABORTREASON_LINK_LOST)
                self._dedup.clear()
                self._outgoing.clear()
            self.esp_boot_id = esp_boot_id
            self.session = True
            self.last_heartbeat_ms = self.now_ms
            self._queue(
                protocol_ids.MSG_HELLO_RSP,
                (self.boot_id, 0x3F, 0, 1, 0, self.state),
                protocol_ids.SLOTFLAGS_RESPONSE,
                priority=True,
            )
            return
        if message_type == protocol_ids.MSG_HEARTBEAT:
            if self.session:
                self.last_heartbeat_ms = self.now_ms
            return
        if message_type == protocol_ids.MSG_GET_STATE:
            self._queue(protocol_ids.MSG_STATE_SNAPSHOT, self._snapshot_values(), protocol_ids.SLOTFLAGS_RESPONSE)
            return
        if message_type == protocol_ids.MSG_GET_RESOURCE_STATUS:
            (update_id,) = unpack_payload(message_type, decoded["payload"])
            if self.resource_update is not None and update_id in (0, self.resource_update["update_id"]):
                self.resource_update["last_activity_ms"] = self.now_ms
            self._queue(
                protocol_ids.MSG_RESOURCE_STATUS,
                self._resource_status_values(update_id),
                protocol_ids.SLOTFLAGS_RESPONSE,
            )
            return

        command_id, values = self._command_id(decoded)
        dedup_key = (message_type, command_id)
        if message_type == protocol_ids.MSG_RESOURCE_CHUNK:
            self._handle_resource_chunk(seq, values)
            return
        if dedup_key in self._dedup:
            if len(self._outgoing) >= self.OUTGOING_CAPACITY:
                self._outgoing.pop()
            self._outgoing.appendleft(self._dedup[dedup_key])
            return
        if message_type == protocol_ids.MSG_RESOURCE_BEGIN:
            response = self._handle_resource_begin(seq, values)
            self._remember_dedup(dedup_key, response)
            return
        if message_type == protocol_ids.MSG_RESOURCE_FINISH:
            response = self._handle_resource_finish(seq, values)
            self._remember_dedup(dedup_key, response)
            return
        if message_type == protocol_ids.MSG_RESOURCE_ABORT:
            response = self._handle_resource_abort(seq, values)
            self._remember_dedup(dedup_key, response)
            return
        if message_type == protocol_ids.MSG_SET_MODE:
            _command_id, mode = values
            if self.state in (protocol_ids.ROBOTSTATE_ESTOP, protocol_ids.ROBOTSTATE_FAULT):
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_STATE)
            elif mode not in (protocol_ids.MODE_IDLE, protocol_ids.MODE_MANUAL, protocol_ids.MODE_AI):
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_PAYLOAD)
            elif self.resource_update is not None and mode != protocol_ids.MODE_IDLE:
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_STATE)
            else:
                self._abort_active(protocol_ids.ABORTREASON_MODE_CHANGE)
                self.selected_mode = mode
                self.state = mode
                response = self._ack(seq, command_id)
                self._queue(protocol_ids.MSG_MODE_CHANGED, (mode, 1))
            self._remember_dedup(dedup_key, response)
            return

        if message_type == protocol_ids.MSG_MOVE_WHEELS:
            _command_id, left_steps, right_steps, rate, _accel, timeout_ms = values
            if self.resource_update is not None:
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_STATE)
            elif self.state not in (protocol_ids.ROBOTSTATE_MANUAL, protocol_ids.ROBOTSTATE_AI):
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_STATE)
            elif self.active is not None:
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_QUEUE_FULL)
            elif rate == 0 or rate > 800 or timeout_ms == 0 or timeout_ms > 2000:
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_OUT_OF_RANGE)
            else:
                duration = min(timeout_ms, max(1, (max(abs(left_steps), abs(right_steps)) * 1000 + rate - 1) // rate))
                self.active = {
                    "kind": "motion",
                    "command_id": command_id,
                    "left_steps": left_steps,
                    "right_steps": right_steps,
                    "finish_ms": self.now_ms + duration,
                }
                response = self._ack(seq, command_id)
                self._queue(protocol_ids.MSG_MOTION_STARTED, (command_id,))
            self._remember_dedup(dedup_key, response)
            return

        if message_type == protocol_ids.MSG_COIL_DIAGNOSTIC:
            _command_id, wheel, channel, duration_ms = values
            if self.resource_update is not None:
                response = self._nack(
                    seq, command_id, protocol_ids.ERRORCODE_BAD_STATE
                )
            elif self.state != protocol_ids.ROBOTSTATE_MANUAL:
                response = self._nack(
                    seq, command_id, protocol_ids.ERRORCODE_BAD_STATE
                )
            elif self.active is not None:
                response = self._nack(
                    seq, command_id, protocol_ids.ERRORCODE_QUEUE_FULL
                )
            elif (
                wheel not in (
                    protocol_ids.COILWHEEL_LEFT,
                    protocol_ids.COILWHEEL_RIGHT,
                )
                or channel < protocol_ids.COILCHANNEL_A
                or channel > protocol_ids.COILCHANNEL_D
                or duration_ms < 100
                or duration_ms > 3000
            ):
                response = self._nack(
                    seq, command_id, protocol_ids.ERRORCODE_OUT_OF_RANGE
                )
            else:
                self.active = {
                    "kind": "coil",
                    "command_id": command_id,
                    "wheel": wheel,
                    "channel": channel,
                    "started_ms": self.now_ms,
                    "duration_ms": duration_ms,
                }
                response = self._ack(seq, command_id)
            self._remember_dedup(dedup_key, response)
            return

        if message_type == protocol_ids.MSG_STOP:
            self._abort_active(protocol_ids.ABORTREASON_STOP)
            self.state = protocol_ids.ROBOTSTATE_ESTOP
            self.selected_mode = protocol_ids.MODE_IDLE
            self.fault_code = 11
            response = self._ack(seq, command_id)
            self._remember_dedup(dedup_key, response)
            return

        if message_type == protocol_ids.MSG_SET_EXPRESSION:
            if self.state in (protocol_ids.ROBOTSTATE_ESTOP, protocol_ids.ROBOTSTATE_FAULT):
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_STATE)
            elif values[1] > protocol_ids.EXPRESSION_SLEEPY:
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_PAYLOAD)
            else:
                response = self._ack(seq, command_id)
            self._remember_dedup(dedup_key, response)
            return

        if message_type == protocol_ids.MSG_CLEAR_ESTOP:
            if self.state != protocol_ids.ROBOTSTATE_ESTOP or self.active is not None:
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_STATE)
            else:
                self.state = protocol_ids.ROBOTSTATE_IDLE
                self.selected_mode = protocol_ids.MODE_IDLE
                self.fault_code = 0
                response = self._ack(seq, command_id)
                self._queue(protocol_ids.MSG_MODE_CHANGED, (protocol_ids.MODE_IDLE, 3))
            self._remember_dedup(dedup_key, response)
            return

        if message_type == protocol_ids.MSG_SET_RUNTIME_CONFIG:
            response = self._ack(seq, command_id)
            self._remember_dedup(dedup_key, response)
            return

        response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_MESSAGE)
        self._remember_dedup(dedup_key, response)

    def transact(self, tx_slot, now_ms=None):
        if now_ms is None:
            now_ms = int(time.monotonic() * 1000)
        self._tick(now_ms)
        response = self._outgoing.popleft() if self._outgoing else self._noop()
        try:
            decoded = decode_slot(tx_slot)
        except SlotError:
            self.rx_errors += 1
            return response
        self.session = True
        self.last_heartbeat_ms = self.now_ms
        self._handle_command(decoded)
        return response


def recv_exact(connection, length):
    chunks = bytearray()
    while len(chunks) < length:
        chunk = connection.recv(length - len(chunks))
        if not chunk:
            return None
        chunks.extend(chunk)
    return bytes(chunks)


def serve_tcp(host, port):
    simulator = RobotSimulator()
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(1)
        print("STM simulator listening on {}:{}".format(host, port), flush=True)
        while True:
            connection, address = server.accept()
            print("client connected: {}:{}".format(*address), flush=True)
            with connection:
                while True:
                    tx = recv_exact(connection, protocol_ids.SLOT_SIZE)
                    if tx is None:
                        break
                    connection.sendall(simulator.transact(tx))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tcp", default="127.0.0.1:9001", help="HOST:PORT")
    args = parser.parse_args()
    host, port_text = args.tcp.rsplit(":", 1)
    serve_tcp(host, int(port_text))


if __name__ == "__main__":
    main()
