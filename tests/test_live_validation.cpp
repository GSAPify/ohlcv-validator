#include <gtest/gtest.h>

#include "ingest/live_report.h"
#include "ingest/parser.h"
#include "ingest/to_wire.h"
#include "validate/validator.h"

namespace {

using ohlcv::ingest::Parser;
using ohlcv::ingest::to_wire;
namespace v = ohlcv::validate;

// Drive the REAL live chain — Parser → adapt → Validator — with frame strings,
// so these run with no network and no Alpaca keys (i.e. with the market closed).
// Live IEX data is clean, so the value of the live path can't be shown live;
// this is where "it catches real-shaped bad data" is actually proven.

TEST(LiveValidation, CleanTradeStaysSilent) {
    Parser p;
    const auto f = p.parse(
        R"json([{"T":"t","i":1,"S":"AAPL","x":"V","p":144.5,"s":25,"z":"C",)json"
        R"json("t":"2022-01-12T19:46:00.000000000Z"}])json");
    ASSERT_EQ(f.trades.size(), 1U);
    v::Validator val;
    EXPECT_EQ(val.check(to_wire(f.trades[0], 1U)).flags, v::kNone);  // earned silence
}

TEST(LiveValidation, NegativePriceTradeCaught) {
    Parser p;
    const auto f = p.parse(
        R"json([{"T":"t","i":1,"S":"AAPL","x":"V","p":-1.0,"s":25,"z":"C",)json"
        R"json("t":"2022-01-12T19:46:00.000000000Z"}])json");
    ASSERT_EQ(f.trades.size(), 1U);
    v::Validator val;
    EXPECT_TRUE(val.check(to_wire(f.trades[0], 1U)).has(v::kTradeNonPositivePrice));
}

TEST(LiveValidation, InvertedBarBandCaught) {
    Parser p;
    const auto f = p.parse(
        R"json([{"T":"b","S":"AAPL","o":175.6,"h":175.5,"l":175.8,"c":175.7,)json"
        R"json("v":1234,"t":"2022-01-12T19:30:00.000000000Z","n":50,)json"
        R"json("vw":175.65}])json");
    ASSERT_EQ(f.bars.size(), 1U);
    v::Validator val;
    EXPECT_TRUE(val.check(to_wire(f.bars[0], 1U)).has(v::kBarLowAboveHigh));
}

TEST(LiveValidation, PriceBandOutlierCaughtAcrossFrames) {
    Parser       p;
    v::Validator val;  // one validator: per-symbol state must carry across frames

    const auto f1 = p.parse(
        R"json([{"T":"t","i":1,"S":"AAPL","x":"V","p":100.0,"s":10,"z":"C",)json"
        R"json("t":"2026-05-28T14:30:00.000000000Z"}])json");
    ASSERT_EQ(f1.trades.size(), 1U);
    EXPECT_FALSE(val.check(to_wire(f1.trades[0], 1U)).has(v::kPriceBandBreach));  // warms ref

    const auto f2 = p.parse(
        R"json([{"T":"t","i":2,"S":"AAPL","x":"V","p":110.0,"s":10,"z":"C",)json"
        R"json("t":"2026-05-28T14:30:01.000000000Z"}])json");
    ASSERT_EQ(f2.trades.size(), 1U);
    EXPECT_TRUE(val.check(to_wire(f2.trades[0], 2U)).has(v::kPriceBandBreach));  // +10% > 5% band
}

TEST(LiveValidation, CleanQuoteStaysSilent) {
    Parser p;
    const auto f = p.parse(
        R"json([{"T":"q","S":"AAPL","bx":"V","bp":144.50,"bs":2,"ax":"W","ap":144.60,)json"
        R"json("as":3,"z":"C","t":"2022-01-12T19:46:00.000000000Z"}])json");
    ASSERT_EQ(f.quotes.size(), 1U);
    v::Validator val;
    EXPECT_EQ(val.check(to_wire(f.quotes[0], 1U)).flags, v::kNone);  // bid < ask, sizes > 0
}

TEST(LiveValidation, CrossedQuoteCaught) {
    Parser p;
    const auto f = p.parse(
        R"json([{"T":"q","S":"AAPL","bx":"V","bp":144.60,"bs":2,"ax":"W","ap":144.50,)json"
        R"json("as":3,"z":"C","t":"2022-01-12T19:46:00.000000000Z"}])json");
    ASSERT_EQ(f.quotes.size(), 1U);
    v::Validator val;
    EXPECT_TRUE(val.check(to_wire(f.quotes[0], 1U)).has(v::kQuoteCrossed));  // bid > ask
}

// An Alpaca minute bar's start_ns precedes its trades, and trades+bars share the
// per-symbol last-timestamp, so the validator flags a regression on the bar.
// That's a feed/representation artifact — which is exactly why the live report
// suppresses timestamp regression for bars (next test).
TEST(LiveValidation, BarAfterTradeRegressesAtValidatorLevel) {
    v::Validator        val;
    ohlcv::model::Trade t;
    t.symbol = "AAPL";
    t.price  = 100.0;
    t.size   = 10;
    t.ts_ns  = 1'700'000'059'000'000'000ULL;  // ...:59 — a trade late in the minute
    ohlcv::model::Bar b;
    b.symbol = "AAPL";
    b.open = b.high = b.low = b.close = b.vwap = 100.0;
    b.volume = 10;
    b.trade_count = 1;
    b.start_ns = 1'700'000'000'000'000'000ULL;  // ...:00 — the bar's window start, earlier
    (void)val.check(to_wire(t, 1U));
    EXPECT_TRUE(val.check(to_wire(b, 2U)).has(v::kTimestampRegression));
}

// A quote and a trade are separate streams sharing one per-symbol clock inside
// the validator: a quote advances last_ts, so a trade with a slightly earlier
// event time that follows it false-flags regression. This is *why* the live
// report suppresses timestamp regression for the whole stream (next test) — the
// report layer can't fix it, because the quote already mutated last_ts.
TEST(LiveValidation, QuotePollutesSharedTimestampClock) {
    v::Validator        val;
    ohlcv::model::Quote q;
    q.symbol = "AAPL";
    q.bid_price = 100.0;
    q.ask_price = 100.1;
    q.bid_size = q.ask_size = 1;
    q.ts_ns = 1'700'000'000'500'000'000ULL;     // ...000.500
    ohlcv::model::Trade t;
    t.symbol = "AAPL";
    t.price  = 100.05;
    t.size   = 10;
    t.ts_ns  = 1'700'000'000'400'000'000ULL;     // ...000.400 — earlier event time
    (void)val.check(to_wire(q, 1U));
    EXPECT_TRUE(val.check(to_wire(t, 2U)).has(v::kTimestampRegression));
}

// The report layer surfaces only clock-independent value checks. The
// clock-coupled / coverage-dependent ones (timestamp regression, sequence gap,
// reconstruction) are never surfaced live; quote value checks are.
TEST(LiveReport, SuppressesClockCoupledSurfacesValueChecks) {
    using ohlcv::ingest::describe;
    using ohlcv::ingest::kSurfacedMask;

    EXPECT_FALSE(kSurfacedMask & v::kTimestampRegression);
    EXPECT_FALSE(kSurfacedMask & v::kSequenceGap);
    EXPECT_FALSE(kSurfacedMask & v::kBarVwapReconstructMismatch);
    EXPECT_EQ(describe(v::kTimestampRegression), "");

    EXPECT_TRUE(kSurfacedMask & v::kQuoteCrossed);
    EXPECT_EQ(describe(v::kQuoteCrossed), "quote crossed (bid > ask)");
    EXPECT_EQ(describe(v::kBarLowAboveHigh), "bar low > high");
}

}  // namespace
