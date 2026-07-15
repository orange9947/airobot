#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fake_storage_flash.h"
#include "resource_crc32.h"
#include "resource_format.h"
#include "storage_service.h"
#include "test_harness.h"

#define TEST_PACKAGE_SIZE 120u
#define TEST_PACKAGE_CRC32 0x7B6CDB84u
#define TEST_FRAME_CRC32 0xEFB5AF2Eu
#define TEST_UPDATE_ID 0x10203040u
#define TEST_TICK_LIMIT 10000u
#define CROSS_BOUNDARY_PACKAGE_SIZE 627u
#define RAW_PACKAGE_SIZE (104u + RESOURCE_DECODED_FRAME_SIZE)

static fake_storage_flash_t fake;
static storage_service_t service;
static storage_flash_t storage;
static uint8_t package_data[TEST_PACKAGE_SIZE];
static uint8_t cross_boundary_package[CROSS_BOUNDARY_PACKAGE_SIZE];
static uint8_t raw_package[RAW_PACKAGE_SIZE];
static uint8_t flash_snapshot[FAKE_STORAGE_FLASH_CAPACITY];

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

static uint32_t read_u32_le(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static bool bytes_are_value(const uint8_t *data, uint8_t value,
                            uint32_t length) {
    uint32_t index;
    for (index = 0u; index < length; ++index) {
        if (data[index] != value) {
            return false;
        }
    }
    return true;
}

static uint32_t refresh_package_crc(uint8_t *data, uint32_t length) {
    uint32_t crc;

    write_u32_le(&data[RESOURCE_PACKAGE_CRC_OFFSET], 0u);
    crc = resource_crc32(data, length);
    write_u32_le(&data[RESOURCE_PACKAGE_CRC_OFFSET], crc);
    return crc;
}

static uint32_t crc32_repeat_update(uint32_t state, uint8_t value,
                                    uint32_t length) {
    while (length != 0u) {
        state = resource_crc32_update(state, &value, 1u);
        length--;
    }
    return state;
}

static void build_test_package(void) {
    uint32_t index;

    memset(package_data, 0, sizeof(package_data));
    memcpy(&package_data[0], "ARPK", 4u);
    write_u16_le(&package_data[4], RESOURCE_FORMAT_VERSION);
    write_u16_le(&package_data[6], RESOURCE_PACKAGE_HEADER_SIZE);
    write_u16_le(&package_data[8], RESOURCE_FRAME_WIDTH);
    write_u16_le(&package_data[10], RESOURCE_FRAME_HEIGHT);
    write_u16_le(&package_data[12], 1u);
    write_u16_le(&package_data[14], 1u);
    write_u32_le(&package_data[16], 64u);
    write_u32_le(&package_data[20], 80u);
    write_u32_le(&package_data[24], 104u);
    write_u32_le(&package_data[28], TEST_PACKAGE_SIZE);
    write_u32_le(&package_data[32], TEST_PACKAGE_CRC32);
    package_data[64] = 0u;
    package_data[65] = 1u;
    write_u16_le(&package_data[66], 100u);
    write_u16_le(&package_data[68], 1u);
    package_data[80] = RESOURCE_FRAME_ENCODING_RLE1;
    write_u32_le(&package_data[84], 104u);
    write_u32_le(&package_data[88], 16u);
    write_u32_le(&package_data[92], RESOURCE_DECODED_FRAME_SIZE);
    write_u32_le(&package_data[96], TEST_FRAME_CRC32);
    for (index = 104u; index < sizeof(package_data); index += 2u) {
        package_data[index] = 0xFFu;
        package_data[index + 1u] = 0u;
    }
}

static uint32_t build_cross_boundary_package(void) {
    uint32_t crc_state = resource_crc32_init();
    uint32_t offset = 104u;
    uint32_t index;
    uint8_t value;

    memset(cross_boundary_package, 0, sizeof(cross_boundary_package));
    memcpy(&cross_boundary_package[0], "ARPK", 4u);
    write_u16_le(&cross_boundary_package[4], RESOURCE_FORMAT_VERSION);
    write_u16_le(&cross_boundary_package[6], RESOURCE_PACKAGE_HEADER_SIZE);
    write_u16_le(&cross_boundary_package[8], RESOURCE_FRAME_WIDTH);
    write_u16_le(&cross_boundary_package[10], RESOURCE_FRAME_HEIGHT);
    write_u16_le(&cross_boundary_package[12], 1u);
    write_u16_le(&cross_boundary_package[14], 1u);
    write_u32_le(&cross_boundary_package[16], 64u);
    write_u32_le(&cross_boundary_package[20], 80u);
    write_u32_le(&cross_boundary_package[24], 104u);
    write_u32_le(&cross_boundary_package[28],
                 CROSS_BOUNDARY_PACKAGE_SIZE);
    cross_boundary_package[64] = 0u;
    cross_boundary_package[65] = 1u;
    write_u16_le(&cross_boundary_package[66], 100u);
    write_u16_le(&cross_boundary_package[68], 1u);
    cross_boundary_package[80] = RESOURCE_FRAME_ENCODING_RLE1;
    write_u32_le(&cross_boundary_package[84], 104u);
    write_u32_le(&cross_boundary_package[88], 523u);
    write_u32_le(&cross_boundary_package[92], RESOURCE_DECODED_FRAME_SIZE);

    for (index = 0u; index < 125u; ++index) {
        cross_boundary_package[offset++] = 0x80u;
        cross_boundary_package[offset++] = 0x30u;
    }
    crc_state = crc32_repeat_update(crc_state, 0x30u, 125u);

    cross_boundary_package[offset++] = 0x09u;
    for (index = 0u; index < 10u; ++index) {
        value = (uint8_t)index;
        cross_boundary_package[offset++] = value;
        crc_state = resource_crc32_update(crc_state, &value, 1u);
    }

    for (index = 0u; index < 125u; ++index) {
        cross_boundary_package[offset++] = 0x80u;
        cross_boundary_package[offset++] = 0x31u;
    }
    crc_state = crc32_repeat_update(crc_state, 0x31u, 125u);

    cross_boundary_package[offset++] = 0xFFu;
    cross_boundary_package[offset++] = 0x40u;
    crc_state = crc32_repeat_update(crc_state, 0x40u, 128u);

    for (index = 0u; index < 4u; ++index) {
        value = (uint8_t)(0x41u + index);
        cross_boundary_package[offset++] = 0xFFu;
        cross_boundary_package[offset++] = value;
        crc_state = crc32_repeat_update(crc_state, value, 128u);
    }
    cross_boundary_package[offset++] = 0xFBu;
    cross_boundary_package[offset++] = 0x45u;
    crc_state = crc32_repeat_update(crc_state, 0x45u, 124u);

    if (offset != sizeof(cross_boundary_package)) {
        return 0u;
    }
    write_u32_le(&cross_boundary_package[96],
                 resource_crc32_finalize(crc_state));
    return refresh_package_crc(cross_boundary_package,
                               sizeof(cross_boundary_package));
}

static uint32_t build_raw_package(void) {
    uint32_t index;

    memset(raw_package, 0, sizeof(raw_package));
    memcpy(&raw_package[0], "ARPK", 4u);
    write_u16_le(&raw_package[4], RESOURCE_FORMAT_VERSION);
    write_u16_le(&raw_package[6], RESOURCE_PACKAGE_HEADER_SIZE);
    write_u16_le(&raw_package[8], RESOURCE_FRAME_WIDTH);
    write_u16_le(&raw_package[10], RESOURCE_FRAME_HEIGHT);
    write_u16_le(&raw_package[12], 1u);
    write_u16_le(&raw_package[14], 1u);
    write_u32_le(&raw_package[16], 64u);
    write_u32_le(&raw_package[20], 80u);
    write_u32_le(&raw_package[24], 104u);
    write_u32_le(&raw_package[28], RAW_PACKAGE_SIZE);
    raw_package[64] = 0u;
    raw_package[65] = 1u;
    write_u16_le(&raw_package[66], 100u);
    write_u16_le(&raw_package[68], 1u);
    raw_package[80] = RESOURCE_FRAME_ENCODING_RAW1;
    write_u32_le(&raw_package[84], 104u);
    write_u32_le(&raw_package[88], RESOURCE_DECODED_FRAME_SIZE);
    write_u32_le(&raw_package[92], RESOURCE_DECODED_FRAME_SIZE);
    for (index = 0u; index < RESOURCE_DECODED_FRAME_SIZE; ++index) {
        raw_package[104u + index] = (uint8_t)(index * 37u + 11u);
    }
    write_u32_le(&raw_package[96],
                 resource_crc32(&raw_package[104],
                                RESOURCE_DECODED_FRAME_SIZE));
    return refresh_package_crc(raw_package, sizeof(raw_package));
}

static int tick_once(storage_service_t *instance, fake_storage_flash_t *flash,
                     uint32_t now_ms) {
    fake_storage_flash_begin_tick(flash);
    storage_service_tick(instance, now_ms);
    TEST_ASSERT(flash->calls_this_tick <= 1u);
    return 0;
}

static int drive_to_state(storage_service_t *instance,
                          fake_storage_flash_t *flash,
                          storage_service_state_t expected,
                          uint32_t *now_ms) {
    uint32_t attempt;

    for (attempt = 0u; attempt < TEST_TICK_LIMIT; ++attempt) {
        storage_service_status_t status;
        storage_service_get_status(instance, &status);
        if (status.state == expected) {
            return 0;
        }
        TEST_ASSERT_EQ(0, tick_once(instance, flash, *now_ms));
        (*now_ms)++;
    }
    return 1;
}

static int drive_until_offset(storage_service_t *instance,
                              fake_storage_flash_t *flash,
                              uint32_t expected_offset,
                              uint32_t *now_ms) {
    uint32_t attempt;

    for (attempt = 0u; attempt < TEST_TICK_LIMIT; ++attempt) {
        storage_service_status_t status;
        storage_service_get_status(instance, &status);
        if (status.next_offset == expected_offset &&
            (status.state == STORAGE_SERVICE_READY ||
             status.state == STORAGE_SERVICE_RECEIVING)) {
            return 0;
        }
        TEST_ASSERT_EQ(0, tick_once(instance, flash, *now_ms));
        (*now_ms)++;
    }
    return 1;
}

static int drive_to_phase(storage_service_t *instance,
                          fake_storage_flash_t *flash,
                          storage_service_phase_t expected,
                          uint32_t *now_ms) {
    uint32_t attempt;

    for (attempt = 0u; attempt < TEST_TICK_LIMIT; ++attempt) {
        if (instance->phase == expected &&
            instance->pending_operation == STORAGE_PENDING_NONE) {
            return 0;
        }
        TEST_ASSERT_EQ(0, tick_once(instance, flash, *now_ms));
        (*now_ms)++;
    }
    return 1;
}

static int boot_service(storage_service_t *instance,
                        fake_storage_flash_t *flash, uint32_t *now_ms) {
    storage = fake_storage_flash_interface(flash);
    storage_service_init(instance, &storage);
    return drive_to_state(instance, flash, STORAGE_SERVICE_IDLE, now_ms);
}

static int upload_package(storage_service_t *instance,
                          fake_storage_flash_t *flash, uint32_t update_id,
                          uint32_t *now_ms) {
    uint32_t first_length = 37u;
    uint32_t second_length = TEST_PACKAGE_SIZE - first_length;

    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(instance, update_id, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, *now_ms));
    TEST_ASSERT_EQ(0, drive_to_state(instance, flash, STORAGE_SERVICE_READY,
                                     now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       instance, update_id, 0u, package_data, first_length,
                       resource_crc32(package_data, first_length), *now_ms));
    TEST_ASSERT_EQ(0, drive_until_offset(instance, flash, first_length, now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       instance, update_id, first_length, &package_data[first_length],
                       second_length,
                       resource_crc32(&package_data[first_length], second_length),
                       *now_ms));
    TEST_ASSERT_EQ(0,
                   drive_until_offset(instance, flash, TEST_PACKAGE_SIZE, now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_finish(instance, update_id, *now_ms));
    return drive_to_state(instance, flash, STORAGE_SERVICE_IDLE, now_ms);
}

static int upload_custom_package(storage_service_t *instance,
                                 fake_storage_flash_t *flash,
                                 uint32_t update_id, const uint8_t *data,
                                 uint32_t length, uint32_t package_crc32,
                                 uint32_t *now_ms) {
    uint32_t offset = 0u;

    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(instance, update_id, length,
                                         package_crc32,
                                         RESOURCE_FORMAT_VERSION, *now_ms));
    TEST_ASSERT_EQ(0, drive_to_state(instance, flash, STORAGE_SERVICE_READY,
                                     now_ms));
    while (offset < length) {
        uint32_t remaining = length - offset;
        uint16_t chunk_length =
            remaining > STORAGE_SERVICE_MAX_CHUNK_SIZE
                ? (uint16_t)STORAGE_SERVICE_MAX_CHUNK_SIZE
                : (uint16_t)remaining;

        TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                       storage_service_write_chunk(
                           instance, update_id, offset, &data[offset],
                           chunk_length,
                           resource_crc32(&data[offset], chunk_length),
                           *now_ms));
        offset += chunk_length;
        TEST_ASSERT_EQ(0,
                       drive_until_offset(instance, flash, offset, now_ms));
    }
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_finish(instance, update_id, *now_ms));
    return drive_to_state(instance, flash, STORAGE_SERVICE_IDLE, now_ms);
}

