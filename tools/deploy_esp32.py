#!/usr/bin/env python3
"""Deploy the generated filesystem bundle over mpremote."""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from build_esp_bundle import build_bundle


TARGETS = ("firmware", "protocol", "www", "boot.py", "main.py", "bundle-manifest.json")


def run_mpremote(port, arguments, check=True):
    command = ["mpremote", "connect", port] + list(arguments)
    return subprocess.run(command, check=check)


def main():
    parser = argparse.ArgumentParser(description="Deploy AI robot MicroPython files")
    parser.add_argument("--port", required=True, help="serial port, for example /dev/ttyACM0")
    parser.add_argument("--no-reset", action="store_true", help="do not reset after copying")
    args = parser.parse_args()

    if shutil.which("mpremote") is None:
        sys.exit("mpremote is missing; install it with: python3 -m pip install mpremote")

    with tempfile.TemporaryDirectory(prefix="airobot-esp32-") as temporary:
        bundle = build_bundle(Path(temporary) / "bundle")
        for target in TARGETS:
            run_mpremote(args.port, ["fs", "rm", "-r", ":/" + target], check=False)
        for target in TARGETS:
            run_mpremote(args.port, ["fs", "cp", "-r", str(bundle / target), ":/"])

    if not args.no_reset:
        run_mpremote(args.port, ["reset"])
    print("Deployment complete. Existing /config credentials were preserved.")


if __name__ == "__main__":
    main()
