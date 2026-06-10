#include <gtest/gtest.h>

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

}  // namespace
