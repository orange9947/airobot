#include "controller_session_policy.h"

#include "protocol_ids.h"

controller_session_route_t controller_session_route_policy(
    bool session_active, uint16_t message_type) {
    if (session_active) {
        return CONTROLLER_SESSION_ROUTE_ALLOW;
    }

    switch (message_type) {
        case ROBOT_MSG_SET_MODE:
        case ROBOT_MSG_MOVE_WHEELS:
        case ROBOT_MSG_STOP:
        case ROBOT_MSG_SET_EXPRESSION:
        case ROBOT_MSG_SET_RUNTIME_CONFIG:
        case ROBOT_MSG_CLEAR_ESTOP:
        case ROBOT_MSG_RESOURCE_BEGIN:
        case ROBOT_MSG_RESOURCE_CHUNK:
        case ROBOT_MSG_RESOURCE_FINISH:
        case ROBOT_MSG_RESOURCE_ABORT:
        case ROBOT_MSG_GET_RESOURCE_STATUS:
            return CONTROLLER_SESSION_ROUTE_REJECT_BAD_STATE;
        default:
            return CONTROLLER_SESSION_ROUTE_ALLOW;
    }
}
