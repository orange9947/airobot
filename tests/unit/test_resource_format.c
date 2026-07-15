#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "resource_crc32.h"
#include "resource_format.h"
#include "test_harness.h"

#define FIXTURE_CLIP_COUNT 1u
#define FIXTURE_FRAME_COUNT 2u
#define FIXTURE_CLIP_TABLE_OFFSET RESOURCE_PACKAGE_HEADER_SIZE
#define FIXTURE_FRAME_TABLE_OFFSET \
    (FIXTURE_CLIP_TABLE_OFFSET + RESOURCE_CLIP_RECORD_SIZE)
#define FIXTURE_DATA_OFFSET \
    (FIXTURE_FRAME_TABLE_OFFSET + FIXTURE_FRAME_COUNT * RESOURCE_FRAME_RECORD_SIZE)
#define FIXTURE_RAW_OFFSET FIXTURE_DATA_OFFSET
#define FIXTURE_RLE_OFFSET (FIXTURE_RAW_OFFSET + RESOURCE_DECODED_FRAME_SIZE)
#define FIXTURE_RLE_SIZE 16u
#define FIXTURE_SIZE (FIXTURE_RLE_OFFSET + FIXTURE_RLE_SIZE)
#define BLANK_GOLDEN_SIZE 120u
#define BLANK_GOLDEN_FRAME_CRC32 0xEFB5AF2Eu
#define BLANK_GOLDEN_PACKAGE_CRC32 0x7B6CDB84u

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t reads;
    bool out_of_bounds_read;
    bool fail_reads;
} memory_reader_t;

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

