#include "robot_spi_slot.h"

#include <string.h>

#include "protocol_layouts.h"
#include "robot_crc16.h"

uint16_t robot_read_u16_le(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

uint32_t robot_read_u32_le(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) | ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

int32_t robot_read_i32_le(const uint8_t *data) {
    return (int32_t)robot_read_u32_le(data);
}

void robot_write_u16_le(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)(value >> 8u);
}

void robot_write_u32_le(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)((value >> 8u) & 0xFFu);
    data[2] = (uint8_t)((value >> 16u) & 0xFFu);
    data[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

void robot_write_i32_le(uint8_t *data, int32_t value) {
    robot_write_u32_le(data, (uint32_t)value);
}

uint16_t robot_protocol_payload_length(uint16_t type) {
    switch (type) {
        case ROBOT_MSG_NOOP:
            return ROBOT_PAYLOAD_LEN_NOOP;
        case ROBOT_MSG_HELLO_REQ:
            return ROBOT_PAYLOAD_LEN_HELLO_REQ;
        case ROBOT_MSG_HELLO_RSP:
            return ROBOT_PAYLOAD_LEN_HELLO_RSP;
        case ROBOT_MSG_HEARTBEAT:
            return ROBOT_PAYLOAD_LEN_HEARTBEAT;
        case ROBOT_MSG_GET_STATE:
            return ROBOT_PAYLOAD_LEN_GET_STATE;
        case ROBOT_MSG_STATE_SNAPSHOT:
            return ROBOT_PAYLOAD_LEN_STATE_SNAPSHOT;
        case ROBOT_MSG_SET_MODE:
            return ROBOT_PAYLOAD_LEN_SET_MODE;
        case ROBOT_MSG_MOVE_WHEELS:
            return ROBOT_PAYLOAD_LEN_MOVE_WHEELS;
        case ROBOT_MSG_STOP:
            return ROBOT_PAYLOAD_LEN_STOP;
        case ROBOT_MSG_SET_EXPRESSION:
            return ROBOT_PAYLOAD_LEN_SET_EXPRESSION;
        case ROBOT_MSG_SET_RUNTIME_CONFIG:
            return ROBOT_PAYLOAD_LEN_SET_RUNTIME_CONFIG;
        case ROBOT_MSG_CLEAR_ESTOP:
            return ROBOT_PAYLOAD_LEN_CLEAR_ESTOP;
        case ROBOT_MSG_COIL_DIAGNOSTIC:
            return ROBOT_PAYLOAD_LEN_COIL_DIAGNOSTIC;
        case ROBOT_MSG_ACK:
            return ROBOT_PAYLOAD_LEN_ACK;
        case ROBOT_MSG_NACK:
            return ROBOT_PAYLOAD_LEN_NACK;
        case ROBOT_MSG_MOTION_STARTED:
            return ROBOT_PAYLOAD_LEN_MOTION_STARTED;
        case ROBOT_MSG_MOTION_DONE:
            return ROBOT_PAYLOAD_LEN_MOTION_DONE;
        case ROBOT_MSG_MOTION_ABORTED:
            return ROBOT_PAYLOAD_LEN_MOTION_ABORTED;
        case ROBOT_MSG_MODE_CHANGED:
            return ROBOT_PAYLOAD_LEN_MODE_CHANGED;
        case ROBOT_MSG_FAULT_EVENT:
            return ROBOT_PAYLOAD_LEN_FAULT_EVENT;
        case ROBOT_MSG_COIL_DIAGNOSTIC_RESULT:
            return ROBOT_PAYLOAD_LEN_COIL_DIAGNOSTIC_RESULT;
        case ROBOT_MSG_FLASH_INFO:
            return ROBOT_PAYLOAD_LEN_FLASH_INFO;
        case ROBOT_MSG_RESOURCE_BEGIN:
            return ROBOT_PAYLOAD_LEN_RESOURCE_BEGIN;
        case ROBOT_MSG_RESOURCE_CHUNK:
            return ROBOT_PAYLOAD_LEN_RESOURCE_CHUNK;
        case ROBOT_MSG_RESOURCE_FINISH:
            return ROBOT_PAYLOAD_LEN_RESOURCE_FINISH;
        case ROBOT_MSG_RESOURCE_ABORT:
            return ROBOT_PAYLOAD_LEN_RESOURCE_ABORT;
        case ROBOT_MSG_GET_RESOURCE_STATUS:
            return ROBOT_PAYLOAD_LEN_GET_RESOURCE_STATUS;
        case ROBOT_MSG_RESOURCE_STATUS:
            return ROBOT_PAYLOAD_LEN_RESOURCE_STATUS;
        default:
            return UINT16_MAX;
    }
}

robot_slot_status_t robot_spi_slot_encode(uint16_t type, uint16_t seq, uint8_t flags,
                                           const uint8_t *payload, uint16_t length,
                                           uint8_t *output, size_t output_size) {
    uint16_t expected_length;
    uint16_t crc;

    if (output == NULL || (payload == NULL && length != 0u)) {
        return ROBOT_SLOT_NULL;
    }
    if (output_size != ROBOT_SPI_SLOT_SIZE) {
        return ROBOT_SLOT_BAD_SIZE;
    }
    expected_length = robot_protocol_payload_length(type);
    if (expected_length == UINT16_MAX) {
        return ROBOT_SLOT_BAD_TYPE;
    }
    if (length != expected_length || length > ROBOT_SPI_PAYLOAD_SIZE) {
        return ROBOT_SLOT_BAD_LENGTH;
    }

    memset(output, 0, output_size);
    output[0] = ROBOT_SPI_MAGIC_0;
    output[1] = ROBOT_SPI_MAGIC_1;
    output[2] = ROBOT_PROTOCOL_VERSION;
    output[3] = flags;
    robot_write_u16_le(&output[4], type);
    robot_write_u16_le(&output[6], seq);
    robot_write_u16_le(&output[8], length);
    if (length != 0u) {
        memcpy(&output[ROBOT_SPI_PAYLOAD_OFFSET], payload, length);
    }
    crc = robot_crc16_ccitt(&output[ROBOT_SPI_HEADER_OFFSET],
                            ROBOT_SPI_CRC_OFFSET - ROBOT_SPI_HEADER_OFFSET, 0xFFFFu);
    robot_write_u16_le(&output[ROBOT_SPI_CRC_OFFSET], crc);
    return ROBOT_SLOT_OK;
}

robot_slot_status_t robot_spi_slot_decode(const uint8_t *slot, size_t slot_size,
                                           robot_spi_slot_view_t *view) {
    uint16_t type;
    uint16_t length;
    uint16_t expected_length;
    uint16_t expected_crc;
    uint16_t actual_crc;
    size_t index;

    if (slot == NULL || view == NULL) {
        return ROBOT_SLOT_NULL;
    }
    if (slot_size != ROBOT_SPI_SLOT_SIZE) {
        return ROBOT_SLOT_BAD_SIZE;
    }
    if (slot[0] != ROBOT_SPI_MAGIC_0 || slot[1] != ROBOT_SPI_MAGIC_1) {
        return ROBOT_SLOT_BAD_MAGIC;
    }
    if (slot[2] != ROBOT_PROTOCOL_VERSION) {
        return ROBOT_SLOT_BAD_VERSION;
    }
    type = robot_read_u16_le(&slot[4]);
    length = robot_read_u16_le(&slot[8]);
    expected_length = robot_protocol_payload_length(type);
    if (expected_length == UINT16_MAX) {
        return ROBOT_SLOT_BAD_TYPE;
    }
    if (length != expected_length || length > ROBOT_SPI_PAYLOAD_SIZE) {
        return ROBOT_SLOT_BAD_LENGTH;
    }
    for (index = ROBOT_SPI_PAYLOAD_OFFSET + length; index < ROBOT_SPI_CRC_OFFSET; ++index) {
        if (slot[index] != 0u) {
            return ROBOT_SLOT_BAD_PADDING;
        }
    }
    expected_crc = robot_read_u16_le(&slot[ROBOT_SPI_CRC_OFFSET]);
    actual_crc = robot_crc16_ccitt(&slot[ROBOT_SPI_HEADER_OFFSET],
                                   ROBOT_SPI_CRC_OFFSET - ROBOT_SPI_HEADER_OFFSET, 0xFFFFu);
    if (expected_crc != actual_crc) {
        return ROBOT_SLOT_BAD_CRC;
    }

    view->version = slot[2];
    view->flags = slot[3];
    view->type = type;
    view->seq = robot_read_u16_le(&slot[6]);
    view->length = length;
    view->payload = &slot[ROBOT_SPI_PAYLOAD_OFFSET];
    return ROBOT_SLOT_OK;
}
