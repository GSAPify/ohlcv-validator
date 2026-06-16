#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "ingest/alpaca_client.h"
#include "ingest/live_report.h"
#include "ingest/parser.h"
#include "ingest/to_wire.h"
#include "ingest/violation_log.h"
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

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Symbols to validate come from the command line (positional); default to a
    // single name. One symbol → "watch" mode (print every record); many symbols
    // → quiet mode (stdout shows only violations + the summary, since printing
    // every trade/quote/bar across many names is an unreadable firehose — the
    // structured log below carries the full record stream instead).
    std::vector<std::string> symbols;
    for (int i = 1; i < argc; ++i) symbols.emplace_back(argv[i]);
    if (symbols.empty()) symbols = {"AAPL"};
    const bool verbose = symbols.size() == 1;

    // Optional machine-readable violations log (JSONL). Off unless the env var
    // points somewhere; the human `!!` lines on stdout are unaffected.
    std::ofstream vlog;
    if (const char* path = std::getenv("OHLCV_VIOLATIONS_LOG"); path && *path) {
        vlog.open(path, std::ios::app);
        if (!vlog) spdlog::warn("cannot open violations log {}; continuing without it", path);
    }

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

    // Tally the surfaced (clock-independent value) violations, print the human
    // flag line, and append a structured record to the log if one is open.
    // Suppressed bits (reconstruction/gap/ts-regression) are outside
    // kSurfacedMask, so they never count, print, or get logged — see
    // live_report.h for why each is N/A on this feed.
    const auto record = [&](std::string_view kind, const std::string& sym,
                            std::uint64_t ts, std::uint64_t seq,
                            std::uint32_t flags) {
        for (std::size_t i = 0; i < ig::kLiveLabels.size(); ++i)
            if (flags & ig::kSurfacedMask & static_cast<std::uint32_t>(ig::kLiveLabels[i].bit))
                ++counts[i];
        if (flags & ig::kSurfacedMask) {
            ++n_flagged;
            std::cout << "  !! " << sym << ": " << ig::describe(flags) << '\n';
            if (vlog.is_open())
                vlog << ig::violation_json(kind, sym, ts, seq, flags) << '\n';
        }
    };

    try {
        client.connect();
        spdlog::info("welcome: {}", client.read_frame());

        client.authenticate();
        spdlog::info("auth response: {}", client.read_frame());

        client.subscribe(symbols, symbols, symbols);
        spdlog::info("subscription response: {}", client.read_frame());

        ohlcv::ingest::Parser parser;

        spdlog::info("validating {} symbol(s) inline ({} mode){}; Ctrl+C to stop",
                     symbols.size(), verbose ? "watch" : "quiet — violations only",
                     vlog.is_open() ? "; logging violations to JSONL" : "");
        while (!g_stop.load(std::memory_order_relaxed)) {
            const auto arrival_ns = ohlcv::util::now_ns();
            const auto frame      = client.read_frame();
            const auto parsed     = parser.parse(frame);

            for (const auto& t : parsed.trades) {
                if (t.symbol.empty()) continue;  // no symbol → can't key state
                const std::uint64_t seq = ++next_seq[t.symbol];
                const auto r = validator.check(ohlcv::ingest::to_wire(t, seq));
                ++n_trades;
                if (verbose)
                    std::cout << "TRADE " << arrival_ns << ' ' << t.symbol
                              << " p=" << t.price << " s=" << t.size
                              << " ts=" << t.ts_ns << '\n';
                record("trade", t.symbol, t.ts_ns, seq, r.flags);
            }
            for (const auto& q : parsed.quotes) {
                if (q.symbol.empty()) continue;
                const std::uint64_t seq = ++next_seq[q.symbol];
                const auto r = validator.check(ohlcv::ingest::to_wire(q, seq));
                ++n_quotes;
                if (verbose)
                    std::cout << "QUOTE " << arrival_ns << ' ' << q.symbol
                              << " b=" << q.bid_price << 'x' << q.bid_size
                              << " a=" << q.ask_price << 'x' << q.ask_size
                              << " ts=" << q.ts_ns << '\n';
                record("quote", q.symbol, q.ts_ns, seq, r.flags);
            }
            for (const auto& b : parsed.bars) {
                if (b.symbol.empty()) continue;
                const std::uint64_t seq = ++next_seq[b.symbol];
                const auto r = validator.check(ohlcv::ingest::to_wire(b, seq));
                ++n_bars;
                if (verbose)
                    std::cout << "BAR   " << arrival_ns << ' ' << b.symbol
                              << " o=" << b.open << " h=" << b.high
                              << " l=" << b.low  << " c=" << b.close
                              << " v=" << b.volume << " vw=" << b.vwap << '\n';
                record("bar", b.symbol, b.start_ns, seq, r.flags);
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
                 "(partial volume, no feed seq); timestamp-regression is now "
                 "per-stream and correct, but stays suppressed live pending a "
                 "measurement that it doesn't false-flag on bursty/reordered "
                 "delivery. Surfaced: trade/bar/quote value checks incl. "
                 "crossed/locked books and quote-mid outliers.\n";

    spdlog::info("shut down cleanly");
    return 0;
}
