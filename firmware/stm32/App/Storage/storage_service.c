#include "storage_service.h"

#include <stddef.h>
#include <string.h>

#include "resource_crc32.h"

#define STORAGE_JOURNAL_FORMAT_VERSION 1u
#define STORAGE_BANK_HEADER_CRC_OFFSET 24u
#define STORAGE_JOURNAL_CRC_OFFSET 24u
#define STORAGE_CRC_FIELD_SIZE 4u
#define STORAGE_UNUSED_OFFSET UINT32_MAX

static uint16_t read_u16_le(const uint8_t *source) {
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8u));
}

static uint32_t read_u32_le(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

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

static bool bytes_equal(const uint8_t *data, uint8_t value, uint32_t length) {
    uint32_t index;

    for (index = 0u; index < length; ++index) {
        if (data[index] != value) {
            return false;
        }
    }
    return true;
}

static bool range_fits(uint32_t offset, uint32_t length, uint32_t limit) {
    return offset <= limit && length <= limit - offset;
}

uint32_t storage_service_bank_base(storage_bank_t bank) {
    if (bank == STORAGE_BANK_A) {
        return STORAGE_BANK_A_BASE;
    }
    if (bank == STORAGE_BANK_B) {
        return STORAGE_BANK_B_BASE;
    }
    return 0u;
}

static uint32_t bank_end(storage_bank_t bank) {
    return bank == STORAGE_BANK_A ? STORAGE_BANK_A_END : STORAGE_BANK_B_END;
}

static uint32_t bank_package_base(storage_bank_t bank) {
    return storage_service_bank_base(bank) + STORAGE_FLASH_SECTOR_SIZE;
}

bool storage_service_generation_is_newer(uint32_t candidate,
                                         uint32_t current) {
    return candidate != current && (int32_t)(candidate - current) > 0;
}

static uint32_t crc_with_zero_field(uint8_t *data, uint32_t length,
                                    uint32_t crc_offset) {
    uint32_t stored = read_u32_le(&data[crc_offset]);
    uint32_t crc;

    write_u32_le(&data[crc_offset], 0u);
    crc = resource_crc32(data, length);
    write_u32_le(&data[crc_offset], stored);
    return crc;
}

static bool flash_interface_valid(const storage_flash_t *flash) {
    return flash != NULL && flash->ops != NULL && flash->context != NULL &&
           flash->capacity_bytes >= STORAGE_FLASH_CAPACITY_REQUIRED &&
           flash->ops->read != NULL && flash->ops->start_sector_erase != NULL &&
           flash->ops->start_page_program != NULL &&
           flash->ops->poll_busy != NULL;
}

static storage_service_error_t map_flash_error(storage_flash_result_t result) {
    switch (result) {
        case STORAGE_FLASH_PROTECTED:
            return STORAGE_SERVICE_ERROR_FLASH_PROTECTED;
        case STORAGE_FLASH_RANGE:
            return STORAGE_SERVICE_ERROR_FLASH_RANGE;
        case STORAGE_FLASH_UNAVAILABLE:
            return STORAGE_SERVICE_ERROR_FLASH_UNAVAILABLE;
        case STORAGE_FLASH_IO:
        case STORAGE_FLASH_BUSY:
        case STORAGE_FLASH_OK:
        default:
            return STORAGE_SERVICE_ERROR_FLASH_IO;
    }
}

static storage_service_error_t map_flash_read_error(
    storage_flash_result_t result) {
    return result == STORAGE_FLASH_BUSY
               ? STORAGE_SERVICE_ERROR_FLASH_TIMEOUT
               : map_flash_error(result);
}

static void reset_flash_read_retry(storage_service_t *service) {
    service->flash_retry_count = 0u;
    service->flash_busy_retry_active = false;
    service->flash_retry_started_ms = 0u;
}

static bool update_state_active(const storage_service_t *service) {
    return service->state == STORAGE_SERVICE_ERASING ||
           service->state == STORAGE_SERVICE_READY ||
           service->state == STORAGE_SERVICE_RECEIVING ||
           service->state == STORAGE_SERVICE_VERIFYING ||
           (service->state == STORAGE_SERVICE_COMMITTING &&
            !service->update_committed &&
            !service->commit_marker_started);
}

static bool update_session_visible(const storage_service_t *service) {
    return service->update_id != 0u &&
           (update_state_active(service) ||
            service->state == STORAGE_SERVICE_COMMITTING ||
            service->state == STORAGE_SERVICE_ABORTED ||
            service->state == STORAGE_SERVICE_FAILED);
}

static void clear_update(storage_service_t *service) {
    if (service->update_id != 0u) {
        service->last_update_id = service->update_id;
        service->last_next_offset = service->next_offset;
        service->last_total_size = service->update_size;
    }
    service->update_id = 0u;
    service->update_size = 0u;
    service->update_crc32 = 0u;
    service->update_format_version = 0u;
    service->target_bank = STORAGE_BANK_NONE;
    service->target_generation = 0u;
    service->erase_address = 0u;
    service->erase_end = 0u;
    service->next_offset = 0u;
    service->chunk_length = 0u;
    service->chunk_progress = 0u;
    service->last_chunk_valid = false;
    service->update_committed = false;
    service->commit_marker_started = false;
    reset_flash_read_retry(service);
}

static void finish_update(storage_service_t *service) {
    service->pending_operation = STORAGE_PENDING_NONE;
    service->pending_program_length = 0u;
    clear_update(service);
    service->state = STORAGE_SERVICE_IDLE;
    service->phase = STORAGE_PHASE_WAIT_FOR_CHUNK;
}

static void fail_service(storage_service_t *service,
                         storage_service_error_t error) {
    if (service->update_committed) {
        service->error = STORAGE_SERVICE_ERROR_JOURNAL;
        service->journal_warning = true;
        finish_update(service);
        return;
    }
    service->error = error;
    service->state = STORAGE_SERVICE_FAILED;
    service->phase = STORAGE_PHASE_TERMINAL;
}

static bool handle_flash_result(storage_service_t *service,
                                storage_flash_result_t result) {
    if (result == STORAGE_FLASH_OK) {
        return true;
    }
    if (result == STORAGE_FLASH_BUSY) {
        return false;
    }
    fail_service(service, map_flash_error(result));
    return false;
}

static void build_bank_header(storage_service_t *service) {
    uint32_t header_crc;

    memset(service->io_buffer, 0, STORAGE_BANK_COMMIT_MARKER_OFFSET);
    memcpy(&service->io_buffer[0], "ARBK", 4u);
    write_u16_le(&service->io_buffer[4], STORAGE_BANK_FORMAT_VERSION);
    write_u16_le(&service->io_buffer[6], STORAGE_BANK_HEADER_SIZE);
    service->io_buffer[8] = (uint8_t)service->target_bank;
    service->io_buffer[9] = STORAGE_BANK_STATE_PREPARED;
    write_u16_le(&service->io_buffer[10], service->update_format_version);
    write_u32_le(&service->io_buffer[12], service->target_generation);
    write_u32_le(&service->io_buffer[16], service->update_size);
    write_u32_le(&service->io_buffer[20], service->update_crc32);
    write_u32_le(&service->io_buffer[STORAGE_BANK_HEADER_CRC_OFFSET], 0u);
    header_crc = resource_crc32(service->io_buffer,
                                STORAGE_BANK_COMMIT_MARKER_OFFSET);
    write_u32_le(&service->io_buffer[STORAGE_BANK_HEADER_CRC_OFFSET],
                 header_crc);
}

static bool parse_bank_header(uint8_t *header, storage_bank_t expected_bank,
                              bool require_commit,
                              storage_bank_info_t *info, bool *present) {
    uint32_t package_base = bank_package_base(expected_bank);
    uint32_t stored_header_crc;

    memset(info, 0, sizeof(*info));
    info->bank = expected_bank;
    *present = !bytes_equal(header, 0xFFu, STORAGE_BANK_HEADER_SIZE);
    if (!*present || memcmp(header, "ARBK", 4u) != 0 ||
        read_u16_le(&header[4]) != STORAGE_BANK_FORMAT_VERSION ||
        read_u16_le(&header[6]) != STORAGE_BANK_HEADER_SIZE ||
        header[8] != (uint8_t)expected_bank ||
        header[9] != STORAGE_BANK_STATE_PREPARED ||
        read_u16_le(&header[10]) != RESOURCE_FORMAT_VERSION ||
        !bytes_equal(&header[28], 0u,
                     STORAGE_BANK_COMMIT_MARKER_OFFSET - 28u) ||
        (require_commit &&
         read_u32_le(&header[STORAGE_BANK_COMMIT_MARKER_OFFSET]) !=
             STORAGE_BANK_COMMIT_MARKER) ||
        (!require_commit &&
         !bytes_equal(&header[STORAGE_BANK_COMMIT_MARKER_OFFSET], 0xFFu,
                      STORAGE_CRC_FIELD_SIZE))) {
        return false;
    }
    stored_header_crc = read_u32_le(&header[STORAGE_BANK_HEADER_CRC_OFFSET]);
    if (crc_with_zero_field(header, STORAGE_BANK_COMMIT_MARKER_OFFSET,
                            STORAGE_BANK_HEADER_CRC_OFFSET) !=
        stored_header_crc) {
        return false;
    }
    info->resource_format_version = read_u16_le(&header[10]);
    info->generation = read_u32_le(&header[12]);
    info->package_length = read_u32_le(&header[16]);
    info->package_crc32 = read_u32_le(&header[20]);
    if (info->package_length < RESOURCE_PACKAGE_HEADER_SIZE ||
        info->package_length > RESOURCE_MAX_PACKAGE_SIZE ||
        !range_fits(package_base, info->package_length,
                    bank_end(expected_bank))) {
        return false;
    }
    info->header_valid = true;
    return true;
}

static bool parse_journal_record(uint8_t *record, storage_bank_t *bank,
                                 uint32_t *generation) {
    uint32_t stored_crc;

    if (memcmp(record, "ARJR", 4u) != 0 ||
        read_u16_le(&record[4]) != STORAGE_JOURNAL_FORMAT_VERSION ||
        read_u16_le(&record[6]) != STORAGE_JOURNAL_RECORD_SIZE ||
        (record[8] != STORAGE_BANK_A && record[8] != STORAGE_BANK_B) ||
        !bytes_equal(&record[9], 0u, 3u) ||
        !bytes_equal(&record[28], 0u, 4u)) {
        return false;
    }
    stored_crc = read_u32_le(&record[STORAGE_JOURNAL_CRC_OFFSET]);
    if (crc_with_zero_field(record, STORAGE_JOURNAL_RECORD_SIZE,
                            STORAGE_JOURNAL_CRC_OFFSET) != stored_crc) {
        return false;
    }
    *bank = (storage_bank_t)record[8];
    *generation = read_u32_le(&record[12]);
    return true;
}

static void build_journal_record(storage_service_t *service) {
    uint32_t record_crc;

    memset(service->io_buffer, 0, STORAGE_JOURNAL_RECORD_SIZE);
    memcpy(&service->io_buffer[0], "ARJR", 4u);
    write_u16_le(&service->io_buffer[4], STORAGE_JOURNAL_FORMAT_VERSION);
    write_u16_le(&service->io_buffer[6], STORAGE_JOURNAL_RECORD_SIZE);
    service->io_buffer[8] = (uint8_t)service->active.bank;
    write_u32_le(&service->io_buffer[12], service->active.generation);
    write_u32_le(&service->io_buffer[16], service->active.package_crc32);
    write_u32_le(&service->io_buffer[20], service->active.package_length);
    record_crc = resource_crc32(service->io_buffer, STORAGE_JOURNAL_RECORD_SIZE);
    write_u32_le(&service->io_buffer[STORAGE_JOURNAL_CRC_OFFSET], record_crc);
}

static storage_bank_info_t *bank_slot(storage_service_t *service,
                                      storage_bank_t bank) {
    return &service->banks[bank == STORAGE_BANK_A ? 0u : 1u];
}

static storage_bank_info_t *select_boot_candidate(storage_service_t *service) {
    storage_bank_info_t *bank_a = &service->banks[0];
    storage_bank_info_t *bank_b = &service->banks[1];

    if (!bank_a->header_valid || bank_a->tried) {
        return bank_b->header_valid && !bank_b->tried ? bank_b : NULL;
    }
    if (!bank_b->header_valid || bank_b->tried) {
        return bank_a;
    }
    if (bank_a->generation == bank_b->generation &&
        service->journal_generation == bank_a->generation) {
        if (service->journal_bank == STORAGE_BANK_B) {
            return bank_b;
        }
        if (service->journal_bank == STORAGE_BANK_A) {
            return bank_a;
        }
    }
    return storage_service_generation_is_newer(bank_b->generation,
                                               bank_a->generation)
               ? bank_b
               : bank_a;
}

static void prepare_verification(storage_service_t *service,
                                 const storage_bank_info_t *bank,
                                 bool for_boot) {
    service->verifying = *bank;
    service->verify_for_boot = for_boot;
    service->verify_offset = 0u;
    service->verify_crc_state = resource_crc32_init();
    service->verify_index = 0u;
    service->verify_expected_frame = 0u;
    service->verify_next_data_offset = 0u;
    memset(&service->verify_frame, 0, sizeof(service->verify_frame));
    service->verify_frame_encoded_consumed = 0u;
    service->verify_frame_decoded_count = 0u;
    service->verify_frame_crc_state = resource_crc32_init();
    service->verify_rle_literal_remaining = 0u;
    service->verify_rle_repeat_length = 0u;
    service->verify_rle_needs_repeat_value = false;
    reset_flash_read_retry(service);
    service->phase = for_boot ? STORAGE_PHASE_VERIFY_CRC
                              : STORAGE_PHASE_VERIFY_BANK_HEADER;
    service->state = for_boot ? STORAGE_SERVICE_BOOT_VERIFY
                              : STORAGE_SERVICE_VERIFYING;
}

static void boot_candidate_failed(storage_service_t *service,
                                  storage_service_error_t error) {
    storage_bank_info_t *slot = bank_slot(service, service->verifying.bank);
    reset_flash_read_retry(service);
    slot->header_valid = false;
    service->degraded = true;
    service->error = error;
    service->phase = STORAGE_PHASE_BOOT_SELECT;
    service->state = STORAGE_SERVICE_BOOT_VERIFY;
}

static void verification_failed(storage_service_t *service,
                                storage_service_error_t error) {
    if (service->verify_for_boot) {
        boot_candidate_failed(service, error);
    } else {
        fail_service(service, error);
    }
}

static bool flash_read_should_retry(storage_service_t *service,
                                    storage_flash_result_t result) {
    if (result == STORAGE_FLASH_BUSY) {
        if (!service->flash_busy_retry_active) {
            service->flash_busy_retry_active = true;
            service->flash_retry_started_ms = service->current_time_ms;
            return true;
        }
        if ((uint32_t)(service->current_time_ms -
                       service->flash_retry_started_ms) <
            STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS) {
            return true;
        }
        reset_flash_read_retry(service);
        return false;
    }
    service->flash_busy_retry_active = false;
    service->flash_retry_started_ms = 0u;
    if (result == STORAGE_FLASH_IO &&
        service->flash_retry_count <
            STORAGE_SERVICE_FLASH_READ_RETRY_LIMIT) {
        service->flash_retry_count++;
        return true;
    }
    reset_flash_read_retry(service);
    return false;
}

static bool handle_verification_read_result(
    storage_service_t *service, storage_flash_result_t result) {
    if (result == STORAGE_FLASH_OK) {
        reset_flash_read_retry(service);
        return true;
    }
    if (service->verify_for_boot) {
        if (flash_read_should_retry(service, result)) {
            return false;
        }
        verification_failed(service, map_flash_read_error(result));
        return false;
    }
    return handle_flash_result(service, result);
}

static bool format_read_callback(void *context, uint32_t offset,
                                 uint8_t *destination, uint32_t length) {
    storage_service_t *service = (storage_service_t *)context;
    uint32_t package_address = bank_package_base(service->verifying.bank);

    if (length == 0u || length > UINT16_MAX ||
        !range_fits(offset, length, service->verifying.package_length)) {
        service->callback_result = STORAGE_FLASH_RANGE;
        return false;
    }
    service->callback_result = service->flash.ops->read(
        service->flash.context, package_address + offset, destination,
        (uint16_t)length);
    return service->callback_result == STORAGE_FLASH_OK;
}

static void tick_boot_journal(storage_service_t *service) {
    uint16_t read_length;
    storage_flash_result_t result;
    uint32_t local_offset;

    if (service->scan_offset >= STORAGE_JOURNAL_SIZE) {
        service->phase = STORAGE_PHASE_BOOT_BANK_A;
        return;
    }
    read_length = (uint16_t)(STORAGE_JOURNAL_SIZE - service->scan_offset);
    if (read_length > sizeof(service->io_buffer)) {
        read_length = sizeof(service->io_buffer);
    }
    result = service->flash.ops->read(
        service->flash.context, STORAGE_JOURNAL_BASE + service->scan_offset,
        service->io_buffer, read_length);
    if (result != STORAGE_FLASH_OK) {
        if (flash_read_should_retry(service, result)) {
            return;
        }
        service->journal_bank = STORAGE_BANK_NONE;
        service->journal_generation = 0u;
        service->journal_next_offset = STORAGE_UNUSED_OFFSET;
        service->scan_offset = STORAGE_JOURNAL_SIZE;
        service->degraded = true;
        service->error = map_flash_read_error(result);
        service->phase = STORAGE_PHASE_BOOT_BANK_A;
        return;
    }
    reset_flash_read_retry(service);
    for (local_offset = 0u; local_offset < read_length;
         local_offset += STORAGE_JOURNAL_RECORD_SIZE) {
        uint8_t *record = &service->io_buffer[local_offset];
        uint32_t record_offset = service->scan_offset + local_offset;
        storage_bank_t bank;
        uint32_t generation;

        if (bytes_equal(record, 0xFFu, STORAGE_JOURNAL_RECORD_SIZE)) {
            if (service->journal_next_offset == STORAGE_UNUSED_OFFSET) {
                service->journal_next_offset = record_offset;
            }
        } else if (parse_journal_record(record, &bank, &generation) &&
                   (service->journal_bank == STORAGE_BANK_NONE ||
                    storage_service_generation_is_newer(
                        generation, service->journal_generation))) {
            service->journal_bank = bank;
            service->journal_generation = generation;
        }
    }
    service->scan_offset += read_length;
}

static void tick_boot_bank_header(storage_service_t *service,
                                  storage_bank_t bank) {
    storage_flash_result_t result = service->flash.ops->read(
        service->flash.context, storage_service_bank_base(bank),
        service->io_buffer, STORAGE_BANK_HEADER_SIZE);
    bool present = false;
    storage_bank_info_t *slot;

    slot = bank_slot(service, bank);
    if (result != STORAGE_FLASH_OK) {
        if (flash_read_should_retry(service, result)) {
            return;
        }
        memset(slot, 0, sizeof(*slot));
        slot->bank = bank;
        service->degraded = true;
        service->error = map_flash_read_error(result);
        service->phase = bank == STORAGE_BANK_A
                             ? STORAGE_PHASE_BOOT_BANK_B
                             : STORAGE_PHASE_BOOT_SELECT;
        return;
    }
    reset_flash_read_retry(service);
    if (!parse_bank_header(service->io_buffer, bank, true, slot, &present) &&
        present) {
        service->degraded = true;
    }
    service->phase = bank == STORAGE_BANK_A ? STORAGE_PHASE_BOOT_BANK_B
                                            : STORAGE_PHASE_BOOT_SELECT;
}

static void tick_verify_bank_header(storage_service_t *service) {
    storage_bank_info_t readback;
    storage_flash_result_t result = service->flash.ops->read(
        service->flash.context,
        storage_service_bank_base(service->verifying.bank),
        service->io_buffer, STORAGE_BANK_HEADER_SIZE);
    bool present = false;

    if (!handle_flash_result(service, result)) {
        return;
    }
    if (!parse_bank_header(service->io_buffer, service->verifying.bank, false,
                           &readback, &present) || !present ||
        readback.generation != service->verifying.generation ||
        readback.package_length != service->verifying.package_length ||
        readback.package_crc32 != service->verifying.package_crc32 ||
        readback.resource_format_version !=
            service->verifying.resource_format_version) {
        verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
        return;
    }
    service->phase = STORAGE_PHASE_VERIFY_CRC;
}

static void tick_boot_select(storage_service_t *service) {
    storage_bank_info_t *candidate = select_boot_candidate(service);

    if (candidate == NULL) {
        memset(&service->active, 0, sizeof(service->active));
        service->active.bank = STORAGE_BANK_NONE;
        service->state = STORAGE_SERVICE_IDLE;
        service->phase = STORAGE_PHASE_WAIT_FOR_CHUNK;
        return;
    }
    candidate->tried = true;
    prepare_verification(service, candidate, true);
}

static void tick_verify_crc(storage_service_t *service) {
    uint32_t remaining;
    uint16_t read_length;
    uint32_t index;
    uint32_t package_address = bank_package_base(service->verifying.bank);
    storage_flash_result_t result;

    if (service->verify_offset >= service->verifying.package_length) {
        uint32_t calculated = resource_crc32_finalize(service->verify_crc_state);
        if (calculated != service->verifying.package_crc32) {
            verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_CRC);
            return;
        }
        service->phase = STORAGE_PHASE_VERIFY_OPEN;
        return;
    }
    remaining = service->verifying.package_length - service->verify_offset;
    read_length = remaining > sizeof(service->io_buffer)
                      ? sizeof(service->io_buffer)
                      : (uint16_t)remaining;
    result = service->flash.ops->read(
        service->flash.context, package_address + service->verify_offset,
        service->io_buffer, read_length);
    if (!handle_verification_read_result(service, result)) {
        return;
    }
    for (index = 0u; index < read_length; ++index) {
        uint32_t package_offset = service->verify_offset + index;
        if (package_offset >= RESOURCE_PACKAGE_CRC_OFFSET &&
            package_offset <
                RESOURCE_PACKAGE_CRC_OFFSET + STORAGE_CRC_FIELD_SIZE) {
            service->io_buffer[index] = 0u;
        }
    }
    service->verify_crc_state = resource_crc32_update(
        service->verify_crc_state, service->io_buffer, read_length);
    service->verify_offset += read_length;
}

