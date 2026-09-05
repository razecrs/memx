# Gate 1 Status and Evidence

## Scope freeze

Gate 1 implements an address-to-granule-metadata experiment.

Included:

- portable C library;
- native-word handles;
- bounded directory;
- sparse two-level directory;
- EMPTY representation;
- UNIFORM representation;
- DENSE representation;
- fully dense flat fallback;
- checked flat baseline;
- proven-in-range flat baseline;
- two-level baseline;
- radix baseline;
- cached radix baseline;
- synthetic workloads;
- lifecycle trace parser;
- trace generator;
- trace replay;
- reproducible sweep runner;
- gate checker;
- Pareto reporter;
- source and binary metrics;
- unit, differential, exhaustive, fault, and tool tests;
- optional fuzz target.

Excluded:

- allocator replacement;
- concurrent mutation;
- lock-free publication;
- reclamation implementation;
- SPARSE within-region representation;
- RUN representation;
- dynamic adaptation policy;
- layout-hint optimization;
- assembly;
- intrinsics;
- language bindings;
- non-x86 performance claims;
- production allocator integration.

## Implemented public contract

The public handle is one `uintptr_t`.

`UINTPTR_MAX` is the invalid sentinel.

Zero is valid.

Insert and remove operate on granule-aligned ranges.

Checked lookup returns invalid outside configured coverage.

Removal requires the expected handle across the complete range.

Overlap is rejected across the complete range.

Failed mutations preserve visible lookup results.

The library is currently single-threaded for mutation and access.

## Representation behavior

EMPTY is encoded as descriptor zero.

Small UNIFORM handles are encoded inline.

DENSE uses one entry per granule.

A large uniform handle uses DENSE to preserve all handle bits.

DENSE reclassifies to UNIFORM when every entry becomes equal and inlineable.

DENSE reclassifies to EMPTY when every entry becomes invalid.

Partial removal from UNIFORM converts to DENSE.

Whole UNIFORM removal clears the descriptor directly.

## Directory behavior

BOUNDED stores a contiguous descriptor array.

Its checked lookup validates the configured range.

Its assumed-mapped view skips that validation.

SPARSE splits region-index bits between a root and leaves.

Leaves allocate lazily.

An absent leaf resolves to invalid.

The current sparse directory supports at most 32 region-index bits.

Every allocated directory byte appears in stats.

## Dense fallback behavior

`memx_index_optimize` requires a bounded index.

It requires every region to be physically DENSE.

It allocates one direct entry array.

It copies every handle.

It frees old dense tables and descriptors only after copying succeeds.

Allocation failure preserves adaptive mode.

Calling optimize again in flat mode succeeds without work.

Insert and remove continue to work in flat mode.

The bounded adaptive view becomes unavailable.

The flat assumed-mapped view becomes available.

## Frozen overlay behavior

`memx_index_optimize_overlay` explicitly freezes a bounded adaptive index on
Linux/Android. It reserves a guarded direct overlay, copies only DENSE region
pages, retains EMPTY/UNIFORM descriptors, and releases allocator-owned dense
tables. All fallible work precedes publication. Earlier bounded views become
invalid and mutation returns `MEMX_ERROR_BUSY` after conversion.

`memx_overlay_lookup_trusted` performs a branchless descriptor/direct-entry
selection for an address the allocator has already proven mapped. The checked
generic lookup remains separate and must not be reported as this fast path.

Statistics count page-rounded private overlay representation bytes and report
the full guarded virtual reservation separately. Kernel page-table bytes are
not yet attributed per index.

## Baseline semantics

Every baseline stores the same `memx_handle_t` values.

Every baseline uses the same granularity.

Every baseline uses the same managed coverage.

The two-level structure allocates leaves lazily.

The radix structure allocates middle nodes and leaves lazily.

The cached radix adds a direct-mapped leaf cache.

Cache bytes are included in metadata.

Missing leaves are not cached.

## Workloads

Uniform assigns one handle per region.

Mixed selects a configured fraction of dense regions.

Random assigns pseudo-random handles to every granule.

Queries select random granules and byte offsets.

Queries are generated before timing.

Every implementation is verified against the truth array before timing.

Every timed result contributes to a checksum.

Checksum disagreement invalidates a run.

## Gate equations

Low-entropy acceptance requires:

```text
L_memx <= 1.10 * L_flat
M_memx <= 0.50 * M_flat
```

Dense safety requires:

```text
L_memx <= 1.15 * L_flat
M_memx <= 1.02 * M_flat
```

`L` is median time under equal semantics.

For the current formal gate, `M` is conservative
`page_accounted_backing`: every live metadata allocation is rounded
independently to the host page size, while overlay DENSE intervals are counted
by the pages they intersect. Requested bytes, measured RSS, allocator usable
bytes, and reserved virtual address space remain separate diagnostics.

