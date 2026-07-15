#include <stdbool.h>
#include <stdint.h>

#include "ec11.h"
#include "input_policy.h"
#include "motion_service.h"
#include "robot_state.h"
#include "safety_supervisor.h"
#include "session_epoch.h"
#include "test_harness.h"

typedef struct {
    uint8_t left;
    uint8_t right;
    uint32_t calls;
} output_capture_t;

static void capture_output(uint8_t left, uint8_t right, void *context) {
    output_capture_t *capture = context;
    capture->left = left;
    capture->right = right;
    capture->calls++;
}

static int test_state_and_safety(void) {
    robot_state_t state;
    safety_supervisor_t safety;

    robot_state_init(&state);
    TEST_ASSERT_EQ(ROBOT_STATE_SELF_TEST, state.value);
    TEST_ASSERT(robot_state_finish_self_test(&state, true));
    TEST_ASSERT(robot_state_request_mode(&state, ROBOT_STATE_MANUAL));
    safety_supervisor_init(&safety);
    TEST_ASSERT(!safety.session_active);
    TEST_ASSERT(!safety.link_healthy);
    safety_supervisor_valid_slot(&safety, 100u);
    TEST_ASSERT(safety.session_active);
    TEST_ASSERT(safety.link_healthy);
    TEST_ASSERT_EQ(100u, safety.last_valid_slot_ms);
    TEST_ASSERT(!safety_supervisor_tick(&safety, &state, 850u));
    TEST_ASSERT(safety_supervisor_tick(&safety, &state, 851u));
    TEST_ASSERT_EQ(ROBOT_STATE_ESTOP, state.value);
    TEST_ASSERT_EQ(SAFETY_REASON_LINK_LOST, state.fault_code);
    TEST_ASSERT(!safety.session_active);
    TEST_ASSERT(!safety.link_healthy);
    TEST_ASSERT(!robot_state_clear_estop(&state, false, true, true));
    safety_supervisor_valid_slot(&safety, 900u);
    TEST_ASSERT(safety.session_active);
    TEST_ASSERT(safety.link_healthy);
    TEST_ASSERT_EQ(ROBOT_STATE_ESTOP, state.value);
    TEST_ASSERT_EQ(SAFETY_REASON_LINK_LOST, state.fault_code);
    TEST_ASSERT(robot_state_clear_estop(
        &state, safety.link_healthy, true, true));
    TEST_ASSERT_EQ(ROBOT_STATE_IDLE, state.value);
    TEST_ASSERT_EQ(0u, state.fault_code);
    return 0;
}

static int test_motion(void) {
    motion_service_t motion;
    output_capture_t output = {0};
    uint32_t now;

    TEST_ASSERT_EQ(0x01u, motion_halfstep_pattern(0));
    TEST_ASSERT_EQ(0x09u, motion_halfstep_pattern(7));
    motion_service_init(&motion, capture_output, &output);
    TEST_ASSERT(motion_service_start(&motion, 42u, 20, -10, 400u, 600u, 1000u, 0u));
    for (now = 0u; now < 1000u && motion.active; ++now) {
        motion_service_tick_1ms(&motion, now);
    }
    TEST_ASSERT_EQ(MOTION_RESULT_DONE, motion.result);
    TEST_ASSERT_EQ(20u, motion.left_done);
    TEST_ASSERT_EQ(10u, motion.right_done);
    TEST_ASSERT(output.calls > 0u);
    TEST_ASSERT_EQ(0u, output.left);
    TEST_ASSERT_EQ(0u, output.right);

    TEST_ASSERT(!motion_service_start(&motion, 1u, 1, 1, 801u, 600u, 100u, 0u));
    TEST_ASSERT(motion_service_start(&motion, 1u, 1000, 1000, 100u, 100u, 50u, 0u));
    motion_service_tick_1ms(&motion, 50u);
    TEST_ASSERT_EQ(MOTION_RESULT_TIMEOUT, motion.result);
    return 0;
}

static int test_encoder(void) {
    ec11_t encoder;
    ec11_event_t event = EC11_EVENT_NONE;
    bool long_seen = false;
    uint32_t index;
    static const uint8_t clockwise[] = {3u, 1u, 0u, 2u, 3u};

    ec11_init(&encoder, true, true, true);
    for (index = 1u; index < sizeof(clockwise); ++index) {
        uint8_t ab = clockwise[index];
        event = ec11_sample_1ms(&encoder, (ab & 2u) != 0u, (ab & 1u) != 0u, true);
    }
    TEST_ASSERT(event == EC11_EVENT_CLOCKWISE || event == EC11_EVENT_COUNTERCLOCKWISE);

    for (index = 0u; index < 25u; ++index) {
        event = ec11_sample_1ms(&encoder, true, true, false);
    }
    TEST_ASSERT_EQ(EC11_EVENT_NONE, event);
    for (index = 0u; index < 1500u; ++index) {
        event = ec11_sample_1ms(&encoder, true, true, false);
        if (event == EC11_EVENT_LONG_PRESS) {
            TEST_ASSERT(!long_seen);
            long_seen = true;
        }
    }
    TEST_ASSERT(long_seen);
    event = ec11_sample_1ms(&encoder, true, true, false);
    TEST_ASSERT_EQ(EC11_EVENT_NONE, event);
    return 0;
}