static bool memory_read(void *context, uint32_t offset, uint8_t *destination,
                        uint32_t length) {
    memory_reader_t *reader = (memory_reader_t *)context;

    reader->reads++;
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

static uint32_t package_crc(const uint8_t *package, uint32_t length) {
    uint32_t state = resource_crc32_init();
    static const uint8_t zeros[4] = {0u, 0u, 0u, 0u};

    state = resource_crc32_update(state, package, RESOURCE_PACKAGE_CRC_OFFSET);
    state = resource_crc32_update(state, zeros, sizeof(zeros));
    state = resource_crc32_update(
        state, &package[RESOURCE_PACKAGE_CRC_OFFSET + sizeof(zeros)],
        length - RESOURCE_PACKAGE_CRC_OFFSET - (uint32_t)sizeof(zeros));
    return resource_crc32_finalize(state);
}

static void build_fixture(uint8_t package[FIXTURE_SIZE]) {
    uint32_t index;
    uint32_t crc;

    memset(package, 0, FIXTURE_SIZE);
    memcpy(&package[0], "ARPK", 4u);
    write_u16_le(&package[4], RESOURCE_FORMAT_VERSION);
    write_u16_le(&package[6], RESOURCE_PACKAGE_HEADER_SIZE);
    write_u16_le(&package[8], RESOURCE_FRAME_WIDTH);
    write_u16_le(&package[10], RESOURCE_FRAME_HEIGHT);
    write_u16_le(&package[12], FIXTURE_CLIP_COUNT);
    write_u16_le(&package[14], FIXTURE_FRAME_COUNT);
    write_u32_le(&package[16], FIXTURE_CLIP_TABLE_OFFSET);
    write_u32_le(&package[20], FIXTURE_FRAME_TABLE_OFFSET);
    write_u32_le(&package[24], FIXTURE_DATA_OFFSET);
    write_u32_le(&package[28], FIXTURE_SIZE);

    package[FIXTURE_CLIP_TABLE_OFFSET] = 1u;
    package[FIXTURE_CLIP_TABLE_OFFSET + 1u] = 7u;
    write_u16_le(&package[FIXTURE_CLIP_TABLE_OFFSET + 2u], 120u);
    write_u16_le(&package[FIXTURE_CLIP_TABLE_OFFSET + 4u], FIXTURE_FRAME_COUNT);
    write_u32_le(&package[FIXTURE_CLIP_TABLE_OFFSET + 8u], 0u);

    package[FIXTURE_FRAME_TABLE_OFFSET] = RESOURCE_FRAME_ENCODING_RAW1;
    write_u32_le(&package[FIXTURE_FRAME_TABLE_OFFSET + 4u], FIXTURE_RAW_OFFSET);
    write_u32_le(&package[FIXTURE_FRAME_TABLE_OFFSET + 8u],
                 RESOURCE_DECODED_FRAME_SIZE);
    write_u32_le(&package[FIXTURE_FRAME_TABLE_OFFSET + 12u],
                 RESOURCE_DECODED_FRAME_SIZE);
    for (index = 0u; index < RESOURCE_DECODED_FRAME_SIZE; ++index) {
        package[FIXTURE_RAW_OFFSET + index] = (uint8_t)index;
    }
    crc = resource_crc32(&package[FIXTURE_RAW_OFFSET],
                         RESOURCE_DECODED_FRAME_SIZE);
    write_u32_le(&package[FIXTURE_FRAME_TABLE_OFFSET + 16u], crc);

    package[FIXTURE_FRAME_TABLE_OFFSET + RESOURCE_FRAME_RECORD_SIZE] =
        RESOURCE_FRAME_ENCODING_RLE1;
    write_u32_le(&package[FIXTURE_FRAME_TABLE_OFFSET + RESOURCE_FRAME_RECORD_SIZE + 4u],
                 FIXTURE_RLE_OFFSET);
    write_u32_le(&package[FIXTURE_FRAME_TABLE_OFFSET + RESOURCE_FRAME_RECORD_SIZE + 8u],
                 FIXTURE_RLE_SIZE);
    write_u32_le(&package[FIXTURE_FRAME_TABLE_OFFSET + RESOURCE_FRAME_RECORD_SIZE + 12u],
                 RESOURCE_DECODED_FRAME_SIZE);
    for (index = 0u; index < FIXTURE_RLE_SIZE; index += 2u) {
        package[FIXTURE_RLE_OFFSET + index] = 0xFFu;
        package[FIXTURE_RLE_OFFSET + index + 1u] = 0xA5u;
    }
    {
        uint8_t decoded[RESOURCE_DECODED_FRAME_SIZE];
        memset(decoded, 0xA5, sizeof(decoded));
        crc = resource_crc32(decoded, sizeof(decoded));
    }
    write_u32_le(&package[FIXTURE_FRAME_TABLE_OFFSET + RESOURCE_FRAME_RECORD_SIZE + 16u],
                 crc);

    write_u32_le(&package[RESOURCE_PACKAGE_CRC_OFFSET], package_crc(package, FIXTURE_SIZE));
}

static void build_blank_python_golden(uint8_t package[BLANK_GOLDEN_SIZE]) {
    uint32_t index;

    memset(package, 0, BLANK_GOLDEN_SIZE);
    memcpy(&package[0], "ARPK", 4u);
    write_u16_le(&package[4], RESOURCE_FORMAT_VERSION);
    write_u16_le(&package[6], RESOURCE_PACKAGE_HEADER_SIZE);
    write_u16_le(&package[8], RESOURCE_FRAME_WIDTH);
    write_u16_le(&package[10], RESOURCE_FRAME_HEIGHT);
    write_u16_le(&package[12], 1u);
    write_u16_le(&package[14], 1u);
    write_u32_le(&package[16], 64u);
    write_u32_le(&package[20], 80u);
    write_u32_le(&package[24], 104u);
    write_u32_le(&package[28], BLANK_GOLDEN_SIZE);
    write_u32_le(&package[32], BLANK_GOLDEN_PACKAGE_CRC32);

    package[64] = 0u;
    package[65] = 1u;
    write_u16_le(&package[66], 100u);
    write_u16_le(&package[68], 1u);
    write_u32_le(&package[72], 0u);

    package[80] = RESOURCE_FRAME_ENCODING_RLE1;
    write_u32_le(&package[84], 104u);
    write_u32_le(&package[88], 16u);
    write_u32_le(&package[92], RESOURCE_DECODED_FRAME_SIZE);
    write_u32_le(&package[96], BLANK_GOLDEN_FRAME_CRC32);
    for (index = 104u; index < BLANK_GOLDEN_SIZE; index += 2u) {
        package[index] = 0xFFu;
        package[index + 1u] = 0u;
    }
}

static resource_format_status_t open_fixture(
    uint8_t package_data[FIXTURE_SIZE], memory_reader_t *reader,
    resource_package_t *package) {
    build_fixture(package_data);
    reader->data = package_data;
    reader->length = FIXTURE_SIZE;
    reader->reads = 0u;
    reader->out_of_bounds_read = false;
    reader->fail_reads = false;
    return resource_package_open(package, memory_read, reader, reader->length);
}

static int test_crc32_standard_vector_and_incremental(void) {
    static const uint8_t input[] = "123456789";
    uint8_t blank_frame[RESOURCE_DECODED_FRAME_SIZE] = {0u};
    uint32_t state;

    TEST_ASSERT_EQ(0xCBF43926u, resource_crc32(input, sizeof(input) - 1u));
    TEST_ASSERT_EQ(BLANK_GOLDEN_FRAME_CRC32,
                   resource_crc32(blank_frame, sizeof(blank_frame)));
    TEST_ASSERT_EQ(0u, resource_crc32(NULL, 0u));

    state = resource_crc32_init();
    state = resource_crc32_update(state, input, 3u);
    state = resource_crc32_update(state, &input[3], 2u);
    state = resource_crc32_update(state, &input[5], 4u);
    TEST_ASSERT_EQ(0xCBF43926u, resource_crc32_finalize(state));
    return 0;
}

static int test_matches_python_blank_package_golden(void) {
    uint8_t package_data[BLANK_GOLDEN_SIZE];
    uint8_t decoded[RESOURCE_DECODED_FRAME_SIZE];
    memory_reader_t reader;
    resource_package_t package;
    resource_frame_t frame;
    uint32_t calculated_crc = 0u;
    uint32_t index;

    build_blank_python_golden(package_data);
    reader = (memory_reader_t){package_data, sizeof(package_data), 0u, false, false};
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK, resource_package_verify(&package));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_calculate_crc32(&package, &calculated_crc));
    TEST_ASSERT_EQ(BLANK_GOLDEN_PACKAGE_CRC32, calculated_crc);
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 0u, &frame));
    TEST_ASSERT_EQ(BLANK_GOLDEN_FRAME_CRC32, frame.decoded_crc32);
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_decode_frame(&package, &frame, decoded,
                                                 sizeof(decoded)));
    for (index = 0u; index < sizeof(decoded); ++index) {
        TEST_ASSERT_EQ(0u, decoded[index]);
    }
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

