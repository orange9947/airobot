import asyncio
import hashlib
import tempfile
import unittest

from firmware.esp32.core.security import pbkdf2_sha256, pbkdf2_sha256_async
from firmware.esp32.services.config_service import ConfigService
from firmware.esp32.services.device_service import DeviceService
from protocol.generated import protocol_ids
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


class EspServiceTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
