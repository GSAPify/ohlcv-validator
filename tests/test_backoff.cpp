#include <chrono>

#include <gtest/gtest.h>

#include "util/backoff.h"

using ohlcv::util::backoff_ceiling;
using std::chrono::milliseconds;

namespace {

TEST(Backoff, DoublesEachAttemptThenCaps) {
    const auto base = milliseconds{100};
    const auto cap  = milliseconds{2000};
    EXPECT_EQ(backoff_ceiling(0, base, cap), milliseconds{100});
    EXPECT_EQ(backoff_ceiling(1, base, cap), milliseconds{200});
    EXPECT_EQ(backoff_ceiling(2, base, cap), milliseconds{400});
    EXPECT_EQ(backoff_ceiling(3, base, cap), milliseconds{800});
    EXPECT_EQ(backoff_ceiling(4, base, cap), milliseconds{1600});
    EXPECT_EQ(backoff_ceiling(5, base, cap), cap);  // 3200 → capped
    EXPECT_EQ(backoff_ceiling(9, base, cap), cap);
}

// Large attempts must saturate to the cap, never overflow into a tiny/negative
// delay (which would turn backoff into a hammer).
TEST(Backoff, LargeAttemptSaturatesNoOverflow) {
    const auto base = milliseconds{250};
    const auto cap  = milliseconds{30000};
    EXPECT_EQ(backoff_ceiling(31, base, cap), cap);
    EXPECT_EQ(backoff_ceiling(63, base, cap), cap);
    EXPECT_EQ(backoff_ceiling(1000, base, cap), cap);
}

}  // namespace
