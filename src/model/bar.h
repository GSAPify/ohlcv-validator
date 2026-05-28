#pragma once

#include <cstdint>
#include <string>

namespace ohlcv::model {

using Timestamp = std::uint64_t;  // nanoseconds since Unix epoch

// std::string for symbol/exchange/tape is convenient for v1 and not yet a hot
// path. Once we measure, these get fixed-size buffers (most US tickers <= 5
// chars, so a 8-byte array fits with NUL and aligns nicely).
struct Trade {
    std::string   symbol;
    Timestamp     ts_ns      = 0;   // exchange timestamp
    std::uint64_t trade_id   = 0;
    double        price      = 0.0;
    std::uint64_t size       = 0;
    std::string   exchange;          // single-char codes in practice (e.g. "V")
    std::string   tape;              // "A", "B", or "C"
};

struct Bar {
    std::string   symbol;
    Timestamp     start_ns    = 0;   // bar start
    double        open        = 0.0;
    double        high        = 0.0;
    double        low         = 0.0;
    double        close       = 0.0;
    std::uint64_t volume      = 0;
    std::uint64_t trade_count = 0;
    double        vwap        = 0.0;
};

}  // namespace ohlcv::model
