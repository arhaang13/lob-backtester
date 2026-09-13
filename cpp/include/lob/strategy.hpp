// strategy.hpp - what a strategy can see and do.
//
// The backtester implements StrategyContext. A strategy only ever talks to the
// exchange through it, which is what makes latency enforceable: every
// submit/cancel is stamped with `now()` and reaches the book later.
#pragma once
#include <cstdint>
#include <vector>

#include "lob/event.hpp"
#include "lob/lobster.hpp"
#include "lob/order_book.hpp"

namespace lob {

class StrategyContext {
public:
    virtual ~StrategyContext() = default;
    virtual Ts               now() const = 0;
    virtual const OrderBook& book() const = 0;
    virtual Qty              position() const = 0;
    virtual double           cash() const = 0;
    virtual bool             closing() const = 0;   // end-of-day flatten in progress; new orders rejected

    // Returns the simulated order id (0 if rejected).
    virtual std::uint64_t submit_limit(Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GTC) = 0;
    virtual std::uint64_t submit_market(Side side, Qty qty) = 0;
    virtual void          cancel(std::uint64_t order_id) = 0;
    virtual void          schedule_timer(Ts at) = 0;

    virtual const SimOrder*             order(std::uint64_t id) const = 0;
    virtual std::vector<const SimOrder*> open_orders() const = 0;
    virtual Qty                          open_qty(Side side) const = 0;  // remaining on open orders
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_start(StrategyContext&) {}
    // `ofi_e` is the per-event OFI term computed by the engine for this message.
    virtual void on_market(StrategyContext&, const Message&, std::int64_t ofi_e) = 0;
    virtual void on_fill(StrategyContext&, const Fill&) {}
    virtual void on_timer(StrategyContext&, Ts) {}
    virtual void on_end(StrategyContext&) {}
};

}  // namespace lob