static int prepare_commit_marker(storage_service_t *instance,
                                 fake_storage_flash_t *flash,
                                 uint32_t update_id, uint32_t *now_ms) {
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(instance, update_id,
                                         TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, *now_ms));
    TEST_ASSERT_EQ(0, drive_to_state(instance, flash, STORAGE_SERVICE_READY,
                                     now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       instance, update_id, 0u, package_data,
                       sizeof(package_data),
                       resource_crc32(package_data, sizeof(package_data)),
                       *now_ms));
    TEST_ASSERT_EQ(0, drive_until_offset(instance, flash,
                                         sizeof(package_data), now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_finish(instance, update_id, *now_ms));
    TEST_ASSERT_EQ(0, drive_to_phase(instance, flash,
                                     STORAGE_PHASE_WRITE_COMMIT_MARKER,
                                     now_ms));
    TEST_ASSERT_EQ(0, tick_once(instance, flash, (*now_ms)++));
    TEST_ASSERT_EQ(STORAGE_PENDING_COMMIT_MARKER,
                   instance->pending_operation);
    TEST_ASSERT(instance->commit_marker_started);
    TEST_ASSERT_EQ(STORAGE_SERVICE_COMMITTING, instance->state);
    return 0;
}

static int fail_current_flash_read(storage_service_t *instance,
                                   fake_storage_flash_t *flash,
                                   uint32_t failure_count,
                                   uint32_t *now_ms) {
    uint32_t attempt;

    for (attempt = 0u; attempt < failure_count; ++attempt) {
        flash->fail_next = STORAGE_FLASH_IO;
        TEST_ASSERT_EQ(0, tick_once(instance, flash, (*now_ms)++));
    }
    return 0;
}

