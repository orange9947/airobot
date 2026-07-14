#!/usr/bin/env python3
"""Reject ARM-mode code in the Thumb-only Cortex-M3 firmware."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def find_readelf():
    tool = shutil.which("arm-none-eabi-readelf")
    if tool:
        return tool
    root = os.environ.get("AIROBOT_ARM_GCC_ROOT")
    if root:
        candidate = Path(root) / "usr/bin/arm-none-eabi-readelf"
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError("arm-none-eabi-readelf not found")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    attributes = subprocess.check_output(
        [find_readelf(), "-A", str(args.elf)], text=True
    )
    if "Tag_ARM_ISA_use: Yes" in attributes:
        print("invalid Cortex-M3 ELF: ARM-mode code is present", file=sys.stderr)
        return 1
    if "Tag_THUMB_ISA_use:" not in attributes:
        print("invalid Cortex-M3 ELF: Thumb attributes are missing", file=sys.stderr)
        return 1
    print("ISA: Thumb-only Cortex-M3")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(2)
