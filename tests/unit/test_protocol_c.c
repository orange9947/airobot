#include <stdint.h>
#include <string.h>

#include "controller_session_policy.h"
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

static int test_clear_estop_layout(void) {
    TEST_ASSERT_EQ(0x0206u, ROBOT_MSG_CLEAR_ESTOP);
    TEST_ASSERT_EQ(4u, ROBOT_PAYLOAD_LEN_CLEAR_ESTOP);
    TEST_ASSERT_EQ(4u, robot_protocol_payload_length(ROBOT_MSG_CLEAR_ESTOP));
    return 0;
}

static int test_resource_layouts(void) {
    TEST_ASSERT_EQ(0u, ROBOT_RESOURCESTATE_BOOT_SCAN);
    TEST_ASSERT_EQ(9u, ROBOT_RESOURCESTATE_FAILED);
    TEST_ASSERT_EQ(0u, ROBOT_RESOURCEERROR_NONE);
    TEST_ASSERT_EQ(21u, ROBOT_RESOURCEERROR_INTERNAL);
    TEST_ASSERT_EQ(0x0402u, ROBOT_MSG_RESOURCE_BEGIN);
    TEST_ASSERT_EQ(0x0403u, ROBOT_MSG_RESOURCE_CHUNK);
    TEST_ASSERT_EQ(0x0404u, ROBOT_MSG_RESOURCE_FINISH);
    TEST_ASSERT_EQ(0x0405u, ROBOT_MSG_RESOURCE_ABORT);
    TEST_ASSERT_EQ(0x0406u, ROBOT_MSG_GET_RESOURCE_STATUS);
    TEST_ASSERT_EQ(0x0407u, ROBOT_MSG_RESOURCE_STATUS);
    TEST_ASSERT_EQ(256u, ROBOT_PAYLOAD_LEN_RESOURCE_CHUNK);
    TEST_ASSERT_EQ(256u, robot_protocol_payload_length(ROBOT_MSG_RESOURCE_CHUNK));
    return 0;
}

static int test_controller_session_policy(void) {
    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_REJECT_BAD_STATE,
                   controller_session_route_policy(
                       false, ROBOT_MSG_MOVE_WHEELS));
    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_REJECT_BAD_STATE,
                   controller_session_route_policy(
                       false, ROBOT_MSG_RESOURCE_BEGIN));
    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_REJECT_BAD_STATE,
                   controller_session_route_policy(
                       false, ROBOT_MSG_GET_RESOURCE_STATUS));

    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_ALLOW,
                   controller_session_route_policy(false, ROBOT_MSG_NOOP));
    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_ALLOW,
                   controller_session_route_policy(
                       false, ROBOT_MSG_HELLO_REQ));
    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_ALLOW,
                   controller_session_route_policy(
                       false, ROBOT_MSG_HEARTBEAT));
    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_ALLOW,
                   controller_session_route_policy(
                       false, ROBOT_MSG_GET_STATE));

    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_ALLOW,
                   controller_session_route_policy(
                       true, ROBOT_MSG_MOVE_WHEELS));
    TEST_ASSERT_EQ(CONTROLLER_SESSION_ROUTE_ALLOW,
                   controller_session_route_policy(
                       true, ROBOT_MSG_RESOURCE_BEGIN));
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

static int test_decode_resource_chunk_golden(void) {
    robot_spi_slot_view_t view;
    size_t index;

    TEST_ASSERT_EQ(ROBOT_SLOT_OK,
                   robot_spi_slot_decode(ROBOT_GOLDEN_RESOURCE_CHUNK, ROBOT_SPI_SLOT_SIZE,
                                         &view));
    TEST_ASSERT_EQ(ROBOT_MSG_RESOURCE_CHUNK, view.type);
    TEST_ASSERT_EQ(ROBOT_PAYLOAD_LEN_RESOURCE_CHUNK, view.length);
    TEST_ASSERT_EQ(48u, robot_read_u32_le(&view.payload[0]));
    TEST_ASSERT_EQ(0x11223344u, robot_read_u32_le(&view.payload[4]));
    TEST_ASSERT_EQ(0u, robot_read_u32_le(&view.payload[8]));
    TEST_ASSERT_EQ(4u, robot_read_u16_le(&view.payload[12]));
    TEST_ASSERT_EQ(0xB63CFBCDu, robot_read_u32_le(&view.payload[14]));
    TEST_ASSERT_EQ(1u, view.payload[18]);
    TEST_ASSERT_EQ(2u, view.payload[19]);
    TEST_ASSERT_EQ(3u, view.payload[20]);
    TEST_ASSERT_EQ(4u, view.payload[21]);
    for (index = 22u; index < ROBOT_PAYLOAD_LEN_RESOURCE_CHUNK; ++index) {
        TEST_ASSERT_EQ(0u, view.payload[index]);
    }
    return 0;
}

static int test_encode_resource_chunk_matches_golden(void) {
    uint8_t payload[ROBOT_PAYLOAD_LEN_RESOURCE_CHUNK] = {0};
    uint8_t output[ROBOT_SPI_SLOT_SIZE];

    robot_write_u32_le(&payload[0], 48u);
    robot_write_u32_le(&payload[4], 0x11223344u);
    robot_write_u32_le(&payload[8], 0u);
    robot_write_u16_le(&payload[12], 4u);
    robot_write_u32_le(&payload[14], 0xB63CFBCDu);
    payload[18] = 1u;
    payload[19] = 2u;
    payload[20] = 3u;
    payload[21] = 4u;
    TEST_ASSERT_EQ(ROBOT_SLOT_OK,
                   robot_spi_slot_encode(ROBOT_MSG_RESOURCE_CHUNK, 22u, 1u, payload,
                                         sizeof(payload), output, sizeof(output)));
    TEST_ASSERT(memcmp(output, ROBOT_GOLDEN_RESOURCE_CHUNK, sizeof(output)) == 0);
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
    TEST_ASSERT_EQ(0, test_clear_estop_layout());
    TEST_ASSERT_EQ(0, test_resource_layouts());
    TEST_ASSERT_EQ(0, test_controller_session_policy());
    TEST_ASSERT_EQ(0, test_decode_golden());
    TEST_ASSERT_EQ(0, test_encode_matches_golden());
    TEST_ASSERT_EQ(0, test_decode_resource_chunk_golden());
    TEST_ASSERT_EQ(0, test_encode_resource_chunk_matches_golden());
    TEST_ASSERT_EQ(0, test_rejects_corruption());
    return 0;
}
