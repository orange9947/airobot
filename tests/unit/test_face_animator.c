#include <stdint.h>

#include "face_animator.h"
#include "protocol_ids.h"
#include "test_harness.h"

static int test_schedules_and_pose_changes(void) {
    face_animator_t animator;
    int8_t first_gaze;
    uint8_t first_mouth;

    face_animator_init(&animator, 0x12345678u, ROBOT_EXPRESSION_HAPPY, 100u);
    TEST_ASSERT_EQ(ROBOT_EXPRESSION_HAPPY, animator.expression);
    TEST_ASSERT(!animator.pose.blinking);
    TEST_ASSERT(animator.pose.gaze >= -1 && animator.pose.gaze <= 1);
    TEST_ASSERT(animator.pose.mouth_variant < 3u);
    TEST_ASSERT(animator.next_blink_ms >= 2100u && animator.next_blink_ms <= 6100u);
    TEST_ASSERT(animator.next_gaze_ms >= 3100u && animator.next_gaze_ms <= 7100u);
    TEST_ASSERT(animator.next_mouth_ms >= 2600u && animator.next_mouth_ms <= 5100u);
    TEST_ASSERT(!face_animator_tick(&animator, 1000u));

    TEST_ASSERT(face_animator_tick(&animator, animator.next_blink_ms));
    TEST_ASSERT(animator.pose.blinking);
    TEST_ASSERT(animator.blink_end_ms >= animator.next_blink_ms + 160u);
    TEST_ASSERT(animator.blink_end_ms <= animator.next_blink_ms + 240u);
    TEST_ASSERT(face_animator_tick(&animator, animator.blink_end_ms));
    TEST_ASSERT(!animator.pose.blinking);

    first_gaze = animator.pose.gaze;
    TEST_ASSERT(face_animator_tick(&animator, animator.next_gaze_ms));
    TEST_ASSERT(animator.pose.gaze != first_gaze);
    first_mouth = animator.pose.mouth_variant;
    TEST_ASSERT(face_animator_tick(&animator, animator.next_mouth_ms));
    TEST_ASSERT(animator.pose.mouth_variant != first_mouth);
    TEST_ASSERT_EQ(ROBOT_EXPRESSION_HAPPY, animator.expression);
    return 0;
}

static int test_expression_and_safety_override(void) {
    face_animator_t animator;
    face_pose_t pose;

    face_animator_init(&animator, 0u, ROBOT_EXPRESSION_NEUTRAL, 0u);
    TEST_ASSERT(face_animator_set_expression(
        &animator, ROBOT_EXPRESSION_THINKING, 50u));
    TEST_ASSERT_EQ(ROBOT_EXPRESSION_THINKING, animator.expression);
    pose = animator.pose;

    TEST_ASSERT(face_animator_set_expression(
        &animator, ROBOT_EXPRESSION_ESTOP, 60u));
    TEST_ASSERT_EQ(ROBOT_EXPRESSION_ESTOP, animator.expression);
    TEST_ASSERT(!animator.pose.blinking);
    TEST_ASSERT(!face_animator_tick(&animator, 100000u));
    TEST_ASSERT_EQ(ROBOT_EXPRESSION_ESTOP, animator.expression);

    TEST_ASSERT(face_animator_set_expression(
        &animator, ROBOT_EXPRESSION_THINKING, 100100u));
    TEST_ASSERT_EQ(ROBOT_EXPRESSION_THINKING, animator.expression);
    TEST_ASSERT(!animator.pose.blinking);
    TEST_ASSERT(pose.gaze >= -1 && pose.gaze <= 1);
    return 0;
}

int main(void) {
    TEST_ASSERT_EQ(0, test_schedules_and_pose_changes());
    TEST_ASSERT_EQ(0, test_expression_and_safety_override());
    return 0;
}
