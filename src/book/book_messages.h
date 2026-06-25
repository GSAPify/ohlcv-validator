#pragma once

#include <cstdint>
#include <type_traits>

namespace ohlcv::book {

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

// An L2 (price-level aggregated) book update, ITCH-style. Sets the total resting
// size at one price on one side; new_size == 0 removes the level. Sequenced, so
// the builder can detect a lost update -- which, unlike a skipped trade, makes the
// whole book stale until a snapshot rebuilds it.
//
// This is a separate message family from the WireRecord feed (trades/quotes/bars):
// book depth is a different data model, so it gets its own type rather than being
// forced into the existing union. Single-instrument here; multi-symbol routing
// would reuse the validator's per-symbol table pattern (deferred).
struct BookDelta {
    std::uint64_t seq;
    double        price;
    std::uint64_t new_size;   // aggregate size at price; 0 removes the level
    std::uint8_t  side;       // Side
    std::uint8_t  _pad[7];
};

// One price level in a snapshot of the book's state.
struct SnapshotLevel {
    double        price;
    std::uint64_t size;
    std::uint8_t  side;       // Side
    std::uint8_t  _pad[7];
};

static_assert(std::is_trivially_copyable_v<BookDelta>);
static_assert(std::is_trivially_copyable_v<SnapshotLevel>);

}  // namespace ohlcv::book
