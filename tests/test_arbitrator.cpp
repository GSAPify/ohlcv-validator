#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "feed/arbitrator.h"
#include "feed/feed_protocol.h"
#include "replay/binary_format.h"

using ohlcv::feed::FeedArbitrator;
using ohlcv::feed::FeedPacket;
using ohlcv::feed::Line;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;

namespace {

// A packet whose payload carries its own sequence in trade_id, so a delivered
// record can be checked against the seq it was released under -- this is what
// proves there's no aliasing (slot reuse handing back the wrong message).
FeedPacket mk(std::uint64_t seq, Line line) {
    FeedPacket p{};
    p.seq = seq;
    p.line = static_cast<std::uint8_t>(line);
    p.record.type = static_cast<std::uint8_t>(RecordType::Trade);
    p.record.body.trade.trade_id = seq;
    return p;
}

// Collects (released_seq, payload_seq) pairs in delivery order.
struct Collector {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> got;
    void operator()(std::uint64_t seq, const WireRecord& r) {
        got.push_back({seq, r.body.trade.trade_id});
    }
    // The seqs released, in order.
    std::vector<std::uint64_t> seqs() const {
        std::vector<std::uint64_t> v;
        for (auto& [s, _] : got) v.push_back(s);
        return v;
    }
    // True iff every released record's payload matches the seq it was released
    // under (no aliasing / mis-delivery).
    bool consistent() const {
        for (auto& [s, payload] : got) if (s != payload) return false;
        return true;
    }
};

}  // namespace

TEST(Arbitrator, InOrderSingleLineDeliversAllOnce) {
    FeedArbitrator<16> arb;
    Collector c;
    for (std::uint64_t s = 0; s < 10; ++s) arb.offer(mk(s, Line::A), c);
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
    EXPECT_TRUE(c.consistent());
    EXPECT_EQ(arb.stats().delivered, 10U);
    EXPECT_EQ(arb.stats().gaps, 0U);
    EXPECT_EQ(arb.stats().duplicates[0] + arb.stats().duplicates[1], 0U);
}

TEST(Arbitrator, BothLinesDeliverEachSeqDeliveredOnceDupsCounted) {
    FeedArbitrator<16> arb;
    Collector c;
    for (std::uint64_t s = 0; s < 5; ++s) {
        arb.offer(mk(s, Line::A), c);
        arb.offer(mk(s, Line::B), c);  // redundant copy
    }
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(arb.stats().delivered, 5U);
    EXPECT_EQ(arb.stats().duplicates[1], 5U);  // B's copies were all duplicates
    EXPECT_EQ(arb.stats().gaps, 0U);
}

// The money test: each line drops half the stream, but between them every seq
// arrives exactly once -> no gaps, full in-order delivery.
TEST(Arbitrator, LineADropsLineBCoversNoGap) {
    FeedArbitrator<16> arb;
    Collector c;
    for (std::uint64_t s = 0; s < 10; ++s) {
        arb.offer(mk(s, (s % 2 == 0) ? Line::A : Line::B), c);  // evens on A, odds on B
    }
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
    EXPECT_TRUE(c.consistent());
    EXPECT_EQ(arb.stats().gaps, 0U);
    EXPECT_EQ(arb.stats().delivered, 10U);
}

TEST(Arbitrator, ReorderWithinWindowReleasedInOrder) {
    FeedArbitrator<16> arb;
    Collector c;
    arb.offer(mk(0, Line::A), c);  // delivered, frontier -> 1
    arb.offer(mk(2, Line::A), c);  // buffered (1 still missing)
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0}));  // 2 NOT yet released
    arb.offer(mk(1, Line::A), c);  // fills the hole -> 1 then 2 release
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0, 1, 2}));
    EXPECT_TRUE(c.consistent());
    EXPECT_EQ(arb.stats().gaps, 0U);
    EXPECT_GE(arb.stats().max_reorder, 1U);
}

