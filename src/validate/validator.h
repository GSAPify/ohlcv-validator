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
};

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
    };

    // Find (or insert) the slot for a symbol. Returns nullptr only if the table
    // is full — a deliberate, observable failure rather than silent corruption.
    Slot* slot_for(const char (&symbol)[model::kSymbolLen]) noexcept;

    // Shared sequencing check for both trades and bars.
    void check_sequencing(const char (&symbol)[model::kSymbolLen],
                          std::uint64_t ts, std::uint64_t seq,
                          Result& out) noexcept;

    std::array<Slot, kCapacity> table_{};
};

}  // namespace ohlcv::validate
