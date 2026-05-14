#include "generic.h"
#include "test_harness.h"

// Edge cases:
// 1) update_bits with width 0 is a no-op
// 2) saturating increment stays at max
// 3) saturating decrement stays at min
// 4) normal increment/decrement within bounds

static void test_width_zero_noop() {
    unsigned v = 5;
    SaturatingCounter::update_bits(v, true, 0);
    EXPECT_EQ(v, 5u);
}

static void test_inc_saturates_at_max() {
    unsigned v = 3; // for width=2, max=3
    SaturatingCounter::update_bits(v, true, 2);
    EXPECT_EQ(v, 3u);
}

static void test_dec_saturates_at_min() {
    unsigned v = 0;
    SaturatingCounter::update_bits(v, false, 2);
    EXPECT_EQ(v, 0u);
}

static void test_inc_then_dec_within_bounds() {
    unsigned v = 1;
    SaturatingCounter::update_bits(v, true, 2);  // ->2
    EXPECT_EQ(v, 2u);
    SaturatingCounter::update_bits(v, false, 2); // ->1
    EXPECT_EQ(v, 1u);
}

int main() {
    test_width_zero_noop();
    test_inc_saturates_at_max();
    test_dec_saturates_at_min();
    test_inc_then_dec_within_bounds();
    return test_harness::finish();
}
