#include "spi_mailbox.h"

#include <stddef.h>
#include <string.h>

#include "main.h"

static spi_mailbox_t *active_mailbox;

static uint16_t next_seq(spi_mailbox_t *mailbox) {
    mailbox->tx_seq++;
    if (mailbox->tx_seq == 0u) {
        mailbox->tx_seq = 1u;
    }
    return mailbox->tx_seq;
}

static void prepare_noop(spi_mailbox_t *mailbox) {
    (void)robot_spi_slot_encode(ROBOT_MSG_NOOP, next_seq(mailbox), 0u, NULL, 0u,
                                mailbox->tx_slot, sizeof(mailbox->tx_slot));
}

void spi_mailbox_init(spi_mailbox_t *mailbox) {
    if (mailbox == NULL) {
        return;
    }
    memset(mailbox, 0, sizeof(*mailbox));
    prepare_noop(mailbox);
    active_mailbox = mailbox;
}

bool spi_mailbox_start(spi_mailbox_t *mailbox) {
    if (mailbox == NULL || mailbox->armed) {
        return false;
    }
    mailbox->transfer_complete = false;
    mailbox->transfer_error = false;
    mailbox->armed = HAL_SPI_TransmitReceive_DMA(&hspi1, mailbox->tx_slot, mailbox->rx_slot,
                                                  ROBOT_SPI_SLOT_SIZE) == HAL_OK;
    return mailbox->armed;
}

bool spi_mailbox_take_received(spi_mailbox_t *mailbox, robot_spi_slot_view_t *view,
                               robot_slot_status_t *status) {
    if (mailbox == NULL || view == NULL || status == NULL || !mailbox->transfer_complete) {
        return false;
    }
    mailbox->transfer_complete = false;
    mailbox->armed = false;
    *status = robot_spi_slot_decode(mailbox->rx_slot, sizeof(mailbox->rx_slot), view);
    if (*status != ROBOT_SLOT_OK) {
        mailbox->errors++;
    }
    return true;
}

bool spi_mailbox_queue(spi_mailbox_t *mailbox, uint16_t type, uint8_t flags,
                       const uint8_t *payload, uint16_t length, bool priority) {
    uint8_t index;
    if (mailbox == NULL) {
        return false;
    }
    if (mailbox->queue_count >= SPI_MAILBOX_QUEUE_CAPACITY) {
        if (!priority) {
            mailbox->dropped++;
            return false;
        }
        mailbox->queue_head = (uint8_t)((mailbox->queue_head + 1u) % SPI_MAILBOX_QUEUE_CAPACITY);
        mailbox->queue_count--;
        mailbox->dropped++;
    }
    if (priority) {
        mailbox->queue_head = (uint8_t)((mailbox->queue_head + SPI_MAILBOX_QUEUE_CAPACITY - 1u) %
                                        SPI_MAILBOX_QUEUE_CAPACITY);
        index = mailbox->queue_head;
    } else {
        index = (uint8_t)((mailbox->queue_head + mailbox->queue_count) % SPI_MAILBOX_QUEUE_CAPACITY);
    }
    if (robot_spi_slot_encode(type, next_seq(mailbox), flags, payload, length,
                              mailbox->queue[index], ROBOT_SPI_SLOT_SIZE) != ROBOT_SLOT_OK) {
        if (priority) {
            mailbox->queue_head = (uint8_t)((mailbox->queue_head + 1u) % SPI_MAILBOX_QUEUE_CAPACITY);
        }
        return false;
    }
    mailbox->queue_count++;
    return true;
}

bool spi_mailbox_rearm(spi_mailbox_t *mailbox) {
    if (mailbox == NULL || mailbox->armed) {
        return false;
    }
    if (mailbox->transfer_error) {
        (void)HAL_SPI_Abort(&hspi1);
        mailbox->transfer_error = false;
    }
    if (mailbox->queue_count != 0u) {
        memcpy(mailbox->tx_slot, mailbox->queue[mailbox->queue_head], ROBOT_SPI_SLOT_SIZE);
        mailbox->queue_head = (uint8_t)((mailbox->queue_head + 1u) % SPI_MAILBOX_QUEUE_CAPACITY);
        mailbox->queue_count--;
    } else {
        prepare_noop(mailbox);
    }
    return spi_mailbox_start(mailbox);
}

void spi_mailbox_transfer_complete_isr(void) {
    if (active_mailbox != NULL) {
        active_mailbox->transfer_complete = true;
        active_mailbox->armed = false;
    }
}

void spi_mailbox_transfer_error_isr(void) {
    if (active_mailbox != NULL) {
        active_mailbox->transfer_error = true;
        active_mailbox->armed = false;
        active_mailbox->errors++;
    }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *spi) {
    if (spi->Instance == SPI1) {
        spi_mailbox_transfer_complete_isr();
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *spi) {
    if (spi->Instance == SPI1) {
        spi_mailbox_transfer_error_isr();
    }
}
