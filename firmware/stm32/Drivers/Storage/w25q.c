#include "w25q.h"

#include <stddef.h>

#include "board_pins.h"
#include "main.h"

#define W25Q_COMMAND_JEDEC_ID 0x9Fu
#define W25Q_COMMAND_READ 0x03u
#define W25Q_TIMEOUT_MS 20u

static void select_flash(bool selected) {
    HAL_GPIO_WritePin(W25Q_CS_PORT, W25Q_CS_PIN, selected ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static bool transfer(const uint8_t *tx, uint8_t *rx, uint16_t length) {
    return HAL_SPI_TransmitReceive(&hspi2, (uint8_t *)tx, rx, length, W25Q_TIMEOUT_MS) == HAL_OK;
}

bool w25q_init(w25q_t *flash) {
    uint8_t tx[4] = {W25Q_COMMAND_JEDEC_ID, 0xFFu, 0xFFu, 0xFFu};
    uint8_t rx[4] = {0};
    uint8_t capacity_code;

    if (flash == NULL) {
        return false;
    }
    *flash = (w25q_t){0};
    select_flash(true);
    if (!transfer(tx, rx, sizeof(tx))) {
        select_flash(false);
        flash->errors++;
        return false;
    }
    select_flash(false);
    flash->jedec_id = ((uint32_t)rx[1] << 16u) | ((uint32_t)rx[2] << 8u) | rx[3];
    capacity_code = rx[3];
    if (rx[1] == 0x00u || rx[1] == 0xFFu || capacity_code < 0x14u || capacity_code > 0x1Fu) {
        flash->errors++;
        return false;
    }
    flash->capacity_bytes = 1u << capacity_code;
    flash->available = flash->capacity_bytes >= 1048576u;
    return flash->available;
}

bool w25q_read(w25q_t *flash, uint32_t address, uint8_t *data, uint16_t length) {
    uint8_t command[4];
    uint16_t index;

    if (flash == NULL || data == NULL || !flash->available || length == 0u ||
        address + length > flash->capacity_bytes) {
        return false;
    }
    command[0] = W25Q_COMMAND_READ;
    command[1] = (uint8_t)(address >> 16u);
    command[2] = (uint8_t)(address >> 8u);
    command[3] = (uint8_t)address;
    select_flash(true);
    if (HAL_SPI_Transmit(&hspi2, command, sizeof(command), W25Q_TIMEOUT_MS) != HAL_OK) {
        select_flash(false);
        flash->errors++;
        return false;
    }
    for (index = 0u; index < length; ++index) {
        uint8_t dummy = 0xFFu;
        if (!transfer(&dummy, &data[index], 1u)) {
            select_flash(false);
            flash->errors++;
            return false;
        }
    }
    select_flash(false);
    return true;
}
