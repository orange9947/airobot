#include "uln2003_hw.h"

#include "board_pins.h"

static uint32_t pattern_bsrr(uint8_t pattern, const uint16_t pins[4]) {
    uint32_t set_bits = 0u;
    uint32_t reset_bits = 0u;
    uint8_t index;
    for (index = 0u; index < 4u; ++index) {
        if ((pattern & (1u << index)) != 0u) {
            set_bits |= pins[index];
        } else {
            reset_bits |= pins[index];
        }
    }
    return set_bits | (reset_bits << 16u);
}

void uln2003_hw_apply(uint8_t left_pattern, uint8_t right_pattern, void *context) {
    static const uint16_t left_pins[4] = {
        MOTOR_LEFT_IN1, MOTOR_LEFT_IN2, MOTOR_LEFT_IN3, MOTOR_LEFT_IN4,
    };
    static const uint16_t right_pins[4] = {
        MOTOR_RIGHT_IN1, MOTOR_RIGHT_IN2, MOTOR_RIGHT_IN3, MOTOR_RIGHT_IN4,
    };
    (void)context;
    MOTOR_LEFT_PORT->BSRR = pattern_bsrr(left_pattern, left_pins);
    MOTOR_RIGHT_PORT->BSRR = pattern_bsrr(right_pattern, right_pins);
}

void uln2003_hw_off(void) {
    uln2003_hw_apply(0u, 0u, NULL);
}
