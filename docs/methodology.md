# Methodology

Measurement is the product of this study, so the method is documented in as much detail as
the result. Every choice below exists to make the latency distributions trustworthy and
attributable to a known machine.

## Timing mechanism

Each operation is timed with the x86 time stamp counter through `rdtscp`. The counter read
is bracketed by `lfence` on both sides. The leading fence stops the read from being issued
before prior work has completed, and the trailing fence stops later work from being issued
before the read retires, so an interval bracketed by two reads contains only the code under
test and not a speculatively reordered neighbour.

The study requires an invariant time stamp counter, one that advances at a fixed rate
independent of core frequency scaling and turbo. Without that invariant a cycle delta does
not convert to a fixed wall time, and the nanosecond figures would be meaningless. The
timer detects the invariant flag through `cpuid` leaf `0x80000007` and the benchmark prints
whether it is present. If it is absent, the numbers should not be trusted.

## Calibration

The counter frequency is measured once at start up against `std::chrono::steady_clock` over
a short fixed interval, and the resulting hertz value is recorded and printed. All cycle to
nanosecond conversions use that single calibrated factor. Record the printed `tsc_hz` in
`docs/hardware.md` alongside each result so the conversion is reproducible.

The harness also measures the overhead of an empty `rdtscp` read pair and prints it. The
per operation latencies are not corrected for this overhead; the figure is printed so a
reader can account for it when comparing against other measurement setups.

## Portable fallback

Off x86, or where `rdtscp` is unavailable, the timer falls back to `steady_clock` and
reports its readings directly as nanoseconds. This keeps the project building and the clock
test passing everywhere, but the per call overhead of `steady_clock` is far too high to
time a single book operation honestly. The benchmark therefore flags the fallback loudly
and the measurement runs assume x86 Linux with an invariant time stamp counter.

## Thread pinning

The benchmark pins itself to a single core with `sched_setaffinity` before measuring, which
removes scheduler migration and the cold caches that follow a migration. On the measurement
machine the chosen core should also be isolated from the scheduler and from interrupts so
that nothing else runs on it during a measurement.

## Warm up

Before any sample is recorded, the full event stream is replayed once on a throwaway
instance of the same variant. This faults in pages, fills the instruction cache, and trains
the branch predictor, so the measured pass reflects steady state behaviour rather than cold
start costs.

## Recording every operation, and coordinated omission

One latency sample is recorded for every measured operation into a buffer pre allocated to
the known operation count, so recording never allocates on the timed path.

The study measures every operation as the replay drives it, rather than issuing operations
at a fixed offered rate and sampling the response. This sidesteps coordinated omission in
the sense Gil Tene describes. Coordinated omission arises when a load generator that issues
at a target rate stalls along with the system under test, and so stops issuing requests
during exactly the periods of worst latency, which then go unrecorded and flatter the tail.
Here there is no offered rate to fall behind: the replay applies the next event only after
the previous one completes, every operation is timed, and no operation can be omitted. The
cost is that this measures service time under replay, not response time under a fixed
arrival process; that is the right quantity for comparing data structures in isolation.

## Exact percentiles

After the run, percentiles are computed exactly by partial selection over the recorded
buffer with `nth_element`, not by accumulating into a histogram. The tail is the whole
point of the study, and histogram bucketing blurs precisely the p99.9 and p99.99 figures
that matter most. The nearest rank method is used: the reported percentile is the smallest
sample whose cumulative share reaches the requested fraction. Reported points are p50, p90,
p99, p99.9, p99.99, and max, for each operation type.

## Hardware counters

Over the timed pass the harness reads a `perf_event_open` counter group: instructions,
cycles, branch instructions, branch misses, and cache misses. From these it reports
instructions per cycle, branch misprediction rate, and cache misses per thousand
instructions. The counters are read once across the whole pass rather than per operation,
because a counter read on every operation would cost more than the operation it brackets and
would dominate the measured interval. The aggregate figures are what explain the latency
result: the map variant is expected to lose on cache misses from pointer chasing, and the
contiguous variant can lose on branch misses once its scan grows long.

## Required machine state

Before quoting any number, on the measurement machine:

- Build in release with `-O3 -march=native`.
- Disable frequency scaling and pin the governor to performance.
- Disable turbo so the core runs at a fixed frequency that matches the invariant time stamp
  counter rate.
- Pin the benchmark to a single isolated core.
- Confirm the benchmark reports `rdtscp` in use and an invariant time stamp counter present.
- Record the full machine description in `docs/hardware.md`.
