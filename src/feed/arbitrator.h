#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "feed/feed_protocol.h"
#include "replay/binary_format.h"

namespace ohlcv::feed {

// A/B line arbitration + in-order release + gap DETECTION -- the core of an
// exchange market-data feed handler, with the network transport left to the
// caller (the multicast wiring lives in feed_handler).
//
// THE PROBLEM. An exchange publishes one sequence-numbered message stream
// redundantly on two lines, A and B. Either line can drop or reorder packets;
// between them, almost nothing is lost. The handler must reconstruct the single
// true stream: deliver every sequence exactly once, strictly in order, taking
// whichever line delivers each copy first -- and notice when a sequence is
// genuinely lost on *both* lines (a gap). That gap signal matters downstream: a
// stateless trade/quote stream tolerates a skipped sequence, but an order book
// is invalid until it recovers from a snapshot -- so detection has to be exact.
//
// THE TWO CORRECTNESS PILLARS -- what a real handler gets right and a naive one
// gets wrong:
//
//  1. Don't cry gap too early. Seeing seq N+2 before N+1 is normal -- N+1 may be
//     on the other line microseconds behind. We buffer ahead in a reorder window
//     and release only the contiguous prefix; a gap is declared only when
//     *forced*: a packet arrives so far ahead the window can't hold the hole, or
//     the caller flush()es at end of stream. (A real handler also waits a short
//     time/packet budget before giving up; that wait is abstracted here into the
//     window-overflow and explicit-flush triggers, so the logic is deterministic
//     and unit-testable rather than wall-clock dependent.)
//
//  2. Don't alias. The reorder buffer is a power-of-two ring indexed by
//     seq & (Window-1), so two sequences Window apart map to the same slot. Each
//     slot therefore stores its own seq and we validate it on read: same seq is a
//     duplicate (the other line's copy); a different seq would alias a live slot
//     and is rejected. A sequence past the window forces the frontier forward
//     (declaring the unfilled hole a gap) until it fits -- a far-future packet
//     never overwrites a live slot.
//
// This is gap DETECTION, not recovery: a detected gap is the hook where a real
// system would request a retransmit or replay a snapshot. Zero allocation -- the
// window is a fixed std::array of POD slots; offer()/flush() never touch the heap
// (proven by the alloc-guard test).
template <std::size_t Window = 1024>
class FeedArbitrator {
    static_assert(Window >= 2 && (Window & (Window - 1)) == 0,
                  "Window must be a power of two");

public:
    struct Stats {
        std::uint64_t delivered     = 0;     // messages released, in order
        std::uint64_t duplicates[2] = {0, 0};// per-line copies of an already-seen seq
        std::uint64_t gaps          = 0;     // sequences declared lost on both lines
        std::uint64_t max_reorder   = 0;     // deepest a seq sat ahead of the frontier
    };

    // Offer one received packet. Every message that becomes deliverable (the
    // contiguous run starting at the frontier) is handed to sink(seq, record) in
    // strict sequence order before offer() returns.
    template <typename Sink>
    void offer(const FeedPacket& pkt, Sink&& sink) {
        const std::uint64_t seq = pkt.seq;
        const unsigned ln =
            (pkt.line == static_cast<std::uint8_t>(Line::B)) ? 1u : 0u;

        // Anchor the session at the first sequence seen. (A production handler
        // would start from a known session start or a recovery snapshot; first-
        // seen is the deliberate simplification.)
        if (!started_) {
            frontier_ = seq;
            started_ = true;
        }

        // Below the frontier: already delivered, or already declared a gap and
        // skipped. Either way this is a late/duplicate copy.
        if (seq < frontier_) {
            ++stats_.duplicates[ln];
            return;
        }

        // Too far ahead for the window to hold the intervening hole: the frontier
        // can't wait any longer. Force it forward -- releasing what's buffered,
        // declaring the rest gaps -- until seq fits the window.
        while (seq - frontier_ >= Window) {
            advance_frontier(sink);
        }

        if (const std::uint64_t depth = seq - frontier_; depth > stats_.max_reorder) {
            stats_.max_reorder = depth;
        }

        Slot& s = buf_[seq & (Window - 1)];
        if (s.occupied) {
            // Same seq is the other line's redundant copy. A different seq would
            // alias a live slot -- impossible given the window bound above, but
            // reject rather than corrupt.
            if (s.seq == seq) ++stats_.duplicates[ln];
            return;
        }
        s.occupied = true;
        s.seq = seq;
        s.record = pkt.record;
        ++occupied_;

        release_prefix(sink);
    }

    // End of stream / give up waiting: drain everything still buffered, declaring
    // each unfilled hole a gap, so no message is left stranded.
    template <typename Sink>
    void flush(Sink&& sink) {
        while (occupied_ > 0) {
            advance_frontier(sink);
        }
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t frontier() const noexcept { return frontier_; }

private:
    struct Slot {
        bool               occupied = false;
        std::uint64_t      seq      = 0;
        replay::WireRecord record{};
    };

    // Release the contiguous run of buffered messages starting at the frontier.
    template <typename Sink>
    void release_prefix(Sink&& sink) {
        for (;;) {
            Slot& f = buf_[frontier_ & (Window - 1)];
            if (!(f.occupied && f.seq == frontier_)) break;
            sink(frontier_, f.record);
            ++stats_.delivered;
            f.occupied = false;
            --occupied_;
            ++frontier_;
        }
    }

    // Move the frontier forward by one: deliver the frontier message if present,
    // else declare it a gap. Then release any run that has become contiguous.
    template <typename Sink>
    void advance_frontier(Sink&& sink) {
        Slot& f = buf_[frontier_ & (Window - 1)];
        if (f.occupied && f.seq == frontier_) {
            sink(frontier_, f.record);
            ++stats_.delivered;
            f.occupied = false;
            --occupied_;
        } else {
            ++stats_.gaps;
        }
        ++frontier_;
        release_prefix(sink);
    }

    std::array<Slot, Window> buf_{};
    std::uint64_t            frontier_ = 0;
    std::size_t              occupied_ = 0;
    bool                     started_  = false;
    Stats                    stats_{};
};

}  // namespace ohlcv::feed
