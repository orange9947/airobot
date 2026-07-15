#include "resource_policy.h"

bool resource_policy_update_active(storage_service_state_t state) {
    return state >= STORAGE_SERVICE_ERASING &&
           state <= STORAGE_SERVICE_FAILED;
}

bool resource_policy_can_begin(robot_state_value_t robot_state,
                               storage_service_state_t storage_state,
                               bool link_healthy, bool motion_active) {
    return link_healthy && !motion_active &&
           storage_state == STORAGE_SERVICE_IDLE &&
           (robot_state == ROBOT_STATE_IDLE ||
            robot_state == ROBOT_STATE_ESTOP);
}

bool resource_policy_allows_mode(storage_service_state_t storage_state,
                                 robot_state_value_t requested_mode) {
    return !resource_policy_update_active(storage_state) ||
           requested_mode == ROBOT_STATE_IDLE;
}

bool resource_policy_allows_move(storage_service_state_t storage_state) {
    return !resource_policy_update_active(storage_state);
}
