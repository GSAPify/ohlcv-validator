#include "validate/validator.h"

#include <cmath>
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

// Relative-tolerance compare for reconstructed prices. The fmax(scale, 1.0)
// floor keeps it well-behaved near zero; for real prices (~100) it's a plain
// relative comparison.
bool approx_equal(double a, double b) noexcept {
    const double scale = std::fmax(std::fabs(a), std::fabs(b));
    return std::fabs(a - b) <= kPriceRelTol * std::fmax(scale, 1.0);
}

}  // namespace

Validator::Slot* Validator::slot_for(
    const char (&symbol)[model::kSymbolLen]) noexcept {
    const std::uint64_t key = symbol_key(symbol);
    // Fibonacci hashing: multiply by 2^64/φ and take the top kBits. The raw
    // symbol bytes cluster in the low bits (e.g. synthetic/numbered tickers that
    // share a prefix), and a plain `key & kMask` would collapse them all into one
    // bucket — turning the linear probe into an O(N) scan. Mixing first spreads
    // any key set evenly across the table.
    std::size_t idx =
        static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> (64 - kBits));

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

void Validator::check_sequencing(Slot& s, std::uint64_t ts, std::uint64_t seq,
                                 Result& out) noexcept {
    if (s.seen) {
        if (ts < s.last_ts) {
            out.flags |= kTimestampRegression;
        }
        // seq should advance by exactly 1; a larger jump means dropped messages.
        if (seq > s.last_seq + 1) {
            out.flags |= kSequenceGap;
            out.gap = seq - s.last_seq - 1;
        }
    }

    s.seen     = true;
    s.last_ts  = ts;
    s.last_seq = seq;
}

void Validator::accumulate(Slot& s, double price, std::uint64_t size) noexcept {
    if (!s.has_trades) {
        s.acc_open = s.acc_high = s.acc_low = s.acc_close = price;
        s.has_trades = true;
    } else {
        if (price > s.acc_high) s.acc_high = price;
        if (price < s.acc_low)  s.acc_low  = price;
        s.acc_close = price;
    }
    s.acc_count  += 1;
    s.acc_volume += size;
    s.acc_pv     += price * static_cast<double>(size);
}

void Validator::reconcile(Slot& s, const model::WireBar& b, Result& out) noexcept {
    if (!s.has_trades) {
        return;  // no constituent trades to compare (e.g. symbol's first bar)
    }

    if (s.acc_volume != b.volume)      out.flags |= kBarVolumeReconstructMismatch;
    if (s.acc_count  != b.trade_count) out.flags |= kBarTradeCountReconstructMismatch;

    if (s.acc_volume != 0) {
        const double recon_vwap = s.acc_pv / static_cast<double>(s.acc_volume);
        if (!approx_equal(recon_vwap, b.vwap)) {
            out.flags |= kBarVwapReconstructMismatch;
        }
    }

    if (!approx_equal(s.acc_open,  b.open)  ||
        !approx_equal(s.acc_high,  b.high)  ||
        !approx_equal(s.acc_low,   b.low)   ||
        !approx_equal(s.acc_close, b.close)) {
        out.flags |= kBarOhlcReconstructMismatch;
    }

    // Reset the accumulator for the next bar window.
    s.has_trades = false;
    s.acc_count  = 0;
    s.acc_volume = 0;
    s.acc_pv     = 0.0;
}

Result Validator::check(const model::WireTrade& t) noexcept {
    Result r;
    const bool valid_price = t.price > 0.0;
    const bool valid_size  = t.size != 0;
    if (!valid_price) r.flags |= kTradeNonPositivePrice;
    if (!valid_size)  r.flags |= kTradeNonPositiveSize;

    Slot* s = slot_for(t.symbol);
    if (s == nullptr) return r;  // table full — skip stateful checks
    check_sequencing(*s, t.ts_ns, t.seq, r);

    // Reconstruct from VALID trades only: a corrupt tick trips its own flag above
    // but is excluded from the bar's aggregate, so the bar reconstructs cleanly
    // from the remaining good trades. (Real feeds also round prices and exclude
    // certain sale conditions from volume/high/low — condition-filtering is
    // deferred; here we filter only on basic validity.)
    if (valid_price && valid_size) {
        accumulate(*s, t.price, t.size);
    }
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

    Slot* s = slot_for(b.symbol);
    if (s == nullptr) return r;  // table full — skip stateful checks
    check_sequencing(*s, b.start_ns, b.seq, r);
    reconcile(*s, b, r);
    return r;
}

}  // namespace ohlcv::validate
