// Fundamental value types for the order book latency study.
//
// Everything here is an integer. LOBSTER prices arrive as integers in units of
// dollar times ten thousand, so an integer tick keeps comparisons exact and avoids
// the rounding hazards a matching path inherits from floating point.
#pragma once

#include <cstdint>
#include <limits>

namespace obls {

// Signed because intermediate quantities derived from a price, such as a spread or a
// depth offset, can legitimately be negative even though a quoted price is not.
using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::int64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

// Sentinel returned by best_bid and best_ask when the relevant side is empty. A level
// is treated as absent when its quantity is zero, so callers never need to special
// case the price field.
inline constexpr Price kInvalidPrice = std::numeric_limits<Price>::min();

// 32 bytes, two orders to a 64 byte cache line. This struct is the value record stored
// in the shared order index, so id leads: the index probes on id and the first eight
// bytes of the line therefore carry the comparison key. side sits last in a full byte
// rather than a bitfield because the record stays trivially copyable, which lets the
// index live in one pre allocated slab that we move with plain byte copies.
struct Order {
    OrderId id;
    Price price;
    Quantity remaining;
    Side side;
};

// 16 bytes, four levels to a 64 byte cache line. The level is the unit the contiguous
// variant walks during a linear scan, and this density is the whole reason that scan
// can win: one cache miss brings in four price levels, so a scan over the top of book
// touches very little memory. Keeping the level free of an order list, holding only the
// aggregate quantity, is what preserves the density.
struct Level {
    Price price;
    Quantity quantity;
};

}  // namespace obls
