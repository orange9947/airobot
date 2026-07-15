#include <stdint.h>

#include "test_harness.h"
#include "w25q.h"

_Static_assert(W25Q_THREE_BYTE_ADDRESS_LIMIT == 0x01000000u,
               "three-byte commands must stop at 16 MiB");

static int test_capacity_is_clamped_to_three_byte_limit(void) {
    TEST_ASSERT_EQ(0u, w25q_addressable_capacity(0u));
    TEST_ASSERT_EQ(0x00100000u,
                   w25q_addressable_capacity(0x00100000u));
    TEST_ASSERT_EQ(W25Q_THREE_BYTE_ADDRESS_LIMIT,
                   w25q_addressable_capacity(
                       W25Q_THREE_BYTE_ADDRESS_LIMIT));
    TEST_ASSERT_EQ(W25Q_THREE_BYTE_ADDRESS_LIMIT,
                   w25q_addressable_capacity(0x02000000u));
    TEST_ASSERT_EQ(W25Q_THREE_BYTE_ADDRESS_LIMIT,
                   w25q_addressable_capacity(UINT32_MAX));
    return 0;
}

static int test_ranges_cannot_wrap_or_cross_capacity(void) {
    const uint32_t small_capacity = 0x00800000u;

    TEST_ASSERT(!w25q_range_is_addressable(small_capacity, 0u, 0u));
    TEST_ASSERT(w25q_range_is_addressable(small_capacity, 0u, 1u));
    TEST_ASSERT(w25q_range_is_addressable(
        small_capacity, small_capacity - 1u, 1u));
    TEST_ASSERT(!w25q_range_is_addressable(
        small_capacity, small_capacity - 1u, 2u));
    TEST_ASSERT(!w25q_range_is_addressable(
        small_capacity, small_capacity, 1u));
    TEST_ASSERT(!w25q_range_is_addressable(
        small_capacity, UINT32_MAX, 2u));
    return 0;
}

static int test_large_physical_flash_still_uses_only_low_16_mib(void) {
    const uint32_t reported_capacity = 0x02000000u;

    TEST_ASSERT(w25q_range_is_addressable(
        reported_capacity, W25Q_THREE_BYTE_ADDRESS_LIMIT - 1u, 1u));
    TEST_ASSERT(!w25q_range_is_addressable(
        reported_capacity, W25Q_THREE_BYTE_ADDRESS_LIMIT - 1u, 2u));
    TEST_ASSERT(!w25q_range_is_addressable(
        reported_capacity, W25Q_THREE_BYTE_ADDRESS_LIMIT, 1u));
    TEST_ASSERT(w25q_range_is_addressable(
        reported_capacity, W25Q_THREE_BYTE_ADDRESS_LIMIT - 4096u, 4096u));
    TEST_ASSERT(!w25q_range_is_addressable(
        reported_capacity, W25Q_THREE_BYTE_ADDRESS_LIMIT - 4095u, 4096u));
    TEST_ASSERT(w25q_range_is_addressable(
        reported_capacity, W25Q_THREE_BYTE_ADDRESS_LIMIT - 256u, 256u));
    return 0;
}

int main(void) {
    TEST_ASSERT_EQ(0, test_capacity_is_clamped_to_three_byte_limit());
    TEST_ASSERT_EQ(0, test_ranges_cannot_wrap_or_cross_capacity());
    TEST_ASSERT_EQ(0, test_large_physical_flash_still_uses_only_low_16_mib());
    return 0;
}