static void build_committed_bank_header(uint8_t header[STORAGE_BANK_HEADER_SIZE],
                                        storage_bank_t bank,
                                        uint32_t generation,
                                        uint32_t package_length,
                                        uint32_t package_crc32) {
    uint32_t header_crc;

    memset(header, 0, STORAGE_BANK_HEADER_SIZE);
    memcpy(&header[0], "ARBK", 4u);
    write_u16_le(&header[4], STORAGE_BANK_FORMAT_VERSION);
    write_u16_le(&header[6], STORAGE_BANK_HEADER_SIZE);
    header[8] = (uint8_t)bank;
    header[9] = STORAGE_BANK_STATE_PREPARED;
    write_u16_le(&header[10], RESOURCE_FORMAT_VERSION);
    write_u32_le(&header[12], generation);
    write_u32_le(&header[16], package_length);
    write_u32_le(&header[20], package_crc32);
    header_crc = resource_crc32(header, STORAGE_BANK_COMMIT_MARKER_OFFSET);
    write_u32_le(&header[24], header_crc);
    write_u32_le(&header[STORAGE_BANK_COMMIT_MARKER_OFFSET],
                 STORAGE_BANK_COMMIT_MARKER);
}

static void install_committed_bank_data(fake_storage_flash_t *flash,
                                        storage_bank_t bank,
                                        uint32_t generation,
                                        const uint8_t *data,
                                        uint32_t package_length,
                                        uint32_t package_crc32) {
    uint8_t header[STORAGE_BANK_HEADER_SIZE];
    uint32_t bank_base = storage_service_bank_base(bank);

    build_committed_bank_header(header, bank, generation, package_length,
                                package_crc32);
    memcpy(&flash->bytes[bank_base], header, sizeof(header));
    memcpy(&flash->bytes[bank_base + STORAGE_FLASH_SECTOR_SIZE], data,
           package_length);
}

static void install_committed_bank(fake_storage_flash_t *flash,
                                   storage_bank_t bank,
                                   uint32_t generation) {
    install_committed_bank_data(flash, bank, generation, package_data,
                                sizeof(package_data), TEST_PACKAGE_CRC32);
}

static void build_journal_record(uint8_t record[STORAGE_JOURNAL_RECORD_SIZE],
                                 storage_bank_t bank, uint32_t generation) {
    uint32_t crc;

    memset(record, 0, STORAGE_JOURNAL_RECORD_SIZE);
    memcpy(record, "ARJR", 4u);
    write_u16_le(&record[4], 1u);
    write_u16_le(&record[6], STORAGE_JOURNAL_RECORD_SIZE);
    record[8] = (uint8_t)bank;
    write_u32_le(&record[12], generation);
    write_u32_le(&record[16], TEST_PACKAGE_CRC32);
    write_u32_le(&record[20], TEST_PACKAGE_SIZE);
    crc = resource_crc32(record, STORAGE_JOURNAL_RECORD_SIZE);
    write_u32_le(&record[24], crc);
}

static int test_erased_flash_boots_without_active_bank(void) {
    storage_service_status_t status;
    uint32_t now_ms = 0u;

    fake_storage_flash_init(&fake);
    TEST_ASSERT(sizeof(storage_service_t) <= 1536u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_NONE, status.active_bank);
    TEST_ASSERT_EQ(0u, status.generation);
    TEST_ASSERT_EQ(0u, status.update_id);
    TEST_ASSERT_EQ(0u, status.next_offset);
    TEST_ASSERT_EQ(0u, status.total_size);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_NONE, status.error);
    TEST_ASSERT(fake.read_calls > 0u);
    TEST_ASSERT_EQ(1u, fake.max_calls_per_tick);
    return 0;
}

