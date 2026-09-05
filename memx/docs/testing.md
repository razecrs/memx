# MemX v0 Testing Strategy

## Purpose

MemX sits between raw addresses and allocator metadata.

A wrong answer is more dangerous than a clean miss.

The test strategy therefore treats lookup equivalence as the primary property.

Performance gates run only after correctness gates pass.

The portable C implementation is the semantic authority for future backends.

No architecture-specific path may redefine an edge case.

## Test layers

The v0 suite has six executable layers:

1. focused API tests;
2. baseline data-structure tests;
3. trace parser and lifecycle tests;
4. exhaustive small-region tests;
5. allocation-failure tests;
6. Python analysis-tool tests.

The optional fuzz target forms a seventh, continuous layer.

Each layer answers a different question.

Passing one does not substitute for passing another.

## Focused API suite

`tests/test_memx.c` exercises public API behavior.

It covers default configuration.

It covers bounded configuration validation.

It covers sparse configuration validation.

It covers empty lookup.

It covers full-region UNIFORM insertion.

It covers partial-region DENSE insertion.

It verifies that handle zero is valid.

It verifies that `MEMX_HANDLE_INVALID` is rejected on mutation.

It verifies that large handles retain every non-reserved bit.

It verifies overlap rejection.

It verifies expected-handle removal.

It verifies aligned-range requirements.

It verifies cross-region mutation.

It verifies DENSE-to-UNIFORM reclassification.

It verifies partial removal from a UNIFORM region.

It verifies sparse directory lookup.

It verifies dense global flat optimization.

It verifies the checked and assumed-mapped views.

It verifies simple custom allocator failures.

It performs randomized bounded-versus-sparse differential testing.

## Baseline suite

`tests/test_baselines.c` verifies benchmark controls.

The controls are not trusted merely because they are simple.

The flat baseline is checked against a truth array.

The two-level baseline is checked against the same truth array.

The radix baseline is checked against the same truth array.

The cached radix baseline is checked against the same truth array.

Every supported byte offset inside a granule must return the same handle.

Empty entries must return `MEMX_HANDLE_INVALID`.

Metadata byte counts must include roots, leaves, and caches.

The cached radix test explicitly checks cache invalidation behavior.

This caught a real v0 bug:

- a missing leaf was cached;
- the leaf was later created;
- the stale null cache entry still caused a miss.

The fix avoids caching missing leaves.

This history is retained because benchmark baselines can contain bugs too.

## Exhaustive small-region suite

`tests/test_exhaustive.c` changes scale, not semantics.

It uses 32 KiB regions.

It uses 4 KiB granules.

Each region therefore contains eight granules.

Three values are considered for each granule:

- EMPTY;
- handle zero;
- a large non-inline handle.

That produces:

```text
3^8 = 6,561 layouts
```

Every layout is constructed from contiguous runs.

Every layout is compared with an independent flat-array oracle.

Five byte positions in every granule are sampled.

The positions include both boundaries.

Every populated run is then removed.

The empty result is verified again.

This enumeration exercises:

- empty descriptors;
- inline uniform descriptors;
- dense descriptors;
- zero handles;
- large handles;
- alternating values;
- long runs;
- isolated holes;
- reclassification after insertion;
- reclassification after removal.

The suite also performs 40,000 randomized operations in bounded mode.

It repeats 40,000 operations in sparse mode.

The oracle decides the expected mutation status.

MemX must match both the status and subsequent lookup state.

Failed operations are therefore checked for non-destructive behavior.

## Fault-injection suite

`tests/test_faults.c` supplies a tracking allocator.

The allocator records every allocation.

It records every deallocation.

It tracks live allocation count.

It tracks live requested bytes.

It detects unknown frees.

It detects double frees.

It can fail one selected allocation call.

Create is tested with failure at each required allocation.

Cross-region insertion is tested at each secondary allocation.

Partial removal from UNIFORM regions is tested at each conversion allocation.

Flat optimization is tested when its destination allocation fails.

Sparse leaf creation is tested independently from dense-table creation.

Overlap rejection is checked to allocate nothing.

Wrong-handle removal is checked to allocate nothing.

Every scenario destroys the index afterward.

Every scenario must end with zero tracked live allocations.

### Mutation failure guarantee

The public semantic guarantee is:

> A failed mutation does not change any lookup result.

Representation shape may change after some failures.

For example, an empty region may retain an allocated all-invalid DENSE table.

That is permitted because lookup behavior is unchanged.

This distinction avoids pretending the implementation offers rollback of
internal allocation decisions.

It still provides the guarantee callers need.

### Partial UNIFORM removal

Partial removal from a UNIFORM descriptor requires a DENSE table.

If a removal crosses several such regions, allocation happens first.

No handle is cleared until every necessary conversion succeeds.

An allocation failure may leave earlier regions converted to DENSE.

All of their entries still carry the original handle.

