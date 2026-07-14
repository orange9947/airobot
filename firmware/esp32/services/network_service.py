"""Wi-Fi station connection with a protected setup access-point fallback."""

import binascii

from firmware.esp32.core.compat import sleep_ms, ticks_diff, ticks_ms

try:
    import network
    from machine import unique_id
except ImportError:
    network = None

    def unique_id():
        return b"HOST"


class NetworkService:
    def __init__(self, config, network_module=None):
        self.config = config
        self.network = network_module or network
        self.mode = "offline"
        self.ip = "0.0.0.0"
        suffix = binascii.hexlify(unique_id()[-2:]).decode().upper()
        self.ap_ssid = "Robot-" + suffix
        self.ap_password = config.setup_ap_password()

    async def connect_station(self, timeout_ms=15000):
        if self.network is None:
            return False
        wifi = self.config.config.get("wifi", {})
        ssid = wifi.get("ssid", "")
        if not ssid:
            return False
        station = self.network.WLAN(self.network.STA_IF)
        station.active(True)
        station.connect(ssid, wifi.get("password", ""))
        started = ticks_ms()
        while not station.isconnected() and ticks_diff(ticks_ms(), started) < timeout_ms:
            await sleep_ms(250)
        if not station.isconnected():
            station.active(False)
            return False
        self.mode = "station"
        self.ip = station.ifconfig()[0]
        return True

    def start_access_point(self):
        if self.network is None:
            self.mode = "host"
            self.ip = "127.0.0.1"
            return
        access_point = self.network.WLAN(self.network.AP_IF)
        access_point.active(True)
        access_point.config(essid=self.ap_ssid, password=self.ap_password, authmode=3)
        self.mode = "access_point"
        self.ip = access_point.ifconfig()[0]
        print("Setup Wi-Fi: {} / {} / http://{}".format(self.ap_ssid, self.ap_password, self.ip))

    async def ensure_connected(self):
        if not await self.connect_station():
            self.start_access_point()
        return self.status()

    def status(self):
        return {"mode": self.mode, "ip": self.ip, "setup_ssid": self.ap_ssid if self.mode == "access_point" else ""}
