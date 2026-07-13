#!/usr/bin/env python3
"""Minimal diagnostic console for the TCP STM simulator."""

import argparse
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from firmware.esp32.transport.frame_codec import decode_slot, encode_slot, pack_payload
from protocol.generated import protocol_ids
from tools.stm_simulator import recv_exact


def exchange(connection, tx):
    connection.sendall(tx)
    rx = recv_exact(connection, protocol_ids.SLOT_SIZE)
    if rx is None:
        raise ConnectionError("simulator disconnected")
    return decode_slot(rx)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tcp", default="127.0.0.1:9001")
    parser.add_argument("command", choices=("hello", "state", "manual", "ai", "stop"))
    args = parser.parse_args()
    host, port_text = args.tcp.rsplit(":", 1)

    commands = {
        "hello": (protocol_ids.MSG_HELLO_REQ, (0x45535031, 1)),
        "state": (protocol_ids.MSG_GET_STATE, ()),
        "manual": (protocol_ids.MSG_SET_MODE, (1, protocol_ids.MODE_MANUAL)),
        "ai": (protocol_ids.MSG_SET_MODE, (2, protocol_ids.MODE_AI)),
        "stop": (protocol_ids.MSG_STOP, (3, protocol_ids.ABORTREASON_STOP)),
    }
    message_type, values = commands[args.command]
    payload = pack_payload(message_type, values)
    request = encode_slot(message_type, 1, protocol_ids.SLOTFLAGS_ACK_REQUEST, payload)
    noop = encode_slot(protocol_ids.MSG_NOOP, 2)

    with socket.create_connection((host, int(port_text)), timeout=2) as connection:
        first = exchange(connection, request)
        print("first:", first)
        for _ in range(6):
            response = exchange(connection, noop)
            print("next:", response)
            if response["type"] != protocol_ids.MSG_NOOP:
                break
            time.sleep(0.01)


if __name__ == "__main__":
    main()
