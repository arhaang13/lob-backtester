// price_level.hpp - one price on one side of the book: a FIFO queue of orders.
//
// Price-time priority: the exchange fills orders at the same price in the order
// they arrived. So each level is a queue; head is the oldest (first to fill).
//
// anon_qty ("anonymous liquidity"): LOBSTER files start at 9:30 but orders were
// placed earlier (pre-market). Their later cancels/executions reference IDs we
// never saw. We seed each level from the first orderbook snapshot and store that
// quantity here, separate from orders we can identify. visible_qty() is what
// the exchange's aggregated feed would show: identified + anonymous.
#pragma once
#include <cassert>
#include <cstdint>

#include "lob/order.hpp"
#include "lob/types.hpp"

namespace lob {

struct PriceLevel {
    Price         price{0};
    Qty           total_qty{0};   // sum of qty over orders in the queue
    Qty           anon_qty{0};    // liquidity from orders that predate the data
    std::uint32_t num_orders{0};
    Order*        head{nullptr};  // oldest order (front of the queue)
    Order*        tail{nullptr};  // newest order (back of the queue)

    Qty  visible_qty() const noexcept { return total_qty + anon_qty; }
    bool empty() const noexcept { return num_orders == 0 && anon_qty == 0; }

    // O(1): append at the back of the queue (new orders have lowest priority).
    void push_back(Order* o) noexcept {
        assert(o && o->level == nullptr);
        o->level = this;
        o->prev  = tail;
        o->next  = nullptr;
        if (tail) tail->next = o; else head = o;
        tail = o;
        total_qty += o->qty;
        ++num_orders;
    }

    // O(1): unlink an order from anywhere in the queue.
    void erase(Order* o) noexcept {
        assert(o && o->level == this);
        if (o->prev) o->prev->next = o->next; else head = o->next;
        if (o->next) o->next->prev = o->prev; else tail = o->prev;
        total_qty -= o->qty;
        --num_orders;
        o->prev = o->next = nullptr;
        o->level = nullptr;
    }

    // Reduce an order's quantity in place (partial cancel or partial execution).
    void reduce(Order* o, Qty by) noexcept {
        assert(o && o->level == this && by <= o->qty);
        o->qty    -= by;
        total_qty -= by;
    }

    // Shares queued ahead of `o` at this price (anonymous liquidity is older
    // than anything we saw arrive, so it always counts as ahead).
    // O(n_level); only used when a simulated order joins the queue.
    Qty qty_ahead(const Order* o) const noexcept {
        Qty ahead = anon_qty;
        for (const Order* p = head; p && p != o; p = p->next) ahead += p->qty;
        return ahead;
    }
};

}  // namespace lob
