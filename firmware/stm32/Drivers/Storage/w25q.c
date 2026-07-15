#include "w25q.h"

#include <stddef.h>
#include <string.h>

#include "board_pins.h"
#include "main.h"

#define W25Q_COMMAND_JEDEC_ID 0x9Fu
#define W25Q_COMMAND_READ 0x03u
#define W25Q_COMMAND_READ_STATUS_1 0x05u
#define W25Q_COMMAND_WRITE_ENABLE 0x06u
#define W25Q_COMMAND_PAGE_PROGRAM 0x02u
#define W25Q_COMMAND_SECTOR_ERASE 0x20u
#define W25Q_COMMAND_RELEASE_POWER_DOWN 0xABu
#define W25Q_COMMAND_RESET_ENABLE 0x66u
#define W25Q_COMMAND_RESET 0x99u
#define W25Q_TIMEOUT_MS 20u
#define W25Q_ID_ATTEMPTS 3u
#define W25Q_SECTOR_SIZE 4096u
#define W25Q_PAGE_SIZE 256u
#define W25Q_STATUS_BUSY 0x01u
#define W25Q_STATUS_WRITE_ENABLE_LATCH 0x02u
#define W25Q_STATUS_BLOCK_PROTECT_MASK 0x1Cu
#define W25Q_READ_CHUNK_SIZE 32u

_Static_assert(W25Q_THREE_BYTE_ADDRESS_LIMIT == (1u << 24u),
               "three-byte flash commands must be limited to 16 MiB");

