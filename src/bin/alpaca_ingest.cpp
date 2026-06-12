#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "ingest/alpaca_client.h"
#include "ingest/live_report.h"
#include "ingest/parser.h"
#include "ingest/to_wire.h"
#include "util/timing.h"
#include "validate/validator.h"

namespace {

namespace v  = ohlcv::validate;
namespace ig = ohlcv::ingest;

std::atomic<bool> g_stop{false};

void on_signal(int) noexcept {
    g_stop.store(true, std::memory_order_relaxed);
}

std::string require_env(const char* name) {
    const char* val = std::getenv(name);
    if (val == nullptr || *val == '\0') {
        spdlog::error("required env var {} is not set", name);
        std::exit(1);
    }
    return val;
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    ohlcv::ingest::AlpacaConfig cfg;
    cfg.key_id     = require_env("SECRET_ALPACA_API_KEY");
    cfg.secret_key = require_env("SECRET_ALPACA_API_SECRET");

    ohlcv::ingest::AlpacaClient client{std::move(cfg)};

    // Live validation state. The validator is zero-alloc after construction; the
    // per-symbol seq counter stands in for the feed sequence the IEX JSON lacks.
    v::Validator                                   validator;
    std::unordered_map<std::string, std::uint64_t> next_seq;
    std::array<std::uint64_t, ig::kLiveLabels.size()> counts{};  // parallel to kLiveLabels
    std::uint64_t n_trades = 0, n_bars = 0, n_quotes = 0, n_flagged = 0;

    // Tally the surfaced (clock-independent value) violations and print a flag
    // line if any fired. Suppressed bits (reconstruction/gap/ts-regression) are
    // outside kSurfacedMask, so they're never counted or printed — see
    // live_report.h for why each is N/A on this feed.
    const auto record = [&](const std::string& sym, std::uint32_t flags) {
        for (std::size_t i = 0; i < ig::kLiveLabels.size(); ++i)
            if (flags & ig::kSurfacedMask & static_cast<std::uint32_t>(ig::kLiveLabels[i].bit))
                ++counts[i];
        if (flags & ig::kSurfacedMask) {
            ++n_flagged;
            std::cout << "  !! " << sym << ": " << ig::describe(flags) << '\n';
        }
    };

    try {
        client.connect();
        spdlog::info("welcome: {}", client.read_frame());

        client.authenticate();
        spdlog::info("auth response: {}", client.read_frame());

        client.subscribe({"AAPL"}, {"AAPL"}, {"AAPL"});
        spdlog::info("subscription response: {}", client.read_frame());

        ohlcv::ingest::Parser parser;

        spdlog::info("entering read loop; validating inline; Ctrl+C to stop");
        while (!g_stop.load(std::memory_order_relaxed)) {
            const auto arrival_ns = ohlcv::util::now_ns();
            const auto frame      = client.read_frame();
            const auto parsed     = parser.parse(frame);

            for (const auto& t : parsed.trades) {
                if (t.symbol.empty()) continue;  // no symbol → can't key state
                const std::uint64_t seq = ++next_seq[t.symbol];
                const auto r = validator.check(ohlcv::ingest::to_wire(t, seq));
                ++n_trades;
                std::cout << "TRADE " << arrival_ns << ' ' << t.symbol
                          << " p=" << t.price << " s=" << t.size
                          << " ts=" << t.ts_ns << '\n';
                record(t.symbol, r.flags);
            }
            for (const auto& q : parsed.quotes) {
                if (q.symbol.empty()) continue;
                const std::uint64_t seq = ++next_seq[q.symbol];
                const auto r = validator.check(ohlcv::ingest::to_wire(q, seq));
                ++n_quotes;
                std::cout << "QUOTE " << arrival_ns << ' ' << q.symbol
                          << " b=" << q.bid_price << 'x' << q.bid_size
                          << " a=" << q.ask_price << 'x' << q.ask_size
                          << " ts=" << q.ts_ns << '\n';
                record(q.symbol, r.flags);
            }
            for (const auto& b : parsed.bars) {
                if (b.symbol.empty()) continue;
                const std::uint64_t seq = ++next_seq[b.symbol];
                const auto r = validator.check(ohlcv::ingest::to_wire(b, seq));
                ++n_bars;
                std::cout << "BAR   " << arrival_ns << ' ' << b.symbol
                          << " o=" << b.open << " h=" << b.high
                          << " l=" << b.low  << " c=" << b.close
                          << " v=" << b.volume << " vw=" << b.vwap << '\n';
                record(b.symbol, r.flags);
            }
            for (const auto& e : parsed.errors) {
                spdlog::error("alpaca error: {}", e);
            }
            for (const auto& u : parsed.unknown_types) {
                spdlog::warn("unhandled message type: {}", u);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("fatal: {}", e.what());
        client.close();
        return 1;
    }

    client.close();

    // Honest summary. A correctness validator on clean vendor data is *supposed*
    // to be quiet — silence here is the result, not a letdown.
    std::cout << "\n--- validation summary ---\n"
              << "validated " << n_trades << " trades, " << n_quotes
              << " quotes, " << n_bars << " bars; " << n_flagged
              << " records flagged\n";
    for (std::size_t i = 0; i < ig::kLiveLabels.size(); ++i)
        if (counts[i] != 0)
            std::cout << "  " << ig::kLiveLabels[i].text << ": " << counts[i] << '\n';
    if (n_flagged == 0)
        std::cout << "  (no anomalies — expected on a clean feed)\n";
    std::cout << "note: reconstruction + sequence-gap are N/A on the IEX sample "
                 "(partial volume, no feed seq); timestamp-regression is suppressed "
                 "live (trades/quotes/bars share one per-symbol clock, so it isn't "
                 "a cross-stream property). Surfaced: trade/bar/quote value checks "
                 "incl. crossed/locked books and quote-mid outliers.\n";

    spdlog::info("shut down cleanly");
    return 0;
}
