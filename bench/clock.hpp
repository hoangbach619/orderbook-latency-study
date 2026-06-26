// Cycle accurate timing.
//
// On x86_64 the timer reads the time stamp counter with rdtscp, bracketed by load
// fences so the read cannot drift across the code being measured. The counter
// frequency is calibrated once against a steady clock at start up and recorded, so the
// methodology document can state the exact cycles to nanoseconds factor used for a run.
//
// The study requires an invariant time stamp counter, one that ticks at a fixed rate
// independent of core frequency scaling and turbo. Without that invariant a cycle delta
// would not convert to a fixed wall time and the nanosecond figures would be wrong.
// calibrate detects the invariant flag through cpuid and exposes it so the harness can
// refuse to trust the numbers when it is absent.
//
// Off x86, and anywhere rdtscp is unavailable, the timer falls back to steady_clock and
// reports cycles directly as nanoseconds. That fallback is fine for the portable build
// and the clock test, but its per call overhead is far too high to time a single book
// operation honestly, so using_rdtscp is surfaced and the benchmark flags the fallback
// loudly rather than presenting fallback timings as if they were trustworthy.
#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#define OBLS_HAVE_RDTSCP 1
#include <x86intrin.h>
#if defined(__GNUC__)
#include <cpuid.h>
#endif
#else
#define OBLS_HAVE_RDTSCP 0
#endif

namespace obls {

class Timer {
public:
    static Timer calibrate() {
        Timer timer;
#if OBLS_HAVE_RDTSCP
        timer.using_rdtscp_ = true;
        timer.invariant_tsc_ = detect_invariant_tsc();

        const auto steady_start = std::chrono::steady_clock::now();
        const std::uint64_t tsc_start = read();
        const std::chrono::nanoseconds target{50'000'000};
        std::chrono::steady_clock::time_point steady_end;
        std::uint64_t tsc_end = tsc_start;
        do {
            steady_end = std::chrono::steady_clock::now();
            tsc_end = read();
        } while (steady_end - steady_start < target);
        const double seconds =
            std::chrono::duration<double>(steady_end - steady_start).count();
        timer.tsc_hz_ = static_cast<double>(tsc_end - tsc_start) / seconds;
#else
        timer.using_rdtscp_ = false;
        timer.invariant_tsc_ = false;
        timer.tsc_hz_ = 1e9;  // the fallback counter is already in nanoseconds
#endif
        return timer;
    }

    // The two load fences pin the counter read between completed prior work and not yet
    // issued later work, so an interval bracketed by two reads contains only the code
    // under test and not speculatively reordered neighbours.
    static inline std::uint64_t read() {
#if OBLS_HAVE_RDTSCP
        unsigned aux;
        _mm_lfence();
        const std::uint64_t tsc = __rdtscp(&aux);
        _mm_lfence();
        return tsc;
#else
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
#endif
    }

    double cycles_to_ns(std::uint64_t cycles) const {
#if OBLS_HAVE_RDTSCP
        return static_cast<double>(cycles) * 1e9 / tsc_hz_;
#else
        return static_cast<double>(cycles);  // already nanoseconds
#endif
    }

    double tsc_hz() const { return tsc_hz_; }
    bool using_rdtscp() const { return using_rdtscp_; }
    bool invariant_tsc() const { return invariant_tsc_; }

private:
    static bool detect_invariant_tsc() {
#if OBLS_HAVE_RDTSCP && defined(__GNUC__)
        unsigned eax = 0;
        unsigned ebx = 0;
        unsigned ecx = 0;
        unsigned edx = 0;
        if (__get_cpuid(0x80000007U, &eax, &ebx, &ecx, &edx) != 0) {
            return (edx & (1U << 8)) != 0U;  // invariant TSC advertised in EDX bit 8
        }
        return false;
#else
        return false;
#endif
    }

    double tsc_hz_ = 1e9;
    bool using_rdtscp_ = false;
    bool invariant_tsc_ = false;
};

}  // namespace obls
