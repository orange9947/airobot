#include "soft_i2c.h"

#include <stddef.h>

#include "board_pins.h"

static void delay_half_period(void) {
    volatile uint32_t count = SystemCoreClock / 1200000u;
    while (count-- != 0u) {
        __NOP();
    }
}

static void sda(bool high) {
    HAL_GPIO_WritePin(OLED_SDA_PORT, OLED_SDA_PIN, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void scl(bool high) {
    HAL_GPIO_WritePin(OLED_SCL_PORT, OLED_SCL_PIN, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void start_condition(void) {
    sda(true);
    scl(true);
    delay_half_period();
    sda(false);
    delay_half_period();
    scl(false);
}

static void stop_condition(void) {
    sda(false);
    scl(true);
    delay_half_period();
    sda(true);
    delay_half_period();
}

static bool write_byte(uint8_t value) {
    uint8_t bit;
    bool acknowledged;
    for (bit = 0u; bit < 8u; ++bit) {
        sda((value & 0x80u) != 0u);
        delay_half_period();
        scl(true);
        delay_half_period();
        scl(false);
        value <<= 1u;
    }
    sda(true);
    delay_half_period();
    scl(true);
    delay_half_period();
    acknowledged = HAL_GPIO_ReadPin(OLED_SDA_PORT, OLED_SDA_PIN) == GPIO_PIN_RESET;
    scl(false);
    return acknowledged;
}

void soft_i2c_init(void) {
    sda(true);
    scl(true);
}

bool soft_i2c_probe(uint8_t address) {
    bool acknowledged;
    start_condition();
    acknowledged = write_byte((uint8_t)(address << 1u));
    stop_condition();
    return acknowledged;
}

bool soft_i2c_write(uint8_t address, uint8_t control, const uint8_t *data, uint16_t length) {
    uint16_t index;
    if (data == NULL && length != 0u) {
        return false;
    }
    start_condition();
    if (!write_byte((uint8_t)(address << 1u)) || !write_byte(control)) {
        stop_condition();
        return false;
    }
    for (index = 0u; index < length; ++index) {
        if (!write_byte(data[index])) {
            stop_condition();
            return false;
        }
    }
    stop_condition();
    return true;
}
