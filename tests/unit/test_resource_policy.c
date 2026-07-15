#include "resource_policy.h"
#include "test_harness.h"

int main(void) {
    storage_service_state_t state;

    TEST_ASSERT(resource_policy_can_begin(
        ROBOT_STATE_IDLE, STORAGE_SERVICE_IDLE, true, false));
    TEST_ASSERT(resource_policy_can_begin(
        ROBOT_STATE_ESTOP, STORAGE_SERVICE_IDLE, true, false));
    TEST_ASSERT(!resource_policy_can_begin(
        ROBOT_STATE_MANUAL, STORAGE_SERVICE_IDLE, true, false));
    TEST_ASSERT(!resource_policy_can_begin(
        ROBOT_STATE_IDLE, STORAGE_SERVICE_IDLE, false, false));
    TEST_ASSERT(!resource_policy_can_begin(
        ROBOT_STATE_IDLE, STORAGE_SERVICE_IDLE, true, true));

    for (state = STORAGE_SERVICE_ERASING;
         state <= STORAGE_SERVICE_COMMITTING;
         state = (storage_service_state_t)(state + 1)) {
        TEST_ASSERT(resource_policy_update_active(state));
        TEST_ASSERT(resource_policy_allows_mode(state, ROBOT_STATE_IDLE));
        TEST_ASSERT(!resource_policy_allows_mode(state, ROBOT_STATE_MANUAL));
        TEST_ASSERT(!resource_policy_allows_mode(state, ROBOT_STATE_AI));
        TEST_ASSERT(!resource_policy_allows_move(state));
    }

    TEST_ASSERT(!resource_policy_update_active(STORAGE_SERVICE_IDLE));
    TEST_ASSERT(resource_policy_allows_mode(
        STORAGE_SERVICE_IDLE, ROBOT_STATE_MANUAL));
    TEST_ASSERT(resource_policy_allows_move(STORAGE_SERVICE_IDLE));
    TEST_ASSERT(resource_policy_update_active(STORAGE_SERVICE_ABORTED));
    TEST_ASSERT(resource_policy_update_active(STORAGE_SERVICE_FAILED));
    TEST_ASSERT(!resource_policy_allows_move(STORAGE_SERVICE_ABORTED));
    TEST_ASSERT(!resource_policy_allows_move(STORAGE_SERVICE_FAILED));
    return 0;
}
