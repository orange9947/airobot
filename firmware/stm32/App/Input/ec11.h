#ifndef AIROBOT_EC11_H
#define AIROBOT_EC11_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EC11_EVENT_NONE = 0,
    EC11_EVENT_CLOCKWISE,
    EC11_EVENT_COUNTERCLOCKWISE,
    EC11_EVENT_SHORT_PRESS,
    EC11_EVENT_LONG_PRESS,
} ec11_event_t;

typedef struct {
    uint8_t previous_ab;
    int8_t quarter_steps;
    bool raw_pressed;
    bool stable_pressed;
    bool long_reported;
    uint16_t debounce_ms;
    uint32_t pressed_ms;
} ec11_t;

void ec11_init(ec11_t *encoder, bool a_high, bool b_high, bool switch_high);
ec11_event_t ec11_sample_1ms(ec11_t *encoder, bool a_high, bool b_high,
                             bool switch_high);

#endif
