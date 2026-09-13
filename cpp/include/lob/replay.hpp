// replay.hpp - drive the book through a whole day and emit per-event features.
//
// This is the function Python calls: it receives raw column pointers (numpy
// arrays, zero-copy) and returns dense arrays, so the per-message loop never
// runs in Python.
//
// OFI (Cont, Kukanov & Stoikov 2014). Let (Pb, qb) and (Pa, qa) be the best bid
// and ask price/size before (n-1) and after (n) an event. The per-event
// order-flow-imbalance contribution is
//   e_n =  1[Pb_n >= Pb_{n-1}] qb_n  - 1[Pb_n <= Pb_{n-1}] qb_{n-1}
//        - 1[Pa_n <= Pa_{n-1}] qa_n  + 1[Pa_n >= Pa_{n-1}] qa_{n-1}
// Intuition: bid-side demand rises when size is added at (or above) the old
// best bid and falls when it is removed; symmetrically for the ask. Summed over
// a window, OFI predicts the contemporaneous mid-price change linearly.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/lobster.hpp"
#include "lob/order_book.hpp"

namespace lob {

// Column-oriented view of the message file (same dtypes as the Parquet schema).
struct MessageColumns {
    const std::int64_t* ts;     // ns since midnight
    const std::int8_t*  type;
    const std::int64_t* id;
    const std::int32_t* size;
    const std::int64_t* price;
    const std::int8_t*  dir;    // +1 / -1
    std::size_t         n;

    Message at(std::size_t i) const noexcept {
        return Message{ts[i], static_cast<MsgType>(type[i]), id[i], size[i], price[i],
                       dir[i] >= 0 ? Side::Buy : Side::Sell};
    }
};

// Liquidity to pre-load before replay (from orderbook-file row `start-1`).
struct SeedLevel { Side side; Price price; Qty qty; };

struct ReplayOptions {
    std::size_t start   = 0;   // first message index to apply
    std::size_t depth_k = 0;   // if > 0, also emit top-k snapshots after every message
    bool price_priority_purge = true;   // message-only phantom removal (see lobster.hpp)
    SnapshotColumns snapshot{};         // if valid, resync with the orderbook file after every message
};

struct ReplayResult {
    // one entry per message index in [0, n): rows before `start` are copies of the seed state
    std::vector<std::int64_t> best_bid, best_ask;
    std::vector<std::int32_t> bid_qty, ask_qty;
    std::vector<std::int64_t> ofi_e;         // per-event OFI term
    std::vector<std::int32_t> trade_qty;     // executed shares (types 4 and 5), else 0
    std::vector<std::int8_t>  trade_side;    // side of the resting order hit, else 0
    // flattened (n x depth_k) snapshots in LOBSTER orderbook-file order, only if depth_k > 0
    std::vector<std::int64_t> ask_px, bid_px;
    std::vector<std::int32_t> ask_sz, bid_sz;
    std::size_t depth_k{0};
    ApplyStats  apply_stats;
    BookStats   book_stats;
    ResyncStats resync_stats;
};

// Compute the CKS per-event OFI term from before/after L1 states.
inline std::int64_t ofi_term(Price pb0, Qty qb0, Price pa0, Qty qa0,
                             Price pb1, Qty qb1, Price pa1, Qty qa1) noexcept {
    std::int64_t e = 0;
    if (pb1 >= pb0) e += qb1;
    if (pb1 <= pb0) e -= qb0;
    if (pa1 <= pa0) e -= qa1;
    if (pa1 >= pa0) e += qa0;
    return e;
}

ReplayResult replay(const MessageColumns& cols, const std::vector<SeedLevel>& seed,
                    const ReplayOptions& opt);

}  // namespace lob
