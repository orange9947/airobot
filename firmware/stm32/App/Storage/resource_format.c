#include "resource_format.h"

#include <stddef.h>
#include <string.h>

#include "resource_crc32.h"

#define RESOURCE_CRC_FIELD_SIZE 4u
#define RESOURCE_CRC_READ_CHUNK_SIZE 64u
#define RESOURCE_DECODE_READ_CHUNK_SIZE 32u

typedef struct {
    const resource_package_t *package;
    const resource_frame_t *frame;
    uint32_t loaded;
    uint32_t consumed;
    uint8_t buffer[RESOURCE_DECODE_READ_CHUNK_SIZE];
    uint8_t buffer_length;
    uint8_t buffer_index;
} resource_decode_stream_t;

static uint16_t read_u16_le(const uint8_t *source) {
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8u));
}

static uint32_t read_u32_le(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static bool bytes_are_zero(const uint8_t *data, uint32_t length) {
    uint32_t index;

    for (index = 0u; index < length; ++index) {
        if (data[index] != 0u) {
            return false;
        }
    }
    return true;
}

static bool range_fits(uint32_t offset, uint32_t length, uint32_t limit) {
    return offset <= limit && length <= limit - offset;
}

static bool multiply_fits(uint32_t left, uint32_t right, uint32_t *product) {
    if (product == NULL || (left != 0u && right > UINT32_MAX / left)) {
        return false;
    }
    *product = left * right;
    return true;
}

static resource_format_status_t read_package_bytes(
    const resource_package_t *package, uint32_t offset, uint8_t *destination,
    uint32_t length) {
    if (package == NULL || package->read == NULL || destination == NULL) {
        return RESOURCE_FORMAT_INVALID_ARGUMENT;
    }
    if (!range_fits(offset, length, package->header.total_length) ||
        !range_fits(offset, length, package->source_length)) {
        return RESOURCE_FORMAT_OUT_OF_RANGE;
    }
    if (length == 0u) {
        return RESOURCE_FORMAT_OK;
    }
    return package->read(package->read_context, offset, destination, length)
               ? RESOURCE_FORMAT_OK
               : RESOURCE_FORMAT_READ_FAILED;
}

static resource_format_status_t validate_frame(
    const resource_package_t *package, const resource_frame_t *frame) {
    if (package == NULL || frame == NULL) {
        return RESOURCE_FORMAT_INVALID_ARGUMENT;
    }
    if (frame->encoding != RESOURCE_FRAME_ENCODING_RAW1 &&
        frame->encoding != RESOURCE_FRAME_ENCODING_RLE1) {
        return RESOURCE_FORMAT_BAD_FRAME;
    }
    if (frame->decoded_length != RESOURCE_DECODED_FRAME_SIZE) {
        return RESOURCE_FORMAT_BAD_FRAME;
    }
    if (frame->encoding == RESOURCE_FRAME_ENCODING_RAW1) {
        if (frame->encoded_length != RESOURCE_DECODED_FRAME_SIZE) {
            return RESOURCE_FORMAT_BAD_FRAME;
        }
    } else if (frame->encoded_length < 2u) {
        return RESOURCE_FORMAT_BAD_FRAME;
    }
    if (frame->data_offset < package->header.data_offset ||
        !range_fits(frame->data_offset, frame->encoded_length,
                    package->header.total_length)) {
        return RESOURCE_FORMAT_OUT_OF_RANGE;
    }
    return RESOURCE_FORMAT_OK;
}

static resource_format_status_t decode_stream_next(
    resource_decode_stream_t *stream, uint8_t *value) {
    resource_format_status_t status;

    if (stream == NULL || value == NULL ||
        stream->consumed >= stream->frame->encoded_length) {
        return RESOURCE_FORMAT_BAD_ENCODING;
    }
    if (stream->buffer_index >= stream->buffer_length) {
        uint32_t remaining = stream->frame->encoded_length - stream->loaded;
        uint32_t length = remaining > sizeof(stream->buffer)
                              ? sizeof(stream->buffer)
                              : remaining;

        status = read_package_bytes(
            stream->package, stream->frame->data_offset + stream->loaded,
            stream->buffer, length);
        if (status != RESOURCE_FORMAT_OK) {
            return status;
        }
        stream->loaded += length;
        stream->buffer_length = (uint8_t)length;
        stream->buffer_index = 0u;
    }
    *value = stream->buffer[stream->buffer_index++];
    stream->consumed++;
    return RESOURCE_FORMAT_OK;
}

resource_format_status_t resource_package_open(resource_package_t *package,
                                                resource_read_fn_t read,
                                                void *read_context,
                                                uint32_t source_length) {
    uint8_t header[RESOURCE_PACKAGE_HEADER_SIZE];
    uint32_t clip_table_length;
    uint32_t frame_table_length;
    uint32_t clip_table_end;
    uint32_t frame_table_end;

    if (package == NULL || read == NULL) {
        return RESOURCE_FORMAT_INVALID_ARGUMENT;
    }
    memset(package, 0, sizeof(*package));
    if (source_length < RESOURCE_PACKAGE_HEADER_SIZE) {
        return RESOURCE_FORMAT_OUT_OF_RANGE;
    }
    if (!read(read_context, 0u, header, sizeof(header))) {
        return RESOURCE_FORMAT_READ_FAILED;
    }
    if (memcmp(header, "ARPK", 4u) != 0) {
        return RESOURCE_FORMAT_BAD_MAGIC;
    }

    package->header.version = read_u16_le(&header[4]);
    package->header.header_size = read_u16_le(&header[6]);
    package->header.width = read_u16_le(&header[8]);
    package->header.height = read_u16_le(&header[10]);
    package->header.clip_count = read_u16_le(&header[12]);
    package->header.frame_count = read_u16_le(&header[14]);
    package->header.clip_table_offset = read_u32_le(&header[16]);
    package->header.frame_table_offset = read_u32_le(&header[20]);
    package->header.data_offset = read_u32_le(&header[24]);
    package->header.total_length = read_u32_le(&header[28]);
    package->header.package_crc32 = read_u32_le(&header[32]);

    if (package->header.version != RESOURCE_FORMAT_VERSION) {
        return RESOURCE_FORMAT_UNSUPPORTED_VERSION;
    }
    if (package->header.header_size != RESOURCE_PACKAGE_HEADER_SIZE ||
        package->header.width != RESOURCE_FRAME_WIDTH ||
        package->header.height != RESOURCE_FRAME_HEIGHT ||
        package->header.clip_count == 0u ||
        package->header.clip_count > RESOURCE_MAX_CLIPS ||
        package->header.frame_count == 0u ||
        package->header.frame_count > RESOURCE_MAX_FRAMES ||
        !bytes_are_zero(&header[36], 28u)) {
        return RESOURCE_FORMAT_BAD_HEADER;
    }
    if (package->header.total_length > RESOURCE_MAX_PACKAGE_SIZE ||
        package->header.total_length > source_length ||
        package->header.total_length < RESOURCE_PACKAGE_HEADER_SIZE) {
        return RESOURCE_FORMAT_OUT_OF_RANGE;
    }
    if (!multiply_fits(package->header.clip_count, RESOURCE_CLIP_RECORD_SIZE,
                       &clip_table_length) ||
        !multiply_fits(package->header.frame_count, RESOURCE_FRAME_RECORD_SIZE,
                       &frame_table_length) ||
        !range_fits(package->header.clip_table_offset, clip_table_length,
                    package->header.total_length) ||
        !range_fits(package->header.frame_table_offset, frame_table_length,
                    package->header.total_length)) {
        return RESOURCE_FORMAT_OUT_OF_RANGE;
    }
    clip_table_end = package->header.clip_table_offset + clip_table_length;
    frame_table_end = package->header.frame_table_offset + frame_table_length;
    if (package->header.clip_table_offset != RESOURCE_PACKAGE_HEADER_SIZE ||
        package->header.frame_table_offset != clip_table_end ||
        package->header.data_offset != frame_table_end ||
        package->header.data_offset > package->header.total_length) {
        return RESOURCE_FORMAT_OUT_OF_RANGE;
    }

    package->read = read;
    package->read_context = read_context;
    package->source_length = source_length;
    return RESOURCE_FORMAT_OK;
}

resource_format_status_t resource_package_calculate_crc32(
    const resource_package_t *package, uint32_t *calculated_crc32) {
    uint8_t buffer[RESOURCE_CRC_READ_CHUNK_SIZE];
    uint32_t state;
    uint32_t offset = 0u;

    if (package == NULL || package->read == NULL || calculated_crc32 == NULL) {
        return RESOURCE_FORMAT_INVALID_ARGUMENT;
    }
    state = resource_crc32_init();
    while (offset < package->header.total_length) {
        uint32_t chunk_length = package->header.total_length - offset;
        uint32_t index;
        resource_format_status_t status;

        if (chunk_length > sizeof(buffer)) {
            chunk_length = sizeof(buffer);
        }
        status = read_package_bytes(package, offset, buffer, chunk_length);
        if (status != RESOURCE_FORMAT_OK) {
            return status;
        }
        for (index = 0u; index < chunk_length; ++index) {
            uint32_t package_offset = offset + index;
            if (package_offset >= RESOURCE_PACKAGE_CRC_OFFSET &&
                package_offset <
                    RESOURCE_PACKAGE_CRC_OFFSET + RESOURCE_CRC_FIELD_SIZE) {
                buffer[index] = 0u;
            }
        }
        state = resource_crc32_update(state, buffer, chunk_length);
        offset += chunk_length;
    }
    *calculated_crc32 = resource_crc32_finalize(state);
    return RESOURCE_FORMAT_OK;
}

resource_format_status_t resource_package_read_clip(
    const resource_package_t *package, uint16_t clip_index,
    resource_clip_t *clip) {
    uint8_t record[RESOURCE_CLIP_RECORD_SIZE];
    uint32_t offset;
    resource_format_status_t status;

    if (package == NULL || clip == NULL) {
        return RESOURCE_FORMAT_INVALID_ARGUMENT;
    }
    if (clip_index >= package->header.clip_count) {
        return RESOURCE_FORMAT_OUT_OF_RANGE;
    }
    offset = package->header.clip_table_offset +
             (uint32_t)clip_index * RESOURCE_CLIP_RECORD_SIZE;
    status = read_package_bytes(package, offset, record, sizeof(record));
    if (status != RESOURCE_FORMAT_OK) {
        return status;
    }
    clip->expression_id = record[0];
    clip->weight = record[1];
    clip->frame_interval_ms = read_u16_le(&record[2]);
    clip->frame_count = read_u16_le(&record[4]);
    clip->first_frame_index = read_u32_le(&record[8]);

    if (clip->expression_id > RESOURCE_MAX_EXPRESSION_ID || clip->weight == 0u ||
        clip->frame_interval_ms < RESOURCE_MIN_FRAME_INTERVAL_MS ||
        clip->frame_interval_ms > RESOURCE_MAX_FRAME_INTERVAL_MS ||
        clip->frame_count == 0u || !bytes_are_zero(&record[6], 2u) ||
        !bytes_are_zero(&record[12], 4u) ||
        clip->first_frame_index > package->header.frame_count ||
        clip->frame_count >
            (uint32_t)package->header.frame_count - clip->first_frame_index) {
        return RESOURCE_FORMAT_BAD_CLIP;
    }
    return RESOURCE_FORMAT_OK;
}

resource_format_status_t resource_package_read_frame(
    const resource_package_t *package, uint16_t frame_index,
    resource_frame_t *frame) {
    uint8_t record[RESOURCE_FRAME_RECORD_SIZE];
    uint32_t offset;
    resource_format_status_t status;

    if (package == NULL || frame == NULL) {
        return RESOURCE_FORMAT_INVALID_ARGUMENT;
    }
    if (frame_index >= package->header.frame_count) {
        return RESOURCE_FORMAT_OUT_OF_RANGE;
    }
    offset = package->header.frame_table_offset +
             (uint32_t)frame_index * RESOURCE_FRAME_RECORD_SIZE;
    status = read_package_bytes(package, offset, record, sizeof(record));
    if (status != RESOURCE_FORMAT_OK) {
        return status;
    }
    frame->encoding = record[0];
    frame->data_offset = read_u32_le(&record[4]);
    frame->encoded_length = read_u32_le(&record[8]);
    frame->decoded_length = read_u32_le(&record[12]);
    frame->decoded_crc32 = read_u32_le(&record[16]);

    if (!bytes_are_zero(&record[1], 3u) || !bytes_are_zero(&record[20], 4u)) {
        return RESOURCE_FORMAT_BAD_FRAME;
    }
    return validate_frame(package, frame);
}

resource_format_status_t resource_package_verify(
    const resource_package_t *package) {
    uint32_t expected_first_frame = 0u;
    uint32_t next_data_offset;
    uint16_t index;
    uint32_t calculated_crc32;
    resource_format_status_t status;

    if (package == NULL || package->read == NULL) {
        return RESOURCE_FORMAT_INVALID_ARGUMENT;
    }
    for (index = 0u; index < package->header.clip_count; ++index) {
        resource_clip_t clip;

        status = resource_package_read_clip(package, index, &clip);
        if (status != RESOURCE_FORMAT_OK) {
            return status;
        }
        if (clip.first_frame_index != expected_first_frame) {
            return RESOURCE_FORMAT_BAD_CLIP;
        }
        expected_first_frame += clip.frame_count;
    }
    if (expected_first_frame != package->header.frame_count) {
        return RESOURCE_FORMAT_BAD_CLIP;
    }
    next_data_offset = package->header.data_offset;
    for (index = 0u; index < package->header.frame_count; ++index) {
        resource_frame_t frame;

        status = resource_package_read_frame(package, index, &frame);
        if (status != RESOURCE_FORMAT_OK) {
            return status;
        }
        if (frame.data_offset != next_data_offset) {
            return RESOURCE_FORMAT_BAD_FRAME;
        }
        next_data_offset = frame.data_offset + frame.encoded_length;
    }
    if (next_data_offset != package->header.total_length) {
        return RESOURCE_FORMAT_BAD_FRAME;
    }
    status = resource_package_calculate_crc32(package, &calculated_crc32);
    if (status != RESOURCE_FORMAT_OK) {
        return status;
    }
    return calculated_crc32 == package->header.package_crc32
               ? RESOURCE_FORMAT_OK
               : RESOURCE_FORMAT_BAD_CRC;
}

resource_format_status_t resource_package_decode_frame(
    const resource_package_t *package, const resource_frame_t *frame,
    uint8_t *output, uint32_t output_capacity) {
    uint32_t output_offset = 0u;
    resource_format_status_t status;

    if (package == NULL || frame == NULL || output == NULL ||
        output_capacity < RESOURCE_DECODED_FRAME_SIZE) {
        return RESOURCE_FORMAT_INVALID_ARGUMENT;
    }
    status = validate_frame(package, frame);
    if (status != RESOURCE_FORMAT_OK) {
        return status;
    }
    if (frame->encoding == RESOURCE_FRAME_ENCODING_RAW1) {
        status = read_package_bytes(package, frame->data_offset, output,
                                    RESOURCE_DECODED_FRAME_SIZE);
        if (status != RESOURCE_FORMAT_OK) {
            return status;
        }
    } else {
        resource_decode_stream_t stream = {
            .package = package,
            .frame = frame,
        };

        while (stream.consumed < frame->encoded_length) {
            uint8_t control;
            uint32_t run_length;

            if (output_offset == RESOURCE_DECODED_FRAME_SIZE) {
                return RESOURCE_FORMAT_BAD_ENCODING;
            }
            status = decode_stream_next(&stream, &control);
            if (status != RESOURCE_FORMAT_OK) {
                return status;
            }
            run_length = (uint32_t)(control & 0x7Fu) + 1u;
            if (run_length > RESOURCE_DECODED_FRAME_SIZE - output_offset) {
                return RESOURCE_FORMAT_BAD_ENCODING;
            }
            if ((control & 0x80u) == 0u) {
                uint32_t index;

                if (run_length > frame->encoded_length - stream.consumed) {
                    return RESOURCE_FORMAT_BAD_ENCODING;
                }
                for (index = 0u; index < run_length; ++index) {
                    status = decode_stream_next(
                        &stream, &output[output_offset + index]);
                    if (status != RESOURCE_FORMAT_OK) {
                        return status;
                    }
                }
            } else {
                uint8_t value;

                if (stream.consumed == frame->encoded_length) {
                    return RESOURCE_FORMAT_BAD_ENCODING;
                }
                status = decode_stream_next(&stream, &value);
                if (status != RESOURCE_FORMAT_OK) {
                    return status;
                }
                memset(&output[output_offset], value, run_length);
            }
            output_offset += run_length;
        }
        if (output_offset != RESOURCE_DECODED_FRAME_SIZE) {
            return RESOURCE_FORMAT_BAD_ENCODING;
        }
    }
    return resource_crc32(output, RESOURCE_DECODED_FRAME_SIZE) ==
                   frame->decoded_crc32
               ? RESOURCE_FORMAT_OK
               : RESOURCE_FORMAT_BAD_CRC;
}
