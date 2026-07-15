#include "app.h"

#include <stddef.h>
#include <string.h>

#include "board.h"
#include "board_pins.h"
#include "controller_session_policy.h"
#include "ec11.h"
#include "input_policy.h"
#include "motion_service.h"
#include "protocol_ids.h"
#include "protocol_layouts.h"
#include "robot_spi_slot.h"
#include "robot_state.h"
#include "safety_supervisor.h"
#include "session_epoch.h"
#include "spi_mailbox.h"
#include "resource_policy.h"
#include "storage_service.h"
#include "ui_service.h"
#include "uln2003_hw.h"
#include "w25q.h"
#include "w25q_storage_adapter.h"

#define DEGRADED_OLED 0x0001u
#define DEGRADED_FLASH 0x0002u
#define CAPABILITIES 0x0000001Fu
#define DEDUP_CAPACITY 8u

_Static_assert(STORAGE_SERVICE_BOOT_SCAN == ROBOT_RESOURCESTATE_BOOT_SCAN,
               "resource state IDs must match the wire protocol");
_Static_assert(STORAGE_SERVICE_FAILED == ROBOT_RESOURCESTATE_FAILED,
               "resource state IDs must match the wire protocol");

typedef struct {
    bool valid;
    uint16_t type;
    uint32_t command_id;
    uint16_t request_seq;
    uint16_t error;
} dedup_entry_t;

typedef struct {
    storage_flash_t *flash;
    uint32_t package_base;
    uint32_t package_length;
} resource_reader_t;

typedef struct {
    robot_state_t state;
    safety_supervisor_t safety;
    motion_service_t motion;
    ec11_t encoder;
    input_policy_t input_policy;
    ui_service_t ui;
    w25q_t flash;
    storage_flash_t storage_flash;
    storage_service_t storage;
    face_resource_provider_t face_resources;
    resource_reader_t resource_reader;
    spi_mailbox_t mailbox;
    dedup_entry_t dedup[DEDUP_CAPACITY];
    uint8_t dedup_next;
    volatile ec11_event_t encoder_event;
    uint16_t soft_rate_sps;
    uint16_t soft_accel_sps2;
    uint16_t hold_ms;
    uint8_t expression;
    uint32_t boot_id;
    session_peer_epoch_t esp_epoch;
    bool controller_session_active;
    motion_result_t reported_motion_result;
    uint32_t loaded_resource_generation;
    uint16_t resource_request_error;
    bool face_resources_loaded;
} app_context_t;

static app_context_t app;

static bool resource_update_active(void) {
    storage_service_status_t status;

    storage_service_get_status(&app.storage, &status);
    return resource_policy_update_active(status.state);
}

static bool bytes_are_zero(const uint8_t *data, uint16_t length) {
    uint16_t index;

    for (index = 0u; index < length; ++index) {
        if (data[index] != 0u) {
            return false;
        }
    }
    return true;
}

static bool resource_read(void *context, uint32_t offset,
                          uint8_t *destination, uint32_t length) {
    resource_reader_t *reader = (resource_reader_t *)context;

    if (reader == NULL || reader->flash == NULL || destination == NULL ||
        length == 0u || length > UINT16_MAX ||
        offset > reader->package_length ||
        length > reader->package_length - offset ||
        reader->package_base > UINT32_MAX - offset) {
        return false;
    }
    return reader->flash->ops->read(
               reader->flash->context, reader->package_base + offset,
               destination, (uint16_t)length) == STORAGE_FLASH_OK;
}

static void queue_message(uint16_t type, uint8_t flags, const uint8_t *payload,
                          uint16_t length, bool priority) {
    (void)spi_mailbox_queue(&app.mailbox, type, flags, payload, length, priority);
}

static void queue_ack(uint16_t request_seq, uint32_t command_id) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_ACK];
    robot_write_u16_le(&payload[0], request_seq);
    robot_write_u32_le(&payload[2], command_id);
    payload[6] = ROBOT_ERRORCODE_OK;
    queue_message(ROBOT_MSG_ACK, ROBOT_SLOTFLAGS_RESPONSE, payload, sizeof(payload), true);
}

static void queue_nack(uint16_t request_seq, uint32_t command_id, uint16_t error) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_NACK];
    robot_write_u16_le(&payload[0], request_seq);
    robot_write_u32_le(&payload[2], command_id);
    robot_write_u16_le(&payload[6], error);
    queue_message(ROBOT_MSG_NACK, ROBOT_SLOTFLAGS_RESPONSE, payload, sizeof(payload), true);
}

