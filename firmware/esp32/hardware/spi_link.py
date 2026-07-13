"""YD-ESP32-S3 SPI2 master link using the final GPIO assignment."""

try:
    from machine import Pin, SPI
except ImportError:
    Pin = None
    SPI = None

from protocol.generated import protocol_ids


class Stm32SpiLink:
    def __init__(self, spi=None, chip_select=None, baudrate=1_000_000):
        if spi is None:
            if SPI is None:
                raise RuntimeError("machine.SPI is only available on the ESP32")
            spi = SPI(
                2,
                baudrate=baudrate,
                polarity=0,
                phase=0,
                bits=8,
                firstbit=SPI.MSB,
                sck=Pin(12),
                mosi=Pin(11),
                miso=Pin(13),
            )
        if chip_select is None:
            if Pin is None:
                raise RuntimeError("machine.Pin is only available on the ESP32")
            chip_select = Pin(10, Pin.OUT, value=1)
        self.spi = spi
        self.cs = chip_select
        self.rx = bytearray(protocol_ids.SLOT_SIZE)

    def exchange(self, tx):
        if len(tx) != protocol_ids.SLOT_SIZE:
            raise ValueError("SPI transaction must be one protocol slot")
        self.cs.value(0)
        try:
            self.spi.write_readinto(tx, self.rx)
        finally:
            self.cs.value(1)
        return bytes(self.rx)
