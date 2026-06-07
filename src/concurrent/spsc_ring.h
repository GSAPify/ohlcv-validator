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
// CursorAlign places the two cursors in memory. At 64 they sit on separate cache
// lines: the producer writing `tail` and the consumer writing `head` then dirty
// different lines and never invalidate each other's — no false sharing. Shrink
// it and the cursors pack onto one line, so every push and pop ping-pongs that
// line between the two cores. That smaller alignment is the deliberately
// pessimised variant the bench uses to *measure* the cost of false sharing.
template <typename T, std::size_t Capacity, std::size_t CursorAlign = 64>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    using value_type = T;

    // Enqueue without blocking. Returns false if the ring is full, leaving the
    // backpressure policy (spin, drop, …) to the caller.
    bool try_push(const T& item) noexcept {
        const std::size_t t = tail_.v.load(std::memory_order_relaxed);
        // head is written only by the consumer; acquire so we observe freed slots.
        if (t - head_.v.load(std::memory_order_acquire) >= Capacity)
            return false;  // full
        buf_[t & kMask] = item;
        tail_.v.store(t + 1, std::memory_order_release);  // publish the slot
        return true;
    }

    // Dequeue without blocking into `out`. Returns false if the ring is empty.
    bool try_pop(T& out) noexcept {
        const std::size_t h = head_.v.load(std::memory_order_relaxed);
        // tail is written only by the producer; acquire so we observe its writes.
        if (h == tail_.v.load(std::memory_order_acquire))
            return false;  // empty
        out = buf_[h & kMask];
        head_.v.store(h + 1, std::memory_order_release);  // free the slot
        return true;
    }

    // Approximate queue depth. Either side may call it, but the two cursor loads
    // aren't a consistent snapshot, so use it for metrics (mean occupancy), never
    // for control flow.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t t = tail_.v.load(std::memory_order_relaxed);
        const std::size_t h = head_.v.load(std::memory_order_relaxed);
        return t - h;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    struct alignas(CursorAlign) Cursor {
        std::atomic<std::size_t> v{0};
    };

    Cursor head_;                    // advanced by the consumer
    Cursor tail_;                    // advanced by the producer
    std::array<T, Capacity> buf_{};  // the slots themselves
};

}  // namespace ohlcv::concurrent
