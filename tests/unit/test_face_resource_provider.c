#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "face_resource_provider.h"
#include "resource_crc32.h"
#include "resource_format.h"
#include "storage_service.h"
#include "test_harness.h"

#define TEST_CLIP_COUNT 3u
#define TEST_FRAME_COUNT 4u
#define TEST_CLIP_TABLE_OFFSET RESOURCE_PACKAGE_HEADER_SIZE
#define TEST_FRAME_TABLE_OFFSET \
    (TEST_CLIP_TABLE_OFFSET + TEST_CLIP_COUNT * RESOURCE_CLIP_RECORD_SIZE)
#define TEST_DATA_OFFSET \
    (TEST_FRAME_TABLE_OFFSET + TEST_FRAME_COUNT * RESOURCE_FRAME_RECORD_SIZE)
#define TEST_ENCODED_FRAME_SIZE 16u
#define TEST_PACKAGE_SIZE \
    (TEST_DATA_OFFSET + TEST_FRAME_COUNT * TEST_ENCODED_FRAME_SIZE)

#define TEST_EXPRESSION_NEUTRAL 0u
#define TEST_EXPRESSION_HAPPY 1u
#define TEST_EXPRESSION_SAD 2u

typedef struct {
    uint8_t *data;
    uint32_t length;
    uint32_t reads;
    bool fail_reads;
    bool out_of_bounds_read;
} test_reader_t;

static void write_u16_le(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void write_u32_le(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static bool test_read(void *context, uint32_t offset, uint8_t *destination,
                      uint32_t length) {
    test_reader_t *reader = (test_reader_t *)context;

    ++reader->reads;
    if (offset > reader->length || length > reader->length - offset) {
        reader->out_of_bounds_read = true;
        return false;
    }
    if (reader->fail_reads) {
        return false;
    }
    if (length > 0u) {
        memcpy(destination, &reader->data[offset], length);
    }
    return true;
}

static uint32_t package_crc32(const uint8_t *package, uint32_t length) {
    static const uint8_t zeros[4] = {0u, 0u, 0u, 0u};
    uint32_t state = resource_crc32_init();

    state = resource_crc32_update(state, package, RESOURCE_PACKAGE_CRC_OFFSET);
    state = resource_crc32_update(state, zeros, sizeof(zeros));
    state = resource_crc32_update(
        state, &package[RESOURCE_PACKAGE_CRC_OFFSET + sizeof(zeros)],
        length - RESOURCE_PACKAGE_CRC_OFFSET - (uint32_t)sizeof(zeros));
    return resource_crc32_finalize(state);
}

static void write_clip(uint8_t *package, uint16_t clip_index,
                       uint8_t expression, uint8_t weight,
                       uint16_t interval_ms, uint16_t frame_count,
                       uint32_t first_frame_index) {
    uint32_t offset = TEST_CLIP_TABLE_OFFSET +
                      (uint32_t)clip_index * RESOURCE_CLIP_RECORD_SIZE;

    package[offset] = expression;
    package[offset + 1u] = weight;
    write_u16_le(&package[offset + 2u], interval_ms);
    write_u16_le(&package[offset + 4u], frame_count);
    write_u32_le(&package[offset + 8u], first_frame_index);
}

static void write_frame(uint8_t *package, uint16_t frame_index, uint8_t value) {
    uint8_t decoded[RESOURCE_DECODED_FRAME_SIZE];
    uint32_t record_offset = TEST_FRAME_TABLE_OFFSET +
                             (uint32_t)frame_index * RESOURCE_FRAME_RECORD_SIZE;
    uint32_t data_offset = TEST_DATA_OFFSET +
                           (uint32_t)frame_index * TEST_ENCODED_FRAME_SIZE;
    uint32_t index;

    package[record_offset] = RESOURCE_FRAME_ENCODING_RLE1;
    write_u32_le(&package[record_offset + 4u], data_offset);
    write_u32_le(&package[record_offset + 8u], TEST_ENCODED_FRAME_SIZE);
    write_u32_le(&package[record_offset + 12u], RESOURCE_DECODED_FRAME_SIZE);
    memset(decoded, value, sizeof(decoded));
    write_u32_le(&package[record_offset + 16u],
                 resource_crc32(decoded, sizeof(decoded)));
    for (index = 0u; index < TEST_ENCODED_FRAME_SIZE; index += 2u) {
        package[data_offset + index] = 0xFFu;
        package[data_offset + index + 1u] = value;
    }
}

static void build_package(uint8_t package[TEST_PACKAGE_SIZE]) {
    memset(package, 0, TEST_PACKAGE_SIZE);
    memcpy(package, "ARPK", 4u);
    write_u16_le(&package[4], RESOURCE_FORMAT_VERSION);
    write_u16_le(&package[6], RESOURCE_PACKAGE_HEADER_SIZE);
    write_u16_le(&package[8], RESOURCE_FRAME_WIDTH);
    write_u16_le(&package[10], RESOURCE_FRAME_HEIGHT);
    write_u16_le(&package[12], TEST_CLIP_COUNT);
    write_u16_le(&package[14], TEST_FRAME_COUNT);
    write_u32_le(&package[16], TEST_CLIP_TABLE_OFFSET);
    write_u32_le(&package[20], TEST_FRAME_TABLE_OFFSET);
    write_u32_le(&package[24], TEST_DATA_OFFSET);
    write_u32_le(&package[28], TEST_PACKAGE_SIZE);

    write_clip(package, 0u, TEST_EXPRESSION_HAPPY, 1u, 100u, 1u, 0u);
    write_clip(package, 1u, TEST_EXPRESSION_HAPPY, 3u, 200u, 2u, 1u);
    write_clip(package, 2u, TEST_EXPRESSION_SAD, 2u, 50u, 1u, 3u);
    write_frame(package, 0u, 0x11u);
    write_frame(package, 1u, 0x21u);
    write_frame(package, 2u, 0x22u);
    write_frame(package, 3u, 0x31u);
    write_u32_le(&package[RESOURCE_PACKAGE_CRC_OFFSET],
                 package_crc32(package, TEST_PACKAGE_SIZE));
}

static int open_verified_package(uint8_t package_data[TEST_PACKAGE_SIZE],
                                 test_reader_t *reader,
                                 resource_package_t *package) {
    build_package(package_data);
    *reader = (test_reader_t){package_data, TEST_PACKAGE_SIZE, 0u, false, false};
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(package, test_read, reader,
                                         reader->length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK, resource_package_verify(package));
    return 0;
}

static int assert_frame_value(const uint8_t *frame, uint8_t expected) {
    uint32_t index;

    for (index = 0u; index < RESOURCE_DECODED_FRAME_SIZE; ++index) {
        TEST_ASSERT_EQ(expected, frame[index]);
    }
    return 0;
}

static int load_directory_for_expression(
    face_resource_provider_t *provider, test_reader_t *reader,
    uint8_t expression, uint32_t now_ms,
    face_resource_provider_status_t final_status) {
    uint16_t index;
    uint32_t reads_before = reader->reads;

    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_FALLBACK,
                   face_resource_provider_set_expression(
                       provider, expression, now_ms));
    TEST_ASSERT_EQ(reads_before, reader->reads);
    for (index = 0u; index < TEST_CLIP_COUNT; ++index) {
        bool changed = true;
        face_resource_provider_status_t status;

        reads_before = reader->reads;
        status = face_resource_provider_tick(provider, now_ms, &changed);
        TEST_ASSERT_EQ(reads_before + 1u, reader->reads);
        if (index + 1u < TEST_CLIP_COUNT) {
            TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_FALLBACK, status);
            TEST_ASSERT(!changed);
            TEST_ASSERT(!face_resource_provider_has_frame(provider));
        } else {
            TEST_ASSERT_EQ(final_status, status);
            TEST_ASSERT(changed ==
                        (final_status == FACE_RESOURCE_PROVIDER_OK));
        }
    }
    return 0;
}

