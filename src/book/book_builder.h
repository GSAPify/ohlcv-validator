#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "book/book_messages.h"
#include "book/order_book.h"

namespace ohlcv::book {

// Maintains an order book from a sequenced delta stream, and -- the point of this
// whole subsystem -- handles the one thing that makes order books harder than a
// trade feed: a LOST update invalidates the entire book until a snapshot rebuilds
// it. A trade stream tolerates a skipped sequence; a book cannot, because every
// delta mutates state that all later deltas depend on.
//
// STATE MACHINE:  Recovering  <-->  Live
//   - Starts Recovering (no book yet). on_delta() buffers; the first snapshot
//     rebuilds the book and goes Live.
//   - Live: deltas apply in strict sequence. A gap (seq > expected) invalidates
//     the book -> Recovering, and incoming deltas are buffered until a snapshot.
//   - on_snapshot(as_of): rebuild from the snapshot (truth as of `as_of`), then
//     replay buffered deltas with seq > as_of, then go Live.
//
// THE RECOVERY RACE -- the subtle, load-bearing part. A snapshot is generated
// asynchronously while deltas keep flowing, so it is "correct as of seq N" while
// you may have already buffered N+1, N+2, ... Correct recovery discards every
// buffered delta with seq <= N (already baked into the snapshot) and replays only
// seq > N -- re-applying one would double-count, dropping one would leave a hole.
// And the snapshot only closes the gap if those seq>N deltas are contiguous from
// N+1: a too-old snapshot that leaves a hole is rejected, and we wait for a newer
// one rather than build a corrupt book.
//
// Input is assumed in sequence order (it comes off the feed arbitrator, which
// already delivers in order). Zero allocation: the book and the recovery buffer
// are fixed arrays.
template <std::size_t MaxLevels = 64, std::size_t RecoverBuf = 1024>
class BookBuilder {
public:
    enum class State { Recovering, Live };

    struct Stats {
        std::uint64_t applied          = 0;  // deltas applied while Live
        std::uint64_t gaps             = 0;  // gaps detected (book invalidated)
        std::uint64_t recovered        = 0;  // successful snapshot recoveries
        std::uint64_t stale_dropped    = 0;  // deltas with seq < expected (dup/old)
        std::uint64_t snapshot_too_old = 0;  // snapshot couldn't close the hole
        std::uint64_t buffer_overflow  = 0;  // recovery buffer full (need newer snapshot)
    };

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool is_live() const noexcept { return state_ == State::Live; }
    [[nodiscard]] const OrderBook<MaxLevels>& book() const noexcept { return book_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t expected_seq() const noexcept { return expected_; }

    void on_delta(const BookDelta& d) noexcept {
        if (state_ == State::Live) {
            if (d.seq == expected_) {
                book_.apply(d);
                ++expected_;
                ++stats_.applied;
            } else if (d.seq > expected_) {       // GAP: book is now untrustworthy
                ++stats_.gaps;
                state_ = State::Recovering;
                nbuf_ = 0;
                buffer(d);
            } else {                              // seq < expected: a late/dup copy
                ++stats_.stale_dropped;
            }
        } else {                                  // Recovering: hold until a snapshot
            buffer(d);
        }
    }

    // A snapshot of the book valid as of `as_of_seq` (it reflects every update
    // through that sequence). `levels` is any range of SnapshotLevel.
    template <typename LevelRange>
    void on_snapshot(std::uint64_t as_of_seq, const LevelRange& levels) noexcept {
        // First, prove the buffered increments past the snapshot are contiguous
        // from as_of+1 -- otherwise the snapshot is too old to close the gap and
        // we must wait for a newer one (don't touch the book).
        std::uint64_t expect = as_of_seq + 1;
        for (std::size_t i = 0; i < nbuf_; ++i) {
            if (buf_[i].seq <= as_of_seq) continue;       // already in the snapshot
            if (buf_[i].seq != expect) { ++stats_.snapshot_too_old; return; }  // hole
            ++expect;
        }

        // Commit: rebuild from the snapshot, then replay only the increments after
        // it (seq > as_of), in order.
        book_.clear();
        for (const auto& l : levels)
            book_.apply(static_cast<Side>(l.side), l.price, l.size);
        for (std::size_t i = 0; i < nbuf_; ++i)
            if (buf_[i].seq > as_of_seq) book_.apply(buf_[i]);

        expected_ = expect;
        nbuf_ = 0;
        state_ = State::Live;
        ++stats_.recovered;
    }

private:
    void buffer(const BookDelta& d) noexcept {
        if (nbuf_ < RecoverBuf) buf_[nbuf_++] = d;
        else ++stats_.buffer_overflow;  // recovery outran the buffer -> needs a newer snapshot
    }

    OrderBook<MaxLevels>             book_{};
    State                            state_    = State::Recovering;
    std::uint64_t                    expected_ = 0;
    std::array<BookDelta, RecoverBuf> buf_{};
    std::size_t                      nbuf_     = 0;
    Stats                            stats_{};
};

}  // namespace ohlcv::book
