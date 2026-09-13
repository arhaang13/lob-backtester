#include <gtest/gtest.h>

#include <vector>

#include "lob/lobster.hpp"
#include "lob/replay.hpp"

using namespace lob;

namespace {
struct Cols {
    std::vector<std::int64_t> ts, id, price;
    std::vector<std::int8_t> type, dir;
    std::vector<std::int32_t> size;
    void push(Ts t, int ty, OrderId i, Qty s, Price p, int d) {
        ts.push_back(t); type.push_back(static_cast<std::int8_t>(ty)); id.push_back(i);
        size.push_back(s); price.push_back(p); dir.push_back(static_cast<std::int8_t>(d));
    }
    MessageColumns view() const {
        return {ts.data(), type.data(), id.data(), size.data(), price.data(), dir.data(), ts.size()};
    }
};
}  // namespace

TEST(Lobster, ApplyAllTypes) {
    OrderBook b; ApplyStats st;
    apply(b, {1, MsgType::NewLimit, 10, 100, 2200000, Side::Buy}, st);
    apply(b, {2, MsgType::NewLimit, 11, 200, 2200100, Side::Sell}, st);
    apply(b, {3, MsgType::PartialCancel, 10, 40, 2200000, Side::Buy}, st);
    EXPECT_EQ(b.best_bid_qty(), 60);
    apply(b, {4, MsgType::ExecVisible, 11, 50, 2200100, Side::Sell}, st);
    EXPECT_EQ(b.best_ask_qty(), 150);
    apply(b, {5, MsgType::ExecHidden, 0, 50, 2200050, Side::Sell}, st);
    EXPECT_EQ(b.best_ask_qty(), 150);  // hidden execs leave the visible book alone
    apply(b, {6, MsgType::Delete, 10, 60, 2200000, Side::Buy}, st);
    EXPECT_EQ(b.best_bid(), nullptr);
    apply(b, {7, MsgType::Halt, 0, 0, -1, Side::Buy}, st);
    EXPECT_EQ(st.hidden_execs, 1u); EXPECT_EQ(st.halts, 1u); EXPECT_EQ(st.messages, 7u);
}

TEST(Lobster, UnknownIdGoesToAnon) {
    OrderBook b; ApplyStats st;
    b.add_anon(Side::Sell, 2200100, 500);
    apply(b, {1, MsgType::ExecVisible, 999, 100, 2200100, Side::Sell}, st);
    EXPECT_EQ(st.unknown_id, 1u);
    EXPECT_EQ(b.best_ask_qty(), 400);
    apply(b, {2, MsgType::Delete, 998, 400, 2200100, Side::Sell}, st);
    EXPECT_EQ(b.best_ask(), nullptr);
}

TEST(Replay, FeaturesAndOfi) {
    Cols c;
    c.push(1, 1, 1, 100, 1000, +1);   // bid 1000x100
    c.push(2, 1, 2, 100, 1100, -1);   // ask 1100x100
    c.push(3, 1, 3, 50,  1000, +1);   // bid size 150 -> e = +50
    c.push(4, 4, 2, 30,  1100, -1);   // ask size 70  -> e = +30 (ask depletion is bullish)
    c.push(5, 1, 4, 20,  1050, -1);   // new best ask 1050 -> e = -20 (qa1) since pa1 < pa0
    ReplayOptions o; o.depth_k = 2;
    auto r = replay(c.view(), {}, o);
    EXPECT_EQ(r.best_bid[1], 1000); EXPECT_EQ(r.best_ask[1], 1100);
    EXPECT_EQ(r.ofi_e[2], 50);
    EXPECT_EQ(r.ofi_e[3], 30);
    EXPECT_EQ(r.ofi_e[4], -20);
    EXPECT_EQ(r.trade_qty[3], 30); EXPECT_EQ(r.trade_side[3], -1);
    // snapshot after last message: asks 1050x20, 1100x70 ; bids 1000x150, empty
    EXPECT_EQ(r.ask_px[4 * 2 + 0], 1050); EXPECT_EQ(r.ask_sz[4 * 2 + 1], 70);
    EXPECT_EQ(r.bid_px[4 * 2 + 0], 1000); EXPECT_EQ(r.bid_sz[4 * 2 + 0], 150);
    EXPECT_EQ(r.bid_px[4 * 2 + 1], kNoBid); EXPECT_EQ(r.bid_sz[4 * 2 + 1], 0);
}

