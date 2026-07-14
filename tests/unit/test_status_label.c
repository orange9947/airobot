#include <string.h>

#include "protocol_ids.h"
#include "status_label.h"
#include "test_harness.h"

static int assert_layout(uint8_t state, const char *text,
                         uint8_t x, uint8_t width) {
    status_label_layout_t layout;

    TEST_ASSERT(status_label_for_state(state, &layout));
    TEST_ASSERT(strcmp(text, layout.text) == 0);
    TEST_ASSERT_EQ(x, layout.x);
    TEST_ASSERT_EQ(2u, layout.y);
    TEST_ASSERT_EQ(width, layout.width);
    TEST_ASSERT(layout.y + 5u <= 8u);
    return 0;
}

int main(void) {
    TEST_ASSERT_EQ(0, assert_layout(ROBOT_ROBOTSTATE_BOOT, "IDLE", 111u, 15u));
    TEST_ASSERT_EQ(0, assert_layout(ROBOT_ROBOTSTATE_SELF_TEST, "IDLE", 111u, 15u));
    TEST_ASSERT_EQ(0, assert_layout(ROBOT_ROBOTSTATE_IDLE, "IDLE", 111u, 15u));
    TEST_ASSERT_EQ(0, assert_layout(ROBOT_ROBOTSTATE_MANUAL, "MAN", 115u, 11u));
    TEST_ASSERT_EQ(0, assert_layout(ROBOT_ROBOTSTATE_AI, "AI", 119u, 7u));
    TEST_ASSERT_EQ(0, assert_layout(ROBOT_ROBOTSTATE_ESTOP, "STOP", 111u, 15u));
    TEST_ASSERT_EQ(0, assert_layout(ROBOT_ROBOTSTATE_FAULT, "ERR", 115u, 11u));
    return 0;
}
