import unittest

from firmware.esp32.transport.frame_codec import decode_slot, encode_slot, pack_payload, unpack_payload
from firmware.esp32.transport.spi_mailbox import MailboxClient
from protocol.generated import protocol_ids


class DelayedDevice:
    def __init__(self):
        self.next_rx = encode_slot(protocol_ids.MSG_NOOP, 1)
        self.transactions = []

    def exchange(self, tx):
        response = self.next_rx
        decoded = decode_slot(tx)
        self.transactions.append(decoded)
        if decoded["type"] == protocol_ids.MSG_SET_MODE:
            command_id, _mode = unpack_payload(decoded["type"], decoded["payload"])
            payload = pack_payload(protocol_ids.MSG_ACK, (decoded["seq"], command_id, 0))
            self.next_rx = encode_slot(
                protocol_ids.MSG_ACK,
                100,
                protocol_ids.SLOTFLAGS_RESPONSE,
                payload,
            )
        else:
            self.next_rx = encode_slot(protocol_ids.MSG_NOOP, 101)
        return response


class SpiMailboxTests(unittest.TestCase):
    def test_command_ack_arrives_on_next_transaction(self):
        device = DelayedDevice()
        mailbox = MailboxClient(device.exchange, boot_id=1)
        seq = mailbox.submit(protocol_ids.MSG_SET_MODE, (42, protocol_ids.MODE_MANUAL), command_id=42)

        first = mailbox.poll(0)
        self.assertEqual(first["type"], protocol_ids.MSG_NOOP)
        self.assertEqual(device.transactions[-1]["seq"], seq)

        second = mailbox.poll(10)
        self.assertEqual(second["type"], protocol_ids.MSG_ACK)
        event = mailbox.events.popleft()
        self.assertEqual(event["kind"], "ack")

    def test_invalid_rx_is_counted_without_stopping(self):
        mailbox = MailboxClient(lambda _tx: bytes(protocol_ids.SLOT_SIZE), boot_id=1)
        self.assertIsNone(mailbox.poll(0))
        self.assertEqual(mailbox.rx_errors, 1)

    def test_command_queue_uses_an_explicit_micropython_compatible_capacity(self):
        mailbox = MailboxClient(lambda _tx: bytes(protocol_ids.SLOT_SIZE), boot_id=1)
        for command_id in range(8):
            mailbox.submit(
                protocol_ids.MSG_SET_MODE,
                (command_id, protocol_ids.MODE_IDLE),
                command_id=command_id,
            )
        with self.assertRaisesRegex(RuntimeError, "queue full"):
            mailbox.submit(protocol_ids.MSG_SET_MODE, (9, protocol_ids.MODE_IDLE), command_id=9)


if __name__ == "__main__":
    unittest.main()
