#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "model/bar.h"

namespace ohlcv::model {

// Parse Alpaca's RFC3339 UTC timestamp into ns-since-Unix-epoch.
//
// Expected shape: "YYYY-MM-DDTHH:MM:SS[.fraction]Z" — always UTC, fraction
// optional (0-9 digits). Hand-rolled rather than std::chrono::from_stream
// because libc++'s chrono::parse is incomplete on Apple Clang, and because
// a fixed-shape parser is faster than a general one.
[[nodiscard]] inline Timestamp parse_rfc3339_nano(std::string_view s) {
    if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
        s[13] != ':' || s[16] != ':') {
        throw std::runtime_error("malformed timestamp: " + std::string{s});
    }

    auto d = [&](std::size_t i) { return static_cast<int>(s[i] - '0'); };

    const int yr = d(0) * 1000 + d(1) * 100 + d(2) * 10 + d(3);
    const int mo = d(5) * 10 + d(6);
    const int dy = d(8) * 10 + d(9);
    const int hr = d(11) * 10 + d(12);
    const int mi = d(14) * 10 + d(15);
    const int sc = d(17) * 10 + d(18);

    // Optional fractional seconds, scaled to nanoseconds.
    std::uint64_t nano = 0;
    std::size_t i = 19;
    if (i < s.size() && s[i] == '.') {
        ++i;
        int digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 9) {
            nano = nano * 10 + static_cast<std::uint64_t>(s[i] - '0');
            ++i;
            ++digits;
        }
        for (int j = digits; j < 9; ++j) {
            nano *= 10;  // ".123" → 123_000_000 ns
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;  // discard sub-ns precision rather than overflow
        }
    }

    if (i >= s.size() || s[i] != 'Z') {
        throw std::runtime_error("timestamp must end in Z: " + std::string{s});
    }

    using namespace std::chrono;
    const sys_days days{year{yr} / month{static_cast<unsigned>(mo)} /
                        day{static_cast<unsigned>(dy)}};
    const auto days_part_sec =
        duration_cast<seconds>(days.time_since_epoch()).count();
    const std::int64_t total_sec = days_part_sec + std::int64_t{hr} * 3600 +
                                   std::int64_t{mi} * 60 + std::int64_t{sc};

    return static_cast<std::uint64_t>(total_sec) * 1'000'000'000ULL + nano;
}

}  // namespace ohlcv::model
