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


class SilentDevice:
    def __init__(self):
        self.transactions = []

    def exchange(self, tx):
        self.transactions.append(decode_slot(tx))
        return encode_slot(protocol_ids.MSG_NOOP, len(self.transactions))


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

    def test_command_is_not_retried_before_stm32_rearm_window(self):
        device = SilentDevice()
        mailbox = MailboxClient(device.exchange, boot_id=1)
        seq = mailbox.submit(
            protocol_ids.MSG_SET_MODE,
            (42, protocol_ids.MODE_MANUAL),
            command_id=42,
        )
        mailbox.poll(0)
        mailbox.poll(100)
        mailbox.poll(250)
        self.assertEqual(device.transactions[0]["seq"], seq)
        self.assertEqual(device.transactions[1]["type"], protocol_ids.MSG_NOOP)
        self.assertEqual(device.transactions[2]["seq"], seq)

    def test_cancel_removes_queued_command_without_reordering_others(self):
        device = SilentDevice()
        mailbox = MailboxClient(device.exchange, boot_id=1)
        first = mailbox.submit(
            protocol_ids.MSG_SET_MODE,
            (1, protocol_ids.MODE_IDLE),
            command_id=1,
        )
        second = mailbox.submit(
            protocol_ids.MSG_SET_MODE,
            (2, protocol_ids.MODE_MANUAL),
            command_id=2,
        )

        self.assertTrue(mailbox.cancel(first))
        self.assertFalse(mailbox.cancel(first))
        mailbox.poll(0)
        self.assertEqual(device.transactions[-1]["seq"], second)

    def test_cancel_removes_pending_command_and_prevents_retry(self):
        device = SilentDevice()
        mailbox = MailboxClient(device.exchange, boot_id=1)
        seq = mailbox.submit(
            protocol_ids.MSG_SET_MODE,
            (42, protocol_ids.MODE_MANUAL),
            command_id=42,
        )
        mailbox.poll(0)
        self.assertEqual(device.transactions[-1]["seq"], seq)

        self.assertTrue(mailbox.cancel(seq))
        mailbox.poll(250)
        self.assertNotEqual(device.transactions[-1]["seq"], seq)
        self.assertEqual(device.transactions[-1]["type"], protocol_ids.MSG_HEARTBEAT)

    def test_urgent_command_is_coalesced_and_bypasses_a_full_queue(self):
        device = SilentDevice()
        mailbox = MailboxClient(device.exchange, boot_id=1)
        for command_id in range(8):
            mailbox.submit(
                protocol_ids.MSG_SET_MODE,
                (command_id, protocol_ids.MODE_IDLE),
                command_id=command_id,
            )

        urgent_seq = mailbox.submit_urgent(
            protocol_ids.MSG_STOP,
            (99, protocol_ids.ABORTREASON_STOP),
            command_id=99,
        )
        duplicate_seq = mailbox.submit_urgent(
            protocol_ids.MSG_STOP,
            (100, protocol_ids.ABORTREASON_STOP),
            command_id=100,
        )

        self.assertEqual(duplicate_seq, urgent_seq)
        self.assertEqual(len(mailbox._queue), 8)
        mailbox.poll(0)
        self.assertEqual(device.transactions[-1]["type"], protocol_ids.MSG_STOP)
        self.assertEqual(device.transactions[-1]["seq"], urgent_seq)
        self.assertTrue(mailbox._pending["urgent"])

    def test_urgent_command_preempts_a_regular_pending_command(self):
        device = SilentDevice()
        mailbox = MailboxClient(device.exchange, boot_id=1)
        regular_seq = mailbox.submit(
            protocol_ids.MSG_SET_MODE,
            (42, protocol_ids.MODE_MANUAL),
            command_id=42,
        )
        mailbox.poll(0)
        self.assertEqual(mailbox._pending["seq"], regular_seq)

        urgent_seq = mailbox.submit_urgent(
            protocol_ids.MSG_STOP,
            (43, protocol_ids.ABORTREASON_STOP),
            command_id=43,
        )
        mailbox.poll(10)

        self.assertEqual(device.transactions[-1]["type"], protocol_ids.MSG_STOP)
        self.assertEqual(device.transactions[-1]["seq"], urgent_seq)
        self.assertEqual(mailbox._pending["seq"], urgent_seq)
        event = mailbox.events.popleft()
        self.assertEqual(event["kind"], "preempted")
        self.assertEqual(event["command"]["seq"], regular_seq)

    def test_session_reset_discards_all_old_traffic_but_keeps_sequence_moving(self):
        device = SilentDevice()
        mailbox = MailboxClient(device.exchange, boot_id=1)
        first_seq = mailbox.submit(
            protocol_ids.MSG_SET_MODE,
            (1, protocol_ids.MODE_IDLE),
            command_id=1,
        )
        mailbox.poll(0)
        mailbox.submit(
            protocol_ids.MSG_SET_MODE,
            (2, protocol_ids.MODE_MANUAL),
            command_id=2,
        )
        mailbox.submit_urgent(
            protocol_ids.MSG_STOP,
            (3, protocol_ids.ABORTREASON_STOP),
            command_id=3,
        )
        mailbox.events.append({"kind": "message"})

        mailbox.reset_session()

        self.assertIsNone(mailbox._pending)
        self.assertIsNone(mailbox._urgent)
        self.assertFalse(mailbox._queue)
        self.assertFalse(mailbox.events)
        self.assertGreater(mailbox.hello(), first_seq)


if __name__ == "__main__":
    unittest.main()
