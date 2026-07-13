#ifndef ROBOT_CRC16_H
#define ROBOT_CRC16_H

#include <stddef.h>
#include <stdint.h>

uint16_t robot_crc16_ccitt(const uint8_t *data, size_t length, uint16_t initial);

#endif
