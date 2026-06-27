// The depth sweep, commit 4's experiment.
//
// The question the whole study builds towards: at how many resting price levels per side
// does the cache friendly contiguous scan stop being the cheapest, and does the branchless
// binary search ever overtake the branching one. Big O says the logarithmic structures must
// win as depth grows. The point here is to find where the hardware actually crosses over,
// which the asymptotics cannot tell us because they ignore cache misses and branch
// mispredictions.
//
// For each depth the book is populated to that depth per side, then a fixed batch of near
// touch lookups is replayed and bracketed by the hardware counters alone, with no per
// operation wall clock read, so the cycles per lookup are clean. Lookups are the operation
// measured because the level lookup is the only thing that differs between the variants,
// and a read leaves the depth fixed so every variant is compared at exactly the same depth.
// The access offset from the touch is geometric, seed 42, so most lookups land near the top
// of book as real order flow does, with a thin tail reaching deeper.
//
// Data source by depth. The shallow depths that a real equity actually rests at are drawn
// from the real LOBSTER AAPL book, by snapshotting it and taking its top levels. The
// extreme depths that no real equity reaches, where the only purpose is to locate the
// structural crossover, use a synthetic deep book seeded 42. Every row is labelled with its
// source so a reader never mistakes a synthetic deep book for a real one.
//
// The real snapshot is a from empty reconstruction of the message stream. The LOBSTER
// sample opens with a non empty book from the opening auction, whose orders the in window
// messages cannot recreate, so this snapshot is a real, internally consistent book driven
// by real order flow but not the auction exact published one. That is sufficient here,
// because the sweep compares variants on the same book and measures structural cost against
// depth, which does not depend on the book being byte identical to the reference. See
// docs/methodology.md.
//
// Cycles per operation needs the kperf counters, which require sudo on Apple Silicon.
// Without them the sweep still runs and writes its table, but the counter columns are blank
// and the run is structural only.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "bench/perf_counters.hpp"
#include "bench/replay.hpp"
#include "obls/book_branchless.hpp"
#include "obls/book_linear.hpp"
#include "obls/book_map.hpp"
#include "obls/book_sorted_vector.hpp"
#include "obls/types.hpp"