static int test_opens_and_verifies_golden_fixture(void) {
    uint8_t package_data[FIXTURE_SIZE];
    memory_reader_t reader;
    resource_package_t package;
    resource_clip_t clip;
    resource_frame_t frame;
    uint32_t calculated_crc = 0u;

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   open_fixture(package_data, &reader, &package));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_VERSION, package.header.version);
    TEST_ASSERT_EQ(RESOURCE_FRAME_WIDTH, package.header.width);
    TEST_ASSERT_EQ(RESOURCE_FRAME_HEIGHT, package.header.height);
    TEST_ASSERT_EQ(FIXTURE_CLIP_COUNT, package.header.clip_count);
    TEST_ASSERT_EQ(FIXTURE_FRAME_COUNT, package.header.frame_count);
    TEST_ASSERT_EQ(FIXTURE_SIZE, package.header.total_length);
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_calculate_crc32(&package, &calculated_crc));
    TEST_ASSERT_EQ(package.header.package_crc32, calculated_crc);
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK, resource_package_verify(&package));

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_clip(&package, 0u, &clip));
    TEST_ASSERT_EQ(1u, clip.expression_id);
    TEST_ASSERT_EQ(7u, clip.weight);
    TEST_ASSERT_EQ(120u, clip.frame_interval_ms);
    TEST_ASSERT_EQ(2u, clip.frame_count);
    TEST_ASSERT_EQ(0u, clip.first_frame_index);

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 1u, &frame));
    TEST_ASSERT_EQ(RESOURCE_FRAME_ENCODING_RLE1, frame.encoding);
    TEST_ASSERT_EQ(FIXTURE_RLE_OFFSET, frame.data_offset);
    TEST_ASSERT_EQ(FIXTURE_RLE_SIZE, frame.encoded_length);
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

