// Variant A, the std::map baseline.
//
// Each side is a red black tree keyed by price. This is the structure a textbook
// reaches for: ordered, logarithmic insertion and lookup, the best price always at
// begin(). It exists in this study to be beaten. Its weakness is not its asymptotic
// complexity but its memory layout. Every price level is a separately allocated node,
// so walking even a few levels chases pointers across the heap and pays a cache miss
// per step, which is exactly what the contiguous variant avoids.
#pragma once

#include <cstddef>
#include <functional>
#include <iterator>
#include <map>

#include "obls/book_concept.hpp"
#include "obls/order_index.hpp"
#include "obls/types.hpp"

namespace obls {

class BookMap {
public:
    explicit BookMap(std::size_t order_capacity_hint) : index_(order_capacity_hint) {}

    void add(OrderId id, Side side, Price price, Quantity qty) {
        index_.insert(Order{id, price, qty, side});
        if (side == Side::Buy) {
            bids_[price] += qty;
        } else {
            asks_[price] += qty;
        }
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
        if (bids_.empty()) {
            return Level{kInvalidPrice, 0};
        }
        return Level{bids_.begin()->first, bids_.begin()->second};
    }

    Level best_ask() const {
        if (asks_.empty()) {
            return Level{kInvalidPrice, 0};
        }
        return Level{asks_.begin()->first, asks_.begin()->second};
    }

    Quantity quantity_at(Side side, Price price) const {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            return it == bids_.end() ? 0 : it->second;
        }
        auto it = asks_.find(price);
        return it == asks_.end() ? 0 : it->second;
    }

    Level nth_bid(std::size_t rank) const { return nth(bids_, rank); }
    Level nth_ask(std::size_t rank) const { return nth(asks_, rank); }

    std::size_t bid_depth() const { return bids_.size(); }
    std::size_t ask_depth() const { return asks_.size(); }

private:
    // Bids descend so begin() is the highest price; asks ascend so begin() is the
    // lowest. Both therefore expose the best price as begin(), which is the only thing
    // the read path needs.
    using BidMap = std::map<Price, Quantity, std::greater<Price>>;
    using AskMap = std::map<Price, Quantity, std::less<Price>>;

    template <typename MapT>
    static Level nth(const MapT& book, std::size_t rank) {
        if (rank >= book.size()) {
            return Level{kInvalidPrice, 0};
        }
        auto it = book.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rank));
        return Level{it->first, it->second};
    }

    void level_sub(Side side, Price price, Quantity amount) {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            if (it == bids_.end()) {
                return;
            }
            it->second -= amount;
            if (it->second <= 0) {
                bids_.erase(it);
            }
        } else {
            auto it = asks_.find(price);
            if (it == asks_.end()) {
                return;
            }
            it->second -= amount;
            if (it->second <= 0) {
                asks_.erase(it);
            }
        }
    }

    BidMap bids_;
    AskMap asks_;
    OrderIndex index_;
};

static_assert(Book<BookMap>, "BookMap must satisfy the Book concept");

}  // namespace obls
