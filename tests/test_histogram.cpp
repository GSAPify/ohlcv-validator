#include <gtest/gtest.h>

#include "util/histogram.h"

using ohlcv::util::LatencyHistogram;

TEST(Histogram, EmptyReturnsZeroes) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.percentile(0.5), 0U);
    EXPECT_EQ(h.min(), 0U);
    EXPECT_EQ(h.max(), 0U);
}

TEST(Histogram, BasicStats) {
    LatencyHistogram h;
    for (std::uint64_t ns = 1; ns <= 100; ++ns) h.record(ns);
    EXPECT_EQ(h.count(), 100U);
    EXPECT_EQ(h.min(), 1U);
    EXPECT_EQ(h.max(), 100U);
    EXPECT_DOUBLE_EQ(h.mean(), 50.5);
}

TEST(Histogram, PercentilesAreMonotonic) {
    LatencyHistogram h;
    for (std::uint64_t ns = 1; ns <= 1000; ++ns) h.record(ns);
    EXPECT_LE(h.percentile(0.5),  h.percentile(0.9));
    EXPECT_LE(h.percentile(0.9),  h.percentile(0.99));
    EXPECT_EQ(h.percentile(0.5),  500U);
    EXPECT_EQ(h.percentile(0.99), 990U);
}

TEST(Histogram, OverflowCountedButCapped) {
    LatencyHistogram h(/*max_ns=*/100);
    h.record(50);
    h.record(1'000'000);  // well past max_ns
    EXPECT_EQ(h.count(), 2U);
    EXPECT_EQ(h.overflow(), 1U);
    EXPECT_EQ(h.max(), 1'000'000U);  // true tail still tracked
}
