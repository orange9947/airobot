#!/usr/bin/env python3
"""Build, inspect, and verify OLED expression resource packages."""

import argparse
import json
from pathlib import Path

if __package__:
    from . import resource_format
else:
    import resource_format


def _unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise resource_format.ResourceFormatError(
                "manifest contains duplicate key {!r}".format(key)
            )
        result[key] = value
    return result


def _load_manifest(path):
    try:
        manifest = json.loads(
            Path(path).read_text(encoding="utf-8"), object_pairs_hook=_unique_json_object
        )
    except UnicodeDecodeError as error:
        raise resource_format.ResourceFormatError("manifest must be UTF-8 JSON") from error
    except json.JSONDecodeError as error:
        raise resource_format.ResourceFormatError(
            "invalid manifest JSON at line {}, column {}".format(error.lineno, error.colno)
        ) from error
    if not isinstance(manifest, dict):
        raise resource_format.ResourceFormatError("manifest root must be an object")
    if set(manifest) != {"clips"}:
        raise resource_format.ResourceFormatError(
            "manifest root must contain only the 'clips' field"
        )
    if not isinstance(manifest["clips"], list):
        raise resource_format.ResourceFormatError("manifest clips must be a list")
    return manifest


def png_to_frame(path):
    """Convert a 128x64 PNG to SSD1306 page-major bytes."""
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError as error:
        raise resource_format.ResourceFormatError(
            "Pillow is required for PNG input; install requirements-tools.txt"
        ) from error

    path = Path(path)
    try:
        with Image.open(path) as image:
            if image.format != "PNG":
                raise resource_format.ResourceFormatError(
                    "frame source must be a PNG file: {}".format(path)
                )
            if image.size != (resource_format.WIDTH, resource_format.HEIGHT):
                raise resource_format.ResourceFormatError(
                    "PNG dimensions must be exactly 128 x 64: {}".format(path)
                )
            rgba = image.convert("RGBA")
            if hasattr(rgba, "get_flattened_data"):
                pixels = tuple(rgba.get_flattened_data())
            else:
                pixels = tuple(rgba.getdata())
    except resource_format.ResourceFormatError:
        raise
    except (OSError, UnidentifiedImageError) as error:
        raise resource_format.ResourceFormatError(
            "cannot read PNG frame {}: {}".format(path, error)
        ) from error

    frame = bytearray(resource_format.FRAME_SIZE)
    for y in range(resource_format.HEIGHT):
        row = y * resource_format.WIDTH
        page = (y // 8) * resource_format.WIDTH
        bit = 1 << (y & 7)
        for x in range(resource_format.WIDTH):
            red, green, blue, alpha = pixels[row + x]
            if alpha < 128:
                continue
            luminance = (299 * red + 587 * green + 114 * blue + 500) // 1000
            if luminance >= 128:
                frame[page + x] |= bit
    return bytes(frame)


def build_from_manifest(manifest_path):
    manifest_path = Path(manifest_path)
    manifest = _load_manifest(manifest_path)
    clips = manifest["clips"]
    if not 1 <= len(clips) <= resource_format.MAX_CLIPS:
        raise resource_format.ResourceFormatError("clip count must be in the range 1..32")

    required_fields = {"expression", "weight", "frame_interval_ms", "frames"}
    sources = []
    total_frames = 0
    for index, clip in enumerate(clips):
        if not isinstance(clip, dict):
            raise resource_format.ResourceFormatError("clip {} must be an object".format(index))
        if set(clip) != required_fields:
            raise resource_format.ResourceFormatError(
                "clip {} must contain exactly expression, weight, frame_interval_ms, and frames".format(
                    index
                )
            )
        expression = clip["expression"]
        if not isinstance(expression, str) or expression not in resource_format.EXPRESSION_IDS:
            raise resource_format.ResourceFormatError(
                "clip {} expression must be one of {}".format(
                    index, ", ".join(resource_format.EXPRESSION_IDS)
                )
            )
        frames = clip["frames"]
        if not isinstance(frames, list) or not frames:
            raise resource_format.ResourceFormatError(
                "clip {} frames must be a non-empty list".format(index)
            )
        if any(not isinstance(frame, str) or not frame for frame in frames):
            raise resource_format.ResourceFormatError(
                "clip {} frame paths must be non-empty strings".format(index)
            )
        total_frames += len(frames)
        if total_frames > resource_format.MAX_FRAMES:
            raise resource_format.ResourceFormatError("frame count must be in the range 1..256")

        # Validate scalar fields before opening files so malformed manifests fail predictably.
        resource_format.validate_clip_values(
            resource_format.EXPRESSION_IDS[expression],
            clip["weight"],
            clip["frame_interval_ms"],
            len(frames),
        )
        decoded_frames = tuple(png_to_frame(manifest_path.parent / frame) for frame in frames)
        sources.append(
            resource_format.ClipSource(
                expression_id=resource_format.EXPRESSION_IDS[expression],
                weight=clip["weight"],
                frame_interval_ms=clip["frame_interval_ms"],
                frames=decoded_frames,
            )
        )
    return resource_format.build_package(sources)


def inspect_package(package):
    if not isinstance(package, resource_format.ResourcePackage):
        package = resource_format.parse_package(package)
    return {
        "magic": resource_format.MAGIC.decode("ascii"),
        "version": package.version,
        "header_size": package.header_size,
        "width": package.width,
        "height": package.height,
        "clip_count": len(package.clips),
        "frame_count": len(package.frames),
        "clip_table_offset": package.clip_table_offset,
        "frame_table_offset": package.frame_table_offset,
        "data_offset": package.data_offset,
        "total_size": package.total_size,
        "package_crc32": "{:08x}".format(package.package_crc32),
        "clips": [
            {
                "expression": resource_format.EXPRESSION_NAMES[clip.expression_id],
                "expression_id": clip.expression_id,
                "weight": clip.weight,
                "frame_interval_ms": clip.frame_interval_ms,
                "frame_count": clip.frame_count,
                "first_frame_index": clip.first_frame_index,
            }
            for clip in package.clips
        ],
        "frames": [
            {
                "encoding": resource_format.ENCODING_NAMES[frame.encoding],
                "data_offset": frame.data_offset,
                "encoded_length": frame.encoded_length,
                "decoded_length": frame.decoded_length,
                "frame_crc32": "{:08x}".format(frame.frame_crc32),
            }
            for frame in package.frames
        ],
    }


def _read_and_parse(path):
    return resource_format.parse_package(Path(path).read_bytes())


def _build_parser():
    parser = argparse.ArgumentParser(
        description="Build, inspect, and verify AI robot OLED expression resources"
    )
    commands = parser.add_subparsers(dest="command", required=True)

    build = commands.add_parser("build", help="build an .arp package from a JSON manifest")
    build.add_argument("manifest", type=Path)
    build.add_argument("output", type=Path)

    inspect = commands.add_parser("inspect", help="print validated package metadata as JSON")
    inspect.add_argument("package", type=Path)

    verify = commands.add_parser("verify", help="validate a package and every decoded frame")
    verify.add_argument("package", type=Path)
    return parser


def main(argv=None):
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            package_bytes = build_from_manifest(args.manifest)
            args.output.write_bytes(package_bytes)
            package = resource_format.parse_package(package_bytes)
            print(
                "built {} bytes, {} clips, {} frames, CRC32 {:08x} -> {}".format(
                    package.total_size,
                    len(package.clips),
                    len(package.frames),
                    package.package_crc32,
                    args.output,
                )
            )
        elif args.command == "inspect":
            print(json.dumps(inspect_package(_read_and_parse(args.package)), indent=2, sort_keys=True))
        else:
            package = _read_and_parse(args.package)
            print(
                "valid ARPK v{}: {} bytes, {} clips, {} frames, CRC32 {:08x}".format(
                    package.version,
                    package.total_size,
                    len(package.clips),
                    len(package.frames),
                    package.package_crc32,
                )
            )
    except (OSError, resource_format.ResourceFormatError) as error:
        parser.exit(1, "error: {}\n".format(error))
    return 0


if __name__ == "__main__":
    main()