static int test_upload_is_ordered_idempotent_and_survives_reboot(void) {
    storage_service_status_t status;
    uint32_t now_ms = 100u;
    uint32_t package_address;
    uint32_t package_length;
    uint32_t package_crc32;
    uint8_t changed[37];

    fake_storage_flash_init(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, TEST_UPDATE_ID,
                                         TEST_PACKAGE_SIZE, TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_READY, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_BAD_OFFSET,
                   storage_service_write_chunk(
                       &service, TEST_UPDATE_ID, 1u, package_data, 37u,
                       resource_crc32(package_data, 37u), now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       &service, TEST_UPDATE_ID, 0u, package_data, 37u,
                       resource_crc32(package_data, 37u), now_ms));
    TEST_ASSERT_EQ(0, drive_until_offset(&service, &fake, 37u, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       &service, TEST_UPDATE_ID, 0u, package_data, 37u,
                       resource_crc32(package_data, 37u), now_ms));
    memcpy(changed, package_data, sizeof(changed));
    changed[10] ^= 1u;
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_CONFLICT,
                   storage_service_write_chunk(
                       &service, TEST_UPDATE_ID, 0u, changed, sizeof(changed),
                       resource_crc32(changed, sizeof(changed)), now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       &service, TEST_UPDATE_ID, 37u, &package_data[37],
                       TEST_PACKAGE_SIZE - 37u,
                       resource_crc32(&package_data[37],
                                      TEST_PACKAGE_SIZE - 37u),
                       now_ms));
    TEST_ASSERT_EQ(0,
                   drive_until_offset(&service, &fake, TEST_PACKAGE_SIZE, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_finish(&service, TEST_UPDATE_ID, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE, &now_ms));

    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(1u, status.generation);
    TEST_ASSERT_EQ(TEST_UPDATE_ID, status.update_id);
    TEST_ASSERT_EQ(TEST_PACKAGE_SIZE, status.next_offset);
    TEST_ASSERT_EQ(TEST_PACKAGE_SIZE, status.total_size);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_NONE, status.error);
    TEST_ASSERT_EQ(STORAGE_BANK_COMMIT_MARKER,
                   read_u32_le(&fake.bytes[STORAGE_BANK_A_BASE +
                                           STORAGE_BANK_COMMIT_MARKER_OFFSET]));
    TEST_ASSERT(memcmp(&fake.bytes[STORAGE_BANK_A_PACKAGE_BASE], package_data,
                       sizeof(package_data)) == 0);
    TEST_ASSERT(storage_service_get_active_package(
        &service, &package_address, &package_length, &package_crc32));
    TEST_ASSERT_EQ(STORAGE_BANK_A_PACKAGE_BASE, package_address);
    TEST_ASSERT_EQ(TEST_PACKAGE_SIZE, package_length);
    TEST_ASSERT_EQ(TEST_PACKAGE_CRC32, package_crc32);

    fake_storage_flash_power_cycle(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(1u, status.generation);
    TEST_ASSERT_EQ(1u, fake.max_calls_per_tick);
    return 0;
}

static int test_new_generation_uses_inactive_bank(void) {
    storage_service_status_t status;
    uint32_t now_ms = 1000u;

    fake_storage_flash_init(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, upload_package(&service, &fake, 1u, &now_ms));
    TEST_ASSERT_EQ(0, upload_package(&service, &fake, 2u, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(2u, status.generation);

    fake_storage_flash_power_cycle(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(2u, status.generation);
    return 0;
}

static int test_generation_selection_is_wrap_safe(void) {
    storage_service_status_t status;
    uint32_t now_ms = 2000u;

    TEST_ASSERT(storage_service_generation_is_newer(0u, UINT32_MAX));
    TEST_ASSERT(storage_service_generation_is_newer(1u, UINT32_MAX));
    TEST_ASSERT(!storage_service_generation_is_newer(UINT32_MAX, 0u));
    TEST_ASSERT(!storage_service_generation_is_newer(7u, 7u));

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, UINT32_MAX);
    install_committed_bank(&fake, STORAGE_BANK_B, 0u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(0u, status.generation);
    return 0;
}

static int test_partial_commit_marker_never_replaces_old_bank(void) {
    uint8_t marker[4];
    storage_service_status_t status;
    uint32_t now_ms = 3000u;
    uint32_t prefix;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    install_committed_bank(&fake, STORAGE_BANK_B, 11u);
    memset(&fake.bytes[STORAGE_BANK_B_BASE + STORAGE_BANK_COMMIT_MARKER_OFFSET],
           0xFF, 4u);
    memcpy(flash_snapshot, fake.bytes, sizeof(flash_snapshot));
    write_u32_le(marker, STORAGE_BANK_COMMIT_MARKER);

    for (prefix = 0u; prefix < sizeof(marker); ++prefix) {
        memcpy(fake.bytes, flash_snapshot, sizeof(flash_snapshot));
        fake_storage_flash_direct_program(
            &fake, STORAGE_BANK_B_BASE + STORAGE_BANK_COMMIT_MARKER_OFFSET,
            marker, prefix);
        fake_storage_flash_power_cycle(&fake);
        TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
        storage_service_get_status(&service, &status);
        TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
        TEST_ASSERT_EQ(10u, status.generation);
    }
    return 0;
}

static int test_resets_before_marker_always_keep_old_bank(void) {
    storage_service_status_t status;
    uint32_t now_ms = 3500u;
    uint32_t stage;

    for (stage = 0u; stage < 4u; ++stage) {
        fake_storage_flash_init(&fake);
        install_committed_bank(&fake, STORAGE_BANK_A, 10u);
        TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
        TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                       storage_service_begin(
                           &service, TEST_UPDATE_ID + stage, TEST_PACKAGE_SIZE,
                           TEST_PACKAGE_CRC32, RESOURCE_FORMAT_VERSION, now_ms));
        if (stage >= 1u) {
            TEST_ASSERT_EQ(0, drive_to_state(&service, &fake,
                                             STORAGE_SERVICE_READY, &now_ms));
        }
        if (stage >= 2u) {
            TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                           storage_service_write_chunk(
                               &service, TEST_UPDATE_ID + stage, 0u,
                               package_data, sizeof(package_data),
                               resource_crc32(package_data,
                                              sizeof(package_data)),
                               now_ms));
            TEST_ASSERT_EQ(0, drive_until_offset(&service, &fake,
                                                 sizeof(package_data), &now_ms));
        }
        if (stage >= 3u) {
            TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                           storage_service_finish(&service,
                                                  TEST_UPDATE_ID + stage,
                                                  now_ms));
            TEST_ASSERT_EQ(0, drive_to_phase(&service, &fake,
                                             STORAGE_PHASE_VERIFY_FRAMES,
                                             &now_ms));
        }
        fake_storage_flash_power_cycle(&fake);
        TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
        storage_service_get_status(&service, &status);
        TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
        TEST_ASSERT_EQ(10u, status.generation);
    }
    return 0;
}

