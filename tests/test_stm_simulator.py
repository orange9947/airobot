import unittest

from firmware.esp32.transport.frame_codec import decode_slot, encode_slot, pack_payload, unpack_payload
from protocol.generated import protocol_ids
from tools.stm_simulator import RobotSimulator


class SimulatorHarness:
    def __init__(self):
        self.sim = RobotSimulator(boot_id=0x1234)
        self.seq = 0
        self.now = 0

    def slot(self, message_type, values=(), flags=0):
        self.seq += 1
        return encode_slot(message_type, self.seq, flags, pack_payload(message_type, values))

    def transact(self, message_type, values=(), flags=0, advance=10):
        self.now += advance
        return decode_slot(self.sim.transact(self.slot(message_type, values, flags), self.now))

    def poll(self, advance=10):
        return self.transact(protocol_ids.MSG_NOOP, (), 0, advance)

    def hello(self):
        first = self.transact(protocol_ids.MSG_HELLO_REQ, (0x55, 1))
        self.assert_type(first, protocol_ids.MSG_NOOP)
        response = self.poll()
        self.assert_type(response, protocol_ids.MSG_HELLO_RSP)
        return response

    @staticmethod
    def assert_type(slot, message_type):
        if slot["type"] != message_type:
            raise AssertionError("expected type {}, got {}".format(message_type, slot["type"]))


class StmSimulatorTests(unittest.TestCase):
    def setUp(self):
        self.h = SimulatorHarness()
        self.h.hello()

    def test_hello_reports_boot_id_and_idle(self):
        response = self.h.transact(protocol_ids.MSG_GET_STATE)
        self.assertEqual(response["type"], protocol_ids.MSG_NOOP)
        snapshot = self.h.poll()
        values = unpack_payload(snapshot["type"], snapshot["payload"])
        self.assertEqual(values[0], 0x1234)
        self.assertEqual(values[2], protocol_ids.ROBOTSTATE_IDLE)

    def test_mode_move_and_completion(self):
        self.h.transact(protocol_ids.MSG_SET_MODE, (1, protocol_ids.MODE_MANUAL))
        ack = self.h.poll()
        self.assertEqual(ack["type"], protocol_ids.MSG_ACK)
        changed = self.h.poll()
        self.assertEqual(changed["type"], protocol_ids.MSG_MODE_CHANGED)

        self.h.transact(protocol_ids.MSG_MOVE_WHEELS, (2, 40, 40, 400, 600, 1000))
        ack = self.h.poll()
        self.assertEqual(ack["type"], protocol_ids.MSG_ACK)
        started = self.h.poll()
        self.assertEqual(started["type"], protocol_ids.MSG_MOTION_STARTED)
        done = self.h.poll(advance=200)
        self.assertEqual(done["type"], protocol_ids.MSG_MOTION_DONE)

    def test_duplicate_move_does_not_start_twice(self):
        self.h.transact(protocol_ids.MSG_SET_MODE, (1, protocol_ids.MODE_MANUAL))
        self.h.poll()
        self.h.poll()
        tx = self.h.slot(protocol_ids.MSG_MOVE_WHEELS, (2, 400, 400, 400, 600, 1500))
        self.h.now += 10
        self.h.sim.transact(tx, self.h.now)
        self.h.poll()
        self.h.poll()
        self.h.now += 10
        self.h.sim.transact(tx, self.h.now)
        response = self.h.poll()
        self.assertEqual(response["type"], protocol_ids.MSG_ACK)
        remaining_types = [decode_slot(slot)["type"] for slot in self.h.sim._outgoing]
        self.assertNotIn(protocol_ids.MSG_MOTION_STARTED, remaining_types)

    def test_heartbeat_timeout_enters_estop(self):
        response_types = [self.h.poll(advance=800)["type"]]
        for _ in range(4):
            response_types.append(self.h.poll()["type"])
        self.assertIn(protocol_ids.MSG_FAULT_EVENT, response_types)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_ESTOP)

    def test_valid_slot_recovers_session_without_clearing_estop(self):
        self.h.sim.session = False
        self.h.sim.state = protocol_ids.ROBOTSTATE_ESTOP
        self.h.sim.fault_code = protocol_ids.ERRORCODE_LINK_LOST

        self.h.poll()
        self.assertTrue(self.h.sim.session)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_ESTOP)
        self.assertEqual(self.h.sim.fault_code, protocol_ids.ERRORCODE_LINK_LOST)

        self.h.transact(protocol_ids.MSG_CLEAR_ESTOP, (39,))
        ack = self.h.poll()
        self.assertEqual(ack["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_IDLE)
        self.assertEqual(self.h.sim.fault_code, 0)

    def test_clear_estop_requires_estop(self):
        self.h.transact(protocol_ids.MSG_CLEAR_ESTOP, (40,))
        nack = self.h.poll()
        self.assertEqual(nack["type"], protocol_ids.MSG_NACK)

        self.h.sim.state = protocol_ids.ROBOTSTATE_ESTOP
        self.h.sim.fault_code = protocol_ids.ERRORCODE_LINK_LOST
        self.h.transact(protocol_ids.MSG_CLEAR_ESTOP, (41,))
        ack = self.h.poll()
        self.assertEqual(ack["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_IDLE)
        self.assertEqual(self.h.sim.fault_code, 0)
        self.assertIsNone(self.h.sim.active)

    def test_remote_stop_then_confirmed_clear(self):
        self.h.transact(protocol_ids.MSG_STOP, (44, protocol_ids.ABORTREASON_STOP))
        ack = self.h.poll()
        self.assertEqual(ack["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_ESTOP)
        self.assertEqual(self.h.sim.fault_code, 11)

        self.h.transact(protocol_ids.MSG_CLEAR_ESTOP, (45,))
        ack = self.h.poll()
        self.assertEqual(ack["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_IDLE)
        self.assertEqual(self.h.sim.fault_code, 0)

    def test_estop_rejects_expression(self):
        self.h.sim.state = protocol_ids.ROBOTSTATE_ESTOP
        self.h.transact(
            protocol_ids.MSG_SET_EXPRESSION,
            (42, protocol_ids.EXPRESSION_HAPPY),
        )
        nack = self.h.poll()
        self.assertEqual(nack["type"], protocol_ids.MSG_NACK)
        values = unpack_payload(nack["type"], nack["payload"])
        self.assertEqual(values[2], protocol_ids.ERRORCODE_BAD_STATE)

    def test_safety_expressions_are_not_remote_moods(self):
        self.h.transact(
            protocol_ids.MSG_SET_EXPRESSION,
            (43, protocol_ids.EXPRESSION_ESTOP),
        )
        nack = self.h.poll()
        self.assertEqual(nack["type"], protocol_ids.MSG_NACK)
        values = unpack_payload(nack["type"], nack["payload"])
        self.assertEqual(values[2], protocol_ids.ERRORCODE_BAD_PAYLOAD)

    def test_bad_slot_is_counted_and_next_transaction_recovers(self):
        self.h.sim.session = False
        bad = bytearray(self.h.slot(protocol_ids.MSG_NOOP))
        bad[-1] ^= 1
        self.h.sim.transact(bytes(bad), self.h.now)
        self.assertEqual(self.h.sim.rx_errors, 1)
        self.assertFalse(self.h.sim.session)
        response = self.h.poll()
        self.assertIn(response["type"], (protocol_ids.MSG_NOOP, protocol_ids.MSG_STATE_SNAPSHOT))


if __name__ == "__main__":
    unittest.main()
