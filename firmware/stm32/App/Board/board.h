#ifndef AIROBOT_BOARD_H
#define AIROBOT_BOARD_H

#include <stdbool.h>
#include <stdint.h>

bool board_clock_config(void);
void board_gpio_init(void);
bool board_peripherals_init(void);
void board_motors_off(void);
void board_status_led_set(bool on);
uint32_t board_millis(void);
uint32_t board_next_boot_id(void);

#endif
