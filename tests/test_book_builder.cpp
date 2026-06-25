#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "book/book_builder.h"
#include "book/book_messages.h"
#include "book/order_book.h"

using ohlcv::book::BookBuilder;
using ohlcv::book::BookDelta;
using ohlcv::book::OrderBook;
using ohlcv::book::Side;
using ohlcv::book::SnapshotLevel;

namespace {

// A deterministic, internally-consistent stream of book deltas (seq 1..K)
// evolving a small book around 99/100.
std::vector<BookDelta> gen_deltas(std::uint64_t k) {
    std::vector<BookDelta> v;
    std::mt19937_64 rng(123);
    for (std::uint64_t seq = 1; seq <= k; ++seq) {
        BookDelta d{};
        d.seq = seq;
        const bool bid = (rng() & 1) != 0;
        d.side = static_cast<std::uint8_t>(bid ? Side::Bid : Side::Ask);
        const int lvl = static_cast<int>(rng() % 5);
        d.price = bid ? 99.0 - lvl * 0.01 : 100.0 + lvl * 0.01;
        d.new_size = (rng() % 4 == 0) ? 0 : (100 + rng() % 900);  // ~25% removals
        v.push_back(d);
    }
    return v;
}

// The from-scratch book after applying deltas[0 .. upto).
OrderBook<64> reference(const std::vector<BookDelta>& d, std::size_t upto) {
    OrderBook<64> b;
    for (std::size_t i = 0; i < upto; ++i) b.apply(d[i]);
    return b;
}

// A snapshot (level list) of a book's current state.
std::vector<SnapshotLevel> snapshot_of(const OrderBook<64>& b) {
    std::vector<SnapshotLevel> s;
    for (std::size_t i = 0; i < b.bid_levels(); ++i)
        s.push_back({b.bid(i).price, b.bid(i).size, static_cast<std::uint8_t>(Side::Bid), {}});
    for (std::size_t i = 0; i < b.ask_levels(); ++i)
        s.push_back({b.ask(i).price, b.ask(i).size, static_cast<std::uint8_t>(Side::Ask), {}});
    return s;
}

}  // namespace

TEST(BookBuilder, StartsRecoveringUntilFirstSnapshot) {
    BookBuilder<64> b;
    EXPECT_FALSE(b.is_live());
    BookDelta d{};
    d.seq = 1; d.side = static_cast<std::uint8_t>(Side::Bid); d.price = 99; d.new_size = 100;
    b.on_delta(d);                       // buffered, not applied (no book yet)
    EXPECT_FALSE(b.is_live());
    b.on_snapshot(0, std::vector<SnapshotLevel>{});  // empty snapshot @0
    EXPECT_TRUE(b.is_live());
    EXPECT_EQ(b.expected_seq(), 2U);     // the buffered delta 1 replayed -> next is 2
    EXPECT_EQ(b.book().best_bid(), 99.0);
}

TEST(BookBuilder, LiveDeltasApplyInOrder) {
    auto d = gen_deltas(20);
    BookBuilder<64> b;
    b.on_snapshot(0, std::vector<SnapshotLevel>{});
    for (std::size_t i = 0; i < 20; ++i) b.on_delta(d[i]);
    EXPECT_TRUE(b.is_live());
    EXPECT_EQ(b.stats().applied, 20U);
    EXPECT_TRUE(b.book() == reference(d, 20));
}

TEST(BookBuilder, GapInvalidatesBookUntilSnapshot) {
    auto d = gen_deltas(40);
    BookBuilder<64> b;
    b.on_snapshot(0, std::vector<SnapshotLevel>{});
    for (std::size_t i = 0; i < 20; ++i) b.on_delta(d[i]);   // seq 1..20 live

    // Gap: seq 21,22 lost on the wire. The next delta is seq 23.
    for (std::size_t i = 22; i < 40; ++i) b.on_delta(d[i]);  // seq 23..40 buffered
    EXPECT_FALSE(b.is_live());           // book is stale -- not to be trusted
    EXPECT_EQ(b.stats().gaps, 1U);
}

// THE headline: a stale snapshot (taken @22, while we'd already buffered 23..40)
// recovers to a book byte-identical to a from-scratch build of all 40 deltas --
// the lost 21,22 come back via the snapshot, the buffered 23..40 replay onto it.
TEST(BookBuilder, StaleSnapshotRecoversToByteIdenticalBook) {
    auto d = gen_deltas(40);
    const OrderBook<64> ref = reference(d, 40);

    BookBuilder<64> b;
    b.on_snapshot(0, std::vector<SnapshotLevel>{});
    for (std::size_t i = 0; i < 20; ++i) b.on_delta(d[i]);   // live 1..20
    for (std::size_t i = 22; i < 40; ++i) b.on_delta(d[i]);  // gap (21,22 lost), buffer 23..40
    ASSERT_FALSE(b.is_live());

    // Exchange snapshot valid as of seq 22 -- it INCLUDES the lost 21,22.
    b.on_snapshot(22, snapshot_of(reference(d, 22)));

    EXPECT_TRUE(b.is_live());
    EXPECT_EQ(b.stats().recovered, 2U);  // the initial sync + this gap recovery
    EXPECT_EQ(b.expected_seq(), 41U);
    EXPECT_TRUE(b.book() == ref) << "recovered book must equal the from-scratch build";
}

TEST(BookBuilder, TooOldSnapshotLeavesAHoleAndIsRejected) {
    auto d = gen_deltas(40);
    BookBuilder<64> b;
    b.on_snapshot(0, std::vector<SnapshotLevel>{});
    for (std::size_t i = 0; i < 20; ++i) b.on_delta(d[i]);

    // Two gaps while recovering: lose 21,22 (buffer 23..25), then lose 26..29
    // (next buffered is 30) -> the buffer has 23,24,25,30 with a hole at 26..29.
    b.on_delta(d[22]); b.on_delta(d[23]); b.on_delta(d[24]);  // seq 23,24,25
    b.on_delta(d[29]);                                        // seq 30 (26..29 lost)
    EXPECT_FALSE(b.is_live());

    // Snapshot @22 can't close the 26..29 hole -> rejected, stay Recovering.
    b.on_snapshot(22, snapshot_of(reference(d, 22)));
    EXPECT_FALSE(b.is_live());
    EXPECT_EQ(b.stats().snapshot_too_old, 1U);

    // A newer snapshot @29 covers the hole; only seq 30 replays. Now Live, and
    // the book matches a from-scratch build through 30.
    b.on_snapshot(29, snapshot_of(reference(d, 29)));
    EXPECT_TRUE(b.is_live());
    EXPECT_EQ(b.expected_seq(), 31U);
    EXPECT_TRUE(b.book() == reference(d, 30));
}

TEST(BookBuilder, StaleDuplicateBelowExpectedIsDropped) {
    auto d = gen_deltas(10);
    BookBuilder<64> b;
    b.on_snapshot(0, std::vector<SnapshotLevel>{});
    for (std::size_t i = 0; i < 5; ++i) b.on_delta(d[i]);     // seq 1..5, expected=6
    b.on_delta(d[2]);                                          // re-deliver seq 3
    EXPECT_EQ(b.stats().stale_dropped, 1U);
    EXPECT_EQ(b.expected_seq(), 6U);                           // unchanged
    EXPECT_TRUE(b.book() == reference(d, 5));                  // not re-applied
}
