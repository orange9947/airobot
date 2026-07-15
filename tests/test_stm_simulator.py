import binascii
import unittest

from firmware.esp32.transport.frame_codec import decode_slot, encode_slot, pack_payload, unpack_payload
from protocol.generated import protocol_ids
from tools import resource_format
from tools.stm_simulator import RobotSimulator


def valid_package():
    frame = bytes(range(256)) * 4
    return resource_format.build_package(
        (
            resource_format.ClipSource(
                expression_id=resource_format.EXPRESSION_IDS["HAPPY"],
                weight=1,
                frame_interval_ms=100,
                frames=(frame,),
            ),
        )
    )


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

    def resource_begin(self, command_id, update_id, package, package_crc=None):
        if package_crc is None:
            try:
                package_crc = resource_format.verify_package(package).package_crc32
            except resource_format.ResourceFormatError:
                package_crc = binascii.crc32(package) & 0xFFFFFFFF
        self.h.transact(
            protocol_ids.MSG_RESOURCE_BEGIN,
            (command_id, update_id, len(package), package_crc, 1),
        )
        return self.h.poll()

    def upload_package(self, command_id, update_id, package):
        for offset in range(0, len(package), 238):
            chunk = package[offset : offset + 238]
            response = self.resource_chunk(
                command_id + offset // 238,
                update_id,
                offset,
                chunk,
            )
            self.assertEqual(response["type"], protocol_ids.MSG_ACK)

    def resource_chunk(self, command_id, update_id, offset, data, crc=None, fixed_data=None):
        if crc is None:
            crc = binascii.crc32(data) & 0xFFFFFFFF
        if fixed_data is None:
            fixed_data = data + b"\x00" * (238 - len(data))
        self.h.transact(
            protocol_ids.MSG_RESOURCE_CHUNK,
            (command_id, update_id, offset, len(data), crc, fixed_data),
        )
        return self.h.poll()

    def resource_status(self, update_id):
        self.h.transact(protocol_ids.MSG_GET_RESOURCE_STATUS, (update_id,))
        response = self.h.poll()
        self.assertEqual(response["type"], protocol_ids.MSG_RESOURCE_STATUS)
        return unpack_payload(response["type"], response["payload"])

    def resource_finish(self, command_id, update_id):
        self.h.transact(protocol_ids.MSG_RESOURCE_FINISH, (command_id, update_id))
        return self.h.poll()

    def test_hello_reports_boot_id_and_idle(self):
        response = self.h.transact(protocol_ids.MSG_GET_STATE)
        self.assertEqual(response["type"], protocol_ids.MSG_NOOP)
        snapshot = self.h.poll()
        values = unpack_payload(snapshot["type"], snapshot["payload"])
        self.assertEqual(values[0], 0x1234)
        self.assertEqual(values[2], protocol_ids.ROBOTSTATE_IDLE)

    def test_hello_advertises_coil_diagnostic_capability(self):
        harness = SimulatorHarness()
        response = harness.hello()
        values = unpack_payload(response["type"], response["payload"])
        self.assertEqual(values[1] & 0x20, 0x20)

    def test_coil_diagnostic_requires_manual_and_valid_bounds(self):
        self.h.transact(
            protocol_ids.MSG_COIL_DIAGNOSTIC,
            (
                201,
                protocol_ids.COILWHEEL_RIGHT,
                protocol_ids.COILCHANNEL_A,
                3000,
            ),
        )
        rejected = self.h.poll()
        self.assertEqual(rejected["type"], protocol_ids.MSG_NACK)
        self.assertIsNone(self.h.sim.active)

        self.h.transact(protocol_ids.MSG_SET_MODE, (202, protocol_ids.MODE_MANUAL))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_MODE_CHANGED)
        self.h.transact(
            protocol_ids.MSG_COIL_DIAGNOSTIC,
            (
                203,
                protocol_ids.COILWHEEL_RIGHT,
                protocol_ids.COILCHANNEL_A,
                3001,
            ),
        )
        rejected = self.h.poll()
        self.assertEqual(rejected["type"], protocol_ids.MSG_NACK)
        values = unpack_payload(rejected["type"], rejected["payload"])
        self.assertEqual(values[2], protocol_ids.ERRORCODE_OUT_OF_RANGE)
        self.assertIsNone(self.h.sim.active)

    def test_coil_diagnostic_locks_move_and_completes_at_deadline(self):
        self.h.transact(protocol_ids.MSG_SET_MODE, (210, protocol_ids.MODE_MANUAL))
        self.h.poll()
        self.h.poll()
        self.h.transact(
            protocol_ids.MSG_COIL_DIAGNOSTIC,
            (
                211,
                protocol_ids.COILWHEEL_RIGHT,
                protocol_ids.COILCHANNEL_C,
                3000,
            ),
        )
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.h.sim.active["kind"], "coil")
        started_ms = self.h.sim.active["started_ms"]

        self.h.transact(
            protocol_ids.MSG_MOVE_WHEELS,
            (212, 10, 10, 100, 200, 500),
        )
        rejected = self.h.poll()
        self.assertEqual(rejected["type"], protocol_ids.MSG_NACK)
        self.assertEqual(self.h.sim.active["command_id"], 211)

        elapsed = (self.h.now - started_ms) & 0xFFFFFFFF
        remaining = 2999 - elapsed
        while remaining:
            advance = min(500, remaining)
            self.h.poll(advance=advance)
            remaining -= advance
        self.assertIsNotNone(self.h.sim.active)
        done = self.h.poll(advance=1)
        self.assertEqual(done["type"], protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT)
        values = unpack_payload(done["type"], done["payload"])
        self.assertEqual(values, (211, protocol_ids.COILDIAGNOSTICRESULT_DONE))
        self.assertIsNone(self.h.sim.active)

    def test_stop_aborts_active_coil_diagnostic(self):
        self.h.transact(protocol_ids.MSG_SET_MODE, (220, protocol_ids.MODE_MANUAL))
        self.h.poll()
        self.h.poll()
        self.h.transact(
            protocol_ids.MSG_COIL_DIAGNOSTIC,
            (
                221,
                protocol_ids.COILWHEEL_RIGHT,
                protocol_ids.COILCHANNEL_D,
                3000,
            ),
        )
        self.h.poll()

        self.h.transact(
            protocol_ids.MSG_STOP,
            (222, protocol_ids.ABORTREASON_STOP),
        )
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_ACK)
        aborted = self.h.poll()
        self.assertEqual(
            aborted["type"], protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT
        )
        values = unpack_payload(aborted["type"], aborted["payload"])
        self.assertEqual(
            values, (221, protocol_ids.COILDIAGNOSTICRESULT_ABORTED)
        )
        self.assertIsNone(self.h.sim.active)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_ESTOP)

    def test_link_timeout_aborts_active_coil_diagnostic(self):
        self.h.transact(protocol_ids.MSG_SET_MODE, (230, protocol_ids.MODE_MANUAL))
        self.h.poll()
        self.h.poll()
        self.h.transact(
            protocol_ids.MSG_COIL_DIAGNOSTIC,
            (
                231,
                protocol_ids.COILWHEEL_RIGHT,
                protocol_ids.COILCHANNEL_A,
                3000,
            ),
        )
        self.h.poll()

        response = self.h.poll(
            advance=self.h.sim.HEARTBEAT_TIMEOUT_MS + 1
        )
        self.assertEqual(response["type"], protocol_ids.MSG_FAULT_EVENT)
        self.assertIsNone(self.h.sim.active)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_ESTOP)
        aborted = self.h.poll()
        self.assertEqual(
            aborted["type"], protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT
        )
        values = unpack_payload(aborted["type"], aborted["payload"])
        self.assertEqual(
            values, (231, protocol_ids.COILDIAGNOSTICRESULT_ABORTED)
        )

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

    def test_simulator_queues_and_dedup_are_bounded(self):
        for index in range(self.h.sim.OUTGOING_CAPACITY * 3):
            self.h.sim._queue(
                protocol_ids.MSG_MODE_CHANGED,
                (protocol_ids.MODE_IDLE, index & 0xFF),
            )
        self.assertEqual(
            len(self.h.sim._outgoing), self.h.sim.OUTGOING_CAPACITY
        )

        self.h.sim._dedup.clear()
        for index in range(self.h.sim.DEDUP_CAPACITY * 3):
            self.h.sim._remember_dedup((protocol_ids.MSG_SET_MODE, index), b"x")
        self.assertEqual(len(self.h.sim._dedup), self.h.sim.DEDUP_CAPACITY)
        self.assertNotIn((protocol_ids.MSG_SET_MODE, 0), self.h.sim._dedup)

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

    def test_resource_begin_has_one_bounded_session_and_reports_status(self):
        package = bytes(64)
        ack = self.resource_begin(100, 0xA001, package)
        self.assertEqual(ack["type"], protocol_ids.MSG_ACK)
        self.assertEqual(len(self.h.sim.resource_update["buffer"]), len(package))
        self.assertLessEqual(len(self.h.sim.resource_update["buffer"]), self.h.sim.RESOURCE_MAX_PACKAGE_SIZE)

        status = self.resource_status(0xA001)
        self.assertEqual(status[0], 0xA001)
        self.assertEqual(status[1], protocol_ids.RESOURCESTATE_READY)
        self.assertEqual(status[2], self.h.sim.RESOURCE_BANK_NONE)
        self.assertEqual(status[3], 0)
        self.assertEqual(status[4], 0)
        self.assertEqual(status[5], len(package))
        self.assertEqual(status[6], protocol_ids.RESOURCEERROR_NONE)

        second = self.resource_begin(101, 0xA002, b"other")
        self.assertEqual(second["type"], protocol_ids.MSG_NACK)
        self.assertEqual(self.h.sim.resource_update["update_id"], 0xA001)

    def test_resource_chunks_are_ordered_and_last_retry_is_idempotent(self):
        package = bytes(range(64))
        self.assertEqual(self.resource_begin(110, 0xA010, package)["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.resource_chunk(111, 0xA010, 0, package[:4])["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.resource_chunk(112, 0xA010, 0, package[:4])["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.resource_status(0xA010)[4], 4)

        skipped = self.resource_chunk(113, 0xA010, 5, package[5:])
        self.assertEqual(skipped["type"], protocol_ids.MSG_NACK)
        self.assertEqual(self.resource_status(0xA010)[6], protocol_ids.RESOURCEERROR_BAD_OFFSET)

        overlap = self.resource_chunk(114, 0xA010, 0, b"wxyz")
        self.assertEqual(overlap["type"], protocol_ids.MSG_NACK)
        self.assertEqual(self.h.sim.resource_update["next_offset"], 4)

        self.assertEqual(self.resource_chunk(115, 0xA010, 4, package[4:])["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.resource_status(0xA010)[4], len(package))

        stale_retry = self.resource_chunk(111, 0xA010, 0, package[:4])
        self.assertEqual(stale_retry["type"], protocol_ids.MSG_NACK)

    def test_resource_chunk_rejects_bad_crc_nonzero_padding_and_wrong_session(self):
        package = bytes(64)
        self.assertEqual(self.resource_begin(120, 0xA020, package)["type"], protocol_ids.MSG_ACK)

        data = b"abcd"
        bad_crc = self.resource_chunk(121, 0xA020, 0, data, crc=0)
        self.assertEqual(bad_crc["type"], protocol_ids.MSG_NACK)
        self.assertEqual(self.resource_status(0xA020)[6], protocol_ids.RESOURCEERROR_CHUNK_CRC)

        nonzero_tail = data + b"\x01" + b"\x00" * 233
        bad_padding = self.resource_chunk(122, 0xA020, 0, data, fixed_data=nonzero_tail)
        self.assertEqual(bad_padding["type"], protocol_ids.MSG_NACK)
        self.assertEqual(self.resource_status(0xA020)[6], protocol_ids.RESOURCEERROR_BAD_LENGTH)

        wrong_session = self.resource_chunk(123, 0xA021, 0, data)
        self.assertEqual(wrong_session["type"], protocol_ids.MSG_NACK)
        self.assertEqual(self.h.sim.resource_update["next_offset"], 0)

    def test_resource_finish_checks_package_crc_and_activates_bank(self):
        package = valid_package()
        self.assertEqual(self.resource_begin(130, 0xA030, package)["type"], protocol_ids.MSG_ACK)
        self.upload_package(131, 0xA030, package)
        self.assertEqual(self.resource_finish(132, 0xA030)["type"], protocol_ids.MSG_ACK)

        verifying = self.resource_status(0xA030)
        self.assertEqual(verifying[1], protocol_ids.RESOURCESTATE_VERIFYING)
        self.assertIsNotNone(self.h.sim.resource_update)

        self.h.poll(advance=self.h.sim.RESOURCE_VERIFY_DELAY_MS)
        committing = self.resource_status(0xA030)
        self.assertEqual(committing[1], protocol_ids.RESOURCESTATE_COMMITTING)
        self.assertEqual(committing[3], 0)

        self.h.poll(advance=self.h.sim.RESOURCE_COMMIT_DELAY_MS)
        status = self.resource_status(0xA030)
        self.assertEqual(status[1], protocol_ids.RESOURCESTATE_IDLE)
        self.assertEqual(status[2], 0)
        self.assertEqual(status[3], 1)
        self.assertEqual(status[4], len(package))
        self.assertEqual(status[6], protocol_ids.RESOURCEERROR_NONE)
        self.assertIsNone(self.h.sim.resource_update)

        bad_package = valid_package()
        self.assertEqual(self.resource_begin(133, 0xA031, bad_package, package_crc=0)["type"], protocol_ids.MSG_ACK)
        self.upload_package(134, 0xA031, bad_package)
        self.assertEqual(self.resource_finish(135, 0xA031)["type"], protocol_ids.MSG_ACK)
        self.assertEqual(
            self.resource_status(0xA031)[1],
            protocol_ids.RESOURCESTATE_VERIFYING,
        )
        self.h.poll(advance=self.h.sim.RESOURCE_VERIFY_DELAY_MS)
        failed = self.resource_status(0xA031)
        self.assertEqual(failed[1], protocol_ids.RESOURCESTATE_FAILED)
        self.assertEqual(failed[2], 0)
        self.assertEqual(failed[3], 1)
        self.assertEqual(failed[6], protocol_ids.RESOURCEERROR_PACKAGE_CRC)
        self.assertIsNone(self.h.sim.resource_update)

    def test_resource_finish_rejects_structurally_invalid_package(self):
        package = bytearray(valid_package())
        package[0:4] = b"NOPE"
        package = bytes(package)
        self.assertEqual(
            self.resource_begin(
                136,
                0xA031,
                package,
                package_crc=binascii.crc32(package) & 0xFFFFFFFF,
            )["type"],
            protocol_ids.MSG_ACK,
        )
        for offset in range(0, len(package), 238):
            chunk = package[offset : offset + 238]
            self.assertEqual(
                self.resource_chunk(137 + offset, 0xA031, offset, chunk)["type"],
                protocol_ids.MSG_ACK,
            )
        self.assertEqual(
            self.resource_finish(190, 0xA031)["type"], protocol_ids.MSG_ACK
        )
        self.h.poll(advance=self.h.sim.RESOURCE_VERIFY_DELAY_MS)
        failed = self.resource_status(0xA031)
        self.assertEqual(failed[1], protocol_ids.RESOURCESTATE_FAILED)
        self.assertEqual(failed[6], protocol_ids.RESOURCEERROR_BAD_DIRECTORY)

    def test_resource_finish_can_be_aborted_while_verifying(self):
        package = valid_package()
        self.assertEqual(self.resource_begin(136, 0xA032, package)["type"], protocol_ids.MSG_ACK)
        self.upload_package(137, 0xA032, package)
        self.assertEqual(self.resource_finish(138, 0xA032)["type"], protocol_ids.MSG_ACK)
        self.assertEqual(
            self.resource_status(0xA032)[1],
            protocol_ids.RESOURCESTATE_VERIFYING,
        )

        self.h.transact(protocol_ids.MSG_RESOURCE_ABORT, (139, 0xA032))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_ACK)
        aborted = self.resource_status(0xA032)
        self.assertEqual(aborted[1], protocol_ids.RESOURCESTATE_ABORTED)
        self.assertEqual(aborted[6], protocol_ids.RESOURCEERROR_NONE)
        self.assertIsNone(self.h.sim.resource_update)

    def test_resource_abort_is_rejected_after_commit_point(self):
        package = valid_package()
        self.assertEqual(self.resource_begin(140, 0xA034, package)["type"], protocol_ids.MSG_ACK)
        self.upload_package(141, 0xA034, package)
        self.assertEqual(self.resource_finish(142, 0xA034)["type"], protocol_ids.MSG_ACK)
        self.h.poll(advance=self.h.sim.RESOURCE_VERIFY_DELAY_MS)
        self.assertEqual(
            self.resource_status(0xA034)[1],
            protocol_ids.RESOURCESTATE_COMMITTING,
        )

        self.h.transact(protocol_ids.MSG_RESOURCE_ABORT, (143, 0xA034))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_NACK)
        self.assertIsNotNone(self.h.sim.resource_update)
        self.h.poll(advance=self.h.sim.RESOURCE_COMMIT_DELAY_MS)
        committed = self.resource_status(0xA034)
        self.assertEqual(committed[1], protocol_ids.RESOURCESTATE_IDLE)
        self.assertEqual(committed[3], 1)

    def test_link_loss_after_commit_point_does_not_roll_back_bank(self):
        package = valid_package()
        self.assertEqual(self.resource_begin(144, 0xA035, package)["type"], protocol_ids.MSG_ACK)
        self.upload_package(145, 0xA035, package)
        self.assertEqual(self.resource_finish(146, 0xA035)["type"], protocol_ids.MSG_ACK)
        self.h.poll(advance=self.h.sim.RESOURCE_VERIFY_DELAY_MS)
        self.assertEqual(
            self.resource_status(0xA035)[1],
            protocol_ids.RESOURCESTATE_COMMITTING,
        )

        response = self.h.poll(advance=self.h.sim.HEARTBEAT_TIMEOUT_MS + 1)
        self.assertEqual(response["type"], protocol_ids.MSG_FAULT_EVENT)
        committed = self.resource_status(0xA035)
        self.assertEqual(committed[1], protocol_ids.RESOURCESTATE_IDLE)
        self.assertEqual(committed[2], 0)
        self.assertEqual(committed[3], 1)

    def test_resource_abort_releases_session_and_allows_a_new_begin(self):
        package = bytes(64)
        self.assertEqual(self.resource_begin(136, 0xA032, package)["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.resource_chunk(137, 0xA032, 0, package[:4])["type"], protocol_ids.MSG_ACK)

        self.h.transact(protocol_ids.MSG_RESOURCE_ABORT, (148, 0xA032))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_ACK)
        aborted = self.resource_status(0xA032)
        self.assertEqual(aborted[1], protocol_ids.RESOURCESTATE_ABORTED)
        self.assertEqual(aborted[4], 4)
        self.assertIsNone(self.h.sim.resource_update)
        self.assertEqual(self.resource_begin(149, 0xA033, bytes(64))["type"], protocol_ids.MSG_ACK)

    def test_resource_update_locks_motion_but_stop_and_clear_remain_available(self):
        package = bytes(64)
        self.assertEqual(self.resource_begin(140, 0xA040, package)["type"], protocol_ids.MSG_ACK)

        self.h.transact(protocol_ids.MSG_SET_MODE, (141, protocol_ids.MODE_MANUAL))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_NACK)
        self.h.transact(protocol_ids.MSG_MOVE_WHEELS, (142, 10, 10, 100, 200, 500))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_NACK)
        self.h.transact(
            protocol_ids.MSG_COIL_DIAGNOSTIC,
            (
                146,
                protocol_ids.COILWHEEL_RIGHT,
                protocol_ids.COILCHANNEL_A,
                3000,
            ),
        )
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_NACK)

        self.h.transact(protocol_ids.MSG_STOP, (143, protocol_ids.ABORTREASON_STOP))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.h.sim.state, protocol_ids.ROBOTSTATE_ESTOP)
        self.assertIsNotNone(self.h.sim.resource_update)

        self.h.transact(protocol_ids.MSG_CLEAR_ESTOP, (144,))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_ACK)
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_MODE_CHANGED)
        self.h.transact(protocol_ids.MSG_SET_MODE, (145, protocol_ids.MODE_AI))
        self.assertEqual(self.h.poll()["type"], protocol_ids.MSG_NACK)
        self.assertIsNotNone(self.h.sim.resource_update)

    def test_link_timeout_or_lost_session_aborts_resource_update(self):
        package = bytes(64)
        self.assertEqual(self.resource_begin(150, 0xA050, package)["type"], protocol_ids.MSG_ACK)
        response = self.h.poll(advance=self.h.sim.HEARTBEAT_TIMEOUT_MS + 1)
        self.assertEqual(response["type"], protocol_ids.MSG_FAULT_EVENT)
        self.assertIsNone(self.h.sim.resource_update)
        status = self.resource_status(0xA050)
        self.assertEqual(status[1], protocol_ids.RESOURCESTATE_FAILED)
        self.assertEqual(status[6], protocol_ids.RESOURCEERROR_LINK_LOST)

        other = SimulatorHarness()
        other.hello()
        self.h = other
        self.assertEqual(self.resource_begin(151, 0xA051, package)["type"], protocol_ids.MSG_ACK)
        self.h.sim.session = False
        self.h.poll()
        self.assertIsNone(self.h.sim.resource_update)
        lost = self.resource_status(0xA051)
        self.assertEqual(lost[1], protocol_ids.RESOURCESTATE_FAILED)
        self.assertEqual(lost[6], protocol_ids.RESOURCEERROR_LINK_LOST)

    def test_link_timeout_without_update_keeps_resource_status_idle(self):
        self.h.poll(advance=self.h.sim.HEARTBEAT_TIMEOUT_MS + 1)
        status = self.resource_status(0)
        self.assertEqual(status[1], protocol_ids.RESOURCESTATE_IDLE)
        self.assertEqual(status[6], protocol_ids.RESOURCEERROR_NONE)

    def test_resource_inactivity_timeout_aborts_while_heartbeats_continue(self):
        package = bytes(64)
        self.assertEqual(self.resource_begin(160, 0xA060, package)["type"], protocol_ids.MSG_ACK)
        for index in range(87):
            self.h.transact(protocol_ids.MSG_HEARTBEAT, (index,), advance=700)
        self.assertIsNone(self.h.sim.resource_update)
        status = self.resource_status(0xA060)
        self.assertEqual(status[1], protocol_ids.RESOURCESTATE_FAILED)
        self.assertEqual(status[6], protocol_ids.RESOURCEERROR_BUSY_TIMEOUT)


if __name__ == "__main__":
    unittest.main()
