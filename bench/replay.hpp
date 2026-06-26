// Event stream: LOBSTER message file loader, deterministic synthetic fallback, and the
// mapping from a LOBSTER message to a book operation.
//
// Real market data drives the study. A LOBSTER message file is parsed once into a
// contiguous pre allocated array, so the replay loop that the harness times iterates
// flat memory and allocates nothing. When no file is supplied the synthetic generator
// produces a deterministic stream seeded with 42, which the harness labels as synthetic
// so a reader never mistakes a fallback run for a measured one.
//
// LOBSTER message columns are time, event type, order id, size, price, direction.
// Event types: 1 new limit order, 2 partial cancellation, 3 full deletion, 4 visible
// execution, 5 hidden execution which never touches the visible book, 7 trading halt.
// Direction is 1 for a buy limit order and minus 1 for a sell. Types 5 and 7 are read
// and then ignored because they do not change the resting book.
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "obls/book_concept.hpp"
#include "obls/types.hpp"

namespace obls {

struct Event {
    int type = 0;
    OrderId id = 0;
    Quantity size = 0;
    Price price = 0;
    int direction = 0;  // 1 buy, minus 1 sell
};

struct StreamStats {
    std::size_t add_count = 0;
    std::size_t peak_live_estimate = 0;
};

// Drives a book from one event. Kept here so the harness, the warm up pass, and the
// equivalence test all map events identically; divergence in this mapping would make
// the variant comparison meaningless.
template <typename B>
    requires Book<B>
inline void apply(B& book, const Event& event) {
    switch (event.type) {
        case 1:
            book.add(event.id, event.direction == 1 ? Side::Buy : Side::Sell, event.price,
                     event.size);
            break;
        case 2:
            book.reduce(event.id, event.size);
            break;
        case 3:
            book.cancel(event.id);
            break;
        case 4:
            book.reduce(event.id, event.size);
            break;
        default:
            break;  // 5 hidden execution and 7 halt leave the resting book unchanged
    }
}

// Upper bounds used to pre size the order index and the contiguous level vectors so the
// measured path never reallocates. peak_live_estimate counts adds minus full deletions,
// which overestimates because it ignores orders that executions retire, and that
// overestimate is the safe direction for a capacity hint.
inline StreamStats analyse(const std::vector<Event>& events) {
    StreamStats stats;
    std::ptrdiff_t live = 0;
    std::ptrdiff_t peak = 0;
    for (const Event& event : events) {
        if (event.type == 1) {
            ++stats.add_count;
            ++live;
            if (live > peak) {
                peak = live;
            }
        } else if (event.type == 3) {
            --live;
        }
    }
    stats.peak_live_estimate = static_cast<std::size_t>(peak < 0 ? 0 : peak);
    return stats;
}

namespace detail {

inline std::string_view next_field(std::string_view& line) {
    const std::size_t comma = line.find(',');
    std::string_view field = line.substr(0, comma);
    line.remove_prefix(comma == std::string_view::npos ? line.size() : comma + 1);
    return field;
}

template <typename T>
inline bool parse_int(std::string_view field, T& out) {
    const char* begin = field.data();
    const char* end = begin + field.size();
    return std::from_chars(begin, end, out).ec == std::errc{};
}

}  // namespace detail

// Returns true on a successful load, false when the file cannot be opened, so callers
// can fall back to synthetic data without an exception on the common case of absent
// sample data.
inline bool load_lobster_messages(const std::string& path, std::vector<Event>& out) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    out.clear();
    // One reservation up front sized to the file's line count keeps the parse to a
    // single allocation and leaves replay itself allocation free.
    {
        std::ifstream counter(path);
        out.reserve(
            static_cast<std::size_t>(std::count(std::istreambuf_iterator<char>(counter),
                                                std::istreambuf_iterator<char>(), '\n')));
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        std::string_view view(line);
        detail::next_field(view);  // time, not needed for replay
        Event event;
        if (!detail::parse_int(detail::next_field(view), event.type)) {
            continue;
        }
        detail::parse_int(detail::next_field(view), event.id);
        detail::parse_int(detail::next_field(view), event.size);
        detail::parse_int(detail::next_field(view), event.price);
        detail::parse_int(detail::next_field(view), event.direction);
        out.push_back(event);
    }
    return true;
}

// Deterministic synthetic stream. The fixed seed of 42 makes every synthetic run
// byte for byte reproducible. The generator keeps a list of live orders so that
// cancellations and executions only ever reference orders that are actually resting,
// which is what keeps the two variants in agreement under replay.
inline std::vector<Event> make_synthetic_stream(std::size_t event_count) {
    std::vector<Event> events;
    events.reserve(event_count);

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> action(0, 99);
    std::uniform_int_distribution<int> depth(1, 32);
    std::uniform_int_distribution<int> sidepick(0, 1);
    std::uniform_int_distribution<Quantity> qty(1, 500);

    struct Live {
        OrderId id;
        Side side;
        Price price;
        Quantity remaining;
    };
    std::vector<Live> live;
    live.reserve(event_count);

    const Price tick = 100;  // one cent in LOBSTER units of dollar times ten thousand
    const Price mid = 100 * 10'000;  // a one hundred dollar instrument
    OrderId next_id = 1;

    for (std::size_t i = 0; i < event_count; ++i) {
        const int roll = action(rng);
        const bool can_touch_live = !live.empty();

        if (roll < 60 || !can_touch_live) {
            const Side side = sidepick(rng) == 0 ? Side::Buy : Side::Sell;
            const Price offset = static_cast<Price>(depth(rng)) * tick;
            const Price price = side == Side::Buy ? mid - offset : mid + offset;
            const Quantity size = qty(rng);
            const OrderId id = next_id++;
            events.push_back(Event{1, id, size, price, side == Side::Buy ? 1 : -1});
            live.push_back(Live{id, side, price, size});
        } else if (roll < 85) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t k = pick(rng);
            Live& target = live[k];
            const Quantity cut =
                target.remaining > 1
                    ? std::uniform_int_distribution<Quantity>(1, target.remaining)(rng)
                    : target.remaining;
            events.push_back(
                Event{4, target.id, cut, target.price, target.side == Side::Buy ? 1 : -1});
            target.remaining -= cut;
            if (target.remaining <= 0) {
                target = live.back();
                live.pop_back();
            }
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t k = pick(rng);
            const Live target = live[k];
            events.push_back(Event{3, target.id, target.remaining, target.price,
                                   target.side == Side::Buy ? 1 : -1});
            live[k] = live.back();
            live.pop_back();
        }
    }
    return events;
}

}  // namespace obls