static int test_directory_loading_is_incremental_and_cache_is_reused(void) {
    uint8_t package_data[TEST_PACKAGE_SIZE];
    test_reader_t reader;
    resource_package_t package;
    face_resource_provider_t provider;
    uint32_t reads_before;
    uint16_t index;

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    reads_before = reader.reads;
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 1u));
    TEST_ASSERT_EQ(reads_before, reader.reads);
    TEST_ASSERT(!provider.directory_ready);
    TEST_ASSERT_EQ(0u, provider.loaded_clip_count);

    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_FALLBACK,
                   face_resource_provider_set_expression(
                       &provider, TEST_EXPRESSION_HAPPY, 0u));
    TEST_ASSERT_EQ(reads_before, reader.reads);
    for (index = 0u; index < TEST_CLIP_COUNT; ++index) {
        bool changed = true;
        face_resource_provider_status_t status;

        reads_before = reader.reads;
        status = face_resource_provider_tick(&provider, 0u, &changed);
        TEST_ASSERT_EQ(reads_before + 1u, reader.reads);
        TEST_ASSERT_EQ(index + 1u, provider.loaded_clip_count);
        if (index + 1u < TEST_CLIP_COUNT) {
            TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_FALLBACK, status);
            TEST_ASSERT(!changed);
        } else {
            TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK, status);
            TEST_ASSERT(changed);
        }
    }
    TEST_ASSERT(provider.directory_ready);
    TEST_ASSERT(face_resource_provider_has_frame(&provider));

    reads_before = reader.reads;
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_set_expression(
                       &provider, TEST_EXPRESSION_SAD, 10u));
    TEST_ASSERT_EQ(reads_before, reader.reads);
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_set_expression(
                       &provider, TEST_EXPRESSION_HAPPY, 20u));
    TEST_ASSERT_EQ(reads_before, reader.reads);
    {
        bool changed = false;
        TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                       face_resource_provider_tick(&provider, 220u, &changed));
        TEST_ASSERT(changed);
        TEST_ASSERT_EQ(reads_before, reader.reads);
    }
    TEST_ASSERT(sizeof(face_resource_provider_t) + sizeof(storage_service_t) <=
                1536u);
    return 0;
}