static int test_decodes_raw_and_rle_frames(void) {
    uint8_t package_data[FIXTURE_SIZE];
    uint8_t output[RESOURCE_DECODED_FRAME_SIZE];
    memory_reader_t reader;
    resource_package_t package;
    resource_frame_t frame;
    uint32_t index;

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   open_fixture(package_data, &reader, &package));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 0u, &frame));
    memset(output, 0xCC, sizeof(output));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_decode_frame(&package, &frame, output,
                                                 sizeof(output)));
    for (index = 0u; index < sizeof(output); ++index) {
        TEST_ASSERT_EQ((uint8_t)index, output[index]);
    }

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 1u, &frame));
    reader.reads = 0u;
    memset(output, 0xCC, sizeof(output));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_decode_frame(&package, &frame, output,
                                                 sizeof(output)));
    TEST_ASSERT_EQ(1u, reader.reads);
    for (index = 0u; index < sizeof(output); ++index) {
        TEST_ASSERT_EQ(0xA5u, output[index]);
    }
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

static int test_rejects_header_ranges_before_dereferencing(void) {
    uint8_t package_data[FIXTURE_SIZE];
    memory_reader_t reader;
    resource_package_t package;
    static const uint32_t bad_offsets[] = {
        0u, 63u, UINT32_MAX - 7u, UINT32_MAX,
    };
    uint32_t index;

    build_fixture(package_data);
    reader.data = package_data;
    reader.length = FIXTURE_SIZE;
    reader.fail_reads = false;
    for (index = 0u; index < sizeof(bad_offsets) / sizeof(bad_offsets[0]); ++index) {
        reader.reads = 0u;
        reader.out_of_bounds_read = false;
        write_u32_le(&package_data[16], bad_offsets[index]);
        TEST_ASSERT(resource_package_open(&package, memory_read, &reader,
                                          reader.length) != RESOURCE_FORMAT_OK);
        TEST_ASSERT(!reader.out_of_bounds_read);
        TEST_ASSERT_EQ(1u, reader.reads);
    }

    build_fixture(package_data);
    write_u16_le(&package_data[12], UINT16_MAX);
    reader.reads = 0u;
    reader.out_of_bounds_read = false;
    TEST_ASSERT(resource_package_open(&package, memory_read, &reader, reader.length) !=
                RESOURCE_FORMAT_OK);
    TEST_ASSERT(!reader.out_of_bounds_read);
    TEST_ASSERT_EQ(1u, reader.reads);

    reader.length = RESOURCE_PACKAGE_HEADER_SIZE - 1u;
    reader.reads = 0u;
    reader.out_of_bounds_read = false;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OUT_OF_RANGE,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(0u, reader.reads);
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

static int test_rejects_directory_gaps(void) {
    uint8_t package_data[FIXTURE_SIZE];
    memory_reader_t reader;
    resource_package_t package;

    build_fixture(package_data);
    reader = (memory_reader_t){package_data, FIXTURE_SIZE, 0u, false, false};
    write_u32_le(&package_data[16], FIXTURE_CLIP_TABLE_OFFSET + 1u);
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OUT_OF_RANGE,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));

    build_fixture(package_data);
    write_u32_le(&package_data[20], FIXTURE_FRAME_TABLE_OFFSET + 1u);
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OUT_OF_RANGE,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));

    build_fixture(package_data);
    write_u32_le(&package_data[24], FIXTURE_DATA_OFFSET + 1u);
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OUT_OF_RANGE,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

