#include "safety_supervisor.h"

#include <stddef.h>

void safety_supervisor_init(safety_supervisor_t *supervisor) {
    if (supervisor == NULL) {
        return;
    }
    supervisor->session_active = false;
    supervisor->link_healthy = false;
    supervisor->last_valid_slot_ms = 0u;
    supervisor->timeout_count = 0u;
}

void safety_supervisor_session_started(safety_supervisor_t *supervisor, uint32_t now_ms) {
    if (supervisor == NULL) {
        return;
    }
    supervisor->session_active = true;
    supervisor->link_healthy = true;
    supervisor->last_valid_slot_ms = now_ms;
}

void safety_supervisor_valid_slot(safety_supervisor_t *supervisor, uint32_t now_ms) {
    if (supervisor == NULL) {
        return;
    }
    supervisor->session_active = true;
    supervisor->link_healthy = true;
    supervisor->last_valid_slot_ms = now_ms;
}

bool safety_supervisor_tick(safety_supervisor_t *supervisor, robot_state_t *state,
                            uint32_t now_ms) {
    if (supervisor == NULL || state == NULL || !supervisor->session_active ||
        !supervisor->link_healthy) {
        return false;
    }
    if ((uint32_t)(now_ms - supervisor->last_valid_slot_ms) <= SAFETY_HEARTBEAT_TIMEOUT_MS) {
        return false;
    }
    supervisor->link_healthy = false;
    supervisor->session_active = false;
    supervisor->timeout_count++;
    robot_state_enter_estop(state, SAFETY_REASON_LINK_LOST);
    return true;
}

void safety_supervisor_stop(safety_supervisor_t *supervisor, robot_state_t *state,
                            uint16_t reason) {
    (void)supervisor;
    robot_state_enter_estop(state, reason);
}
