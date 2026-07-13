#!/usr/bin/env python3
"""Fail when the STM32F103C8 firmware exceeds flash or RAM budgets."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

FLASH_LIMIT = 64 * 1024
RAM_LIMIT = 20 * 1024
STACK_RESERVE = 2 * 1024


def find_size_tool():
    tool = shutil.which("arm-none-eabi-size")
    if tool:
        return tool
    root = os.environ.get("AIROBOT_ARM_GCC_ROOT")
    if root:
        candidate = Path(root) / "usr/bin/arm-none-eabi-size"
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError("arm-none-eabi-size not found")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    output = subprocess.check_output([find_size_tool(), str(args.elf)], text=True)
    lines = [line.split() for line in output.splitlines() if line.strip()]
    if len(lines) < 2 or len(lines[-1]) < 3:
        raise RuntimeError("unexpected arm-none-eabi-size output")
    text_size, data_size, bss_size = map(int, lines[-1][:3])
    flash_used = text_size + data_size
    ram_used = data_size + bss_size + STACK_RESERVE
    print("flash: {}/{} bytes; ram+stack: {}/{} bytes".format(
        flash_used, FLASH_LIMIT, ram_used, RAM_LIMIT
    ))
    if flash_used > FLASH_LIMIT or ram_used > RAM_LIMIT:
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(2)
