#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "concurrent/spsc_ring.h"

namespace {

using ohlcv::concurrent::SpscRing;

TEST(SpscRing, PopFromEmptyFails) {
    SpscRing<int, 4> ring;
    int out = -1;
    EXPECT_FALSE(ring.try_pop(out));
    EXPECT_EQ(out, -1);  // left untouched
}

TEST(SpscRing, FillToCapacityThenReject) {
    SpscRing<int, 4> ring;  // all 4 slots usable
    EXPECT_TRUE(ring.try_push(10));
    EXPECT_TRUE(ring.try_push(11));
    EXPECT_TRUE(ring.try_push(12));
    EXPECT_TRUE(ring.try_push(13));
    EXPECT_EQ(ring.size_approx(), 4u);
    EXPECT_FALSE(ring.try_push(14));  // full — no sacrificed slot
}

TEST(SpscRing, FifoOrder) {
    SpscRing<int, 8> ring;
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(ring.try_push(i));
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        EXPECT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_EQ(ring.size_approx(), 0u);
}

TEST(SpscRing, WrapsAroundPreservingOrder) {
    SpscRing<int, 4> ring;
    int out = -1;
    // Interleave well past Capacity so the masked index wraps many times.
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(ring.try_push(i));
        EXPECT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_EQ(ring.size_approx(), 0u);
}

// The race-freedom test. A producer pushes a strictly increasing sequence; the
// consumer must observe *exactly* that sequence — no drops, no duplicates, no
// reordering. A small ring forces heavy full/empty churn so both backpressure
// paths are exercised. Run under ThreadSanitizer (the project's `-DSANITIZER=
// thread` build) this also proves the acquire/release pairing is correct: TSan
// flags any unsynchronised access to the slot storage.
TEST(SpscRing, ThreadedStrictFifoNoLoss) {
    constexpr std::uint64_t kN = 1'000'000;
    SpscRing<std::uint64_t, 1024> ring;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kN; ++i) {
            while (!ring.try_push(i)) { /* spin: ring full */ }
        }
    });

    std::uint64_t popped   = 0;
    std::uint64_t expected = 0;
    bool          ordered  = true;
    while (popped < kN) {
        std::uint64_t v = 0;
        if (ring.try_pop(v)) {
            if (v != expected) ordered = false;
            ++expected;
            ++popped;
        }
    }
    producer.join();

    EXPECT_TRUE(ordered);     // strict FIFO, nothing reordered or duplicated
    EXPECT_EQ(popped, kN);    // nothing dropped
}

// The cached-cursor variant (CacheFarCursor = true) must obey the exact same
// contract — caching the far cursor is a performance change, not a semantic one.

TEST(SpscRingCached, FullEmptyAndFifo) {
    SpscRing<int, 4, 64, true> ring;
    int out = -1;
    EXPECT_FALSE(ring.try_pop(out));            // empty
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.try_push(i));
    EXPECT_FALSE(ring.try_push(99));            // full, all slots usable
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);                      // FIFO
    }
    EXPECT_FALSE(ring.try_pop(out));            // empty again
}

TEST(SpscRingCached, ThreadedStrictFifoNoLoss) {
    constexpr std::uint64_t kN = 1'000'000;
    SpscRing<std::uint64_t, 1024, 64, true> ring;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kN; ++i) {
            while (!ring.try_push(i)) { /* spin: ring full */ }
        }
    });

    std::uint64_t popped   = 0;
    std::uint64_t expected = 0;
    bool          ordered  = true;
    while (popped < kN) {
        std::uint64_t v = 0;
        if (ring.try_pop(v)) {
            if (v != expected) ordered = false;
            ++expected;
            ++popped;
        }
    }
    producer.join();

    EXPECT_TRUE(ordered);
    EXPECT_EQ(popped, kN);
}

}  // namespace
