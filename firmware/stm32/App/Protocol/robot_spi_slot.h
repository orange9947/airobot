#ifndef ROBOT_SPI_SLOT_H
#define ROBOT_SPI_SLOT_H

#include <stddef.h>
#include <stdint.h>

#include "protocol_ids.h"

#define ROBOT_SPI_HEADER_OFFSET 2u
#define ROBOT_SPI_PAYLOAD_OFFSET 10u
#define ROBOT_SPI_CRC_OFFSET (ROBOT_SPI_SLOT_SIZE - 2u)

typedef enum {
    ROBOT_SLOT_OK = 0,
    ROBOT_SLOT_NULL,
    ROBOT_SLOT_BAD_SIZE,
    ROBOT_SLOT_BAD_MAGIC,
    ROBOT_SLOT_BAD_VERSION,
    ROBOT_SLOT_BAD_TYPE,
    ROBOT_SLOT_BAD_LENGTH,
    ROBOT_SLOT_BAD_PADDING,
    ROBOT_SLOT_BAD_CRC,
} robot_slot_status_t;

typedef struct {
    uint8_t version;
    uint8_t flags;
    uint16_t type;
    uint16_t seq;
    uint16_t length;
    const uint8_t *payload;
} robot_spi_slot_view_t;

robot_slot_status_t robot_spi_slot_encode(uint16_t type, uint16_t seq, uint8_t flags,
                                           const uint8_t *payload, uint16_t length,
                                           uint8_t *output, size_t output_size);
robot_slot_status_t robot_spi_slot_decode(const uint8_t *slot, size_t slot_size,
                                           robot_spi_slot_view_t *view);
uint16_t robot_protocol_payload_length(uint16_t type);

uint16_t robot_read_u16_le(const uint8_t *data);
uint32_t robot_read_u32_le(const uint8_t *data);
int32_t robot_read_i32_le(const uint8_t *data);
void robot_write_u16_le(uint8_t *data, uint16_t value);
void robot_write_u32_le(uint8_t *data, uint32_t value);
void robot_write_i32_le(uint8_t *data, int32_t value);

#endif
