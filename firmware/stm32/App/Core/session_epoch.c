#include "session_epoch.h"

#include <stddef.h>

#define SESSION_BOOT_RECORD_MAGIC 0xB07Eu
#define SESSION_BOOT_CHECK_SEED 0x6D3Au

static uint16_t boot_record_check(uint32_t counter) {
    uint32_t mixed = counter ^ (counter >> 16u) ^ SESSION_BOOT_CHECK_SEED;

    mixed ^= mixed << 7u;
    mixed ^= mixed >> 11u;
    return (uint16_t)(mixed ^ (mixed >> 16u));
}

static bool counter_is_newer(uint32_t candidate, uint32_t current) {
    uint32_t distance = candidate - current;

    return distance != 0u && distance < 0x80000000u;
}

void session_peer_epoch_init(session_peer_epoch_t *epoch) {
    if (epoch == NULL) {
        return;
    }
    epoch->seen = false;
    epoch->boot_id = 0u;
}

session_peer_result_t session_peer_epoch_observe(session_peer_epoch_t *epoch,
                                                 uint32_t boot_id) {
    if (epoch == NULL) {
        return SESSION_PEER_CHANGED;
    }
    if (!epoch->seen) {
        epoch->seen = true;
        epoch->boot_id = boot_id;
        return SESSION_PEER_FIRST;
    }
    if (epoch->boot_id == boot_id) {
        return SESSION_PEER_SAME;
    }
    epoch->boot_id = boot_id;
    return SESSION_PEER_CHANGED;
}

bool session_boot_record_decode(const session_boot_record_t *record,
                                uint32_t *counter) {
    uint32_t value;

    if (record == NULL || counter == NULL ||
        record->magic != SESSION_BOOT_RECORD_MAGIC) {
        return false;
    }
    value = (uint32_t)record->counter_low |
            ((uint32_t)record->counter_high << 16u);
    if (value == 0u || record->check != boot_record_check(value)) {
        return false;
    }
    *counter = value;
    return true;
}

void session_boot_record_encode(uint32_t counter,
                                session_boot_record_t *record) {
    if (record == NULL) {
        return;
    }
    record->magic = SESSION_BOOT_RECORD_MAGIC;
    record->counter_low = (uint16_t)counter;
    record->counter_high = (uint16_t)(counter >> 16u);
    record->check = boot_record_check(counter);
}

uint32_t session_boot_counter_advance(const session_boot_record_t *slot_a,
                                      const session_boot_record_t *slot_b,
                                      uint8_t *target_slot,
                                      session_boot_record_t *next_record) {
    uint32_t counter_a = 0u;
    uint32_t counter_b = 0u;
    uint32_t previous = 0u;
    uint32_t next;
    bool valid_a = session_boot_record_decode(slot_a, &counter_a);
    bool valid_b = session_boot_record_decode(slot_b, &counter_b);
    uint8_t target = SESSION_BOOT_SLOT_A;

    if (valid_a && valid_b) {
        if (counter_is_newer(counter_b, counter_a)) {
            previous = counter_b;
            target = SESSION_BOOT_SLOT_A;
        } else {
            previous = counter_a;
            target = SESSION_BOOT_SLOT_B;
        }
    } else if (valid_a) {
        previous = counter_a;
        target = SESSION_BOOT_SLOT_B;
    } else if (valid_b) {
        previous = counter_b;
        target = SESSION_BOOT_SLOT_A;
    }
    next = previous + 1u;
    if (next == 0u) {
        next = 1u;
    }
    if (target_slot != NULL) {
        *target_slot = target;
    }
    session_boot_record_encode(next, next_record);
    return next;
}
