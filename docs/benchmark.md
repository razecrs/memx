# MemX Benchmark Methodology

## 1. What the benchmark may claim

The Gate 1 benchmark compares address-to-handle index structures under a common
contract. It may establish that one structure had lower lookup time or lower
reported metadata bytes for a named dataset on a named machine.

It may not establish that MemX beats jemalloc, mimalloc, TCMalloc, snmalloc, or
any complete allocator. Those systems exploit allocator-specific layout,
caching, concurrency, and call-path invariants outside this harness.

## 2. Equal semantics

Every timed structure must return the exact same `memx_handle_t` for every
managed granule and `MEMX_HANDLE_INVALID` for every intentionally empty
granule.

The current baselines are:

- flat native-word array;
- two-level direct map with lazily allocated leaves;
- three-level radix map with lazily allocated middle and leaf nodes;
- the same radix map with a 16-entry direct-mapped leaf cache;
- checked bounded MemX;
- proven-valid bounded MemX;
- checked frozen-overlay MemX;
- trusted frozen-overlay MemX;
- checked sparse-directory MemX.

The cached radix baseline includes the cache storage in its byte count. Cache
hit rate is a workload outcome, not fixed by the benchmark.

## 3. Dataset construction

The harness constructs a complete truth array before any structure is timed.
It then populates every baseline from that array and verifies every granule.

Randomness uses a fixed xorshift64* generator. The seed is printed with every
result and can be overridden on the command line.

Queries are generated before timing. Each query selects a random granule and a
random byte inside it. Address generation is not included in lookup time.

### Uniform workload

Every region contains one handle repeated across all granules. Handles differ
between regions. This measures the best case for region-level uniform
compression without making the entire managed range one value.

### Mixed workload

Each region independently becomes dense according to `dense_percent`. Uniform
regions contain one repeated handle; dense regions contain pseudo-random
handles.

This workload exposes the cost of unpredictable region tags as dense fraction
changes. Required sweeps include 0, 1, 5, 10, 25, 50, 75, 90, 99, and 100%.

### Random workload

Every region contains pseudo-random handles. This is the worst case for the
Gate 1 compression scheme and should converge on dense-table memory cost plus
descriptor overhead.

## 4. Timing protocol

The current harness uses `CLOCK_MONOTONIC` and reports nanoseconds per lookup.
It performs a short warmup and carries every result into a checksum.

Publication-quality runs must additionally:

1. use a Release build with the exact compiler and flags recorded;
2. pin the process to one physical core;
3. record CPU model, microcode, kernel, governor, turbo policy, and SMT state;
4. run each dataset in an independent process;
5. randomize or rotate structure execution order;
6. perform at least 15 independent repetitions;
7. report median, p95, p99, minimum, maximum, median absolute deviation, and a
   confidence interval;
8. separate hot-cache, capacity-pressure, and deliberately cold-cache cases;
9. record thermal and frequency behavior where the platform exposes it;
10. retain every raw output row.

One sequence of structures in one process is a smoke result, not a paper result.

## 5. Hardware events

When `perf` is available on Linux, collect at minimum:

```text
cycles
instructions
branches
branch-misses
L1-dcache-loads
L1-dcache-load-misses
LLC-loads
LLC-load-misses
dTLB-loads
dTLB-load-misses
```

Report events per successful lookup. Do not compare raw event totals between
runs with different lookup counts.

Architecture-specific events may refine the analysis but cannot replace the
portable set. Unsupported or multiplexed counters must be labeled.

## 6. Metadata accounting

Four memory quantities remain distinct:

```text
requested metadata bytes
allocator usable bytes
resident metadata pages
reserved virtual address bytes
```

The public `total_bytes` remains a representation diagnostic: adaptive/flat
indexes report live requested allocations, while an overlay combines requested
directory bytes with page-rounded private overlay intervals. It must not be
used for an equal-category gate.

Gate CSV rows use `page_accounted_bytes` instead. For every structure, each
live heap allocation is rounded independently to the actual host page size.
An overlay adds its page-rounded DENSE intervals; its inaccessible guards and
demand-zero EMPTY/UNIFORM slots are not backing bytes. This is a conservative,
equal accounting model—not process RSS and not allocator-usable size. It may
overcount when multiple small heap allocations share one physical page.