TEST(Replay, SeedAndStart) {
    Cols c;
    c.push(1, 1, 1, 100, 1000, +1);   // already in seed; skipped via start=1
    c.push(2, 3, 1, 100, 1000, +1);   // delete unknown id -> anon reduce
    std::vector<SeedLevel> seed{{Side::Buy, 1000, 100}, {Side::Sell, 1100, 40}};
    ReplayOptions o; o.start = 1;
    auto r = replay(c.view(), seed, o);
    EXPECT_EQ(r.best_bid[0], 1000);      // seeded state echoed for skipped rows
    EXPECT_EQ(r.best_bid[1], kNoBid);    // gone after the delete
    EXPECT_EQ(r.best_ask[1], 1100);
    EXPECT_EQ(r.apply_stats.unknown_id, 1u);
}

TEST(Lobster, PricePriorityPurgeRemovesPhantoms) {
    OrderBook b; ApplyStats st;
    b.add_anon(Side::Sell, 1000, 100);          // stale phantom best ask
    b.add(1, Side::Sell, 1200, 50, 0);
    // A visible trade on the ask at 1200 proves nothing visible rests at 1000.
    apply(b, {1, MsgType::ExecVisible, 1, 10, 1200, Side::Sell}, st);
    EXPECT_EQ(b.best_ask_price(), 1200); EXPECT_EQ(b.best_ask_qty(), 40);
    EXPECT_EQ(st.priority_purges, 1u);
    // A new bid resting at 1250 proves no ask <= 1250 exists.
    apply(b, {2, MsgType::NewLimit, 7, 10, 1250, Side::Buy}, st);
    EXPECT_EQ(b.best_ask(), nullptr); EXPECT_EQ(b.best_bid_price(), 1250);
    EXPECT_EQ(st.priority_purges, 2u);
    // Disabled: the phantom survives.
    OrderBook c; ApplyStats st2;
    c.add_anon(Side::Sell, 1000, 100); c.add(1, Side::Sell, 1200, 50, 0);
    apply(c, {1, MsgType::ExecVisible, 1, 10, 1200, Side::Sell}, st2, ApplyOptions{false});
    EXPECT_EQ(c.best_ask_price(), 1000);
}

TEST(Lobster, SnapshotResync) {
    OrderBook b; ResyncStats st;
    b.add(1, Side::Sell, 1000, 100, 0);   // will be missing from the snapshot -> purged
    b.add(2, Side::Sell, 1100, 100, 0);   // snapshot says 60 -> trimmed
    b.add(3, Side::Buy,  900, 100, 0);    // matches
    // snapshot (k = 2): asks 1100x60, 1200x30 ; bids 900x100, 800x10
    std::int64_t ask_px[] = {1100, 1200}, bid_px[] = {900, 800};
    std::int32_t ask_sz[] = {60, 30},     bid_sz[] = {100, 10};
    SnapshotColumns snap{ask_px, ask_sz, bid_px, bid_sz, 2};
    resync(b, snap, 0, st);
    EXPECT_EQ(b.best_ask_price(), 1100); EXPECT_EQ(b.best_ask_qty(), 60);
    EXPECT_EQ(b.level_qty(Side::Sell, 1200), 30);
    EXPECT_EQ(b.level_qty(Side::Buy, 800), 10);
    EXPECT_EQ(b.find(1), nullptr);
    EXPECT_EQ(b.find(2)->qty, 60);
    EXPECT_EQ(st.levels_purged, 1u); EXPECT_EQ(st.levels_added, 2u); EXPECT_EQ(st.qty_adjusted, 1u);
    EXPECT_TRUE(b.check_invariants());
    // Levels beyond the window (worse than the snapshot's last level) are untouched.
    b.add(9, Side::Buy, 700, 5, 0);
    resync(b, snap, 0, st);
    EXPECT_EQ(b.level_qty(Side::Buy, 700), 5);
}