static void remember_result(uint16_t type, uint32_t command_id, uint16_t request_seq,
                            uint16_t error) {
    dedup_entry_t *entry = &app.dedup[app.dedup_next];
    entry->valid = true;
    entry->type = type;
    entry->command_id = command_id;
    entry->request_seq = request_seq;
    entry->error = error;
    app.dedup_next = (uint8_t)((app.dedup_next + 1u) % DEDUP_CAPACITY);
}

static void clear_dedup(void) {
    memset(app.dedup, 0, sizeof(app.dedup));
    app.dedup_next = 0u;
}

static bool replay_duplicate(uint16_t type, uint32_t command_id, uint16_t request_seq) {
    uint8_t index;
    for (index = 0u; index < DEDUP_CAPACITY; ++index) {
        const dedup_entry_t *entry = &app.dedup[index];
        if (entry->valid && entry->type == type && entry->command_id == command_id) {
            if (entry->error == ROBOT_ERRORCODE_OK) {
                queue_ack(request_seq, command_id);
            } else {
                queue_nack(request_seq, command_id, entry->error);
            }
            return true;
        }
    }
    return false;
}

static void finish_command(uint16_t type, uint32_t command_id, uint16_t request_seq,
                           uint16_t error) {
    if (error == ROBOT_ERRORCODE_OK) {
        queue_ack(request_seq, command_id);
    } else {
        queue_nack(request_seq, command_id, error);
    }
    remember_result(type, command_id, request_seq, error);
}

static void queue_mode_changed(uint8_t source) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_MODE_CHANGED] = {
        (uint8_t)app.state.value, source,
    };
    queue_message(ROBOT_MSG_MODE_CHANGED, ROBOT_SLOTFLAGS_EVENT, payload, sizeof(payload), false);
}

static void queue_state_snapshot(void) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_STATE_SNAPSHOT] = {0};
    robot_write_u32_le(&payload[0], app.boot_id);
    robot_write_u32_le(&payload[4], board_millis());
    payload[8] = (uint8_t)app.state.value;
    payload[9] = (uint8_t)app.input_policy.candidate_mode;
    robot_write_u16_le(&payload[10], app.state.degraded_flags);
    robot_write_u16_le(&payload[12], app.state.fault_code);
    robot_write_u32_le(&payload[14], app.motion.active ? app.motion.command_id : 0u);
    robot_write_u16_le(&payload[18], (uint16_t)app.mailbox.errors);
    payload[20] = app.mailbox.queue_count;
    payload[21] = 0u;
    queue_message(ROBOT_MSG_STATE_SNAPSHOT, ROBOT_SLOTFLAGS_RESPONSE, payload,
                  sizeof(payload), false);
}

static void queue_flash_info(void) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_FLASH_INFO] = {0};
    robot_write_u32_le(&payload[0], app.flash.jedec_id);
    robot_write_u32_le(&payload[4], app.flash.capacity_bytes);
    payload[8] = app.flash.available ? 0u : 1u;
    queue_message(ROBOT_MSG_FLASH_INFO, ROBOT_SLOTFLAGS_EVENT, payload, sizeof(payload), false);
}

static uint16_t resource_error_from_storage(storage_service_error_t error) {
    switch (error) {
        case STORAGE_SERVICE_ERROR_NONE:
            return ROBOT_RESOURCEERROR_NONE;
        case STORAGE_SERVICE_ERROR_FLASH_UNAVAILABLE:
            return ROBOT_RESOURCEERROR_FLASH_UNAVAILABLE;
        case STORAGE_SERVICE_ERROR_FLASH_PROTECTED:
            return ROBOT_RESOURCEERROR_WRITE_PROTECTED;
        case STORAGE_SERVICE_ERROR_FLASH_RANGE:
            return ROBOT_RESOURCEERROR_CAPACITY_INSUFFICIENT;
        case STORAGE_SERVICE_ERROR_PACKAGE_CRC:
            return ROBOT_RESOURCEERROR_PACKAGE_CRC;
        case STORAGE_SERVICE_ERROR_PACKAGE_FORMAT:
            return ROBOT_RESOURCEERROR_BAD_DIRECTORY;
        case STORAGE_SERVICE_ERROR_FLASH_TIMEOUT:
        case STORAGE_SERVICE_ERROR_TIMEOUT:
            return ROBOT_RESOURCEERROR_BUSY_TIMEOUT;
        case STORAGE_SERVICE_ERROR_LINK_LOST:
            return ROBOT_RESOURCEERROR_LINK_LOST;
        case STORAGE_SERVICE_ERROR_ABORTED:
        case STORAGE_SERVICE_ERROR_JOURNAL:
            return ROBOT_RESOURCEERROR_NONE;
        case STORAGE_SERVICE_ERROR_FLASH_IO:
        default:
            return ROBOT_RESOURCEERROR_INTERNAL;
    }
}

