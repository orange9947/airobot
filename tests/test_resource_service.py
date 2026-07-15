import asyncio
import binascii
import unittest

from firmware.esp32.services.device_service import DeviceCommandError, DeviceService
from firmware.esp32.services.resource_service import ResourceService, ResourceServiceError
from protocol.generated import protocol_ids
from tools import resource_format
from tools.stm_simulator import RobotSimulator


class FakeClock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        return self.now & 0xFFFFFFFF

    def advance(self, milliseconds):
        self.now = (self.now + milliseconds) & 0xFFFFFFFF


class FakeDevice:
    def __init__(self):
        self.connected = True
        self.calls = []
        self.listeners = []
        self.update_id = 0
        self.next_offset = 0
        self.total_size = 0
        self.state = protocol_ids.RESOURCESTATE_IDLE
        self.error = protocol_ids.RESOURCEERROR_NONE
        self.fail_method = None
        self.fail_as_timeout = False
        self.fail_message = "private resource bytes must not leak"

    def add_listener(self, listener):
        self.listeners.append(listener)

    def disconnect(self):
        self.connected = False
        for listener in tuple(self.listeners):
            listener({"type": "link", "connected": False})

    def _maybe_fail(self, method):
        if self.fail_method == method:
            self.fail_method = None
            if self.fail_as_timeout:
                self.fail_as_timeout = False
                error = DeviceCommandError(self.fail_message)
                error.timed_out = True
                raise error
            raise RuntimeError(self.fail_message)

    async def resource_begin(self, update_id, size, crc, format_version):
        self._maybe_fail("begin")
        self.calls.append(("begin", update_id, size, crc, format_version))
        self.update_id = update_id
        self.next_offset = 0
        self.total_size = size
        self.state = protocol_ids.RESOURCESTATE_READY
        self.error = protocol_ids.RESOURCEERROR_NONE
        return {"ok": True}

    async def resource_chunk(self, update_id, offset, data):
        self._maybe_fail("chunk")
        self.calls.append(("chunk", update_id, offset, bytes(data)))
        self.update_id = update_id
        self.next_offset = offset + len(data)
        self.state = protocol_ids.RESOURCESTATE_RECEIVING
        self.error = protocol_ids.RESOURCEERROR_NONE
        return {"ok": True}

    async def resource_finish(self, update_id):
        self._maybe_fail("finish")
        self.calls.append(("finish", update_id))
        self.state = protocol_ids.RESOURCESTATE_IDLE
        return {"ok": True}

    async def resource_abort(self, update_id):
        self._maybe_fail("abort")
        self.calls.append(("abort", update_id))
        self.state = protocol_ids.RESOURCESTATE_ABORTED
        return {"ok": True}

    async def get_resource_status(self, update_id=0):
        self._maybe_fail("status")
        self.calls.append(("status", update_id))
        return {
            "update_id": self.update_id,
            "state": self.state,
            "active_bank": 0xFF,
            "generation": 0,
            "next_offset": self.next_offset,
            "total_size": self.total_size,
            "error": self.error,
        }


class BlockingDevice(FakeDevice):
    def __init__(self):
        super().__init__()
        self.chunk_started = asyncio.Event()
        self.release_chunk = asyncio.Event()

    async def resource_chunk(self, update_id, offset, data):
        self.chunk_started.set()
        await self.release_chunk.wait()
        return await super().resource_chunk(update_id, offset, data)


class SimulatedSpiLink:
    def __init__(self):
        self.simulator = RobotSimulator()
        self.now_ms = 0

    def exchange(self, tx):
        self.now_ms += 10
        return self.simulator.transact(tx, self.now_ms)


