#ifndef AIROBOT_RESOURCE_POLICY_H
#define AIROBOT_RESOURCE_POLICY_H

#include <stdbool.h>

#include "robot_state.h"
#include "storage_service.h"

bool resource_policy_update_active(storage_service_state_t state);
bool resource_policy_can_begin(robot_state_value_t robot_state,
                               storage_service_state_t storage_state,
                               bool link_healthy, bool motor_outputs_active);
bool resource_policy_allows_mode(storage_service_state_t storage_state,
                                 robot_state_value_t requested_mode);
bool resource_policy_allows_move(storage_service_state_t storage_state);

#endif
