#pragma once

#include <chrono>
#include <cstdint>

namespace ohlcv::util {

// Monotonic nanosecond timestamp.
//
// On x86_64 with an invariant TSC the right primitive is rdtscp + a calibration
// step. On Apple Silicon the system counter (CNTVCT_EL0) is virtualized at
// 24 MHz (~41 ns resolution), so we lean on steady_clock which wraps
// mach_absolute_time and still reports nanoseconds. Resume-grade latency
// numbers will eventually be measured on a Linux x86_64 host.
[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    using clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now().time_since_epoch()
        ).count()
    );
}

}  // namespace ohlcv::util
