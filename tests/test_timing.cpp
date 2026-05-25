#include <gtest/gtest.h>

#include "util/timing.h"

using ohlcv::util::now_ns;

TEST(Timing, NonDecreasingBetweenConsecutiveCalls) {
    const auto a = now_ns();
    const auto b = now_ns();
    EXPECT_GE(b, a);
}

TEST(Timing, AdvancesAcrossManyCalls) {
    const auto start = now_ns();
    for (int i = 0; i < 100'000; ++i) {
        (void)now_ns();
    }
    const auto end = now_ns();
    EXPECT_GT(end, start);
}
