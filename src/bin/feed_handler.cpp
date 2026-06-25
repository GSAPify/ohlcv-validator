// feed_handler -- the receiver side of the multicast demo.
//
// Joins both redundant lines (multicast groups A and B), polls them in a single
// thread (so the arbitrator stays lock-free), and feeds every datagram to the
// FeedArbitrator. The reconstructed in-order stream is handed to the Validator,
// exactly as a real handler feeds normalized messages downstream. On idle (the
// publisher has stopped) it flushes and prints the arbitration + validation tally.
//
//   feed_handler                       # run until the feed goes idle, then report
//
// Single thread + poll() is deliberate: one core draining both lines is the shape
// a real feed handler takes, and it keeps the arbitrator a plain SPSC consumer.

#include <fcntl.h>
#include <poll.h>

#include <chrono>
#include <cstdint>
#include <cstdio>

#include "feed/arbitrator.h"
#include "feed/feed_protocol.h"
#include "feed/udp_multicast.h"
#include "replay/binary_format.h"
#include "validate/validator.h"

using ohlcv::feed::FeedArbitrator;
using ohlcv::feed::FeedPacket;
using ohlcv::feed::McastReceiver;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;
using ohlcv::validate::Validator;

namespace {
void set_nonblocking_with_big_buffer(int fd) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rcvbuf = 8 * 1024 * 1024;  // absorb bursts so loss is the publisher's, not ours
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
}
}  // namespace

int main() {
    McastReceiver line_a("239.255.0.10", 34010);
    McastReceiver line_b("239.255.0.11", 34011);
    set_nonblocking_with_big_buffer(line_a.fd());
    set_nonblocking_with_big_buffer(line_b.fd());

    // 4096-deep reorder window: ample for the inter-line skew at feed rates.
    // (max_reorder can still read high here because this handler drains all of
    // line A before line B, so the frontier lags by line A's backlog -- cosmetic,
    // delivery is unaffected; a production handler would interleave the drain.)
    FeedArbitrator<4096> arb;
    Validator validator;
    std::uint64_t violations = 0;

    auto sink = [&](std::uint64_t /*seq*/, const WireRecord& r) {
        switch (static_cast<RecordType>(r.type)) {
            case RecordType::Trade:
                violations += (validator.check(r.body.trade).flags != 0); break;
            case RecordType::Bar:
                violations += (validator.check(r.body.bar).flags != 0); break;
            case RecordType::Quote:
                violations += (validator.check(r.body.quote).flags != 0); break;
        }
    };

    std::printf("feed_handler listening on A=239.255.0.10:34010 B=239.255.0.11:34011\n");

    // Busy-drain both lines continuously -- a real feed handler can't afford to
    // sleep while a burst is arriving (an earlier sleep-poll version overflowed
    // the socket buffers and reported tens of thousands of false gaps). Each
    // iteration drains everything currently available on both sockets; only when
    // both are empty do we poll(1ms) to avoid a 100% spin, and we give up after a
    // sustained idle period.
    using clock = std::chrono::steady_clock;
    pollfd fds[2] = {{line_a.fd(), POLLIN, 0}, {line_b.fd(), POLLIN, 0}};
    bool seen = false;                                   // have we seen the stream start?
    auto last_data = clock::now();
    constexpr auto kEndOfStream = std::chrono::milliseconds(800);   // idle after data = done
    constexpr auto kStartupWait = std::chrono::seconds(10);         // wait this long for it to begin

    for (;;) {
        ::poll(fds, 2, 1);  // wait up to 1ms for either line, then drain BOTH fully
        bool got = false;
        FeedPacket pkt;
        while (line_a.recv(pkt)) { arb.offer(pkt, sink); got = true; }
        while (line_b.recv(pkt)) { arb.offer(pkt, sink); got = true; }
        if (got) { seen = true; last_data = clock::now(); continue; }
        const auto idle = clock::now() - last_data;
        if (seen ? idle > kEndOfStream : idle > kStartupWait) break;
    }

    arb.flush(sink);

    const auto& s = arb.stats();
    std::printf("\n=== arbitration ===\n");
    std::printf("  delivered (in order) : %llu\n", (unsigned long long)s.delivered);
    std::printf("  duplicates  A / B    : %llu / %llu  (redundant copies dropped)\n",
                (unsigned long long)s.duplicates[0], (unsigned long long)s.duplicates[1]);
    std::printf("  gaps (lost on both)  : %llu\n", (unsigned long long)s.gaps);
    std::printf("  max reorder depth    : %llu  (size the window above this)\n",
                (unsigned long long)s.max_reorder);
    std::printf("=== validation of the reconstructed stream ===\n");
    std::printf("  violations           : %llu\n", (unsigned long long)violations);
    return 0;
}
