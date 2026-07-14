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
    const face_pose_t *pose = &ui->animator.pose;
    int16_t gaze_offset = (int16_t)pose->gaze * 3;
    uint8_t left_x = (uint8_t)(32 + gaze_offset);
    uint8_t right_x = (uint8_t)(80 + gaze_offset);
    uint8_t index;
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
        if (pose->blinking || ui->expression == ROBOT_EXPRESSION_SLEEPY) {
            ssd1306_fill_rect(display, left_x, 24u, 16u, 2u, true);
            ssd1306_fill_rect(display, right_x, 24u, 16u, 2u, true);
        } else if (ui->expression == ROBOT_EXPRESSION_THINKING) {
            ssd1306_fill_rect(display, left_x, 20u, 16u, 10u, true);
            ssd1306_fill_rect(display, right_x, 24u, 16u, 3u, true);
        } else if (ui->expression == ROBOT_EXPRESSION_SURPRISED) {
            ssd1306_fill_rect(display, left_x, 17u, 14u, 16u, true);
            ssd1306_fill_rect(display, right_x, 17u, 14u, 16u, true);
        } else if (ui->expression == ROBOT_EXPRESSION_HAPPY &&
                   pose->mouth_variant == 2u) {
            ssd1306_fill_rect(display, left_x, 23u, 16u, 4u, true);
            ssd1306_fill_rect(display, right_x, 23u, 16u, 4u, true);
        } else {
            uint8_t eye_y = ui->expression == ROBOT_EXPRESSION_SAD ? 22u : 20u;
            uint8_t eye_height = ui->expression == ROBOT_EXPRESSION_SAD ? 8u : 10u;
            ssd1306_fill_rect(display, left_x, eye_y, 16u, eye_height, true);
            ssd1306_fill_rect(display, right_x, eye_y, 16u, eye_height, true);
        }
        if (ui->expression == ROBOT_EXPRESSION_HAPPY) {
            uint8_t inset = (uint8_t)(pose->mouth_variant * 2u);
            ssd1306_fill_rect(display, (uint8_t)(49u + inset), 38u, 3u, 5u, true);
            ssd1306_fill_rect(display, (uint8_t)(76u - inset), 38u, 3u, 5u, true);
            ssd1306_fill_rect(display, (uint8_t)(52u + inset), 42u,
                              (uint8_t)(24u - inset * 2u), 3u, true);
        } else if (ui->expression == ROBOT_EXPRESSION_SAD) {
            uint8_t inset = (uint8_t)(pose->mouth_variant * 2u);
            ssd1306_fill_rect(display, (uint8_t)(53u + inset), 38u,
                              (uint8_t)(22u - inset * 2u), 3u, true);
            ssd1306_fill_rect(display, (uint8_t)(50u + inset), 40u, 3u, 5u, true);
            ssd1306_fill_rect(display, (uint8_t)(75u - inset), 40u, 3u, 5u, true);
            for (index = 0u; index < 6u; ++index) {
                ssd1306_pixel(display, (uint8_t)(30u + index), (uint8_t)(18u + index / 2u), true);
                ssd1306_pixel(display, (uint8_t)(98u - index), (uint8_t)(18u + index / 2u), true);
            }
        } else if (ui->expression == ROBOT_EXPRESSION_THINKING) {
            uint8_t offset = pose->mouth_variant;
            ssd1306_fill_rect(display, (uint8_t)(57u + offset), 40u, 3u, 3u, true);
            ssd1306_fill_rect(display, (uint8_t)(64u + offset), 40u, 3u, 3u, true);
            ssd1306_fill_rect(display, (uint8_t)(71u + offset), 40u, 3u, 3u, true);
        } else if (ui->expression == ROBOT_EXPRESSION_SURPRISED) {
            uint8_t mouth_height = (uint8_t)(9u + pose->mouth_variant);
            ssd1306_fill_rect(display, 59u, 37u, 10u, 2u, true);
            ssd1306_fill_rect(display, 59u, (uint8_t)(37u + mouth_height), 10u, 2u, true);
            ssd1306_fill_rect(display, 59u, 39u, 2u, (uint8_t)(mouth_height - 2u), true);
            ssd1306_fill_rect(display, 67u, 39u, 2u, (uint8_t)(mouth_height - 2u), true);
        } else if (ui->expression == ROBOT_EXPRESSION_SLEEPY) {
            ssd1306_fill_rect(display, (uint8_t)(58u + pose->mouth_variant), 40u,
                              (uint8_t)(12u - pose->mouth_variant * 2u), 2u, true);
        } else {
            uint8_t width = (uint8_t)(10u + pose->mouth_variant * 4u);
            ssd1306_fill_rect(display, (uint8_t)(64u - width / 2u),
                              (uint8_t)(40u + pose->mouth_variant % 2u), width, 2u, true);
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

bool ui_service_init(ui_service_t *ui, uint32_t random_seed) {
    if (ui == NULL) {
        return false;
    }
    ui->expression = ROBOT_EXPRESSION_NEUTRAL;
    face_animator_init(&ui->animator, random_seed, ROBOT_EXPRESSION_NEUTRAL, 0u);
    ui->state = ROBOT_STATE_SELF_TEST;
    ui->link_healthy = false;
    ui->dirty = true;
    ui->flush_active = false;
    ui->next_page = 0u;
    ui->last_flush_ms = 0u;
    return ssd1306_init(&ui->display);
}

void ui_service_set_expression(ui_service_t *ui, uint8_t expression, uint32_t now_ms) {
    if (ui != NULL && expression <= ROBOT_EXPRESSION_FAULT) {
        ui->expression = expression;
        (void)face_animator_set_expression(&ui->animator, expression, now_ms);
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
    if (ui == NULL) {
        return;
    }
    if (ui->state != ROBOT_STATE_ESTOP && ui->state != ROBOT_STATE_FAULT &&
        face_animator_tick(&ui->animator, now_ms)) {
        ui->dirty = true;
    }
    if (!ui->display.available) {
        return;
    }
    if (!ui->flush_active) {
        if (!ui->dirty || (uint32_t)(now_ms - ui->last_flush_ms) < 100u) {
            return;
        }
        draw_face(ui);
        ui->dirty = false;
        ui->flush_active = true;
        ui->next_page = 0u;
    }
    if (!ssd1306_flush_page(&ui->display, ui->next_page)) {
        ui->flush_active = false;
        return;
    }
    ui->next_page++;
    if (ui->next_page >= SSD1306_PAGE_COUNT) {
        ui->flush_active = false;
        ui->last_flush_ms = now_ms;
    }
}
