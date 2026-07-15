#ifndef AIROBOT_FAKE_STORAGE_FLASH_H
#define AIROBOT_FAKE_STORAGE_FLASH_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "storage_flash.h"

#define FAKE_STORAGE_FLASH_CAPACITY (1024u * 1024u)
#define FAKE_STORAGE_FLASH_SECTOR_SIZE 4096u
#define FAKE_STORAGE_FLASH_PAGE_SIZE 256u

typedef enum {
    FAKE_STORAGE_PENDING_NONE = 0,
    FAKE_STORAGE_PENDING_ERASE,
    FAKE_STORAGE_PENDING_PROGRAM,
} fake_storage_pending_t;

typedef struct {
    uint8_t bytes[FAKE_STORAGE_FLASH_CAPACITY];
    uint8_t pending_data[FAKE_STORAGE_FLASH_PAGE_SIZE];
    fake_storage_pending_t pending;
    uint32_t pending_address;
    uint16_t pending_length;
    uint8_t busy_polls_before_complete;
    uint8_t busy_polls_remaining;
    bool stay_busy;
    storage_flash_result_t fail_next;
    uint32_t calls_this_tick;
    uint32_t max_calls_per_tick;
    uint32_t read_calls;
    uint32_t erase_calls;
    uint32_t program_calls;
    uint32_t poll_calls;
} fake_storage_flash_t;

static inline void fake_storage_flash_record_call(fake_storage_flash_t *fake) {
    fake->calls_this_tick++;
    if (fake->calls_this_tick > fake->max_calls_per_tick) {
        fake->max_calls_per_tick = fake->calls_this_tick;
    }
}

static inline storage_flash_result_t fake_storage_flash_take_failure(
    fake_storage_flash_t *fake) {
    storage_flash_result_t result = fake->fail_next;
    fake->fail_next = STORAGE_FLASH_OK;
    return result;
}

static inline storage_flash_result_t fake_storage_read(
    void *context, uint32_t address, uint8_t *data, uint16_t length) {
    fake_storage_flash_t *fake = (fake_storage_flash_t *)context;
    storage_flash_result_t failure;

    fake_storage_flash_record_call(fake);
    fake->read_calls++;
    failure = fake_storage_flash_take_failure(fake);
    if (failure != STORAGE_FLASH_OK) {
        return failure;
    }
    if (data == NULL || length == 0u || fake->pending != FAKE_STORAGE_PENDING_NONE ||
        address > FAKE_STORAGE_FLASH_CAPACITY ||
        length > FAKE_STORAGE_FLASH_CAPACITY - address) {
        return fake->pending != FAKE_STORAGE_PENDING_NONE ? STORAGE_FLASH_BUSY
                                                          : STORAGE_FLASH_RANGE;
    }
    memcpy(data, &fake->bytes[address], length);
    return STORAGE_FLASH_OK;
}

static inline storage_flash_result_t fake_storage_start_erase(void *context,
                                                               uint32_t address) {
    fake_storage_flash_t *fake = (fake_storage_flash_t *)context;
    storage_flash_result_t failure;

    fake_storage_flash_record_call(fake);
    fake->erase_calls++;
    failure = fake_storage_flash_take_failure(fake);
    if (failure != STORAGE_FLASH_OK) {
        return failure;
    }
    if (fake->pending != FAKE_STORAGE_PENDING_NONE) {
        return STORAGE_FLASH_BUSY;
    }
    if ((address & (FAKE_STORAGE_FLASH_SECTOR_SIZE - 1u)) != 0u ||
        address > FAKE_STORAGE_FLASH_CAPACITY ||
        FAKE_STORAGE_FLASH_SECTOR_SIZE > FAKE_STORAGE_FLASH_CAPACITY - address) {
        return STORAGE_FLASH_RANGE;
    }
    fake->pending = FAKE_STORAGE_PENDING_ERASE;
    fake->pending_address = address;
    fake->pending_length = FAKE_STORAGE_FLASH_SECTOR_SIZE;
    fake->busy_polls_remaining = fake->busy_polls_before_complete;
    return STORAGE_FLASH_OK;
}

