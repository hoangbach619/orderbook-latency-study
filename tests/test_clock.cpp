// Calibration sanity and read monotonicity for the timer.
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "bench/clock.hpp"

TEST_CASE("calibrated frequency is positive and plausible") {
    const obls::Timer timer = obls::Timer::calibrate();
    REQUIRE(timer.tsc_hz() > 0.0);
    // Any real time stamp counter or the nanosecond fallback sits comfortably inside
    // this band. A value outside it means calibration produced nonsense.
    REQUIRE(timer.tsc_hz() > 1e8);
    REQUIRE(timer.tsc_hz() < 1e11);
}

TEST_CASE("successive reads do not move backwards") {
    static_cast<void>(obls::Timer::calibrate());
    std::uint64_t previous = obls::Timer::read();
    for (int i = 0; i < 10000; ++i) {
        const std::uint64_t current = obls::Timer::read();
        REQUIRE(current >= previous);
        previous = current;
    }
}
