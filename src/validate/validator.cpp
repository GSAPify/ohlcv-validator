#include "validate/validator.h"

#include <cstring>

namespace ohlcv::validate {

namespace {

// Reinterpret the 8-byte symbol as a u64 key. memcpy (not a cast) keeps this
// well-defined and the compiler lowers it to a single load. The all-zero symbol
// can't occur for a real ticker, so 0 is a safe "empty slot" sentinel.
std::uint64_t symbol_key(const char (&symbol)[model::kSymbolLen]) noexcept {
    std::uint64_t k;
    std::memcpy(&k, symbol, sizeof(k));
    return k;
}

}  // namespace

Validator::Slot* Validator::slot_for(
    const char (&symbol)[model::kSymbolLen]) noexcept {
    const std::uint64_t key = symbol_key(symbol);
    std::size_t idx = static_cast<std::size_t>(key) & kMask;

    // Linear probe. Bounded by kCapacity so a full table terminates instead of
    // looping forever.
    for (std::size_t probes = 0; probes < kCapacity; ++probes) {
        Slot& s = table_[idx];
        if (s.key == key && s.seen) {
            return &s;  // existing symbol
        }
        if (!s.seen) {
            s.key = key;  // claim this empty slot
            return &s;
        }
        idx = (idx + 1) & kMask;
    }
    return nullptr;  // table full
}

void Validator::check_sequencing(const char (&symbol)[model::kSymbolLen],
                                 std::uint64_t ts, std::uint64_t seq,
                                 Result& out) noexcept {
    Slot* s = slot_for(symbol);
    if (s == nullptr) {
        return;  // table full — can't track this symbol; skip sequencing
    }

    if (s->seen) {
        if (ts < s->last_ts) {
            out.flags |= kTimestampRegression;
        }
        // seq should advance by exactly 1; a larger jump means dropped messages.
        if (seq > s->last_seq + 1) {
            out.flags |= kSequenceGap;
            out.gap = seq - s->last_seq - 1;
        }
    }

    s->seen     = true;
    s->last_ts  = ts;
    s->last_seq = seq;
}

Result Validator::check(const model::WireTrade& t) noexcept {
    Result r;
    if (t.price <= 0.0) r.flags |= kTradeNonPositivePrice;
    if (t.size == 0)    r.flags |= kTradeNonPositiveSize;
    check_sequencing(t.symbol, t.ts_ns, t.seq, r);
    return r;
}

Result Validator::check(const model::WireBar& b) noexcept {
    Result r;

    if (b.low > b.high) r.flags |= kBarLowAboveHigh;
    if (b.open  < b.low || b.open  > b.high) r.flags |= kBarOpenOutOfRange;
    if (b.close < b.low || b.close > b.high) r.flags |= kBarCloseOutOfRange;
    if (b.vwap  < b.low || b.vwap  > b.high) r.flags |= kBarVwapOutOfRange;

    if (b.open <= 0.0 || b.high <= 0.0 || b.low <= 0.0 || b.close <= 0.0) {
        r.flags |= kBarNonPositivePrice;
    }

    // A bar with trades must have volume, and a bar with volume must have trades.
    if ((b.trade_count == 0) != (b.volume == 0)) {
        r.flags |= kBarVolumeInconsistent;
    }

    check_sequencing(b.symbol, b.start_ns, b.seq, r);
    return r;
}

}  // namespace ohlcv::validate
