"""Expression resource package format, validation, CRC, and frame codecs."""

import binascii
import struct
from dataclasses import dataclass


MAGIC = b"ARPK"
FORMAT_VERSION = 1
WIDTH = 128
HEIGHT = 64
FRAME_SIZE = WIDTH * HEIGHT // 8

MAX_CLIPS = 32
MAX_FRAMES = 256
MAX_PACKAGE_SIZE = 504 * 1024
MIN_FRAME_INTERVAL_MS = 50
MAX_FRAME_INTERVAL_MS = 2000

ENCODING_RAW1 = 0
ENCODING_RLE1 = 1
ENCODING_NAMES = {
    ENCODING_RAW1: "RAW1",
    ENCODING_RLE1: "RLE1",
}

EXPRESSION_IDS = {
    "NEUTRAL": 0,
    "HAPPY": 1,
    "SAD": 2,
    "THINKING": 3,
    "SURPRISED": 4,
    "SLEEPY": 5,
}
EXPRESSION_NAMES = {value: name for name, value in EXPRESSION_IDS.items()}

HEADER_STRUCT = struct.Struct("<4sHHHHHHIIIII28s")
CLIP_STRUCT = struct.Struct("<BBHHHII")
FRAME_STRUCT = struct.Struct("<BBHIIIII")
PACKAGE_CRC32_OFFSET = 32
FRAME_CRC32_OFFSET = 16


class ResourceFormatError(ValueError):
    """Raised when a resource package or source value violates the format."""


@dataclass(frozen=True)
class ClipSource:
    expression_id: int
    weight: int
    frame_interval_ms: int
    frames: tuple


@dataclass(frozen=True)
class ClipEntry:
    expression_id: int
    weight: int
    frame_interval_ms: int
    frame_count: int
    first_frame_index: int


@dataclass(frozen=True)
class FrameEntry:
    encoding: int
    data_offset: int
    encoded_length: int
    decoded_length: int
    frame_crc32: int


@dataclass(frozen=True)
class ResourcePackage:
    version: int
    header_size: int
    width: int
    height: int
    clip_table_offset: int
    frame_table_offset: int
    data_offset: int
    total_size: int
    package_crc32: int
    clips: tuple
    frames: tuple
    package_bytes: bytes

    def decoded_frame(self, index):
        if isinstance(index, bool) or not isinstance(index, int):
            raise TypeError("frame index must be an integer")
        if index < 0 or index >= len(self.frames):
            raise IndexError("frame index out of range")
        frame = self.frames[index]
        encoded = self.package_bytes[
            frame.data_offset : frame.data_offset + frame.encoded_length
        ]
        return decode_frame(frame.encoding, encoded)


def crc32(data):
    """Return an unsigned IEEE CRC32."""
    return binascii.crc32(_as_bytes(data, "CRC32 input")) & 0xFFFFFFFF


def _as_bytes(value, label):
    if not isinstance(value, (bytes, bytearray, memoryview)):
        raise ResourceFormatError("{} must contain bytes".format(label))
    return bytes(value)


def _require_plain_int(value, label):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ResourceFormatError("{} must be an integer".format(label))
    return value


def validate_clip_values(expression_id, weight, frame_interval_ms, frame_count):
    expression_id = _require_plain_int(expression_id, "expression id")
    weight = _require_plain_int(weight, "clip weight")
    frame_interval_ms = _require_plain_int(frame_interval_ms, "frame interval")
    frame_count = _require_plain_int(frame_count, "clip frame count")
    if expression_id not in EXPRESSION_NAMES:
        raise ResourceFormatError("expression id must be in the range 0..5")
    if not 1 <= weight <= 255:
        raise ResourceFormatError("clip weight must be in the range 1..255")
    if not MIN_FRAME_INTERVAL_MS <= frame_interval_ms <= MAX_FRAME_INTERVAL_MS:
        raise ResourceFormatError("frame interval must be in the range 50..2000 ms")
    if not 1 <= frame_count <= MAX_FRAMES:
        raise ResourceFormatError("each clip must contain 1..256 frames")


