#pragma once

#include <chrono>

namespace ohlcv::util {

// Exponential backoff ceiling: base * 2^attempt, capped. Pure and overflow-safe,
// so it's the unit-tested core of the reconnect loop. Callers add jitter (a
// random fraction of the ceiling) on top — jitter avoids a thundering herd of
// clients reconnecting in lockstep; the deterministic ceiling is what's tested.
inline std::chrono::milliseconds backoff_ceiling(
    unsigned attempt, std::chrono::milliseconds base,
    std::chrono::milliseconds cap) noexcept {
    if (attempt >= 32) return cap;  // guard the shift; 2^32 ms dwarfs any sane cap
    const long long shifted = base.count() << attempt;
    if (shifted <= 0 || shifted > cap.count()) return cap;  // overflow or over cap
    return std::chrono::milliseconds{shifted};
}

}  // namespace ohlcv::util
