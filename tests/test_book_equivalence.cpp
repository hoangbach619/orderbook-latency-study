// The property that makes the latency comparison fair: every variant holds identical
// observable state after the same event stream. If this fails, any latency difference
// is between two different books and means nothing.
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "bench/replay.hpp"
#include "obls/book_linear.hpp"
#include "obls/book_map.hpp"

namespace {

// Compares the full ladder on both sides. Returns true only when prices and aggregate
// quantities match rank for rank, which is the strongest observable equality available
// without reaching into either variant's private layout.
bool same_ladder(const obls::BookMap& a, const obls::BookLinear& b) {
    if (a.bid_depth() != b.bid_depth() || a.ask_depth() != b.ask_depth()) {
        return false;
    }
    for (std::size_t r = 0; r < a.bid_depth(); ++r) {
        const obls::Level la = a.nth_bid(r);
        const obls::Level lb = b.nth_bid(r);
        if (la.price != lb.price || la.quantity != lb.quantity) {
            return false;
        }
    }
    for (std::size_t r = 0; r < a.ask_depth(); ++r) {
        const obls::Level la = a.nth_ask(r);
        const obls::Level lb = b.nth_ask(r);
        if (la.price != lb.price || la.quantity != lb.quantity) {
            return false;
        }
    }
    return true;
}

bool same_top(const obls::BookMap& a, const obls::BookLinear& b) {
    const obls::Level ab = a.best_bid();
    const obls::Level bb = b.best_bid();
    const obls::Level aa = a.best_ask();
    const obls::Level ba = b.best_ask();
    return ab.price == bb.price && ab.quantity == bb.quantity && aa.price == ba.price &&
           aa.quantity == ba.quantity;
}

}  // namespace

TEST_CASE("map and linear variants agree step by step") {
    const std::vector<obls::Event> events = obls::make_synthetic_stream(40000);
    const obls::StreamStats stats = obls::analyse(events);

    obls::BookMap map_book(stats.peak_live_estimate + 1024);
    obls::BookLinear linear_book(stats.peak_live_estimate + 1024, stats.add_count + 1024);

    std::size_t step = 0;
    for (const obls::Event& event : events) {
        obls::apply(map_book, event);
        obls::apply(linear_book, event);

        // Top of book is checked every step because it is the cheapest strong signal and
        // the quantity most sensitive to an ordering bug.
        REQUIRE(same_top(map_book, linear_book));

        // The full ladder is checked periodically to keep the assertion count tractable
        // while still catching a divergence deep in the book.
        if (step % 250 == 0) {
            REQUIRE(same_ladder(map_book, linear_book));
        }
        ++step;
    }

    REQUIRE(same_ladder(map_book, linear_book));
    REQUIRE(map_book.bid_depth() == linear_book.bid_depth());
    REQUIRE(map_book.ask_depth() == linear_book.ask_depth());
}
