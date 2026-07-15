#include "motion_service.h"

#include <stddef.h>

static const uint8_t HALFSTEP_PATTERNS[8] = {
    0x01u, 0x03u, 0x02u, 0x06u, 0x04u, 0x0Cu, 0x08u, 0x09u,
};

static uint32_t abs_steps(int32_t value) {
    return value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
}

uint8_t motion_halfstep_pattern(uint8_t phase) {
    return HALFSTEP_PATTERNS[phase & 0x07u];
}

void motion_service_init(motion_service_t *motion, motion_apply_fn apply, void *context) {
    if (motion == NULL) {
        return;
    }
    *motion = (motion_service_t){0};
    motion->apply = apply;
    motion->apply_context = context;
}

bool motion_service_start(motion_service_t *motion, uint32_t command_id,
                          int32_t left_steps, int32_t right_steps,
                          uint16_t max_rate_sps, uint16_t accel_sps2,
                          uint16_t timeout_ms, uint32_t now_ms) {
    uint32_t left_abs;
    uint32_t right_abs;

    if (motion == NULL || motion->active || (left_steps == 0 && right_steps == 0) ||
        max_rate_sps == 0u || max_rate_sps > MOTION_HARD_MAX_RATE_SPS ||
        accel_sps2 == 0u || accel_sps2 > MOTION_HARD_MAX_ACCEL_SPS2 ||
        timeout_ms == 0u || timeout_ms > MOTION_HARD_MAX_TIMEOUT_MS) {
        return false;
    }
    left_abs = abs_steps(left_steps);
    right_abs = abs_steps(right_steps);
    motion->active = true;
    motion->command_id = command_id;
    motion->left_target = left_steps;
    motion->right_target = right_steps;
    motion->left_done = 0u;
    motion->right_done = 0u;
    motion->max_steps = left_abs > right_abs ? left_abs : right_abs;
    motion->left_dda = 0u;
    motion->right_dda = 0u;
    motion->rate_accumulator = 0u;
    motion->accel_accumulator = 0u;
    motion->current_rate_sps = 1u;
    motion->max_rate_sps = max_rate_sps;
    motion->accel_sps2 = accel_sps2;
    motion->timeout_ms = timeout_ms;
    motion->started_ms = now_ms;
    motion->result = MOTION_RESULT_NONE;
    return true;
}

void motion_service_abort(motion_service_t *motion, motion_result_t result) {
    if (motion == NULL) {
        return;
    }
    motion->active = false;
    motion->result = result;
    if (motion->apply != NULL) {
        motion->apply(0u, 0u, motion->apply_context);
    }
}

static void step_phase(uint8_t *phase, int32_t target) {
    if (target < 0) {
        *phase = (uint8_t)((*phase - 1u) & 0x07u);
    } else {
        *phase = (uint8_t)((*phase + 1u) & 0x07u);
    }
}

static uint32_t braking_steps(const motion_service_t *motion) {
    uint32_t rate = motion->current_rate_sps;
    return (rate * rate) / (2u * motion->accel_sps2) + 1u;
}

void motion_service_tick_1ms(motion_service_t *motion, uint32_t now_ms) {
    uint32_t remaining;
    uint32_t left_abs;
    uint32_t right_abs;
    uint8_t left_pattern;
    uint8_t right_pattern;

    if (motion == NULL || !motion->active) {
        return;
    }
    if ((uint32_t)(now_ms - motion->started_ms) >= motion->timeout_ms) {
        motion_service_abort(motion, MOTION_RESULT_TIMEOUT);
        return;
    }

    remaining = motion->max_steps - (motion->left_done > motion->right_done
                                         ? motion->left_done
                                         : motion->right_done);
    motion->accel_accumulator += motion->accel_sps2;
    while (motion->accel_accumulator >= 1000u) {
        motion->accel_accumulator -= 1000u;
        if (remaining <= braking_steps(motion)) {
            if (motion->current_rate_sps > 1u) {
                motion->current_rate_sps--;
            }
        } else if (motion->current_rate_sps < motion->max_rate_sps) {
            motion->current_rate_sps++;
        }
    }

    motion->rate_accumulator += motion->current_rate_sps;
    if (motion->rate_accumulator < 1000u) {
        return;
    }
    motion->rate_accumulator -= 1000u;

    left_abs = abs_steps(motion->left_target);
    right_abs = abs_steps(motion->right_target);
    motion->left_dda += left_abs;
    motion->right_dda += right_abs;
    if (motion->left_done < left_abs && motion->left_dda >= motion->max_steps) {
        motion->left_dda -= motion->max_steps;
        motion->left_done++;
        step_phase(&motion->left_phase, motion->left_target);
    }
    if (motion->right_done < right_abs && motion->right_dda >= motion->max_steps) {
        motion->right_dda -= motion->max_steps;
        motion->right_done++;
        step_phase(&motion->right_phase, motion->right_target);
    }
    left_pattern = left_abs == 0u ? 0u : motion_halfstep_pattern(motion->left_phase);
    right_pattern = right_abs == 0u ? 0u : motion_halfstep_pattern(motion->right_phase);
    if (motion->apply != NULL) {
        motion->apply(left_pattern, right_pattern, motion->apply_context);
    }
    if (motion->left_done >= left_abs && motion->right_done >= right_abs) {
        motion_service_abort(motion, MOTION_RESULT_DONE);
    }
}
