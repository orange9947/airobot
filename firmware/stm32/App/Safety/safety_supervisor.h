#ifndef AIROBOT_SAFETY_SUPERVISOR_H
#define AIROBOT_SAFETY_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>

#include "robot_state.h"

#define SAFETY_HEARTBEAT_TIMEOUT_MS 750u
#define SAFETY_REASON_LINK_LOST 8u
#define SAFETY_REASON_LOCAL_STOP 10u
#define SAFETY_REASON_REMOTE_STOP 11u

typedef struct {
    bool session_active;
    bool link_healthy;
    uint32_t last_valid_slot_ms;
    uint32_t timeout_count;
} safety_supervisor_t;

void safety_supervisor_init(safety_supervisor_t *supervisor);
void safety_supervisor_session_started(safety_supervisor_t *supervisor, uint32_t now_ms);
void safety_supervisor_valid_slot(safety_supervisor_t *supervisor, uint32_t now_ms);
bool safety_supervisor_tick(safety_supervisor_t *supervisor, robot_state_t *state,
                            uint32_t now_ms);
void safety_supervisor_stop(safety_supervisor_t *supervisor, robot_state_t *state,
                            uint16_t reason);

#endif
