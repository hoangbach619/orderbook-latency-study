# orderbook-latency-study

A comparative latency study of order book price level data structures, with hardware
counter analysis. It reproduces and explains a result from David Gross's CppCon 2024
Optiver keynote: at realistic book depths a contiguous structure scanned linearly can
beat a `std::map` with logarithmic lookup, because it respects the cache.

## The question

A limit order book keeps, per side, the resting quantity at each price level. The
textbook choice is an ordered associative container, `std::map`, which gives logarithmic
insertion and lookup and keeps the best price at `begin()`. The asymptotics look
unbeatable. Yet a plain contiguous vector of levels, scanned linearly from the best price
outward, is routinely faster at the depths a real book occupies near the top.

The reason is memory, not complexity. A `std::map` node is a separate heap allocation, so
walking even a few levels chases pointers across memory and pays a cache miss per step. A
contiguous vector packs four sixteen byte levels into every sixty four byte cache line, so
a short scan over the top of book touches one or two lines that the prefetcher has usually
already brought in, with no pointer chasing and no per node allocation. Order flow
concentrates near the top of book, so the scan is short in the common case, and the branch
predictor learns the short loop. The interesting part is the crossover: as depth grows the
scan eventually gets long enough that the map's asymptotics win again, and locating and
explaining that crossover is the point of the experiment.

## What this repository is

The deliverable is rigorous measurement, not a feature rich engine. The value here is
honest latency distributions plus a hardware level account of why each structure wins or
loses. Measurement methodology is treated as the product:

- Timing is per operation with `rdtscp`, calibrated once against a steady clock.
- Every operation is recorded and percentiles are computed exactly, never as a mean and
  never from a histogram.
- Reported percentiles are p50, p90, p99, p99.9, p99.99, and max for every operation.
- Hardware counters report instructions per cycle, branch misprediction rate, and cache
  misses, so the latency result comes with its explanation.
- Real market data drives the benchmark. A deterministic synthetic stream is a clearly
  labelled fallback only.
- The measured hot path performs no heap allocation. Everything is pre allocated. The only
  allocations on a timed path are those intrinsic to the container under test, which for
  `std::map` is precisely part of what is being measured.

## Variants under test

This first slice implements two variants behind a common C++20 `Book` concept, so the
harness drives every variant through identical call sites.

- Variant A, `std::map` keyed by price. The baseline that exists to be beaten.
- Variant B, a contiguous `std::vector` of levels held sorted with the best price at the
  back, scanned linearly from that hot end.

A shared open addressing order index maps order id to the stored order, identical across
variants, because the price level container is the only independent variable in the study.

## Build

C++20. Builds with GCC and Clang, warnings as errors. Catch2 is fetched at configure time.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitiser builds:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOBLS_SANITIZER=asan-ubsan
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DOBLS_SANITIZER=tsan
```

The measurement build uses `-O3 -march=native`, which is on by default and can be turned
off with `-DOBLS_NATIVE_TUNING=OFF` for portability checks.

## Run the benchmark

```
./build/obls_bench path/to/AAPL_2012-06-21_message_10.csv
```

With no path it falls back to a deterministic synthetic stream seeded with 42 and labels
the output as synthetic:

```
./build/obls_bench
```

The hardware counters and core pinning are Linux only. On other platforms the binary still
runs, falls back to a steady clock, reports counters as unavailable, and flags the fallback
loudly, because steady clock overhead is too high to time a single book operation honestly.

## Honest note on the numbers

Absolute latencies are hardware dependent and mean nothing without the machine they were
produced on. Before quoting a figure, build in release with native tuning, disable
frequency scaling and turbo so the time stamp counter rate matches the core rate, pin the
benchmark to an isolated core, and record the machine in `docs/hardware.md`. The
methodology is described in `docs/methodology.md`. There is no throughput headline here on
purpose; the tail latency distribution is the result.

## Reference

David Gross, CppCon 2024 Optiver keynote on building low latency trading systems. The
contiguous versus tree result reproduced here is from that talk.

## Roadmap

This first commit is one reviewable vertical slice. Planned later commits, not implemented
yet:

- Commit 2, variant C, sorted vector with `std::lower_bound` binary search.
- Commit 3, variant D, branchless binary search, with a note on the branch misprediction
  it removes.
- Commit 4, the depth sweep at roughly ten, one hundred, one thousand, ten thousand, and
  one hundred thousand levels per side, to locate and explain the crossover where linear
  scan stops winning.
- Commit 5, the results write up in `docs/results.md` with measured distributions, the
  cache and branch explanation, and a CSV export of raw results.
- Optional later, an Eytzinger layout variant.
