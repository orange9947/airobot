#ifndef AIROBOT_UI_SERVICE_H
#define AIROBOT_UI_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "face_animator.h"
#include "robot_state.h"
#include "ssd1306.h"

typedef struct {
    ssd1306_t display;
    uint8_t expression;
    face_animator_t animator;
    robot_state_value_t state;
    bool link_healthy;
    bool dirty;
    bool flush_active;
    uint8_t next_page;
    uint32_t last_flush_ms;
} ui_service_t;

bool ui_service_init(ui_service_t *ui, uint32_t random_seed);
void ui_service_set_expression(ui_service_t *ui, uint8_t expression, uint32_t now_ms);
void ui_service_set_status(ui_service_t *ui, robot_state_value_t state, bool link_healthy);
void ui_service_tick(ui_service_t *ui, uint32_t now_ms);

#endif
