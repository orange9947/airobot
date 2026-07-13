"""ESP32-S3 application entry point."""

import binascii

from firmware.esp32.core.compat import asyncio, sleep_ms
from firmware.esp32.hardware.spi_link import Stm32SpiLink
from firmware.esp32.services.config_service import ConfigService
from firmware.esp32.services.device_service import DeviceService
from firmware.esp32.services.http_client import AsyncJsonClient
from firmware.esp32.services.llm_service import LlmService
from firmware.esp32.services.network_service import NetworkService
from firmware.esp32.services.web_service import WebService

try:
    from machine import unique_id
except ImportError:
    def unique_id():
        return b"HOST"


def boot_id():
    raw = binascii.hexlify(unique_id())[-8:]
    return int(raw, 16)


async def sync_runtime_config(config, device):
    applied = None
    while True:
        motion = config.config["motion"]
        desired = (
            int(motion["soft_rate_sps"]),
            int(motion["accel_sps2"]),
            int(motion["hold_ms"]),
        )
        if device.connected and desired != applied:
            try:
                await device.set_runtime_config(*desired)
                applied = desired
            except Exception as exc:
                print("Runtime config sync failed: {}".format(str(exc)[:80]))
        await sleep_ms(1000)


async def application():
    config = ConfigService()
    config.load()
    network = NetworkService(config)
    await network.ensure_connected()

    device = DeviceService(Stm32SpiLink(), boot_id())
    llm = LlmService(config, device, AsyncJsonClient())
    web = WebService(config, device, llm, network)
    device.add_listener(web.publish)
    llm.add_listener(web.publish)

    asyncio.create_task(device.run())
    asyncio.create_task(sync_runtime_config(config, device))
    await web.start(port=80)
    print("Robot console: http://{}".format(network.ip))
    while True:
        await sleep_ms(1000)


if __name__ == "__main__":
    asyncio.run(application())
