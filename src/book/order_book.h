#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "book/book_messages.h"

namespace ohlcv::book {

// An aggregated (L2) limit order book: the best `MaxLevels` price levels per side,
// each holding the total resting size at that price. Built by applying BookDeltas.
//
// Both sides keep the best level at index 0 -- bids sorted descending, asks
// ascending -- so top-of-book is buf[0] and depth(n) is the first n entries.
//
// Zero allocation: fixed std::arrays, no heap. The cost is that insert/remove is
// O(levels) (a linear find + a shift to keep the side sorted). That's deliberate:
// book building is the control plane, not the nanosecond hot path -- correctness
// and no-heap matter here, not shaving a memmove over <=64 levels. If a level
// arrives beyond capacity it's kept only if it betters the current worst level
// (we track the best MaxLevels, the part anyone trades on).
template <std::size_t MaxLevels = 64>
class OrderBook {
public:
    struct Level {
        double        price = 0.0;
        std::uint64_t size  = 0;
    };

    void clear() noexcept { n_bid_ = 0; n_ask_ = 0; }

    void apply(Side side, double price, std::uint64_t new_size) noexcept {
        if (side == Side::Bid) apply_side(bid_, n_bid_, price, new_size, true);
        else                   apply_side(ask_, n_ask_, price, new_size, false);
    }
    void apply(const BookDelta& d) noexcept {
        apply(static_cast<Side>(d.side), d.price, d.new_size);
    }

    [[nodiscard]] bool has_bid() const noexcept { return n_bid_ > 0; }
    [[nodiscard]] bool has_ask() const noexcept { return n_ask_ > 0; }
    [[nodiscard]] double best_bid() const noexcept { return bid_[0].price; }
    [[nodiscard]] double best_ask() const noexcept { return ask_[0].price; }
    [[nodiscard]] std::uint64_t best_bid_size() const noexcept { return bid_[0].size; }
    [[nodiscard]] std::uint64_t best_ask_size() const noexcept { return ask_[0].size; }
    [[nodiscard]] double spread() const noexcept { return best_ask() - best_bid(); }
    [[nodiscard]] double mid() const noexcept { return 0.5 * (best_bid() + best_ask()); }
    [[nodiscard]] std::size_t bid_levels() const noexcept { return n_bid_; }
    [[nodiscard]] std::size_t ask_levels() const noexcept { return n_ask_; }
    [[nodiscard]] const Level& bid(std::size_t i) const noexcept { return bid_[i]; }
    [[nodiscard]] const Level& ask(std::size_t i) const noexcept { return ask_[i]; }

    // bid >= ask: crossed (bid > ask) or locked (bid == ask). Real books cross
    // momentarily during multi-level updates, so this is an observation to flag,
    // not a hard reject (mirrors the validator's quote crossed/locked check).
    [[nodiscard]] bool crossed() const noexcept {
        return n_bid_ > 0 && n_ask_ > 0 && best_bid() >= best_ask();
    }

    bool operator==(const OrderBook& o) const noexcept {
        if (n_bid_ != o.n_bid_ || n_ask_ != o.n_ask_) return false;
        for (std::size_t i = 0; i < n_bid_; ++i)
            if (bid_[i].price != o.bid_[i].price || bid_[i].size != o.bid_[i].size)
                return false;
        for (std::size_t i = 0; i < n_ask_; ++i)
            if (ask_[i].price != o.ask_[i].price || ask_[i].size != o.ask_[i].size)
                return false;
        return true;
    }

private:
    static void apply_side(std::array<Level, MaxLevels>& lv, std::size_t& n,
                           double price, std::uint64_t new_size,
                           bool descending) noexcept {
        for (std::size_t i = 0; i < n; ++i) {  // existing level?
            if (lv[i].price == price) {
                if (new_size == 0) {           // remove: shift the tail down
                    for (std::size_t j = i + 1; j < n; ++j) lv[j - 1] = lv[j];
                    --n;
                } else {
                    lv[i].size = new_size;
                }
                return;
            }
        }
        if (new_size == 0) return;             // removing a level we don't have

        std::size_t pos = 0;                   // insertion point (best stays at 0)
        while (pos < n && (descending ? lv[pos].price > price : lv[pos].price < price))
            ++pos;
        if (n < MaxLevels) {
            for (std::size_t j = n; j > pos; --j) lv[j] = lv[j - 1];
            lv[pos] = {price, new_size};
            ++n;
        } else if (pos < MaxLevels) {          // full, but this betters the worst
            for (std::size_t j = MaxLevels - 1; j > pos; --j) lv[j] = lv[j - 1];
            lv[pos] = {price, new_size};
        }
        // else: worse than every kept level -> dropped (we track the best N)
    }

    std::array<Level, MaxLevels> bid_{};
    std::array<Level, MaxLevels> ask_{};
    std::size_t n_bid_ = 0;
    std::size_t n_ask_ = 0;
};

}  // namespace ohlcv::book
