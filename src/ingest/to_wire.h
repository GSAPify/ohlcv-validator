#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "model/bar.h"
#include "model/wire.h"

namespace ohlcv::ingest {

// Bridge the live JSON path to the validator. The parser lifts frames into the
// convenience types in bar.h (std::string symbols); the validator works on the
// fixed-layout wire types in wire.h. These adapters do that one conversion.
//
// `seq` is supplied by the caller, not the feed: Alpaca's IEX JSON carries no
// per-feed sequence number (trade `i` is an id, not a +1 monotonic counter), so
// the live path assigns a per-symbol monotonic seq. That makes sequence-gap
// detection structurally inert on this feed — there is no feed seq to diff — and
// the ingest summary says so rather than implying gaps simply never happened.

inline void set_symbol(char (&dst)[model::kSymbolLen], const std::string& sym) {
    std::memset(dst, 0, sizeof(dst));
    // Truncates symbols longer than the field and NUL-pads shorter ones; every
    // US equity ticker fits in 8 bytes.
    std::strncpy(dst, sym.c_str(), sizeof(dst));
}

inline model::WireTrade to_wire(const model::Trade& t, std::uint64_t seq) noexcept {
    model::WireTrade w{};
    set_symbol(w.symbol, t.symbol);
    w.ts_ns    = t.ts_ns;
    w.seq      = seq;
    w.trade_id = t.trade_id;
    w.price    = t.price;
    w.size     = t.size;
    w.exchange = t.exchange.empty() ? '\0' : t.exchange.front();
    w.tape     = t.tape.empty() ? '\0' : t.tape.front();
    return w;
}

inline model::WireBar to_wire(const model::Bar& b, std::uint64_t seq) noexcept {
    model::WireBar w{};
    set_symbol(w.symbol, b.symbol);
    w.start_ns    = b.start_ns;
    w.seq         = seq;
    w.open        = b.open;
    w.high        = b.high;
    w.low         = b.low;
    w.close       = b.close;
    w.vwap        = b.vwap;
    w.volume      = b.volume;
    w.trade_count = b.trade_count;
    return w;
}

inline model::WireQuote to_wire(const model::Quote& q, std::uint64_t seq) noexcept {
    model::WireQuote w{};
    set_symbol(w.symbol, q.symbol);
    w.ts_ns        = q.ts_ns;
    w.seq          = seq;
    w.bid_price    = q.bid_price;
    w.ask_price    = q.ask_price;
    w.bid_size     = q.bid_size;
    w.ask_size     = q.ask_size;
    w.bid_exchange = q.bid_exchange.empty() ? '\0' : q.bid_exchange.front();
    w.ask_exchange = q.ask_exchange.empty() ? '\0' : q.ask_exchange.front();
    w.tape         = q.tape.empty() ? '\0' : q.tape.front();
    return w;
}

}  // namespace ohlcv::ingest