Both dimensions must pass.

## Smoke observations

One historical local Release smoke run was performed on an Intel Core i7-9750H host.

The uniform bounded assumed-mapped path measured about 2.18 ns per lookup.

The checked flat baseline measured about 3.26 ns per lookup in that run.

Uniform MemX metadata measured about 4.3 bytes per managed MiB.

Flat metadata measured 2,048 bytes per managed MiB for both flat paths.

The fully random flat fallback measured about 3.00 ns per lookup.

The checked flat baseline measured about 3.23 ns per lookup in that run.

The flat fallback metadata ratio was about 1.002.

A current 15-repetition, CPU-pinned production-path sweep with three million
lookups per run measured the following medians:

| dense regions | flat assumed-mapped | overlay trusted | latency ratio |
| ---: | ---: | ---: | ---: |
| 0% | 2.9378 ns | 2.2185 ns | 0.755 |
| 10% | 3.0525 ns | 2.3228 ns | 0.761 |
| 25% | 3.0449 ns | 2.7298 ns | 0.897 |
| 50% | 2.9941 ns | 3.0301 ns | 1.012 |
| 75% | 2.9858 ns | 3.3574 ns | 1.124 |
| 100% | 2.8495 ns | 3.3347 ns | 1.170 |

At 25%, overlay metadata was 301,256 bytes versus 1,048,608 bytes for flat
(ratio 0.287). The dense overlay itself occupied 299,008 private bytes and
reserved 1,056,768 virtual bytes including guards. At 100%, the existing flat
fallback remained faster at 2.8148 ns and used 1,048,776 bytes.

These historical numbers are smoke evidence only.

## First candidate local gate outcome

The first preserved 15-repetition run passed both configured numeric checks on
2026-08-26:

| case | candidate | latency ratio | limit | metadata ratio | limit | result |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| uniform, 0% dense | `memx_overlay_trusted` | 0.724415 | 1.10 | 0.002144 | 0.50 | PASS |
| random, 100% dense | `memx_flat_fallback` | 0.983679 | 1.15 | 1.000160 | 1.02 | PASS |

The run used GCC 16.2.1 Release code, 256 regions, 2 MiB regions, 4 KiB
granules, five million lookups per row, 15 repetitions, seed `0x6d656d78`, and
was pinned to logical CPU 2 on an Intel Core i7-9750H. The governor remained
`powersave`, so this is a local engineering gate—not a publication run.

Every raw row, summary, and the captured environment are preserved under
`bench/results/gate1-overlay-2026-08-26`.

An independent audit found that the overlay candidate's byte count combined
requested directory bytes with page-rounded private overlay intervals, while
the flat baseline reported requested allocation bytes. The figures are useful
diagnostics but are not one equal memory category. Consequently this run is no
longer called a formal metadata-gate pass. The next run must compare requested
with requested or resident/page-backed with resident/page-backed and preserve
that memory evidence in the raw artifact.

## Corrected equal-category gate outcome

The replacement run uses `page_accounted_backing` for every structure. Each
live heap allocation is rounded independently to the actual host page size;
the overlay adds its page-rounded DENSE intervals. Reserved VA and measured
overlay RSS/private/shared bytes remain distinct and are now preserved in every
raw row.

| case | candidate | latency ratio | limit | page-backed ratio | limit | result |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| uniform, 0% dense | `memx_overlay_trusted` | 0.753251 | 1.10 | 0.007782 | 0.50 | PASS |
| random, 100% dense | `memx_flat_fallback` | 0.974340 | 1.15 | 1.000000 | 1.02 | PASS |

The configuration matches the candidate run: GCC 16.2.1 Release, 256 regions,
2 MiB/4 KiB geometry, five million lookups, 15 repetitions, CPU 2, and seed
`0x6d656d78` on the i7-9750H host. The raw evidence is under
`bench/results/gate1-page-accounted-2026-08-26`.

This closes the local Gate 1 accounting defect. It does not replace the need
for other physical hosts, controlled frequency policy, or real allocator
traces.

The gate script compares a trusted/assumed-mapped MemX candidate with the
matching `flat_assume_mapped` baseline. A dense `memx_flat_fallback` pass
demonstrates safe graceful degradation to a direct table.

They were not collected with the full publication protocol.

They must not be described as general performance results.

## Interpretation

The uniform result supports continuing the experiment.

It shows that an inline region descriptor can reduce metadata dramatically.

It does not show how often production heaps are uniform.

The dense fallback shows that worst-case entropy can degrade to direct lookup.

It does not compress that workload.

The original tagged-pointer path still reveals the mixed danger zone: at 25%
dense it measured 5.4296 ns. Perf sampling attributed branch misses to its
representation dispatch. The overlay removes that branch and makes the direct
dense address independent of the descriptor load.

