#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace ohlcv::util {

// Linear, 1-nanosecond-resolution latency histogram.
//
// The buckets are allocated once in the constructor; record() only indexes and
// increments, so it never touches the heap — that's the whole point on a hot
// path whose self-overhead we're trying to measure. Hot-path latencies here are
// tens to low-hundreds of ns, so a flat array indexed directly by ns is exact
// and cheaper than the log-bucketed scheme a general histogram (HdrHistogram)
// would use. Samples above max_ns land in an overflow counter instead.
class LatencyHistogram {
public:
    explicit LatencyHistogram(std::uint64_t max_ns = 65535)
        : buckets_(max_ns + 1, 0), max_ns_(max_ns) {}

    void record(std::uint64_t ns) noexcept {
        ++count_;
        sum_ns_ += ns;
        if (ns < min_ns_) min_ns_ = ns;
        if (ns > max_observed_) max_observed_ = ns;
        if (ns > max_ns_) {
            ++overflow_;
            return;
        }
        ++buckets_[ns];
    }

    // Smallest latency at or below which `p` (0..1) of samples fall. Samples in
    // the overflow region are counted toward the total, so a percentile that
    // lands there returns max_ns_+1 as a floor (caller can read max() for the
    // true tail). Returns 0 when no samples have been recorded.
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        const std::uint64_t target =
            static_cast<std::uint64_t>(p * static_cast<double>(count_));
        std::uint64_t cumulative = 0;
        for (std::uint64_t ns = 0; ns < buckets_.size(); ++ns) {
            cumulative += buckets_[ns];
            if (cumulative >= target) return ns;
        }
        return max_ns_ + 1;  // fell into the overflow region
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t overflow() const noexcept { return overflow_; }
    [[nodiscard]] std::uint64_t min() const noexcept {
        return count_ ? min_ns_ : 0;
    }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_observed_; }
    [[nodiscard]] double mean() const noexcept {
        return count_ ? static_cast<double>(sum_ns_) / static_cast<double>(count_)
                      : 0.0;
    }

private:
    std::vector<std::uint64_t> buckets_;
    std::uint64_t max_ns_;
    std::uint64_t count_        = 0;
    std::uint64_t sum_ns_       = 0;
    std::uint64_t overflow_     = 0;
    std::uint64_t min_ns_       = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_observed_ = 0;
};

}  // namespace ohlcv::util