static int test_weighted_selection_is_deterministic(void) {
    uint8_t package_data[TEST_PACKAGE_SIZE];
    uint8_t output[RESOURCE_DECODED_FRAME_SIZE];
    test_reader_t reader;
    resource_package_t package;
    face_resource_provider_t provider;

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 4u));
    TEST_ASSERT_EQ(0, load_directory_for_expression(
                          &provider, &reader, TEST_EXPRESSION_HAPPY, 10u,
                          FACE_RESOURCE_PROVIDER_OK));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_decode_current(
                       &provider, output, sizeof(output)));
    TEST_ASSERT_EQ(0, assert_frame_value(output, 0x11u));

    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 1u));
    TEST_ASSERT_EQ(0, load_directory_for_expression(
                          &provider, &reader, TEST_EXPRESSION_HAPPY, 20u,
                          FACE_RESOURCE_PROVIDER_OK));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_decode_current(
                       &provider, output, sizeof(output)));
    TEST_ASSERT_EQ(0, assert_frame_value(output, 0x21u));
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

static int test_tick_advances_frames_and_reselects_at_clip_end(void) {
    uint8_t package_data[TEST_PACKAGE_SIZE];
    uint8_t output[RESOURCE_DECODED_FRAME_SIZE];
    test_reader_t reader;
    resource_package_t package;
    face_resource_provider_t provider;
    bool changed = true;

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 1u));
    TEST_ASSERT_EQ(0, load_directory_for_expression(
                          &provider, &reader, TEST_EXPRESSION_HAPPY, 0u,
                          FACE_RESOURCE_PROVIDER_OK));

    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_tick(&provider, 199u, &changed));
    TEST_ASSERT(!changed);
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_tick(&provider, 200u, &changed));
    TEST_ASSERT(changed);
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_decode_current(
                       &provider, output, sizeof(output)));
    TEST_ASSERT_EQ(0, assert_frame_value(output, 0x22u));

    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_tick(&provider, 399u, &changed));
    TEST_ASSERT(!changed);
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_tick(&provider, 400u, &changed));
    TEST_ASSERT(changed);
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_decode_current(
                       &provider, output, sizeof(output)));
    TEST_ASSERT_EQ(0, assert_frame_value(output, 0x21u));
    return 0;
}

static int test_tick_handles_u32_time_wrap(void) {
    uint8_t package_data[TEST_PACKAGE_SIZE];
    test_reader_t reader;
    resource_package_t package;
    face_resource_provider_t provider;
    bool changed = true;

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 1u));
    TEST_ASSERT_EQ(0, load_directory_for_expression(
                          &provider, &reader, TEST_EXPRESSION_HAPPY,
                          UINT32_MAX - 100u, FACE_RESOURCE_PROVIDER_OK));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_tick(&provider, UINT32_MAX, &changed));
    TEST_ASSERT(!changed);
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_tick(&provider, 99u, &changed));
    TEST_ASSERT(changed);
    return 0;
}

static int test_missing_expression_falls_back_without_harming_package(void) {
    uint8_t package_data[TEST_PACKAGE_SIZE];
    uint8_t output[RESOURCE_DECODED_FRAME_SIZE];
    test_reader_t reader;
    resource_package_t package;
    face_resource_provider_t provider;
    bool changed = true;

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 1u));
    TEST_ASSERT_EQ(0, load_directory_for_expression(
                          &provider, &reader, TEST_EXPRESSION_NEUTRAL, 0u,
                          FACE_RESOURCE_PROVIDER_FALLBACK));
    TEST_ASSERT(face_resource_provider_is_healthy(&provider));
    TEST_ASSERT(!face_resource_provider_has_frame(&provider));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_FALLBACK,
                   face_resource_provider_decode_current(
                       &provider, output, sizeof(output)));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_FALLBACK,
                   face_resource_provider_tick(&provider, 1000u, &changed));
    TEST_ASSERT(!changed);

    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT,
                   face_resource_provider_set_expression(
                       &provider, RESOURCE_MAX_EXPRESSION_ID + 1u, 0u));
    TEST_ASSERT(face_resource_provider_is_healthy(&provider));
    return 0;
}

