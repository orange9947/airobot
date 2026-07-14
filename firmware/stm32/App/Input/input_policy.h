#ifndef AIROBOT_INPUT_POLICY_H
#define AIROBOT_INPUT_POLICY_H

#include <stdbool.h>

#include "ec11.h"
#include "robot_state.h"

typedef enum {
    INPUT_POLICY_ACTION_NONE = 0,
    INPUT_POLICY_ACTION_SET_MODE,
    INPUT_POLICY_ACTION_ESTOP,
    INPUT_POLICY_ACTION_CLEAR_ESTOP,
} input_policy_action_type_t;

typedef struct {
    input_policy_action_type_t type;
    robot_state_value_t mode;
} input_policy_action_t;

typedef struct {
    bool button_present;
    robot_state_value_t candidate_mode;
} input_policy_t;

void input_policy_init(input_policy_t *policy, bool button_present,
                       robot_state_value_t initial_mode);
void input_policy_sync_mode(input_policy_t *policy, robot_state_value_t mode);
input_policy_action_t input_policy_handle(input_policy_t *policy, ec11_event_t event,
                                          robot_state_value_t current_state);

#endif
