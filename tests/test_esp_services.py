import asyncio
import binascii
import hashlib
import tempfile
import unittest

from firmware.esp32.core.security import pbkdf2_sha256, pbkdf2_sha256_async
from firmware.esp32.services.config_service import ConfigService
from firmware.esp32.services.device_service import DeviceCommandError, DeviceService
from firmware.esp32.transport.frame_codec import (
    decode_slot,
    encode_slot,
    pack_payload,
    unpack_payload,
)
from protocol.generated import protocol_ids
from tools import resource_format
from tools.stm_simulator import RobotSimulator


class SimulatedSpiLink:
    def __init__(self):
        self.simulator = RobotSimulator()
        self.now_ms = 0

    def exchange(self, tx):
        self.now_ms += 10
        return self.simulator.transact(tx, self.now_ms)


class SwitchableSpiLink(SimulatedSpiLink):
    def __init__(self):
        super().__init__()
        self.available = True

    def exchange(self, tx):
        if not self.available:
            return bytes(protocol_ids.SLOT_SIZE)
        return super().exchange(tx)


class RecordingSpiLink:
    def __init__(self):
        self.transactions = []

    def exchange(self, tx):
        self.transactions.append(decode_slot(tx))
        return encode_slot(protocol_ids.MSG_NOOP, len(self.transactions))


def deliver_command_result(device, message_type, seq, command_id, code=0):
    slot = decode_slot(
        encode_slot(
            message_type,
            0x7000,
            protocol_ids.SLOTFLAGS_RESPONSE,
            pack_payload(message_type, (seq, command_id, code)),
        )
    )
    device.mailbox._handle_rx(slot)
    device._drain_events()


