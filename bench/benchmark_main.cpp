// The latency harness.
//
// For each book variant the same event stream is replayed twice: once untimed to warm
// the instruction cache and train the branch predictor, then once with every add,
// cancel, reduce, and best quote read individually timed with rdtscp. Per operation
// type the full percentile set is reported, alongside the hardware counters gathered
// over the timed pass, so the latency result and its cache and branch explanation sit
// next to each other.
//
// The numbers are only meaningful on a controlled machine. They depend on the release
// build with march native, on a pinned core, and on frequency scaling and turbo being
// disabled so the time stamp counter rate matches the core rate. See docs/methodology.md
// and record the machine in docs/hardware.md before quoting any figure.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread/qos.h>
#endif

#include "bench/clock.hpp"
#include "bench/percentiles.hpp"
#include "bench/perf_counters.hpp"
#include "bench/replay.hpp"
#include "obls/book_branchless.hpp"
#include "obls/book_linear.hpp"
#include "obls/book_map.hpp"
#include "obls/book_sorted_vector.hpp"

namespace {

using obls::Event;
using obls::Level;
using obls::Percentiles;
using obls::Timer;

// Forces the compiler to treat the value as observed, so a best quote read is never
// optimised away. The memory clobber also blocks reordering of the timed region.
template <typename T>
inline void sink(const T& value) {
#if defined(__GNUC__)
    asm volatile("" : : "m"(value) : "memory");
#else
    volatile T copy = value;
    static_cast<void>(copy);
#endif
}

bool pin_to_core(int core) {
#if defined(__linux__)
    // A negative core is not a valid cpu index, so reject it before it reaches the cpu
    // mask. This guard also makes the cast below unambiguously safe.
    if (core < 0) {
        return false;
    }
    // glibc's CPU_SET converts its argument to an unsigned type to index the mask, which
    // trips -Werror=sign-conversion when a signed int is passed. core is known non
    // negative here, so convert it explicitly.
    const unsigned cpu = static_cast<unsigned>(core);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    static_cast<void>(core);
    return false;
#endif
}

// Biases the measuring thread onto a performance core on Apple Silicon. macOS exposes no
// hard core pinning like sched_setaffinity, so pin_to_core is a no op there; requesting the
// interactive quality of service class is the closest available lever, nudging the
// scheduler to keep this thread off the efficiency cores. No op on every other platform.
void request_measurement_qos() {
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

void print_op(const char* op, Percentiles p, const Timer& timer) {
    if (p.count == 0) {
        std::printf("    %-11s no samples\n", op);
        return;
    }
    std::printf(
        "    %-11s n=%-9zu p50=%8.1f p90=%8.1f p99=%8.1f p99.9=%8.1f p99.99=%8.1f "
        "max=%10.1f\n",
        op, p.count, timer.cycles_to_ns(p.p50), timer.cycles_to_ns(p.p90),
        timer.cycles_to_ns(p.p99), timer.cycles_to_ns(p.p999), timer.cycles_to_ns(p.p9999),
        timer.cycles_to_ns(p.max));
}

template <typename Factory>
void run_variant(const char* name, Factory make_book, const std::vector<Event>& events,
                 const Timer& timer) {
    // Warm up on a throwaway instance to fault in pages and train the predictor before
    // any sample is recorded.
    {
        auto warm = make_book();
        for (const Event& event : events) {
            obls::apply(warm, event);
        }
    }

    auto book = make_book();

    obls::LatencySamples add_s(events.size());
    obls::LatencySamples cancel_s(events.size());
    obls::LatencySamples reduce_s(events.size());
    obls::LatencySamples quote_s(events.size());

    obls::PerfCounters counters;
    counters.start();
    for (const Event& event : events) {
        switch (event.type) {
            case 1: {
                const std::uint64_t t0 = Timer::read();
                book.add(event.id,
                         event.direction == 1 ? obls::Side::Buy : obls::Side::Sell,
                         event.price, event.size);
                const std::uint64_t t1 = Timer::read();
                add_s.record(t1 - t0);
                break;
            }
            case 2:
            case 4: {
                const std::uint64_t t0 = Timer::read();
                book.reduce(event.id, event.size);
                const std::uint64_t t1 = Timer::read();
                reduce_s.record(t1 - t0);
                break;
            }
            case 3: {
                const std::uint64_t t0 = Timer::read();
                book.cancel(event.id);
                const std::uint64_t t1 = Timer::read();
                cancel_s.record(t1 - t0);
                break;
            }
            default:
                break;
        }
        const std::uint64_t q0 = Timer::read();
        const Level bid = book.best_bid();
        const Level ask = book.best_ask();
        const std::uint64_t q1 = Timer::read();
        sink(bid);
        sink(ask);
        quote_s.record(q1 - q0);
    }
    counters.stop();
    const obls::CounterReading reading = counters.read_counters();

    std::printf("\n%s\n", name);
    print_op("add", add_s.compute(), timer);
    print_op("cancel", cancel_s.compute(), timer);
    print_op("reduce", reduce_s.compute(), timer);
    print_op("best_quote", quote_s.compute(), timer);
    if (reading.available) {
        std::printf(
            "    counters    IPC=%.2f  branch-miss=%.2f%%  cache-miss=%.2f /1k-instr\n",
            reading.ipc(), reading.branch_miss_rate() * 100.0,
            reading.cache_misses_per_kilo_instr());
    } else {
        std::printf("    counters    unavailable on this platform or configuration\n");
    }
}

std::uint64_t timer_overhead_cycles() {
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t a = Timer::read();
        const std::uint64_t b = Timer::read();
        if (b - a < best) {
            best = b - a;
        }
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    std::string data_path;
    std::size_t synthetic_count = 1'000'000;
    std::size_t max_events = 0;  // zero means no cap
    int core = 2;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--count" && i + 1 < argc) {
            synthetic_count =
                static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--max-events" && i + 1 < argc) {
            max_events = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--core" && i + 1 < argc) {
            core = std::atoi(argv[++i]);
        } else if (!arg.empty() && arg[0] != '-') {
            data_path = arg;
        }
    }