static void select_flash(bool selected) {
    HAL_GPIO_WritePin(W25Q_CS_PORT, W25Q_CS_PIN, selected ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static bool transfer(const uint8_t *tx, uint8_t *rx, uint16_t length) {
    return HAL_SPI_TransmitReceive(&hspi2, (uint8_t *)tx, rx, length, W25Q_TIMEOUT_MS) == HAL_OK;
}

static bool fail(w25q_t *flash, w25q_error_t error) {
    if (flash != NULL) {
        flash->last_error = error;
        flash->errors++;
    }
    return false;
}

static void succeed(w25q_t *flash) {
    if (flash != NULL) {
        flash->last_error = W25Q_ERROR_NONE;
    }
}

static bool send_command(w25q_t *flash, uint8_t command) {
    HAL_StatusTypeDef status;
    select_flash(true);
    status = HAL_SPI_Transmit(&hspi2, &command, 1u, W25Q_TIMEOUT_MS);
    select_flash(false);
    if (status != HAL_OK) {
        return fail(flash, W25Q_ERROR_BUS);
    }
    succeed(flash);
    return true;
}

static bool range_valid(const w25q_t *flash, uint32_t address, uint32_t length) {
    return flash != NULL && flash->available &&
           w25q_range_is_addressable(flash->capacity_bytes, address, length);
}

static bool ready_for_write(w25q_t *flash) {
    uint8_t status;
    if (!w25q_read_status(flash, &status)) {
        return false;
    }
    if ((status & W25Q_STATUS_BUSY) != 0u) {
        return fail(flash, W25Q_ERROR_BUSY);
    }
    if ((status & W25Q_STATUS_BLOCK_PROTECT_MASK) != 0u) {
        return fail(flash, W25Q_ERROR_WRITE_PROTECTED);
    }
    if (!send_command(flash, W25Q_COMMAND_WRITE_ENABLE) ||
        !w25q_read_status(flash, &status)) {
        return false;
    }
    if ((status & W25Q_STATUS_WRITE_ENABLE_LATCH) == 0u) {
        return fail(flash, W25Q_ERROR_WRITE_ENABLE);
    }
    return true;
}

bool w25q_init(w25q_t *flash) {
    uint8_t attempt;
    uint8_t capacity_code;

    if (flash == NULL) {
        return false;
    }
    *flash = (w25q_t){0};
    if (!send_command(flash, W25Q_COMMAND_RELEASE_POWER_DOWN)) {
        return false;
    }
    HAL_Delay(1u);
    if (!send_command(flash, W25Q_COMMAND_RESET_ENABLE) ||
        !send_command(flash, W25Q_COMMAND_RESET)) {
        return false;
    }
    HAL_Delay(1u);
    for (attempt = 0u; attempt < W25Q_ID_ATTEMPTS; ++attempt) {
        uint8_t tx[4] = {W25Q_COMMAND_JEDEC_ID, 0xFFu, 0xFFu, 0xFFu};
        uint8_t rx[4] = {0};
        select_flash(true);
        if (!transfer(tx, rx, sizeof(tx))) {
            select_flash(false);
            (void)fail(flash, W25Q_ERROR_BUS);
            continue;
        }
        select_flash(false);
        flash->jedec_id = ((uint32_t)rx[1] << 16u) | ((uint32_t)rx[2] << 8u) | rx[3];
        capacity_code = rx[3];
        if (rx[1] != 0x00u && rx[1] != 0xFFu &&
            capacity_code >= 0x14u && capacity_code <= 0x1Fu) {
            flash->capacity_bytes =
                w25q_addressable_capacity(1u << capacity_code);
            flash->available = flash->capacity_bytes >= 1048576u;
            succeed(flash);
            return flash->available;
        }
        HAL_Delay(1u);
    }
    return fail(flash, W25Q_ERROR_UNAVAILABLE);
}

bool w25q_read(w25q_t *flash, uint32_t address, uint8_t *data, uint16_t length) {
    uint8_t command[4];
    uint8_t dummy[W25Q_READ_CHUNK_SIZE];
    uint16_t offset = 0u;
    bool busy;

    if (flash == NULL || data == NULL || length == 0u) {
        return fail(flash, W25Q_ERROR_ARGUMENT);
    }
    if (!range_valid(flash, address, length)) {
        return fail(flash, flash->available ? W25Q_ERROR_OUT_OF_RANGE
                                            : W25Q_ERROR_UNAVAILABLE);
    }
    if (!w25q_is_busy(flash, &busy)) {
        return false;
    }
    if (busy) {
        return fail(flash, W25Q_ERROR_BUSY);
    }
    command[0] = W25Q_COMMAND_READ;
    command[1] = (uint8_t)(address >> 16u);
    command[2] = (uint8_t)(address >> 8u);
    command[3] = (uint8_t)address;
    select_flash(true);
    if (HAL_SPI_Transmit(&hspi2, command, sizeof(command), W25Q_TIMEOUT_MS) != HAL_OK) {
        select_flash(false);
        return fail(flash, W25Q_ERROR_BUS);
    }
    memset(dummy, 0xFF, sizeof(dummy));
    while (offset < length) {
        uint16_t remaining = (uint16_t)(length - offset);
        uint16_t chunk = remaining < sizeof(dummy) ? remaining : sizeof(dummy);
        if (!transfer(dummy, &data[offset], chunk)) {
            select_flash(false);
            return fail(flash, W25Q_ERROR_BUS);
        }
        offset = (uint16_t)(offset + chunk);
    }
    select_flash(false);
    succeed(flash);
    return true;
}

bool w25q_read_status(w25q_t *flash, uint8_t *status) {
    uint8_t tx[2] = {W25Q_COMMAND_READ_STATUS_1, 0xFFu};
    uint8_t rx[2] = {0};
    if (flash == NULL || status == NULL) {
        return fail(flash, W25Q_ERROR_ARGUMENT);
    }
    if (!flash->available) {
        return fail(flash, W25Q_ERROR_UNAVAILABLE);
    }
    select_flash(true);
    if (!transfer(tx, rx, sizeof(tx))) {
        select_flash(false);
        return fail(flash, W25Q_ERROR_BUS);
    }
    select_flash(false);
    *status = rx[1];
    succeed(flash);
    return true;
}

bool w25q_is_busy(w25q_t *flash, bool *busy) {
    uint8_t status;
    if (busy == NULL) {
        return fail(flash, W25Q_ERROR_ARGUMENT);
    }
    if (!w25q_read_status(flash, &status)) {
        return false;
    }
    *busy = (status & W25Q_STATUS_BUSY) != 0u;
    return true;
}

bool w25q_start_sector_erase(w25q_t *flash, uint32_t address) {
    uint8_t command[4];
    if (flash == NULL) {
        return false;
    }
    if ((address & (W25Q_SECTOR_SIZE - 1u)) != 0u ||
        !range_valid(flash, address, W25Q_SECTOR_SIZE)) {
        return fail(flash, flash->available ? W25Q_ERROR_OUT_OF_RANGE
                                            : W25Q_ERROR_UNAVAILABLE);
    }
    if (!ready_for_write(flash)) {
        return false;
    }
    command[0] = W25Q_COMMAND_SECTOR_ERASE;
    command[1] = (uint8_t)(address >> 16u);
    command[2] = (uint8_t)(address >> 8u);
    command[3] = (uint8_t)address;
    select_flash(true);
    if (HAL_SPI_Transmit(&hspi2, command, sizeof(command), W25Q_TIMEOUT_MS) != HAL_OK) {
        select_flash(false);
        return fail(flash, W25Q_ERROR_BUS);
    }
    select_flash(false);
    succeed(flash);
    return true;
}

bool w25q_start_page_program(w25q_t *flash, uint32_t address,
                              const uint8_t *data, uint16_t length) {
    uint8_t command[4];
    uint32_t page_offset = address & (W25Q_PAGE_SIZE - 1u);
    if (flash == NULL || data == NULL || length == 0u || length > W25Q_PAGE_SIZE) {
        return fail(flash, W25Q_ERROR_ARGUMENT);
    }
    if (!range_valid(flash, address, length) || page_offset + length > W25Q_PAGE_SIZE) {
        return fail(flash, flash->available ? W25Q_ERROR_OUT_OF_RANGE
                                            : W25Q_ERROR_UNAVAILABLE);
    }
    if (!ready_for_write(flash)) {
        return false;
    }
    command[0] = W25Q_COMMAND_PAGE_PROGRAM;
    command[1] = (uint8_t)(address >> 16u);
    command[2] = (uint8_t)(address >> 8u);
    command[3] = (uint8_t)address;
    select_flash(true);
    if (HAL_SPI_Transmit(&hspi2, command, sizeof(command), W25Q_TIMEOUT_MS) != HAL_OK ||
        HAL_SPI_Transmit(&hspi2, (uint8_t *)data, length, W25Q_TIMEOUT_MS) != HAL_OK) {
        select_flash(false);
        return fail(flash, W25Q_ERROR_BUS);
    }
    select_flash(false);
    succeed(flash);
    return true;
}
