#include <cstdint>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "ingest/violation_log.h"
#include "validate/validator.h"

namespace v = ohlcv::validate;
using ohlcv::ingest::violation_json;

namespace {

TEST(ViolationLog, SerializesFieldsAndChecks) {
    const auto j = nlohmann::json::parse(violation_json(
        "quote", "AAPL", 1'700'000'000'500'000'000ULL, 42, v::kQuoteCrossed));
    EXPECT_EQ(j["kind"], "quote");
    EXPECT_EQ(j["symbol"], "AAPL");
    EXPECT_EQ(j["seq"], 42);
    ASSERT_TRUE(j["checks"].is_array());
    ASSERT_EQ(j["checks"].size(), 1U);
    EXPECT_EQ(j["checks"][0], "quote crossed (bid > ask)");
}

// The whole reason ts is a string: as a JSON number a ~1.7e18 ns timestamp
// exceeds 2^53 and truncates in double-based tooling (jq, JS).
TEST(ViolationLog, TimestampIsExactString) {
    const std::uint64_t ts = 1'700'000'000'123'456'789ULL;
    const auto j = nlohmann::json::parse(
        violation_json("trade", "X", ts, 1, v::kTradeNonPositivePrice));
    EXPECT_TRUE(j["ts"].is_string());
    EXPECT_EQ(j["ts"].get<std::string>(), "1700000000123456789");
}

// Suppressed checks (here: timestamp regression) must not appear in the log,
// even when the validator set the bit — the log mirrors the live surfaced set.
TEST(ViolationLog, SuppressedChecksAreNotLogged) {
    const auto j = nlohmann::json::parse(violation_json(
        "trade", "X", 1, 1, v::kTimestampRegression | v::kTradeNonPositivePrice));
    ASSERT_EQ(j["checks"].size(), 1U);
    EXPECT_EQ(j["checks"][0], "trade non-positive price");
}

// Two distinct checks on one record both appear.
TEST(ViolationLog, MultipleChecksListed) {
    const auto j = nlohmann::json::parse(violation_json(
        "quote", "X", 1, 1, v::kQuoteCrossed | v::kQuoteZeroSize));
    EXPECT_EQ(j["checks"].size(), 2U);
}

}  // namespace
