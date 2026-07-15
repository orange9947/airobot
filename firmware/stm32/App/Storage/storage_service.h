#ifndef AIROBOT_STORAGE_SERVICE_H
#define AIROBOT_STORAGE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "resource_format.h"
#include "storage_flash.h"

#define STORAGE_FLASH_CAPACITY_REQUIRED 0x100000u
#define STORAGE_FLASH_SECTOR_SIZE 0x1000u
#define STORAGE_FLASH_PAGE_SIZE 256u
#define STORAGE_JOURNAL_BASE 0x000000u
#define STORAGE_JOURNAL_SIZE STORAGE_FLASH_SECTOR_SIZE
#define STORAGE_JOURNAL_RECORD_SIZE 32u
#define STORAGE_BANK_A_BASE 0x001000u
#define STORAGE_BANK_A_END 0x080000u
#define STORAGE_BANK_B_BASE 0x080000u
#define STORAGE_BANK_B_END 0x100000u
#define STORAGE_BANK_A_PACKAGE_BASE (STORAGE_BANK_A_BASE + STORAGE_FLASH_SECTOR_SIZE)
#define STORAGE_BANK_B_PACKAGE_BASE (STORAGE_BANK_B_BASE + STORAGE_FLASH_SECTOR_SIZE)
#define STORAGE_BANK_HEADER_SIZE 64u
#define STORAGE_BANK_FORMAT_VERSION 1u
#define STORAGE_BANK_STATE_PREPARED 1u
#define STORAGE_BANK_COMMIT_MARKER_OFFSET 60u
#define STORAGE_BANK_COMMIT_MARKER 0x54494D43u
#define STORAGE_SERVICE_MAX_CHUNK_SIZE 238u
#define STORAGE_SERVICE_TIMEOUT_MS 60000u
#define STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS 5000u
#define STORAGE_SERVICE_FLASH_READ_RETRY_LIMIT 3u

typedef enum {
    STORAGE_BANK_NONE = 0,
    STORAGE_BANK_A = 1,
    STORAGE_BANK_B = 2,
} storage_bank_t;

typedef enum {
    STORAGE_SERVICE_BOOT_SCAN = 0,
    STORAGE_SERVICE_BOOT_VERIFY,
    STORAGE_SERVICE_IDLE,
    STORAGE_SERVICE_ERASING,
    STORAGE_SERVICE_READY,
    STORAGE_SERVICE_RECEIVING,
    STORAGE_SERVICE_VERIFYING,
    STORAGE_SERVICE_COMMITTING,
    STORAGE_SERVICE_ABORTED,
    STORAGE_SERVICE_FAILED,
} storage_service_state_t;

typedef enum {
    STORAGE_SERVICE_ERROR_NONE = 0,
    STORAGE_SERVICE_ERROR_FLASH_UNAVAILABLE,
    STORAGE_SERVICE_ERROR_FLASH_PROTECTED,
    STORAGE_SERVICE_ERROR_FLASH_RANGE,
    STORAGE_SERVICE_ERROR_FLASH_IO,
    STORAGE_SERVICE_ERROR_FLASH_TIMEOUT,
    STORAGE_SERVICE_ERROR_PACKAGE_CRC,
    STORAGE_SERVICE_ERROR_PACKAGE_FORMAT,
    STORAGE_SERVICE_ERROR_TIMEOUT,
    STORAGE_SERVICE_ERROR_ABORTED,
    STORAGE_SERVICE_ERROR_LINK_LOST,
    STORAGE_SERVICE_ERROR_JOURNAL,
} storage_service_error_t;

typedef enum {
    STORAGE_SERVICE_RESULT_OK = 0,
    STORAGE_SERVICE_RESULT_BUSY,
    STORAGE_SERVICE_RESULT_BAD_STATE,
    STORAGE_SERVICE_RESULT_BAD_SESSION,
    STORAGE_SERVICE_RESULT_BAD_ARGUMENT,
    STORAGE_SERVICE_RESULT_BAD_LENGTH,
    STORAGE_SERVICE_RESULT_BAD_OFFSET,
    STORAGE_SERVICE_RESULT_BAD_CRC,
    STORAGE_SERVICE_RESULT_CONFLICT,
} storage_service_result_t;

typedef struct {
    storage_service_state_t state;
    storage_bank_t active_bank;
    uint32_t generation;
    uint32_t update_id;
    uint32_t next_offset;
    uint32_t total_size;
    storage_service_error_t error;
    bool journal_warning;
    bool degraded;
} storage_service_status_t;

typedef struct {
    bool header_valid;
    bool tried;
    storage_bank_t bank;
    uint32_t generation;
    uint32_t package_length;
    uint32_t package_crc32;
    uint16_t resource_format_version;
} storage_bank_info_t;