static int test_full_marker_recovers_without_journal(void) {
    uint32_t now_ms = 4000u;
    storage_service_status_t status;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 20u);
    install_committed_bank(&fake, STORAGE_BANK_B, 21u);
    memset(&fake.bytes[STORAGE_JOURNAL_BASE], 0xFF, STORAGE_FLASH_SECTOR_SIZE);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(21u, status.generation);
    return 0;
}

static int test_every_partial_journal_record_recovers_from_bank_header(void) {
    uint8_t record[STORAGE_JOURNAL_RECORD_SIZE];
    storage_service_status_t status;
    uint32_t now_ms = 4500u;
    uint32_t prefix;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 20u);
    install_committed_bank(&fake, STORAGE_BANK_B, 21u);
    build_journal_record(record, STORAGE_BANK_B, 21u);
    memcpy(flash_snapshot, fake.bytes, sizeof(flash_snapshot));

    for (prefix = 0u; prefix < sizeof(record); ++prefix) {
        memcpy(fake.bytes, flash_snapshot, sizeof(flash_snapshot));
        fake_storage_flash_direct_program(&fake, STORAGE_JOURNAL_BASE, record,
                                          prefix);
        fake_storage_flash_power_cycle(&fake);
        TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
        storage_service_get_status(&service, &status);
        TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
        TEST_ASSERT_EQ(21u, status.generation);
    }
    return 0;
}

static int test_full_journal_is_compacted_after_bank_commit(void) {
    storage_service_status_t status;
    uint32_t now_ms = 4700u;

    fake_storage_flash_init(&fake);
    memset(&fake.bytes[STORAGE_JOURNAL_BASE], 0u, STORAGE_JOURNAL_SIZE);
    install_committed_bank(&fake, STORAGE_BANK_A, 30u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, upload_package(&service, &fake, 41u, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(31u, status.generation);
    TEST_ASSERT(memcmp(&fake.bytes[STORAGE_JOURNAL_BASE], "ARJR", 4u) == 0);
    TEST_ASSERT(bytes_are_value(&fake.bytes[STORAGE_JOURNAL_BASE +
                                           STORAGE_JOURNAL_RECORD_SIZE],
                                0xFFu,
                                STORAGE_JOURNAL_SIZE -
                                    STORAGE_JOURNAL_RECORD_SIZE));

    fake_storage_flash_power_cycle(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(31u, status.generation);
    return 0;
}

static int test_journal_failure_keeps_committed_bank_with_warning(void) {
    storage_service_status_t status;
    uint32_t now_ms = 4800u;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 40u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, 42u, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_READY, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       &service, 42u, 0u, package_data, sizeof(package_data),
                       resource_crc32(package_data, sizeof(package_data)),
                       now_ms));
    TEST_ASSERT_EQ(0, drive_until_offset(&service, &fake, sizeof(package_data),
                                         &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_finish(&service, 42u, now_ms));
    TEST_ASSERT_EQ(0, drive_to_phase(&service, &fake,
                                     STORAGE_PHASE_WRITE_JOURNAL, &now_ms));
    TEST_ASSERT_EQ(STORAGE_BANK_COMMIT_MARKER,
                   read_u32_le(&fake.bytes[STORAGE_BANK_B_BASE +
                                           STORAGE_BANK_COMMIT_MARKER_OFFSET]));
    fake.fail_next = STORAGE_FLASH_IO;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_IDLE, status.state);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(41u, status.generation);
    TEST_ASSERT(status.journal_warning);
    TEST_ASSERT(status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_JOURNAL, status.error);

    fake_storage_flash_power_cycle(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(41u, status.generation);
    return 0;
}

static int test_flash_failure_does_not_replace_active_bank(void) {
    storage_service_status_t status;
    uint32_t now_ms = 4900u;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 50u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, 51u, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    fake.fail_next = STORAGE_FLASH_PROTECTED;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_FAILED, status.state);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_PROTECTED, status.error);
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_IDLE, status.state);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(50u, status.generation);
    return 0;
}

static int test_stuck_flash_busy_times_out_and_preserves_old_bank(void) {
    storage_service_status_t status;
    uint32_t now_ms = 4950u;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 60u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, 61u, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    fake.stay_busy = true;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms));
    TEST_ASSERT_EQ(STORAGE_PENDING_ERASE_BANK, service.pending_operation);
    now_ms += STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_FAILED, status.state);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_TIMEOUT, status.error);
    TEST_ASSERT_EQ(STORAGE_PENDING_NONE, service.pending_operation);
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_IDLE, status.state);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(60u, status.generation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_TIMEOUT, status.error);
    fake_storage_flash_power_cycle(&fake);
    return 0;
}

static int test_abort_timeout_and_link_loss_preserve_active_bank(void) {
    uint32_t now_ms = 5000u;
    storage_service_status_t status;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 4u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));

    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, 10u, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PENDING_ERASE_BANK, service.pending_operation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_abort(&service, 10u, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE, &now_ms));

    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, 11u, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    now_ms += STORAGE_SERVICE_TIMEOUT_MS;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_TIMEOUT, status.error);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);

    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, 12u, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_READY, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       &service, 12u, 0u, package_data, sizeof(package_data),
                       resource_crc32(package_data, sizeof(package_data)),
                       now_ms));
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PENDING_CHUNK_PAGE, service.pending_operation);
    storage_service_link_lost(&service, now_ms);
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_LINK_LOST, status.error);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    return 0;
}

