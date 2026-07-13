#include "ec11.h"

#include <stddef.h>

#define EC11_DEBOUNCE_MS 20u
#define EC11_LONG_PRESS_MS 1500u

static const int8_t QUADRATURE[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

void ec11_init(ec11_t *encoder, bool a_high, bool b_high, bool switch_high) {
    if (encoder == NULL) {
        return;
    }
    encoder->previous_ab = (uint8_t)((a_high ? 2u : 0u) | (b_high ? 1u : 0u));
    encoder->quarter_steps = 0;
    encoder->raw_pressed = !switch_high;
    encoder->stable_pressed = !switch_high;
    encoder->long_reported = false;
    encoder->debounce_ms = 0u;
    encoder->pressed_ms = 0u;
}

ec11_event_t ec11_sample_1ms(ec11_t *encoder, bool a_high, bool b_high,
                             bool switch_high) {
    uint8_t current_ab;
    int8_t delta;
    bool pressed;
    ec11_event_t event = EC11_EVENT_NONE;

    if (encoder == NULL) {
        return EC11_EVENT_NONE;
    }
    current_ab = (uint8_t)((a_high ? 2u : 0u) | (b_high ? 1u : 0u));
    delta = QUADRATURE[(encoder->previous_ab << 2u) | current_ab];
    encoder->previous_ab = current_ab;
    encoder->quarter_steps += delta;
    if (encoder->quarter_steps >= 4) {
        encoder->quarter_steps = 0;
        event = EC11_EVENT_CLOCKWISE;
    } else if (encoder->quarter_steps <= -4) {
        encoder->quarter_steps = 0;
        event = EC11_EVENT_COUNTERCLOCKWISE;
    }

    pressed = !switch_high;
    if (pressed != encoder->raw_pressed) {
        encoder->raw_pressed = pressed;
        encoder->debounce_ms = 0u;
    } else if (encoder->debounce_ms < EC11_DEBOUNCE_MS) {
        encoder->debounce_ms++;
    }
    if (encoder->debounce_ms == EC11_DEBOUNCE_MS && pressed != encoder->stable_pressed) {
        encoder->stable_pressed = pressed;
        if (pressed) {
            encoder->pressed_ms = 0u;
            encoder->long_reported = false;
        } else if (!encoder->long_reported) {
            event = EC11_EVENT_SHORT_PRESS;
        }
    }
    if (encoder->stable_pressed && !encoder->long_reported) {
        if (encoder->pressed_ms < UINT32_MAX) {
            encoder->pressed_ms++;
        }
        if (encoder->pressed_ms >= EC11_LONG_PRESS_MS) {
            encoder->long_reported = true;
            event = EC11_EVENT_LONG_PRESS;
        }
    }
    return event;
}
