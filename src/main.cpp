#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>

#include "util/timing.h"

namespace {

constexpr std::size_t kSamples = 10'000;

}  // namespace

// Bootstrap binary: measures the self-overhead of our timing primitive.
// This is the baseline number every future latency measurement is relative to.
int main() {
    using ohlcv::util::now_ns;

    // Warm up the clock path so the first call isn't an outlier.
    for (int i = 0; i < 1024; ++i) {
        (void)now_ns();
    }

    std::array<std::uint64_t, kSamples> deltas{};
    for (std::size_t i = 0; i < kSamples; ++i) {
        const auto a = now_ns();
        const auto b = now_ns();
        deltas[i] = b - a;
    }

    std::sort(deltas.begin(), deltas.end());

    const auto min    = deltas.front();
    const auto median = deltas[kSamples / 2];
    const auto p99    = deltas[(kSamples * 99) / 100];
    const auto max    = deltas.back();

    std::cout << "now_ns() self-overhead over " << kSamples << " samples (ns):\n"
              << "  min:    " << min    << '\n'
              << "  median: " << median << '\n'
              << "  p99:    " << p99    << '\n'
              << "  max:    " << max    << '\n';

    return 0;
}
