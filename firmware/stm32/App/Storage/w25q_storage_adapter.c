#include "w25q_storage_adapter.h"

#include <stddef.h>

static storage_flash_result_t map_error(const w25q_t *flash) {
    if (flash == NULL) {
        return STORAGE_FLASH_UNAVAILABLE;
    }
    switch (flash->last_error) {
        case W25Q_ERROR_BUSY:
            return STORAGE_FLASH_BUSY;
        case W25Q_ERROR_WRITE_PROTECTED:
        case W25Q_ERROR_WRITE_ENABLE:
            return STORAGE_FLASH_PROTECTED;
        case W25Q_ERROR_OUT_OF_RANGE:
        case W25Q_ERROR_ARGUMENT:
            return STORAGE_FLASH_RANGE;
        case W25Q_ERROR_UNAVAILABLE:
            return STORAGE_FLASH_UNAVAILABLE;
        case W25Q_ERROR_BUS:
        case W25Q_ERROR_NONE:
        default:
            return STORAGE_FLASH_IO;
    }
}

static storage_flash_result_t adapter_read(void *context, uint32_t address,
                                           uint8_t *data, uint16_t length) {
    w25q_t *flash = context;
    return w25q_read(flash, address, data, length) ? STORAGE_FLASH_OK
                                                   : map_error(flash);
}

static storage_flash_result_t adapter_erase(void *context, uint32_t address) {
    w25q_t *flash = context;
    return w25q_start_sector_erase(flash, address) ? STORAGE_FLASH_OK
                                                   : map_error(flash);
}

static storage_flash_result_t adapter_program(void *context, uint32_t address,
                                              const uint8_t *data, uint16_t length) {
    w25q_t *flash = context;
    return w25q_start_page_program(flash, address, data, length) ? STORAGE_FLASH_OK
                                                                 : map_error(flash);
}

static storage_flash_result_t adapter_poll(void *context, bool *busy) {
    w25q_t *flash = context;
    return w25q_is_busy(flash, busy) ? STORAGE_FLASH_OK : map_error(flash);
}

void w25q_storage_adapter_init(storage_flash_t *storage, w25q_t *flash) {
    static const storage_flash_ops_t operations = {
        .read = adapter_read,
        .start_sector_erase = adapter_erase,
        .start_page_program = adapter_program,
        .poll_busy = adapter_poll,
    };
    if (storage == NULL) {
        return;
    }
    storage->ops = &operations;
    storage->context = flash;
    storage->capacity_bytes = flash != NULL ? flash->capacity_bytes : 0u;
}
