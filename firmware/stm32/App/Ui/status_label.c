#include "status_label.h"

#include <stddef.h>

#include "protocol_ids.h"

#define DISPLAY_WIDTH 128u
#define LABEL_RIGHT_MARGIN 2u
#define LABEL_Y 2u

bool status_label_for_state(uint8_t state, status_label_layout_t *layout) {
    const char *text = "IDLE";
    uint8_t characters = 4u;

    if (layout == NULL) {
        return false;
    }
    switch (state) {
        case ROBOT_ROBOTSTATE_MANUAL:
            text = "MAN";
            characters = 3u;
            break;
        case ROBOT_ROBOTSTATE_AI:
            text = "AI";
            characters = 2u;
            break;
        case ROBOT_ROBOTSTATE_ESTOP:
            text = "STOP";
            characters = 4u;
            break;
        case ROBOT_ROBOTSTATE_FAULT:
            text = "ERR";
            characters = 3u;
            break;
        default:
            break;
    }
    layout->text = text;
    layout->width = (uint8_t)(characters * 4u - 1u);
    layout->x = (uint8_t)(DISPLAY_WIDTH - LABEL_RIGHT_MARGIN - layout->width);
    layout->y = LABEL_Y;
    return true;
}
