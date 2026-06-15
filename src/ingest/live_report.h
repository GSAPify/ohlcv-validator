#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "validate/validator.h"

namespace ohlcv::ingest {

// Which validator flags are worth surfacing on a *sampled, live* feed. The
// validator runs every check; this is the report layer that decides what's
// meaningful here, so the validator itself stays single-path and untouched.
//
// Surfaced: the clock-INDEPENDENT value checks — a record is malformed or not,
// regardless of timing or feed coverage. Trade/bar/quote bits are disjoint per
// record type, so one mask applied to any record naturally surfaces only that
// record's relevant flags; no per-kind logic needed.
//
// NOT surfaced, each for a stated reason:
//   - Reconstruction mismatches — our IEX trades are a fraction of consolidated
//     volume, so they can't rebuild Alpaca's full-market bars.
//   - Sequence gaps — the JSON has no feed seq to diff (the live path assigns a
//     per-symbol monotonic seq, so this is structurally inert).
//   - Timestamp regression — the validator now tracks a per-stream `last_ts`, so
//     the old cross-stream false-flagging (a quote advancing one shared clock) is
//     fixed and the check is *correct*. It stays suppressed live for a different,
//     narrower reason: it's unverified on real delivery. IEX event timestamps
//     arrive over a websocket with no monotonic-delivery guarantee, and
//     same-timestamp / sub-µs-reordered ticks within one stream could flag benign
//     regressions. Un-suppressing is gated on a live measurement (count how often
//     it fires on real frames before trusting it) — a separate follow-up.

struct LiveLabel {
    validate::Violation bit;
    const char*         text;
};

inline constexpr std::array<LiveLabel, 13> kLiveLabels{{
    {validate::kTradeNonPositivePrice, "trade non-positive price"},
    {validate::kTradeNonPositiveSize,  "trade non-positive size"},
    {validate::kPriceBandBreach,       "price-band outlier"},  // trade or quote mid
    {validate::kBarLowAboveHigh,       "bar low > high"},
    {validate::kBarOpenOutOfRange,     "bar open out of [low,high]"},
    {validate::kBarCloseOutOfRange,    "bar close out of [low,high]"},
    {validate::kBarVwapOutOfRange,     "bar vwap out of [low,high]"},
    {validate::kBarNonPositivePrice,   "bar non-positive price"},
    {validate::kBarVolumeInconsistent, "bar volume/trade-count inconsistent"},
    {validate::kQuoteCrossed,          "quote crossed (bid > ask)"},
    {validate::kQuoteLocked,           "quote locked (bid == ask)"},
    {validate::kQuoteNonPositive,      "quote non-positive price"},
    {validate::kQuoteZeroSize,         "quote zero bid/ask size"},
}};

// The bits surfaced on the live feed: every value-check label, OR'd.
inline constexpr std::uint32_t kSurfacedMask = [] {
    std::uint32_t m = 0;
    for (const auto& l : kLiveLabels) m |= static_cast<std::uint32_t>(l.bit);
    return m;
}();

// Comma-joined names of the surfaced violations set in `flags`; empty string
// when nothing surfaced fired.
inline std::string describe(std::uint32_t flags) {
    std::string out;
    for (const auto& l : kLiveLabels) {
        if (flags & kSurfacedMask & static_cast<std::uint32_t>(l.bit)) {
            if (!out.empty()) out += ", ";
            out += l.text;
        }
    }
    return out;
}

}  // namespace ohlcv::ingest
