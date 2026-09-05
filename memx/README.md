# MemX

MemX is an experimental C library for mapping an address to
allocator-defined granule or span metadata. It investigates whether local
regularity in allocator metadata can reduce resident metadata without giving up
near-direct-map lookup performance.

MemX is not a malloc replacement. Its v0 contract is:

```text
address -> allocator-defined granule/span handle
```

If several objects share one indexed granule, the returned handle identifies
the allocator metadata needed to derive the individual object.

## Current milestone

Gate 1 implements:

- portable C17 code that is also valid C23;
- native-word opaque handles with one reserved invalid value;
- `EMPTY`, `UNIFORM`, and `DENSE` region representations;
- an explicit immutable VM-backed dense overlay for bounded Linux/Android
  indexes;
- bounded and sparse two-level directories;
- checked lookup and a bounded proven-valid hot path;
- aligned range insertion and removal;
- custom metadata allocation callbacks;
- exact metadata-footprint counters;
- flat, two-level, radix, and cached-radix reference baselines;
- deterministic synthetic benchmark workloads;
- unit and randomized differential tests.

Concurrency, reclamation, `RUN`, compressed `SPARSE` regions, ISA-specific
code, and language bindings remain gated. They are not hidden inside v0.

An optional Linux/Android experimental allocator layer now implements the
first eight allocator stages: OS-backed arenas, spans, size classes, small and
large allocation, `calloc`/`realloc`/alignment, thread caches, and synchronized
remote frees. Concurrent MemX publication/reclamation is deliberately not
included. See [Experimental MemX heap](docs/allocator.md).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Strict warnings are enabled by default for every C target. GCC uses the full
conversion, sign, cast, prototype, format, enum, VLA, and portability profile;
Clang uses `-Weverything -Werror` with only documented C17/raw-array and ABI
padding exclusions. GCC's interprocedural analyzer is an explicit extra pass:

```bash
cmake -S . -B build-analyzer -G Ninja \
  -DCMAKE_C_COMPILER=gcc \
  -DMEMX_ENABLE_GCC_ANALYZER=ON
cmake --build build-analyzer -j
ctest --test-dir build-analyzer --output-on-failure
```

Debug builds use the same warnings, including warnings-as-errors by default:

```sh
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
```

## Minimal example

```c
#include <memx/memx.h>

#include <stdint.h>

int main(void) {
    memx_config_t config = memx_config_default();
    memx_index_t *index = NULL;

    config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    config.base_address = UINTPTR_C(0x100000000);
    config.managed_size = 64U * 1024U * 1024U;

    if (memx_index_create(&config, &index) != MEMX_OK) {
        return 1;
    }

    if (memx_insert(index, config.base_address, 4096U, 7U) != MEMX_OK) {
        memx_index_destroy(index);
        return 1;
    }

    memx_handle_t handle = memx_lookup_address(index, config.base_address);
    memx_index_destroy(index);
    return handle == 7U ? 0 : 1;
}
```

`MEMX_HANDLE_INVALID` is the only not-found value. Zero is a valid handle.

## Lookup APIs

`memx_lookup_address` is checked. It validates the configured address range and
selects the configured directory.

Bounded indexes can export a `memx_bounded_view_t`. The inline
`memx_bounded_lookup_assume_mapped` path is for allocator internals that already
proved the address belongs to the configured range. Passing an out-of-range
address to that function violates its precondition.

Both API forms are reported separately by the benchmark; an unchecked result
must never be presented as checked-lookup performance.

A bounded index may be frozen with `memx_index_optimize_overlay`. On supported
Linux/Android targets, dense entries then live at direct global offsets in a
demand-zero virtual mapping while uniform handles remain inline. The resulting
`memx_overlay_lookup_trusted` path is branchless, but requires an in-range,
currently mapped address and an uncorrupted live view. Frozen overlay indexes
reject further mutation.

## Benchmark

Large-allocation index scaling can be measured separately:

```sh
./build-current/memx_large_heap_bench 8192 100000
```

The arguments are live 16-KiB allocations and size-query count. JSON output
separates allocation (including payload touches), successful size lookup,
interior-pointer rejection, in-place shrink, and shuffled free timings. This
compares versions of MemX's checked large-allocation path, not complete
production allocators. See [the improvement report](bench/results/large-index-2026-09-05.json)
for repeated before/after measurements and limitations.

Run a release build:

```sh
./build/memx_bench \
    --regions 256 \
    --lookups 10000000 \
    --workload mixed \
    --dense-percent 25
```

The output is CSV with nanoseconds per lookup, conservative page-accounted
backing bytes, bytes per managed MiB, and a checksum that prevents dead-code
elimination. Actual RSS and reserved virtual address space are reported
separately where available.

Available workloads:

- `uniform`: every region has one repeated handle;
- `mixed`: a configurable fraction of regions contain random handles;
- `random`: every region is dense and irregular.

The benchmark verifies every populated granule against every structure before
timing. It still remains a microbenchmark and does not establish allocator
superiority.

## Trace generation and replay

Generate a deterministic allocator-shaped lifecycle trace:

```sh
python3 tools/generate_trace.py /tmp/memx.trace --events 100000
./build/memx_trace_replay /tmp/memx.trace
```

The v1 trace records insertions, lookups, retirements, quiescent points, and
removals. Gate 1 replay treats retirement and quiescence as recorded lifetime
events while leaving the mapping intact until removal. The format is specified
and validated by `bench/trace.h` and `bench/trace.c`.

## Gate 1 success criterion

For Gate 1 low-entropy synthetic layouts, `BALANCED` must use at least 50% less
page-accounted backing than a flat map while staying within 10% of flat lookup
latency. Fully dense layouts must stay within 15% of flat latency and 2% of
flat page-accounted backing. This is a conservative equal-category model, not
an RSS claim; allocator traces and measured residency are higher evidence
levels.

Failure triggers redesign. It is not permission to add assembly or more region
types.

## Documents

- [Original research brief](MEMX.md)
- [Competitor source analysis](COMPETITOR_ANALYSIS.md)
- [Algorithm and representation specification](docs/algorithm.md)
- [Experimental allocator design and concurrency boundary](docs/allocator.md)
- [API and integration contract](docs/api.md)
- [Benchmark methodology](docs/benchmark.md)
- [Math and representation research](docs/math-and-representation-research.md)
- [Correctness invariants](docs/invariants.md)
- [Validated compiler and ABI matrix](docs/toolchain-matrix.md)
- [Gated roadmap](docs/roadmap.md)
- [First equal-category Gate 1 evidence](bench/results/gate1-page-accounted-2026-08-26/README.md)

## Research posture

MemX makes no novelty or performance claim yet. Results must name the baseline,
semantics, workload, compiler, build flags, hardware, and metadata accounting
method. An optimization remains only when measurement justifies its complexity.
