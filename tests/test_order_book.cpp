#include <cstdint>

#include <gtest/gtest.h>

#include "book/order_book.h"

using ohlcv::book::OrderBook;
using ohlcv::book::Side;

TEST(OrderBook, BestAtFrontBidsDescAsksAsc) {
    OrderBook<16> b;
    b.apply(Side::Bid, 99.0, 100);
    b.apply(Side::Bid, 99.5, 200);   // higher bid -> becomes best
    b.apply(Side::Bid, 98.5, 300);
    b.apply(Side::Ask, 100.5, 100);
    b.apply(Side::Ask, 100.0, 200);  // lower ask -> becomes best
    b.apply(Side::Ask, 101.0, 300);

    EXPECT_EQ(b.bid_levels(), 3U);
    EXPECT_EQ(b.ask_levels(), 3U);
    EXPECT_DOUBLE_EQ(b.best_bid(), 99.5);
    EXPECT_DOUBLE_EQ(b.best_ask(), 100.0);
    EXPECT_EQ(b.best_bid_size(), 200U);
    EXPECT_DOUBLE_EQ(b.spread(), 0.5);
    // bids descending, asks ascending
    EXPECT_DOUBLE_EQ(b.bid(0).price, 99.5);
    EXPECT_DOUBLE_EQ(b.bid(2).price, 98.5);
    EXPECT_DOUBLE_EQ(b.ask(0).price, 100.0);
    EXPECT_DOUBLE_EQ(b.ask(2).price, 101.0);
}

TEST(OrderBook, UpdateResizesRemoveDeletesLevel) {
    OrderBook<16> b;
    b.apply(Side::Bid, 99.0, 100);
    b.apply(Side::Bid, 99.0, 250);   // same price -> resize, not a new level
    EXPECT_EQ(b.bid_levels(), 1U);
    EXPECT_EQ(b.best_bid_size(), 250U);

    b.apply(Side::Bid, 98.0, 100);
    b.apply(Side::Bid, 99.0, 0);     // size 0 -> remove the 99.0 level
    EXPECT_EQ(b.bid_levels(), 1U);
    EXPECT_DOUBLE_EQ(b.best_bid(), 98.0);

    b.apply(Side::Bid, 97.0, 0);     // removing a level we don't have -> no-op
    EXPECT_EQ(b.bid_levels(), 1U);
}

TEST(OrderBook, CrossedAndLockedAreObservable) {
    OrderBook<16> b;
    b.apply(Side::Bid, 100.0, 100);
    b.apply(Side::Ask, 100.5, 100);
    EXPECT_FALSE(b.crossed());

    b.apply(Side::Ask, 100.0, 100);  // ask == bid -> locked
    EXPECT_TRUE(b.crossed());

    b.apply(Side::Ask, 99.5, 100);   // ask < bid -> crossed
    EXPECT_TRUE(b.crossed());
}

TEST(OrderBook, EqualityIgnoresInsertionOrder) {
    OrderBook<16> a, b;
    a.apply(Side::Bid, 99.0, 100);
    a.apply(Side::Bid, 99.5, 200);
    a.apply(Side::Ask, 100.0, 300);

    b.apply(Side::Ask, 100.0, 300);  // same levels, different order
    b.apply(Side::Bid, 99.5, 200);
    b.apply(Side::Bid, 99.0, 100);

    EXPECT_TRUE(a == b);
    b.apply(Side::Bid, 99.5, 201);   // one size differs
    EXPECT_FALSE(a == b);
}

TEST(OrderBook, KeepsBestLevelsAtCapacity) {
    OrderBook<2> b;  // tiny: only the best 2 bids kept
    b.apply(Side::Bid, 99.0, 10);
    b.apply(Side::Bid, 98.0, 10);
    b.apply(Side::Bid, 100.0, 10);  // better than the worst -> evicts 98.0
    EXPECT_EQ(b.bid_levels(), 2U);
    EXPECT_DOUBLE_EQ(b.bid(0).price, 100.0);
    EXPECT_DOUBLE_EQ(b.bid(1).price, 99.0);
    b.apply(Side::Bid, 1.0, 10);    // worse than everything kept -> dropped
    EXPECT_EQ(b.bid_levels(), 2U);
    EXPECT_DOUBLE_EQ(b.bid(1).price, 99.0);
}