def encode_rle(raw):
    """Encode bytes using deterministic RLE1 controls."""
    raw = _as_bytes(raw, "RLE1 input")
    output = bytearray()
    position = 0
    while position < len(raw):
        run_length = 1
        while (
            run_length < 128
            and position + run_length < len(raw)
            and raw[position + run_length] == raw[position]
        ):
            run_length += 1

        if run_length >= 3:
            output.append(0x80 | (run_length - 1))
            output.append(raw[position])
            position += run_length
            continue

        literal_start = position
        position += run_length
        while position < len(raw) and position - literal_start < 128:
            next_run = 1
            while (
                next_run < 128
                and position + next_run < len(raw)
                and raw[position + next_run] == raw[position]
            ):
                next_run += 1
            if next_run >= 3:
                break
            if position - literal_start + next_run > 128:
                break
            position += next_run

        literal_length = position - literal_start
        output.append(literal_length - 1)
        output.extend(raw[literal_start:position])

    return bytes(output)


def decode_rle(encoded, expected_length=FRAME_SIZE):
    """Strictly decode RLE1, requiring exact input and output consumption."""
    encoded = _as_bytes(encoded, "RLE1 input")
    expected_length = _require_plain_int(expected_length, "decoded length")
    if expected_length < 0:
        raise ResourceFormatError("decoded length cannot be negative")

    output = bytearray()
    position = 0
    while position < len(encoded):
        control = encoded[position]
        position += 1
        length = (control & 0x7F) + 1
        if control & 0x80:
            if position >= len(encoded):
                raise ResourceFormatError("truncated RLE1 repeat")
            if len(output) + length > expected_length:
                raise ResourceFormatError("RLE1 output exceeds decoded length")
            output.extend(bytes((encoded[position],)) * length)
            position += 1
        else:
            if len(encoded) - position < length:
                raise ResourceFormatError("truncated RLE1 literal")
            if len(output) + length > expected_length:
                raise ResourceFormatError("RLE1 output exceeds decoded length")
            output.extend(encoded[position : position + length])
            position += length

    if len(output) != expected_length:
        raise ResourceFormatError(
            "RLE1 decoded length is {}, expected {}".format(len(output), expected_length)
        )
    return bytes(output)


def encode_frame(raw):
    raw = _as_bytes(raw, "decoded frame")
    if len(raw) != FRAME_SIZE:
        raise ResourceFormatError("decoded frame must be exactly 1024 bytes")
    rle = encode_rle(raw)
    if len(rle) < len(raw):
        return ENCODING_RLE1, rle
    return ENCODING_RAW1, raw


def decode_frame(encoding, encoded):
    if encoding == ENCODING_RAW1:
        raw = _as_bytes(encoded, "RAW1 frame")
        if len(raw) != FRAME_SIZE:
            raise ResourceFormatError("RAW1 frame must be exactly 1024 bytes")
        return raw
    if encoding == ENCODING_RLE1:
        return decode_rle(encoded)
    raise ResourceFormatError("unsupported frame encoding {}".format(encoding))


def _check_region(offset, length, total_size, label):
    if offset < 0 or length < 0 or offset > total_size or length > total_size - offset:
        raise ResourceFormatError("{} is outside the package".format(label))


def _package_crc32(package):
    view = memoryview(package)
    checksum = binascii.crc32(view[:PACKAGE_CRC32_OFFSET])
    checksum = binascii.crc32(b"\x00\x00\x00\x00", checksum)
    checksum = binascii.crc32(view[PACKAGE_CRC32_OFFSET + 4 :], checksum)
    return checksum & 0xFFFFFFFF


