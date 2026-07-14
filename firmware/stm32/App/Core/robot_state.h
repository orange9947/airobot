#ifndef AIROBOT_STATE_H
#define AIROBOT_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ROBOT_STATE_BOOT = 0,
    ROBOT_STATE_SELF_TEST = 1,
    ROBOT_STATE_IDLE = 2,
    ROBOT_STATE_MANUAL = 3,
    ROBOT_STATE_AI = 4,
    ROBOT_STATE_ESTOP = 5,
    ROBOT_STATE_FAULT = 6,
} robot_state_value_t;

typedef struct {
    robot_state_value_t value;
    robot_state_value_t selected_mode;
    uint16_t degraded_flags;
    uint16_t fault_code;
    uint32_t transitions;
} robot_state_t;

void robot_state_init(robot_state_t *state);
bool robot_state_finish_self_test(robot_state_t *state, bool safety_ok);
bool robot_state_request_mode(robot_state_t *state, robot_state_value_t mode);
void robot_state_enter_estop(robot_state_t *state, uint16_t reason);
bool robot_state_clear_estop(robot_state_t *state, bool link_healthy,
                             bool button_released, bool operator_confirm);
void robot_state_enter_fault(robot_state_t *state, uint16_t fault_code);
void robot_state_set_degraded(robot_state_t *state, uint16_t flag, bool active);

#endif
