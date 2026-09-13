// event.hpp - simulated orders, fills, and the latency-delayed action queue.
//
// Simulated orders NEVER enter the real OrderBook: the book is a faithful
// replay of what actually happened. Our orders live beside it, and the fill
// model decides when history *would have* filled them.
#pragma once
#include <cstdint>

#include "lob/types.hpp"

namespace lob {

enum class TimeInForce : std::int8_t { GTC = 0, IOC = 1 };
enum class SimOrderState : std::int8_t { Pending = 0, Live = 1, Filled = 2, Cancelled = 3 };

struct SimOrder {
    std::uint64_t id{0};
    Side          side{Side::Buy};
    Price         price{0};        // limit price; kNoAsk / kNoBid for market orders
    Qty           qty{0};
    Qty           filled{0};
    Qty           queue_ahead{0};  // shares in front of us at our price (passive only)
    Ts            submit_ts{0};    // when the strategy decided
    Ts            arrival_ts{0};   // when the exchange saw it (submit + latency)
    TimeInForce   tif{TimeInForce::GTC};
    SimOrderState state{SimOrderState::Pending};
    bool          is_market{false};

    Qty  remaining() const noexcept { return qty - filled; }
    bool open() const noexcept { return state == SimOrderState::Pending || state == SimOrderState::Live; }
};

struct Fill {
    Ts            ts;
    std::uint64_t order_id;
    Side          side;
    Price         price;
    Qty           qty;
    bool          passive;  // true -> maker rebate, false -> taker fee
    double        fee;      // signed cost in dollars (negative = rebate)
};

enum class ActionKind : std::int8_t { Submit = 0, Cancel = 1, Timer = 2 };

// Heap element: something that reaches the exchange (or fires) at time `at`.
struct Action {
    Ts            at;
    std::uint64_t seq;   // tie-break so equal timestamps stay FIFO
    ActionKind    kind;
    std::uint64_t order_id;
};
struct ActionLater {  // min-heap comparator for std::priority_queue
    bool operator()(const Action& a, const Action& b) const noexcept {
        return a.at != b.at ? a.at > b.at : a.seq > b.seq;
    }
};

}  // namespace lob
