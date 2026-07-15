#ifndef AIROBOT_SESSION_EPOCH_H
#define AIROBOT_SESSION_EPOCH_H

#include <stdbool.h>
#include <stdint.h>

#define SESSION_BOOT_SLOT_A 0u
#define SESSION_BOOT_SLOT_B 1u

typedef enum {
    SESSION_PEER_FIRST = 0,
    SESSION_PEER_SAME,
    SESSION_PEER_CHANGED,
} session_peer_result_t;

typedef struct {
    bool seen;
    uint32_t boot_id;
} session_peer_epoch_t;

typedef struct {
    uint16_t magic;
    uint16_t counter_low;
    uint16_t counter_high;
    uint16_t check;
} session_boot_record_t;

void session_peer_epoch_init(session_peer_epoch_t *epoch);
session_peer_result_t session_peer_epoch_observe(session_peer_epoch_t *epoch,
                                                 uint32_t boot_id);
bool session_boot_record_decode(const session_boot_record_t *record,
                                uint32_t *counter);
void session_boot_record_encode(uint32_t counter,
                                session_boot_record_t *record);
uint32_t session_boot_counter_advance(const session_boot_record_t *slot_a,
                                      const session_boot_record_t *slot_b,
                                      uint8_t *target_slot,
                                      session_boot_record_t *next_record);

#endif
