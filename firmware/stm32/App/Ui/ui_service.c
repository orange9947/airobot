#include "ui_service.h"

#include <stddef.h>

#include "protocol_ids.h"

static uint16_t glyph3x5(char character) {
    switch (character) {
        case 'A': return 0x7BEDu;
        case 'D': return 0x6B6Eu;
        case 'E': return 0x79CFu;
        case 'I': return 0x7497u;
        case 'L': return 0x4927u;
        case 'M': return 0x7DB5u;
        case 'N': return 0x7DBDu;
        case 'O': return 0x7B6Fu;
        case 'R': return 0x7B5Du;
        case 'S': return 0x79E7u;
        case 'T': return 0x7492u;
        case 'P': return 0x7B48u;
        case '!': return 0x2492u;
        default: return 0u;
    }
}

static void draw_text(ssd1306_t *display, uint8_t x, uint8_t y, const char *text) {
    while (*text != '\0') {
        uint16_t glyph = glyph3x5(*text++);
        uint8_t row;
        uint8_t column;
        for (row = 0u; row < 5u; ++row) {
            for (column = 0u; column < 3u; ++column) {
                bool on = (glyph & (1u << (row * 3u + column))) != 0u;
                ssd1306_pixel(display, (uint8_t)(x + column), (uint8_t)(y + row), on);
            }
        }
        x = (uint8_t)(x + 4u);
    }
}

static void draw_face(ui_service_t *ui) {
    ssd1306_t *display = &ui->display;
    uint8_t eye_y = 20u;
    ssd1306_clear(display);
    if (ui->state == ROBOT_STATE_ESTOP || ui->expression == ROBOT_EXPRESSION_ESTOP) {
        uint8_t offset;
        for (offset = 0u; offset < 12u; ++offset) {
            ssd1306_pixel(display, (uint8_t)(34u + offset), (uint8_t)(16u + offset), true);
            ssd1306_pixel(display, (uint8_t)(45u - offset), (uint8_t)(16u + offset), true);
            ssd1306_pixel(display, (uint8_t)(82u + offset), (uint8_t)(16u + offset), true);
            ssd1306_pixel(display, (uint8_t)(93u - offset), (uint8_t)(16u + offset), true);
        }
        draw_text(display, 52u, 48u, "STOP");
    } else if (ui->state == ROBOT_STATE_FAULT || ui->expression == ROBOT_EXPRESSION_FAULT) {
        ssd1306_fill_rect(display, 33u, 16u, 14u, 14u, true);
        ssd1306_fill_rect(display, 81u, 16u, 14u, 14u, true);
        draw_text(display, 56u, 48u, "ERR");
    } else {
        uint8_t eye_height = ui->expression == ROBOT_EXPRESSION_SLEEPY ? 2u : 10u;
        if (ui->expression == ROBOT_EXPRESSION_SURPRISED) {
            eye_height = 16u;
            eye_y = 17u;
        }
        ssd1306_fill_rect(display, 32u, eye_y, 16u, eye_height, true);
        ssd1306_fill_rect(display, 80u, eye_y, 16u, eye_height, true);
        if (ui->expression == ROBOT_EXPRESSION_HAPPY) {
            ssd1306_fill_rect(display, 52u, 40u, 24u, 2u, true);
            ssd1306_fill_rect(display, 56u, 42u, 16u, 2u, true);
        } else if (ui->expression == ROBOT_EXPRESSION_SAD) {
            ssd1306_fill_rect(display, 56u, 38u, 16u, 2u, true);
            ssd1306_fill_rect(display, 52u, 40u, 4u, 2u, true);
            ssd1306_fill_rect(display, 72u, 40u, 4u, 2u, true);
        }
        if (ui->state == ROBOT_STATE_AI) {
            draw_text(display, 60u, 55u, "AI");
        } else if (ui->state == ROBOT_STATE_MANUAL) {
            draw_text(display, 56u, 55u, "MAN");
        } else {
            draw_text(display, 56u, 55u, "IDLE");
        }
    }
    ssd1306_fill_rect(display, 2u, 2u, 5u, 5u, ui->link_healthy);
}

bool ui_service_init(ui_service_t *ui) {
    if (ui == NULL) {
        return false;
    }
    ui->expression = ROBOT_EXPRESSION_NEUTRAL;
    ui->state = ROBOT_STATE_SELF_TEST;
    ui->link_healthy = false;
    ui->dirty = true;
    ui->last_flush_ms = 0u;
    return ssd1306_init(&ui->display);
}

void ui_service_set_expression(ui_service_t *ui, uint8_t expression) {
    if (ui != NULL && expression <= ROBOT_EXPRESSION_FAULT) {
        ui->expression = expression;
        ui->dirty = true;
    }
}

void ui_service_set_status(ui_service_t *ui, robot_state_value_t state, bool link_healthy) {
    if (ui != NULL && (ui->state != state || ui->link_healthy != link_healthy)) {
        ui->state = state;
        ui->link_healthy = link_healthy;
        ui->dirty = true;
    }
}

void ui_service_tick(ui_service_t *ui, uint32_t now_ms) {
    if (ui == NULL || !ui->display.available || !ui->dirty ||
        (uint32_t)(now_ms - ui->last_flush_ms) < 100u) {
        return;
    }
    draw_face(ui);
    if (ssd1306_flush(&ui->display)) {
        ui->dirty = false;
        ui->last_flush_ms = now_ms;
    }
}
