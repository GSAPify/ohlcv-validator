// feed_publisher -- the "exchange" side of the multicast demo.
//
// Generates one monotonically sequenced stream of trades and publishes each
// packet on BOTH redundant lines (multicast groups A and B), independently
// dropping a configurable fraction on each line to simulate packet loss. Between
// the two lines almost everything survives; a sequence lost on *both* is a real
// gap -- which is exactly what feed_handler's arbitrator must reconstruct and
// detect.
//
//   feed_publisher [count] [dropA_pct] [dropB_pct]
//   feed_publisher 20000 2 2          # 20k packets, ~2% loss per line
//
// The feed sequence (packet.seq, global, what the arbitrator dedups on) is
// deliberately distinct from each trade's per-symbol sequence (trade.seq, what
// the validator checks): an exchange's session sequence is not an instrument's.

#include <arpa/inet.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

#include "feed/feed_protocol.h"
#include "feed/udp_multicast.h"
#include "model/wire.h"
#include "replay/binary_format.h"

using ohlcv::feed::FeedPacket;
using ohlcv::feed::Line;
using ohlcv::feed::McastSender;
using ohlcv::replay::RecordType;

int main(int argc, char** argv) {
    const std::uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8000;
    const double drop_a = (argc > 2 ? std::atof(argv[2]) : 2.0) / 100.0;
    const double drop_b = (argc > 3 ? std::atof(argv[3]) : 2.0) / 100.0;

    const std::string group_a = "239.255.0.10";
    const std::string group_b = "239.255.0.11";
    constexpr std::uint16_t port_a = 34010, port_b = 34011;

    McastSender line_a(group_a, port_a);
    McastSender line_b(group_b, port_b);

    std::printf("publishing %llu packets  A=%s:%u  B=%s:%u  (drop A=%.1f%% B=%.1f%%)\n",
                static_cast<unsigned long long>(count), group_a.c_str(), port_a,
                group_b.c_str(), port_b, drop_a * 100, drop_b * 100);

    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    constexpr int kSyms = 4;
    const char* names[kSyms] = {"AAPL", "MSFT", "NVDA", "TSLA"};
    double px[kSyms] = {190.0, 420.0, 1200.0, 250.0};
    std::uint64_t sym_seq[kSyms] = {0, 0, 0, 0};  // per-symbol trade sequence
    std::uint64_t base_ns = 1'704'206'000'000'000'000ULL;

    std::uint64_t sent_a = 0, sent_b = 0;
    for (std::uint64_t seq = 0; seq < count; ++seq) {
        const int si = static_cast<int>(seq % kSyms);
        px[si] *= 1.0 + (unit(rng) - 0.5) / 50000.0;  // tiny random walk

        FeedPacket pkt{};
        pkt.seq = seq;  // global feed sequence
        pkt.record.type = static_cast<std::uint8_t>(RecordType::Trade);
        auto& t = pkt.record.body.trade;
        std::strncpy(t.symbol, names[si], sizeof(t.symbol));
        t.ts_ns = base_ns + seq * 1000;
        t.seq = ++sym_seq[si];  // per-symbol monotonic sequence (validator-clean)
        t.trade_id = seq;
        t.price = px[si];
        t.size = 100;
        t.exchange = 'V';
        t.tape = 'A';

        if (unit(rng) >= drop_a) { pkt.line = static_cast<std::uint8_t>(Line::A);
                                   if (line_a.send(pkt)) ++sent_a; }
        if (unit(rng) >= drop_b) { pkt.line = static_cast<std::uint8_t>(Line::B);
                                   if (line_b.send(pkt)) ++sent_b; }

        // Pace to a realistic feed rate (~32k msg/s). A tight unthrottled flood
        // overruns OS multicast-loopback delivery (especially on macOS), causing
        // false socket-level loss that has nothing to do with the 2% we model.
        if ((seq & 0x1F) == 0x1F) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::printf("sent A=%llu B=%llu of %llu sequences\n",
                static_cast<unsigned long long>(sent_a),
                static_cast<unsigned long long>(sent_b),
                static_cast<unsigned long long>(count));
    return 0;
}
