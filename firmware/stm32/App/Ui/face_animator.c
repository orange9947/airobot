#include "face_animator.h"

#include <stddef.h>

#include "protocol_ids.h"

#define BLINK_INTERVAL_MIN_MS 2000u
#define BLINK_INTERVAL_SPAN_MS 4001u
#define BLINK_DURATION_MIN_MS 160u
#define BLINK_DURATION_SPAN_MS 81u
#define GAZE_INTERVAL_MIN_MS 3000u
#define GAZE_INTERVAL_SPAN_MS 4001u
#define MOUTH_INTERVAL_MIN_MS 2500u
#define MOUTH_INTERVAL_SPAN_MS 2501u

static uint32_t next_random(face_animator_t *animator) {
    uint32_t value = animator->random_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    animator->random_state = value;
    return value;
}

static bool time_reached(uint32_t now_ms, uint32_t target_ms) {
    return (int32_t)(now_ms - target_ms) >= 0;
}

static bool is_safety_expression(uint8_t expression) {
    return expression == ROBOT_EXPRESSION_ESTOP || expression == ROBOT_EXPRESSION_FAULT;
}

static void schedule_events(face_animator_t *animator, uint32_t now_ms) {
    animator->next_blink_ms = now_ms + BLINK_INTERVAL_MIN_MS +
                              next_random(animator) % BLINK_INTERVAL_SPAN_MS;
    animator->next_gaze_ms = now_ms + GAZE_INTERVAL_MIN_MS +
                             next_random(animator) % GAZE_INTERVAL_SPAN_MS;
    animator->next_mouth_ms = now_ms + MOUTH_INTERVAL_MIN_MS +
                              next_random(animator) % MOUTH_INTERVAL_SPAN_MS;
}

static void reset_pose(face_animator_t *animator, uint32_t now_ms) {
    animator->pose.blinking = false;
    animator->pose.gaze = (int8_t)(next_random(animator) % 3u) - 1;
    animator->pose.mouth_variant = (uint8_t)(next_random(animator) % 3u);
    animator->blink_end_ms = now_ms;
    schedule_events(animator, now_ms);
}

void face_animator_init(face_animator_t *animator, uint32_t seed,
                        uint8_t expression, uint32_t now_ms) {
    if (animator == NULL) {
        return;
    }
    animator->random_state = seed != 0u ? seed : 0xA17E5EEDu;
    animator->expression = expression <= ROBOT_EXPRESSION_FAULT
                               ? expression
                               : ROBOT_EXPRESSION_NEUTRAL;
    reset_pose(animator, now_ms);
}

bool face_animator_set_expression(face_animator_t *animator,
                                  uint8_t expression, uint32_t now_ms) {
    if (animator == NULL || expression > ROBOT_EXPRESSION_FAULT) {
        return false;
    }
    animator->expression = expression;
    reset_pose(animator, now_ms);
    return true;
}

bool face_animator_tick(face_animator_t *animator, uint32_t now_ms) {
    bool changed = false;
    uint8_t index;

    if (animator == NULL || is_safety_expression(animator->expression)) {
        return false;
    }
    if (animator->pose.blinking) {
        if (time_reached(now_ms, animator->blink_end_ms)) {
            animator->pose.blinking = false;
            animator->next_blink_ms = now_ms + BLINK_INTERVAL_MIN_MS +
                                      next_random(animator) % BLINK_INTERVAL_SPAN_MS;
            changed = true;
        }
    } else if (time_reached(now_ms, animator->next_blink_ms)) {
        animator->pose.blinking = true;
        animator->blink_end_ms = now_ms + BLINK_DURATION_MIN_MS +
                                 next_random(animator) % BLINK_DURATION_SPAN_MS;
        changed = true;
    }
    if (time_reached(now_ms, animator->next_gaze_ms)) {
        index = (uint8_t)(animator->pose.gaze + 1);
        index = (uint8_t)((index + 1u + next_random(animator) % 2u) % 3u);
        animator->pose.gaze = (int8_t)index - 1;
        animator->next_gaze_ms = now_ms + GAZE_INTERVAL_MIN_MS +
                                 next_random(animator) % GAZE_INTERVAL_SPAN_MS;
        changed = true;
    }
    if (time_reached(now_ms, animator->next_mouth_ms)) {
        animator->pose.mouth_variant = (uint8_t)(
            (animator->pose.mouth_variant + 1u + next_random(animator) % 2u) % 3u);
        animator->next_mouth_ms = now_ms + MOUTH_INTERVAL_MIN_MS +
                                  next_random(animator) % MOUTH_INTERVAL_SPAN_MS;
        changed = true;
    }
    return changed;
}
