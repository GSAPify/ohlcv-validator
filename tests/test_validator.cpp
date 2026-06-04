#include <gtest/gtest.h>

#include <cstring>

#include "validate/validator.h"

using ohlcv::model::WireBar;
using ohlcv::model::WireQuote;
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

// ---- Trade→bar reconstruction (cross-record, stateful) ---------------------

namespace {

// Build a bar with explicit reconstruction fields over make_bar's symbol/seq/ts.
WireBar bar_with(const char* sym, std::uint64_t seq, std::uint64_t ts,
                 std::uint64_t volume, std::uint64_t count, double open,
                 double high, double low, double close, double vwap) {
    WireBar b   = make_bar(sym, seq, ts);
    b.volume      = volume;
    b.trade_count = count;
    b.open  = open;
    b.high  = high;
    b.low   = low;
    b.close = close;
    b.vwap  = vwap;
    return b;
}

// Feed the canonical three-trade window for AAPL into v.
// aggregate: volume 60, count 3, vwap 6070/60 = 101.1666…, O100 H102 L100 C101.
void feed_three_trades(Validator& v) {
    (void)v.check(make_trade("AAPL", 1, 1000, 100.0, 10));
    (void)v.check(make_trade("AAPL", 2, 2000, 102.0, 20));
    (void)v.check(make_trade("AAPL", 3, 3000, 101.0, 30));
}

constexpr double kReconVwap = 6070.0 / 60.0;

}  // namespace

TEST(Validator, ReconstructionConsistentBarPasses) {
    Validator v;
    feed_three_trades(v);
    auto b = bar_with("AAPL", 4, 4000, 60, 3, 100.0, 102.0, 100.0, 101.0,
                      kReconVwap);
    EXPECT_TRUE(v.check(b).ok());
}

TEST(Validator, ReconstructionVolumeMismatch) {
    Validator v;
    feed_three_trades(v);
    auto b = bar_with("AAPL", 4, 4000, 61, 3, 100.0, 102.0, 100.0, 101.0,
                      kReconVwap);  // volume off by one
    auto r = v.check(b);
    EXPECT_TRUE(r.has(v::kBarVolumeReconstructMismatch));
    EXPECT_FALSE(r.has(v::kBarVwapReconstructMismatch));
    EXPECT_FALSE(r.has(v::kBarOhlcReconstructMismatch));
}

TEST(Validator, ReconstructionTradeCountMismatch) {
    Validator v;
    feed_three_trades(v);
    auto b = bar_with("AAPL", 4, 4000, 60, 4, 100.0, 102.0, 100.0, 101.0,
                      kReconVwap);  // count says 4, only 3 trades
    EXPECT_TRUE(v.check(b).has(v::kBarTradeCountReconstructMismatch));
}

TEST(Validator, ReconstructionVwapMismatch) {
    Validator v;
    feed_three_trades(v);
    // 100.5 is inside the band [100,102] (so no kBarVwapOutOfRange) but is not
    // the reconstructed VWAP — only the reconstruction flag should fire.
    auto b = bar_with("AAPL", 4, 4000, 60, 3, 100.0, 102.0, 100.0, 101.0, 100.5);
    auto r = v.check(b);
    EXPECT_TRUE(r.has(v::kBarVwapReconstructMismatch));
    EXPECT_FALSE(r.has(v::kBarVwapOutOfRange));
}

TEST(Validator, ReconstructionOhlcMismatch) {
    Validator v;
    feed_three_trades(v);
    // high claims 103 but the trades only reached 102.
    auto b = bar_with("AAPL", 4, 4000, 60, 3, 100.0, 103.0, 100.0, 101.0,
                      kReconVwap);
    auto r = v.check(b);
    EXPECT_TRUE(r.has(v::kBarOhlcReconstructMismatch));
    EXPECT_FALSE(r.has(v::kBarVolumeReconstructMismatch));
}

// The test that proves the check is not a tautology: a feed that rounds VWAP to
// four decimals must still pass; one off by more than the tolerance must fail.
TEST(Validator, ReconstructionToleratesRounding) {
    {
        Validator v;
        feed_three_trades(v);
        auto b = bar_with("AAPL", 4, 4000, 60, 3, 100.0, 102.0, 100.0, 101.0,
                          101.1667);  // 101.16666… rounded to 4 dp
        EXPECT_FALSE(v.check(b).has(v::kBarVwapReconstructMismatch));
    }
    {
        Validator v;
        feed_three_trades(v);
        auto b = bar_with("AAPL", 4, 4000, 60, 3, 100.0, 102.0, 100.0, 101.0,
                          101.17);  // off by ~3e-5 relative — beyond tolerance
        EXPECT_TRUE(v.check(b).has(v::kBarVwapReconstructMismatch));
    }
}

