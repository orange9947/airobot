#ifndef AIROBOT_FACE_ANIMATOR_H
#define AIROBOT_FACE_ANIMATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool blinking;
    int8_t gaze;
    uint8_t mouth_variant;
} face_pose_t;

typedef struct {
    uint8_t expression;
    uint32_t random_state;
    face_pose_t pose;
    uint32_t next_blink_ms;
    uint32_t blink_end_ms;
    uint32_t next_gaze_ms;
    uint32_t next_mouth_ms;
} face_animator_t;

void face_animator_init(face_animator_t *animator, uint32_t seed,
                        uint8_t expression, uint32_t now_ms);
bool face_animator_set_expression(face_animator_t *animator,
                                  uint8_t expression, uint32_t now_ms);
bool face_animator_tick(face_animator_t *animator, uint32_t now_ms);

#endif
