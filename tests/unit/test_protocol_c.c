#include <stdint.h>
#include <string.h>

#include "protocol_golden.h"
#include "protocol_layouts.h"
#include "robot_crc16.h"
#include "robot_spi_slot.h"
#include "test_harness.h"

static int test_crc(void) {
    static const uint8_t input[] = "123456789";
    TEST_ASSERT_EQ(0x29B1u, robot_crc16_ccitt(input, sizeof(input) - 1u, 0xFFFFu));
    return 0;
}

static int test_decode_golden(void) {
    robot_spi_slot_view_t view;
    TEST_ASSERT_EQ(ROBOT_SLOT_OK,
                   robot_spi_slot_decode(ROBOT_GOLDEN_HELLO_REQ, ROBOT_SPI_SLOT_SIZE, &view));
    TEST_ASSERT_EQ(ROBOT_MSG_HELLO_REQ, view.type);
    TEST_ASSERT_EQ(2u, view.seq);
    TEST_ASSERT_EQ(ROBOT_PAYLOAD_LEN_HELLO_REQ, view.length);
    TEST_ASSERT_EQ(0x12345678u, robot_read_u32_le(view.payload));

    TEST_ASSERT_EQ(ROBOT_SLOT_OK,
                   robot_spi_slot_decode(ROBOT_GOLDEN_MOVE_WHEELS, ROBOT_SPI_SLOT_SIZE, &view));
    TEST_ASSERT_EQ(ROBOT_MSG_MOVE_WHEELS, view.type);
    TEST_ASSERT_EQ(42u, robot_read_u32_le(view.payload));
    TEST_ASSERT_EQ(-512, robot_read_i32_le(&view.payload[4]));
    TEST_ASSERT_EQ(512, robot_read_i32_le(&view.payload[8]));
    return 0;
}

static int test_encode_matches_golden(void) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_MOVE_WHEELS] = {0};
    uint8_t output[ROBOT_SPI_SLOT_SIZE];

    robot_write_u32_le(&payload[0], 42u);
    robot_write_i32_le(&payload[4], -512);
    robot_write_i32_le(&payload[8], 512);
    robot_write_u16_le(&payload[12], 400u);
    robot_write_u16_le(&payload[14], 600u);
    robot_write_u16_le(&payload[16], 2000u);
    TEST_ASSERT_EQ(ROBOT_SLOT_OK,
                   robot_spi_slot_encode(ROBOT_MSG_MOVE_WHEELS, 8u, 1u, payload,
                                         sizeof(payload), output, sizeof(output)));
    TEST_ASSERT(memcmp(output, ROBOT_GOLDEN_MOVE_WHEELS, sizeof(output)) == 0);
    return 0;
}

static int test_rejects_corruption(void) {
    uint8_t slot[ROBOT_SPI_SLOT_SIZE];
    robot_spi_slot_view_t view;

    memcpy(slot, ROBOT_GOLDEN_ACK, sizeof(slot));
    slot[0] ^= 0x01u;
    TEST_ASSERT_EQ(ROBOT_SLOT_BAD_MAGIC, robot_spi_slot_decode(slot, sizeof(slot), &view));

    memcpy(slot, ROBOT_GOLDEN_ACK, sizeof(slot));
    slot[20] = 1u;
    TEST_ASSERT_EQ(ROBOT_SLOT_BAD_PADDING, robot_spi_slot_decode(slot, sizeof(slot), &view));

    memcpy(slot, ROBOT_GOLDEN_ACK, sizeof(slot));
    slot[ROBOT_SPI_CRC_OFFSET] ^= 0x01u;
    TEST_ASSERT_EQ(ROBOT_SLOT_BAD_CRC, robot_spi_slot_decode(slot, sizeof(slot), &view));
    return 0;
}

int main(void) {
    TEST_ASSERT_EQ(0, test_crc());
    TEST_ASSERT_EQ(0, test_decode_golden());
    TEST_ASSERT_EQ(0, test_encode_matches_golden());
    TEST_ASSERT_EQ(0, test_rejects_corruption());
    return 0;
}