TEST(Validator, ReconstructionResetsBetweenBars) {
    Validator v;
    (void)v.check(make_trade("AAPL", 1, 1000, 100.0, 10));
    auto b1 = bar_with("AAPL", 2, 2000, 10, 1, 100.0, 100.0, 100.0, 100.0, 100.0);
    EXPECT_TRUE(v.check(b1).ok());

    // Second window: if the accumulator didn't reset, this bar would be compared
    // against volume 15 / count 2 and fail.
    (void)v.check(make_trade("AAPL", 3, 3000, 200.0, 5));
    auto b2 = bar_with("AAPL", 4, 4000, 5, 1, 200.0, 200.0, 200.0, 200.0, 200.0);
    EXPECT_TRUE(v.check(b2).ok());
}

TEST(Validator, ReconstructionIsPerSymbol) {
    Validator v;
    (void)v.check(make_trade("AAPL", 1, 1000, 100.0, 10));
    (void)v.check(make_trade("MSFT", 1, 1000, 200.0, 5));
    (void)v.check(make_trade("AAPL", 2, 2000, 100.0, 10));

    auto ba = bar_with("AAPL", 3, 3000, 20, 2, 100.0, 100.0, 100.0, 100.0, 100.0);
    EXPECT_TRUE(v.check(ba).ok());
    auto bm = bar_with("MSFT", 2, 2000, 5, 1, 200.0, 200.0, 200.0, 200.0, 200.0);
    EXPECT_TRUE(v.check(bm).ok());
}

TEST(Validator, ReconstructionExcludesInvalidTrades) {
    Validator v;
    (void)v.check(make_trade("AAPL", 1, 1000, 100.0, 10));      // valid
    auto bad = v.check(make_trade("AAPL", 2, 2000, -1.0, 10));  // negative price
    EXPECT_TRUE(bad.has(v::kTradeNonPositivePrice));

    // The bar reconstructs from the single VALID trade only.
    auto b = bar_with("AAPL", 3, 3000, 10, 1, 100.0, 100.0, 100.0, 100.0, 100.0);
    auto r = v.check(b);
    EXPECT_FALSE(r.has(v::kBarVolumeReconstructMismatch));
    EXPECT_FALSE(r.has(v::kBarTradeCountReconstructMismatch));
    EXPECT_FALSE(r.has(v::kBarVwapReconstructMismatch));
    EXPECT_FALSE(r.has(v::kBarOhlcReconstructMismatch));
}

TEST(Validator, ReconstructionSkippedWhenNoTrades) {
    Validator v;
    // A symbol's first bar has no constituent trades — reconstruction is skipped
    // even though make_bar's volume/count don't correspond to any trades.
    EXPECT_TRUE(v.check(make_bar("ZZZ", 1, 1000)).ok());
}

// ---- Quote invariants ------------------------------------------------------

namespace {

WireQuote make_quote(const char* sym, std::uint64_t seq, std::uint64_t ts,
                     double bid = 100.0, double ask = 100.5,
                     std::uint64_t bid_size = 5, std::uint64_t ask_size = 5) {
    WireQuote q{};
    std::strncpy(q.symbol, sym, sizeof(q.symbol));
    q.seq          = seq;
    q.ts_ns        = ts;
    q.bid_price    = bid;
    q.ask_price    = ask;
    q.bid_size     = bid_size;
    q.ask_size     = ask_size;
    q.bid_exchange = 'V';
    q.ask_exchange = 'V';
    q.tape         = 'C';
    return q;
}

}  // namespace

TEST(Validator, CleanQuotePasses) {
    Validator v;
    EXPECT_TRUE(v.check(make_quote("AAPL", 1, 1000)).ok());
}

TEST(Validator, QuoteCrossed) {
    Validator v;
    // bid above ask — crossed book.
    auto r = v.check(make_quote("AAPL", 1, 1000, /*bid=*/101.0, /*ask=*/100.0));
    EXPECT_TRUE(r.has(v::kQuoteCrossed));
    EXPECT_FALSE(r.has(v::kQuoteLocked));
}

TEST(Validator, QuoteLocked) {
    Validator v;
    auto r = v.check(make_quote("AAPL", 1, 1000, /*bid=*/100.0, /*ask=*/100.0));
    EXPECT_TRUE(r.has(v::kQuoteLocked));
    EXPECT_FALSE(r.has(v::kQuoteCrossed));
}

TEST(Validator, QuoteNonPositive) {
    Validator v;
    auto r = v.check(make_quote("AAPL", 1, 1000, /*bid=*/0.0, /*ask=*/100.5));
    EXPECT_TRUE(r.has(v::kQuoteNonPositive));
}

TEST(Validator, QuoteZeroSize) {
    Validator v;
    auto r = v.check(make_quote("AAPL", 1, 1000, 100.0, 100.5,
                                /*bid_size=*/0, /*ask_size=*/5));
    EXPECT_TRUE(r.has(v::kQuoteZeroSize));
}

TEST(Validator, QuotesShareSymbolSequencing) {
    Validator v;
    EXPECT_TRUE(v.check(make_trade("AAPL", 1, 1000)).ok());
    // A quote on the same symbol continues the seq stream; a jump shows a gap.
    auto r = v.check(make_quote("AAPL", 5, 2000));
    EXPECT_TRUE(r.has(v::kSequenceGap));
    EXPECT_EQ(r.gap, 3U);
}