static int test_rejects_invalid_reserved_and_directory_entries(void) {
    uint8_t package_data[FIXTURE_SIZE];
    memory_reader_t reader;
    resource_package_t package;

    build_fixture(package_data);
    package_data[36] = 1u;
    reader = (memory_reader_t){package_data, FIXTURE_SIZE, 0u, false, false};
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_HEADER,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   open_fixture(package_data, &reader, &package));
    package_data[FIXTURE_CLIP_TABLE_OFFSET + 6u] = 1u;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_CLIP, resource_package_verify(&package));

    build_fixture(package_data);
    write_u32_le(&package_data[FIXTURE_CLIP_TABLE_OFFSET + 8u], 1u);
    write_u32_le(&package_data[RESOURCE_PACKAGE_CRC_OFFSET],
                 package_crc(package_data, FIXTURE_SIZE));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_CLIP, resource_package_verify(&package));

    build_fixture(package_data);
    package_data[FIXTURE_FRAME_TABLE_OFFSET + 1u] = 1u;
    write_u32_le(&package_data[RESOURCE_PACKAGE_CRC_OFFSET],
                 package_crc(package_data, FIXTURE_SIZE));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_FRAME, resource_package_verify(&package));

    build_fixture(package_data);
    write_u32_le(&package_data[FIXTURE_FRAME_TABLE_OFFSET +
                               RESOURCE_FRAME_RECORD_SIZE + 4u],
                 FIXTURE_RAW_OFFSET + 1u);
    write_u32_le(&package_data[RESOURCE_PACKAGE_CRC_OFFSET],
                 package_crc(package_data, FIXTURE_SIZE));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_FRAME, resource_package_verify(&package));

    build_fixture(package_data);
    write_u32_le(&package_data[FIXTURE_FRAME_TABLE_OFFSET + 4u],
                 FIXTURE_RAW_OFFSET + 1u);
    write_u32_le(&package_data[RESOURCE_PACKAGE_CRC_OFFSET],
                 package_crc(package_data, FIXTURE_SIZE));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_FRAME, resource_package_verify(&package));

    build_fixture(package_data);
    write_u32_le(&package_data[FIXTURE_FRAME_TABLE_OFFSET +
                               RESOURCE_FRAME_RECORD_SIZE + 8u],
                 FIXTURE_RLE_SIZE - 2u);
    write_u32_le(&package_data[RESOURCE_PACKAGE_CRC_OFFSET],
                 package_crc(package_data, FIXTURE_SIZE));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_FRAME, resource_package_verify(&package));
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

static int test_rejects_corrupt_package_and_frame_crc(void) {
    uint8_t package_data[FIXTURE_SIZE];
    uint8_t output[RESOURCE_DECODED_FRAME_SIZE];
    memory_reader_t reader;
    resource_package_t package;
    resource_frame_t frame;

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   open_fixture(package_data, &reader, &package));
    package_data[FIXTURE_RAW_OFFSET + 10u] ^= 0x01u;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_CRC, resource_package_verify(&package));

    build_fixture(package_data);
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 0u, &frame));
    frame.decoded_crc32 ^= 0x01u;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_CRC,
                   resource_package_decode_frame(&package, &frame, output,
                                                 sizeof(output)));
    return 0;
}

static int test_rle_requires_exact_input_and_output(void) {
    uint8_t package_data[FIXTURE_SIZE + 2u];
    uint8_t guarded[RESOURCE_DECODED_FRAME_SIZE + 2u];
    memory_reader_t reader;
    resource_package_t package;
    resource_frame_t frame;

    build_fixture(package_data);
    reader = (memory_reader_t){package_data, FIXTURE_SIZE + 2u, 0u, false, false};
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 1u, &frame));

    memset(guarded, 0x3Cu, sizeof(guarded));
    frame.encoded_length = FIXTURE_RLE_SIZE - 2u;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_ENCODING,
                   resource_package_decode_frame(&package, &frame, &guarded[1],
                                                 RESOURCE_DECODED_FRAME_SIZE));
    TEST_ASSERT_EQ(0x3Cu, guarded[0]);
    TEST_ASSERT_EQ(0x3Cu, guarded[sizeof(guarded) - 1u]);

    frame.encoded_length = FIXTURE_RLE_SIZE + 2u;
    package.header.total_length = FIXTURE_SIZE + 2u;
    package_data[FIXTURE_RLE_OFFSET + FIXTURE_RLE_SIZE] = 0x80u;
    package_data[FIXTURE_RLE_OFFSET + FIXTURE_RLE_SIZE + 1u] = 0x00u;
    memset(guarded, 0x3Cu, sizeof(guarded));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_ENCODING,
                   resource_package_decode_frame(&package, &frame, &guarded[1],
                                                 RESOURCE_DECODED_FRAME_SIZE));
    TEST_ASSERT_EQ(0x3Cu, guarded[0]);
    TEST_ASSERT_EQ(0x3Cu, guarded[sizeof(guarded) - 1u]);

    frame.encoded_length = 2u;
    package_data[FIXTURE_RLE_OFFSET] = 0x7Fu;
    package_data[FIXTURE_RLE_OFFSET + 1u] = 0xA5u;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_ENCODING,
                   resource_package_decode_frame(&package, &frame, &guarded[1],
                                                 RESOURCE_DECODED_FRAME_SIZE));
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

