"""Encode and decode fixed-size SPI mailbox slots."""

import struct

from protocol.generated import protocol_ids

from .crc16 import crc16_ccitt

HEADER_FORMAT = "<BBHHH"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
PAYLOAD_OFFSET = 2 + HEADER_SIZE
CRC_OFFSET = protocol_ids.SLOT_SIZE - 2


class SlotError(ValueError):
    pass


def pack_payload(message_type, values=None):
    fmt = protocol_ids.MESSAGE_FORMATS.get(message_type)
    if fmt is None:
        raise SlotError("unknown message type")
    if values is None:
        values = ()
    try:
        return struct.pack(fmt, *values)
    except (TypeError, ValueError, struct.error) as exc:
        raise SlotError("invalid payload values") from exc


def unpack_payload(message_type, payload):
    fmt = protocol_ids.MESSAGE_FORMATS.get(message_type)
    expected = protocol_ids.MESSAGE_LENGTHS.get(message_type)
    if fmt is None or expected is None:
        raise SlotError("unknown message type")
    if len(payload) != expected:
        raise SlotError("invalid payload length")
    return struct.unpack(fmt, payload)


def encode_slot(message_type, seq, flags=0, payload=b""):
    if message_type not in protocol_ids.MESSAGE_LENGTHS:
        raise SlotError("unknown message type")
    if len(payload) != protocol_ids.MESSAGE_LENGTHS[message_type]:
        raise SlotError("payload length does not match message")
    if not 0 <= seq <= 0xFFFF or not 0 <= flags <= 0xFF:
        raise SlotError("slot field out of range")

    slot = bytearray(protocol_ids.SLOT_SIZE)
    slot[0:2] = protocol_ids.MAGIC
    struct.pack_into(
        HEADER_FORMAT,
        slot,
        2,
        protocol_ids.PROTOCOL_VERSION,
        flags,
        message_type,
        seq,
        len(payload),
    )
    slot[PAYLOAD_OFFSET : PAYLOAD_OFFSET + len(payload)] = payload
    struct.pack_into("<H", slot, CRC_OFFSET, crc16_ccitt(slot[2:CRC_OFFSET]))
    return bytes(slot)


def decode_slot(slot):
    if len(slot) != protocol_ids.SLOT_SIZE:
        raise SlotError("invalid slot size")
    if bytes(slot[0:2]) != protocol_ids.MAGIC:
        raise SlotError("invalid magic")

    version, flags, message_type, seq, length = struct.unpack_from(HEADER_FORMAT, slot, 2)
    if version != protocol_ids.PROTOCOL_VERSION:
        raise SlotError("unsupported protocol version")
    expected_length = protocol_ids.MESSAGE_LENGTHS.get(message_type)
    if expected_length is None:
        raise SlotError("unknown message type")
    if length != expected_length or length > protocol_ids.PAYLOAD_SIZE:
        raise SlotError("invalid payload length")
    if any(slot[PAYLOAD_OFFSET + length : CRC_OFFSET]):
        raise SlotError("non-zero payload padding")

    expected_crc = struct.unpack_from("<H", slot, CRC_OFFSET)[0]
    actual_crc = crc16_ccitt(slot[2:CRC_OFFSET])
    if expected_crc != actual_crc:
        raise SlotError("CRC mismatch")

    payload = bytes(slot[PAYLOAD_OFFSET : PAYLOAD_OFFSET + length])
    return {
        "version": version,
        "flags": flags,
        "type": message_type,
        "seq": seq,
        "payload": payload,
        "values": unpack_payload(message_type, payload),
    }
