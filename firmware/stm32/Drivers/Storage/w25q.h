#ifndef AIROBOT_W25Q_H
#define AIROBOT_W25Q_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool available;
    uint32_t jedec_id;
    uint32_t capacity_bytes;
    uint32_t errors;
} w25q_t;

bool w25q_init(w25q_t *flash);
bool w25q_read(w25q_t *flash, uint32_t address, uint8_t *data, uint16_t length);

#endif