static bool parser_read_retry(storage_service_t *service,
                              resource_format_status_t status) {
    if (status == RESOURCE_FORMAT_OK) {
        reset_flash_read_retry(service);
        return false;
    }
    if (status == RESOURCE_FORMAT_READ_FAILED &&
        service->callback_result == STORAGE_FLASH_BUSY) {
        if (service->verify_for_boot &&
            !flash_read_should_retry(service, service->callback_result)) {
            verification_failed(service,
                                STORAGE_SERVICE_ERROR_FLASH_TIMEOUT);
        }
        return true;
    }
    if (status == RESOURCE_FORMAT_READ_FAILED &&
        service->callback_result != STORAGE_FLASH_OK) {
        if (service->verify_for_boot && flash_read_should_retry(
                                            service,
                                            service->callback_result)) {
            return true;
        }
        verification_failed(
            service, map_flash_read_error(service->callback_result));
        return true;
    }
    verification_failed(
        service, status == RESOURCE_FORMAT_BAD_CRC
                     ? STORAGE_SERVICE_ERROR_PACKAGE_CRC
                     : STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
    return true;
}

static void tick_verify_open(storage_service_t *service) {
    resource_format_status_t status;

    service->callback_result = STORAGE_FLASH_OK;
    status = resource_package_open(&service->parsed_package, format_read_callback,
                                   service,
                                   service->verifying.package_length);
    if (parser_read_retry(service, status)) {
        return;
    }
    if (service->parsed_package.header.total_length !=
            service->verifying.package_length ||
        service->parsed_package.header.package_crc32 !=
            service->verifying.package_crc32 ||
        service->parsed_package.header.version !=
            service->verifying.resource_format_version) {
        verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
        return;
    }
    service->verify_index = 0u;
    service->verify_expected_frame = 0u;
    service->phase = STORAGE_PHASE_VERIFY_CLIPS;
}

static void tick_verify_clips(storage_service_t *service) {
    resource_clip_t clip;
    resource_format_status_t status;

    if (service->verify_index >= service->parsed_package.header.clip_count) {
        if (service->verify_expected_frame !=
            service->parsed_package.header.frame_count) {
            verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
            return;
        }
        service->verify_index = 0u;
        service->verify_next_data_offset =
            service->parsed_package.header.data_offset;
        service->phase = STORAGE_PHASE_VERIFY_FRAMES;
        return;
    }
    service->callback_result = STORAGE_FLASH_OK;
    status = resource_package_read_clip(&service->parsed_package,
                                        service->verify_index, &clip);
    if (parser_read_retry(service, status)) {
        return;
    }
    if (clip.first_frame_index != service->verify_expected_frame) {
        verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
        return;
    }
    service->verify_expected_frame += clip.frame_count;
    service->verify_index++;
}

static void verification_succeeded(storage_service_t *service) {
    if (service->verify_for_boot) {
        service->active = service->verifying;
        service->state = STORAGE_SERVICE_IDLE;
        service->phase = STORAGE_PHASE_WAIT_FOR_CHUNK;
        return;
    }
    service->state = STORAGE_SERVICE_COMMITTING;
    service->phase = STORAGE_PHASE_WRITE_COMMIT_MARKER;
}

static void tick_verify_frames(storage_service_t *service) {
    resource_frame_t frame;
    resource_format_status_t status;

    if (service->verify_index >= service->parsed_package.header.frame_count) {
        if (service->verify_next_data_offset !=
            service->parsed_package.header.total_length) {
            verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
            return;
        }
        verification_succeeded(service);
        return;
    }
    service->callback_result = STORAGE_FLASH_OK;
    status = resource_package_read_frame(&service->parsed_package,
                                         service->verify_index, &frame);
    if (parser_read_retry(service, status)) {
        return;
    }
    if (frame.data_offset != service->verify_next_data_offset) {
        verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
        return;
    }
    service->verify_frame = frame;
    service->verify_frame_encoded_consumed = 0u;
    service->verify_frame_decoded_count = 0u;
    service->verify_frame_crc_state = resource_crc32_init();
    service->verify_rle_literal_remaining = 0u;
    service->verify_rle_repeat_length = 0u;
    service->verify_rle_needs_repeat_value = false;
    service->phase = STORAGE_PHASE_VERIFY_FRAME_DATA;
}

static void finish_frame_verification(storage_service_t *service) {
    if (service->verify_frame_encoded_consumed !=
            service->verify_frame.encoded_length ||
        service->verify_frame_decoded_count !=
            RESOURCE_DECODED_FRAME_SIZE ||
        service->verify_rle_literal_remaining != 0u ||
        service->verify_rle_needs_repeat_value) {
        verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
        return;
    }
    if (resource_crc32_finalize(service->verify_frame_crc_state) !=
        service->verify_frame.decoded_crc32) {
        verification_failed(service, STORAGE_SERVICE_ERROR_PACKAGE_CRC);
        return;
    }
    service->verify_next_data_offset =
        service->verify_frame.data_offset +
        service->verify_frame.encoded_length;
    service->verify_index++;
    service->phase = STORAGE_PHASE_VERIFY_FRAMES;
}

static bool verify_decoded_run_fits(const storage_service_t *service,
                                    uint32_t run_length) {
    return service->verify_frame_decoded_count <=
               RESOURCE_DECODED_FRAME_SIZE &&
           run_length <= RESOURCE_DECODED_FRAME_SIZE -
                             service->verify_frame_decoded_count;
}

static void tick_verify_rle_data(storage_service_t *service,
                                 uint16_t read_length) {
    uint16_t input_offset = 0u;

    while (input_offset < read_length) {
        if (service->verify_rle_literal_remaining != 0u) {
            uint16_t available = (uint16_t)(read_length - input_offset);
            uint16_t length = service->verify_rle_literal_remaining < available
                                  ? service->verify_rle_literal_remaining
                                  : available;

            service->verify_frame_crc_state = resource_crc32_update(
                service->verify_frame_crc_state,
                &service->io_buffer[input_offset], length);
            service->verify_frame_decoded_count += length;
            service->verify_rle_literal_remaining =
                (uint16_t)(service->verify_rle_literal_remaining - length);
            input_offset = (uint16_t)(input_offset + length);
            continue;
        }
        if (service->verify_rle_needs_repeat_value) {
            uint8_t value = service->io_buffer[input_offset++];
            uint8_t remaining = service->verify_rle_repeat_length;

            while (remaining != 0u) {
                service->verify_frame_crc_state = resource_crc32_update(
                    service->verify_frame_crc_state, &value, 1u);
                remaining--;
            }
            service->verify_frame_decoded_count +=
                service->verify_rle_repeat_length;
            service->verify_rle_repeat_length = 0u;
            service->verify_rle_needs_repeat_value = false;
            continue;
        }
        {
            uint8_t control = service->io_buffer[input_offset++];
            uint32_t run_length = (uint32_t)(control & 0x7Fu) + 1u;

            if (!verify_decoded_run_fits(service, run_length)) {
                verification_failed(service,
                                    STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
                return;
            }
            if ((control & 0x80u) != 0u) {
                service->verify_rle_repeat_length = (uint8_t)run_length;
                service->verify_rle_needs_repeat_value = true;
            } else {
                service->verify_rle_literal_remaining = (uint16_t)run_length;
            }
        }
    }
}

static void tick_verify_frame_data(storage_service_t *service) {
    uint32_t remaining;
    uint16_t read_length;
    uint32_t package_address = bank_package_base(service->verifying.bank);
    storage_flash_result_t result;

    if (service->verify_frame_encoded_consumed >=
        service->verify_frame.encoded_length) {
        finish_frame_verification(service);
        return;
    }
    remaining = service->verify_frame.encoded_length -
                service->verify_frame_encoded_consumed;
    read_length = remaining > sizeof(service->io_buffer)
                      ? sizeof(service->io_buffer)
                      : (uint16_t)remaining;
    result = service->flash.ops->read(
        service->flash.context,
        package_address + service->verify_frame.data_offset +
            service->verify_frame_encoded_consumed,
        service->io_buffer, read_length);
    if (!handle_verification_read_result(service, result)) {
        return;
    }
    if (service->verify_frame.encoding == RESOURCE_FRAME_ENCODING_RAW1) {
        if (!verify_decoded_run_fits(service, read_length)) {
            verification_failed(service,
                                STORAGE_SERVICE_ERROR_PACKAGE_FORMAT);
            return;
        }
        service->verify_frame_crc_state = resource_crc32_update(
            service->verify_frame_crc_state, service->io_buffer, read_length);
        service->verify_frame_decoded_count += read_length;
    } else {
        tick_verify_rle_data(service, read_length);
        if (service->phase != STORAGE_PHASE_VERIFY_FRAME_DATA) {
            return;
        }
    }
    service->verify_frame_encoded_consumed += read_length;
    if (service->verify_frame_encoded_consumed ==
        service->verify_frame.encoded_length) {
        finish_frame_verification(service);
    }
}

static void start_erase(storage_service_t *service, uint32_t address,
                        storage_pending_operation_t pending) {
    storage_flash_result_t result = service->flash.ops->start_sector_erase(
        service->flash.context, address);

    if (result == STORAGE_FLASH_OK) {
        service->pending_operation = pending;
        service->pending_started_ms = service->current_time_ms;
    } else {
        (void)handle_flash_result(service, result);
    }
}

static void start_program(storage_service_t *service, uint32_t address,
                          const uint8_t *data, uint16_t length,
                          storage_pending_operation_t pending) {
    storage_flash_result_t result = service->flash.ops->start_page_program(
        service->flash.context, address, data, length);

    if (result == STORAGE_FLASH_OK) {
        service->pending_operation = pending;
        service->pending_program_length = length;
        service->pending_started_ms = service->current_time_ms;
        if (pending == STORAGE_PENDING_COMMIT_MARKER) {
            service->commit_marker_started = true;
            reset_flash_read_retry(service);
        }
    } else {
        (void)handle_flash_result(service, result);
    }
}

static void tick_erase_bank(storage_service_t *service) {
    if (service->erase_address >= service->erase_end) {
        service->phase = STORAGE_PHASE_WRITE_BANK_HEADER;
        return;
    }
    start_erase(service, service->erase_address, STORAGE_PENDING_ERASE_BANK);
}

static void tick_write_bank_header(storage_service_t *service) {
    build_bank_header(service);
    start_program(service, storage_service_bank_base(service->target_bank),
                  service->io_buffer, STORAGE_BANK_COMMIT_MARKER_OFFSET,
                  STORAGE_PENDING_BANK_HEADER);
}

static void tick_write_chunk(storage_service_t *service) {
    uint32_t absolute_address = bank_package_base(service->target_bank) +
                                service->chunk_offset +
                                service->chunk_progress;
    uint16_t remaining = (uint16_t)(service->chunk_length -
                                    service->chunk_progress);
    uint16_t page_remaining =
        (uint16_t)(STORAGE_FLASH_PAGE_SIZE -
                   (absolute_address & (STORAGE_FLASH_PAGE_SIZE - 1u)));
    uint16_t length = remaining < page_remaining ? remaining : page_remaining;

    start_program(service, absolute_address,
                  &service->chunk_data[service->chunk_progress], length,
                  STORAGE_PENDING_CHUNK_PAGE);
}

static void tick_write_commit_marker(storage_service_t *service) {
    write_u32_le(service->io_buffer, STORAGE_BANK_COMMIT_MARKER);
    start_program(service,
                  storage_service_bank_base(service->target_bank) +
                      STORAGE_BANK_COMMIT_MARKER_OFFSET,
                  service->io_buffer, STORAGE_CRC_FIELD_SIZE,
                  STORAGE_PENDING_COMMIT_MARKER);
}

static void activate_committed_update(storage_service_t *service) {
    service->update_committed = true;
    service->active.header_valid = true;
    service->active.tried = true;
    service->active.bank = service->target_bank;
    service->active.generation = service->target_generation;
    service->active.package_length = service->update_size;
    service->active.package_crc32 = service->update_crc32;
    service->active.resource_format_version = service->update_format_version;
    *bank_slot(service, service->target_bank) = service->active;
    service->phase = STORAGE_PHASE_WRITE_JOURNAL;
}

static void tick_verify_commit_marker(storage_service_t *service) {
    storage_flash_result_t result = service->flash.ops->read(
        service->flash.context,
        storage_service_bank_base(service->target_bank) +
            STORAGE_BANK_COMMIT_MARKER_OFFSET,
        service->io_buffer, STORAGE_CRC_FIELD_SIZE);

    if (result == STORAGE_FLASH_BUSY) {
        if ((uint32_t)(service->current_time_ms -
                       service->pending_started_ms) >=
            STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS) {
            fail_service(service, STORAGE_SERVICE_ERROR_FLASH_TIMEOUT);
        }
        return;
    }
    if (result != STORAGE_FLASH_OK) {
        if (flash_read_should_retry(service, result)) {
            return;
        }
        fail_service(service, map_flash_error(result));
        return;
    }
    reset_flash_read_retry(service);
    if (read_u32_le(service->io_buffer) != STORAGE_BANK_COMMIT_MARKER) {
        fail_service(service, STORAGE_SERVICE_ERROR_FLASH_IO);
        return;
    }
    activate_committed_update(service);
}

static void tick_write_journal(storage_service_t *service) {
    if (service->journal_next_offset == STORAGE_UNUSED_OFFSET ||
        service->journal_next_offset + STORAGE_JOURNAL_RECORD_SIZE >
            STORAGE_JOURNAL_SIZE) {
        service->phase = STORAGE_PHASE_ERASE_JOURNAL;
        return;
    }
    build_journal_record(service);
    start_program(service, STORAGE_JOURNAL_BASE + service->journal_next_offset,
                  service->io_buffer, STORAGE_JOURNAL_RECORD_SIZE,
                  STORAGE_PENDING_JOURNAL_RECORD);
}

static void tick_erase_journal(storage_service_t *service) {
    start_erase(service, STORAGE_JOURNAL_BASE,
                STORAGE_PENDING_JOURNAL_ERASE);
}

static void pending_completed(storage_service_t *service) {
    storage_pending_operation_t completed = service->pending_operation;
    uint16_t completed_length = service->pending_program_length;

    service->pending_operation = STORAGE_PENDING_NONE;
    service->pending_program_length = 0u;
    if (completed == STORAGE_PENDING_COMMIT_MARKER &&
        service->commit_marker_started) {
        service->phase = STORAGE_PHASE_VERIFY_COMMIT_MARKER;
        service->state = STORAGE_SERVICE_COMMITTING;
        return;
    }
    if (service->state == STORAGE_SERVICE_ABORTED ||
        service->state == STORAGE_SERVICE_FAILED) {
        finish_update(service);
        return;
    }
    switch (completed) {
        case STORAGE_PENDING_ERASE_BANK:
            service->erase_address += STORAGE_FLASH_SECTOR_SIZE;
            if (service->erase_address >= service->erase_end) {
                service->phase = STORAGE_PHASE_WRITE_BANK_HEADER;
            }
            break;
        case STORAGE_PENDING_BANK_HEADER:
            service->state = STORAGE_SERVICE_READY;
            service->phase = STORAGE_PHASE_WAIT_FOR_CHUNK;
            break;
        case STORAGE_PENDING_CHUNK_PAGE:
            service->chunk_progress =
                (uint16_t)(service->chunk_progress + completed_length);
            if (service->chunk_progress >= service->chunk_length) {
                service->next_offset = service->chunk_offset +
                                       service->chunk_length;
                service->last_chunk_valid = true;
                service->last_chunk_offset = service->chunk_offset;
                service->last_chunk_length = service->chunk_length;
                service->last_chunk_crc32 = service->chunk_crc32;
                service->phase = STORAGE_PHASE_WAIT_FOR_CHUNK;
                service->state = STORAGE_SERVICE_RECEIVING;
            }
            break;
        case STORAGE_PENDING_COMMIT_MARKER:
            break;
        case STORAGE_PENDING_JOURNAL_ERASE:
            service->journal_next_offset = 0u;
            service->phase = STORAGE_PHASE_WRITE_JOURNAL;
            break;
        case STORAGE_PENDING_JOURNAL_RECORD:
            service->journal_bank = service->active.bank;
            service->journal_generation = service->active.generation;
            service->journal_next_offset += STORAGE_JOURNAL_RECORD_SIZE;
            service->error = STORAGE_SERVICE_ERROR_NONE;
            finish_update(service);
            break;
        case STORAGE_PENDING_NONE:
        default:
            break;
    }
}

static void tick_pending(storage_service_t *service) {
    bool busy = true;
    storage_flash_result_t result;

    if ((uint32_t)(service->current_time_ms - service->pending_started_ms) >=
        STORAGE_SERVICE_FLASH_BUSY_TIMEOUT_MS) {
        service->pending_operation = STORAGE_PENDING_NONE;
        service->pending_program_length = 0u;
        fail_service(service, STORAGE_SERVICE_ERROR_FLASH_TIMEOUT);
        return;
    }
    result = service->flash.ops->poll_busy(service->flash.context, &busy);

    if (result == STORAGE_FLASH_BUSY) {
        return;
    }
    if (result != STORAGE_FLASH_OK) {
        if (service->pending_operation == STORAGE_PENDING_COMMIT_MARKER &&
            flash_read_should_retry(service, result)) {
            return;
        }
        service->pending_operation = STORAGE_PENDING_NONE;
        fail_service(service, map_flash_error(result));
        return;
    }
    reset_flash_read_retry(service);
    if (!busy) {
        pending_completed(service);
    }
}

void storage_service_init(storage_service_t *service,
                          const storage_flash_t *flash) {
    if (service == NULL) {
        return;
    }
    memset(service, 0, sizeof(*service));
    service->journal_next_offset = STORAGE_UNUSED_OFFSET;
    service->active.bank = STORAGE_BANK_NONE;
    service->banks[0].bank = STORAGE_BANK_A;
    service->banks[1].bank = STORAGE_BANK_B;
    if (!flash_interface_valid(flash)) {
        service->state = STORAGE_SERVICE_IDLE;
        service->phase = STORAGE_PHASE_WAIT_FOR_CHUNK;
        service->error = STORAGE_SERVICE_ERROR_FLASH_UNAVAILABLE;
        return;
    }
    service->flash = *flash;
    service->state = STORAGE_SERVICE_BOOT_SCAN;
    service->phase = STORAGE_PHASE_BOOT_JOURNAL;
}

void storage_service_tick(storage_service_t *service, uint32_t now_ms) {
    if (service == NULL || service->flash.ops == NULL) {
        return;
    }
    service->current_time_ms = now_ms;
    if (update_state_active(service) &&
        (uint32_t)(now_ms - service->last_activity_ms) >=
            STORAGE_SERVICE_TIMEOUT_MS) {
        service->error = STORAGE_SERVICE_ERROR_TIMEOUT;
        service->state = STORAGE_SERVICE_ABORTED;
        service->phase = STORAGE_PHASE_TERMINAL;
    }
    if (service->pending_operation != STORAGE_PENDING_NONE) {
        tick_pending(service);
        return;
    }
    if (service->state == STORAGE_SERVICE_ABORTED ||
        service->state == STORAGE_SERVICE_FAILED) {
        finish_update(service);
        return;
    }
    switch (service->phase) {
        case STORAGE_PHASE_BOOT_JOURNAL:
            tick_boot_journal(service);
            break;
        case STORAGE_PHASE_BOOT_BANK_A:
            tick_boot_bank_header(service, STORAGE_BANK_A);
            break;
        case STORAGE_PHASE_BOOT_BANK_B:
            tick_boot_bank_header(service, STORAGE_BANK_B);
            break;
        case STORAGE_PHASE_BOOT_SELECT:
            tick_boot_select(service);
            break;
        case STORAGE_PHASE_ERASE_BANK:
            tick_erase_bank(service);
            break;
        case STORAGE_PHASE_WRITE_BANK_HEADER:
            tick_write_bank_header(service);
            break;
        case STORAGE_PHASE_WRITE_CHUNK:
            tick_write_chunk(service);
            break;
        case STORAGE_PHASE_VERIFY_BANK_HEADER:
            tick_verify_bank_header(service);
            break;
        case STORAGE_PHASE_VERIFY_CRC:
            tick_verify_crc(service);
            break;
        case STORAGE_PHASE_VERIFY_OPEN:
            tick_verify_open(service);
            break;
        case STORAGE_PHASE_VERIFY_CLIPS:
            tick_verify_clips(service);
            break;
        case STORAGE_PHASE_VERIFY_FRAMES:
            tick_verify_frames(service);
            break;
        case STORAGE_PHASE_VERIFY_FRAME_DATA:
            tick_verify_frame_data(service);
            break;
        case STORAGE_PHASE_WRITE_COMMIT_MARKER:
            tick_write_commit_marker(service);
            break;
        case STORAGE_PHASE_VERIFY_COMMIT_MARKER:
            tick_verify_commit_marker(service);
            break;
        case STORAGE_PHASE_ERASE_JOURNAL:
            tick_erase_journal(service);
            break;
        case STORAGE_PHASE_WRITE_JOURNAL:
            tick_write_journal(service);
            break;
        case STORAGE_PHASE_WAIT_FOR_CHUNK:
        case STORAGE_PHASE_TERMINAL:
        default:
            break;
    }
}

storage_service_result_t storage_service_begin(
    storage_service_t *service, uint32_t update_id, uint32_t package_size,
    uint32_t package_crc32, uint16_t format_version, uint32_t now_ms) {
    uint32_t rounded_size;
    uint32_t package_base;

    if (service == NULL || update_id == 0u) {
        return STORAGE_SERVICE_RESULT_BAD_ARGUMENT;
    }
    if (service->state != STORAGE_SERVICE_IDLE) {
        return STORAGE_SERVICE_RESULT_BAD_STATE;
    }
    if (!flash_interface_valid(&service->flash)) {
        return STORAGE_SERVICE_RESULT_BAD_STATE;
    }
    if (package_size < RESOURCE_PACKAGE_HEADER_SIZE ||
        package_size > RESOURCE_MAX_PACKAGE_SIZE ||
        format_version != RESOURCE_FORMAT_VERSION) {
        return STORAGE_SERVICE_RESULT_BAD_LENGTH;
    }

    service->target_bank = service->active.header_valid &&
                                   service->active.bank == STORAGE_BANK_A
                               ? STORAGE_BANK_B
                               : STORAGE_BANK_A;
    service->target_generation = service->active.header_valid
                                     ? service->active.generation + 1u
                                     : 1u;
    package_base = bank_package_base(service->target_bank);
    rounded_size = (package_size + STORAGE_FLASH_SECTOR_SIZE - 1u) &
                   ~(STORAGE_FLASH_SECTOR_SIZE - 1u);
    if (!range_fits(package_base, rounded_size,
                    bank_end(service->target_bank))) {
        return STORAGE_SERVICE_RESULT_BAD_LENGTH;
    }

    service->update_id = update_id;
    service->update_size = package_size;
    service->update_crc32 = package_crc32;
    service->last_update_id = update_id;
    service->last_next_offset = 0u;
    service->last_total_size = package_size;
    service->update_format_version = format_version;
    service->erase_address = storage_service_bank_base(service->target_bank);
    service->erase_end = package_base + rounded_size;
    service->next_offset = 0u;
    service->last_activity_ms = now_ms;
    service->last_chunk_valid = false;
    service->chunk_length = 0u;
    service->chunk_progress = 0u;
    service->update_committed = false;
    service->commit_marker_started = false;
    reset_flash_read_retry(service);
    service->journal_warning = false;
    service->error = STORAGE_SERVICE_ERROR_NONE;
    service->state = STORAGE_SERVICE_ERASING;
    service->phase = STORAGE_PHASE_ERASE_BANK;
    return STORAGE_SERVICE_RESULT_OK;
}

storage_service_result_t storage_service_write_chunk(
    storage_service_t *service, uint32_t update_id, uint32_t offset,
    const uint8_t *data, uint16_t length, uint32_t chunk_crc32,
    uint32_t now_ms) {
    uint32_t calculated_crc;

    if (service == NULL || data == NULL) {
        return STORAGE_SERVICE_RESULT_BAD_ARGUMENT;
    }
    if (service->state != STORAGE_SERVICE_READY &&
        service->state != STORAGE_SERVICE_RECEIVING) {
        return STORAGE_SERVICE_RESULT_BAD_STATE;
    }
    if (update_id != service->update_id) {
        return STORAGE_SERVICE_RESULT_BAD_SESSION;
    }
    if (service->phase != STORAGE_PHASE_WAIT_FOR_CHUNK ||
        service->pending_operation != STORAGE_PENDING_NONE) {
        return STORAGE_SERVICE_RESULT_BUSY;
    }
    if (length == 0u || length > STORAGE_SERVICE_MAX_CHUNK_SIZE) {
        return STORAGE_SERVICE_RESULT_BAD_LENGTH;
    }
    calculated_crc = resource_crc32(data, length);
    if (calculated_crc != chunk_crc32) {
        return STORAGE_SERVICE_RESULT_BAD_CRC;
    }
    if (service->last_chunk_valid && offset == service->last_chunk_offset) {
        if (length == service->last_chunk_length &&
            chunk_crc32 == service->last_chunk_crc32 &&
            memcmp(data, service->chunk_data, length) == 0) {
            service->last_activity_ms = now_ms;
            return STORAGE_SERVICE_RESULT_OK;
        }
        return STORAGE_SERVICE_RESULT_CONFLICT;
    }
    if (offset != service->next_offset) {
        return STORAGE_SERVICE_RESULT_BAD_OFFSET;
    }
    if (!range_fits(offset, length, service->update_size)) {
        return STORAGE_SERVICE_RESULT_BAD_LENGTH;
    }

    memcpy(service->chunk_data, data, length);
    service->chunk_offset = offset;
    service->chunk_length = length;
    service->chunk_progress = 0u;
    service->chunk_crc32 = chunk_crc32;
    service->last_activity_ms = now_ms;
    service->state = STORAGE_SERVICE_RECEIVING;
    service->phase = STORAGE_PHASE_WRITE_CHUNK;
    return STORAGE_SERVICE_RESULT_OK;
}

storage_service_result_t storage_service_finish(storage_service_t *service,
                                                uint32_t update_id,
                                                uint32_t now_ms) {
    storage_bank_info_t target;

    if (service == NULL) {
        return STORAGE_SERVICE_RESULT_BAD_ARGUMENT;
    }
    if (service->state != STORAGE_SERVICE_READY &&
        service->state != STORAGE_SERVICE_RECEIVING) {
        return STORAGE_SERVICE_RESULT_BAD_STATE;
    }
    if (update_id != service->update_id) {
        return STORAGE_SERVICE_RESULT_BAD_SESSION;
    }
    if (service->phase != STORAGE_PHASE_WAIT_FOR_CHUNK ||
        service->pending_operation != STORAGE_PENDING_NONE) {
        return STORAGE_SERVICE_RESULT_BUSY;
    }
    if (service->next_offset != service->update_size) {
        return STORAGE_SERVICE_RESULT_BAD_LENGTH;
    }

    memset(&target, 0, sizeof(target));
    target.header_valid = true;
    target.bank = service->target_bank;
    target.generation = service->target_generation;
    target.package_length = service->update_size;
    target.package_crc32 = service->update_crc32;
    target.resource_format_version = service->update_format_version;
    service->last_activity_ms = now_ms;
    prepare_verification(service, &target, false);
    return STORAGE_SERVICE_RESULT_OK;
}

storage_service_result_t storage_service_abort(storage_service_t *service,
                                               uint32_t update_id,
                                               uint32_t now_ms) {
    (void)now_ms;
    if (service == NULL) {
        return STORAGE_SERVICE_RESULT_BAD_ARGUMENT;
    }
    if (!update_state_active(service)) {
        return STORAGE_SERVICE_RESULT_BAD_STATE;
    }
    if (update_id != service->update_id) {
        return STORAGE_SERVICE_RESULT_BAD_SESSION;
    }
    service->error = STORAGE_SERVICE_ERROR_ABORTED;
    service->state = STORAGE_SERVICE_ABORTED;
    service->phase = STORAGE_PHASE_TERMINAL;
    return STORAGE_SERVICE_RESULT_OK;
}

void storage_service_link_lost(storage_service_t *service, uint32_t now_ms) {
    (void)now_ms;
    if (service == NULL || !update_state_active(service)) {
        return;
    }
    service->error = STORAGE_SERVICE_ERROR_LINK_LOST;
    service->state = STORAGE_SERVICE_ABORTED;
    service->phase = STORAGE_PHASE_TERMINAL;
}

bool storage_service_touch(storage_service_t *service, uint32_t update_id,
                           uint32_t now_ms) {
    if (service == NULL || !update_state_active(service) ||
        (update_id != 0u && update_id != service->update_id)) {
        return false;
    }
    service->last_activity_ms = now_ms;
    return true;
}

void storage_service_get_status(const storage_service_t *service,
                                storage_service_status_t *status) {
    if (service == NULL || status == NULL) {
        return;
    }
    status->state = service->state;
    status->active_bank = service->active.header_valid ? service->active.bank
                                                        : STORAGE_BANK_NONE;
    status->generation = service->active.header_valid
                             ? service->active.generation
                             : 0u;
    status->update_id = update_session_visible(service)
                            ? service->update_id
                            : service->last_update_id;
    status->next_offset = update_session_visible(service)
                              ? service->next_offset
                              : service->last_next_offset;
    status->total_size = update_session_visible(service)
                             ? service->update_size
                             : service->last_total_size;
    status->error = service->error;
    status->journal_warning = service->journal_warning;
    status->degraded = service->degraded || service->journal_warning;
}

bool storage_service_get_active_package(const storage_service_t *service,
                                        uint32_t *address, uint32_t *length,
                                        uint32_t *package_crc32) {
    if (service == NULL || address == NULL || length == NULL ||
        package_crc32 == NULL || !service->active.header_valid) {
        return false;
    }
    *address = bank_package_base(service->active.bank);
    *length = service->active.package_length;
    *package_crc32 = service->active.package_crc32;
    return true;
}
