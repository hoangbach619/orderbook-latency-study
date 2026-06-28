# Results

## Headline

Cycles per operation, near touch lookup, mean of five runs on an Apple M2 Pro driven by
replayed AAPL order flow:

```
depth     source      A:map    B:scan   C:bsearch   D:branchless   cheapest
10        LOBSTER      20.8     26.0     22.9        13.8           D
100       LOBSTER      72.8     50.6     43.2        21.8           D
1000      SYNTHETIC    118.0    50.7     44.1        31.3           D
10000     SYNTHETIC    179.3    50.7     86.9        45.4           D
100000    SYNTHETIC    242.1    50.8     115.4       55.0           B
```

Variant D, the branchless binary search, is the cheapest at every depth from 10 to 10000
levels per side. Linear scan, variant B, takes the lead only at 100000. The ordering held in
all five runs, D ahead from 10 through 10000 and B ahead at 100000, so this is signal rather
than run to run noise.

The shape matches Gross's CppCon result, simple cache aware code overtaking the cleverer
structure once the book grows. The crossover sits elsewhere. His x86 linear scan led by about
1000 levels. Here the scan does not lead until 100000. The crossover is two orders of
magnitude deeper on this silicon, measured rather than assumed.

The branch and cache miss per lookup figures below are from the representative run recorded in
`results/depth_sweep.csv`. The cycle figures above are the five run mean.

## Mechanisms

B costs 50.6 cycles at depth 100 and 50.8 at depth 100000. The book deepens by three orders
of magnitude and the cost moves by under half a percent. Activity sits at the touch, so the
scan walks a short run from the hot end of one contiguous array, and its length is set by how
far the lookup falls from the best price, not by the depth. Once the book is deeper than the
access offset, by depth 100, the scan length saturates and the cost stops tracking depth. The
bounded scan is what holds the cost flat. B reads a short contiguous run the prefetcher streams
ahead of, and its cache miss per lookup stays under 0.02 through depth 10000. Even at 0.066 per
lookup at 100000 the misses are too rare to move the 50 cycle cost.

C rises from 43.2 cycles at depth 100 to 115.4 at 100000, a 2.7 times climb over the span that
leaves B unmoved. The probe count drives it: the work grows with the log of the depth, from
about seven probes at depth 100 to seventeen at 100000. C also sits above D at every depth,
and that gap is the branch. Each probe ends in a data dependent comparison the predictor
cannot learn, and C pays one misprediction per probe. The cache does not punish C here. On the
contiguous array its miss per lookup stays under 0.02, because two hundred thousand repeated
near touch lookups keep the upper probe positions and the top levels resident. At these depths
the binary search loses to the scan on probe count and misprediction, not on memory.

The map shows what a cache bound climb actually looks like. A is the most expensive at every
depth and the only variant whose miss per lookup grows with depth, from 0.007 at depth 10 to
0.064 at 100000, as its separately allocated nodes scatter across the heap. That is the
textbook tree result. It belongs to A, not to the contiguous binary search.

D is cheaper than C at every depth, 13.8 against 22.9 at the shallow end and 55.0 against 115.4
at the deep end. The conditional advance in its search is a conditional move, so the per step
comparison never becomes a branch to mispredict. The counters show this at two scales. On the
full add cancel reduce mix on real data the branch misses per operation run 1.670 for C and
1.456 for D, a modest gap because most of the mix's branch misses come from the order index
probes and the array shifts, not the lookup. On the isolated lookup the reduction is far
larger: D's branch misses per lookup fall close to zero while C's sit near a third of a
lookup. D mispredicts measurably less in the mix and by a large margin in isolation, and it
spends fewer cycles per lookup at every depth measured.

## The branchless property is architecture specific

D is only branchless because the source forces it, and the force needed differs by target. On
arm64 the portable mask form lowers to csel under both GCC 14 and Clang, with the loop counter
as the only branch. On x86 under Clang it does not survive. LLVM's X86CmovConversion pass
rewrites the compiler's own cmov back into a data dependent branch inside the loop, and it does
so for every portable idiom tried: the mask, a ternary, a multiply by the comparison, a pointer
select, an explicit if carrying the unpredictable hint. Disabling the pass restores the cmov,
which pins the cause. So D carries a hand written cmov on x86 and the mask form on arm64. The
branchless property was measured on this arm64 machine and defended at instruction level on x86
by disassembly, not by a second run.

## Caveats

- The sweep measures an isolated near touch lookup, not the full add cancel reduce mix. The
  mix carries order index churn and array shifting that dwarf the lookup, which is why an
  earlier full mix run did not put D ahead. D's edge lives in the lookup, and the sweep isolates
  the lookup to show it.
- Depths 10 and 100 are the real LOBSTER AAPL book. The real sample reached 942 levels per
  side, so 1000 and deeper use a synthetic book seeded 42. The deep rows locate the structural
  crossover and do not claim a real equity rests that deep.
- The figures are the mean of five runs and the crossover was stable across them. The measuring
  thread is QoS biased to a performance core rather than hard pinned, and the wall clock is
  about 42 ns granular, so cycles per operation is the primary metric and only the tail of the
  nanosecond latency distribution is worth reading.
