#pragma once

#include <array>
#include <cstdint>

#include "model/wire.h"

namespace ohlcv::validate {

// Violations are a bitmask so a single record can fail several checks at once
// and the result still fits in a register — no heap, no per-record vector.
enum Violation : std::uint32_t {
    kNone                  = 0,
    // Bar invariants (per-record, stateless)
    kBarLowAboveHigh       = 1u << 0,  // low > high — the OHLC band is inverted
    kBarOpenOutOfRange     = 1u << 1,  // open  not in [low, high]
    kBarCloseOutOfRange    = 1u << 2,  // close not in [low, high]
    kBarVwapOutOfRange     = 1u << 3,  // vwap  not in [low, high]
    kBarNonPositivePrice   = 1u << 4,  // some OHLC field <= 0
    kBarVolumeInconsistent = 1u << 5,  // volume vs trade_count disagree on emptiness
    // Trade invariants (per-record, stateless)
    kTradeNonPositivePrice = 1u << 6,
    kTradeNonPositiveSize  = 1u << 7,
    // Sequencing (per-symbol, stateful)
    kTimestampRegression   = 1u << 8,  // ts went backwards for this symbol
    kSequenceGap           = 1u << 9,  // seq jumped by >1 — a message was dropped
    // Reconstruction (per-symbol, stateful): do the bar's constituent trades
    // actually rebuild it? This is the cross-record check that distinguishes a
    // real OHLCV validator from a bounds-checker.
    kBarVolumeReconstructMismatch     = 1u << 10,  // Σ trade size != bar.volume
    kBarTradeCountReconstructMismatch = 1u << 11,  // trade count != bar.trade_count
    kBarVwapReconstructMismatch       = 1u << 12,  // recomputed vwap != bar.vwap
    kBarOhlcReconstructMismatch       = 1u << 13,  // first/last/max/min != O/C/H/L
    // Quote invariants (per-record, stateless)
    kQuoteCrossed          = 1u << 14,  // bid > ask — the book is crossed
    kQuoteLocked           = 1u << 15,  // bid == ask — the book is locked
    kQuoteNonPositive      = 1u << 16,  // bid or ask price <= 0
    kQuoteZeroSize         = 1u << 17,  // bid or ask size is 0
    // Price-band / outlier detection (per-symbol, stateful): the per-trade
    // EWMA reference tracks the symbol's fair-value estimate; a trade or quote
    // mid that deviates by more than kPriceBandFrac is an outlier.
    kPriceBandBreach       = 1u << 18,  // price more than 5% from EWMA reference
};

// Relative tolerance for reconstructed *price* comparisons (vwap and OHLC). Real
// feeds round bar prices to a tick, so bit-exact agreement is the wrong test and
// would false-positive on every live bar. Volume and trade_count are integers and
// are compared exactly.
inline constexpr double kPriceRelTol = 1e-6;

// Price-band outlier detection parameters. kPriceBandFrac is the maximum
// fractional deviation from the EWMA reference before a record is flagged;
// kRefEwmaAlpha controls how quickly the reference tracks the true price.
// Outliers do NOT update the reference — one bad tick must not poison it.
inline constexpr double kPriceBandFrac  = 0.05;
inline constexpr double kRefEwmaAlpha   = 0.2;

struct Result {
    std::uint32_t flags = kNone;
    std::uint64_t gap   = 0;  // dropped-message count when kSequenceGap is set

    [[nodiscard]] bool ok() const noexcept { return flags == kNone; }
    [[nodiscard]] bool has(Violation v) const noexcept { return flags & v; }
};

// Stateful validator: the per-record checks are pure, but timestamp-regression
// and sequence-gap detection need the last value seen *for that symbol*. That
// state lives in a fixed-capacity, open-addressing table keyed on the 8-byte
// symbol reinterpreted as a u64 — so the hot path allocates nothing after
// construction. Capacity is a power of two for mask-based probing.
class Validator {
public:
    [[nodiscard]] Result check(const model::WireTrade& t) noexcept;
    [[nodiscard]] Result check(const model::WireBar& b) noexcept;
    [[nodiscard]] Result check(const model::WireQuote& q) noexcept;

private:
    static constexpr std::size_t kBits     = 12;                  // 4096 slots
    static constexpr std::size_t kCapacity = std::size_t{1} << kBits;
    static constexpr std::size_t kMask     = kCapacity - 1;

    // Trades, quotes, and bars are separate streams with independent event-time
    // clocks. Timestamp monotonicity is a *within-stream* property, so each
    // stream tracks its own last timestamp (a quote can't make a later-arriving
    // trade with an earlier event time look like a regression). Sequence numbers,
    // by contrast, are a single per-symbol counter the feed/generator advances
    // across all types, so gap detection stays shared.
    enum Stream : std::size_t { kTradeStream = 0, kBarStream, kQuoteStream, kStreamCount };

    struct Slot {
        std::uint64_t key      = 0;      // symbol bytes as u64; 0 == empty
        std::array<std::uint64_t, kStreamCount> last_ts{};  // per stream; 0 = unseen
        std::uint64_t last_seq = 0;      // shared per-symbol sequence cursor
        bool          seen     = false;  // any record seen (for seq-gap gating)

        // Price-band EWMA reference. Initialised on the first valid trade; only
        // valid (non-outlier) trades move it. Quote mid checks read but never
        // write this field so quotes cannot drift the reference.
        double        ref_price  = 0.0;
        bool          ref_init   = false;

        // Trade accumulator for bar reconstruction: the aggregate of the VALID
        // trades seen for this symbol since its last bar. Reset when a bar
        // reconciles against it.
        bool          has_trades = false;
        std::uint64_t acc_count  = 0;
        std::uint64_t acc_volume = 0;    // Σ size
        double        acc_pv     = 0.0;  // Σ price·size  (the VWAP numerator)
        double        acc_open   = 0.0;  // first valid trade price since reset
        double        acc_high   = 0.0;
        double        acc_low    = 0.0;
        double        acc_close  = 0.0;  // last valid trade price
    };

    // Find (or insert) the slot for a symbol. Returns nullptr only if the table
    // is full — a deliberate, observable failure rather than silent corruption.
    Slot* slot_for(const char (&symbol)[model::kSymbolLen]) noexcept;

    // Sequencing check shared by trades, quotes, and bars; operates on an
    // already-resolved slot so the hot path does a single table lookup per
    // record. `stream` selects the per-stream timestamp cursor for the
    // regression check; the sequence-gap check uses the shared cursor.
    static void check_sequencing(Slot& s, Stream stream, std::uint64_t ts,
                                 std::uint64_t seq, Result& out) noexcept;

    // Price-band check: flags kPriceBandBreach if price deviates from the slot's
    // EWMA reference by more than kPriceBandFrac. On the first call (warmup) the
    // reference is initialised and no flag is set. An outlier does NOT update the
    // reference — subsequent normal trades will still pass.
    static void check_price_band(Slot& s, double price, Result& out) noexcept;

    // Fold one valid trade into the slot's accumulator.
    static void accumulate(Slot& s, double price, std::uint64_t size) noexcept;

    // Reconcile a bar against the accumulated trades, then reset the accumulator.
    static void reconcile(Slot& s, const model::WireBar& b, Result& out) noexcept;

    std::array<Slot, kCapacity> table_{};
};

}  // namespace ohlcv::validate
