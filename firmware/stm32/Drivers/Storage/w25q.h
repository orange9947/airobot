#ifndef AIROBOT_W25Q_H
#define AIROBOT_W25Q_H

#include <stdbool.h>
#include <stdint.h>

/* The implemented read/program/erase commands carry exactly three address bytes. */
#define W25Q_THREE_BYTE_ADDRESS_LIMIT 0x01000000u

static inline uint32_t w25q_addressable_capacity(uint32_t capacity_bytes) {
    return capacity_bytes < W25Q_THREE_BYTE_ADDRESS_LIMIT
               ? capacity_bytes
               : W25Q_THREE_BYTE_ADDRESS_LIMIT;
}

static inline bool w25q_range_is_addressable(uint32_t capacity_bytes,
                                              uint32_t address,
                                              uint32_t length) {
    uint32_t limit = w25q_addressable_capacity(capacity_bytes);
    return length > 0u && address < limit && length <= limit - address;
}

typedef enum {
    W25Q_ERROR_NONE = 0,
    W25Q_ERROR_ARGUMENT,
    W25Q_ERROR_UNAVAILABLE,
    W25Q_ERROR_OUT_OF_RANGE,
    W25Q_ERROR_BUS,
    W25Q_ERROR_BUSY,
    W25Q_ERROR_WRITE_PROTECTED,
    W25Q_ERROR_WRITE_ENABLE,
} w25q_error_t;

typedef struct {
    bool available;
    uint32_t jedec_id;
    /* Capacity safely addressable by the current three-byte command set. */
    uint32_t capacity_bytes;
    uint32_t errors;
    w25q_error_t last_error;
} w25q_t;

bool w25q_init(w25q_t *flash);
bool w25q_read(w25q_t *flash, uint32_t address, uint8_t *data, uint16_t length);
bool w25q_read_status(w25q_t *flash, uint8_t *status);
bool w25q_is_busy(w25q_t *flash, bool *busy);
bool w25q_start_sector_erase(w25q_t *flash, uint32_t address);
bool w25q_start_page_program(w25q_t *flash, uint32_t address,
                              const uint8_t *data, uint16_t length);

#endif