class EspServiceTests(unittest.TestCase):
    def test_state_snapshot_detects_stm32_reset_and_discards_old_session(self):
        device = DeviceService(RecordingSpiLink(), boot_id=0x45535031)
        events = []
        device.add_listener(events.append)
        device.connected = True
        device.status["connected"] = True
        device._stm_boot_id = 0x11111111
        device._stm_uptime_ms = 10000
        device.status["resource"] = {"update_id": 7}
        device.mailbox.submit(
            protocol_ids.MSG_SET_MODE,
            (1, protocol_ids.MODE_IDLE),
            command_id=1,
        )
        device._waiting_results[3] = 1
        device._motion_waiters[4] = True
        device._coil_diagnostic_waiters[6] = True
        device._resource_status_waiters[5] = 7

        device._process_slot(
            {
                "type": protocol_ids.MSG_STATE_SNAPSHOT,
                "values": (
                    0x22222222,
                    5,
                    protocol_ids.ROBOTSTATE_BOOT,
                    protocol_ids.MODE_IDLE,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                ),
            }
        )

        self.assertFalse(device.connected)
        self.assertEqual(device.status["state"], protocol_ids.ROBOTSTATE_BOOT)
        self.assertIsNone(device.status["resource"])
        self.assertIsNone(device.mailbox._pending)
        self.assertFalse(device.mailbox._queue)
        self.assertFalse(device._waiting_results)
        self.assertFalse(device._motion_waiters)
        self.assertFalse(device._coil_diagnostic_waiters)
        self.assertFalse(device._resource_status_waiters)
        self.assertTrue(
            any(event.get("type") == "link" and not event["connected"] for event in events)
        )

    def test_uptime_wrap_is_allowed_but_small_regression_forces_reconnect(self):
        device = DeviceService(RecordingSpiLink(), boot_id=0x45535031)
        device.connected = True
        device.status["connected"] = True
        device._stm_boot_id = 0x11111111
        device._stm_uptime_ms = 0xFFFFFFF0

        wrapped = (
            0x11111111,
            20,
            protocol_ids.ROBOTSTATE_IDLE,
            protocol_ids.MODE_IDLE,
            0,
            0,
            0,
            0,
            0,
            0,
        )
        device._process_slot(
            {"type": protocol_ids.MSG_STATE_SNAPSHOT, "values": wrapped}
        )
        self.assertTrue(device.connected)

        regressed = list(wrapped)
        regressed[1] = 10
        device._process_slot(
            {"type": protocol_ids.MSG_STATE_SNAPSHOT, "values": tuple(regressed)}
        )
        self.assertFalse(device.connected)

    def test_pbkdf2_matches_cpython_reference(self):
        password = b"correct horse"
        salt = b"0123456789abcdef"
        expected = hashlib.pbkdf2_hmac("sha256", password, salt, 25, 32)
        self.assertEqual(pbkdf2_sha256(password, salt, 25), expected)

    def test_async_pbkdf2_matches_sync_and_yields(self):
        async def scenario():
            done = False
            scheduler_ticks = 0

            async def ticker():
                nonlocal scheduler_ticks
                while not done:
                    scheduler_ticks += 1
                    await asyncio.sleep(0)

            ticker_task = asyncio.create_task(ticker())
            result = await pbkdf2_sha256_async(
                b"correct horse", b"0123456789abcdef", 64, yield_every=4
            )
            done = True
            await ticker_task
            self.assertEqual(
                result,
                pbkdf2_sha256(b"correct horse", b"0123456789abcdef", 64),
            )
            self.assertGreater(scheduler_ticks, 1)

        asyncio.run(scenario())

    def test_config_is_atomic_and_secrets_are_redacted(self):
        with tempfile.TemporaryDirectory() as root:
            config = ConfigService(root)
            config.load()
            config.set_admin_password("long-password", iterations=10)
            config.set_provider_key("openai", "sk-secret")
            config.update_public({"active_provider": "openai"})

            reloaded = ConfigService(root)
            reloaded.load()
            self.assertTrue(reloaded.verify_admin_password("long-password"))
            self.assertFalse(reloaded.verify_admin_password("wrong-password"))
            self.assertTrue(reloaded.public_view()["keys_configured"]["openai"])
            self.assertNotIn("sk-secret", str(reloaded.public_view()))
            self.assertEqual(reloaded.config["active_provider"], "openai")

    def test_setup_ap_password_persists_and_stays_private(self):
        with tempfile.TemporaryDirectory() as root:
            first = ConfigService(root)
            first.load()
            password = first.setup_ap_password()
            self.assertGreaterEqual(len(password), 8)

            reloaded = ConfigService(root)
            reloaded.load()
            self.assertEqual(reloaded.setup_ap_password(), password)
            self.assertNotIn(password, str(reloaded.public_view()))

            reloaded.set_setup_ap_password("fixed-local-password")
            final = ConfigService(root)
            final.load()
            self.assertEqual(final.setup_ap_password(), "fixed-local-password")
            self.assertNotIn("fixed-local-password", str(final.public_view()))

    def test_device_service_closes_command_loop_with_simulator(self):
        async def scenario():
            link = SimulatedSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            task = asyncio.create_task(device.run())
            try:
                await device.wait_connected()
                await device.set_mode(protocol_ids.MODE_MANUAL)
                await device.set_runtime_config(400, 600, 200)
                result = await device.move(40, 40, 400, timeout_ms=1000)
                self.assertTrue(result["ok"])
                self.assertEqual(result["left_steps"], 40)
                await device.stop()
            finally:
                device.running = False
                await asyncio.sleep(0.06)
                await task

        asyncio.run(scenario())

    def test_device_service_clears_estop_with_simulator(self):
        async def scenario():
            link = SimulatedSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            task = asyncio.create_task(device.run())
            try:
                await device.wait_connected()
                await device.stop()
                self.assertEqual(link.simulator.state, protocol_ids.ROBOTSTATE_ESTOP)
                device.status.update(
                    state=protocol_ids.ROBOTSTATE_ESTOP,
                    selected_mode=protocol_ids.MODE_IDLE,
                    fault_code=protocol_ids.ERRORCODE_LINK_LOST,
                )
                command_id = await device.clear_estop()
                self.assertGreater(command_id, 0)
                self.assertEqual(link.simulator.state, protocol_ids.ROBOTSTATE_IDLE)
                self.assertEqual(device.status["state"], protocol_ids.ROBOTSTATE_IDLE)
                self.assertEqual(device.status["fault_code"], 0)
            finally:
                device.running = False
                await asyncio.sleep(0.06)
                await task

        asyncio.run(scenario())

    def test_device_service_marks_a_lost_spi_link_offline(self):
        async def scenario():
            link = SwitchableSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            task = asyncio.create_task(device.run())
            try:
                await device.wait_connected(timeout_ms=2500)
                link.available = False
                await asyncio.sleep(0.9)
                self.assertFalse(device.status["connected"])
                self.assertGreater(device.status["rx_errors"], 0)
            finally:
                device.running = False
                await asyncio.sleep(0.06)
                await task

        asyncio.run(scenario())

    def test_device_service_resource_update_closes_with_simulator(self):
        async def scenario():
            link = SimulatedSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            task = asyncio.create_task(device.run())
            frame = bytes(range(256)) * 4
            package = resource_format.build_package(
                (
                    resource_format.ClipSource(
                        expression_id=resource_format.EXPRESSION_IDS["HAPPY"],
                        weight=1,
                        frame_interval_ms=100,
                        frames=(frame,),
                    ),
                )
            )
            update_id = 0xA100
            try:
                await device.wait_connected()
                await device.resource_begin(
                    update_id,
                    len(package),
                    resource_format.verify_package(package).package_crc32,
                )
                for offset in range(0, len(package), 238):
                    await device.resource_chunk(
                        update_id, offset, package[offset : offset + 238]
                    )
                status = await device.get_resource_status(update_id)
                self.assertEqual(status["next_offset"], len(package))
                self.assertEqual(status["state"], protocol_ids.RESOURCESTATE_RECEIVING)

                await device.resource_finish(update_id)
                observed_states = set()
                for _attempt in range(32):
                    status = await device.get_resource_status(update_id)
                    observed_states.add(status["state"])
                    if status["state"] == protocol_ids.RESOURCESTATE_IDLE:
                        break
                self.assertIn(protocol_ids.RESOURCESTATE_VERIFYING, observed_states)
                self.assertIn(protocol_ids.RESOURCESTATE_COMMITTING, observed_states)
                self.assertEqual(status["state"], protocol_ids.RESOURCESTATE_IDLE)
                self.assertEqual(status["active_bank"], 0)
                self.assertEqual(status["generation"], 1)
                self.assertEqual(device.status["resource"], status)
            finally:
                device.running = False
                await asyncio.sleep(0.12)
                await task

        asyncio.run(scenario())

    def test_device_service_rejects_invalid_resource_chunks_locally(self):
        device = DeviceService(SimulatedSpiLink(), boot_id=0x45535031)

        async def scenario():
            with self.assertRaisesRegex(ValueError, "1..238"):
                await device.resource_chunk(1, 0, b"")
            with self.assertRaisesRegex(ValueError, "1..238"):
                await device.resource_chunk(1, 0, bytes(239))
            with self.assertRaisesRegex(ValueError, "out of range"):
                await device.resource_chunk(1, 0xFFFFFFFF, b"xx")

        asyncio.run(scenario())

    def test_command_timeout_cancels_and_discards_late_ack_or_nack(self):
        async def scenario():
            for message_type in (protocol_ids.MSG_ACK, protocol_ids.MSG_NACK):
                with self.subTest(message_type=message_type):
                    device = DeviceService(SimulatedSpiLink(), boot_id=0x45535031)
                    command_id = 77
                    with self.assertRaisesRegex(
                        DeviceCommandError, "timed out"
                    ) as context:
                        await device.command(
                            protocol_ids.MSG_SET_MODE,
                            (command_id, protocol_ids.MODE_IDLE),
                            command_id,
                            timeout_ms=0,
                        )
                    self.assertTrue(context.exception.timed_out)
                    seq = device.mailbox._seq
                    self.assertFalse(device.mailbox.cancel(seq))
                    self.assertIn(seq, device._ignored_results)
                    self.assertNotIn(seq, device._waiting_results)

                    device._process_slot(
                        {
                            "type": message_type,
                            "values": (
                                seq,
                                command_id,
                                protocol_ids.ERRORCODE_OK,
                            ),
                        }
                    )
                    self.assertNotIn(seq, device._results)
                    self.assertNotIn(seq, device._ignored_results)

        asyncio.run(scenario())

    def test_command_cancellation_removes_mailbox_state_and_late_ack(self):
        async def scenario():
            device = DeviceService(RecordingSpiLink(), boot_id=0x45535031)
            command_id = 77
            task = asyncio.create_task(
                device.command(
                    protocol_ids.MSG_SET_MODE,
                    (command_id, protocol_ids.MODE_IDLE),
                    command_id,
                    timeout_ms=1000,
                )
            )
            await asyncio.sleep(0)
            seq = device.mailbox._queue[0]["seq"]

            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task

            self.assertFalse(
                any(item["seq"] == seq for item in device.mailbox._queue)
            )
            self.assertNotIn(seq, device._waiting_results)
            self.assertNotIn(seq, device._results)
            self.assertIn(seq, device._ignored_results)

            device._process_slot(
                {
                    "type": protocol_ids.MSG_ACK,
                    "values": (seq, command_id, protocol_ids.ERRORCODE_OK),
                }
            )
            self.assertNotIn(seq, device._results)
            self.assertNotIn(seq, device._ignored_results)

        asyncio.run(scenario())

    def test_command_cancelled_after_ack_does_not_leave_a_result(self):
        async def scenario():
            link = RecordingSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            command_id = 78
            task = asyncio.create_task(
                device.command(
                    protocol_ids.MSG_SET_MODE,
                    (command_id, protocol_ids.MODE_IDLE),
                    command_id,
                    timeout_ms=1000,
                )
            )
            await asyncio.sleep(0)
            seq = device.mailbox._queue[0]["seq"]
            device.mailbox.poll(0)
            deliver_command_result(
                device, protocol_ids.MSG_ACK, seq, command_id
            )
            self.assertIn(seq, device._results)

            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task

            self.assertNotIn(seq, device._results)
            self.assertNotIn(seq, device._waiting_results)
            self.assertIn(seq, device._ignored_results)

        asyncio.run(scenario())

    def test_move_ack_timeout_cancels_move_and_sends_one_urgent_stop(self):
        class FastAckTimeoutDevice(DeviceService):
            async def command(
                self, message_type, values, command_id, timeout_ms=600
            ):
                return await DeviceService.command(
                    self, message_type, values, command_id, timeout_ms=0
                )

        async def scenario():
            link = RecordingSpiLink()
            device = FastAckTimeoutDevice(link, boot_id=0x45535031)
            with self.assertRaises(DeviceCommandError) as context:
                await device.move(20, 20, 200, timeout_ms=500)
            self.assertTrue(context.exception.timed_out)

            self.assertFalse(
                any(
                    item["type"] == protocol_ids.MSG_MOVE_WHEELS
                    for item in device.mailbox._queue
                )
            )
            self.assertIsNotNone(device.mailbox._urgent)
            self.assertEqual(
                device.mailbox._urgent["type"], protocol_ids.MSG_STOP
            )
            stop_seq = device.mailbox._urgent["seq"]
            self.assertEqual(device._queue_best_effort_stop(), stop_seq)

            device.mailbox.poll(0)
            self.assertEqual(link.transactions[-1]["type"], protocol_ids.MSG_STOP)
            self.assertIsNone(device.mailbox._urgent)
            self.assertEqual(device.mailbox._pending["seq"], stop_seq)
            self.assertFalse(device._motion_results)
            self.assertFalse(device._motion_waiters)

        asyncio.run(scenario())

    def test_move_wait_timeout_queues_stop_and_discards_late_motion(self):
        class FastMotionTimeoutDevice(DeviceService):
            async def wait_motion(self, command_id, timeout_ms=2500):
                return await DeviceService.wait_motion(
                    self, command_id, timeout_ms=0
                )

        async def scenario():
            link = RecordingSpiLink()
            device = FastMotionTimeoutDevice(link, boot_id=0x45535031)
            task = asyncio.create_task(
                device.move(20, 20, 200, timeout_ms=500)
            )
            await asyncio.sleep(0)
            move_item = device.mailbox._queue[0]
            move_seq = move_item["seq"]
            command_id = move_item["command_id"]
            device.mailbox.poll(0)
            deliver_command_result(
                device, protocol_ids.MSG_ACK, move_seq, command_id
            )

            with self.assertRaises(DeviceCommandError) as context:
                await task
            self.assertTrue(context.exception.timed_out)
            self.assertEqual(
                device.mailbox._urgent["type"], protocol_ids.MSG_STOP
            )
            self.assertNotIn(command_id, device._motion_waiters)
            self.assertNotIn(command_id, device._motion_results)
            self.assertIn(command_id, device._ignored_motion_results)

            device._process_slot(
                {
                    "type": protocol_ids.MSG_MOTION_DONE,
                    "values": (command_id, 20, 20, 0),
                }
            )
            self.assertNotIn(command_id, device._motion_results)
            self.assertNotIn(command_id, device._ignored_motion_results)

        asyncio.run(scenario())

    def test_move_cancelled_while_waiting_queues_stop_and_cleans_motion(self):
        async def scenario():
            link = RecordingSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            task = asyncio.create_task(
                device.move(20, 20, 200, timeout_ms=500)
            )
            await asyncio.sleep(0)
            move_item = device.mailbox._queue[0]
            move_seq = move_item["seq"]
            command_id = move_item["command_id"]
            device.mailbox.poll(0)
            deliver_command_result(
                device, protocol_ids.MSG_ACK, move_seq, command_id
            )
            await asyncio.sleep(0.02)
            self.assertFalse(task.done())

            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task

            self.assertEqual(
                device.mailbox._urgent["type"], protocol_ids.MSG_STOP
            )
            self.assertNotIn(command_id, device._motion_waiters)
            self.assertNotIn(command_id, device._motion_results)
            self.assertIn(command_id, device._ignored_motion_results)

        asyncio.run(scenario())

    def test_unmatched_ack_or_nack_does_not_create_a_result(self):
        for message_type in (protocol_ids.MSG_ACK, protocol_ids.MSG_NACK):
            with self.subTest(message_type=message_type):
                device = DeviceService(SimulatedSpiLink(), boot_id=0x45535031)
                expected_seq = 23
                expected_command_id = 77
                device._waiting_results[expected_seq] = expected_command_id

                device._process_slot(
                    {
                        "type": message_type,
                        "values": (
                            expected_seq + 1,
                            expected_command_id,
                            protocol_ids.ERRORCODE_OK,
                        ),
                    }
                )
                device._process_slot(
                    {
                        "type": message_type,
                        "values": (
                            expected_seq,
                            expected_command_id + 1,
                            protocol_ids.ERRORCODE_OK,
                        ),
                    }
                )

                self.assertFalse(device._results)
                self.assertEqual(
                    device._waiting_results[expected_seq], expected_command_id
                )

                device._process_slot(
                    {
                        "type": message_type,
                        "values": (
                            expected_seq,
                            expected_command_id,
                            protocol_ids.ERRORCODE_OK,
                        ),
                    }
                )
                self.assertIn(expected_seq, device._results)
                self.assertNotIn(expected_seq, device._waiting_results)

    def test_ignored_result_set_is_bounded(self):
        device = DeviceService(SimulatedSpiLink(), boot_id=0x45535031)
        for seq in range(1, device._ignored_capacity + 6):
            device._ignore_result(seq)
        self.assertEqual(len(device._ignored_results), device._ignored_capacity)
        self.assertIn(device._ignored_capacity + 5, device._ignored_results)

    def test_motion_results_require_a_waiter_and_ignored_set_is_bounded(self):
        device = DeviceService(SimulatedSpiLink(), boot_id=0x45535031)
        device._process_slot(
            {
                "type": protocol_ids.MSG_MOTION_DONE,
                "values": (90, 1, 1, 0),
            }
        )
        self.assertFalse(device._motion_results)

        device._motion_waiters[91] = True
        device._process_slot(
            {
                "type": protocol_ids.MSG_MOTION_ABORTED,
                "values": (92, protocol_ids.ABORTREASON_STOP),
            }
        )
        self.assertFalse(device._motion_results)
        self.assertIn(91, device._motion_waiters)

        device._process_slot(
            {
                "type": protocol_ids.MSG_MOTION_DONE,
                "values": (91, 4, 5, 0),
            }
        )
        self.assertEqual(device._motion_results[91]["left_steps"], 4)
        self.assertNotIn(91, device._motion_waiters)

        for command_id in range(100, 100 + device._ignored_motion_capacity + 6):
            device._ignore_motion_result(command_id)
        self.assertEqual(
            len(device._ignored_motion_results),
            device._ignored_motion_capacity,
        )
        self.assertIn(
            100 + device._ignored_motion_capacity + 5,
            device._ignored_motion_results,
        )

    def test_coil_diagnostic_waits_for_correlated_result(self):
        async def scenario():
            link = RecordingSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            device.status["capabilities"] = 0x20
            task = asyncio.create_task(
                device.diagnose_coil(
                    protocol_ids.COILWHEEL_RIGHT,
                    protocol_ids.COILCHANNEL_A,
                    3000,
                )
            )
            await asyncio.sleep(0)
            item = device.mailbox._queue[0]
            self.assertEqual(item["type"], protocol_ids.MSG_COIL_DIAGNOSTIC)
            self.assertEqual(
                unpack_payload(item["type"], item["payload"]),
                (
                    item["command_id"],
                    protocol_ids.COILWHEEL_RIGHT,
                    protocol_ids.COILCHANNEL_A,
                    3000,
                ),
            )

            device.mailbox.poll(0)
            deliver_command_result(
                device,
                protocol_ids.MSG_ACK,
                item["seq"],
                item["command_id"],
            )
            await asyncio.sleep(0)
            device._process_slot(
                {
                    "type": protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT,
                    "values": (
                        item["command_id"],
                        protocol_ids.COILDIAGNOSTICRESULT_DONE,
                    ),
                }
            )
            result = await task
            self.assertTrue(result["ok"])
            self.assertEqual(
                result["result"], protocol_ids.COILDIAGNOSTICRESULT_DONE
            )
            self.assertFalse(device._coil_diagnostic_waiters)
            self.assertFalse(device._coil_diagnostic_results)

        asyncio.run(scenario())

    def test_coil_diagnostic_rejects_invalid_or_unsupported_locally(self):
        async def scenario():
            device = DeviceService(RecordingSpiLink(), boot_id=0x45535031)
            with self.assertRaises(DeviceCommandError):
                await device.diagnose_coil(
                    protocol_ids.COILWHEEL_RIGHT,
                    protocol_ids.COILCHANNEL_A,
                    3000,
                )

            device.status["capabilities"] = 0x20
            for wheel, channel, duration in (
                (2, protocol_ids.COILCHANNEL_A, 3000),
                (protocol_ids.COILWHEEL_RIGHT, 4, 3000),
                (protocol_ids.COILWHEEL_RIGHT, "A", 3000),
                (protocol_ids.COILWHEEL_RIGHT, protocol_ids.COILCHANNEL_A, 99),
                (protocol_ids.COILWHEEL_RIGHT, protocol_ids.COILCHANNEL_A, 3001),
                (protocol_ids.COILWHEEL_RIGHT, protocol_ids.COILCHANNEL_A, 1.5),
            ):
                with self.subTest(
                    wheel=wheel, channel=channel, duration=duration
                ):
                    with self.assertRaises(ValueError):
                        await device.diagnose_coil(wheel, channel, duration)
            self.assertFalse(device.mailbox._queue)

        asyncio.run(scenario())

    def test_coil_diagnostic_cancel_queues_stop_and_cleans_waiter(self):
        async def scenario():
            link = RecordingSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            device.status["capabilities"] = 0x20
            task = asyncio.create_task(
                device.diagnose_coil(
                    protocol_ids.COILWHEEL_RIGHT,
                    protocol_ids.COILCHANNEL_B,
                    3000,
                )
            )
            await asyncio.sleep(0)
            item = device.mailbox._queue[0]
            device.mailbox.poll(0)
            deliver_command_result(
                device,
                protocol_ids.MSG_ACK,
                item["seq"],
                item["command_id"],
            )
            await asyncio.sleep(0.02)
            self.assertFalse(task.done())

            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertEqual(
                device.mailbox._urgent["type"], protocol_ids.MSG_STOP
            )
            self.assertNotIn(
                item["command_id"], device._coil_diagnostic_waiters
            )
            self.assertIn(
                item["command_id"],
                device._ignored_coil_diagnostic_results,
            )

        asyncio.run(scenario())

    def test_coil_results_require_waiter_and_ignored_set_is_bounded(self):
        device = DeviceService(SimulatedSpiLink(), boot_id=0x45535031)
        device._process_slot(
            {
                "type": protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT,
                "values": (90, protocol_ids.COILDIAGNOSTICRESULT_DONE),
            }
        )
        self.assertFalse(device._coil_diagnostic_results)

        device._coil_diagnostic_waiters[91] = True
        device._process_slot(
            {
                "type": protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT,
                "values": (92, protocol_ids.COILDIAGNOSTICRESULT_DONE),
            }
        )
        self.assertFalse(device._coil_diagnostic_results)
        self.assertIn(91, device._coil_diagnostic_waiters)

        device._process_slot(
            {
                "type": protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT,
                "values": (91, protocol_ids.COILDIAGNOSTICRESULT_ABORTED),
            }
        )
        self.assertFalse(device._coil_diagnostic_results[91]["ok"])
        self.assertNotIn(91, device._coil_diagnostic_waiters)

        for command_id in range(
            100, 100 + device._ignored_coil_diagnostic_capacity + 6
        ):
            device._ignore_coil_diagnostic_result(command_id)
        self.assertEqual(
            len(device._ignored_coil_diagnostic_results),
            device._ignored_coil_diagnostic_capacity,
        )

    def test_coil_wait_timeout_queues_stop_and_discards_late_result(self):
        class FastCoilTimeoutDevice(DeviceService):
            async def wait_coil_diagnostic(self, command_id, timeout_ms=3600):
                return await DeviceService.wait_coil_diagnostic(
                    self, command_id, timeout_ms=0
                )

        async def scenario():
            link = RecordingSpiLink()
            device = FastCoilTimeoutDevice(link, boot_id=0x45535031)
            device.status["capabilities"] = 0x20
            task = asyncio.create_task(
                device.diagnose_coil(
                    protocol_ids.COILWHEEL_RIGHT,
                    protocol_ids.COILCHANNEL_A,
                    3000,
                )
            )
            await asyncio.sleep(0)
            item = device.mailbox._queue[0]
            device.mailbox.poll(0)
            deliver_command_result(
                device,
                protocol_ids.MSG_ACK,
                item["seq"],
                item["command_id"],
            )

            with self.assertRaises(DeviceCommandError) as context:
                await task
            self.assertTrue(context.exception.timed_out)
            self.assertEqual(
                device.mailbox._urgent["type"], protocol_ids.MSG_STOP
            )
            self.assertIn(
                item["command_id"],
                device._ignored_coil_diagnostic_results,
            )

            device._process_slot(
                {
                    "type": protocol_ids.MSG_COIL_DIAGNOSTIC_RESULT,
                    "values": (
                        item["command_id"],
                        protocol_ids.COILDIAGNOSTICRESULT_DONE,
                    ),
                }
            )
            self.assertNotIn(
                item["command_id"],
                device._ignored_coil_diagnostic_results,
            )
            self.assertNotIn(
                item["command_id"], device._coil_diagnostic_results
            )

        asyncio.run(scenario())

    def test_resource_status_waiters_are_correlated_by_request_seq(self):
        async def scenario():
            device = DeviceService(SimulatedSpiLink(), boot_id=0x45535031)
            device.connected = True
            first = asyncio.create_task(device.get_resource_status(11, timeout_ms=1000))
            second = asyncio.create_task(device.get_resource_status(22, timeout_ms=1000))
            await asyncio.sleep(0)
            queued = list(device.mailbox._queue)
            self.assertEqual(len(queued), 2)
            first_seq = queued[0]["seq"]
            second_seq = queued[1]["seq"]

            device._process_slot(
                {
                    "type": protocol_ids.MSG_RESOURCE_STATUS,
                    "values": (11, protocol_ids.RESOURCESTATE_READY, 0xFF, 0, 0, 100, 0),
                },
                request_seq=first_seq,
            )
            await asyncio.sleep(0.02)
            self.assertTrue(first.done())
            self.assertFalse(second.done())
            self.assertEqual((await first)["update_id"], 11)

            device._process_slot(
                {
                    "type": protocol_ids.MSG_RESOURCE_STATUS,
                    "values": (22, protocol_ids.RESOURCESTATE_RECEIVING, 0xFF, 0, 50, 100, 0),
                },
                request_seq=second_seq,
            )
            self.assertEqual((await second)["update_id"], 22)
            self.assertFalse(device._resource_status_results)
            device.mailbox.cancel(first_seq)
            device.mailbox.cancel(second_seq)

        asyncio.run(scenario())

    def test_cancelled_resource_status_waiter_cleans_mailbox_and_results(self):
        async def scenario():
            device = DeviceService(SimulatedSpiLink(), boot_id=0x45535031)
            device.connected = True
            task = asyncio.create_task(
                device.get_resource_status(11, timeout_ms=1000)
            )
            await asyncio.sleep(0)
            seq = device.mailbox._queue[0]["seq"]
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task

            self.assertNotIn(seq, device._resource_status_waiters)
            self.assertNotIn(seq, device._resource_status_results)
            self.assertFalse(device.mailbox.cancel(seq))
            device._process_slot(
                {
                    "type": protocol_ids.MSG_RESOURCE_STATUS,
                    "values": (
                        11,
                        protocol_ids.RESOURCESTATE_READY,
                        0xFF,
                        0,
                        0,
                        100,
                        0,
                    ),
                },
                request_seq=seq,
            )
            self.assertNotIn(seq, device._resource_status_results)

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
