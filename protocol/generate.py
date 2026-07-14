#!/usr/bin/env python3
"""Generate deterministic protocol constants and golden SPI slots."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SCHEMA_PATH = ROOT / "messages.json"
GENERATED_DIR = ROOT / "generated"
GOLDEN_DIR = ROOT / "golden"

TYPE_FORMATS = {
    "u8": "B",
    "i8": "b",
    "u16": "H",
    "i16": "h",
    "u32": "I",
    "i32": "i",
}


def field_format(field: dict) -> str:
    field_type = field["type"]
    if field_type == "bytes":
        length = field.get("length")
        if isinstance(length, bool) or not isinstance(length, int) or length <= 0:
            raise ValueError("bytes field requires positive length")
        return f"{length}s"
    if field_type not in TYPE_FORMATS:
        raise ValueError(f"unknown field type: {field_type}")
    return TYPE_FORMATS[field_type]


def decode_bytes_sample(message: dict, field: dict) -> bytes:
    sample = message["sample"][field["name"]]
    expected_length = field["length"]
    if not isinstance(sample, str):
        raise ValueError(f"invalid bytes sample: {message['name']}.{field['name']}")
    if len(sample) != expected_length * 2:
        raise ValueError(f"bytes sample length must be {expected_length}: {message['name']}.{field['name']}")
    try:
        value = bytes.fromhex(sample)
    except ValueError as exc:
        raise ValueError(f"invalid bytes sample: {message['name']}.{field['name']}") from exc
    if len(value) != expected_length:
        raise ValueError(f"bytes sample length must be {expected_length}: {message['name']}.{field['name']}")
    return value


def crc16_ccitt(data: bytes, initial: int = 0xFFFF) -> int:
    crc = initial
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def load_schema() -> dict:
    with SCHEMA_PATH.open("r", encoding="utf-8") as handle:
        schema = json.load(handle)
    validate_schema(schema)
    return schema


def message_format(message: dict) -> str:
    return "<" + "".join(field_format(field) for field in message["fields"])


def validate_schema(schema: dict) -> None:
    protocol = schema["protocol"]
    payload_size = protocol["payload_size"]
    expected_slot = 2 + 1 + 1 + 2 + 2 + 2 + payload_size + 2
    if protocol["slot_size"] != expected_slot:
        raise ValueError(f"slot_size must be {expected_slot}")
    if len(protocol["magic"]) != 2 or any(not 0 <= value <= 255 for value in protocol["magic"]):
        raise ValueError("magic must contain two bytes")

    ids = set()
    names = set()
    for message in schema["messages"]:
        if message["id"] in ids:
            raise ValueError(f"duplicate message id: {message['id']}")
        if message["name"] in names:
            raise ValueError(f"duplicate message name: {message['name']}")
        ids.add(message["id"])
        names.add(message["name"])
        fields_by_name = {field["name"]: field for field in message["fields"]}
        for field in message["fields"]:
            field_format(field)
            if field["name"] not in message["sample"]:
                raise ValueError(f"missing sample field: {message['name']}.{field['name']}")
            if field["type"] == "bytes":
                value = decode_bytes_sample(message, field)
                length_field = fields_by_name.get(f"{field['name']}_length")
                if length_field is not None:
                    used_length = message["sample"][length_field["name"]]
                    if (
                        isinstance(used_length, bool)
                        or not isinstance(used_length, int)
                        or not 0 <= used_length <= field["length"]
                    ):
                        raise ValueError(
                            f"bytes length field out of range: {message['name']}.{length_field['name']}"
                        )
                    if any(value[used_length:]):
                        raise ValueError(
                            f"non-zero bytes padding: {message['name']}.{field['name']}"
                        )
        length = struct.calcsize(message_format(message))
        if length > payload_size:
            raise ValueError(f"payload too large: {message['name']}")


def encode_sample_slot(schema: dict, message: dict, seq: int) -> tuple[bytes, bytes]:
    protocol = schema["protocol"]
    values = [
        decode_bytes_sample(message, field)
        if field["type"] == "bytes"
        else message["sample"][field["name"]]
        for field in message["fields"]
    ]
    payload = struct.pack(message_format(message), *values)
    slot = bytearray(protocol["slot_size"])
    slot[0:2] = bytes(protocol["magic"])
    flags = 0 if message["name"] == "NOOP" else 1
    struct.pack_into("<BBHHH", slot, 2, protocol["version"], flags, message["id"], seq, len(payload))
    slot[10 : 10 + len(payload)] = payload
    crc = crc16_ccitt(slot[2:-2])
    struct.pack_into("<H", slot, len(slot) - 2, crc)
    return payload, bytes(slot)


def render_python(schema: dict) -> str:
    protocol = schema["protocol"]
    lines = [
        '"""Generated from protocol/messages.json. Do not edit."""',
        "",
        f"PROTOCOL_VERSION = {protocol['version']}",
        f"MAGIC = bytes(({protocol['magic'][0]}, {protocol['magic'][1]}))",
        f"PAYLOAD_SIZE = {protocol['payload_size']}",
        f"SLOT_SIZE = {protocol['slot_size']}",
        "",
    ]
    for enum_name, values in schema["enums"].items():
        lines.append(f"# {enum_name}")
        for name, value in values.items():
            lines.append(f"{enum_name.upper()}_{name} = {value}")
        lines.append("")
    for message in sorted(schema["messages"], key=lambda item: item["id"]):
        lines.append(f"MSG_{message['name']} = 0x{message['id']:04X}")
    lines.extend(["", "MESSAGE_FORMATS = {"])
    for message in sorted(schema["messages"], key=lambda item: item["id"]):
        lines.append(f"    MSG_{message['name']}: {message_format(message)!r},")
    lines.extend(["}", "", "MESSAGE_LENGTHS = {"])
    for message in sorted(schema["messages"], key=lambda item: item["id"]):
        length = struct.calcsize(message_format(message))
        lines.append(f"    MSG_{message['name']}: {length},")
    lines.extend(["}", ""])
    return "\n".join(lines)


def render_c_ids(schema: dict) -> str:
    protocol = schema["protocol"]
    lines = [
        "/* Generated from protocol/messages.json. Do not edit. */",
        "#ifndef ROBOT_PROTOCOL_IDS_H",
        "#define ROBOT_PROTOCOL_IDS_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define ROBOT_PROTOCOL_VERSION {protocol['version']}u",
        f"#define ROBOT_SPI_MAGIC_0 0x{protocol['magic'][0]:02X}u",
        f"#define ROBOT_SPI_MAGIC_1 0x{protocol['magic'][1]:02X}u",
        f"#define ROBOT_SPI_PAYLOAD_SIZE {protocol['payload_size']}u",
        f"#define ROBOT_SPI_SLOT_SIZE {protocol['slot_size']}u",
        "",
    ]
    for enum_name, values in schema["enums"].items():
        lines.append(f"/* {enum_name} */")
        for name, value in values.items():
            lines.append(f"#define ROBOT_{enum_name.upper()}_{name} {value}u")
        lines.append("")
    lines.extend(["typedef enum {", "    ROBOT_MSG_NOOP = 0x0000,"])
    for message in sorted(schema["messages"], key=lambda item: item["id"]):
        if message["name"] == "NOOP":
            continue
        lines.append(f"    ROBOT_MSG_{message['name']} = 0x{message['id']:04X},")
    lines.extend(["} robot_message_type_t;", "", "#endif", ""])
    return "\n".join(lines)


def render_c_layouts(schema: dict) -> str:
    lines = [
        "/* Generated from protocol/messages.json. Do not edit. */",
        "#ifndef ROBOT_PROTOCOL_LAYOUTS_H",
        "#define ROBOT_PROTOCOL_LAYOUTS_H",
        "",
    ]
    for message in sorted(schema["messages"], key=lambda item: item["id"]):
        length = struct.calcsize(message_format(message))
        lines.append(f"#define ROBOT_PAYLOAD_LEN_{message['name']} {length}u")
    lines.extend(["", "#endif", ""])
    return "\n".join(lines)


def render_golden(schema: dict) -> str:
    cases = []
    for seq, message in enumerate(sorted(schema["messages"], key=lambda item: item["id"]), start=1):
        payload, slot = encode_sample_slot(schema, message, seq)
        cases.append(
            {
                "name": message["name"],
                "type": message["id"],
                "seq": seq,
                "fields": message["sample"],
                "payload_hex": payload.hex(),
                "slot_hex": slot.hex(),
            }
        )
    return json.dumps({"cases": cases}, indent=2, sort_keys=True) + "\n"


def render_c_golden(schema: dict) -> str:
    lines = [
        "/* Generated from protocol/messages.json. Do not edit. */",
        "#ifndef ROBOT_PROTOCOL_GOLDEN_H",
        "#define ROBOT_PROTOCOL_GOLDEN_H",
        "",
        "#include <stdint.h>",
        '#include "protocol_ids.h"',
        "",
    ]
    for seq, message in enumerate(sorted(schema["messages"], key=lambda item: item["id"]), start=1):
        _, slot = encode_sample_slot(schema, message, seq)
        name = message["name"]
        lines.append(f"static const uint8_t ROBOT_GOLDEN_{name}[ROBOT_SPI_SLOT_SIZE] = {{")
        for offset in range(0, len(slot), 16):
            chunk = ", ".join(f"0x{value:02X}" for value in slot[offset : offset + 16])
            lines.append(f"    {chunk},")
        lines.extend(["};", ""])
    lines.extend(["#endif", ""])
    return "\n".join(lines)


def desired_outputs(schema: dict) -> dict[Path, str]:
    return {
        GENERATED_DIR / "protocol_ids.py": render_python(schema),
        GENERATED_DIR / "protocol_ids.h": render_c_ids(schema),
        GENERATED_DIR / "protocol_layouts.h": render_c_layouts(schema),
        GENERATED_DIR / "protocol_golden.h": render_c_golden(schema),
        GOLDEN_DIR / "golden_slots.json": render_golden(schema),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs = desired_outputs(load_schema())
    if args.check:
        stale = [str(path.relative_to(ROOT.parent)) for path, content in outputs.items() if not path.exists() or path.read_text(encoding="utf-8") != content]
        if stale:
            print("generated files are stale: " + ", ".join(stale), file=sys.stderr)
            return 1
        return 0
    for path, content in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