static uint8_t resource_bank_on_wire(storage_bank_t bank) {
    if (bank == STORAGE_BANK_A) {
        return 0u;
    }
    if (bank == STORAGE_BANK_B) {
        return 1u;
    }
    return 0xFFu;
}

static void queue_resource_status(uint32_t requested_update_id,
                                  uint32_t now_ms) {
    storage_service_status_t status;
    uint8_t payload[ROBOT_PAYLOAD_LEN_RESOURCE_STATUS] = {0};
    uint16_t error;

    (void)storage_service_touch(&app.storage, requested_update_id, now_ms);
    storage_service_get_status(&app.storage, &status);
    error = resource_error_from_storage(status.error);
    if (error == ROBOT_RESOURCEERROR_NONE) {
        error = app.resource_request_error;
    }
    if (requested_update_id != 0u && requested_update_id != status.update_id) {
        error = ROBOT_RESOURCEERROR_SESSION_MISMATCH;
    }
    robot_write_u32_le(&payload[0], status.update_id);
    payload[4] = (uint8_t)status.state;
    payload[5] = resource_bank_on_wire(status.active_bank);
    robot_write_u32_le(&payload[6], status.generation);
    robot_write_u32_le(&payload[10], status.next_offset);
    robot_write_u32_le(&payload[14], status.total_size);
    robot_write_u16_le(&payload[18], error);
    queue_message(ROBOT_MSG_RESOURCE_STATUS, ROBOT_SLOTFLAGS_RESPONSE,
                  payload, sizeof(payload), true);
}

static uint16_t protocol_error_for_storage_result(
    storage_service_result_t result) {
    switch (result) {
        case STORAGE_SERVICE_RESULT_OK:
            return ROBOT_ERRORCODE_OK;
        case STORAGE_SERVICE_RESULT_BUSY:
            return ROBOT_ERRORCODE_QUEUE_FULL;
        case STORAGE_SERVICE_RESULT_BAD_STATE:
        case STORAGE_SERVICE_RESULT_BAD_SESSION:
            return ROBOT_ERRORCODE_BAD_STATE;
        case STORAGE_SERVICE_RESULT_BAD_LENGTH:
        case STORAGE_SERVICE_RESULT_BAD_OFFSET:
            return ROBOT_ERRORCODE_OUT_OF_RANGE;
        case STORAGE_SERVICE_RESULT_BAD_ARGUMENT:
        case STORAGE_SERVICE_RESULT_BAD_CRC:
        case STORAGE_SERVICE_RESULT_CONFLICT:
        default:
            return ROBOT_ERRORCODE_BAD_PAYLOAD;
    }
}

static uint16_t resource_error_for_storage_result(
    storage_service_result_t result) {
    switch (result) {
        case STORAGE_SERVICE_RESULT_OK:
            return ROBOT_RESOURCEERROR_NONE;
        case STORAGE_SERVICE_RESULT_BUSY:
            return ROBOT_RESOURCEERROR_UPDATE_BUSY;
        case STORAGE_SERVICE_RESULT_BAD_SESSION:
            return ROBOT_RESOURCEERROR_SESSION_MISMATCH;
        case STORAGE_SERVICE_RESULT_BAD_STATE:
            return ROBOT_RESOURCEERROR_BAD_STATE;
        case STORAGE_SERVICE_RESULT_BAD_LENGTH:
            return ROBOT_RESOURCEERROR_BAD_LENGTH;
        case STORAGE_SERVICE_RESULT_BAD_OFFSET:
        case STORAGE_SERVICE_RESULT_CONFLICT:
            return ROBOT_RESOURCEERROR_BAD_OFFSET;
        case STORAGE_SERVICE_RESULT_BAD_CRC:
            return ROBOT_RESOURCEERROR_CHUNK_CRC;
        case STORAGE_SERVICE_RESULT_BAD_ARGUMENT:
        default:
            return ROBOT_RESOURCEERROR_INTERNAL;
    }
}

static bool storage_error_degrades_flash(storage_service_error_t error) {
    return error == STORAGE_SERVICE_ERROR_FLASH_UNAVAILABLE ||
           error == STORAGE_SERVICE_ERROR_FLASH_PROTECTED ||
           error == STORAGE_SERVICE_ERROR_FLASH_RANGE ||
           error == STORAGE_SERVICE_ERROR_FLASH_IO ||
           error == STORAGE_SERVICE_ERROR_FLASH_TIMEOUT;
}

