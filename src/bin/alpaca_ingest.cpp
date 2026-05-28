#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include <spdlog/spdlog.h>

#include "ingest/alpaca_client.h"
#include "ingest/parser.h"
#include "util/timing.h"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) noexcept {
    g_stop.store(true, std::memory_order_relaxed);
}

std::string require_env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        spdlog::error("required env var {} is not set", name);
        std::exit(1);
    }
    return v;
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    ohlcv::ingest::AlpacaConfig cfg;
    cfg.key_id     = require_env("SECRET_ALPACA_API_KEY");
    cfg.secret_key = require_env("SECRET_ALPACA_API_SECRET");

    ohlcv::ingest::AlpacaClient client{std::move(cfg)};

    try {
        client.connect();
        spdlog::info("welcome: {}", client.read_frame());

        client.authenticate();
        spdlog::info("auth response: {}", client.read_frame());

        client.subscribe({"AAPL"}, {}, {"AAPL"});
        spdlog::info("subscription response: {}", client.read_frame());

        ohlcv::ingest::Parser parser;

        spdlog::info("entering read loop; Ctrl+C to stop");
        while (!g_stop.load(std::memory_order_relaxed)) {
            const auto arrival_ns = ohlcv::util::now_ns();
            const auto frame      = client.read_frame();
            const auto parsed     = parser.parse(frame);

            for (const auto& t : parsed.trades) {
                std::cout << "TRADE " << arrival_ns << ' ' << t.symbol
                          << " p=" << t.price << " s=" << t.size
                          << " x=" << t.exchange << " z=" << t.tape
                          << " ts=" << t.ts_ns << '\n';
            }
            for (const auto& b : parsed.bars) {
                std::cout << "BAR   " << arrival_ns << ' ' << b.symbol
                          << " o=" << b.open << " h=" << b.high
                          << " l=" << b.low  << " c=" << b.close
                          << " v=" << b.volume << " n=" << b.trade_count
                          << " vw=" << b.vwap << " ts=" << b.start_ns << '\n';
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
    spdlog::info("shut down cleanly");
    return 0;
}
