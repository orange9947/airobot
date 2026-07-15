#include <stdint.h>

#include "coil_diagnostic_service.h"
#include "test_harness.h"

typedef struct {
    uint8_t left;
    uint8_t right;
    uint32_t calls;
} output_capture_t;

static void capture_output(uint8_t left, uint8_t right, void *context) {
    output_capture_t *capture = (output_capture_t *)context;
    capture->left = left;
    capture->right = right;
    capture->calls++;
}

static int test_left_channel_holds_until_deadline(void) {
    coil_diagnostic_service_t service;
    output_capture_t capture = {0};

    coil_diagnostic_service_init(&service, capture_output, &capture);
    TEST_ASSERT(coil_diagnostic_service_start(
        &service, 42u, COIL_DIAGNOSTIC_WHEEL_LEFT,
        COIL_DIAGNOSTIC_CHANNEL_B, 3000u, 1000u));
    TEST_ASSERT(service.active);
    TEST_ASSERT_EQ(0x02u, capture.left);
    TEST_ASSERT_EQ(0u, capture.right);
    TEST_ASSERT_EQ(1u, capture.calls);

    coil_diagnostic_service_tick_1ms(&service, 3999u);
    TEST_ASSERT(service.active);
    TEST_ASSERT_EQ(1u, capture.calls);

    coil_diagnostic_service_tick_1ms(&service, 4000u);
    TEST_ASSERT(!service.active);
    TEST_ASSERT_EQ(COIL_DIAGNOSTIC_RESULT_DONE, service.result);
    TEST_ASSERT_EQ(0u, capture.left);
    TEST_ASSERT_EQ(0u, capture.right);
    TEST_ASSERT_EQ(2u, capture.calls);
    return 0;
}

static int test_right_channel_abort_releases_both_outputs(void) {
    coil_diagnostic_service_t service;
    output_capture_t capture = {0};

    coil_diagnostic_service_init(&service, capture_output, &capture);
    TEST_ASSERT(coil_diagnostic_service_start(
        &service, 43u, COIL_DIAGNOSTIC_WHEEL_RIGHT,
        COIL_DIAGNOSTIC_CHANNEL_D, 3000u, 50u));
    TEST_ASSERT_EQ(0u, capture.left);
    TEST_ASSERT_EQ(0x08u, capture.right);

    coil_diagnostic_service_abort(
        &service, COIL_DIAGNOSTIC_RESULT_ABORTED);
    TEST_ASSERT(!service.active);
    TEST_ASSERT_EQ(COIL_DIAGNOSTIC_RESULT_ABORTED, service.result);
    TEST_ASSERT_EQ(0u, capture.left);
    TEST_ASSERT_EQ(0u, capture.right);
    TEST_ASSERT_EQ(2u, capture.calls);

    coil_diagnostic_service_abort(
        &service, COIL_DIAGNOSTIC_RESULT_ABORTED);
    TEST_ASSERT_EQ(2u, capture.calls);
    return 0;
}

static int test_validation_and_busy_rejection(void) {
    coil_diagnostic_service_t service;
    output_capture_t capture = {0};

    coil_diagnostic_service_init(&service, NULL, NULL);
    TEST_ASSERT(!coil_diagnostic_service_start(
        &service, 1u, COIL_DIAGNOSTIC_WHEEL_LEFT,
        COIL_DIAGNOSTIC_CHANNEL_A, 100u, 0u));

    coil_diagnostic_service_init(&service, capture_output, &capture);
    TEST_ASSERT(!coil_diagnostic_service_start(
        NULL, 1u, COIL_DIAGNOSTIC_WHEEL_LEFT,
        COIL_DIAGNOSTIC_CHANNEL_A, 100u, 0u));
    TEST_ASSERT(!coil_diagnostic_service_start(
        &service, 1u, (coil_diagnostic_wheel_t)2,
        COIL_DIAGNOSTIC_CHANNEL_A, 100u, 0u));
    TEST_ASSERT(!coil_diagnostic_service_start(
        &service, 1u, COIL_DIAGNOSTIC_WHEEL_LEFT,
        (coil_diagnostic_channel_t)4, 100u, 0u));
    TEST_ASSERT(!coil_diagnostic_service_start(
        &service, 1u, COIL_DIAGNOSTIC_WHEEL_LEFT,
        COIL_DIAGNOSTIC_CHANNEL_A, 99u, 0u));
    TEST_ASSERT(!coil_diagnostic_service_start(
        &service, 1u, COIL_DIAGNOSTIC_WHEEL_LEFT,
        COIL_DIAGNOSTIC_CHANNEL_A, 3001u, 0u));

    TEST_ASSERT(coil_diagnostic_service_start(
        &service, 1u, COIL_DIAGNOSTIC_WHEEL_LEFT,
        COIL_DIAGNOSTIC_CHANNEL_A, 100u, 0u));
    TEST_ASSERT(!coil_diagnostic_service_start(
        &service, 2u, COIL_DIAGNOSTIC_WHEEL_RIGHT,
        COIL_DIAGNOSTIC_CHANNEL_D, 100u, 0u));
    TEST_ASSERT_EQ(1u, capture.calls);
    TEST_ASSERT_EQ(1u, service.command_id);
    return 0;
}

static int test_deadline_handles_tick_wrap(void) {
    coil_diagnostic_service_t service;
    output_capture_t capture = {0};

    coil_diagnostic_service_init(&service, capture_output, &capture);
    TEST_ASSERT(coil_diagnostic_service_start(
        &service, 44u, COIL_DIAGNOSTIC_WHEEL_RIGHT,
        COIL_DIAGNOSTIC_CHANNEL_C, 100u, UINT32_MAX - 49u));

    coil_diagnostic_service_tick_1ms(&service, 49u);
    TEST_ASSERT(service.active);
    coil_diagnostic_service_tick_1ms(&service, 50u);
    TEST_ASSERT(!service.active);
    TEST_ASSERT_EQ(COIL_DIAGNOSTIC_RESULT_DONE, service.result);
    TEST_ASSERT_EQ(0u, capture.left);
    TEST_ASSERT_EQ(0u, capture.right);
    return 0;
}

int main(void) {
    TEST_ASSERT_EQ(0, test_left_channel_holds_until_deadline());
    TEST_ASSERT_EQ(0, test_right_channel_abort_releases_both_outputs());
    TEST_ASSERT_EQ(0, test_validation_and_busy_rejection());
    TEST_ASSERT_EQ(0, test_deadline_handles_tick_wrap());
    return 0;
}
