"""Polling SPI mailbox client with bounded retries and heartbeats."""

from collections import deque

from protocol.generated import protocol_ids

from .frame_codec import SlotError, decode_slot, encode_slot, pack_payload, unpack_payload


def _elapsed(now_ms, then_ms):
    return (now_ms - then_ms) & 0xFFFFFFFF


class MailboxClient:
    def __init__(self, exchange, boot_id, heartbeat_ms=250, ack_timeout_ms=100, max_retries=3):
        self._exchange = exchange
        self.boot_id = boot_id & 0xFFFFFFFF
        self.heartbeat_ms = heartbeat_ms
        self.ack_timeout_ms = ack_timeout_ms
        self.max_retries = max_retries
        self._seq = 0
        self._queue = deque((), 8)
        self._pending = None
        self._last_heartbeat = 0
        self.rx_errors = 0
        self.events = deque((), 16)

    def _next_seq(self):
        self._seq = (self._seq + 1) & 0xFFFF
        if self._seq == 0:
            self._seq = 1
        return self._seq

    def submit(self, message_type, values, command_id=0, flags=None):
        if len(self._queue) >= self._queue.maxlen:
            raise RuntimeError("mailbox command queue full")
        if flags is None:
            flags = protocol_ids.SLOTFLAGS_ACK_REQUEST
        item = {
            "type": message_type,
            "seq": self._next_seq(),
            "flags": flags,
            "payload": pack_payload(message_type, values),
            "command_id": command_id & 0xFFFFFFFF,
            "sent_ms": None,
            "retries": 0,
        }
        self._queue.append(item)
        return item["seq"]

    def hello(self):
        return self.submit(protocol_ids.MSG_HELLO_REQ, (self.boot_id, 1), 0)

    def _select_tx(self, now_ms):
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
        if (
            message_type == protocol_ids.MSG_HELLO_RSP
            and self._pending
            and self._pending["type"] == protocol_ids.MSG_HELLO_REQ
        ):
            self.events.append({"kind": "hello", "slot": decoded})
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
