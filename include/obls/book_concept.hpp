// The interface every book variant satisfies.
//
// Expressing it as a concept lets the benchmark harness and the equivalence test be
// written once and instantiated over each variant, which guarantees that every variant
// is driven through exactly the same call sites. The independent variable in the study
// is the price level container behind this interface and nothing else.
#pragma once

#include <concepts>
#include <cstddef>

#include "obls/types.hpp"

namespace obls {

template <typename B>
concept Book = requires(B book, const B cbook, OrderId id, Side side, Price price,
                        Quantity qty, std::size_t rank) {
    // Mutating operations, driven by the replayed event stream.
    { book.add(id, side, price, qty) } -> std::same_as<void>;
    { book.cancel(id) } -> std::same_as<void>;
    { book.reduce(id, qty) } -> std::same_as<void>;

    // Top of book reads, the quote query whose latency we measure alongside mutation.
    { cbook.best_bid() } -> std::same_as<Level>;
    { cbook.best_ask() } -> std::same_as<Level>;

    // Aggregate read at an arbitrary price, used to assert two variants hold identical
    // state without depending on either one's internal layout.
    { cbook.quantity_at(side, price) } -> std::same_as<Quantity>;

    // Ranked access from the best price outward, rank zero being the best. Used by the
    // LOBSTER validation, which compares the reconstructed book level by level, and by
    // the later depth sweep experiment.
    { cbook.nth_bid(rank) } -> std::same_as<Level>;
    { cbook.nth_ask(rank) } -> std::same_as<Level>;
    { cbook.bid_depth() } -> std::same_as<std::size_t>;
    { cbook.ask_depth() } -> std::same_as<std::size_t>;
};

}  // namespace obls
