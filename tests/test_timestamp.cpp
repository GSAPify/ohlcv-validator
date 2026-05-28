#include <gtest/gtest.h>

#include "model/timestamp.h"

using ohlcv::model::parse_rfc3339_nano;

TEST(Timestamp, UnixEpoch) {
    EXPECT_EQ(parse_rfc3339_nano("1970-01-01T00:00:00.000000000Z"), 0ULL);
}

TEST(Timestamp, OneSecondAfterEpoch) {
    EXPECT_EQ(parse_rfc3339_nano("1970-01-01T00:00:01.000000000Z"),
              1'000'000'000ULL);
}

TEST(Timestamp, NanosecondPrecision) {
    EXPECT_EQ(parse_rfc3339_nano("1970-01-01T00:00:00.000000001Z"), 1ULL);
}

TEST(Timestamp, AlpacaExampleFromDocs) {
    // From Alpaca's own example: 2022-01-12T19:46:00Z = 1642016760 unix sec.
    EXPECT_EQ(parse_rfc3339_nano("2022-01-12T19:46:00.000000000Z"),
              1'642'016'760ULL * 1'000'000'000ULL);
}

TEST(Timestamp, MillisecondShortForm) {
    // ".123" should scale up to 123_000_000 ns.
    EXPECT_EQ(parse_rfc3339_nano("1970-01-01T00:00:00.123Z"), 123'000'000ULL);
}

TEST(Timestamp, MicrosecondShortForm) {
    EXPECT_EQ(parse_rfc3339_nano("1970-01-01T00:00:00.000123Z"), 123'000ULL);
}

TEST(Timestamp, NoFractionalSeconds) {
    EXPECT_EQ(parse_rfc3339_nano("1970-01-01T00:00:00Z"), 0ULL);
}

TEST(Timestamp, RejectsMissingTrailingZ) {
    EXPECT_THROW((void)parse_rfc3339_nano("1970-01-01T00:00:00.000000000"),
                 std::runtime_error);
}

TEST(Timestamp, RejectsMalformed) {
    EXPECT_THROW((void)parse_rfc3339_nano("not a timestamp"),
                 std::runtime_error);
}

TEST(Timestamp, MonotonicAcrossOneNanosecond) {
    const auto a = parse_rfc3339_nano("2026-05-28T14:30:00.000000000Z");
    const auto b = parse_rfc3339_nano("2026-05-28T14:30:00.000000001Z");
    EXPECT_EQ(b - a, 1ULL);
}
