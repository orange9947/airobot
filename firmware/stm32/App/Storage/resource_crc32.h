#ifndef AIROBOT_RESOURCE_CRC32_H
#define AIROBOT_RESOURCE_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t resource_crc32_init(void);
uint32_t resource_crc32_update(uint32_t state, const uint8_t *data, size_t length);
uint32_t resource_crc32_finalize(uint32_t state);
uint32_t resource_crc32(const uint8_t *data, size_t length);

#endif
