// lobster.hpp - LOBSTER message format and how each message mutates the book.
//
// Message file row: time, type, order_id, size, price, direction
//   time      seconds after midnight, up to 9 decimals   -> Ts (ns)
//   type      1..7 (see MsgType)
//   order_id  exchange order reference (0 for types 6/7)
//   size      shares
//   price     dollars * 10,000
//   direction +1 buy order, -1 sell order. For executions (4/5) this is the
//             side of the RESTING order, so the aggressor is on the other side.
#pragma once
#include <cstddef>
#include <cstdint>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

enum class MsgType : std::int8_t {
    NewLimit      = 1,  // new visible limit order            -> book.add
    PartialCancel = 2,  // partial cancellation               -> book.cancel
    Delete        = 3,  // full deletion                      -> book.remove
    ExecVisible   = 4,  // execution of a visible limit order -> book.execute
    ExecHidden    = 5,  // execution of a hidden order        -> no book change
    Cross         = 6,  // cross / auction trade              -> no book change
    Halt          = 7,  // trading halt indicator             -> no book change
};

struct Message {
    Ts      ts;
    MsgType type;
    OrderId id;
    Qty     size;
    Price   price;
    Side    side;
};

// LEVEL-k DATA WINDOW. A LOBSTER "level 10" message file only contains events
// that touch the top 10 levels. A level pushed to level 11 can be modified or
// deleted with no message in the file; when it re-enters the top 10 our copy
// is stale. Two remedies:
//   1. Price-priority inference (message-only, always sound): a visible trade
//      at price p on side s proves nothing visible rests at a better price on
//      s; a new order resting at p proves nothing on the opposite side is
//      priced through it. Any such level in our book is a phantom -> purge.
//   2. Snapshot resync: reconcile our top-k with the orderbook file's row at
//      the window boundary (levels entering/leaving). This is exact by
//      construction inside the window and does not use future information.
struct ApplyOptions {
    bool price_priority_purge = true;
};

struct ApplyStats {
    std::uint64_t messages{0};
    std::uint64_t unknown_id{0};      // cancel/delete/exec on an id we never saw
    std::uint64_t hidden_execs{0};
    std::uint64_t crosses{0};
    std::uint64_t halts{0};
    std::uint64_t ignored{0};         // malformed rows
    std::uint64_t priority_purges{0}; // levels removed by price-priority inference
};

// Apply one message to the book. Unknown ids on types 2/3/4 are routed to the
// anonymous liquidity at (side, price): that is exactly the liquidity that was
// already resting when the file began. Returns true if the book changed.
bool apply(OrderBook& book, const Message& m, ApplyStats& st, const ApplyOptions& opt = ApplyOptions{});

// Row-major (n x k) top-k snapshots in LOBSTER orderbook-file layout.
struct SnapshotColumns {
    const std::int64_t* ask_px{nullptr};
    const std::int32_t* ask_sz{nullptr};
    const std::int64_t* bid_px{nullptr};
    const std::int32_t* bid_sz{nullptr};
    std::size_t k{0};
    bool valid() const noexcept { return ask_px && ask_sz && bid_px && bid_sz && k > 0; }
};

struct ResyncStats {
    std::uint64_t rows{0};
    std::uint64_t levels_added{0};    // snapshot had a level we lacked (entered the window)
    std::uint64_t levels_purged{0};   // we had a level inside the window the snapshot lacks
    std::uint64_t qty_adjusted{0};    // same price, different visible qty
    std::uint64_t orders_trimmed{0};  // identified orders dropped/reduced during resync
};

// Reconcile the book's top-k with snapshot row `row`.
void resync(OrderBook& book, const SnapshotColumns& snap, std::size_t row, ResyncStats& st);

}  // namespace lob