static int test_matching_status_touch_extends_session(void) {
    uint32_t now_ms = 5500u;
    storage_service_status_t status;

    fake_storage_flash_init(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, 20u, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_READY,
                                  &now_ms));

    now_ms += STORAGE_SERVICE_TIMEOUT_MS - 1u;
    TEST_ASSERT(!storage_service_touch(&service, 21u, now_ms));
    TEST_ASSERT(storage_service_touch(&service, 20u, now_ms));
    now_ms += STORAGE_SERVICE_TIMEOUT_MS - 1u;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_READY, status.state);

    now_ms++;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE,
                                  &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_TIMEOUT, status.error);
    return 0;
}

static int test_commit_marker_is_an_uncancellable_point_of_no_return(void) {
    storage_service_status_t status;
    uint32_t now_ms = 100000u;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, prepare_commit_marker(&service, &fake, 70u, &now_ms));

    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_BAD_STATE,
                   storage_service_abort(&service, 70u, now_ms));
    storage_service_link_lost(&service, now_ms);
    service.last_activity_ms = now_ms - STORAGE_SERVICE_TIMEOUT_MS;
    TEST_ASSERT_EQ(0, fail_current_flash_read(
                          &service, &fake,
                          STORAGE_SERVICE_FLASH_READ_RETRY_LIMIT, &now_ms));
    TEST_ASSERT_EQ(STORAGE_PENDING_COMMIT_MARKER,
                   service.pending_operation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_COMMITTING, service.state);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_NONE, service.error);

    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PHASE_VERIFY_COMMIT_MARKER, service.phase);
    TEST_ASSERT_EQ(STORAGE_PENDING_NONE, service.pending_operation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_BAD_STATE,
                   storage_service_abort(&service, 70u, now_ms));
    storage_service_link_lost(&service, now_ms);
    TEST_ASSERT_EQ(0, fail_current_flash_read(
                          &service, &fake,
                          STORAGE_SERVICE_FLASH_READ_RETRY_LIMIT, &now_ms));
    TEST_ASSERT_EQ(STORAGE_PHASE_VERIFY_COMMIT_MARKER, service.phase);
    TEST_ASSERT_EQ(STORAGE_SERVICE_COMMITTING, service.state);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_NONE, service.error);

    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(0, drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE,
                                     &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(11u, status.generation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_NONE, status.error);

    fake_storage_flash_power_cycle(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(11u, status.generation);
    TEST_ASSERT_EQ(1u, fake.max_calls_per_tick);
    return 0;
}

static int test_commit_marker_power_cut_windows_are_atomic(void) {
    storage_service_status_t status;
    uint32_t now_ms = 110000u;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, prepare_commit_marker(&service, &fake, 71u, &now_ms));
    TEST_ASSERT_EQ(0xFFFFFFFFu,
                   read_u32_le(&fake.bytes[STORAGE_BANK_B_BASE +
                                           STORAGE_BANK_COMMIT_MARKER_OFFSET]));
    fake_storage_flash_power_cycle(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(10u, status.generation);

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, prepare_commit_marker(&service, &fake, 72u, &now_ms));
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PHASE_VERIFY_COMMIT_MARKER, service.phase);
    TEST_ASSERT_EQ(STORAGE_BANK_COMMIT_MARKER,
                   read_u32_le(&fake.bytes[STORAGE_BANK_B_BASE +
                                           STORAGE_BANK_COMMIT_MARKER_OFFSET]));
    fake_storage_flash_power_cycle(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(11u, status.generation);
    return 0;
}

static int test_commit_marker_read_error_is_bounded_and_explicit(void) {
    storage_service_status_t status;
    uint32_t now_ms = 120000u;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 20u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, prepare_commit_marker(&service, &fake, 73u, &now_ms));
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PHASE_VERIFY_COMMIT_MARKER, service.phase);

    TEST_ASSERT_EQ(0, fail_current_flash_read(
                          &service, &fake,
                          STORAGE_SERVICE_FLASH_READ_RETRY_LIMIT + 1u,
                          &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_FAILED, status.state);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_IO, status.error);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(20u, status.generation);

    fake_storage_flash_power_cycle(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(21u, status.generation);
    return 0;
}

