#ifndef AIROBOT_SOFT_I2C_H
#define AIROBOT_SOFT_I2C_H

#include <stdbool.h>
#include <stdint.h>

void soft_i2c_init(void);
bool soft_i2c_probe(uint8_t address);
bool soft_i2c_write(uint8_t address, uint8_t control, const uint8_t *data, uint16_t length);

#endif
