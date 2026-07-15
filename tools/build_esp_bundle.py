#!/usr/bin/env python3
"""Build the MicroPython filesystem bundle without host-only files."""

import argparse
import hashlib
import json
import shutil
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
APP_ASSET_VERSION_PLACEHOLDER = "__APP_ASSET_VERSION__"


def copy_python_tree(source, destination):
    shutil.copytree(
        source,
        destination,
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )


def version_web_assets(web_root):
    javascript = (web_root / "app.js").read_bytes()
    version = hashlib.sha256(javascript).hexdigest()[:12]
    index_path = web_root / "index.html"
    html = index_path.read_text(encoding="utf-8")
    if html.count(APP_ASSET_VERSION_PLACEHOLDER) != 1:
        raise ValueError("index.html must contain exactly one app asset version placeholder")
    index_path.write_text(
        html.replace(APP_ASSET_VERSION_PLACEHOLDER, version),
        encoding="utf-8",
    )


def build_bundle(output):
    output = Path(output).resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    (output / "firmware").mkdir()
    shutil.copy2(REPOSITORY / "firmware" / "__init__.py", output / "firmware" / "__init__.py")
    copy_python_tree(REPOSITORY / "firmware" / "esp32", output / "firmware" / "esp32")

    (output / "protocol" / "generated").mkdir(parents=True)
    shutil.copy2(REPOSITORY / "protocol" / "__init__.py", output / "protocol" / "__init__.py")
    for filename in ("__init__.py", "protocol_ids.py"):
        shutil.copy2(
            REPOSITORY / "protocol" / "generated" / filename,
            output / "protocol" / "generated" / filename,
        )

    shutil.copytree(REPOSITORY / "web", output / "www")
    version_web_assets(output / "www")
    shutil.copy2(REPOSITORY / "firmware" / "esp32" / "boot.py", output / "boot.py")
    shutil.copy2(REPOSITORY / "firmware" / "esp32" / "main.py", output / "main.py")

    files = sorted(path.relative_to(output).as_posix() for path in output.rglob("*") if path.is_file())
    manifest = {
        "target": "YD-ESP32-S3-N16R8",
        "micropython": "v1.28.0 ESP32_GENERIC_S3-SPIRAM_OCT",
        "files": files,
    }
    (output / "bundle-manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8"
    )
    total = sum(path.stat().st_size for path in output.rglob("*") if path.is_file())
    print("ESP32 bundle: {} files, {} bytes -> {}".format(len(files) + 1, total, output))
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default=REPOSITORY / "dist" / "esp32")
    args = parser.parse_args()
    build_bundle(args.output)


if __name__ == "__main__":
    main()