    std::vector<Event> events;
    bool synthetic = true;
    if (!data_path.empty() && obls::load_lobster_messages(data_path, events)) {
        synthetic = false;
    } else {
        if (!data_path.empty()) {
            std::printf("could not open %s, falling back to synthetic data\n",
                        data_path.c_str());
        }
        events = obls::make_synthetic_stream(synthetic_count);
    }
    if (max_events != 0 && events.size() > max_events) {
        events.resize(max_events);
    }

    const obls::StreamStats stats = obls::analyse(events);
    const bool pinned = pin_to_core(core);
    request_measurement_qos();
    const Timer timer = Timer::calibrate();

    // Probe counter availability once, up front, so the run announces whether it will carry
    // the cache and branch explanation or be timing only. On Apple Silicon the kperf path
    // needs sudo; without it, as on any platform without counter access, this prints a
    // single loud warning in the same spirit as the steady clock timer fallback below.
    bool counters_available = false;
    {
        obls::PerfCounters probe;
        counters_available = probe.available();
    }

    std::printf("orderbook latency study\n");
    std::printf("data           %s\n",
                synthetic ? "SYNTHETIC (deterministic, seed 42)" : data_path.c_str());
    std::printf("events         %zu\n", events.size());
    std::printf("peak live est  %zu\n", stats.peak_live_estimate);
    std::printf("core pinned    %s\n", pinned ? "yes" : "no (not Linux or denied)");
    std::printf("timer          %s, tsc_hz=%.0f, invariant_tsc=%s\n",
                timer.using_rdtscp() ? "rdtscp" : "steady_clock FALLBACK", timer.tsc_hz(),
                timer.invariant_tsc() ? "yes" : "no/unknown");
    if (!timer.using_rdtscp()) {
        std::printf(
            "WARNING        steady_clock fallback overhead is too high to trust "
            "single operation timings\n");
    }
    if (!counters_available) {
        std::printf(
            "WARNING        hardware counters unavailable, this run is timing only "
            "(on Apple Silicon, run with sudo)\n");
    }
    std::printf("timer overhead %.1f ns (empty read pair, not subtracted below)\n",
                timer.cycles_to_ns(timer_overhead_cycles()));
    std::printf("all latencies below are in nanoseconds\n");

    const std::size_t order_hint =
        stats.peak_live_estimate < 1024 ? 1024 : stats.peak_live_estimate;
    const std::size_t level_hint = stats.add_count < 1024 ? 1024 : stats.add_count;

    run_variant(
        "variant A: std::map", [&] { return obls::BookMap(order_hint); }, events, timer);
    run_variant(
        "variant B: contiguous vector with linear scan",
        [&] { return obls::BookLinear(order_hint, level_hint); }, events, timer);
    run_variant(
        "variant C: sorted vector with binary search",
        [&] { return obls::BookSortedVector(order_hint, level_hint); }, events, timer);
    // Variant D prints its branch miss rate alongside the others on purpose: the claim it
    // exists to test is that the conditional move lowers that rate relative to variant C,
    // so the counter line is part of D's result, not just context.
    run_variant(
        "variant D: branchless binary search",
        [&] { return obls::BookBranchless(order_hint, level_hint); }, events, timer);

    return 0;
}
