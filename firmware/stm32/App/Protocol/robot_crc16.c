#include "robot_crc16.h"

uint16_t robot_crc16_ccitt(const uint8_t *data, size_t length, uint16_t initial) {
    uint16_t crc = initial;
    size_t index;
    uint8_t bit;

    if ((data == NULL) && (length != 0u)) {
        return initial;
    }
    for (index = 0u; index < length; ++index) {
        crc ^= (uint16_t)data[index] << 8u;
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 0x8000u) != 0u ? (uint16_t)((crc << 1u) ^ 0x1021u)
                                           : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}
