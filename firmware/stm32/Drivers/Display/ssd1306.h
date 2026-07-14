#ifndef AIROBOT_SSD1306_H
#define AIROBOT_SSD1306_H

#include <stdbool.h>
#include <stdint.h>

#define SSD1306_WIDTH 128u
#define SSD1306_HEIGHT 64u
#define SSD1306_BUFFER_SIZE 1024u
#define SSD1306_PAGE_COUNT (SSD1306_HEIGHT / 8u)

typedef struct {
    bool available;
    uint8_t address;
    uint8_t buffer[SSD1306_BUFFER_SIZE];
    uint32_t errors;
} ssd1306_t;

bool ssd1306_init(ssd1306_t *display);
void ssd1306_clear(ssd1306_t *display);
void ssd1306_pixel(ssd1306_t *display, uint8_t x, uint8_t y, bool on);
void ssd1306_fill_rect(ssd1306_t *display, uint8_t x, uint8_t y,
                       uint8_t width, uint8_t height, bool on);
bool ssd1306_flush(ssd1306_t *display);
bool ssd1306_flush_page(ssd1306_t *display, uint8_t page);

#endif