static inline storage_flash_result_t fake_storage_start_program(
    void *context, uint32_t address, const uint8_t *data, uint16_t length) {
    fake_storage_flash_t *fake = (fake_storage_flash_t *)context;
    storage_flash_result_t failure;
    uint32_t page_offset = address & (FAKE_STORAGE_FLASH_PAGE_SIZE - 1u);

    fake_storage_flash_record_call(fake);
    fake->program_calls++;
    failure = fake_storage_flash_take_failure(fake);
    if (failure != STORAGE_FLASH_OK) {
        return failure;
    }
    if (fake->pending != FAKE_STORAGE_PENDING_NONE) {
        return STORAGE_FLASH_BUSY;
    }
    if (data == NULL || length == 0u || length > FAKE_STORAGE_FLASH_PAGE_SIZE ||
        page_offset + length > FAKE_STORAGE_FLASH_PAGE_SIZE ||
        address > FAKE_STORAGE_FLASH_CAPACITY ||
        length > FAKE_STORAGE_FLASH_CAPACITY - address) {
        return STORAGE_FLASH_RANGE;
    }
    fake->pending = FAKE_STORAGE_PENDING_PROGRAM;
    fake->pending_address = address;
    fake->pending_length = length;
    memcpy(fake->pending_data, data, length);
    fake->busy_polls_remaining = fake->busy_polls_before_complete;
    return STORAGE_FLASH_OK;
}

static inline storage_flash_result_t fake_storage_poll(void *context,
                                                        bool *busy) {
    fake_storage_flash_t *fake = (fake_storage_flash_t *)context;
    storage_flash_result_t failure;

    fake_storage_flash_record_call(fake);
    fake->poll_calls++;
    failure = fake_storage_flash_take_failure(fake);
    if (failure != STORAGE_FLASH_OK) {
        return failure;
    }
    if (busy == NULL) {
        return STORAGE_FLASH_RANGE;
    }
    if (fake->pending == FAKE_STORAGE_PENDING_NONE) {
        *busy = false;
        return STORAGE_FLASH_OK;
    }
    if (fake->stay_busy) {
        *busy = true;
        return STORAGE_FLASH_OK;
    }
    if (fake->busy_polls_remaining > 0u) {
        fake->busy_polls_remaining--;
        *busy = true;
        return STORAGE_FLASH_OK;
    }
    if (fake->pending == FAKE_STORAGE_PENDING_ERASE) {
        memset(&fake->bytes[fake->pending_address], 0xFF,
               FAKE_STORAGE_FLASH_SECTOR_SIZE);
    } else {
        uint16_t index;
        for (index = 0u; index < fake->pending_length; ++index) {
            fake->bytes[fake->pending_address + index] &= fake->pending_data[index];
        }
    }
    fake->pending = FAKE_STORAGE_PENDING_NONE;
    *busy = false;
    return STORAGE_FLASH_OK;
}

static inline void fake_storage_flash_init(fake_storage_flash_t *fake) {
    memset(fake, 0, sizeof(*fake));
    memset(fake->bytes, 0xFF, sizeof(fake->bytes));
    fake->busy_polls_before_complete = 1u;
}

static inline storage_flash_t fake_storage_flash_interface(
    fake_storage_flash_t *fake) {
    static const storage_flash_ops_t operations = {
        .read = fake_storage_read,
        .start_sector_erase = fake_storage_start_erase,
        .start_page_program = fake_storage_start_program,
        .poll_busy = fake_storage_poll,
    };
    storage_flash_t storage = {
        .ops = &operations,
        .context = fake,
        .capacity_bytes = FAKE_STORAGE_FLASH_CAPACITY,
    };
    return storage;
}

static inline void fake_storage_flash_begin_tick(fake_storage_flash_t *fake) {
    fake->calls_this_tick = 0u;
}

static inline void fake_storage_flash_power_cycle(fake_storage_flash_t *fake) {
    fake->pending = FAKE_STORAGE_PENDING_NONE;
    fake->busy_polls_remaining = 0u;
    fake->calls_this_tick = 0u;
    fake->fail_next = STORAGE_FLASH_OK;
}

static inline void fake_storage_flash_direct_program(fake_storage_flash_t *fake,
                                                      uint32_t address,
                                                      const uint8_t *data,
                                                      uint32_t length) {
    uint32_t index;
    for (index = 0u; index < length; ++index) {
        fake->bytes[address + index] &= data[index];
    }
}

#endif