static int test_rejects_invalid_frame_ranges_without_bad_reads(void) {
    uint8_t package_data[FIXTURE_SIZE];
    uint8_t output[RESOURCE_DECODED_FRAME_SIZE];
    memory_reader_t reader;
    resource_package_t package;
    resource_frame_t frame;
    uint32_t reads_before_decode;

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   open_fixture(package_data, &reader, &package));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 1u, &frame));
    frame.data_offset = UINT32_MAX - 3u;
    frame.encoded_length = 16u;
    reads_before_decode = reader.reads;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OUT_OF_RANGE,
                   resource_package_decode_frame(&package, &frame, output,
                                                 sizeof(output)));
    TEST_ASSERT_EQ(reads_before_decode, reader.reads);
    TEST_ASSERT(!reader.out_of_bounds_read);

    TEST_ASSERT_EQ(RESOURCE_FORMAT_INVALID_ARGUMENT,
                   resource_package_decode_frame(
                       &package, &frame, output, RESOURCE_DECODED_FRAME_SIZE - 1u));

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 0u, &frame));
    frame.encoding = 2u;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_FRAME,
                   resource_package_decode_frame(&package, &frame, output,
                                                 sizeof(output)));
    frame.encoding = RESOURCE_FRAME_ENCODING_RAW1;
    frame.encoded_length = RESOURCE_DECODED_FRAME_SIZE - 1u;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_BAD_FRAME,
                   resource_package_decode_frame(&package, &frame, output,
                                                 sizeof(output)));
    return 0;
}

static int test_propagates_read_failures(void) {
    uint8_t package_data[FIXTURE_SIZE];
    uint8_t output[RESOURCE_DECODED_FRAME_SIZE];
    memory_reader_t reader;
    resource_package_t package;
    resource_frame_t frame;
    uint32_t calculated_crc;

    build_fixture(package_data);
    reader = (memory_reader_t){package_data, FIXTURE_SIZE, 0u, false, true};
    TEST_ASSERT_EQ(RESOURCE_FORMAT_READ_FAILED,
                   resource_package_open(&package, memory_read, &reader,
                                         reader.length));
    TEST_ASSERT(!reader.out_of_bounds_read);

    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   open_fixture(package_data, &reader, &package));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_OK,
                   resource_package_read_frame(&package, 0u, &frame));
    reader.fail_reads = true;
    TEST_ASSERT_EQ(RESOURCE_FORMAT_READ_FAILED,
                   resource_package_calculate_crc32(&package, &calculated_crc));
    TEST_ASSERT_EQ(RESOURCE_FORMAT_READ_FAILED,
                   resource_package_decode_frame(&package, &frame, output,
                                                 sizeof(output)));
    TEST_ASSERT(!reader.out_of_bounds_read);
    return 0;
}

int main(void) {
    TEST_ASSERT_EQ(0, test_crc32_standard_vector_and_incremental());
    TEST_ASSERT_EQ(0, test_matches_python_blank_package_golden());
    TEST_ASSERT_EQ(0, test_opens_and_verifies_golden_fixture());
    TEST_ASSERT_EQ(0, test_decodes_raw_and_rle_frames());
    TEST_ASSERT_EQ(0, test_rejects_header_ranges_before_dereferencing());
    TEST_ASSERT_EQ(0, test_rejects_directory_gaps());
    TEST_ASSERT_EQ(0, test_rejects_invalid_reserved_and_directory_entries());
    TEST_ASSERT_EQ(0, test_rejects_corrupt_package_and_frame_crc());
    TEST_ASSERT_EQ(0, test_rle_requires_exact_input_and_output());
    TEST_ASSERT_EQ(0, test_rejects_invalid_frame_ranges_without_bad_reads());
    TEST_ASSERT_EQ(0, test_propagates_read_failures());
    return 0;
}
