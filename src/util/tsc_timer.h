#pragma once

#include <cstdint>
#include <ctime>

// On x86 we read the CPU's Time Stamp Counter with rdtscp — a user-space cycle
// counter with ~0.2-0.3 ns resolution, which is what makes a real per-record
// latency *distribution* measurable (Apple Silicon's 41 ns clock floor can't).
// On any other arch we fall back to CLOCK_MONOTONIC so the code still compiles;
// this bench is only meaningful on x86.
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define OHLCV_HAVE_RDTSC 1
#else
#define OHLCV_HAVE_RDTSC 0
#endif

namespace ohlcv::util {

// Open a timed region. rdtscp already waits for prior instructions to retire;
// the trailing lfence stops later instructions from starting before the counter
// is read, so the measured window is tight.
[[nodiscard]] inline std::uint64_t cycle_start() noexcept {
#if OHLCV_HAVE_RDTSC
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    std::timespec ts;
    std::timespec_get(&ts, TIME_UTC);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

// Close a timed region: lfence first so the work can't drift past the counter.
[[nodiscard]] inline std::uint64_t cycle_end() noexcept {
#if OHLCV_HAVE_RDTSC
    _mm_lfence();
    unsigned aux;
    return __rdtscp(&aux);
#else
    std::timespec ts;
    std::timespec_get(&ts, TIME_UTC);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

// TSC ticks per nanosecond (≈ the invariant-TSC GHz). Calibrated against
// CLOCK_MONOTONIC over a fixed interval. On the fallback, cycles already are
// nanoseconds, so this is exactly 1.0.
[[nodiscard]] inline double calibrate_ticks_per_ns() noexcept {
#if OHLCV_HAVE_RDTSC
    std::timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const std::uint64_t c0 = cycle_start();

    const std::timespec nap{0, 200'000'000};  // 200 ms
    nanosleep(&nap, nullptr);

    const std::uint64_t c1 = cycle_end();
    std::timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    const double ns = static_cast<double>(t1.tv_sec - t0.tv_sec) * 1e9 +
                      static_cast<double>(t1.tv_nsec - t0.tv_nsec);
    return ns > 0.0 ? static_cast<double>(c1 - c0) / ns : 1.0;
#else
    return 1.0;
#endif
}

[[nodiscard]] inline bool tsc_available() noexcept { return OHLCV_HAVE_RDTSC; }

}  // namespace ohlcv::util
