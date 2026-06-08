#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace ohlcv::concurrent {

// Lock-free single-producer / single-consumer ring buffer.
//
// Exactly one thread pushes and one thread pops — no more. Under that
// restriction the queue needs neither locks nor CAS: each side owns one cursor
// and only ever *reads* the other's. Correctness rests entirely on one
// acquire/release pairing:
//
//   producer: write slot, then RELEASE-store tail   ──happens-before──┐
//   consumer: ACQUIRE-load tail, then read slot   ◄───────────────────┘
//
// and symmetrically for head (consumer frees a slot, producer reuses it). That
// is the whole synchronisation — and it's *verified* under ThreadSanitizer by
// the threaded test, not just argued here.
//
// The cursors are monotonic counters that never reset; the slot index is
// counter & (Capacity-1). So all Capacity slots are usable (no sacrificed slot)
// and a full vs empty ring is told apart by the counters, not the indices.
// Capacity must be a power of two.
//
// Two layout/algorithm knobs the bench sweeps to measure their cost:
//
//   CursorAlign — cache-line placement of the producer's and consumer's cells.
//     At 64 each cell sits on its own line; shrink it and they pack onto one.
//
//   CacheFarCursor — whether each side keeps a private cached copy of the far
//     cursor (Vyukov/Folly style) and re-reads the real one only when the cache
//     says full/empty. With it off (the naive ring), every push reads `head`
//     and every pop reads `tail` cross-core — true sharing on each iteration.
//     With it on, those cross-core reads become rare, so the steady state is
//     each side touching only its own line.
template <typename T, std::size_t Capacity, std::size_t CursorAlign = 64,
          bool CacheFarCursor = false>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    using value_type = T;

    // Enqueue without blocking. Returns false if the ring is full, leaving the
    // backpressure policy (spin, drop, …) to the caller.
    bool try_push(const T& item) noexcept {
        const std::size_t t = prod_.tail.load(std::memory_order_relaxed);
        if constexpr (CacheFarCursor) {
            // Trust the cached head first; only pay the cross-core read of the
            // real head when the cache says the ring looks full.
            if (t - prod_.cached_head >= Capacity) {
                prod_.cached_head = cons_.head.load(std::memory_order_acquire);
                if (t - prod_.cached_head >= Capacity) return false;
            }
        } else {
            if (t - cons_.head.load(std::memory_order_acquire) >= Capacity)
                return false;
        }
        buf_[t & kMask] = item;
        prod_.tail.store(t + 1, std::memory_order_release);  // publish the slot
        return true;
    }

    // Dequeue without blocking into `out`. Returns false if the ring is empty.
    bool try_pop(T& out) noexcept {
        const std::size_t h = cons_.head.load(std::memory_order_relaxed);
        if constexpr (CacheFarCursor) {
            if (h == cons_.cached_tail) {
                cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
                if (h == cons_.cached_tail) return false;
            }
        } else {
            if (h == prod_.tail.load(std::memory_order_acquire)) return false;
        }
        out = buf_[h & kMask];
        cons_.head.store(h + 1, std::memory_order_release);  // free the slot
        return true;
    }

    // Approximate queue depth. Either side may call it, but the two cursor loads
    // aren't a consistent snapshot, so use it for metrics (mean occupancy), never
    // for control flow.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t t = prod_.tail.load(std::memory_order_relaxed);
        const std::size_t h = cons_.head.load(std::memory_order_relaxed);
        return t - h;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Each side's hot data on its own cell: the cursor it writes plus its cached
    // copy of the far cursor. Co-locating them means the cached read (when it
    // happens) and the owner's own writes stay on one line; CursorAlign then
    // decides whether the two sides' cells share a line or not.
    struct alignas(CursorAlign) Producer {
        std::atomic<std::size_t> tail{0};
        std::size_t              cached_head{0};  // used iff CacheFarCursor
    };
    struct alignas(CursorAlign) Consumer {
        std::atomic<std::size_t> head{0};
        std::size_t              cached_tail{0};  // used iff CacheFarCursor
    };

    Producer                prod_;  // written by the producer
    Consumer                cons_;  // written by the consumer
    std::array<T, Capacity> buf_{};  // the slots themselves
};

}  // namespace ohlcv::concurrent
