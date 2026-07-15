#include "resource_crc32.h"

#define RESOURCE_CRC32_INITIAL 0xFFFFFFFFu
#define RESOURCE_CRC32_POLYNOMIAL 0xEDB88320u

uint32_t resource_crc32_init(void) {
    return RESOURCE_CRC32_INITIAL;
}

uint32_t resource_crc32_update(uint32_t state, const uint8_t *data, size_t length) {
    size_t index;
    uint8_t bit;

    if (data == NULL) {
        return state;
    }
    for (index = 0u; index < length; ++index) {
        state ^= data[index];
        for (bit = 0u; bit < 8u; ++bit) {
            state = (state >> 1u) ^
                    ((state & 1u) != 0u ? RESOURCE_CRC32_POLYNOMIAL : 0u);
        }
    }
    return state;
}

uint32_t resource_crc32_finalize(uint32_t state) {
    return state ^ RESOURCE_CRC32_INITIAL;
}

uint32_t resource_crc32(const uint8_t *data, size_t length) {
    return resource_crc32_finalize(
        resource_crc32_update(resource_crc32_init(), data, length));
}
