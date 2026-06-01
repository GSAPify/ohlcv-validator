#include <gtest/gtest.h>

#include <cstring>

#include "validate/validator.h"

using ohlcv::model::WireBar;
using ohlcv::model::WireTrade;
using ohlcv::validate::Validator;
namespace v = ohlcv::validate;

namespace {

WireTrade make_trade(const char* sym, std::uint64_t seq, std::uint64_t ts,
                     double price = 100.0, std::uint64_t size = 10) {
    WireTrade t{};
    std::strncpy(t.symbol, sym, sizeof(t.symbol));
    t.seq      = seq;
    t.ts_ns    = ts;
    t.price    = price;
    t.size     = size;
    t.exchange = 'V';
    t.tape     = 'C';
    return t;
}

// A bar that passes every per-record invariant.
WireBar make_bar(const char* sym, std::uint64_t seq, std::uint64_t ts) {
    WireBar b{};
    std::strncpy(b.symbol, sym, sizeof(b.symbol));
    b.seq         = seq;
    b.start_ns    = ts;
    b.open        = 100.0;
    b.high        = 101.0;
    b.low         = 99.0;
    b.close       = 100.5;
    b.vwap        = 100.2;
    b.volume      = 500;
    b.trade_count = 5;
    return b;
}

}  // namespace

// ---- Trade per-record invariants -------------------------------------------

TEST(Validator, CleanTradePasses) {
    Validator v;
    EXPECT_TRUE(v.check(make_trade("AAPL", 1, 1000)).ok());
}

TEST(Validator, TradeNonPositivePrice) {
    Validator v;
    auto r = v.check(make_trade("AAPL", 1, 1000, /*price=*/0.0));
    EXPECT_TRUE(r.has(v::kTradeNonPositivePrice));
}

TEST(Validator, TradeZeroSize) {
    Validator v;
    auto r = v.check(make_trade("AAPL", 1, 1000, 100.0, /*size=*/0));
    EXPECT_TRUE(r.has(v::kTradeNonPositiveSize));
}

// ---- Bar per-record invariants ---------------------------------------------

TEST(Validator, CleanBarPasses) {
    Validator v;
    EXPECT_TRUE(v.check(make_bar("AAPL", 1, 1000)).ok());
}

TEST(Validator, BarInvertedBand) {
    Validator v;
    auto b = make_bar("AAPL", 1, 1000);
    b.low  = 200.0;  // low now above high
    auto r = v.check(b);
    EXPECT_TRUE(r.has(v::kBarLowAboveHigh));
}

TEST(Validator, BarCloseAboveHigh) {
    Validator v;
    auto b  = make_bar("AAPL", 1, 1000);
    b.close = 105.0;  // outside [low, high]
    EXPECT_TRUE(v.check(b).has(v::kBarCloseOutOfRange));
}

TEST(Validator, BarVwapOutOfBand) {
    Validator v;
    auto b = make_bar("AAPL", 1, 1000);
    b.vwap = 50.0;
    EXPECT_TRUE(v.check(b).has(v::kBarVwapOutOfRange));
}

TEST(Validator, BarVolumeWithoutTrades) {
    Validator v;
    auto b        = make_bar("AAPL", 1, 1000);
    b.trade_count = 0;   // but volume is still 500
    EXPECT_TRUE(v.check(b).has(v::kBarVolumeInconsistent));
}

TEST(Validator, BarMultipleViolationsAtOnce) {
    Validator v;
    auto b  = make_bar("AAPL", 1, 1000);
    b.low   = 200.0;     // inverted band
    b.close = 300.0;     // out of range
    auto r  = v.check(b);
    EXPECT_TRUE(r.has(v::kBarLowAboveHigh));
    EXPECT_TRUE(r.has(v::kBarCloseOutOfRange));
}

// ---- Sequencing (stateful, per-symbol) -------------------------------------

TEST(Validator, MonotonicStreamPasses) {
    Validator v;
    for (std::uint64_t i = 1; i <= 100; ++i) {
        ASSERT_TRUE(v.check(make_trade("AAPL", i, i * 1000)).ok())
            << "failed at seq " << i;
    }
}

TEST(Validator, TimestampRegressionDetected) {
    Validator v;
    EXPECT_TRUE(v.check(make_trade("AAPL", 1, 5000)).ok());
    auto r = v.check(make_trade("AAPL", 2, 4000));  // ts went backwards
    EXPECT_TRUE(r.has(v::kTimestampRegression));
}

TEST(Validator, SequenceGapDetectedWithCount) {
    Validator v;
    EXPECT_TRUE(v.check(make_trade("AAPL", 10, 1000)).ok());
    auto r = v.check(make_trade("AAPL", 14, 2000));  // skipped 11,12,13
    EXPECT_TRUE(r.has(v::kSequenceGap));
    EXPECT_EQ(r.gap, 3U);
}

TEST(Validator, SequencingIsPerSymbol) {
    Validator v;
    // Interleaving two symbols must not cross-contaminate their sequencing.
    EXPECT_TRUE(v.check(make_trade("AAPL", 1, 1000)).ok());
    EXPECT_TRUE(v.check(make_trade("MSFT", 1, 1000)).ok());
    EXPECT_TRUE(v.check(make_trade("AAPL", 2, 2000)).ok());
    EXPECT_TRUE(v.check(make_trade("MSFT", 2, 2000)).ok());
}

TEST(Validator, TradesAndBarsShareSymbolSequencing) {
    Validator v;
    EXPECT_TRUE(v.check(make_trade("AAPL", 1, 1000)).ok());
    // A bar on the same symbol continues the same seq stream; a gap shows.
    auto r = v.check(make_bar("AAPL", 5, 2000));
    EXPECT_TRUE(r.has(v::kSequenceGap));
    EXPECT_EQ(r.gap, 3U);
}
