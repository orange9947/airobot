#include "coil_diagnostic_service.h"

#include <stddef.h>

void coil_diagnostic_service_init(coil_diagnostic_service_t *service,
                                  coil_diagnostic_apply_fn apply,
                                  void *context) {
    if (service == NULL) {
        return;
    }
    *service = (coil_diagnostic_service_t){0};
    service->apply = apply;
    service->apply_context = context;
}

bool coil_diagnostic_service_start(coil_diagnostic_service_t *service,
                                   uint32_t command_id,
                                   coil_diagnostic_wheel_t wheel,
                                   coil_diagnostic_channel_t channel,
                                   uint16_t duration_ms,
                                   uint32_t now_ms) {
    uint8_t pattern;

    if (service == NULL || service->apply == NULL || service->active ||
        (wheel != COIL_DIAGNOSTIC_WHEEL_LEFT &&
         wheel != COIL_DIAGNOSTIC_WHEEL_RIGHT) ||
        channel > COIL_DIAGNOSTIC_CHANNEL_D ||
        duration_ms < COIL_DIAGNOSTIC_MIN_DURATION_MS ||
        duration_ms > COIL_DIAGNOSTIC_MAX_DURATION_MS) {
        return false;
    }

    service->active = true;
    service->command_id = command_id;
    service->started_ms = now_ms;
    service->duration_ms = duration_ms;
    service->wheel = wheel;
    service->channel = channel;
    service->result = COIL_DIAGNOSTIC_RESULT_NONE;
    pattern = (uint8_t)(1u << (uint8_t)channel);
    service->apply(wheel == COIL_DIAGNOSTIC_WHEEL_LEFT ? pattern : 0u,
                   wheel == COIL_DIAGNOSTIC_WHEEL_RIGHT ? pattern : 0u,
                   service->apply_context);
    return true;
}

void coil_diagnostic_service_abort(coil_diagnostic_service_t *service,
                                   coil_diagnostic_result_t result) {
    if (service == NULL || !service->active) {
        return;
    }
    service->active = false;
    service->apply(0u, 0u, service->apply_context);
    service->result = result;
}

void coil_diagnostic_service_tick_1ms(coil_diagnostic_service_t *service,
                                      uint32_t now_ms) {
    if (service == NULL || !service->active) {
        return;
    }
    if ((uint32_t)(now_ms - service->started_ms) >= service->duration_ms) {
        coil_diagnostic_service_abort(service, COIL_DIAGNOSTIC_RESULT_DONE);
    }
}