static int test_boot_read_errors_retry_then_fall_back(void) {
    storage_service_status_t status;
    uint32_t now_ms = 130000u;
    uint32_t exhausted_reads = STORAGE_SERVICE_FLASH_READ_RETRY_LIMIT + 1u;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    install_committed_bank(&fake, STORAGE_BANK_B, 11u);
    storage = fake_storage_flash_interface(&fake);
    storage_service_init(&service, &storage);
    TEST_ASSERT_EQ(0, drive_to_phase(&service, &fake,
                                     STORAGE_PHASE_VERIFY_CRC, &now_ms));
    TEST_ASSERT_EQ(0,
                   fail_current_flash_read(&service, &fake, 1u, &now_ms));
    TEST_ASSERT_EQ(STORAGE_PHASE_VERIFY_CRC, service.phase);
    TEST_ASSERT_EQ(0, drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE,
                                     &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_B, status.active_bank);
    TEST_ASSERT_EQ(11u, status.generation);
    TEST_ASSERT(!status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_NONE, status.error);

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    storage = fake_storage_flash_interface(&fake);
    storage_service_init(&service, &storage);
    TEST_ASSERT_EQ(0, fail_current_flash_read(
                          &service, &fake, exhausted_reads, &now_ms));
    TEST_ASSERT_EQ(STORAGE_PHASE_BOOT_BANK_A, service.phase);
    TEST_ASSERT_EQ(0, drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE,
                                     &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT(status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_IO, status.error);

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    install_committed_bank(&fake, STORAGE_BANK_B, 11u);
    storage = fake_storage_flash_interface(&fake);
    storage_service_init(&service, &storage);
    TEST_ASSERT_EQ(0, drive_to_phase(&service, &fake,
                                     STORAGE_PHASE_BOOT_BANK_B, &now_ms));
    TEST_ASSERT_EQ(0, fail_current_flash_read(
                          &service, &fake, exhausted_reads, &now_ms));
    TEST_ASSERT_EQ(0, drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE,
                                     &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(10u, status.generation);
    TEST_ASSERT(status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_IO, status.error);

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    install_committed_bank(&fake, STORAGE_BANK_B, 11u);
    storage = fake_storage_flash_interface(&fake);
    storage_service_init(&service, &storage);
    TEST_ASSERT_EQ(0, drive_to_phase(&service, &fake,
                                     STORAGE_PHASE_VERIFY_CRC, &now_ms));
    TEST_ASSERT_EQ(0, fail_current_flash_read(
                          &service, &fake, exhausted_reads, &now_ms));
    TEST_ASSERT_EQ(0, drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE,
                                     &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(10u, status.generation);
    TEST_ASSERT(status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_IO, status.error);

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    install_committed_bank(&fake, STORAGE_BANK_B, 11u);
    storage = fake_storage_flash_interface(&fake);
    storage_service_init(&service, &storage);
    TEST_ASSERT_EQ(0, drive_to_phase(&service, &fake,
                                     STORAGE_PHASE_VERIFY_FRAME_DATA,
                                     &now_ms));
    TEST_ASSERT_EQ(0, fail_current_flash_read(
                          &service, &fake, exhausted_reads, &now_ms));
    TEST_ASSERT_EQ(0, drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE,
                                     &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(10u, status.generation);
    TEST_ASSERT(status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_IO, status.error);
    TEST_ASSERT_EQ(1u, fake.max_calls_per_tick);
    return 0;
}

static int test_boot_busy_reads_time_out_and_finish_degraded(void) {
    storage_service_status_t status;
    uint32_t now_ms = 140000u;

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    fake.pending = FAKE_STORAGE_PENDING_PROGRAM;
    fake.stay_busy = true;
    storage = fake_storage_flash_interface(&fake);
    storage_service_init(&service, &storage);

    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PHASE_BOOT_JOURNAL, service.phase);
    TEST_ASSERT(service.flash_busy_retry_active);
    now_ms = service.flash_retry_started_ms +
             STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PHASE_BOOT_BANK_A, service.phase);

    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT(service.flash_busy_retry_active);
    now_ms = service.flash_retry_started_ms +
             STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PHASE_BOOT_BANK_B, service.phase);

    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT(service.flash_busy_retry_active);
    now_ms = service.flash_retry_started_ms +
             STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PHASE_BOOT_SELECT, service.phase);
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_SERVICE_IDLE, status.state);
    TEST_ASSERT_EQ(STORAGE_BANK_NONE, status.active_bank);
    TEST_ASSERT(status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_TIMEOUT, status.error);
    TEST_ASSERT_EQ(1u, fake.max_calls_per_tick);

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    install_committed_bank(&fake, STORAGE_BANK_B, 11u);
    storage = fake_storage_flash_interface(&fake);
    storage_service_init(&service, &storage);
    TEST_ASSERT_EQ(0, drive_to_phase(&service, &fake,
                                     STORAGE_PHASE_VERIFY_CRC, &now_ms));
    fake.pending = FAKE_STORAGE_PENDING_PROGRAM;
    fake.stay_busy = true;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT(service.flash_busy_retry_active);
    now_ms = service.flash_retry_started_ms +
             STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS;
    TEST_ASSERT_EQ(0, tick_once(&service, &fake, now_ms++));
    TEST_ASSERT_EQ(STORAGE_PHASE_BOOT_SELECT, service.phase);
    fake.pending = FAKE_STORAGE_PENDING_NONE;
    fake.stay_busy = false;
    TEST_ASSERT_EQ(0, drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE,
                                     &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(10u, status.generation);
    TEST_ASSERT(status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_FLASH_TIMEOUT, status.error);
    return 0;
}

static int test_bad_rle_with_valid_package_crc_never_commits(void) {
    uint8_t bad_package[TEST_PACKAGE_SIZE];
    storage_service_status_t status;
    uint32_t now_ms = 5800u;
    uint32_t bad_package_crc;

    memcpy(bad_package, package_data, sizeof(bad_package));
    bad_package[104] = 0xFEu;
    bad_package_crc = refresh_package_crc(bad_package, sizeof(bad_package));

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 7u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, upload_custom_package(&service, &fake, 30u,
                                            bad_package,
                                            sizeof(bad_package),
                                            bad_package_crc, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(7u, status.generation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_PACKAGE_FORMAT, status.error);
    TEST_ASSERT_EQ(0xFFFFFFFFu,
                   read_u32_le(&fake.bytes[STORAGE_BANK_B_BASE +
                                           STORAGE_BANK_COMMIT_MARKER_OFFSET]));
    return 0;
}

static int test_bad_decoded_crc_with_valid_package_crc_never_commits(void) {
    uint8_t bad_package[TEST_PACKAGE_SIZE];
    storage_service_status_t status;
    uint32_t now_ms = 5900u;
    uint32_t bad_package_crc;

    memcpy(bad_package, package_data, sizeof(bad_package));
    write_u32_le(&bad_package[96], TEST_FRAME_CRC32 ^ 1u);
    bad_package_crc = refresh_package_crc(bad_package, sizeof(bad_package));

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 8u);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, upload_custom_package(&service, &fake, 31u,
                                            bad_package,
                                            sizeof(bad_package),
                                            bad_package_crc, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(8u, status.generation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_PACKAGE_CRC, status.error);
    TEST_ASSERT_EQ(0xFFFFFFFFu,
                   read_u32_le(&fake.bytes[STORAGE_BANK_B_BASE +
                                           STORAGE_BANK_COMMIT_MARKER_OFFSET]));
    return 0;
}

static int test_boot_falls_back_from_newer_bank_with_bad_rle(void) {
    uint8_t bad_package[TEST_PACKAGE_SIZE];
    storage_service_status_t status;
    uint32_t now_ms = 5950u;
    uint32_t bad_package_crc;

    memcpy(bad_package, package_data, sizeof(bad_package));
    bad_package[104] = 0xFEu;
    bad_package_crc = refresh_package_crc(bad_package, sizeof(bad_package));

    fake_storage_flash_init(&fake);
    install_committed_bank(&fake, STORAGE_BANK_A, 10u);
    install_committed_bank_data(&fake, STORAGE_BANK_B, 11u, bad_package,
                                sizeof(bad_package), bad_package_crc);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(10u, status.generation);
    TEST_ASSERT(status.degraded);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_PACKAGE_FORMAT, status.error);
    TEST_ASSERT_EQ(1u, fake.max_calls_per_tick);
    return 0;
}

