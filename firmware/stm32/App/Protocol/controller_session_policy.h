#ifndef AIROBOT_CONTROLLER_SESSION_POLICY_H
#define AIROBOT_CONTROLLER_SESSION_POLICY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CONTROLLER_SESSION_ROUTE_ALLOW = 0,
    CONTROLLER_SESSION_ROUTE_REJECT_BAD_STATE,
} controller_session_route_t;

controller_session_route_t controller_session_route_policy(
    bool session_active, uint16_t message_type);

#endif
