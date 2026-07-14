#include "app.h"

#include <stddef.h>
#include <string.h>

#include "board.h"
#include "board_pins.h"
#include "ec11.h"
#include "input_policy.h"
#include "motion_service.h"
#include "protocol_ids.h"
#include "protocol_layouts.h"
#include "robot_spi_slot.h"
#include "robot_state.h"
#include "safety_supervisor.h"
#include "spi_mailbox.h"
#include "ui_service.h"
#include "uln2003_hw.h"
#include "w25q.h"

#define DEGRADED_OLED 0x0001u
#define DEGRADED_FLASH 0x0002u
#define CAPABILITIES 0x0000001Fu
#define DEDUP_CAPACITY 8u

typedef struct {
    bool valid;
    uint16_t type;
    uint32_t command_id;
    uint16_t request_seq;
    uint16_t error;
} dedup_entry_t;

typedef struct {
    robot_state_t state;
    safety_supervisor_t safety;
    motion_service_t motion;
    ec11_t encoder;
    input_policy_t input_policy;
    ui_service_t ui;
    w25q_t flash;
    spi_mailbox_t mailbox;
    dedup_entry_t dedup[DEDUP_CAPACITY];
    uint8_t dedup_next;
    volatile ec11_event_t encoder_event;
    uint16_t soft_rate_sps;
    uint16_t soft_accel_sps2;
    uint16_t hold_ms;
    uint8_t expression;
    uint32_t boot_id;
    motion_result_t reported_motion_result;
} app_context_t;

static app_context_t app;

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

static void stop_motion(motion_result_t result) {
    if (app.motion.active) {
        motion_service_abort(&app.motion, result);
    } else {
        uln2003_hw_off();
    }
}

static void handle_hello(const robot_spi_slot_view_t *slot, uint32_t now_ms) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_HELLO_RSP] = {0};
    safety_supervisor_session_started(&app.safety, now_ms);
    robot_write_u32_le(&payload[0], app.boot_id);
    robot_write_u32_le(&payload[4], CAPABILITIES);
    payload[8] = 0u;
    payload[9] = 1u;
    payload[10] = 0u;
    payload[11] = (uint8_t)app.state.value;
    queue_message(ROBOT_MSG_HELLO_RSP, ROBOT_SLOTFLAGS_RESPONSE, payload, sizeof(payload), true);
    (void)slot;
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
    if (app.state.value != ROBOT_STATE_MANUAL && app.state.value != ROBOT_STATE_AI) {
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

static void route_slot(const robot_spi_slot_view_t *slot, uint32_t now_ms) {
    safety_supervisor_valid_slot(&app.safety, now_ms);
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
            if (robot_state_request_mode(&app.state, action.mode)) {
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

bool app_init(void) {
    bool display_ok;
    bool flash_ok;
    memset(&app, 0, sizeof(app));
    app.boot_id = 0x53544D31u ^ HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
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
    }
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