static int test_rle_state_crosses_flash_read_boundaries(void) {
    storage_service_status_t status;
    uint32_t now_ms = 5980u;
    uint32_t package_crc32 = build_cross_boundary_package();

    TEST_ASSERT(package_crc32 != 0u);
    fake_storage_flash_init(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, upload_custom_package(
                          &service, &fake, 32u, cross_boundary_package,
                          sizeof(cross_boundary_package), package_crc32,
                          &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(1u, status.generation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_NONE, status.error);
    TEST_ASSERT_EQ(STORAGE_BANK_COMMIT_MARKER,
                   read_u32_le(&fake.bytes[STORAGE_BANK_A_BASE +
                                           STORAGE_BANK_COMMIT_MARKER_OFFSET]));
    TEST_ASSERT_EQ(1u, fake.max_calls_per_tick);
    return 0;
}

static int test_raw_frame_is_stream_verified(void) {
    storage_service_status_t status;
    uint32_t now_ms = 5990u;
    uint32_t package_crc32 = build_raw_package();

    TEST_ASSERT(package_crc32 != 0u);
    fake_storage_flash_init(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    TEST_ASSERT_EQ(0, upload_custom_package(
                          &service, &fake, 33u, raw_package,
                          sizeof(raw_package), package_crc32, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_A, status.active_bank);
    TEST_ASSERT_EQ(1u, status.generation);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_NONE, status.error);
    TEST_ASSERT_EQ(1u, fake.max_calls_per_tick);
    return 0;
}

static int test_invalid_package_never_commits(void) {
    uint32_t now_ms = 6000u;
    storage_service_status_t status;

    fake_storage_flash_init(&fake);
    TEST_ASSERT_EQ(0, boot_service(&service, &fake, &now_ms));
    package_data[104] ^= 1u;
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_begin(&service, 31u, TEST_PACKAGE_SIZE,
                                         TEST_PACKAGE_CRC32,
                                         RESOURCE_FORMAT_VERSION, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_READY, &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_write_chunk(
                       &service, 31u, 0u, package_data, sizeof(package_data),
                       resource_crc32(package_data, sizeof(package_data)), now_ms));
    TEST_ASSERT_EQ(0, drive_until_offset(&service, &fake, sizeof(package_data),
                                         &now_ms));
    TEST_ASSERT_EQ(STORAGE_SERVICE_RESULT_OK,
                   storage_service_finish(&service, 31u, now_ms));
    TEST_ASSERT_EQ(0,
                   drive_to_state(&service, &fake, STORAGE_SERVICE_IDLE, &now_ms));
    storage_service_get_status(&service, &status);
    TEST_ASSERT_EQ(STORAGE_BANK_NONE, status.active_bank);
    TEST_ASSERT_EQ(31u, status.update_id);
    TEST_ASSERT_EQ(TEST_PACKAGE_SIZE, status.next_offset);
    TEST_ASSERT_EQ(TEST_PACKAGE_SIZE, status.total_size);
    TEST_ASSERT_EQ(STORAGE_SERVICE_ERROR_PACKAGE_CRC, status.error);
    TEST_ASSERT_EQ(0xFFFFFFFFu,
                   read_u32_le(&fake.bytes[STORAGE_BANK_A_BASE +
                                           STORAGE_BANK_COMMIT_MARKER_OFFSET]));
    build_test_package();
    return 0;
}

int main(void) {
    build_test_package();
    TEST_ASSERT_EQ(0, test_erased_flash_boots_without_active_bank());
    TEST_ASSERT_EQ(0, test_upload_is_ordered_idempotent_and_survives_reboot());
    TEST_ASSERT_EQ(0, test_new_generation_uses_inactive_bank());
    TEST_ASSERT_EQ(0, test_generation_selection_is_wrap_safe());
    TEST_ASSERT_EQ(0, test_partial_commit_marker_never_replaces_old_bank());
    TEST_ASSERT_EQ(0, test_resets_before_marker_always_keep_old_bank());
    TEST_ASSERT_EQ(0, test_full_marker_recovers_without_journal());
    TEST_ASSERT_EQ(0,
                   test_every_partial_journal_record_recovers_from_bank_header());
    TEST_ASSERT_EQ(0, test_full_journal_is_compacted_after_bank_commit());
    TEST_ASSERT_EQ(0,
                   test_journal_failure_keeps_committed_bank_with_warning());
    TEST_ASSERT_EQ(0, test_flash_failure_does_not_replace_active_bank());
    TEST_ASSERT_EQ(0,
                   test_stuck_flash_busy_times_out_and_preserves_old_bank());
    TEST_ASSERT_EQ(0, test_abort_timeout_and_link_loss_preserve_active_bank());
    TEST_ASSERT_EQ(0, test_matching_status_touch_extends_session());
    TEST_ASSERT_EQ(
        0, test_commit_marker_is_an_uncancellable_point_of_no_return());
    TEST_ASSERT_EQ(0, test_commit_marker_power_cut_windows_are_atomic());
    TEST_ASSERT_EQ(
        0, test_commit_marker_read_error_is_bounded_and_explicit());
    TEST_ASSERT_EQ(0, test_boot_read_errors_retry_then_fall_back());
    TEST_ASSERT_EQ(0, test_boot_busy_reads_time_out_and_finish_degraded());
    TEST_ASSERT_EQ(0, test_bad_rle_with_valid_package_crc_never_commits());
    TEST_ASSERT_EQ(
        0, test_bad_decoded_crc_with_valid_package_crc_never_commits());
    TEST_ASSERT_EQ(0, test_boot_falls_back_from_newer_bank_with_bad_rle());
    TEST_ASSERT_EQ(0, test_rle_state_crosses_flash_read_boundaries());
    TEST_ASSERT_EQ(0, test_raw_frame_is_stream_verified());
    TEST_ASSERT_EQ(0, test_invalid_package_never_commits());
    return 0;
}
