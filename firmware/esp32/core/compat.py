"""Small CPython/MicroPython compatibility layer."""

try:
    import asyncio
except ImportError:
    import uasyncio as asyncio

try:
    from time import ticks_diff, ticks_ms
except ImportError:
    import time

    def ticks_ms():
        return int(time.monotonic() * 1000) & 0xFFFFFFFF

    def ticks_diff(current, previous):
        return ((current - previous + 0x80000000) & 0xFFFFFFFF) - 0x80000000


async def sleep_ms(milliseconds):
    if hasattr(asyncio, "sleep_ms"):
        await asyncio.sleep_ms(milliseconds)
    else:
        await asyncio.sleep(milliseconds / 1000)
