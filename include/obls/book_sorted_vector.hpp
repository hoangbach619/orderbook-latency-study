// Variant C, the sorted contiguous vector with binary search.
//
// The layout is identical to variant B in book_linear.hpp: the same contiguous vector of
// levels per side, the same per side sort order, the same best price at the back, and the
// same shifting of elements on insert and remove. The single deliberate difference is the
// lookup. Variant B scans linearly from the hot end; variant C locates a level with
// std::lower_bound, a binary search.
//
// This is the hypothesis the whole study turns on. Binary search is O(log n) against the
// linear scan's O(n), so on paper it should pull ahead as the book deepens. The
// expectation under test is the opposite at realistic book depth: binary search jumps
// around the array, so each probe lands on a fresh cache line the hardware prefetcher did
// not see coming, and its comparisons are data dependent branches the predictor cannot
// learn, so it stalls. The linear scan from the hot end stays within a couple of cache
// lines and runs a loop the predictor learns cleanly. Which effect wins, and at what depth
// the asymptotics finally take over, is decided by the benchmark, not asserted here.
//
// The vectors are reserved once at construction so that growth never reallocates on the
// measured path, exactly as in variant B, so that the lookup method is the only thing that
// differs between the two.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "obls/book_concept.hpp"
#include "obls/order_index.hpp"
#include "obls/types.hpp"

namespace obls {

class BookSortedVector {
public:
    BookSortedVector(std::size_t order_capacity_hint, std::size_t level_capacity_hint)
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

    // Binary search for the level. lower_bound returns the first element not ordered
    // before the key, which doubles as the answer for both callers: it is the matching
    // level when one exists and the insertion position when one does not, so add keeps the
    // layout sorted without a second search. The comparator flips with the side ordering,
    // since bids ascend towards the back and asks descend towards it, and the range must
    // be searched with the same order it is stored in.
    static Located locate(const std::vector<Level>& book, Price price, bool ascending) {
        // The comparator is chosen by side rather than stored, because the two sides sort
        // in opposite directions and a stored comparator would mean type erasure on a
        // measured path. lower_bound is then called inline so no indirection survives.
        const auto it = ascending ? std::lower_bound(book.begin(), book.end(), price,
                                                     [](const Level& level, Price key) {
                                                         return level.price < key;
                                                     })
                                  : std::lower_bound(book.begin(), book.end(), price,
                                                     [](const Level& level, Price key) {
                                                         return level.price > key;
                                                     });
        const std::size_t index = static_cast<std::size_t>(it - book.begin());
        if (it != book.end() && it->price == price) {
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

static_assert(Book<BookSortedVector>, "BookSortedVector must satisfy the Book concept");

}  // namespace obls
