#!/usr/bin/env python3
"""STM32 protocol/state simulator using fixed SPI mailbox slots over TCP."""

import argparse
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


class RobotSimulator:
    HEARTBEAT_TIMEOUT_MS = 750

    def __init__(self, boot_id=0x53544D31):
        self.boot_id = boot_id
        self.state = protocol_ids.ROBOTSTATE_IDLE
        self.selected_mode = protocol_ids.MODE_IDLE
        self.session = False
        self.last_heartbeat_ms = 0
        self.now_ms = 0
        self.rx_errors = 0
        self.fault_code = 0
        self.degraded_flags = 0
        self.active = None
        self._tx_seq = 0
        self._outgoing = deque()
        self._dedup = {}

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
            self._outgoing.appendleft(slot)
        else:
            self._outgoing.append(slot)
        return slot

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

    def _abort_motion(self, reason):
        if self.active:
            command_id = self.active["command_id"]
            self.active = None
            self._queue(protocol_ids.MSG_MOTION_ABORTED, (command_id, reason), priority=True)

    def _tick(self, now_ms):
        self.now_ms = now_ms & 0xFFFFFFFF
        if self.active and now_ms >= self.active["finish_ms"]:
            motion = self.active
            self.active = None
            self._queue(
                protocol_ids.MSG_MOTION_DONE,
                (motion["command_id"], motion["left_steps"], motion["right_steps"], 0),
            )
        if self.session and (now_ms - self.last_heartbeat_ms) > self.HEARTBEAT_TIMEOUT_MS:
            self.session = False
            self.state = protocol_ids.ROBOTSTATE_ESTOP
            self.selected_mode = protocol_ids.MODE_IDLE
            self._abort_motion(protocol_ids.ABORTREASON_LINK_LOST)
            self._queue(protocol_ids.MSG_FAULT_EVENT, (protocol_ids.ERRORCODE_LINK_LOST, self.HEARTBEAT_TIMEOUT_MS), priority=True)

    def _command_id(self, decoded):
        values = unpack_payload(decoded["type"], decoded["payload"])
        return values[0] if values else 0, values

    def _handle_command(self, decoded):
        message_type = decoded["type"]
        seq = decoded["seq"]

        if message_type == protocol_ids.MSG_NOOP:
            return
        if message_type == protocol_ids.MSG_HELLO_REQ:
            _esp_boot_id, _capabilities = unpack_payload(message_type, decoded["payload"])
            self.session = True
            self.last_heartbeat_ms = self.now_ms
            self._queue(
                protocol_ids.MSG_HELLO_RSP,
                (self.boot_id, 0x1F, 0, 1, 0, self.state),
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

        command_id, values = self._command_id(decoded)
        dedup_key = (message_type, command_id)
        if dedup_key in self._dedup:
            self._outgoing.appendleft(self._dedup[dedup_key])
            return
        if not self.session and message_type != protocol_ids.MSG_STOP:
            response = self._nack(seq, command_id, protocol_ids.ERRORCODE_LINK_LOST)
            self._dedup[dedup_key] = response
            return

        if message_type == protocol_ids.MSG_SET_MODE:
            _command_id, mode = values
            if mode not in (protocol_ids.MODE_IDLE, protocol_ids.MODE_MANUAL, protocol_ids.MODE_AI):
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_PAYLOAD)
            else:
                self._abort_motion(protocol_ids.ABORTREASON_MODE_CHANGE)
                self.selected_mode = mode
                self.state = mode
                response = self._ack(seq, command_id)
                self._queue(protocol_ids.MSG_MODE_CHANGED, (mode, 1))
            self._dedup[dedup_key] = response
            return

        if message_type == protocol_ids.MSG_MOVE_WHEELS:
            _command_id, left_steps, right_steps, rate, _accel, timeout_ms = values
            if self.state not in (protocol_ids.ROBOTSTATE_MANUAL, protocol_ids.ROBOTSTATE_AI):
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_STATE)
            elif self.active is not None:
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_QUEUE_FULL)
            elif rate == 0 or rate > 800 or timeout_ms == 0 or timeout_ms > 2000:
                response = self._nack(seq, command_id, protocol_ids.ERRORCODE_OUT_OF_RANGE)
            else:
                duration = min(timeout_ms, max(1, (max(abs(left_steps), abs(right_steps)) * 1000 + rate - 1) // rate))
                self.active = {
                    "command_id": command_id,
                    "left_steps": left_steps,
                    "right_steps": right_steps,
                    "finish_ms": self.now_ms + duration,
                }
                response = self._ack(seq, command_id)
                self._queue(protocol_ids.MSG_MOTION_STARTED, (command_id,))
            self._dedup[dedup_key] = response
            return

        if message_type == protocol_ids.MSG_STOP:
            self._abort_motion(protocol_ids.ABORTREASON_STOP)
            self.state = protocol_ids.ROBOTSTATE_ESTOP
            self.selected_mode = protocol_ids.MODE_IDLE
            response = self._ack(seq, command_id)
            self._dedup[dedup_key] = response
            return

        if message_type in (protocol_ids.MSG_SET_EXPRESSION, protocol_ids.MSG_SET_RUNTIME_CONFIG):
            response = self._ack(seq, command_id)
            self._dedup[dedup_key] = response
            return

        response = self._nack(seq, command_id, protocol_ids.ERRORCODE_BAD_MESSAGE)
        self._dedup[dedup_key] = response

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
