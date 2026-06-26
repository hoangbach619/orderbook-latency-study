// Variant B, the contiguous vector with linear scan.
//
// Each side is a single std::vector of price levels held in sorted order, with the best
// price at the back. New activity at the top of book is then a push_back or a pop_back,
// the cheap end of a vector, and a lookup scans backward from that same hot end.
//
// Cache behaviour is the entire point. Four levels share a 64 byte cache line, so a
// scan over the top of book touches one or two lines that the hardware prefetcher has
// usually already streamed in. There is no pointer chasing and no per node allocation,
// unlike the map variant where each level is a separate heap node a cache line miss
// away from its neighbour. At realistic book depths the resting interest is shallow and
// most lookups resolve within the first handful of levels, so the linear scan finishes
// in a few cache friendly steps and the branch predictor learns the short loop. The
// crossover where this stops winning, once the scan grows long enough that its work
// outweighs the map's better asymptotics, is the subject of the later depth sweep.
//
// The vectors are reserved once at construction so that growth never reallocates on the
// measured path; a reallocation would otherwise surface as a latency outlier that has
// nothing to do with the structure under study.
#pragma once

#include <cstddef>
#include <vector>

#include "obls/book_concept.hpp"
#include "obls/order_index.hpp"
#include "obls/types.hpp"

namespace obls {

class BookLinear {
public:
    BookLinear(std::size_t order_capacity_hint, std::size_t level_capacity_hint)
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

    // Scans from the back, which holds the best price. ascending is true for bids, whose
    // prices increase towards the back, and false for asks, whose prices decrease
    // towards the back. Returning the insertion position lets add keep the layout sorted
    // without a second search.
    static Located locate(const std::vector<Level>& book, Price price, bool ascending) {
        std::ptrdiff_t i = static_cast<std::ptrdiff_t>(book.size()) - 1;
        if (ascending) {
            while (i >= 0 && book[static_cast<std::size_t>(i)].price > price) {
                --i;
            }
        } else {
            while (i >= 0 && book[static_cast<std::size_t>(i)].price < price) {
                --i;
            }
        }
        if (i >= 0 && book[static_cast<std::size_t>(i)].price == price) {
            return Located{true, static_cast<std::size_t>(i)};
        }
        return Located{false, static_cast<std::size_t>(i + 1)};
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

static_assert(Book<BookLinear>, "BookLinear must satisfy the Book concept");

}  // namespace obls
