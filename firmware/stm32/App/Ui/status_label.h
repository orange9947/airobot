#ifndef AIROBOT_STATUS_LABEL_H
#define AIROBOT_STATUS_LABEL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *text;
    uint8_t x;
    uint8_t y;
    uint8_t width;
} status_label_layout_t;

bool status_label_for_state(uint8_t state, status_label_layout_t *layout);

#endif
