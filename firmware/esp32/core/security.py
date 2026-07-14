"""Password derivation and session-token helpers without external packages."""

import binascii
import hashlib
import os
import struct

from firmware.esp32.core.compat import sleep_ms


DEFAULT_PASSWORD_ITERATIONS = 2000
PASSWORD_YIELD_INTERVAL = 16


def _hmac_sha256(key, message):
    block_size = 64
    if len(key) > block_size:
        key = hashlib.sha256(key).digest()
    key = key + bytes(block_size - len(key))
    outer = bytes(value ^ 0x5C for value in key)
    inner = bytes(value ^ 0x36 for value in key)
    return hashlib.sha256(outer + hashlib.sha256(inner + message).digest()).digest()


def pbkdf2_sha256(password, salt, iterations=DEFAULT_PASSWORD_ITERATIONS):
    if iterations < 1:
        raise ValueError("iterations must be positive")
    if isinstance(password, str):
        password = password.encode("utf-8")
    block = _hmac_sha256(password, salt + struct.pack(">I", 1))
    result = bytearray(block)
    previous = block
    for _ in range(1, iterations):
        previous = _hmac_sha256(password, previous)
        for index, value in enumerate(previous):
            result[index] ^= value
    return bytes(result)


async def pbkdf2_sha256_async(
    password, salt, iterations=DEFAULT_PASSWORD_ITERATIONS,
    yield_every=PASSWORD_YIELD_INTERVAL,
):
    if iterations < 1:
        raise ValueError("iterations must be positive")
    if yield_every < 1:
        raise ValueError("yield interval must be positive")
    if isinstance(password, str):
        password = password.encode("utf-8")
    block = _hmac_sha256(password, salt + struct.pack(">I", 1))
    result = bytearray(block)
    previous = block
    for index in range(1, iterations):
        previous = _hmac_sha256(password, previous)
        for offset, value in enumerate(previous):
            result[offset] ^= value
        if index % yield_every == 0:
            await sleep_ms(0)
    return bytes(result)


def new_password_record(password, iterations=DEFAULT_PASSWORD_ITERATIONS):
    salt = os.urandom(16)
    derived = pbkdf2_sha256(password, salt, iterations)
    return {
        "salt": binascii.hexlify(salt).decode(),
        "hash": binascii.hexlify(derived).decode(),
        "iterations": iterations,
    }


async def new_password_record_async(password, iterations=DEFAULT_PASSWORD_ITERATIONS):
    salt = os.urandom(16)
    derived = await pbkdf2_sha256_async(password, salt, iterations)
    return {
        "salt": binascii.hexlify(salt).decode(),
        "hash": binascii.hexlify(derived).decode(),
        "iterations": iterations,
    }


def constant_time_equal(left, right):
    if len(left) != len(right):
        return False
    difference = 0
    for left_value, right_value in zip(left, right):
        difference |= left_value ^ right_value
    return difference == 0


def verify_password(password, record):
    try:
        salt = binascii.unhexlify(record["salt"])
        expected = binascii.unhexlify(record["hash"])
        actual = pbkdf2_sha256(password, salt, int(record["iterations"]))
        return constant_time_equal(actual, expected)
    except (KeyError, TypeError, ValueError):
        return False


async def verify_password_async(password, record):
    try:
        salt = binascii.unhexlify(record["salt"])
        expected = binascii.unhexlify(record["hash"])
        actual = await pbkdf2_sha256_async(password, salt, int(record["iterations"]))
        return constant_time_equal(actual, expected)
    except (KeyError, TypeError, ValueError):
        return False


def random_token(byte_count=24):
    return binascii.hexlify(os.urandom(byte_count)).decode()
