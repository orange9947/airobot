"""Polling SPI mailbox client with bounded retries and heartbeats."""

from collections import deque

from protocol.generated import protocol_ids

from .frame_codec import SlotError, decode_slot, encode_slot, pack_payload, unpack_payload


def _elapsed(now_ms, then_ms):
    return (now_ms - then_ms) & 0xFFFFFFFF


class MailboxClient:
    def __init__(self, exchange, boot_id, heartbeat_ms=250, ack_timeout_ms=250, max_retries=3):
        self._exchange = exchange
        self.boot_id = boot_id & 0xFFFFFFFF
        self.heartbeat_ms = heartbeat_ms
        self.ack_timeout_ms = ack_timeout_ms
        self.max_retries = max_retries
        self._seq = 0
        self._queue_capacity = 8
        self._queue = deque((), self._queue_capacity)
        self._pending = None
        self._urgent = None
        self._last_heartbeat = 0
        self.rx_errors = 0
        self.events = deque((), 16)

    def _next_seq(self):
        self._seq = (self._seq + 1) & 0xFFFF
        if self._seq == 0:
            self._seq = 1
        return self._seq

    def _new_item(
        self, message_type, values, command_id, flags, expected_type, urgent=False
    ):
        if flags is None:
            flags = protocol_ids.SLOTFLAGS_ACK_REQUEST
        return {
            "type": message_type,
            "seq": self._next_seq(),
            "flags": flags,
            "payload": pack_payload(message_type, values),
            "command_id": command_id & 0xFFFFFFFF,
            "sent_ms": None,
            "retries": 0,
            "expected_type": expected_type,
            "urgent": bool(urgent),
        }

    def submit(self, message_type, values, command_id=0, flags=None, expected_type=None):
        if len(self._queue) >= self._queue_capacity:
            raise RuntimeError("mailbox command queue full")
        item = self._new_item(
            message_type, values, command_id, flags, expected_type
        )
        self._queue.append(item)
        return item["seq"]

    def submit_urgent(self, message_type, values, command_id=0, flags=None):
        if self._urgent is not None:
            if self._urgent["type"] != message_type:
                raise RuntimeError("mailbox urgent slot is occupied")
            return self._urgent["seq"]
        if self._pending is not None and self._pending.get("urgent"):
            if self._pending["type"] != message_type:
                raise RuntimeError("mailbox urgent slot is occupied")
            return self._pending["seq"]
        self._urgent = self._new_item(
            message_type, values, command_id, flags, None, urgent=True
        )
        return self._urgent["seq"]

    def hello(self):
        return self.submit(
            protocol_ids.MSG_HELLO_REQ,
            (self.boot_id, 1),
            0,
            expected_type=protocol_ids.MSG_HELLO_RSP,
        )

    def request(self, message_type, values, expected_type):
        return self.submit(message_type, values, 0, flags=0, expected_type=expected_type)

    def cancel(self, seq):
        if self._urgent is not None and self._urgent["seq"] == seq:
            self._urgent = None
            return True
        if self._pending is not None and self._pending["seq"] == seq:
            self._pending = None
            return True

        cancelled = False
        queued_count = len(self._queue)
        for _index in range(queued_count):
            item = self._queue.popleft()
            if not cancelled and item["seq"] == seq:
                cancelled = True
            else:
                self._queue.append(item)
        return cancelled

    def reset_session(self):
        """Discard traffic that belongs to a previous STM32 boot session."""
        self._queue.clear()
        self._pending = None
        self._urgent = None
        self.events.clear()
        self._last_heartbeat = 0

    def _select_tx(self, now_ms):
        if self._urgent is not None:
            if self._pending is not None and not self._pending.get("urgent"):
                preempted = self._pending
                self._pending = None
                self.events.append({"kind": "preempted", "command": preempted})
            if self._pending is None:
                self._pending = self._urgent
                self._urgent = None

        if self._pending is None and self._queue:
            self._pending = self._queue.popleft()

        if self._pending is not None:
            item = self._pending
            if item["sent_ms"] is None or _elapsed(now_ms, item["sent_ms"]) >= self.ack_timeout_ms:
                if item["sent_ms"] is not None:
                    if item["retries"] >= self.max_retries:
                        failed = item
                        self._pending = None
                        self.events.append({"kind": "timeout", "command": failed})
                        return self._noop(now_ms)
                    item["retries"] += 1
                item["sent_ms"] = now_ms
                return encode_slot(item["type"], item["seq"], item["flags"], item["payload"])

        if _elapsed(now_ms, self._last_heartbeat) >= self.heartbeat_ms:
            self._last_heartbeat = now_ms
            payload = pack_payload(protocol_ids.MSG_HEARTBEAT, (now_ms & 0xFFFFFFFF,))
            return encode_slot(
                protocol_ids.MSG_HEARTBEAT,
                self._next_seq(),
                protocol_ids.SLOTFLAGS_HEARTBEAT,
                payload,
            )
        return self._noop(now_ms)

    def _noop(self, _now_ms):
        return encode_slot(protocol_ids.MSG_NOOP, self._next_seq(), 0, b"")

    def _handle_rx(self, decoded):
        message_type = decoded["type"]
        if message_type == protocol_ids.MSG_NOOP:
            return
        if self._pending and self._pending["expected_type"] == message_type:
            self.events.append({"kind": "response", "request_seq": self._pending["seq"], "slot": decoded})
            self._pending = None
            return
        if message_type in (protocol_ids.MSG_ACK, protocol_ids.MSG_NACK):
            request_seq, command_id = unpack_payload(message_type, decoded["payload"])[:2]
            if self._pending and request_seq == self._pending["seq"] and command_id == self._pending["command_id"]:
                self.events.append({"kind": "ack" if message_type == protocol_ids.MSG_ACK else "nack", "slot": decoded})
                self._pending = None
                return
        self.events.append({"kind": "message", "slot": decoded})

    def poll(self, now_ms):
        tx = self._select_tx(now_ms)
        rx = self._exchange(tx)
        try:
            decoded = decode_slot(rx)
        except SlotError:
            self.rx_errors += 1
            return None
        self._handle_rx(decoded)
        return decoded