Overlay ceases to win at high density. The observed policy boundary is to use
overlay through roughly 50% dense and switch to flat before 75%; real traces
and additional hardware must calibrate that threshold rather than freezing it
from one machine.

## Current claim level

The current claim level is Level 0.

That means synthetic standalone structure results.

Trace machinery exists but no production trace evidence is committed.

No production allocator has substituted MemX for its production index. The
separate experimental `memx_heap` layer does exercise MemX end to end, but is
not evidence about a mature allocator.

No production application workload result exists. The checked-in heap smoke
benchmark is deliberately excluded from Gate 1 superiority claims.

Therefore the project does not claim to beat jemalloc.

It does not claim to beat mimalloc.

It does not claim to beat TCMalloc.

It does not claim to beat snmalloc.

## Correctness evidence

Focused tests cover public behavior.

Baseline tests perform more than 500,000 comparisons.

Exhaustive tests enumerate 6,561 ternary region layouts.

Randomized state machines run in bounded and sparse modes.

Fault tests inject failures at representation allocation boundaries.

Trace tests separate structural and lifecycle validity.

Tool tests verify statistics, gates, Pareto logic, and source metrics.

A Clang ASan plus UBSan run passed before the latest trace additions.

The complete sanitizer suite must be rerun for final Gate 1 evidence.

## Known limitations

The core implementation is one C translation unit.

That is acceptable for the prototype but not necessarily the final layout.

Policies are named but not dynamically calibrated.

Hint trust modes are reserved but not consumed.

Metadata stats count requested bytes, not allocator usable bytes.

RSS and virtual reservation are not reported by the C harness.

The trace replay uses diagnostic per-event timing.

Hardware counters are not automatically collected on this host.

No cold-cache benchmark is implemented.

No mutation-throughput benchmark is implemented.

No 32-bit runtime has been validated.

No big-endian runtime has been validated.

No tagged-pointer normalization exists.

No concurrent safety exists.

## Gate 1 stop conditions

Stop and redesign if low-entropy wins disappear under controlled repetition.

Stop and redesign if realistic traces are mostly in the mixed danger zone.

Stop and redesign if complete directory memory removes the footprint win.

Stop and redesign if mutations dominate allocator cost.

Stop and redesign if the flat fallback needs unacceptable conversion memory.

Do not answer those failures by immediately adding more representations.

Do not answer them by writing assembly.

Do not answer them by changing baseline semantics.

## Evidence still required

Before declaring Gate 1 passed:

1. rerun all tests under Clang ASan and UBSan;
2. run a bounded libFuzzer campaign;
3. run at least 15 independent benchmark repetitions;
4. pin runs to a physical CPU;
5. record governor, turbo, SMT, kernel, compiler, and microcode;
6. collect cycles, instructions, branches, and cache events;
7. produce Pareto reports for every entropy step;
8. generate and replay several lifecycle traces;
9. collect at least one real allocator trace family;
10. retain raw results and failures.

## Next decision after Gate 1

If real layouts cluster near UNIFORM, refine selection and publication.

If layouts contain long repeated ranges, evaluate RUN.

If layouts contain isolated exceptions, evaluate within-region SPARSE.

If layouts are high entropy, prefer direct or allocator-derived lookup.

Only add a representation for an observed layout family.

Only retain it if its Pareto region is measurable.

## Reproducible commands

Build and test:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

Smoke benchmark:

```bash
./build-release/memx_bench \
  --regions 256 \
  --lookups 10000000 \
  --workload uniform
```

Sweep:

```bash
python3 tools/run_benchmarks.py \
  --executable build-release/memx_bench \
  --repetitions 15 \
  --regions 256 \
  --lookups 5000000 \
  --workloads uniform,random \
  --cpu 2 \
  --output bench/results/gate1-page-accounted-2026-08-26
```

Gate check:

```bash
python3 tools/check_gate.py \
  bench/results/gate1-page-accounted-2026-08-26/summary.csv \
  --low-entropy-candidate memx_overlay_trusted \
  --dense-candidate memx_flat_fallback
```

Pareto report:

```bash
python3 tools/pareto.py \
  bench/results/RUN/summary.csv \
  --csv bench/results/RUN/pareto.csv \
  --markdown bench/results/RUN/pareto.md
```

Source metrics:

```bash
python3 tools/source_metrics.py source .
```

Binary metrics:

```bash
python3 tools/source_metrics.py binary build-release/libmemx.a
```

## Honest summary

Gate 1 has a functioning experiment, not a finished allocator algorithm.

The prototype has found a conditional low-entropy win.

It has also found a mixed-layout loss.

That is useful evidence.

The next work is measurement quality and real traces.

Feature count is not the next metric.
