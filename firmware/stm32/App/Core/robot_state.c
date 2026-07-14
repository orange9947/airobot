#include "robot_state.h"

#include <stddef.h>

void robot_state_init(robot_state_t *state) {
    if (state == NULL) {
        return;
    }
    state->value = ROBOT_STATE_SELF_TEST;
    state->selected_mode = ROBOT_STATE_IDLE;
    state->degraded_flags = 0u;
    state->fault_code = 0u;
    state->transitions = 0u;
}

bool robot_state_finish_self_test(robot_state_t *state, bool safety_ok) {
    if (state == NULL || state->value != ROBOT_STATE_SELF_TEST) {
        return false;
    }
    state->value = safety_ok ? ROBOT_STATE_IDLE : ROBOT_STATE_FAULT;
    state->selected_mode = ROBOT_STATE_IDLE;
    if (!safety_ok && state->fault_code == 0u) {
        state->fault_code = 1u;
    }
    state->transitions++;
    return safety_ok;
}

bool robot_state_request_mode(robot_state_t *state, robot_state_value_t mode) {
    if (state == NULL || (mode != ROBOT_STATE_IDLE && mode != ROBOT_STATE_MANUAL &&
                          mode != ROBOT_STATE_AI)) {
        return false;
    }
    if (state->value == ROBOT_STATE_ESTOP || state->value == ROBOT_STATE_FAULT ||
        state->value == ROBOT_STATE_SELF_TEST) {
        return false;
    }
    state->value = mode;
    state->selected_mode = mode;
    state->transitions++;
    return true;
}

void robot_state_enter_estop(robot_state_t *state, uint16_t reason) {
    if (state == NULL || state->value == ROBOT_STATE_FAULT) {
        return;
    }
    state->value = ROBOT_STATE_ESTOP;
    state->selected_mode = ROBOT_STATE_IDLE;
    state->fault_code = reason;
    state->transitions++;
}

bool robot_state_clear_estop(robot_state_t *state, bool link_healthy,
                             bool button_released, bool operator_confirm) {
    if (state == NULL || state->value != ROBOT_STATE_ESTOP || !link_healthy ||
        !button_released || !operator_confirm) {
        return false;
    }
    state->value = ROBOT_STATE_IDLE;
    state->selected_mode = ROBOT_STATE_IDLE;
    state->fault_code = 0u;
    state->transitions++;
    return true;
}

void robot_state_enter_fault(robot_state_t *state, uint16_t fault_code) {
    if (state == NULL) {
        return;
    }
    state->value = ROBOT_STATE_FAULT;
    state->selected_mode = ROBOT_STATE_IDLE;
    state->fault_code = fault_code;
    state->transitions++;
}

void robot_state_set_degraded(robot_state_t *state, uint16_t flag, bool active) {
    if (state == NULL) {
        return;
    }
    if (active) {
        state->degraded_flags |= flag;
    } else {
        state->degraded_flags &= (uint16_t)~flag;
    }
}
