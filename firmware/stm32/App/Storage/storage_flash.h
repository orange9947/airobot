#ifndef AIROBOT_STORAGE_FLASH_H
#define AIROBOT_STORAGE_FLASH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STORAGE_FLASH_OK = 0,
    STORAGE_FLASH_BUSY,
    STORAGE_FLASH_PROTECTED,
    STORAGE_FLASH_RANGE,
    STORAGE_FLASH_UNAVAILABLE,
    STORAGE_FLASH_IO,
} storage_flash_result_t;

typedef struct {
    storage_flash_result_t (*read)(void *context, uint32_t address,
                                   uint8_t *data, uint16_t length);
    storage_flash_result_t (*start_sector_erase)(void *context, uint32_t address);
    storage_flash_result_t (*start_page_program)(void *context, uint32_t address,
                                                 const uint8_t *data, uint16_t length);
    storage_flash_result_t (*poll_busy)(void *context, bool *busy);
} storage_flash_ops_t;

typedef struct {
    const storage_flash_ops_t *ops;
    void *context;
    uint32_t capacity_bytes;
} storage_flash_t;

#endif
