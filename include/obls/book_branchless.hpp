// Variant D, the branchless binary search.
//
// The layout is identical to variant C in book_sorted_vector.hpp: the same contiguous
// vectors, the same per side sort order, the same best price at the back, the same element
// shifting on insert and remove, and the same shared order index. The one permitted
// difference is the lookup. Variant C searches with std::lower_bound, which carries a data
// dependent branch at each step that the predictor cannot learn and so mispredicts.
// Variant D runs the same binary search but replaces that branch with a conditional move,
// so the index update has nothing to mispredict.
//
// This identity is the experimental control. Building D lets the study split variant C's
// cost into its two parts. C against D isolates the branch misprediction component, which D
// removes. D against the linear scan of variant B at realistic depth then shows whether,
// with branches gone, binary search still loses, and if it does the remaining gap is the
// cache cost of probing scattered positions, which D keeps and makes no attempt to hide.
// The expectation under test, which the benchmark must confirm rather than assume, is that
// D beats C yet still loses to B at around a thousand levels, because at that depth the
// dominant cost is the cache misses of the jumping probe pattern, not the branches.
//
// No prefetching is added here on purpose, since it would change two things at once and
// blur the C against D comparison.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obls/book_concept.hpp"
#include "obls/order_index.hpp"
#include "obls/types.hpp"

namespace obls {
namespace detail {

// A one byte base keeps the search direction tag the size of the Side enum it mirrors; it
// is only ever a compile time template argument, so the width is cosmetic, but it silences
// the enum size lint and stays consistent with Side.
enum class Order : std::uint8_t { Ascending, Descending };

// Branchless lower bound over a contiguous run of levels, length halving form. The caller
// guarantees n is at least one. Order selects the per side comparison, the same predicate
// variants B and C use, so every variant agrees on the result position: ascending compares
// probe < key for bids, descending compares probe > key for asks.
//
// Why the per iteration advance is split by target, with the disassembly that justifies it
// under -O3 and native tuning. The advance has to be a conditional move so the data
// dependent comparison is never a branch the predictor misses, which is the whole reason
// variant D exists; if it lowers to a branch, D is just variant C in disguise.
//
//   - On arm64 the arithmetic select on the portable path below lowers to csel under both
//     GCC 14 and Clang, with the only loop branch being the n counter. Verified.
//
//   - On x86 a compiler generated cmov does not survive. LLVM's X86CmovConversion pass
//     rewrites the cmov back into a data dependent branch inside this loop, and it does so
//     for every plain C++ form tried: the mask below, a ternary, a multiply by the 0 or 1
//     result, a pointer select, and an explicit if, with or without
//     __builtin_unpredictable. Disabling that pass restores the cmov, which pins the cause.
//     So under Clang a portable select would silently turn variant D back into variant C on
//     the very target the benchmark measures. The hand written cmov below is the one
//     construction the pass leaves alone; it emits cmovl or cmovg with the n counter as the
//     only loop branch, verified under Clang on x86. GCC on x86 emits a cmov for the
//     portable form without this, but the explicit cmov is correct under both, so it is
//     used for all x86.
//
// The trailing comparison on n is a loop counter branch, not a data dependent one. The
// final adjustment sits outside the loop and lowers to a setcc, not a branch, so a single
// post search comparison does not reintroduce a per step misprediction.
template <Order O>
inline std::size_t branchless_lower_bound(const Level* data, std::size_t n, Price key) {
    const Level* base = data;
    while (n > 1) {
        const std::size_t half = n / 2;
        const Price probe = base[half].price;
        std::size_t advance = 0;
#if defined(__x86_64__)
        // advance starts at zero and the cmov overwrites it with half only when the side's
        // comparison holds, so base steps forward by half or stays put, branchlessly.
        if constexpr (O == Order::Ascending) {
            __asm__(
                "cmpq %[key], %[probe]\n\t"
                "cmovlq %[half], %[advance]"
                : [advance] "+r"(advance)
                : [probe] "r"(probe), [key] "r"(key), [half] "r"(half)
                : "cc");
        } else {
            __asm__(
                "cmpq %[key], %[probe]\n\t"
                "cmovgq %[half], %[advance]"
                : [advance] "+r"(advance)
                : [probe] "r"(probe), [key] "r"(key), [half] "r"(half)
                : "cc");
        }
#else
        // Portable arithmetic select: the comparison becomes an all ones or all zero mask,
        // and masking half with it advances base by half or by nothing, with no select for
        // the compiler to turn back into a branch.
        const bool take = O == Order::Ascending ? (probe < key) : (probe > key);
        advance = half & (std::size_t{0} - static_cast<std::size_t>(take));
#endif
        base += advance;
        n -= half;
    }
    const bool last = O == Order::Ascending ? (base->price < key) : (base->price > key);
    base += static_cast<std::size_t>(last);
    return static_cast<std::size_t>(base - data);
}

}  // namespace detail

class BookBranchless {
public:
    BookBranchless(std::size_t order_capacity_hint, std::size_t level_capacity_hint)
        : index_(order_capacity_hint) {
        bids_.reserve(level_capacity_hint);
        asks_.reserve(level_capacity_hint);
    }

