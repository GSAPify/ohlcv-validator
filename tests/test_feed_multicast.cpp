// Integration test for the UDP multicast transport (#1b) wired to the arbitrator
// (#1a), over loopback multicast -- no exchange, no external network. Sends a
// known sequence on two groups with a deliberate drop pattern and asserts the
// arbitrator reconstructs the true stream: single-line drops are covered by the
// other line; a sequence dropped on BOTH lines is the one real gap.
//
// Kept OUT of the default ctest set (built but not gtest_discover'd) because
// multicast isn't guaranteed on CI runners; run it locally:
//     ./build/tests/feed_multicast_test
//
// This is the transport analogue of the (CI-safe, deterministic) arbitrator unit
// tests: same logic, now proven over real sockets.

#include <fcntl.h>
#include <poll.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "feed/arbitrator.h"
#include "feed/feed_protocol.h"
#include "feed/udp_multicast.h"
#include "replay/binary_format.h"

using ohlcv::feed::FeedArbitrator;
using ohlcv::feed::FeedPacket;
using ohlcv::feed::Line;
using ohlcv::feed::McastReceiver;
using ohlcv::feed::McastSender;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;

namespace {
FeedPacket mk(std::uint64_t seq, Line line) {
    FeedPacket p{};
    p.seq = seq;
    p.line = static_cast<std::uint8_t>(line);
    p.record.type = static_cast<std::uint8_t>(RecordType::Trade);
    p.record.body.trade.trade_id = seq;  // self-identifying, to catch mis-delivery
    return p;
}
}  // namespace

TEST(FeedMulticast, ArbitratesTwoLinesOverLoopback) {
    const std::string ga = "239.255.0.20", gb = "239.255.0.21";
    constexpr std::uint16_t pa = 34020, pb = 34021;

    McastReceiver ra(ga, pa);
    McastReceiver rb(gb, pb);
    ::fcntl(ra.fd(), F_SETFL, ::fcntl(ra.fd(), F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(rb.fd(), F_SETFL, ::fcntl(rb.fd(), F_GETFL, 0) | O_NONBLOCK);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // membership settle

    McastSender sa(ga, pa);
    McastSender sb(gb, pb);

    constexpr std::uint64_t N = 30;
    // seq 9 dropped on BOTH lines -> the one true gap. seq 5 (A only) and seq 13
    // (B only) are single-line drops the other line must cover.
    auto drop_a = [](std::uint64_t s) { return s == 5 || s == 9; };
    auto drop_b = [](std::uint64_t s) { return s == 9 || s == 13; };
    for (std::uint64_t s = 0; s < N; ++s) {
        if (!drop_a(s)) ASSERT_TRUE(sa.send(mk(s, Line::A)));
        if (!drop_b(s)) ASSERT_TRUE(sb.send(mk(s, Line::B)));
    }

    FeedArbitrator<64> arb;
    std::vector<std::uint64_t> got;
    bool consistent = true;
    auto sink = [&](std::uint64_t seq, const WireRecord& r) {
        got.push_back(seq);
        if (seq != r.body.trade.trade_id) consistent = false;
    };

    pollfd fds[2] = {{ra.fd(), POLLIN, 0}, {rb.fd(), POLLIN, 0}};
    int idle = 0;
    while (idle < 3) {  // drain until both lines are quiet for ~300ms
        if (::poll(fds, 2, 100) <= 0) { ++idle; continue; }
        idle = 0;
        FeedPacket p;
        if (fds[0].revents & POLLIN) while (ra.recv(p)) arb.offer(p, sink);
        if (fds[1].revents & POLLIN) while (rb.recv(p)) arb.offer(p, sink);
    }
    arb.flush(sink);

    std::vector<std::uint64_t> expected;
    for (std::uint64_t s = 0; s < N; ++s) if (s != 9) expected.push_back(s);

    EXPECT_EQ(got, expected) << "every seq but the both-dropped 9 delivered, in order";
    EXPECT_TRUE(consistent) << "a delivered record's payload didn't match its seq";
    EXPECT_EQ(arb.stats().gaps, 1U) << "exactly one gap (seq 9, lost on both lines)";
    // 5 and 13 were single-line drops: the other line covered them, no gap.
    EXPECT_GT(arb.stats().duplicates[0] + arb.stats().duplicates[1], 0U);
}
