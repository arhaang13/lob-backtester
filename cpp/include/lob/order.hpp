// order.hpp - the resting order, which doubles as an intrusive list node.
//
// WHY INTRUSIVE?  A std::list<Order> would allocate a separate node per order
// and require you to hold an iterator to erase in O(1). By embedding prev/next
// *inside* the Order, the Order pointer we already keep in the order-ID hash
// index is enough to unlink it from its price-level queue in O(1), with zero
// extra allocations and one fewer pointer chase (better cache behaviour).
// The back-pointer to the PriceLevel means cancel/execute never need to search
// the price map at all: hash lookup -> Order* -> level -> unlink.
#pragma once
#include "lob/types.hpp"

namespace lob {

struct PriceLevel;  // forward declaration; defined in price_level.hpp

struct Order {
    OrderId     id{0};
    Price       price{0};
    Qty         qty{0};        // remaining visible quantity
    Side        side{Side::Buy};
    Ts          ts{0};         // time the order entered the book

    // Intrusive doubly-linked list links within the price level's FIFO queue.
    Order*      prev{nullptr};
    Order*      next{nullptr};
    PriceLevel* level{nullptr};
};

}  // namespace lob
