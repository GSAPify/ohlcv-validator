#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "validate/validator.h"

namespace ohlcv::ingest {

// Which validator flags are worth surfacing on a *sampled, live* feed, and for
// which record kind. The validator runs every check; this is the report layer
// that decides what's meaningful here, so the validator itself stays single-path
// and untouched.
//
// Excluded everywhere (N/A on the IEX sample): reconstruction mismatches (our
// trades are a fraction of consolidated volume, so they can't rebuild Alpaca's
// full-market bars) and sequence gaps (the JSON has no feed seq to diff).
//
// Excluded for BARS only: timestamp regression. An Alpaca minute bar carries the
// bar's START time and arrives *after* that minute's trades; trades and bars
// share one per-symbol last-timestamp, so a bar's start always looks earlier
// than the last trade and would trip a regression on every single bar. That's a
// representation artifact, not a data fault. It stays meaningful for trades.

struct LiveLabel {
    validate::Violation bit;
    const char*         text;
};

inline constexpr std::array<LiveLabel, 10> kLiveLabels{{
    {validate::kTradeNonPositivePrice, "trade non-positive price"},
    {validate::kTradeNonPositiveSize,  "trade non-positive size"},
    {validate::kPriceBandBreach,       "price-band outlier"},
    {validate::kTimestampRegression,   "timestamp regression"},
    {validate::kBarLowAboveHigh,       "bar low > high"},
    {validate::kBarOpenOutOfRange,     "bar open out of [low,high]"},
    {validate::kBarCloseOutOfRange,    "bar close out of [low,high]"},
    {validate::kBarVwapOutOfRange,     "bar vwap out of [low,high]"},
    {validate::kBarNonPositivePrice,   "bar non-positive price"},
    {validate::kBarVolumeInconsistent, "bar volume/trade-count inconsistent"},
}};

enum class RecordKind { Trade, Bar };

// The bits surfaced for this record kind: every label, minus timestamp
// regression for bars (see above).
inline std::uint32_t live_surfaced_mask(RecordKind kind) noexcept {
    std::uint32_t m = 0;
    for (const auto& l : kLiveLabels) m |= static_cast<std::uint32_t>(l.bit);
    if (kind == RecordKind::Bar)
        m &= ~static_cast<std::uint32_t>(validate::kTimestampRegression);
    return m;
}

// Comma-joined names of the surfaced violations set in `flags` for this kind;
// empty string when nothing surfaced fired.
inline std::string describe(std::uint32_t flags, RecordKind kind) {
    const std::uint32_t mask = live_surfaced_mask(kind);
    std::string         out;
    for (const auto& l : kLiveLabels) {
        if (flags & mask & static_cast<std::uint32_t>(l.bit)) {
            if (!out.empty()) out += ", ";
            out += l.text;
        }
    }
    return out;
}

}  // namespace ohlcv::ingest