static void stop_motion(motion_result_t result) {
    if (app.motion.active) {
        motion_service_abort(&app.motion, result);
    } else {
        uln2003_hw_off();
    }
}

static void handle_hello(const robot_spi_slot_view_t *slot, uint32_t now_ms) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_HELLO_RSP] = {0};
    uint32_t esp_boot_id = robot_read_u32_le(&slot->payload[0]);
    session_peer_result_t peer_result =
        session_peer_epoch_observe(&app.esp_epoch, esp_boot_id);

    if (peer_result == SESSION_PEER_CHANGED) {
        stop_motion(MOTION_RESULT_ABORTED);
        storage_service_link_lost(&app.storage, now_ms);
        clear_dedup();
    }
    app.controller_session_active = true;
    safety_supervisor_session_started(&app.safety, now_ms);
    robot_write_u32_le(&payload[0], app.boot_id);
    robot_write_u32_le(&payload[4], CAPABILITIES);
    payload[8] = 0u;
    payload[9] = 1u;
    payload[10] = 0u;
    payload[11] = (uint8_t)app.state.value;
    queue_message(ROBOT_MSG_HELLO_RSP, ROBOT_SLOTFLAGS_RESPONSE, payload, sizeof(payload), true);
    queue_flash_info();
}

static uint32_t command_id(const robot_spi_slot_view_t *slot) {
    return slot->length >= 4u ? robot_read_u32_le(slot->payload) : 0u;
}

static void handle_set_mode(const robot_spi_slot_view_t *slot) {
    uint32_t id = command_id(slot);
    robot_state_value_t mode = (robot_state_value_t)slot->payload[4];
    uint16_t error = ROBOT_ERRORCODE_OK;
    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    if (mode != ROBOT_STATE_IDLE && mode != ROBOT_STATE_MANUAL && mode != ROBOT_STATE_AI) {
        error = ROBOT_ERRORCODE_BAD_PAYLOAD;
    } else if (resource_update_active() && mode != ROBOT_STATE_IDLE) {
        error = ROBOT_ERRORCODE_BAD_STATE;
    } else if (!robot_state_request_mode(&app.state, mode)) {
        error = ROBOT_ERRORCODE_BAD_STATE;
    } else {
        stop_motion(MOTION_RESULT_ABORTED);
        input_policy_sync_mode(&app.input_policy, mode);
    }
    finish_command(slot->type, id, slot->seq, error);
    if (error == ROBOT_ERRORCODE_OK) {
        queue_mode_changed(1u);
    }
}

static void handle_move(const robot_spi_slot_view_t *slot, uint32_t now_ms) {
    uint32_t id = command_id(slot);
    int32_t left_steps = robot_read_i32_le(&slot->payload[4]);
    int32_t right_steps = robot_read_i32_le(&slot->payload[8]);
    uint16_t rate = robot_read_u16_le(&slot->payload[12]);
    uint16_t accel = robot_read_u16_le(&slot->payload[14]);
    uint16_t timeout = robot_read_u16_le(&slot->payload[16]);
    uint16_t error = ROBOT_ERRORCODE_OK;
    uint8_t event_payload[ROBOT_PAYLOAD_LEN_MOTION_STARTED];

    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    if (resource_update_active()) {
        error = ROBOT_ERRORCODE_BAD_STATE;
    } else if (app.state.value != ROBOT_STATE_MANUAL && app.state.value != ROBOT_STATE_AI) {
        error = ROBOT_ERRORCODE_BAD_STATE;
    } else if (rate > app.soft_rate_sps || accel > app.soft_accel_sps2) {
        error = ROBOT_ERRORCODE_OUT_OF_RANGE;
    } else if (!motion_service_start(&app.motion, id, left_steps, right_steps, rate,
                                     accel, timeout, now_ms)) {
        error = app.motion.active ? ROBOT_ERRORCODE_QUEUE_FULL : ROBOT_ERRORCODE_OUT_OF_RANGE;
    }
    finish_command(slot->type, id, slot->seq, error);
    if (error == ROBOT_ERRORCODE_OK) {
        app.reported_motion_result = MOTION_RESULT_NONE;
        robot_write_u32_le(event_payload, id);
        queue_message(ROBOT_MSG_MOTION_STARTED, ROBOT_SLOTFLAGS_EVENT,
                      event_payload, sizeof(event_payload), false);
    }
}

static void handle_stop(const robot_spi_slot_view_t *slot) {
    uint32_t id = command_id(slot);
    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    stop_motion(MOTION_RESULT_ABORTED);
    safety_supervisor_stop(&app.safety, &app.state, SAFETY_REASON_REMOTE_STOP);
    finish_command(slot->type, id, slot->seq, ROBOT_ERRORCODE_OK);
}

