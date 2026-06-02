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
};

// Relative tolerance for reconstructed *price* comparisons (vwap and OHLC). Real
// feeds round bar prices to a tick, so bit-exact agreement is the wrong test and
// would false-positive on every live bar. Volume and trade_count are integers and
// are compared exactly.
inline constexpr double kPriceRelTol = 1e-6;

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

private:
    static constexpr std::size_t kCapacity = 4096;  // power of two
    static constexpr std::size_t kMask     = kCapacity - 1;

    struct Slot {
        std::uint64_t key      = 0;      // symbol bytes as u64; 0 == empty
        std::uint64_t last_ts  = 0;
        std::uint64_t last_seq = 0;
        bool          seen     = false;

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

    // Sequencing check shared by trades and bars; operates on an already-resolved
    // slot so the hot path does a single table lookup per record.
    static void check_sequencing(Slot& s, std::uint64_t ts, std::uint64_t seq,
                                 Result& out) noexcept;

    // Fold one valid trade into the slot's accumulator.
    static void accumulate(Slot& s, double price, std::uint64_t size) noexcept;

    // Reconcile a bar against the accumulated trades, then reset the accumulator.
    static void reconcile(Slot& s, const model::WireBar& b, Result& out) noexcept;

    std::array<Slot, kCapacity> table_{};
};

}  // namespace ohlcv::validate
