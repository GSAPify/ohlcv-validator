#pragma once

#include <cstdint>
#include <type_traits>

namespace ohlcv::model {

// Fixed-layout POD record types — the hot-path representation.
//
// The Trade/Bar structs in bar.h use std::string for symbol because they back
// the live JSON ingest path, where convenience beats layout. These wire types
// are the opposite tradeoff: trivially-copyable, fixed-size, cache-friendly, and
// written verbatim to/read verbatim from the binary replay file. No heap, ever.
// This is the "fixed-size buffers once we measure" note in bar.h, cashed in.
//
// symbol is an 8-byte NUL-padded field: every US equity ticker is <= 5 chars,
// so 8 bytes holds the symbol with room to spare and keeps each record aligned.
// We also reinterpret those 8 bytes as a u64 key for the per-symbol state table,
// which is why the array is exactly 8 wide.

inline constexpr int kSymbolLen = 8;

struct WireTrade {
    char          symbol[kSymbolLen];  // NUL-padded ticker
    std::uint64_t ts_ns;               // exchange timestamp, ns since epoch
    std::uint64_t seq;                 // monotonic per-feed sequence number
    std::uint64_t trade_id;
    double        price;
    std::uint64_t size;
    char          exchange;            // single-char venue code, e.g. 'V'
    char          tape;                // 'A', 'B', or 'C'
    char          _pad[14];            // pad WireTrade out to a full cache line
};

struct WireBar {
    char          symbol[kSymbolLen];
    std::uint64_t start_ns;            // bar start, ns since epoch
    std::uint64_t seq;
    double        open;
    double        high;
    double        low;
    double        close;
    double        vwap;
    std::uint64_t volume;
    std::uint64_t trade_count;
};

static_assert(std::is_trivially_copyable_v<WireTrade>);
static_assert(std::is_trivially_copyable_v<WireBar>);
static_assert(sizeof(WireTrade) == 64, "WireTrade should stay one cache line");

}  // namespace ohlcv::model
