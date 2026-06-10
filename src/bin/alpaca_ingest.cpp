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
#include "ingest/parser.h"
#include "ingest/to_wire.h"
#include "util/timing.h"
#include "validate/validator.h"

namespace {

namespace v = ohlcv::validate;

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

// The violations worth surfacing on a *sampled* IEX feed. Reconstruction and
// sequence-gap are deliberately excluded: our IEX trades are a fraction of the
// consolidated volume, so they cannot rebuild Alpaca's full-market bars, and
// there is no feed sequence number to diff. Both are N/A here, not "clean" —
// the summary states that explicitly instead of printing misleading zeros.
struct Label {
    v::Violation bit;
    const char*  text;
};
constexpr std::array<Label, 10> kSurfaced{{
    {v::kTradeNonPositivePrice, "trade non-positive price"},
    {v::kTradeNonPositiveSize,  "trade non-positive size"},
    {v::kTimestampRegression,   "timestamp regression"},
    {v::kPriceBandBreach,       "price-band outlier"},
    {v::kBarLowAboveHigh,       "bar low > high"},
    {v::kBarOpenOutOfRange,     "bar open out of [low,high]"},
    {v::kBarCloseOutOfRange,    "bar close out of [low,high]"},
    {v::kBarVwapOutOfRange,     "bar vwap out of [low,high]"},
    {v::kBarNonPositivePrice,   "bar non-positive price"},
    {v::kBarVolumeInconsistent, "bar volume/trade-count inconsistent"},
}};

// Bitmask of the surfaced violations — used to decide whether a record is worth
// flagging inline (reconstruction/gap bits are ignored on this feed).
constexpr std::uint32_t surfaced_mask() noexcept {
    std::uint32_t m = 0;
    for (const auto& l : kSurfaced) m |= l.bit;
    return m;
}

std::string describe(std::uint32_t flags) {
    std::string out;
    for (const auto& l : kSurfaced) {
        if (flags & l.bit) {
            if (!out.empty()) out += ", ";
            out += l.text;
        }
    }
    return out;
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
    std::array<std::uint64_t, 10>                  counts{};  // parallel to kSurfaced
    std::uint64_t n_trades = 0, n_bars = 0, n_flagged = 0;

    const auto tally = [&](std::uint32_t flags) {
        for (std::size_t i = 0; i < kSurfaced.size(); ++i)
            if (flags & kSurfaced[i].bit) ++counts[i];
    };

    try {
        client.connect();
        spdlog::info("welcome: {}", client.read_frame());

        client.authenticate();
        spdlog::info("auth response: {}", client.read_frame());

        client.subscribe({"AAPL"}, {}, {"AAPL"});
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
                tally(r.flags);
                std::cout << "TRADE " << arrival_ns << ' ' << t.symbol
                          << " p=" << t.price << " s=" << t.size
                          << " ts=" << t.ts_ns << '\n';
                if (r.flags & surfaced_mask()) {
                    ++n_flagged;
                    std::cout << "  !! " << t.symbol << ": " << describe(r.flags)
                              << '\n';
                }
            }
            for (const auto& b : parsed.bars) {
                if (b.symbol.empty()) continue;
                const std::uint64_t seq = ++next_seq[b.symbol];
                const auto r = validator.check(ohlcv::ingest::to_wire(b, seq));
                ++n_bars;
                tally(r.flags);
                std::cout << "BAR   " << arrival_ns << ' ' << b.symbol
                          << " o=" << b.open << " h=" << b.high
                          << " l=" << b.low  << " c=" << b.close
                          << " v=" << b.volume << " vw=" << b.vwap << '\n';
                if (r.flags & surfaced_mask()) {
                    ++n_flagged;
                    std::cout << "  !! " << b.symbol << ": " << describe(r.flags)
                              << '\n';
                }
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
              << "validated " << n_trades << " trades, " << n_bars
              << " bars; " << n_flagged << " records flagged\n";
    for (std::size_t i = 0; i < kSurfaced.size(); ++i)
        if (counts[i] != 0)
            std::cout << "  " << kSurfaced[i].text << ": " << counts[i] << '\n';
    if (n_flagged == 0)
        std::cout << "  (no anomalies — expected on a clean feed)\n";
    std::cout << "note: reconstruction + sequence-gap checks are N/A on the IEX "
                 "sample (need a full feed + feed seq); quote crossed/locked + "
                 "mid-outlier checks await quote subscription (next step).\n";

    spdlog::info("shut down cleanly");
    return 0;
}
