#include "input_policy.h"

#include <stddef.h>

static bool is_mode(robot_state_value_t state) {
    return state == ROBOT_STATE_IDLE || state == ROBOT_STATE_MANUAL ||
           state == ROBOT_STATE_AI;
}

static robot_state_value_t rotate_mode(robot_state_value_t mode, bool clockwise) {
    if (clockwise) {
        return mode == ROBOT_STATE_IDLE ? ROBOT_STATE_MANUAL
             : mode == ROBOT_STATE_MANUAL ? ROBOT_STATE_AI
                                           : ROBOT_STATE_IDLE;
    }
    return mode == ROBOT_STATE_IDLE ? ROBOT_STATE_AI
         : mode == ROBOT_STATE_AI ? ROBOT_STATE_MANUAL
                                  : ROBOT_STATE_IDLE;
}

static input_policy_action_t action(input_policy_action_type_t type,
                                    robot_state_value_t mode) {
    input_policy_action_t result = {type, mode};
    return result;
}

void input_policy_init(input_policy_t *policy, bool button_present,
                       robot_state_value_t initial_mode) {
    if (policy == NULL) {
        return;
    }
    policy->button_present = button_present;
    policy->candidate_mode = is_mode(initial_mode) ? initial_mode : ROBOT_STATE_IDLE;
}

void input_policy_sync_mode(input_policy_t *policy, robot_state_value_t mode) {
    if (policy != NULL && is_mode(mode)) {
        policy->candidate_mode = mode;
    }
}

input_policy_action_t input_policy_handle(input_policy_t *policy, ec11_event_t event,
                                          robot_state_value_t current_state) {
    robot_state_value_t base_mode;
    if (policy == NULL) {
        return action(INPUT_POLICY_ACTION_NONE, ROBOT_STATE_IDLE);
    }

    if (event == EC11_EVENT_CLOCKWISE || event == EC11_EVENT_COUNTERCLOCKWISE) {
        if (!is_mode(current_state)) {
            return action(INPUT_POLICY_ACTION_NONE, policy->candidate_mode);
        }
        base_mode = policy->button_present ? policy->candidate_mode : current_state;
        if (!is_mode(base_mode)) {
            base_mode = current_state;
        }
        policy->candidate_mode = rotate_mode(
            base_mode, event == EC11_EVENT_CLOCKWISE);
        if (!policy->button_present) {
            return action(INPUT_POLICY_ACTION_SET_MODE, policy->candidate_mode);
        }
        return action(INPUT_POLICY_ACTION_NONE, policy->candidate_mode);
    }

    if (!policy->button_present) {
        return action(INPUT_POLICY_ACTION_NONE, policy->candidate_mode);
    }
    if (event == EC11_EVENT_LONG_PRESS && current_state != ROBOT_STATE_ESTOP) {
        return action(INPUT_POLICY_ACTION_ESTOP, policy->candidate_mode);
    }
    if (event == EC11_EVENT_SHORT_PRESS) {
        if (current_state == ROBOT_STATE_ESTOP) {
            return action(INPUT_POLICY_ACTION_CLEAR_ESTOP, policy->candidate_mode);
        }
        if (is_mode(current_state)) {
            return action(INPUT_POLICY_ACTION_SET_MODE, policy->candidate_mode);
        }
    }
    return action(INPUT_POLICY_ACTION_NONE, policy->candidate_mode);
}
