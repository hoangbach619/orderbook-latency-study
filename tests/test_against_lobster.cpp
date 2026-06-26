// Correctness against an external reference.
//
// Most order book repositories validate a book only against themselves. This one
// reconstructs the book from a LOBSTER message file and checks the top levels against
// the matching LOBSTER orderbook file, level by level, after every message. That is the
// thing that proves the reconstruction is actually right rather than merely
// self consistent.
//
// The test skips cleanly when the sample data is absent, so continuous integration with
// no data still passes. To run it, place a LOBSTER message and orderbook pair in the
// data directory as described in data/README.md.
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "bench/replay.hpp"
#include "obls/book_linear.hpp"

#ifndef OBLS_DATA_DIR
#define OBLS_DATA_DIR "data"
#endif

namespace {

namespace fs = std::filesystem;

struct SamplePair {
    bool found = false;
    fs::path message;
    fs::path orderbook;
};

// Finds the first message file in the data directory and its matching orderbook file.
// LOBSTER names them identically apart from the message versus orderbook token.
SamplePair find_sample() {
    SamplePair pair;
    const fs::path dir(OBLS_DATA_DIR);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return pair;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.find("_message_") == std::string::npos ||
            entry.path().extension() != ".csv") {
            continue;
        }
        std::string ob_name = name;
        const std::size_t pos = ob_name.find("_message_");
        ob_name.replace(pos, std::string("_message_").size(), "_orderbook_");
        const fs::path ob = dir / ob_name;
        if (fs::exists(ob)) {
            pair.found = true;
            pair.message = entry.path();
            pair.orderbook = ob;
            return pair;
        }
    }
    return pair;
}

std::int64_t to_int(std::string_view field) {
    std::int64_t value = 0;
    std::from_chars(field.data(), field.data() + field.size(), value);
    return value;
}

std::vector<std::int64_t> parse_row(const std::string& line) {
    std::vector<std::int64_t> fields;
    std::string_view view(line);
    while (!view.empty()) {
        const std::size_t comma = view.find(',');
        const std::string_view field = view.substr(0, comma);
        if (!field.empty()) {
            fields.push_back(to_int(field));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        view.remove_prefix(comma + 1);
    }
    return fields;
}

}  // namespace

TEST_CASE("reconstructed book matches the LOBSTER orderbook file") {
    const SamplePair sample = find_sample();
    if (!sample.found) {
        SKIP("no LOBSTER sample present in " OBLS_DATA_DIR
             "; see data/README.md to enable this test");
    }

    std::ifstream messages(sample.message);
    std::ifstream orderbook(sample.orderbook);
    REQUIRE(messages.good());
    REQUIRE(orderbook.good());

    // The book is sized generously because the precise live order count is unknown until
    // the file is read, and over sizing the index and level reserves cannot affect
    // correctness.
    obls::BookLinear book(1u << 21, 1u << 18);

    std::string message_line;
    std::string orderbook_line;
    std::size_t row = 0;
    const std::size_t row_cap = 250000;  // bound the runtime on a full day file

    while (row < row_cap && std::getline(messages, message_line) &&
           std::getline(orderbook, orderbook_line)) {
        if (!message_line.empty() && message_line.back() == '\r') {
            message_line.pop_back();
        }
        if (!orderbook_line.empty() && orderbook_line.back() == '\r') {
            orderbook_line.pop_back();
        }
        if (message_line.empty()) {
            continue;
        }

        std::string_view mview(message_line);
        obls::detail::next_field(mview);  // time
        obls::Event event;
        obls::detail::parse_int(obls::detail::next_field(mview), event.type);
        obls::detail::parse_int(obls::detail::next_field(mview), event.id);
        obls::detail::parse_int(obls::detail::next_field(mview), event.size);
        obls::detail::parse_int(obls::detail::next_field(mview), event.price);
        obls::detail::parse_int(obls::detail::next_field(mview), event.direction);

        obls::apply(book, event);

        // A halt can blank the published book in ways the message stream does not encode,
        // so the comparison is skipped on that row rather than asserting against it.
        if (event.type == 7) {
            ++row;
            continue;
        }

        const std::vector<std::int64_t> ref = parse_row(orderbook_line);
        const std::size_t levels = ref.size() / 4;
        for (std::size_t level = 0; level < levels; ++level) {
            const std::int64_t ask_price = ref[4 * level + 0];
            const std::int64_t ask_size = ref[4 * level + 1];
            const std::int64_t bid_price = ref[4 * level + 2];
            const std::int64_t bid_size = ref[4 * level + 3];

            if (ask_size > 0) {
                const obls::Level got = book.nth_ask(level);
                REQUIRE(got.price == ask_price);
                REQUIRE(got.quantity == ask_size);
            }
            if (bid_size > 0) {
                const obls::Level got = book.nth_bid(level);
                REQUIRE(got.price == bid_price);
                REQUIRE(got.quantity == bid_size);
            }
        }
        ++row;
    }

    REQUIRE(row > 0);
}
