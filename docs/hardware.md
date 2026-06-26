# Measurement machine

Every result is attributable to one machine in one state. Copy this template and fill it in
for each run that produces numbers committed to `docs/results.md`. A latency figure without
a completed record of this kind is not a result.

## Template

```
Date:                     YYYY-MM-DD
CPU model:                e.g. Intel Core i7-9700K
Base frequency:           e.g. 3.6 GHz
Core pinned:              e.g. core 2, isolated via isolcpus
Frequency governor:       e.g. performance
Frequency scaling:        disabled / enabled
Turbo boost:              disabled / enabled
Invariant TSC present:    yes / no   (as reported by the benchmark)
Calibrated tsc_hz:        e.g. 3600000123   (as printed by the benchmark)
Timer overhead:           e.g. 7.2 ns       (as printed by the benchmark)
SMT / hyperthreading:     disabled / enabled
RAM:                      e.g. 32 GB DDR4-3200
Kernel:                   e.g. Linux 6.8.0-generic
Compiler:                 e.g. gcc 13.2.0 / clang 18.1.0
Build type:               Release, -O3 -march=native
Data file:                e.g. AAPL_2012-06-21_message_10.csv, or SYNTHETIC seed 42
Event count:              e.g. 268431
```

## Notes on state

- The pinned core should be isolated from the scheduler and from interrupt handling so that
  nothing else runs on it during a measurement.
- Frequency scaling and turbo must be off so the core frequency is fixed and matches the
  invariant time stamp counter rate the calibration relies on.
- If the benchmark reports the steady clock fallback or an absent invariant time stamp
  counter, do not record the run as a result.