The subsequent lookup state is therefore identical.

The fault suite locks this behavior down.

## Trace tests

`tests/test_trace.c` separates syntax from lifecycle semantics.

Structural validation checks:

- trace version;
- shift relationships;
- strictly increasing sequence numbers;
- known operation values;
- aligned ranges;
- nonzero range sizes;
- address overflow;
- handle restrictions;
- lookup field restrictions;
- quiescent field restrictions.

Lifecycle validation additionally checks:

- INSERT does not overlap a live mapping;
- LOOKUP reports the current expected handle;
- RETIRE refers to an active matching range;
- RETIRE is not repeated for the same granule;
- REMOVE refers to a live matching range;
- removed addresses may be reused later;
- retired addresses remain lookup-visible until removal.

Round-trip serialization is tested through a temporary file.

Malformed magic, versions, operations, and alignment are rejected.

Statistics are calculated only for a lifecycle-valid trace.

This prevents invalid REMOVE events from hiding through byte-count saturation.

## Tool tests

`tests/test_tools.py` uses the standard library `unittest` module.

It tests percentile interpolation.

It tests median absolute deviation.

It tests deterministic bootstrap intervals.

It tests checksum disagreement detection.

It tests Gate 1 inclusive boundaries.

It tests missing gate structures.

It tests strict Pareto dominance.

It tests equal Pareto points.

It tests duplicate structure rejection.

It tests CSV report generation.

It tests Markdown report generation.

It tests source-tree exclusion rules.

It tests physical and nonblank line counting.

It runs the source metrics CLI and parses its JSON.

## Fuzzing

The Clang libFuzzer target is `memx_fuzz`.

Configure it with:

```bash
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DMEMX_BUILD_TESTS=OFF \
  -DMEMX_BUILD_BENCHMARKS=OFF \
  -DMEMX_BUILD_FUZZERS=ON
```

Build it with:

```bash
cmake --build build-fuzz
```

Run a bounded smoke campaign with:

```bash
./build-fuzz/memx_fuzz -runs=100000
```

A crash artifact must be retained.

The exact compiler version and flags must be retained.

A fixed `-seed` should be recorded for reproducible campaigns.

Corpus inputs should remain small enough for review.

Fuzzing is not a replacement for exhaustive testing.

The exhaustive suite proves coverage over a deliberately tiny state space.

Fuzzing explores larger shapes and mutation sequences.

## Sanitizers

The supported CMake switches are:

```text
MEMX_ENABLE_ASAN
MEMX_ENABLE_UBSAN
```

The recommended Clang configuration is:

```bash
cmake -S . -B build-sanitize -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMEMX_ENABLE_ASAN=ON \
  -DMEMX_ENABLE_UBSAN=ON
```

Then run:

