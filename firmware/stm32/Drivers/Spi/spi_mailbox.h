#ifndef AIROBOT_SPI_MAILBOX_H
#define AIROBOT_SPI_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "robot_spi_slot.h"

#define SPI_MAILBOX_QUEUE_CAPACITY 4u

typedef struct {
    uint8_t rx_slot[ROBOT_SPI_SLOT_SIZE];
    uint8_t tx_slot[ROBOT_SPI_SLOT_SIZE];
    uint8_t queue[SPI_MAILBOX_QUEUE_CAPACITY][ROBOT_SPI_SLOT_SIZE];
    volatile bool transfer_complete;
    volatile bool transfer_error;
    bool armed;
    uint8_t queue_head;
    uint8_t queue_count;
    uint16_t tx_seq;
    uint32_t dropped;
    uint32_t errors;
} spi_mailbox_t;

void spi_mailbox_init(spi_mailbox_t *mailbox);
bool spi_mailbox_start(spi_mailbox_t *mailbox);
bool spi_mailbox_take_received(spi_mailbox_t *mailbox, robot_spi_slot_view_t *view,
                               robot_slot_status_t *status);
bool spi_mailbox_queue(spi_mailbox_t *mailbox, uint16_t type, uint8_t flags,
                       const uint8_t *payload, uint16_t length, bool priority);
bool spi_mailbox_rearm(spi_mailbox_t *mailbox);
void spi_mailbox_transfer_complete_isr(void);
void spi_mailbox_transfer_error_isr(void);

#endif