def build_package(clip_sources):
    """Build a canonical version-1 package from decoded 1024-byte frames."""
    clip_sources = tuple(clip_sources)
    if not 1 <= len(clip_sources) <= MAX_CLIPS:
        raise ResourceFormatError("clip count must be in the range 1..32")

    clip_entries = []
    encoded_frames = []
    first_frame_index = 0
    for clip_index, source in enumerate(clip_sources):
        if not isinstance(source, ClipSource):
            raise ResourceFormatError("clip {} must be a ClipSource".format(clip_index))
        try:
            frames = tuple(source.frames)
        except TypeError as error:
            raise ResourceFormatError("clip {} frames must be iterable".format(clip_index)) from error
        validate_clip_values(
            source.expression_id,
            source.weight,
            source.frame_interval_ms,
            len(frames),
        )
        if first_frame_index + len(frames) > MAX_FRAMES:
            raise ResourceFormatError("frame count must be in the range 1..256")
        clip_entries.append(
            ClipEntry(
                expression_id=source.expression_id,
                weight=source.weight,
                frame_interval_ms=source.frame_interval_ms,
                frame_count=len(frames),
                first_frame_index=first_frame_index,
            )
        )
        for frame_index, raw in enumerate(frames):
            raw = _as_bytes(raw, "clip {} frame {}".format(clip_index, frame_index))
            encoding, encoded = encode_frame(raw)
            encoded_frames.append((encoding, encoded, crc32(raw)))
        first_frame_index += len(frames)

    clip_table_offset = HEADER_STRUCT.size
    frame_table_offset = clip_table_offset + len(clip_entries) * CLIP_STRUCT.size
    data_offset = frame_table_offset + len(encoded_frames) * FRAME_STRUCT.size
    total_size = data_offset + sum(len(encoded) for _, encoded, _ in encoded_frames)
    if total_size > MAX_PACKAGE_SIZE:
        raise ResourceFormatError("package exceeds the 504 KiB limit")

    package = bytearray(total_size)
    HEADER_STRUCT.pack_into(
        package,
        0,
        MAGIC,
        FORMAT_VERSION,
        HEADER_STRUCT.size,
        WIDTH,
        HEIGHT,
        len(clip_entries),
        len(encoded_frames),
        clip_table_offset,
        frame_table_offset,
        data_offset,
        total_size,
        0,
        bytes(28),
    )

    for index, clip in enumerate(clip_entries):
        CLIP_STRUCT.pack_into(
            package,
            clip_table_offset + index * CLIP_STRUCT.size,
            clip.expression_id,
            clip.weight,
            clip.frame_interval_ms,
            clip.frame_count,
            0,
            clip.first_frame_index,
            0,
        )

    write_offset = data_offset
    for index, (encoding, encoded, frame_crc32) in enumerate(encoded_frames):
        FRAME_STRUCT.pack_into(
            package,
            frame_table_offset + index * FRAME_STRUCT.size,
            encoding,
            0,
            0,
            write_offset,
            len(encoded),
            FRAME_SIZE,
            frame_crc32,
            0,
        )
        package[write_offset : write_offset + len(encoded)] = encoded
        write_offset += len(encoded)

    struct.pack_into("<I", package, PACKAGE_CRC32_OFFSET, _package_crc32(package))
    built = bytes(package)
    parse_package(built)
    return built


