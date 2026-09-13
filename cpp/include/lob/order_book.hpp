// order_book.hpp - the L3 limit order book.
//
// L1 = best bid/ask only. L2 = aggregated size per price level.
// L3 = every individual order with its queue position. LOBSTER message files
// are L3 (each row names an order id), which is what lets us know *where in
// the queue* a simulated order would sit.
//
// Data structure summary (the resume bullet):
//   * price-level map            : BidSide/AskSide, std::map<Price, PriceLevel>
//   * per-level intrusive lists  : PriceLevel{head,tail}, Order{prev,next}
//   * order-ID hash index        : std::unordered_map<OrderId, Order*>
// Complexity:
//   add      O(log L) to find/create the level, O(1) for everything else
//   cancel   O(1)  hash -> Order* -> level (no map lookup)
//   execute  O(1)  same path as cancel
//   remove   O(1)  + O(log L) only when the level becomes empty and is erased
//   best     O(1)  map.begin()
#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "lob/book_side.hpp"
#include "lob/order.hpp"
#include "lob/order_pool.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Counters instead of exceptions in the hot path: data quirks are expected and
// we want to keep going and report them, not abort a replay.
struct BookStats {
    std::uint64_t adds{0};
    std::uint64_t cancels{0};      // partial cancels applied to identified orders
    std::uint64_t removes{0};      // full deletes of identified orders
    std::uint64_t executes{0};     // executions against identified orders
    std::uint64_t anon_reduce{0};  // events routed to anonymous liquidity
    std::uint64_t duplicate_ids{0};
    std::uint64_t over_qty{0};     // cancel/execute larger than the resting qty
    std::uint64_t anon_underflow{0};  // anon reduction larger than anon qty
    std::uint64_t purged_orders{0};   // identified orders dropped by window maintenance
};

class OrderBook {
public:
    OrderBook() { index_.reserve(1 << 20); }

    // ---- identified orders ------------------------------------------------
    // Returns the new order or nullptr if the id already exists.
    Order* add(OrderId id, Side side, Price price, Qty qty, Ts ts);

    // Partial cancel. Returns false if the id is unknown (caller may then
    // route the event to anonymous liquidity). If qty >= resting qty the order
    // is removed and over_qty is bumped.
    bool cancel(OrderId id, Qty qty);

    // Full delete. Returns false if unknown.
    bool remove(OrderId id);

    // Execution against a resting identified order. Same shape as cancel but
    // counted separately (trades feed the fill model and trade statistics).
    bool execute(OrderId id, Qty qty);

    // ---- anonymous liquidity (orders that predate the data) -----------------
    void add_anon(Side side, Price price, Qty qty);
    // Returns actually removed amount (clamped at the available anon qty).
    Qty  reduce_anon(Side side, Price price, Qty qty);

    // ---- window-boundary maintenance (level-k data, see lobster.hpp) --------
    // Remove every level on `side` strictly better than `limit` (identified
    // orders there are dropped from the index). Returns levels removed.
    std::size_t purge_better_than(Side side, Price limit);
    // Remove one level entirely. Returns orders removed (0 if no such level).
    std::size_t purge_level(Side side, Price price);
    // Force the visible qty at (side, price) to `target` by adjusting anon
    // qty; if identified qty alone exceeds target, trims the youngest orders.
    // Returns the number of identified orders trimmed or removed.
    std::size_t set_level_visible(Side side, Price price, Qty target);

    // ---- queries ------------------------------------------------------------
    Order*       find(OrderId id) noexcept;
    const Order* find(OrderId id) const noexcept;

    const PriceLevel* best_bid() const noexcept { return bids_.best(); }
    const PriceLevel* best_ask() const noexcept { return asks_.best(); }
    Price best_bid_price() const noexcept { auto* b = bids_.best(); return b ? b->price : kNoBid; }
    Price best_ask_price() const noexcept { auto* a = asks_.best(); return a ? a->price : kNoAsk; }
    Qty   best_bid_qty()   const noexcept { auto* b = bids_.best(); return b ? b->visible_qty() : 0; }
    Qty   best_ask_qty()   const noexcept { auto* a = asks_.best(); return a ? a->visible_qty() : 0; }
    bool  has_both_sides() const noexcept { return bids_.best() && asks_.best(); }

    // Mid in *half* price units to stay integral: (bid + ask). Divide by 2 in doubles.
    Price mid2() const noexcept { return best_bid_price() + best_ask_price(); }
    Price spread() const noexcept { return best_ask_price() - best_bid_price(); }

    const PriceLevel* level(Side side, Price price) const noexcept {
        return side == Side::Buy ? bids_.find(price) : asks_.find(price);
    }
    Qty level_qty(Side side, Price price) const noexcept {
        auto* l = level(side, price); return l ? l->visible_qty() : 0;
    }

    std::vector<LevelView> depth(Side side, std::size_t k) const {
        return side == Side::Buy ? bids_.depth(k) : asks_.depth(k);
    }

    const BidSide& bids() const noexcept { return bids_; }
    const AskSide& asks() const noexcept { return asks_; }

    std::size_t num_orders() const noexcept { return index_.size(); }
    const BookStats& stats() const noexcept { return stats_; }

    // Debug-only structural check; returns true if all invariants hold.
    bool check_invariants() const;

private:
    // Shared body of cancel/execute.
    bool reduce_identified(OrderId id, Qty qty, bool is_exec);
    void erase_order(Order* o);
    template <class SideT> void erase_level_if_empty(SideT& side, PriceLevel* lvl) {
        side.erase_if_empty(lvl);
    }

    BidSide bids_;
    AskSide asks_;
    std::unordered_map<OrderId, Order*> index_;
    OrderPool pool_;
    BookStats stats_;
};

}  // namespace lob
