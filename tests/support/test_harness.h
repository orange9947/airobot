#ifndef ROBOT_TEST_HARNESS_H
#define ROBOT_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>

#define TEST_ASSERT(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "assertion failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition);                                        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

#define TEST_ASSERT_EQ(expected, actual) TEST_ASSERT((expected) == (actual))

#endif