class ResourceServiceTests(unittest.TestCase):
    @staticmethod
    def assert_error_code(context, code):
        error = context.exception
        if error.code != code:
            raise AssertionError("expected error code {!r}, got {!r}".format(code, error.code))
        return error

    def test_begin_is_single_session_and_ids_are_not_reused(self):
        async def scenario():
            clock = FakeClock()
            device = FakeDevice()
            service = ResourceService(device, clock=clock)

            first = await service.begin(5000, 0x12345678, 1)
            self.assertEqual(first["update_id"], 1)
            with self.assertRaises(ResourceServiceError) as context:
                await service.begin(10, 0, 1)
            self.assert_error_code(context, "busy")

            await service.abort(first["update_id"])
            second = await service.begin(10, 0, 1)
            self.assertEqual(second["update_id"], 2)
            self.assertEqual([call[1] for call in device.calls if call[0] == "begin"], [1, 2])

        asyncio.run(scenario())

    def test_http_chunk_is_split_into_238_byte_device_chunks(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            session = await service.begin(5000, 0, 1)
            body = bytes(index & 0xFF for index in range(4096))

            result = await service.write_chunk(session["update_id"], 0, body)

            chunks = [call for call in device.calls if call[0] == "chunk"]
            self.assertEqual([len(call[3]) for call in chunks], [238] * 17 + [50])
            self.assertEqual([call[2] for call in chunks], list(range(0, 4096, 238)))
            self.assertEqual(b"".join(call[3] for call in chunks), body)
            self.assertEqual(result["next_offset"], 4096)
            self.assertEqual(service.local_status()["next_offset"], 4096)
            self.assertNotIn("data", service._session)

        asyncio.run(scenario())

    def test_chunk_bounds_and_offsets_fail_before_device_io(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            update_id = (await service.begin(10, 0, 1))["update_id"]

            cases = [
                (1, b"a", "offset_mismatch"),
                (0, b"", "invalid_request"),
                (0, b"a" * 4097, "chunk_too_large"),
                (0, b"a" * 11, "package_overflow"),
            ]
            for offset, data, code in cases:
                with self.subTest(code=code):
                    with self.assertRaises(ResourceServiceError) as context:
                        await service.write_chunk(update_id, offset, data)
                    self.assert_error_code(context, code)
            self.assertFalse([call for call in device.calls if call[0] == "chunk"])

            with self.assertRaises(ResourceServiceError) as context:
                await service.write_chunk(True, 0, b"a")
            self.assert_error_code(context, "invalid_request")
            with self.assertRaises(ResourceServiceError) as context:
                await service.status(-1)
            self.assert_error_code(context, "invalid_request")

        asyncio.run(scenario())

    def test_uncertain_chunk_requires_status_before_resume(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            update_id = (await service.begin(500, 0, 1))["update_id"]
            device.fail_method = "chunk"
            device.fail_as_timeout = True

            with self.assertRaises(ResourceServiceError) as context:
                await service.write_chunk(update_id, 0, b"a" * 238)
            error = self.assert_error_code(context, "device_error")
            self.assertNotIn(device.fail_message, str(error))
            self.assertIsNone(error.__context__)

            with self.assertRaises(ResourceServiceError) as context:
                await service.write_chunk(update_id, 0, b"a")
            self.assert_error_code(context, "status_required")

            device.next_offset = 238
            status = await service.status(update_id)
            self.assertEqual(status["next_offset"], 238)
            result = await service.write_chunk(update_id, 238, b"b" * 10)
            self.assertEqual(result["next_offset"], 248)

        asyncio.run(scenario())

    def test_finish_requires_complete_upload_and_waits_for_terminal_status(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            update_id = (await service.begin(4, 0, 1))["update_id"]

            with self.assertRaises(ResourceServiceError) as context:
                await service.finish(update_id)
            self.assert_error_code(context, "incomplete")
            await service.write_chunk(update_id, 0, b"data")
            result = await service.finish(update_id)
            self.assertEqual(result["update_id"], update_id)
            self.assertTrue(result["accepted"])
            self.assertEqual(service.active_update_id, update_id)
            self.assertTrue(service.local_status()["finishing"])

            with self.assertRaises(ResourceServiceError) as context:
                await service.write_chunk(update_id, 4, b"x")
            self.assert_error_code(context, "finishing")
            with self.assertRaises(ResourceServiceError) as context:
                await service.finish(update_id)
            self.assert_error_code(context, "finishing")
            with self.assertRaises(ResourceServiceError) as context:
                await service.begin(1, 0, 1)
            self.assert_error_code(context, "busy")

            status = await service.status(update_id)
            self.assertEqual(status["state"], protocol_ids.RESOURCESTATE_IDLE)
            self.assertIsNone(service.active_update_id)

        asyncio.run(scenario())

    def test_finish_acceptance_can_still_be_explicitly_aborted(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            update_id = (await service.begin(4, 0, 1))["update_id"]
            await service.write_chunk(update_id, 0, b"data")
            await service.finish(update_id)

            result = await service.abort(update_id)
            self.assertTrue(result["aborted"])
            self.assertIn(("abort", update_id), device.calls)
            self.assertIsNone(service.active_update_id)

        asyncio.run(scenario())

    def test_timeout_aborts_remote_and_reports_stable_error(self):
        async def scenario():
            clock = FakeClock()
            device = FakeDevice()
            service = ResourceService(device, clock=clock)
            update_id = (await service.begin(10, 0, 1))["update_id"]
            clock.advance(60000)

            reason = await service.tick()
            self.assertEqual(reason, "session_timeout")
            self.assertIn(("abort", update_id), device.calls)
            self.assertIsNone(service.active_update_id)
            with self.assertRaises(ResourceServiceError) as context:
                await service.write_chunk(update_id, 0, b"a")
            self.assert_error_code(context, "session_timeout")

        asyncio.run(scenario())

    def test_uncertain_abort_keeps_session_until_status_reconciliation(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            update_id = (await service.begin(10, 0, 1))["update_id"]
            device.fail_method = "abort"

            with self.assertRaises(ResourceServiceError) as context:
                await service.abort(update_id)
            self.assert_error_code(context, "device_error")
            self.assertEqual(service.active_update_id, update_id)
            self.assertTrue(service.local_status()["aborting"])
            self.assertTrue(service.local_status()["needs_sync"])

            status = await service.status(update_id)
            self.assertEqual(status["state"], protocol_ids.RESOURCESTATE_READY)
            self.assertFalse(service.local_status()["aborting"])
            self.assertFalse(service.local_status()["needs_sync"])

            result = await service.abort(update_id)
            self.assertTrue(result["aborted"])
            self.assertIsNone(service.active_update_id)

        asyncio.run(scenario())

    def test_spi_disconnect_immediately_clears_local_session(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            update_id = (await service.begin(10, 0, 1))["update_id"]
            device.disconnect()

            self.assertIsNone(service.active_update_id)
            with self.assertRaises(ResourceServiceError) as context:
                await service.write_chunk(update_id, 0, b"a")
            self.assert_error_code(context, "link_lost")
            self.assertNotIn(("abort", update_id), device.calls)

        asyncio.run(scenario())

    def test_only_one_resource_operation_runs_at_a_time(self):
        async def scenario():
            device = BlockingDevice()
            service = ResourceService(device, clock=FakeClock())
            update_id = (await service.begin(20, 0, 1))["update_id"]
            first = asyncio.create_task(service.write_chunk(update_id, 0, b"a" * 10))
            await device.chunk_started.wait()
            try:
                with self.assertRaises(ResourceServiceError) as context:
                    await service.status(update_id)
                self.assert_error_code(context, "busy")
            finally:
                device.release_chunk.set()
                await first

        asyncio.run(scenario())

    def test_failed_begin_consumes_id_and_does_not_leak_device_error(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            device.fail_method = "begin"
            with self.assertRaises(ResourceServiceError) as context:
                await service.begin(10, 0, 1)
            error = self.assert_error_code(context, "device_error")
            self.assertNotIn(device.fail_message, str(error))
            self.assertIsNone(error.__context__)

            session = await service.begin(10, 0, 1)
            self.assertEqual(session["update_id"], 2)

        asyncio.run(scenario())

    def test_uncertain_begin_returns_claimable_session_and_reconciles(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())

            async def uncertain_begin(update_id, size, _crc, _version):
                device.update_id = update_id
                device.total_size = size
                device.state = protocol_ids.RESOURCESTATE_READY
                error = DeviceCommandError("timed out")
                error.timed_out = True
                raise error

            device.resource_begin = uncertain_begin
            provisional = await service.begin(100, 0, 1)
            self.assertTrue(provisional["active"])
            self.assertTrue(provisional["needs_sync"])
            self.assertTrue(provisional["beginning"])
            self.assertEqual(provisional["update_id"], 1)

            status = await service.status(provisional["update_id"])
            self.assertEqual(status["state"], protocol_ids.RESOURCESTATE_READY)
            self.assertFalse(service.local_status()["needs_sync"])
            self.assertFalse(service.local_status()["beginning"])

        asyncio.run(scenario())

    def test_status_reports_local_session_timeout_before_remote_status(self):
        async def scenario():
            clock = FakeClock()
            device = FakeDevice()
            service = ResourceService(device, clock=clock)
            update_id = (await service.begin(100, 0, 1))["update_id"]
            clock.advance(service.SESSION_TIMEOUT_MS)

            with self.assertRaises(ResourceServiceError) as context:
                await service.status(update_id)
            self.assert_error_code(context, "session_timeout")
            self.assertEqual(
                [call for call in device.calls if call[0] == "status"], []
            )

        asyncio.run(scenario())

    def test_device_nack_codes_map_to_stable_resource_errors(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())

            async def reject_begin(_update_id, _size, _crc, _version):
                raise DeviceCommandError(
                    "rejected", code=protocol_ids.ERRORCODE_BAD_STATE,
                    rejected=True,
                )

            device.resource_begin = reject_begin
            with self.assertRaises(ResourceServiceError) as context:
                await service.begin(100, 0, 1)
            self.assert_error_code(context, "busy")

            async def reject_payload(_update_id, _size, _crc, _version):
                raise DeviceCommandError(
                    "rejected", code=protocol_ids.ERRORCODE_BAD_PAYLOAD,
                    rejected=True,
                )

            device.resource_begin = reject_payload
            with self.assertRaises(ResourceServiceError) as context:
                await service.begin(100, 0, 1)
            self.assert_error_code(context, "invalid_request")

        asyncio.run(scenario())

    def test_invalid_remote_status_is_rejected(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            device.next_offset = 1
            device.total_size = 0
            with self.assertRaises(ResourceServiceError) as context:
                await service.status()
            self.assert_error_code(context, "invalid_status")

            device.next_offset = 0
            device.state = 0xFF
            with self.assertRaises(ResourceServiceError) as context:
                await service.status()
            self.assert_error_code(context, "invalid_status")

            device.state = protocol_ids.RESOURCESTATE_IDLE
            device.error = protocol_ids.RESOURCEERROR_INTERNAL + 1
            with self.assertRaises(ResourceServiceError) as context:
                await service.status()
            self.assert_error_code(context, "invalid_status")

            device.error = protocol_ids.RESOURCEERROR_NONE
            device.total_size = service.MAX_PACKAGE_SIZE + 1
            with self.assertRaises(ResourceServiceError) as context:
                await service.status()
            self.assert_error_code(context, "invalid_status")

        asyncio.run(scenario())

    def test_remote_session_mismatch_clears_local_session(self):
        async def scenario():
            device = FakeDevice()
            service = ResourceService(device, clock=FakeClock())
            update_id = (await service.begin(10, 0, 1))["update_id"]
            device.error = protocol_ids.RESOURCEERROR_SESSION_MISMATCH

            with self.assertRaises(ResourceServiceError) as context:
                await service.status(update_id)
            self.assert_error_code(context, "session_mismatch")
            self.assertIsNone(service.active_update_id)

        asyncio.run(scenario())

    def test_resource_service_closes_with_device_service_and_simulator(self):
        async def scenario():
            link = SimulatedSpiLink()
            device = DeviceService(link, boot_id=0x45535031)
            service = ResourceService(device)
            runner = asyncio.create_task(device.run())
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
            try:
                await device.wait_connected()
                update_id = (
                    await service.begin(
                        len(package),
                        resource_format.verify_package(package).package_crc32,
                    )
                )["update_id"]
                uploaded = await service.write_chunk(update_id, 0, package)
                self.assertEqual(uploaded["next_offset"], len(package))
                status = await service.status(update_id)
                self.assertEqual(status["state"], protocol_ids.RESOURCESTATE_RECEIVING)
                self.assertEqual(status["next_offset"], len(package))
                await service.finish(update_id)
                observed_states = set()
                for _attempt in range(32):
                    status = await service.status(update_id)
                    observed_states.add(status["state"])
                    if status["state"] == protocol_ids.RESOURCESTATE_IDLE:
                        break
                self.assertIn(protocol_ids.RESOURCESTATE_VERIFYING, observed_states)
                self.assertIn(protocol_ids.RESOURCESTATE_COMMITTING, observed_states)
                self.assertEqual(status["state"], protocol_ids.RESOURCESTATE_IDLE)
                self.assertEqual(status["active_bank"], 0)
                self.assertEqual(status["generation"], 1)
            finally:
                device.running = False
                await asyncio.sleep(0.12)
                await runner

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