static int test_input_policy_without_button(void) {
    input_policy_t policy;
    input_policy_action_t action;

    input_policy_init(&policy, false, ROBOT_STATE_IDLE);

    action = input_policy_handle(&policy, EC11_EVENT_CLOCKWISE, ROBOT_STATE_IDLE);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_SET_MODE, action.type);
    TEST_ASSERT_EQ(ROBOT_STATE_MANUAL, action.mode);

    action = input_policy_handle(&policy, EC11_EVENT_CLOCKWISE, ROBOT_STATE_MANUAL);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_SET_MODE, action.type);
    TEST_ASSERT_EQ(ROBOT_STATE_AI, action.mode);

    action = input_policy_handle(&policy, EC11_EVENT_COUNTERCLOCKWISE, ROBOT_STATE_AI);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_SET_MODE, action.type);
    TEST_ASSERT_EQ(ROBOT_STATE_MANUAL, action.mode);

    action = input_policy_handle(&policy, EC11_EVENT_SHORT_PRESS, ROBOT_STATE_MANUAL);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_NONE, action.type);
    action = input_policy_handle(&policy, EC11_EVENT_LONG_PRESS, ROBOT_STATE_MANUAL);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_NONE, action.type);
    action = input_policy_handle(&policy, EC11_EVENT_CLOCKWISE, ROBOT_STATE_ESTOP);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_NONE, action.type);
    return 0;
}

static int test_input_policy_with_button(void) {
    input_policy_t policy;
    input_policy_action_t action;

    input_policy_init(&policy, true, ROBOT_STATE_IDLE);
    action = input_policy_handle(&policy, EC11_EVENT_CLOCKWISE, ROBOT_STATE_IDLE);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_NONE, action.type);
    TEST_ASSERT_EQ(ROBOT_STATE_MANUAL, policy.candidate_mode);

    action = input_policy_handle(&policy, EC11_EVENT_SHORT_PRESS, ROBOT_STATE_IDLE);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_SET_MODE, action.type);
    TEST_ASSERT_EQ(ROBOT_STATE_MANUAL, action.mode);

    input_policy_sync_mode(&policy, ROBOT_STATE_MANUAL);
    TEST_ASSERT_EQ(ROBOT_STATE_MANUAL, policy.candidate_mode);

    action = input_policy_handle(&policy, EC11_EVENT_LONG_PRESS, ROBOT_STATE_MANUAL);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_ESTOP, action.type);
    action = input_policy_handle(&policy, EC11_EVENT_SHORT_PRESS, ROBOT_STATE_ESTOP);
    TEST_ASSERT_EQ(INPUT_POLICY_ACTION_CLEAR_ESTOP, action.type);
    return 0;
}

static int test_session_peer_epoch(void) {
    session_peer_epoch_t epoch;

    session_peer_epoch_init(&epoch);
    TEST_ASSERT_EQ(SESSION_PEER_FIRST,
                   session_peer_epoch_observe(&epoch, 0u));
    TEST_ASSERT_EQ(SESSION_PEER_SAME,
                   session_peer_epoch_observe(&epoch, 0u));
    TEST_ASSERT_EQ(SESSION_PEER_CHANGED,
                   session_peer_epoch_observe(&epoch, 0x12345678u));
    TEST_ASSERT_EQ(SESSION_PEER_SAME,
                   session_peer_epoch_observe(&epoch, 0x12345678u));
    TEST_ASSERT_EQ(SESSION_PEER_CHANGED,
                   session_peer_epoch_observe(&epoch, 0u));
    return 0;
}

static int test_session_boot_counter_slots(void) {
    session_boot_record_t slot_a = {0};
    session_boot_record_t slot_b = {0};
    session_boot_record_t next_record;
    uint32_t decoded;
    uint32_t next;
    uint8_t target;

    next = session_boot_counter_advance(&slot_a, &slot_b, &target,
                                        &next_record);
    TEST_ASSERT_EQ(1u, next);
    TEST_ASSERT_EQ(SESSION_BOOT_SLOT_A, target);
    TEST_ASSERT(session_boot_record_decode(&next_record, &decoded));
    TEST_ASSERT_EQ(next, decoded);

    slot_a = next_record;
    next = session_boot_counter_advance(&slot_a, &slot_b, &target,
                                        &next_record);
    TEST_ASSERT_EQ(2u, next);
    TEST_ASSERT_EQ(SESSION_BOOT_SLOT_B, target);
    slot_b = next_record;

    next = session_boot_counter_advance(&slot_a, &slot_b, &target,
                                        &next_record);
    TEST_ASSERT_EQ(3u, next);
    TEST_ASSERT_EQ(SESSION_BOOT_SLOT_A, target);
    TEST_ASSERT(session_boot_record_decode(&next_record, &decoded));
    TEST_ASSERT_EQ(3u, decoded);

    slot_b.check ^= 1u;
    next = session_boot_counter_advance(&slot_a, &slot_b, &target,
                                        &next_record);
    TEST_ASSERT_EQ(2u, next);
    TEST_ASSERT_EQ(SESSION_BOOT_SLOT_B, target);

    session_boot_record_encode(UINT32_MAX, &slot_a);
    slot_b = (session_boot_record_t){0};
    next = session_boot_counter_advance(&slot_a, &slot_b, &target,
                                        &next_record);
    TEST_ASSERT_EQ(1u, next);
    TEST_ASSERT_EQ(SESSION_BOOT_SLOT_B, target);
    TEST_ASSERT(session_boot_record_decode(&next_record, &decoded));
    TEST_ASSERT_EQ(1u, decoded);
    return 0;
}

int main(void) {
    TEST_ASSERT_EQ(0, test_state_and_safety());
    TEST_ASSERT_EQ(0, test_motion());
    TEST_ASSERT_EQ(0, test_encoder());
    TEST_ASSERT_EQ(0, test_input_policy_without_button());
    TEST_ASSERT_EQ(0, test_input_policy_with_button());
    TEST_ASSERT_EQ(0, test_session_peer_epoch());
    TEST_ASSERT_EQ(0, test_session_boot_counter_slots());
    return 0;
}