On Linux, the benchmark also identifies the exact guarded overlay mapping in
`/proc/self/smaps` and emits its RSS, private, and shared bytes. The sweep tool
preserves these values in every raw row and summarizes their medians. Page-table
memory is not currently observable through this interface and is not silently
estimated.

Publication measurements must add allocator usable size where available and
RSS/page residency through OS interfaces. A sparse virtual reservation cannot
be described as free merely because pages are not resident.

The normalized measure is:

```text
metadata bytes / managed MiB
```

Managed bytes means address coverage offered by the index, not the number of
currently allocated object bytes. Both coverage-normalized and live-byte-
normalized figures may be useful, but they must not be conflated.

## 7. Directory accounting

Every MemX result includes:

- index object;
- bounded descriptor array or sparse root;
- allocated sparse leaves;
- dense secondary tables;
- future reader, retirement, and policy metadata when implemented.

For a frozen overlay, the full virtual reservation is reported separately from
the page-rounded DENSE working set. Demand-zero EMPTY and UNIFORM overlay slots
are not counted as resident representation bytes. The descriptor directory is
still counted because it remains part of every lookup.

The one-load bounded hot path cannot be plotted against a sparse baseline while
omitting its bounded coverage array.

## 8. Code and instruction footprint

Every specialized lookup result must include:

- `.text` bytes for the reachable lookup implementation;
- dynamic instructions per lookup;
- instruction-cache misses when measurable;
- added dispatch tables or initialization code.

An ISA specialization needs a reproducible 3–5% meaningful speed improvement
and justified code/I-cache cost. A 3.1% latency improvement from a 16x code-size
increase does not automatically pass.

## 9. Gate equations

Let `L_x` be median lookup cycles for structure `x` under equal semantics and
`M_x` its page-accounted backing bytes under the model above.

`BALANCED` passes the first low-entropy gate when:

```text
L_memx <= 1.10 * L_flat
M_memx <= 0.50 * M_flat
```

The dense safety gate is:

```text
L_memx <= 1.15 * L_flat
M_memx <= 1.02 * M_flat
```

Both must hold across the declared supported host set, not only the fastest
single run.

The current Gate 1 command uses `memx_overlay_trusted` for the low-entropy
candidate and `memx_flat_fallback` for the fully dense candidate. Their
baselines are the equally trusted `flat_assume_mapped` path. Checked overlay
latency is reported but is not substituted for the allocator-internal gate.

`FAST` later minimizes lookup latency subject to an explicit memory ceiling.
The ceiling and actual usage are reported.

`COMPACT` later minimizes metadata while requiring lookup faster than the
comparable radix structure.

## 10. Real traces

Synthetic entropy is necessary for controlled break-even analysis but cannot
establish that real allocator metadata is sufficiently regular.

Trace records must eventually capture:

- address and size;
- metadata identity or equivalence class;
- allocation, retirement, and removal order;
- thread identity;
- lookup address and dependent/independent status;
- allocator-specific layout hints;
- timestamp or logical sequence number.

Trace collection must avoid storing application payloads and should support
address rebasing when absolute virtual addresses are sensitive.

Replay reports steady-state and peak metadata, mutation cost, update
amplification, and lookup latency. Trace format/version and collection overhead
must be documented.

## 11. Production comparisons

The source-inspired baselines are useful algorithm controls. A named production
comparison requires either calling the production subsystem under equivalent
conditions or integrating MemX into that allocator.

Required claim levels:

```text
Level 0: synthetic standalone structure result
Level 1: allocator trace replay with equal metadata semantics
Level 2: index substituted in an allocator experiment
Level 3: end-to-end workloads with no material allocator regression
```

Only Level 3 supports broad “beats allocator X” language.

At least one C allocator and one C++ allocator integration are required before
generalizing across implementation languages.

## 12. Anti-cheating checklist

A result is invalid when:

- the compiler removes or constant-folds lookups;
- one structure returns a pointer while another returns richer fields;
- checked and unchecked APIs share one label;
- one dataset accidentally makes all branches predictable;
- all addresses fit in a tiny cache despite a large claimed managed range;
- initialization or mutation cost is silently moved outside the reported cost
  while the comparison includes it elsewhere;
- sparse directory memory is omitted;
- QEMU or another emulator is used for native performance claims;
- only the best run is reported;
- benchmark order or frequency scaling systematically favors one structure;
- failed correctness verification is ignored.

The checksum is necessary but not sufficient. Assembly inspection and
cross-structure verification remain required.