static void handle_expression(const robot_spi_slot_view_t *slot) {
    uint32_t id = command_id(slot);
    uint8_t expression = slot->payload[4];
    uint16_t error = ROBOT_ERRORCODE_OK;
    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    if (app.state.value == ROBOT_STATE_ESTOP || app.state.value == ROBOT_STATE_FAULT) {
        error = ROBOT_ERRORCODE_BAD_STATE;
    } else if (expression > ROBOT_EXPRESSION_SLEEPY) {
        error = ROBOT_ERRORCODE_BAD_PAYLOAD;
    } else {
        app.expression = expression;
        ui_service_set_expression(&app.ui, expression, board_millis());
    }
    finish_command(slot->type, id, slot->seq, error);
}

static void handle_clear_estop(const robot_spi_slot_view_t *slot) {
    uint32_t id = command_id(slot);
    uint16_t error = ROBOT_ERRORCODE_BAD_STATE;
    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    if (!app.motion.active &&
        robot_state_clear_estop(&app.state, app.safety.link_healthy, true, true)) {
        app.expression = ROBOT_EXPRESSION_NEUTRAL;
        ui_service_set_expression(&app.ui, ROBOT_EXPRESSION_NEUTRAL, board_millis());
        input_policy_sync_mode(&app.input_policy, ROBOT_STATE_IDLE);
        error = ROBOT_ERRORCODE_OK;
    }
    finish_command(slot->type, id, slot->seq, error);
    if (error == ROBOT_ERRORCODE_OK) {
        queue_mode_changed(3u);
    }
}

static void handle_runtime_config(const robot_spi_slot_view_t *slot) {
    uint32_t id = command_id(slot);
    uint16_t rate = robot_read_u16_le(&slot->payload[4]);
    uint16_t accel = robot_read_u16_le(&slot->payload[6]);
    uint16_t hold = robot_read_u16_le(&slot->payload[8]);
    uint16_t error = ROBOT_ERRORCODE_OK;
    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    if (rate == 0u || rate > MOTION_HARD_MAX_RATE_SPS || accel == 0u ||
        accel > MOTION_HARD_MAX_ACCEL_SPS2 || hold > 2000u) {
        error = ROBOT_ERRORCODE_OUT_OF_RANGE;
    } else {
        app.soft_rate_sps = rate;
        app.soft_accel_sps2 = accel;
        app.hold_ms = hold;
    }
    finish_command(slot->type, id, slot->seq, error);
}

static void handle_resource_begin(const robot_spi_slot_view_t *slot,
                                  uint32_t now_ms) {
    uint32_t id = command_id(slot);
    uint32_t update_id = robot_read_u32_le(&slot->payload[4]);
    uint32_t package_size = robot_read_u32_le(&slot->payload[8]);
    uint32_t package_crc32 = robot_read_u32_le(&slot->payload[12]);
    uint16_t format_version = robot_read_u16_le(&slot->payload[16]);
    storage_service_result_t result = STORAGE_SERVICE_RESULT_BAD_STATE;
    storage_service_status_t status;
    uint16_t error;

    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    storage_service_get_status(&app.storage, &status);
    if (!resource_policy_can_begin(
            app.state.value, status.state, app.safety.link_healthy,
            app.motion.active)) {
        app.resource_request_error = ROBOT_RESOURCEERROR_BAD_STATE;
    } else if (format_version != RESOURCE_FORMAT_VERSION) {
        result = STORAGE_SERVICE_RESULT_BAD_LENGTH;
        app.resource_request_error = ROBOT_RESOURCEERROR_BAD_FORMAT_VERSION;
    } else if (package_size < RESOURCE_PACKAGE_HEADER_SIZE ||
               package_size > RESOURCE_MAX_PACKAGE_SIZE) {
        result = STORAGE_SERVICE_RESULT_BAD_LENGTH;
        app.resource_request_error = ROBOT_RESOURCEERROR_BAD_PACKAGE_SIZE;
    } else {
        result = storage_service_begin(
            &app.storage, update_id, package_size, package_crc32,
            format_version, now_ms);
        app.resource_request_error = resource_error_for_storage_result(result);
        if (result == STORAGE_SERVICE_RESULT_OK) {
            ui_service_set_resource_provider(&app.ui, NULL, now_ms);
        }
    }
    error = protocol_error_for_storage_result(result);
    finish_command(slot->type, id, slot->seq, error);
}

