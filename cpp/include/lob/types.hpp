// types.hpp - fundamental scalar types shared by the whole engine.
//
// DESIGN NOTE: everything in the book is an integer.
//   * Prices follow the LOBSTER convention: dollars * 10,000, so $220.13 == 2201300.
//     One US equity tick ($0.01) is therefore 100 units. Integer prices make level
//     lookups exact (no 0.1 + 0.2 != 0.3 surprises) and let us compare/sort cheaply.
//   * Timestamps are nanoseconds since midnight (LOBSTER gives seconds with up to
//     9 decimals; 34200.000000001 -> 34200000000001). int64 covers ~292 years of ns,
//     so a trading day is trivially in range.
//   * Quantities are int32: US equity order sizes are far below 2^31 shares.
#pragma once
#include <cstdint>
#include <limits>

namespace lob {

using Price   = std::int64_t;  // dollars * 10,000
using Qty     = std::int32_t;  // shares
using OrderId = std::int64_t;  // exchange-assigned order reference number
using Ts      = std::int64_t;  // nanoseconds since midnight

constexpr Price kPriceScale = 10'000;     // units per dollar
constexpr Price kTick       = 100;        // $0.01 in price units
constexpr Ts    kNsPerSec   = 1'000'000'000LL;

// LOBSTER uses these sentinels in the orderbook file for "no level here".
// We reuse them so reconstructed snapshots compare byte-for-byte with the file.
constexpr Price kNoAsk =  9'999'999'999LL;
constexpr Price kNoBid = -9'999'999'999LL;

// Side matches LOBSTER's "direction" column: +1 = buy (bid), -1 = sell (ask).
enum class Side : std::int8_t { Buy = 1, Sell = -1 };

inline constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}
inline constexpr const char* to_string(Side s) noexcept {
    return s == Side::Buy ? "Buy" : "Sell";
}
inline constexpr int sign(Side s) noexcept { return static_cast<int>(s); }

// "Is price a better (more aggressive) than price b for this side?"
// For bids higher is better; for asks lower is better.
inline constexpr bool better(Side s, Price a, Price b) noexcept {
    return s == Side::Buy ? a > b : a < b;
}

}  // namespace lob
