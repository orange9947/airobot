#ifndef AIROBOT_RESOURCE_FORMAT_H
#define AIROBOT_RESOURCE_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#define RESOURCE_FORMAT_VERSION 1u
#define RESOURCE_PACKAGE_HEADER_SIZE 64u
#define RESOURCE_CLIP_RECORD_SIZE 16u
#define RESOURCE_FRAME_RECORD_SIZE 24u
#define RESOURCE_PACKAGE_CRC_OFFSET 32u
#define RESOURCE_FRAME_WIDTH 128u
#define RESOURCE_FRAME_HEIGHT 64u
#define RESOURCE_DECODED_FRAME_SIZE 1024u
#define RESOURCE_MAX_PACKAGE_SIZE (504u * 1024u)
#define RESOURCE_MAX_CLIPS 32u
#define RESOURCE_MAX_FRAMES 256u
#define RESOURCE_MAX_EXPRESSION_ID 5u
#define RESOURCE_MIN_FRAME_INTERVAL_MS 50u
#define RESOURCE_MAX_FRAME_INTERVAL_MS 2000u

#define RESOURCE_FRAME_ENCODING_RAW1 0u
#define RESOURCE_FRAME_ENCODING_RLE1 1u

typedef enum {
    RESOURCE_FORMAT_OK = 0,
    RESOURCE_FORMAT_INVALID_ARGUMENT,
    RESOURCE_FORMAT_READ_FAILED,
    RESOURCE_FORMAT_BAD_MAGIC,
    RESOURCE_FORMAT_UNSUPPORTED_VERSION,
    RESOURCE_FORMAT_BAD_HEADER,
    RESOURCE_FORMAT_OUT_OF_RANGE,
    RESOURCE_FORMAT_BAD_CRC,
    RESOURCE_FORMAT_BAD_CLIP,
    RESOURCE_FORMAT_BAD_FRAME,
    RESOURCE_FORMAT_BAD_ENCODING,
} resource_format_status_t;

/* Offsets are package-relative. The parser bounds every request before calling. */
typedef bool (*resource_read_fn_t)(void *context, uint32_t offset,
                                   uint8_t *destination, uint32_t length);

typedef struct {
    uint16_t version;
    uint16_t header_size;
    uint16_t width;
    uint16_t height;
    uint16_t clip_count;
    uint16_t frame_count;
    uint32_t clip_table_offset;
    uint32_t frame_table_offset;
    uint32_t data_offset;
    uint32_t total_length;
    uint32_t package_crc32;
} resource_package_header_t;

typedef struct {
    uint8_t expression_id;
    uint8_t weight;
    uint16_t frame_interval_ms;
    uint16_t frame_count;
    uint32_t first_frame_index;
} resource_clip_t;

typedef struct {
    uint8_t encoding;
    uint32_t data_offset;
    uint32_t encoded_length;
    uint32_t decoded_length;
    uint32_t decoded_crc32;
} resource_frame_t;

typedef struct {
    resource_read_fn_t read;
    void *read_context;
    uint32_t source_length;
    resource_package_header_t header;
} resource_package_t;

resource_format_status_t resource_package_open(resource_package_t *package,
                                                resource_read_fn_t read,
                                                void *read_context,
                                                uint32_t source_length);
resource_format_status_t resource_package_calculate_crc32(
    const resource_package_t *package, uint32_t *calculated_crc32);
/* Verifies package CRC plus clip/frame metadata. Frame payload CRC is checked on decode. */
resource_format_status_t resource_package_verify(
    const resource_package_t *package);
resource_format_status_t resource_package_read_clip(
    const resource_package_t *package, uint16_t clip_index,
    resource_clip_t *clip);
resource_format_status_t resource_package_read_frame(
    const resource_package_t *package, uint16_t frame_index,
    resource_frame_t *frame);
resource_format_status_t resource_package_decode_frame(
    const resource_package_t *package, const resource_frame_t *frame,
    uint8_t *output, uint32_t output_capacity);

#endif