typedef enum {
    STORAGE_PHASE_BOOT_JOURNAL = 0,
    STORAGE_PHASE_BOOT_BANK_A,
    STORAGE_PHASE_BOOT_BANK_B,
    STORAGE_PHASE_BOOT_SELECT,
    STORAGE_PHASE_ERASE_BANK,
    STORAGE_PHASE_WRITE_BANK_HEADER,
    STORAGE_PHASE_WAIT_FOR_CHUNK,
    STORAGE_PHASE_WRITE_CHUNK,
    STORAGE_PHASE_VERIFY_BANK_HEADER,
    STORAGE_PHASE_VERIFY_CRC,
    STORAGE_PHASE_VERIFY_OPEN,
    STORAGE_PHASE_VERIFY_CLIPS,
    STORAGE_PHASE_VERIFY_FRAMES,
    STORAGE_PHASE_VERIFY_FRAME_DATA,
    STORAGE_PHASE_WRITE_COMMIT_MARKER,
    STORAGE_PHASE_VERIFY_COMMIT_MARKER,
    STORAGE_PHASE_ERASE_JOURNAL,
    STORAGE_PHASE_WRITE_JOURNAL,
    STORAGE_PHASE_TERMINAL,
} storage_service_phase_t;

typedef enum {
    STORAGE_PENDING_NONE = 0,
    STORAGE_PENDING_ERASE_BANK,
    STORAGE_PENDING_BANK_HEADER,
    STORAGE_PENDING_CHUNK_PAGE,
    STORAGE_PENDING_COMMIT_MARKER,
    STORAGE_PENDING_JOURNAL_ERASE,
    STORAGE_PENDING_JOURNAL_RECORD,
} storage_pending_operation_t;

typedef struct {
    storage_flash_t flash;
    storage_service_state_t state;
    storage_service_phase_t phase;
    storage_pending_operation_t pending_operation;
    uint16_t pending_program_length;
    uint32_t pending_started_ms;
    uint32_t current_time_ms;
    storage_service_error_t error;
    bool journal_warning;
    bool degraded;
    bool update_committed;
    bool commit_marker_started;
    uint8_t flash_retry_count;
    bool flash_busy_retry_active;
    uint32_t flash_retry_started_ms;

    storage_bank_info_t banks[2];
    storage_bank_info_t active;
    storage_bank_info_t verifying;
    storage_bank_t journal_bank;
    uint32_t journal_generation;
    uint32_t journal_next_offset;
    uint32_t scan_offset;

    uint32_t update_id;
    uint32_t update_size;
    uint32_t update_crc32;
    uint32_t last_update_id;
    uint32_t last_next_offset;
    uint32_t last_total_size;
    uint16_t update_format_version;
    storage_bank_t target_bank;
    uint32_t target_generation;
    uint32_t erase_address;
    uint32_t erase_end;
    uint32_t next_offset;
    uint32_t last_activity_ms;

    uint8_t chunk_data[STORAGE_SERVICE_MAX_CHUNK_SIZE];
    uint16_t chunk_length;
    uint16_t chunk_progress;
    uint32_t chunk_offset;
    uint32_t chunk_crc32;
    bool last_chunk_valid;
    uint16_t last_chunk_length;
    uint32_t last_chunk_offset;
    uint32_t last_chunk_crc32;

    uint32_t verify_offset;
    uint32_t verify_crc_state;
    uint16_t verify_index;
    uint32_t verify_expected_frame;
    uint32_t verify_next_data_offset;
    resource_frame_t verify_frame;
    uint32_t verify_frame_encoded_consumed;
    uint32_t verify_frame_decoded_count;
    uint32_t verify_frame_crc_state;
    uint16_t verify_rle_literal_remaining;
    uint8_t verify_rle_repeat_length;
    bool verify_rle_needs_repeat_value;
    bool verify_for_boot;
    resource_package_t parsed_package;
    storage_flash_result_t callback_result;

    uint8_t io_buffer[STORAGE_FLASH_PAGE_SIZE];
} storage_service_t;

void storage_service_init(storage_service_t *service,
                          const storage_flash_t *flash);
void storage_service_tick(storage_service_t *service, uint32_t now_ms);
storage_service_result_t storage_service_begin(
    storage_service_t *service, uint32_t update_id, uint32_t package_size,
    uint32_t package_crc32, uint16_t format_version, uint32_t now_ms);
storage_service_result_t storage_service_write_chunk(
    storage_service_t *service, uint32_t update_id, uint32_t offset,
    const uint8_t *data, uint16_t length, uint32_t chunk_crc32,
    uint32_t now_ms);
storage_service_result_t storage_service_finish(storage_service_t *service,
                                                uint32_t update_id,
                                                uint32_t now_ms);
storage_service_result_t storage_service_abort(storage_service_t *service,
                                               uint32_t update_id,
                                               uint32_t now_ms);
void storage_service_link_lost(storage_service_t *service, uint32_t now_ms);
bool storage_service_touch(storage_service_t *service, uint32_t update_id,
                           uint32_t now_ms);
void storage_service_get_status(const storage_service_t *service,
                                storage_service_status_t *status);
bool storage_service_get_active_package(const storage_service_t *service,
                                        uint32_t *address, uint32_t *length,
                                        uint32_t *package_crc32);
uint32_t storage_service_bank_base(storage_bank_t bank);
bool storage_service_generation_is_newer(uint32_t candidate,
                                         uint32_t current);

#endif