static int test_decode_and_read_failures_are_explicit_and_disable_package(void) {
    uint8_t package_data[TEST_PACKAGE_SIZE];
    uint8_t output[RESOURCE_DECODED_FRAME_SIZE];
    test_reader_t reader;
    resource_package_t package;
    face_resource_provider_t provider;

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 4u));
    TEST_ASSERT_EQ(0, load_directory_for_expression(
                          &provider, &reader, TEST_EXPRESSION_HAPPY, 0u,
                          FACE_RESOURCE_PROVIDER_OK));
    package_data[TEST_DATA_OFFSET + 1u] ^= 0x01u;
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_DECODE_ERROR,
                   face_resource_provider_decode_current(
                       &provider, output, sizeof(output)));
    TEST_ASSERT(!face_resource_provider_is_healthy(&provider));
    TEST_ASSERT(!face_resource_provider_has_frame(&provider));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_CRC, provider.last_format_status);

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 1u));
    TEST_ASSERT_EQ(0, load_directory_for_expression(
                          &provider, &reader, TEST_EXPRESSION_SAD, 0u,
                          FACE_RESOURCE_PROVIDER_OK));
    reader.fail_reads = true;
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_READ_ERROR,
                   face_resource_provider_decode_current(
                       &provider, output, sizeof(output)));
    TEST_ASSERT(!face_resource_provider_is_healthy(&provider));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_READ_FAILED, provider.last_format_status);
    return 0;
}

static int test_directory_read_failure_is_bounded_and_explicit(void) {
    uint8_t package_data[TEST_PACKAGE_SIZE];
    test_reader_t reader;
    resource_package_t package;
    face_resource_provider_t provider;
    bool changed = true;
    uint32_t reads_before;

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 1u));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_FALLBACK,
                   face_resource_provider_set_expression(
                       &provider, TEST_EXPRESSION_HAPPY, 0u));
    reader.fail_reads = true;
    reads_before = reader.reads;
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_READ_ERROR,
                   face_resource_provider_tick(&provider, 0u, &changed));
    TEST_ASSERT_EQ(reads_before + 1u, reader.reads);
    TEST_ASSERT(!changed);
    TEST_ASSERT(!face_resource_provider_is_healthy(&provider));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_READ_FAILED, provider.last_format_status);
    return 0;
}

static int test_rejects_invalid_arguments_without_writing_output(void) {
    uint8_t package_data[TEST_PACKAGE_SIZE];
    uint8_t guarded[RESOURCE_DECODED_FRAME_SIZE + 2u];
    test_reader_t reader;
    resource_package_t package;
    face_resource_provider_t provider;

    TEST_ASSERT_EQ(0, open_verified_package(package_data, &reader, &package));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT,
                   face_resource_provider_init(NULL, &package, 1u));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT,
                   face_resource_provider_init(&provider, NULL, 1u));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_OK,
                   face_resource_provider_init(&provider, &package, 1u));
    TEST_ASSERT_EQ(0, load_directory_for_expression(
                          &provider, &reader, TEST_EXPRESSION_SAD, 0u,
                          FACE_RESOURCE_PROVIDER_OK));

    memset(guarded, 0x5Au, sizeof(guarded));
    TEST_ASSERT_EQ(FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT,
                   face_resource_provider_decode_current(
                       &provider, &guarded[1],
                       RESOURCE_DECODED_FRAME_SIZE - 1u));
    TEST_ASSERT_EQ(0x5Au, guarded[0]);
    TEST_ASSERT_EQ(0x5Au, guarded[sizeof(guarded) - 1u]);
    TEST_ASSERT(face_resource_provider_is_healthy(&provider));
    return 0;
}

int main(void) {
    TEST_ASSERT_EQ(0,
                   test_directory_loading_is_incremental_and_cache_is_reused());
    TEST_ASSERT_EQ(0, test_weighted_selection_is_deterministic());
    TEST_ASSERT_EQ(0, test_tick_advances_frames_and_reselects_at_clip_end());
    TEST_ASSERT_EQ(0, test_tick_handles_u32_time_wrap());
    TEST_ASSERT_EQ(0, test_missing_expression_falls_back_without_harming_package());
    TEST_ASSERT_EQ(0,
                   test_decode_and_read_failures_are_explicit_and_disable_package());
    TEST_ASSERT_EQ(0, test_directory_read_failure_is_bounded_and_explicit());
    TEST_ASSERT_EQ(0, test_rejects_invalid_arguments_without_writing_output());
    return 0;
}