static void handle_resource_chunk(const robot_spi_slot_view_t *slot,
                                  uint32_t now_ms) {
    uint32_t id = command_id(slot);
    uint32_t update_id = robot_read_u32_le(&slot->payload[4]);
    uint32_t offset = robot_read_u32_le(&slot->payload[8]);
    uint16_t data_length = robot_read_u16_le(&slot->payload[12]);
    uint32_t chunk_crc32 = robot_read_u32_le(&slot->payload[14]);
    const uint8_t *data = &slot->payload[18];
    storage_service_result_t result;

    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    if (data_length == 0u || data_length > STORAGE_SERVICE_MAX_CHUNK_SIZE ||
        !bytes_are_zero(&data[data_length],
                        (uint16_t)(STORAGE_SERVICE_MAX_CHUNK_SIZE -
                                   data_length))) {
        result = STORAGE_SERVICE_RESULT_BAD_LENGTH;
        app.resource_request_error = ROBOT_RESOURCEERROR_BAD_LENGTH;
    } else {
        result = storage_service_write_chunk(
            &app.storage, update_id, offset, data, data_length,
            chunk_crc32, now_ms);
        app.resource_request_error = resource_error_for_storage_result(result);
    }
    finish_command(slot->type, id, slot->seq,
                   protocol_error_for_storage_result(result));
}

static void handle_resource_finish(const robot_spi_slot_view_t *slot,
                                   uint32_t now_ms) {
    uint32_t id = command_id(slot);
    uint32_t update_id = robot_read_u32_le(&slot->payload[4]);
    storage_service_result_t result;

    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    result = storage_service_finish(&app.storage, update_id, now_ms);
    app.resource_request_error = resource_error_for_storage_result(result);
    finish_command(slot->type, id, slot->seq,
                   protocol_error_for_storage_result(result));
}

static void handle_resource_abort(const robot_spi_slot_view_t *slot,
                                  uint32_t now_ms) {
    uint32_t id = command_id(slot);
    uint32_t update_id = robot_read_u32_le(&slot->payload[4]);
    storage_service_result_t result;

    if (replay_duplicate(slot->type, id, slot->seq)) {
        return;
    }
    result = storage_service_abort(&app.storage, update_id, now_ms);
    app.resource_request_error = resource_error_for_storage_result(result);
    finish_command(slot->type, id, slot->seq,
                   protocol_error_for_storage_result(result));
}

static void route_slot(const robot_spi_slot_view_t *slot, uint32_t now_ms) {
    controller_session_route_t session_route =
        controller_session_route_policy(app.controller_session_active,
                                        slot->type);

    if (session_route == CONTROLLER_SESSION_ROUTE_REJECT_BAD_STATE) {
        queue_nack(slot->seq, command_id(slot), ROBOT_ERRORCODE_BAD_STATE);
        return;
    }
    if (app.controller_session_active &&
        slot->type != ROBOT_MSG_HELLO_REQ) {
        safety_supervisor_valid_slot(&app.safety, now_ms);
    }
    switch (slot->type) {
        case ROBOT_MSG_NOOP:
            break;
        case ROBOT_MSG_HELLO_REQ:
            handle_hello(slot, now_ms);
            break;
        case ROBOT_MSG_HEARTBEAT:
            break;
        case ROBOT_MSG_GET_STATE:
            queue_state_snapshot();
            break;
        case ROBOT_MSG_SET_MODE:
            handle_set_mode(slot);
            break;
        case ROBOT_MSG_MOVE_WHEELS:
            handle_move(slot, now_ms);
            break;
        case ROBOT_MSG_STOP:
            handle_stop(slot);
            break;
        case ROBOT_MSG_SET_EXPRESSION:
            handle_expression(slot);
            break;
        case ROBOT_MSG_SET_RUNTIME_CONFIG:
            handle_runtime_config(slot);
            break;
        case ROBOT_MSG_CLEAR_ESTOP:
            handle_clear_estop(slot);
            break;
        case ROBOT_MSG_RESOURCE_BEGIN:
            handle_resource_begin(slot, now_ms);
            break;
        case ROBOT_MSG_RESOURCE_CHUNK:
            handle_resource_chunk(slot, now_ms);
            break;
        case ROBOT_MSG_RESOURCE_FINISH:
            handle_resource_finish(slot, now_ms);
            break;
        case ROBOT_MSG_RESOURCE_ABORT:
            handle_resource_abort(slot, now_ms);
            break;
        case ROBOT_MSG_GET_RESOURCE_STATUS:
            queue_resource_status(robot_read_u32_le(slot->payload), now_ms);
            break;
        default:
            queue_nack(slot->seq, command_id(slot), ROBOT_ERRORCODE_BAD_MESSAGE);
            break;
    }
}

