#include <gtest/gtest.h>

#include "lob/price_level.hpp"

using namespace lob;

TEST(PriceLevel, PushBackKeepsFifoOrder) {
    PriceLevel lvl; lvl.price = 1000;
    Order a{1, 1000, 10, Side::Buy, 1}, b{2, 1000, 20, Side::Buy, 2}, c{3, 1000, 30, Side::Buy, 3};
    lvl.push_back(&a); lvl.push_back(&b); lvl.push_back(&c);
    EXPECT_EQ(lvl.head, &a); EXPECT_EQ(lvl.tail, &c);
    EXPECT_EQ(a.next, &b); EXPECT_EQ(b.next, &c); EXPECT_EQ(c.prev, &b);
    EXPECT_EQ(lvl.total_qty, 60); EXPECT_EQ(lvl.num_orders, 3u);
    EXPECT_EQ(lvl.qty_ahead(&c), 30); EXPECT_EQ(lvl.qty_ahead(&a), 0);
}

TEST(PriceLevel, EraseMiddleHeadTail) {
    PriceLevel lvl; lvl.price = 1000;
    Order a{1, 1000, 10, Side::Buy, 1}, b{2, 1000, 20, Side::Buy, 2}, c{3, 1000, 30, Side::Buy, 3};
    lvl.push_back(&a); lvl.push_back(&b); lvl.push_back(&c);
    lvl.erase(&b);
    EXPECT_EQ(a.next, &c); EXPECT_EQ(c.prev, &a); EXPECT_EQ(lvl.total_qty, 40);
    lvl.erase(&a);
    EXPECT_EQ(lvl.head, &c); EXPECT_EQ(c.prev, nullptr);
    lvl.erase(&c);
    EXPECT_EQ(lvl.head, nullptr); EXPECT_EQ(lvl.tail, nullptr);
    EXPECT_TRUE(lvl.empty()); EXPECT_EQ(lvl.total_qty, 0);
}

TEST(PriceLevel, AnonCountsAsAhead) {
    PriceLevel lvl; lvl.price = 1000; lvl.anon_qty = 500;
    Order a{1, 1000, 10, Side::Buy, 1};
    lvl.push_back(&a);
    EXPECT_EQ(lvl.qty_ahead(&a), 500);
    EXPECT_EQ(lvl.visible_qty(), 510);
    EXPECT_FALSE(lvl.empty());
}
