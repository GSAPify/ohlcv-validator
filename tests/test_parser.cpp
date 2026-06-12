#include <gtest/gtest.h>

#include "ingest/parser.h"

using ohlcv::ingest::Parser;

// Captured live during the 2026-05-26 smoke test.
constexpr std::string_view kConnectedFrame =
    R"json([{"T":"success","msg":"connected"}])json";
constexpr std::string_view kAuthenticatedFrame =
    R"json([{"T":"success","msg":"authenticated"}])json";
constexpr std::string_view kSubscriptionFrame =
    R"json([{"T":"subscription","trades":["AAPL"],"bars":["AAPL"],)json"
    R"json("corrections":["AAPL"],"cancelErrors":["AAPL"]}])json";

// Hand-crafted to match Alpaca's documented schema.
constexpr std::string_view kSingleTradeFrame =
    R"json([{"T":"t","i":96921,"S":"AAPL","x":"V","p":144.5,"s":25,)json"
    R"json("c":["@","I"],"z":"C","t":"2022-01-12T19:46:00.000000000Z"}])json";
constexpr std::string_view kSingleBarFrame =
    R"json([{"T":"b","S":"AAPL","o":175.6,"h":175.8,"l":175.5,"c":175.7,)json"
    R"json("v":1234,"t":"2022-01-12T19:30:00.000000000Z","n":50,)json"
    R"json("vw":175.65}])json";
constexpr std::string_view kSingleQuoteFrame =
    R"json([{"T":"q","S":"AAPL","bx":"V","bp":144.50,"bs":2,"ax":"W",)json"
    R"json("ap":144.60,"as":7,"z":"C","t":"2022-01-12T19:46:00.000000000Z"}])json";
constexpr std::string_view kMixedFrame =
    R"json([{"T":"t","i":1,"S":"AAPL","x":"V","p":100.0,"s":10,"z":"C",)json"
    R"json("t":"2026-05-28T14:30:00.000000000Z"},)json"
    R"json({"T":"b","S":"AAPL","o":99.9,"h":100.1,"l":99.8,"c":100.0,)json"
    R"json("v":500,"n":5,"vw":99.95,"t":"2026-05-28T14:30:00.000000000Z"}])json";
constexpr std::string_view kErrorFrame =
    R"json([{"T":"error","code":402,"msg":"auth failed"}])json";

TEST(Parser, ConnectedAck) {
    Parser p;
    const auto r = p.parse(kConnectedFrame);
    EXPECT_TRUE(r.has_connected_ack);
    EXPECT_FALSE(r.has_authenticated_ack);
    EXPECT_TRUE(r.trades.empty());
    EXPECT_TRUE(r.bars.empty());
}

TEST(Parser, AuthenticatedAck) {
    Parser p;
    const auto r = p.parse(kAuthenticatedFrame);
    EXPECT_TRUE(r.has_authenticated_ack);
    EXPECT_FALSE(r.has_connected_ack);
}

TEST(Parser, SubscriptionAck) {
    Parser p;
    const auto r = p.parse(kSubscriptionFrame);
    EXPECT_TRUE(r.has_subscription_ack);
}

TEST(Parser, SingleTrade) {
    Parser p;
    const auto r = p.parse(kSingleTradeFrame);
    ASSERT_EQ(r.trades.size(), 1U);
    const auto& t = r.trades[0];
    EXPECT_EQ(t.symbol, "AAPL");
    EXPECT_EQ(t.trade_id, 96921U);
    EXPECT_EQ(t.exchange, "V");
    EXPECT_DOUBLE_EQ(t.price, 144.5);
    EXPECT_EQ(t.size, 25U);
    EXPECT_EQ(t.tape, "C");
    EXPECT_EQ(t.ts_ns, 1'642'016'760'000'000'000ULL);
}

TEST(Parser, SingleBar) {
    Parser p;
    const auto r = p.parse(kSingleBarFrame);
    ASSERT_EQ(r.bars.size(), 1U);
    const auto& b = r.bars[0];
    EXPECT_EQ(b.symbol, "AAPL");
    EXPECT_DOUBLE_EQ(b.open, 175.6);
    EXPECT_DOUBLE_EQ(b.high, 175.8);
    EXPECT_DOUBLE_EQ(b.low, 175.5);
    EXPECT_DOUBLE_EQ(b.close, 175.7);
    EXPECT_EQ(b.volume, 1234U);
    EXPECT_EQ(b.trade_count, 50U);
    EXPECT_DOUBLE_EQ(b.vwap, 175.65);
    EXPECT_EQ(b.start_ns, 1'642'015'800'000'000'000ULL);
}

TEST(Parser, SingleQuote) {
    Parser p;
    const auto r = p.parse(kSingleQuoteFrame);
    ASSERT_EQ(r.quotes.size(), 1U);
    const auto& q = r.quotes[0];
    EXPECT_EQ(q.symbol, "AAPL");
    EXPECT_EQ(q.bid_exchange, "V");
    EXPECT_DOUBLE_EQ(q.bid_price, 144.50);
    EXPECT_EQ(q.bid_size, 2U);
    EXPECT_EQ(q.ask_exchange, "W");
    EXPECT_DOUBLE_EQ(q.ask_price, 144.60);
    EXPECT_EQ(q.ask_size, 7U);          // distinct from bid_size → catches a bs/as swap
    EXPECT_EQ(q.tape, "C");
    EXPECT_EQ(q.ts_ns, 1'642'016'760'000'000'000ULL);
}

TEST(Parser, MixedTradeAndBar) {
    Parser p;
    const auto r = p.parse(kMixedFrame);
    EXPECT_EQ(r.trades.size(), 1U);
    EXPECT_EQ(r.bars.size(), 1U);
}

TEST(Parser, ErrorFrame) {
    Parser p;
    const auto r = p.parse(kErrorFrame);
    ASSERT_EQ(r.errors.size(), 1U);
    EXPECT_EQ(r.errors[0], "auth failed");
}

TEST(Parser, ReuseAcrossManyFrames) {
    Parser p;
    for (int i = 0; i < 1000; ++i) {
        const auto r = p.parse(kSingleTradeFrame);
        ASSERT_EQ(r.trades.size(), 1U);
        EXPECT_EQ(r.trades[0].price, 144.5);
    }
}

TEST(Parser, UnknownMessageType) {
    constexpr std::string_view weird =
        R"json([{"T":"someNewType","foo":"bar"}])json";
    Parser p;
    const auto r = p.parse(weird);
    EXPECT_TRUE(r.trades.empty());
    ASSERT_EQ(r.unknown_types.size(), 1U);
    EXPECT_EQ(r.unknown_types[0], "someNewType");
}

TEST(Parser, ThrowsOnMalformedJson) {
    Parser p;
    EXPECT_THROW(p.parse(R"json(not json at all)json"), std::exception);
}