    void add(OrderId id, Side side, Price price, Quantity qty) {
        index_.insert(Order{id, price, qty, side});
        level_add(side, price, qty);
    }

    void cancel(OrderId id) {
        Order* order = index_.find(id);
        if (order == nullptr) {
            return;
        }
        level_sub(order->side, order->price, order->remaining);
        index_.erase(id);
    }

    void reduce(OrderId id, Quantity delta) {
        Order* order = index_.find(id);
        if (order == nullptr) {
            return;
        }
        Quantity applied = delta < order->remaining ? delta : order->remaining;
        level_sub(order->side, order->price, applied);
        order->remaining -= applied;
        if (order->remaining <= 0) {
            index_.erase(id);
        }
    }

    Level best_bid() const {
        return bids_.empty() ? Level{kInvalidPrice, 0} : bids_.back();
    }

    Level best_ask() const {
        return asks_.empty() ? Level{kInvalidPrice, 0} : asks_.back();
    }

    Quantity quantity_at(Side side, Price price) const {
        const std::vector<Level>& book = side == Side::Buy ? bids_ : asks_;
        Located at = locate(book, price, side == Side::Buy);
        return at.found ? book[at.index].quantity : 0;
    }

    Level nth_bid(std::size_t rank) const { return nth(bids_, rank); }
    Level nth_ask(std::size_t rank) const { return nth(asks_, rank); }

    std::size_t bid_depth() const { return bids_.size(); }
    std::size_t ask_depth() const { return asks_.size(); }

private:
    struct Located {
        bool found;
        std::size_t index;  // position when found, else the insertion position
    };

    // Same answer as variant C's std::lower_bound, reached without a branch on the key
    // comparison. The order flips with the side, since bids ascend towards the back and
    // asks descend towards it, so each side is searched in the order it is stored. The
    // empty case is handled before the search so the loop never dereferences a null run.
    static Located locate(const std::vector<Level>& book, Price price, bool ascending) {
        if (book.empty()) {
            return Located{false, 0};
        }
        const std::size_t index =
            ascending ? detail::branchless_lower_bound<detail::Order::Ascending>(
                            book.data(), book.size(), price)
                      : detail::branchless_lower_bound<detail::Order::Descending>(
                            book.data(), book.size(), price);
        if (index < book.size() && book[index].price == price) {
            return Located{true, index};
        }
        return Located{false, index};
    }

    static Level nth(const std::vector<Level>& book, std::size_t rank) {
        if (rank >= book.size()) {
            return Level{kInvalidPrice, 0};
        }
        // Rank zero is the best price, which lives at the back.
        return book[book.size() - 1 - rank];
    }

    void level_add(Side side, Price price, Quantity qty) {
        std::vector<Level>& book = side == Side::Buy ? bids_ : asks_;
        Located at = locate(book, price, side == Side::Buy);
        if (at.found) {
            book[at.index].quantity += qty;
        } else {
            book.insert(book.begin() + static_cast<std::ptrdiff_t>(at.index),
                        Level{price, qty});
        }
    }

    void level_sub(Side side, Price price, Quantity amount) {
        std::vector<Level>& book = side == Side::Buy ? bids_ : asks_;
        Located at = locate(book, price, side == Side::Buy);
        if (!at.found) {
            return;
        }
        book[at.index].quantity -= amount;
        if (book[at.index].quantity <= 0) {
            book.erase(book.begin() + static_cast<std::ptrdiff_t>(at.index));
        }
    }

    std::vector<Level> bids_;  // ascending, best bid at the back
    std::vector<Level> asks_;  // descending, best ask at the back
    OrderIndex index_;
};

static_assert(Book<BookBranchless>, "BookBranchless must satisfy the Book concept");

}  // namespace obls