```bash
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

ASan checks accessible memory safety and leaks.

UBSan checks executed undefined behavior.

Neither proves absence of all memory bugs.

Sanitizer runtime availability is a host prerequisite.

## Compiler matrix

Gate 1 requires at least:

- GCC release build;
- GCC warning-clean debug build;
- Clang release build;
- Clang ASan plus UBSan build.

When a 32-bit toolchain exists, add:

- 32-bit compile;
- 32-bit focused tests;
- 32-bit exhaustive tests.

Compilation alone is not architecture support.

At least one correctness run is required on the claimed target ABI.

`MEMX_STRICT_WARNINGS` defaults to `ON` and applies to the library, every test,
every benchmark, trace tooling, and the fuzzer. `MEMX_WARNINGS_AS_ERRORS`
defaults to `ON`; disabling it is a local diagnostic convenience and cannot
produce release evidence.

The Clang profile is `-Weverything -Werror`. Its exclusions are limited to:

- C's standard implicit `void *` allocation conversion (Clang 22+ warning);
- pre-C11 compatibility for a C17 project;
- the experimental raw-array bounds migration warning;
- ABI padding reports;
- exhaustive-switch default labels that deliberately diagnose corrupt/future
  enum values.

The GCC profile enables conversion/sign conversion, strict cast alignment and
qualification, prototypes, format level 2, undefined macro use, VLA/alloca,
enum completeness, pointer arithmetic, null dereference, duplicated/logical
conditions, and related warnings. `MEMX_ENABLE_GCC_ANALYZER=ON` adds
`-fanalyzer` as a separate required audit.

## Boundary matrix

Every public range mutation needs cases for:

| Dimension | Cases |
|---|---|
| base alignment | aligned, one byte low, one byte high |
| size | zero, one granule, one region, several regions |
| end | in range, exactly at bound, one granule beyond |
| arithmetic | ordinary, maximum non-overflowing, overflow |
| handle | zero, inline maximum, large, invalid sentinel |
| occupancy | empty, partial, full, overlapping |
| representation | EMPTY, UNIFORM, DENSE, OVERLAY, FLAT |
| directory | BOUNDED, SPARSE |

The matrix is a review tool.

Not every Cartesian product requires a handwritten case.

Exhaustive and randomized tests cover many combinations automatically.

## Invariant checks

Tests indirectly enforce these invariants:

1. every address maps to at most one handle;
2. all bytes in a granule map identically;
3. failed overlap leaves the prior mapping visible;
4. failed wrong-handle removal leaves the prior mapping visible;
5. representation changes preserve lookup equivalence;
6. zero is a normal handle;
7. only `MEMX_HANDLE_INVALID` means not found;
8. out-of-range checked lookup returns invalid;
9. sparse unallocated leaves behave as empty;
10. flat optimization preserves every entry;
11. custom allocations are paired with custom deallocations;
12. reported total bytes equal directory plus representation bytes.
13. overlay conversion preserves every mapped lookup;
14. overlay conversion releases allocator-owned dense tables exactly once;
15. frozen indexes reject mutation and old view acquisition;
16. zero-valued UNIFORM and full-width DENSE handles survive overlay lookup;
17. reserved virtual bytes are reported separately from private metadata pages.
18. formal gate rows use one page-accounted backing model for MemX and every
    baseline; diagnostic `total_bytes` never enters the equal-category gate.

Future concurrent code needs additional invariants.

Those are intentionally outside Gate 1.

## Test determinism

Randomized tests use explicit nonzero seeds.

The generator is xorshift64*.

No test uses process time as its default seed.

A failure should therefore reproduce without logging hidden state.

Changing a seed is allowed only as an additive coverage change.

Replacing a seed that exposes a bug is not allowed.

## Failure triage

When a test fails:

1. retain the exact command;
2. retain compiler and build type;
3. identify the first failing operation;
4. reduce the operation sequence;
5. compare checked lookup with the flat oracle;
6. inspect representation type and stats;
7. rerun under ASan and UBSan;
8. add the reduced case as a regression test;
9. fix the implementation or the incorrect test premise;
10. run the complete matrix again.

The test premise is not automatically trusted.

The implementation is not automatically trusted.

The oracle is not automatically trusted.

Agreement across independent mechanisms is the target.

## Performance test ordering

Correctness verification occurs before timing.

Every structure is populated from one truth array.

Every granule is checked before queries are generated.

All structures must produce one checksum.

If checksums disagree, the run is invalid.

Gate evaluation reads only completed summary files.

Pareto classification occurs within each workload case.

No failed correctness run contributes a performance point.

## Required pre-merge command set

For a normal Gate 1 change:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

For changes touching allocation or descriptor code, also run sanitizers.

For changes touching parser code, run the trace generator and replay tool.

For changes touching benchmark tools, run `memx_tool_test`.

For changes touching hot lookup, run a smoke benchmark and inspect assembly.

For a strict compiler audit, run both compiler families and the analyzer:

```bash
cmake -S . -B build-strict-gcc -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build-strict-gcc -j
ctest --test-dir build-strict-gcc --output-on-failure

cmake -S . -B build-strict-clang -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build-strict-clang -j
ctest --test-dir build-strict-clang --output-on-failure

cmake -S . -B build-analyzer -G Ninja -DCMAKE_C_COMPILER=gcc \
  -DMEMX_ENABLE_GCC_ANALYZER=ON
cmake --build build-analyzer -j
ctest --test-dir build-analyzer --output-on-failure
```

Overlay changes additionally require:

- EMPTY, zero-UNIFORM, partial-DENSE, and invalid-tag regression cases;
- randomized differential lookup before and after freezing;
- fault-allocator accounting through conversion and destruction;
- Linux ASan/UBSan plus fuzzing;
- Android link validation of the real overlay implementation;
- non-Linux compile/link validation of the `UNSUPPORTED` stub;
- no emulated performance claims.

Allocator changes additionally require:

- strict GCC and Clang host builds with the complete allocator test suite;
- ASan/UBSan and a separate Clang ThreadSanitizer run;
- a bounded `memx_heap_fuzz` campaign;
- native 32-bit x86 execution where the multilib runtime is available;
- full Android NDK link validation for all four supported ABIs;
- cross-target strict compilation/linking for the documented Linux matrix;
- explicit remote-free, detached-owner, invalid-free, overflow, alignment, and
  randomized payload-preservation tests;
- a benchmark checksum match before timing numbers are considered usable.

The allocator tests do not authorize Gate 9. Heap destruction remains
externally synchronized, and MemX publication/reclamation is not concurrent.

## Exit criteria

Gate 1 correctness is ready for measurement when:

- all seven host suites pass when `memx_heap` is enabled;
- sanitizer suites pass on an available runtime;
- a bounded fuzz campaign completes without a finding;
- generated traces pass structural and lifecycle validation;
- all baseline checksums agree;
- source and binary metrics can be generated;
- no known correctness finding remains open.

This does not make MemX production-ready.

It makes the experiment credible enough to measure.
