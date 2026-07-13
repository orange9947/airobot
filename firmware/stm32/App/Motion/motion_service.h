#ifndef AIROBOT_MOTION_SERVICE_H
#define AIROBOT_MOTION_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define MOTION_HARD_MAX_RATE_SPS 800u
#define MOTION_HARD_MAX_ACCEL_SPS2 1200u
#define MOTION_HARD_MAX_TIMEOUT_MS 2000u

typedef void (*motion_apply_fn)(uint8_t left_pattern, uint8_t right_pattern, void *context);

typedef enum {
    MOTION_RESULT_NONE = 0,
    MOTION_RESULT_DONE,
    MOTION_RESULT_ABORTED,
    MOTION_RESULT_TIMEOUT,
} motion_result_t;

typedef struct {
    bool active;
    uint32_t command_id;
    int32_t left_target;
    int32_t right_target;
    uint32_t left_done;
    uint32_t right_done;
    uint32_t max_steps;
    uint32_t left_dda;
    uint32_t right_dda;
    uint32_t rate_accumulator;
    uint32_t accel_accumulator;
    uint16_t current_rate_sps;
    uint16_t max_rate_sps;
    uint16_t accel_sps2;
    uint16_t timeout_ms;
    uint32_t started_ms;
    uint8_t left_phase;
    uint8_t right_phase;
    motion_result_t result;
    motion_apply_fn apply;
    void *apply_context;
} motion_service_t;

void motion_service_init(motion_service_t *motion, motion_apply_fn apply, void *context);
bool motion_service_start(motion_service_t *motion, uint32_t command_id,
                          int32_t left_steps, int32_t right_steps,
                          uint16_t max_rate_sps, uint16_t accel_sps2,
                          uint16_t timeout_ms, uint32_t now_ms);
void motion_service_tick_1ms(motion_service_t *motion, uint32_t now_ms);
void motion_service_abort(motion_service_t *motion, motion_result_t result);
uint8_t motion_halfstep_pattern(uint8_t phase);

#endif