static void report_motion_result(void) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_MOTION_DONE] = {0};
    if (app.motion.result == MOTION_RESULT_NONE ||
        app.motion.result == app.reported_motion_result) {
        return;
    }
    app.reported_motion_result = app.motion.result;
    if (app.motion.result == MOTION_RESULT_DONE) {
        robot_write_u32_le(&payload[0], app.motion.command_id);
        robot_write_i32_le(&payload[4], app.motion.left_target < 0
                                            ? -(int32_t)app.motion.left_done
                                            : (int32_t)app.motion.left_done);
        robot_write_i32_le(&payload[8], app.motion.right_target < 0
                                            ? -(int32_t)app.motion.right_done
                                            : (int32_t)app.motion.right_done);
        payload[12] = 0u;
        queue_message(ROBOT_MSG_MOTION_DONE, ROBOT_SLOTFLAGS_EVENT,
                      payload, sizeof(payload), false);
    } else {
        uint8_t aborted[ROBOT_PAYLOAD_LEN_MOTION_ABORTED];
        robot_write_u32_le(&aborted[0], app.motion.command_id);
        robot_write_u16_le(&aborted[4], app.motion.result == MOTION_RESULT_TIMEOUT
                                            ? ROBOT_ABORTREASON_TIMEOUT
                                            : ROBOT_ABORTREASON_STOP);
        queue_message(ROBOT_MSG_MOTION_ABORTED, ROBOT_SLOTFLAGS_EVENT,
                      aborted, sizeof(aborted), true);
    }
}

static bool encoder_switch_high(void) {
#if BOARD_ENCODER_BUTTON_PRESENT
    return HAL_GPIO_ReadPin(ENCODER_SW_PORT, ENCODER_SW_PIN) == GPIO_PIN_SET;
#else
    return true;
#endif
}

static void handle_encoder_event(void) {
    ec11_event_t event = app.encoder_event;
    input_policy_action_t action;
    app.encoder_event = EC11_EVENT_NONE;
    action = input_policy_handle(&app.input_policy, event, app.state.value);
    switch (action.type) {
        case INPUT_POLICY_ACTION_SET_MODE:
            if (resource_update_active() && action.mode != ROBOT_STATE_IDLE) {
                input_policy_sync_mode(&app.input_policy, ROBOT_STATE_IDLE);
            } else if (robot_state_request_mode(&app.state, action.mode)) {
                stop_motion(MOTION_RESULT_ABORTED);
                input_policy_sync_mode(&app.input_policy, action.mode);
                queue_mode_changed(2u);
            }
            break;
        case INPUT_POLICY_ACTION_ESTOP:
            stop_motion(MOTION_RESULT_ABORTED);
            safety_supervisor_stop(&app.safety, &app.state, SAFETY_REASON_LOCAL_STOP);
            break;
        case INPUT_POLICY_ACTION_CLEAR_ESTOP:
            if (robot_state_clear_estop(&app.state, app.safety.link_healthy, true, true)) {
                app.expression = ROBOT_EXPRESSION_NEUTRAL;
                ui_service_set_expression(
                    &app.ui, ROBOT_EXPRESSION_NEUTRAL, board_millis());
                input_policy_sync_mode(&app.input_policy, ROBOT_STATE_IDLE);
                queue_mode_changed(2u);
            }
            break;
        case INPUT_POLICY_ACTION_NONE:
        default:
            break;
    }
}

static void sync_face_resources(uint32_t now_ms) {
    storage_service_status_t status;
    resource_package_t package;
    uint32_t package_address;
    uint32_t package_length;
    uint32_t package_crc32;

    storage_service_get_status(&app.storage, &status);
    robot_state_set_degraded(
        &app.state, DEGRADED_FLASH,
        !app.flash.available || status.degraded ||
            storage_error_degrades_flash(status.error) ||
            (app.face_resources_loaded &&
             !face_resource_provider_is_healthy(&app.face_resources)));
    if (status.state != STORAGE_SERVICE_IDLE ||
        !storage_service_get_active_package(
            &app.storage, &package_address, &package_length, &package_crc32)) {
        if (app.ui.resource_provider != NULL) {
            ui_service_set_resource_provider(&app.ui, NULL, now_ms);
        }
        return;
    }

    if (app.face_resources_loaded &&
        app.loaded_resource_generation == status.generation) {
        if (!face_resource_provider_is_healthy(&app.face_resources)) {
            ui_service_set_resource_provider(&app.ui, NULL, now_ms);
            robot_state_set_degraded(&app.state, DEGRADED_FLASH, true);
        } else if (app.ui.resource_provider == NULL) {
            ui_service_set_resource_provider(
                &app.ui, &app.face_resources, now_ms);
        }
        return;
    }

    app.resource_reader.flash = &app.storage_flash;
    app.resource_reader.package_base = package_address;
    app.resource_reader.package_length = package_length;
    if (resource_package_open(&package, resource_read, &app.resource_reader,
                              package_length) != RESOURCE_FORMAT_OK ||
        package.header.package_crc32 != package_crc32 ||
        face_resource_provider_init(
            &app.face_resources, &package,
            app.boot_id ^ status.generation) != FACE_RESOURCE_PROVIDER_OK) {
        app.face_resources_loaded = false;
        robot_state_set_degraded(&app.state, DEGRADED_FLASH, true);
        ui_service_set_resource_provider(&app.ui, NULL, now_ms);
        return;
    }
    app.loaded_resource_generation = status.generation;
    app.face_resources_loaded = true;
    ui_service_set_resource_provider(&app.ui, &app.face_resources, now_ms);
}

