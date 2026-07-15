#ifndef AIROBOT_COIL_DIAGNOSTIC_SERVICE_H
#define AIROBOT_COIL_DIAGNOSTIC_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define COIL_DIAGNOSTIC_MIN_DURATION_MS 100u
#define COIL_DIAGNOSTIC_MAX_DURATION_MS 3000u

typedef void (*coil_diagnostic_apply_fn)(uint8_t left_pattern,
                                         uint8_t right_pattern,
                                         void *context);

typedef enum {
    COIL_DIAGNOSTIC_WHEEL_LEFT = 0,
    COIL_DIAGNOSTIC_WHEEL_RIGHT = 1,
} coil_diagnostic_wheel_t;

typedef enum {
    COIL_DIAGNOSTIC_CHANNEL_A = 0,
    COIL_DIAGNOSTIC_CHANNEL_B = 1,
    COIL_DIAGNOSTIC_CHANNEL_C = 2,
    COIL_DIAGNOSTIC_CHANNEL_D = 3,
} coil_diagnostic_channel_t;

typedef enum {
    COIL_DIAGNOSTIC_RESULT_NONE = 0,
    COIL_DIAGNOSTIC_RESULT_DONE,
    COIL_DIAGNOSTIC_RESULT_ABORTED,
} coil_diagnostic_result_t;

typedef struct {
    bool active;
    uint32_t command_id;
    uint32_t started_ms;
    uint16_t duration_ms;
    coil_diagnostic_wheel_t wheel;
    coil_diagnostic_channel_t channel;
    coil_diagnostic_result_t result;
    coil_diagnostic_apply_fn apply;
    void *apply_context;
} coil_diagnostic_service_t;

void coil_diagnostic_service_init(coil_diagnostic_service_t *service,
                                  coil_diagnostic_apply_fn apply,
                                  void *context);
bool coil_diagnostic_service_start(coil_diagnostic_service_t *service,
                                   uint32_t command_id,
                                   coil_diagnostic_wheel_t wheel,
                                   coil_diagnostic_channel_t channel,
                                   uint16_t duration_ms,
                                   uint32_t now_ms);
void coil_diagnostic_service_tick_1ms(coil_diagnostic_service_t *service,
                                      uint32_t now_ms);
void coil_diagnostic_service_abort(coil_diagnostic_service_t *service,
                                   coil_diagnostic_result_t result);

#endif
