#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ingest/live_report.h"

namespace ohlcv::ingest {

// One JSON object (one line → JSONL) describing a flagged record, for a
// machine-readable violations log that downstream tooling (or a future model)
// can consume — the structured counterpart to the human `!!` line.
//
// Two deliberate schema choices:
//
//   * Only the *surfaced* checks (kSurfacedMask) are listed. The validator also
//     computes reconstruction / sequence-gap / timestamp-regression, but those
//     are suppressed on the live feed (see live_report.h), so by design they do
//     not appear here either — this log mirrors what the operator is shown, not
//     every bit the validator set.
//
//   * 64-bit timestamps are emitted as STRINGS. A nanosecond epoch ts is ~1.7e18,
//     well past the 2^53 exact-integer range of the IEEE-754 doubles that JSON
//     tooling (jq, JavaScript, most parsers) uses for numbers — as a JSON number
//     it would silently truncate by hundreds of ns. `seq` is small and stays a
//     number.
inline std::string violation_json(std::string_view kind, std::string_view symbol,
                                  std::uint64_t ts, std::uint64_t seq,
                                  std::uint32_t flags) {
    nlohmann::json checks = nlohmann::json::array();
    for (const auto& l : kLiveLabels) {
        if (flags & kSurfacedMask & static_cast<std::uint32_t>(l.bit)) {
            checks.push_back(l.text);
        }
    }

    nlohmann::json j;
    j["kind"]   = std::string(kind);
    j["symbol"] = std::string(symbol);
    j["ts"]     = std::to_string(ts);  // string: see note above
    j["seq"]    = seq;
    j["checks"] = std::move(checks);
    return j.dump();
}

}  // namespace ohlcv::ingest
