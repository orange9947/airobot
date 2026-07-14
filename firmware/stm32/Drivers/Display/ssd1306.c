#include "ssd1306.h"

#include <stddef.h>
#include <string.h>

#include "soft_i2c.h"

#define SSD1306_ADDRESS_PRIMARY 0x3Cu
#define SSD1306_ADDRESS_SECONDARY 0x3Du
#define SSD1306_COMMAND_CONTROL 0x00u
#define SSD1306_DATA_CONTROL 0x40u

static bool command(ssd1306_t *display, uint8_t value) {
    if (!soft_i2c_write(display->address, SSD1306_COMMAND_CONTROL, &value, 1u)) {
        display->errors++;
        return false;
    }
    return true;
}

bool ssd1306_init(ssd1306_t *display) {
    static const uint8_t commands[] = {
        0xAEu, 0x20u, 0x00u, 0xB0u, 0xC8u, 0x00u, 0x10u, 0x40u,
        0x81u, 0x7Fu, 0xA1u, 0xA6u, 0xA8u, 0x3Fu, 0xA4u, 0xD3u,
        0x00u, 0xD5u, 0x80u, 0xD9u, 0xF1u, 0xDAu, 0x12u, 0xDBu,
        0x40u, 0x8Du, 0x14u, 0xAFu,
    };
    uint16_t index;
    if (display == NULL) {
        return false;
    }
    memset(display, 0, sizeof(*display));
    soft_i2c_init();
    display->address = SSD1306_ADDRESS_PRIMARY;
    if (!soft_i2c_probe(display->address)) {
        display->address = SSD1306_ADDRESS_SECONDARY;
        if (!soft_i2c_probe(display->address)) {
            display->errors++;
            return false;
        }
    }
    for (index = 0u; index < sizeof(commands); ++index) {
        if (!command(display, commands[index])) {
            return false;
        }
    }
    display->available = true;
    ssd1306_clear(display);
    return ssd1306_flush(display);
}

void ssd1306_clear(ssd1306_t *display) {
    if (display != NULL) {
        memset(display->buffer, 0, sizeof(display->buffer));
    }
}

void ssd1306_pixel(ssd1306_t *display, uint8_t x, uint8_t y, bool on) {
    uint16_t index;
    uint8_t mask;
    if (display == NULL || x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) {
        return;
    }
    index = (uint16_t)x + (uint16_t)(y / 8u) * SSD1306_WIDTH;
    mask = (uint8_t)(1u << (y & 0x07u));
    if (on) {
        display->buffer[index] |= mask;
    } else {
        display->buffer[index] &= (uint8_t)~mask;
    }
}

void ssd1306_fill_rect(ssd1306_t *display, uint8_t x, uint8_t y,
                       uint8_t width, uint8_t height, bool on) {
    uint16_t px;
    uint16_t py;
    for (py = y; py < (uint16_t)y + height && py < SSD1306_HEIGHT; ++py) {
        for (px = x; px < (uint16_t)x + width && px < SSD1306_WIDTH; ++px) {
            ssd1306_pixel(display, (uint8_t)px, (uint8_t)py, on);
        }
    }
}

bool ssd1306_flush_page(ssd1306_t *display, uint8_t page) {
    uint8_t column;
    if (display == NULL || !display->available || page >= SSD1306_PAGE_COUNT) {
        return false;
    }
    if (!command(display, (uint8_t)(0xB0u + page)) || !command(display, 0x00u) ||
        !command(display, 0x10u)) {
        display->available = false;
        return false;
    }
    for (column = 0u; column < SSD1306_WIDTH; column += 16u) {
        if (!soft_i2c_write(display->address, SSD1306_DATA_CONTROL,
                            &display->buffer[(uint16_t)page * SSD1306_WIDTH + column], 16u)) {
            display->errors++;
            display->available = false;
            return false;
        }
    }
    return true;
}

bool ssd1306_flush(ssd1306_t *display) {
    uint8_t page;
    if (display == NULL || !display->available) {
        return false;
    }
    for (page = 0u; page < SSD1306_PAGE_COUNT; ++page) {
        if (!ssd1306_flush_page(display, page)) {
            return false;
        }
    }
    return true;
}