namespace obls {

// A precomputed near touch lookup. Computed before the timed batch so the measured loop
// only reads the book and never allocates.
struct SweepTarget {
    Side side;
    Price price;
};

// A resting level used to populate a book before measuring. Ordered worst to best per side
// by the builders below so the contiguous variants append rather than shift on insert.
struct SweepLevel {
    Side side;
    Price price;
    Quantity qty;
};

namespace sweep_detail {

// Mean offset of a lookup from the touch, in levels. Real order flow concentrates within a
// few dozen ticks of the touch regardless of how deep the book is, so the mean is a fixed
// constant rather than a fraction of the depth. The value is documented, not tuned to a
// result. A geometric distribution gives the near touch concentration with a thin deep
// tail.
inline constexpr double kMeanOffsetLevels = 16.0;

// One resting order per level keeps the order index population at two times the depth.
inline std::size_t order_hint_for(std::size_t depth) {
    const std::size_t want = 2 * depth + 1024;
    return want;
}

inline std::size_t level_hint_for(std::size_t depth) {
    return depth + 1024;
}

// Populate a freshly constructed book from levels given worst to best per side, so the
// contiguous variants push onto the hot end instead of shifting the whole array each
// insert.
template <typename Book>
void populate(Book& book, const std::vector<SweepLevel>& levels) {
    OrderId id = 1;
    for (const SweepLevel& level : levels) {
        book.add(id, level.side, level.price, level.qty);
        ++id;
    }
}

// Synthetic deep book: depth consecutive tick levels per side around a mid high enough that
// even the deepest level stays a positive price. Quantities are seeded 42. Built worst to
// best per side. Used for the extreme depths no real equity reaches.
inline void build_synthetic_levels(std::size_t depth, std::vector<SweepLevel>& out) {
    out.clear();
    out.reserve(2 * depth);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Quantity> qty(1, 500);
    const Price tick = 1;
    const Price mid =
        1'000'000'000;  // far above the deepest offset so prices stay positive
    // Bids worst to best: lowest price first, so each insert appends at the back.
    for (std::size_t k = depth; k >= 1; --k) {
        const Price price = mid - static_cast<Price>(k) * tick;
        out.push_back(SweepLevel{Side::Buy, price, qty(rng)});
    }
    for (std::size_t k = depth; k >= 1; --k) {
        const Price price = mid + static_cast<Price>(k) * tick;
        out.push_back(SweepLevel{Side::Sell, price, qty(rng)});
    }
}

// Real book levels: the top depth levels of a snapshot of the real LOBSTER book, taken
// worst to best per side. The snapshot's aggregate quantity at each price becomes one
// resting order in the rebuilt book, which is all the lookup path observes.
template <typename Snapshot>
void build_real_levels(const Snapshot& snapshot, std::size_t depth,
                       std::vector<SweepLevel>& out) {
    out.clear();
    out.reserve(2 * depth);
    // Rank depth-1 is the worst of the top depth, rank 0 is the best, so descend to append.
    for (std::size_t r = depth; r >= 1; --r) {
        const Level level = snapshot.nth_bid(r - 1);
        out.push_back(SweepLevel{Side::Buy, level.price, level.quantity});
    }
    for (std::size_t r = depth; r >= 1; --r) {
        const Level level = snapshot.nth_ask(r - 1);
        out.push_back(SweepLevel{Side::Sell, level.price, level.quantity});
    }
}

// Precompute the lookup targets from a populated reference book. Every variant holds the
// same levels, so the same target prices are valid for all of them, which keeps the
// comparison fair. nth is O(1) on the contiguous reference, so building the targets is
// cheap.
template <typename Book>
void build_targets(const Book& book, std::size_t count, std::vector<SweepTarget>& out) {
    out.clear();
    out.reserve(count);
    std::mt19937_64 rng(42);
    std::geometric_distribution<std::size_t> offset(1.0 / (kMeanOffsetLevels + 1.0));
    const std::size_t bid_depth = book.bid_depth();
    const std::size_t ask_depth = book.ask_depth();
    for (std::size_t i = 0; i < count; ++i) {
        const bool buy = (i & 1U) == 0U;
        const std::size_t depth = buy ? bid_depth : ask_depth;
        if (depth == 0) {
            continue;
        }
        std::size_t rank = offset(rng);
        if (rank >= depth) {
            rank = depth - 1;
        }
        const Level level = buy ? book.nth_bid(rank) : book.nth_ask(rank);
        out.push_back(SweepTarget{buy ? Side::Buy : Side::Sell, level.price});
    }
}

// Run the lookup batch once untimed to warm caches and train the predictor, then once
// bracketed by the counters. Returns the counter delta over the timed batch. The
// accumulator is forced observable so the lookups cannot be optimised away, and the opaque
// counter calls stop the timed loop being hoisted out of the bracket.
template <typename Book>
CounterReading measure_lookups(Book& book, const std::vector<SweepTarget>& targets,
                               PerfCounters& counters) {
    std::uint64_t acc = 0;
    for (const SweepTarget& target : targets) {
        acc += static_cast<std::uint64_t>(book.quantity_at(target.side, target.price));
    }
    counters.start();
    for (const SweepTarget& target : targets) {
        acc += static_cast<std::uint64_t>(book.quantity_at(target.side, target.price));
    }
    counters.stop();
#if defined(__GNUC__)
    asm volatile("" : : "r"(acc) : "memory");
#else
    volatile std::uint64_t observed = acc;
    static_cast<void>(observed);
#endif
    return counters.read_counters();
}

struct VariantResult {
    const char* name;
    char tag;
    bool counters_ok;
    double cyc_per_op;
    double bmiss_per_op;
    double cmiss_per_op;
};

// Build one variant at the given depth, measure the lookup batch, and reduce the counter
// delta to per operation figures. Templated over the book type so the four variants share
// one code path.
template <typename Book>
VariantResult measure_variant(const char* name, char tag, const std::vector<SweepLevel>& lv,
                              const std::vector<SweepTarget>& targets, Book&& book,
                              PerfCounters& counters) {
    populate(book, lv);
    const CounterReading reading = measure_lookups(book, targets, counters);
    VariantResult out{name, tag, reading.available, 0.0, 0.0, 0.0};
    if (reading.available && !targets.empty()) {
        const double ops = static_cast<double>(targets.size());
        out.cyc_per_op = static_cast<double>(reading.cycles) / ops;
        out.bmiss_per_op = static_cast<double>(reading.branch_misses) / ops;
        out.cmiss_per_op = static_cast<double>(reading.cache_misses) / ops;
    }
    return out;
}

}  // namespace sweep_detail

// Drives the depth sweep across all four variants and writes a machine readable table plus
// a readable summary. real_available says whether the LOBSTER events were loaded; when
// false, every depth falls back to a synthetic book and is labelled as such.
inline void run_depth_sweep(const std::vector<Event>& real_events, bool real_available,
                            const std::string& csv_path) {
    const std::size_t depths[] = {10, 100, 1000, 10000, 100000};
    const std::size_t kLookups = 200000;

    // Build the real snapshot once. Its depth decides how deep the LOBSTER rows can reach.
    std::size_t snapshot_depth = 0;
    BookSortedVector snapshot(1024, 1024);
    if (real_available && !real_events.empty()) {
        const StreamStats stats = analyse(real_events);
        const std::size_t order_hint =
            stats.peak_live_estimate < 1024 ? 1024 : stats.peak_live_estimate + 1024;
        const std::size_t level_hint =
            stats.add_count < 1024 ? 1024 : stats.add_count + 1024;
        snapshot = BookSortedVector(order_hint, level_hint);
        for (const Event& event : real_events) {
            apply(snapshot, event);
        }
        snapshot_depth = std::min(snapshot.bid_depth(), snapshot.ask_depth());
    }

    std::FILE* csv = std::fopen(csv_path.c_str(), "w");
    if (csv != nullptr) {
        std::fprintf(csv,
                     "depth,source,variant,cycles_per_op,branch_misses_per_op,"
                     "cache_misses_per_op,counters\n");
    }

    std::printf(
        "\ndepth sweep, near touch lookups, cycles per operation is the headline\n");
    std::printf("real snapshot depth per side: %zu levels%s\n", snapshot_depth,
                real_available ? "" : " (no real data, all rows synthetic)");
    std::printf("%8s  %-9s  %10s %10s %10s %10s   cheapest\n", "depth", "source", "A:map",
                "B:scan", "C:bsrch", "D:branchless");

    PerfCounters counters;
    std::vector<SweepLevel> levels;
    std::vector<SweepTarget> targets;
    targets.reserve(kLookups);

    bool any_counters = false;
    // Track the crossover headlines as the sweep deepens.
    for (const std::size_t depth : depths) {
        const bool use_real = real_available && depth <= snapshot_depth;
        const char* source = use_real ? "LOBSTER" : "SYNTHETIC";
        if (use_real) {
            sweep_detail::build_real_levels(snapshot, depth, levels);
        } else {
            sweep_detail::build_synthetic_levels(depth, levels);
        }

        // Targets come from a contiguous reference holding the same levels, so they are
        // valid for every variant and cheap to compute.
        BookSortedVector reference(sweep_detail::order_hint_for(depth),
                                   sweep_detail::level_hint_for(depth));
        sweep_detail::populate(reference, levels);
        sweep_detail::build_targets(reference, kLookups, targets);

        const std::size_t oh = sweep_detail::order_hint_for(depth);
        const std::size_t lh = sweep_detail::level_hint_for(depth);
        const sweep_detail::VariantResult results[] = {
            sweep_detail::measure_variant("A", 'A', levels, targets, BookMap(oh), counters),
            sweep_detail::measure_variant("B", 'B', levels, targets, BookLinear(oh, lh),
                                          counters),
            sweep_detail::measure_variant("C", 'C', levels, targets,
                                          BookSortedVector(oh, lh), counters),
            sweep_detail::measure_variant("D", 'D', levels, targets, BookBranchless(oh, lh),
                                          counters),
        };

        // Cheapest in cycles per operation, when counters are present.
        char cheapest = '?';
        double best = 0.0;
        for (const sweep_detail::VariantResult& r : results) {
            if (!r.counters_ok) {
                continue;
            }
            any_counters = true;
            if (cheapest == '?' || r.cyc_per_op < best) {
                best = r.cyc_per_op;
                cheapest = r.tag;
            }
        }

        if (results[0].counters_ok) {
            std::printf("%8zu  %-9s  %10.1f %10.1f %10.1f %10.1f   %c\n", depth, source,
                        results[0].cyc_per_op, results[1].cyc_per_op, results[2].cyc_per_op,
                        results[3].cyc_per_op, cheapest);
        } else {
            std::printf("%8zu  %-9s  %10s %10s %10s %10s   (counters unavailable, sudo)\n",
                        depth, source, "n/a", "n/a", "n/a", "n/a");
        }

        if (csv != nullptr) {
            for (const sweep_detail::VariantResult& r : results) {
                if (r.counters_ok) {
                    std::fprintf(csv, "%zu,%s,%s,%.3f,%.5f,%.5f,ok\n", depth, source,
                                 r.name, r.cyc_per_op, r.bmiss_per_op, r.cmiss_per_op);
                } else {
                    std::fprintf(csv, "%zu,%s,%s,,,,unavailable\n", depth, source, r.name);
                }
            }
        }
    }

    if (csv != nullptr) {
        std::fclose(csv);
        std::printf("\nwrote %s\n", csv_path.c_str());
    }
    if (any_counters) {
        std::printf(
            "branch and cache miss per operation columns are in the CSV beside cycles\n");
    } else {
        std::printf(
            "WARNING        counters unavailable, cycles per operation not measured; "
            "re-run with sudo on Apple Silicon\n");
    }
}

}  // namespace obls
