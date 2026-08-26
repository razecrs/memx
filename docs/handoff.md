# MemX Handoff Checklist

This file is a compact handoff for someone opening the project after the
initial design discussion.

## What exists

The library maps pointers to allocator-defined granule handles.

The current implementation is portable C17.

The default configuration uses bounded coverage, balanced policy, 2 MiB
regions, and 4 KiB granules.

The public result is one native word.

`MEMX_HANDLE_INVALID` is the not-found sentinel.

The sentinel cannot be inserted.

Handle zero remains valid.

The v0 representations are EMPTY, UNIFORM, and DENSE.

Bounded and sparse directories are available.

A fully dense bounded index can be converted to a flat table.

The core index is not thread-safe.

An optional Linux/Android `memx_heap` experiment implements allocator stages
1 through 8. It is not a production replacement or `malloc` interposer.

There is no reclamation implementation.

There is no assembly backend.

## Where to start reading

Read `include/memx/memx.h` for the public contract.

Read `src/memx.c` for the reference implementation.

Read `tests/test_memx.c` for focused API examples.

Read `tests/test_exhaustive.c` for the independent array oracle.

Read `tests/test_faults.c` for allocation failure guarantees.

Read `include/memx/allocator.h`, `src/allocator/heap.c`, and
`docs/allocator.md` for the separate experimental heap and its concurrency
boundary.

Read `bench/baselines.c` for comparable controls.

Read `bench/memx_bench.c` for the equal-semantics harness.

Read `bench/trace.c` for trace parsing and lifecycle validation.

Read `docs/gate1-status.md` before interpreting benchmark numbers.

## Build

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

The default build enables tests and benchmarks.

Warnings are errors for the library and new test targets.

## Verification order

Run focused tests first when editing the core.

Run the complete CTest suite before a benchmark.

Run the Python tool test after editing analysis code.

Run Clang sanitizers after editing allocation or descriptor code.

Run the fuzz target after changing parser or range arithmetic.

Generate and replay a trace after changing trace semantics.

Only then run a 15-repetition benchmark sweep.

## Metrics

The source metric deliberately excludes `research` and build directories.

Use `tools/source_metrics.py source .` for the first-party line count.

Use `tools/source_metrics.py binary PATH` for binary sections and symbols.

Use `tools/pareto.py` to classify latency/metadata frontiers per case.

Use `tools/check_gate.py` to apply the explicit Gate 1 equations.

Never select only the best repetition.

Never omit directory bytes.

Never mix checked and unchecked lookup labels.

Never call a Level 0 result an allocator comparison.

## Review questions

Does every representation resolve the same handle as the flat oracle?

Can a failed mutation change any visible lookup result?

Can a custom allocator observe an invalid or repeated free?

Can a boundary address escape the configured range?

Can a large valid handle lose high bits?

Does a statistic include every allocated metadata object?

Does a benchmark verify checksums before timing?

Does a trace distinguish structural validity from lifecycle validity?

Does an optimization have a measured workload that needs it?

Does an ISA path justify its code and instruction-cache footprint?

## Next implementation gate

The next gate is not “add every planned representation.”

The next gate is a credible Pareto plot across controlled entropy.

The required axes are lookup latency and metadata bytes per managed MiB.

Uniform, mixed, and random cases must remain separate.

At least 15 independent repetitions are required for a serious result.

The host, compiler, flags, and CPU placement must be recorded.

Hardware counters should be collected when available.

Real allocator traces are required before production claims.

## Stop conditions

Stop if the low-entropy gate fails consistently.

Stop if real traces do not contain useful regularity.

Stop if directory costs erase representation savings.

Stop if conversion costs dominate the allocator workload.

Stop if the radix baseline dominates every MemX point.

Do not hide a failure by changing the baseline.

Do not hide a failure by excluding sparse directory memory.

Do not hide a failure by adding a representation without a trace family.

## Final reminder

MemX is a research direction with a functioning Gate 1 prototype.

The honest output can be a win, a conditional win, or a well-explained loss.

All three are more valuable than an unsupported “faster allocator” claim.

The first question for every new optimization remains:

```text
What measured problem does this solve?
```

The second remains:

```text
What independent test would catch it being wrong?
```

Keep both answers next to the change.