def parse_package(package):
    """Parse and fully validate a version-1 expression resource package."""
    package = _as_bytes(package, "package")
    if len(package) < HEADER_STRUCT.size:
        raise ResourceFormatError("package is shorter than the 64-byte header")
    if len(package) > MAX_PACKAGE_SIZE:
        raise ResourceFormatError("package exceeds the 504 KiB limit")

    (
        magic,
        version,
        header_size,
        width,
        height,
        clip_count,
        frame_count,
        clip_table_offset,
        frame_table_offset,
        data_offset,
        total_size,
        package_crc32,
        header_reserved,
    ) = HEADER_STRUCT.unpack_from(package)

    if magic != MAGIC:
        raise ResourceFormatError("package magic is not ARPK")
    if version != FORMAT_VERSION:
        raise ResourceFormatError("unsupported package version {}".format(version))
    if header_size != HEADER_STRUCT.size:
        raise ResourceFormatError("header size must be 64 bytes")
    if width != WIDTH or height != HEIGHT:
        raise ResourceFormatError("frame dimensions must be 128 x 64")
    if not 1 <= clip_count <= MAX_CLIPS:
        raise ResourceFormatError("clip count must be in the range 1..32")
    if not 1 <= frame_count <= MAX_FRAMES:
        raise ResourceFormatError("frame count must be in the range 1..256")
    if total_size != len(package):
        raise ResourceFormatError("total size does not match the package length")
    if any(header_reserved):
        raise ResourceFormatError("header reserved bytes must be zero")

    clip_table_length = clip_count * CLIP_STRUCT.size
    frame_table_length = frame_count * FRAME_STRUCT.size
    _check_region(clip_table_offset, clip_table_length, total_size, "clip table")
    _check_region(frame_table_offset, frame_table_length, total_size, "frame table")
    if clip_table_offset < header_size:
        raise ResourceFormatError("clip table overlaps the header")
    if frame_table_offset < clip_table_offset + clip_table_length:
        raise ResourceFormatError("frame table overlaps the clip table")
    if data_offset < frame_table_offset + frame_table_length or data_offset > total_size:
        raise ResourceFormatError("frame data overlaps the frame table")
    if _package_crc32(package) != package_crc32:
        raise ResourceFormatError("package CRC32 mismatch")

    clips = []
    expected_first_frame = 0
    for index in range(clip_count):
        offset = clip_table_offset + index * CLIP_STRUCT.size
        (
            expression_id,
            weight,
            frame_interval_ms,
            clip_frame_count,
            reserved16,
            first_frame_index,
            reserved32,
        ) = CLIP_STRUCT.unpack_from(package, offset)
        if reserved16 != 0 or reserved32 != 0:
            raise ResourceFormatError("clip {} reserved fields must be zero".format(index))
        validate_clip_values(expression_id, weight, frame_interval_ms, clip_frame_count)
        if first_frame_index != expected_first_frame:
            raise ResourceFormatError("clip {} frame range is not contiguous".format(index))
        if clip_frame_count > frame_count - first_frame_index:
            raise ResourceFormatError("clip {} frame range is outside the frame table".format(index))
        clips.append(
            ClipEntry(
                expression_id=expression_id,
                weight=weight,
                frame_interval_ms=frame_interval_ms,
                frame_count=clip_frame_count,
                first_frame_index=first_frame_index,
            )
        )
        expected_first_frame += clip_frame_count
    if expected_first_frame != frame_count:
        raise ResourceFormatError("clip ranges do not cover every frame")

    frames = []
    next_data_offset = data_offset
    for index in range(frame_count):
        offset = frame_table_offset + index * FRAME_STRUCT.size
        (
            encoding,
            reserved8,
            reserved16,
            frame_data_offset,
            encoded_length,
            decoded_length,
            frame_crc32,
            reserved32,
        ) = FRAME_STRUCT.unpack_from(package, offset)
        if reserved8 != 0 or reserved16 != 0 or reserved32 != 0:
            raise ResourceFormatError("frame {} reserved fields must be zero".format(index))
        if encoding not in ENCODING_NAMES:
            raise ResourceFormatError("frame {} has an unsupported encoding".format(index))
        if decoded_length != FRAME_SIZE:
            raise ResourceFormatError("frame {} decoded length must be 1024".format(index))
        if encoded_length == 0:
            raise ResourceFormatError("frame {} encoded length cannot be zero".format(index))
        if frame_data_offset != next_data_offset:
            raise ResourceFormatError("frame {} data is not contiguous".format(index))
        _check_region(frame_data_offset, encoded_length, total_size, "frame {} data".format(index))
        encoded = package[frame_data_offset : frame_data_offset + encoded_length]
        try:
            raw = decode_frame(encoding, encoded)
        except ResourceFormatError as error:
            raise ResourceFormatError("frame {}: {}".format(index, error)) from error
        if crc32(raw) != frame_crc32:
            raise ResourceFormatError("frame {} CRC32 mismatch".format(index))
        frames.append(
            FrameEntry(
                encoding=encoding,
                data_offset=frame_data_offset,
                encoded_length=encoded_length,
                decoded_length=decoded_length,
                frame_crc32=frame_crc32,
            )
        )
        next_data_offset = frame_data_offset + encoded_length

    if next_data_offset != total_size:
        raise ResourceFormatError("package contains unreferenced trailing data")

    return ResourcePackage(
        version=version,
        header_size=header_size,
        width=width,
        height=height,
        clip_table_offset=clip_table_offset,
        frame_table_offset=frame_table_offset,
        data_offset=data_offset,
        total_size=total_size,
        package_crc32=package_crc32,
        clips=tuple(clips),
        frames=tuple(frames),
        package_bytes=package,
    )


def verify_package(package):
    """Validate a package and return its parsed metadata."""
    return parse_package(package)