bool app_init(void) {
    bool display_ok;
    bool flash_ok;
    memset(&app, 0, sizeof(app));
    app.boot_id = board_next_boot_id();
    session_peer_epoch_init(&app.esp_epoch);
    app.soft_rate_sps = 400u;
    app.soft_accel_sps2 = 600u;
    app.hold_ms = 200u;
    app.expression = ROBOT_EXPRESSION_NEUTRAL;
    robot_state_init(&app.state);
    input_policy_init(&app.input_policy, BOARD_ENCODER_BUTTON_PRESENT != 0,
                      ROBOT_STATE_IDLE);
    safety_supervisor_init(&app.safety);
    motion_service_init(&app.motion, uln2003_hw_apply, NULL);
    ec11_init(&app.encoder,
              HAL_GPIO_ReadPin(ENCODER_A_PORT, ENCODER_A_PIN) == GPIO_PIN_SET,
              HAL_GPIO_ReadPin(ENCODER_B_PORT, ENCODER_B_PIN) == GPIO_PIN_SET,
              encoder_switch_high());
    display_ok = ui_service_init(&app.ui, app.boot_id ^ board_millis());
    flash_ok = w25q_init(&app.flash);
    w25q_storage_adapter_init(&app.storage_flash, &app.flash);
    storage_service_init(&app.storage, &app.storage_flash);
    robot_state_set_degraded(&app.state, DEGRADED_OLED, !display_ok);
    robot_state_set_degraded(&app.state, DEGRADED_FLASH, !flash_ok);
    (void)robot_state_finish_self_test(&app.state, true);
    ui_service_set_status(&app.ui, app.state.value, false);
    spi_mailbox_init(&app.mailbox);
    return spi_mailbox_start(&app.mailbox);
}

void app_timer_1ms_isr(void) {
    ec11_event_t event;
    motion_service_tick_1ms(&app.motion, HAL_GetTick());
    event = ec11_sample_1ms(
        &app.encoder,
        HAL_GPIO_ReadPin(ENCODER_A_PORT, ENCODER_A_PIN) == GPIO_PIN_SET,
        HAL_GPIO_ReadPin(ENCODER_B_PORT, ENCODER_B_PIN) == GPIO_PIN_SET,
        encoder_switch_high());
    if (event == EC11_EVENT_LONG_PRESS || app.encoder_event == EC11_EVENT_NONE) {
        app.encoder_event = event;
    }
}

void app_process(void) {
    robot_spi_slot_view_t slot;
    robot_slot_status_t status;
    uint32_t now_ms = board_millis();

    if (spi_mailbox_take_received(&app.mailbox, &slot, &status)) {
        if (status == ROBOT_SLOT_OK) {
            route_slot(&slot, now_ms);
        }
        (void)spi_mailbox_rearm(&app.mailbox);
    } else if (!app.mailbox.armed) {
        (void)spi_mailbox_rearm(&app.mailbox);
    }

    if (safety_supervisor_tick(&app.safety, &app.state, now_ms)) {
        stop_motion(MOTION_RESULT_ABORTED);
        storage_service_link_lost(&app.storage, now_ms);
    }
    storage_service_tick(&app.storage, now_ms);
    sync_face_resources(now_ms);
    if (app.encoder_event != EC11_EVENT_NONE) {
        handle_encoder_event();
    }
    report_motion_result();
    ui_service_set_status(&app.ui, app.state.value, app.safety.link_healthy);
    ui_service_tick(&app.ui, now_ms);
    board_status_led_set(app.safety.link_healthy);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *timer) {
    if (timer->Instance == TIM2) {
        app_timer_1ms_isr();
    }
}