TEST(Arbitrator, SeqLostOnBothLinesIsAGapAfterFlush) {
    FeedArbitrator<16> arb;
    Collector c;
    arb.offer(mk(0, Line::A), c);
    arb.offer(mk(1, Line::A), c);  // frontier -> 2
    arb.offer(mk(3, Line::A), c);  // 2 missing on both lines; 3 buffers
    arb.offer(mk(4, Line::A), c);
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0, 1}));  // stuck on the 2 hole
    arb.flush(c);  // give up waiting: 2 is a gap, then 3,4 drain
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0, 1, 3, 4}));
    EXPECT_EQ(arb.stats().gaps, 1U);
    EXPECT_TRUE(c.consistent());
}

// Advisor test (a): a duplicate of an already-released seq is dropped, not
// re-delivered.
TEST(Arbitrator, DuplicateBelowFrontierIsDroppedNotRedelivered) {
    FeedArbitrator<16> arb;
    Collector c;
    for (std::uint64_t s = 0; s < 3; ++s) arb.offer(mk(s, Line::A), c);  // frontier -> 3
    arb.offer(mk(1, Line::B), c);  // a late copy of an already-delivered seq
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0, 1, 2}));  // NOT re-delivered
    EXPECT_EQ(arb.stats().delivered, 3U);
    EXPECT_EQ(arb.stats().duplicates[1], 1U);
}

// The defining operational tradeoff of A/B arbitration: the reorder window must
// be larger than the inter-line skew. If line A races ahead while line B's copies
// are still in flight, a window smaller than that skew declares a FALSE gap -- B
// had the data, it just arrived after the window forced the frontier past it.
TEST(Arbitrator, WindowSmallerThanLineSkewCausesFalseGap) {
    // A delivers 0 then jumps to 5; B carries 1..4 but lagging, arriving only
    // after 5. Window 4 < the skew -> the hole at 1 is declared a gap before B's
    // copy of 1 lands, and that copy then falls below the frontier.
    FeedArbitrator<4> small;
    Collector cs;
    small.offer(mk(0, Line::A), cs);
    small.offer(mk(5, Line::A), cs);                 // forces frontier past 1
    for (std::uint64_t s = 1; s <= 4; ++s) small.offer(mk(s, Line::B), cs);
    small.flush(cs);
    EXPECT_GT(small.stats().gaps, 0U);               // false gap: B HAD it
    EXPECT_GE(small.stats().max_reorder, 3U);        // near Window=4 -> "size up" signal
    EXPECT_TRUE(cs.consistent());

    // Identical arrivals, a window comfortably larger than the skew -> no gap,
    // full in-order delivery. Same data, right-sized window, different outcome.
    FeedArbitrator<16> big;
    Collector cb;
    big.offer(mk(0, Line::A), cb);
    big.offer(mk(5, Line::A), cb);
    for (std::uint64_t s = 1; s <= 4; ++s) big.offer(mk(s, Line::B), cb);
    big.flush(cb);
    EXPECT_EQ(big.stats().gaps, 0U);
    EXPECT_EQ(cb.seqs(), (std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5}));
    EXPECT_TRUE(cb.consistent());
}

// Advisor test (b): a sequence far past the window forces a gap and is delivered
// without aliasing/corruption -- the seq-validated ring's reason to exist.
TEST(Arbitrator, FarAheadJumpForcesGapNoAliasing) {
    FeedArbitrator<8> arb;  // small window to force the overflow path
    Collector c;
    arb.offer(mk(0, Line::A), c);     // delivered, frontier -> 1
    arb.offer(mk(100, Line::A), c);   // 99 sequences ahead -> forces gaps 1..92
    arb.flush(c);                     // drain: gaps 93..99, then deliver 100
    EXPECT_EQ(c.seqs(), (std::vector<std::uint64_t>{0, 100}));
    EXPECT_TRUE(c.consistent());  // 100 came back as 100, not an aliased old slot
    EXPECT_EQ(arb.stats().delivered, 2U);
    EXPECT_EQ(arb.stats().gaps, 99U);  // seqs 1..99 all declared lost
}
