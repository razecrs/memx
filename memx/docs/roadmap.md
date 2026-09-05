# MemX Gated Roadmap

## Gate 0: Baseline truth

Status: passed locally.

Deliver one reproducible address-to-native-handle contract across flat,
two-level, radix, and cached-radix structures.

Required outputs:

- deterministic datasets;
- cross-structure verification;
- exact requested-byte formulas;
- checked versus proven-valid labeling;
- release-build timing;
- raw CSV output.

Exit only when every baseline passes unit tests, randomized differential tests,
sanitizers, and compiler warnings.

## Gate 1: EMPTY, UNIFORM, DENSE

Status: passed locally with equal page-accounted backing bytes on the first
x86-64 host. Multi-host and real-trace validation remain open.

Deliver bounded and sparse MemX directories with static representation choice.

The primary hypothesis is tested on region entropy sweeps and allocator-shaped
traces:

```text
low entropy:
    lookup <= 1.10 * flat
    metadata <= 0.50 * flat

fully dense:
    lookup <= 1.15 * flat
    metadata <= 1.02 * flat
```

If the first condition fails, inspect directory and dispatch cost before adding
representations. If the dense condition fails, simplify descriptor decoding or
accept that the current design is not near-flat.

Gate 1 has no concurrency, assembly, bindings, or allocator replacement.

The original mixed tagged-pointer path missed the latency target because its
representation branch became unpredictable. The accepted Gate 1 candidate is
an explicit frozen VM overlay for low/mixed entropy and the direct flat
fallback for fully dense layouts. The original heterogeneous-byte run remains
preserved and invalidated. A replacement 15-repetition pinned run uses one
page-accounted backing model for every structure and passes both gate
equations. Gate 2 trace machinery may proceed. The result does not freeze the
overlay/flat policy threshold or establish a production claim.

## Gate 2: Real trace evidence

Status: trace tooling exists; production collection waits alongside the Gate 1
accounting correction.

Define a versioned trace format and collect metadata-equivalence traces from at
least one C and one C++ allocator.

Required trace families include small-object slabs, large extents, mixed size
classes, fragmentation, cross-thread free patterns, and long-lived sparse
ranges.

Replay must report lookup distribution, steady and peak metadata, mutation
cost, and per-region entropy. Synthetic and trace results are shown together,
not substituted for each other.

If real traces do not contain enough uniformity to pass the Gate 1 target,
redesign region selection before proceeding.

## Gate 3: RUN

Add a run representation only after measuring repeated contiguous handles that
cross the uniform/dense break-even point.

Candidate lookup mechanisms include linear scan for very small run counts,
binary search, branchless boundary comparison, and SIMD comparison where
portable compilers produce it.

Retain RUN only if it adds a Pareto-optimal point on a named trace family.
Selection must include descriptor, boundaries, handles, allocation overhead,
and mutation rebuild cost.

## Gate 4: SPARSE region representation

This gate concerns a sparse representation inside a region, not the sparse
top-level directory.

Evaluate sorted exceptions, bitmap plus rank, compact index vectors, and tiny
open-addressed hashes. Each candidate uses the same default-handle and
exception semantics.

Remove candidates that never win. MemX is not a museum of clever data
structures.

## Gate 5: Policy calibration

Calibrate predicted lookup cost for each retained representation on x86-64 and
AArch64 real hardware.

Implement constraints:

```text
FAST     minimize latency under memory ceiling
BALANCED minimize memory under 1.10x flat latency
COMPACT  minimize memory while faster than radix
```

Expert weights remain optional overrides. Runtime auto-tuning is excluded until
static calibrated policies prove insufficient on changing real workloads.

## Gate 6: Concurrent guarded readers

Introduce explicit reader contexts and replaceable lifetime-domain operations.

Bundle region-granular epoch/QSBR as the default. Readers enter once, perform
one or many bounded lookups, and exit or announce quiescence.

Writers create immutable replacements and publish one region at a time. Retired
bytes are bounded by configuration. At the ceiling, nonblocking mutation
returns `BUSY`; an explicit wrapper waits and retries.

Concurrency exits only after stress tests, TSan, stalled-reader tests, peak
amplification checks, and a written memory-order proof.

## Gate 7: Standard records and lifecycle

Keep raw lookup returning one native-word handle.

Add an optional standard record store containing owner, size class, lifecycle
state, generation, and caller payload. Custom integrations may resolve handles
themselves.

Measure raw lookup and record resolution separately. Do not hide the second
dependent load in a baseline comparison.

Lifecycle transitions follow ACTIVE, RETIRED, RECLAIMABLE. MemX coordinates
index visibility but does not become a garbage collector.

## Gate 8: Layout hints and hardening

Add explicit validated and trusted hints for bounded range, alignment,
repetition, segment derivation, and tag normalization.

Validated hints are checked during configuration and mutation where practical.
Trusted hints omit checks on hot paths; contract violation is caller error.

Add checked pointer normalization, integer overflow defense, descriptor
validation, corruption diagnostics, optional metadata-address randomization,
and architecture tag policies.

## Gate 9: Architecture optimization

Tier 1 performance targets are x86-64 and AArch64.

For each measured hotspot:

1. inspect compiler output;
2. improve portable data layout or C when possible;
3. try compiler builtins/intrinsics;
4. add assembly only when the compiler cannot express the measured win;
5. differential-test against portable C;
6. measure latency, `.text` bytes, instructions, and I-cache misses.

Keep a specialization only for a reproducible 3–5% meaningful improvement with
justified code footprint.

Portable correctness targets then expand through x86, ARM32, RV32/RV64,
MIPS32/64, and PPC32/64, including big-endian builds. QEMU validates behavior,
never native performance.

## Gate 10: Allocator integration

Integrate MemX into at least one production-quality C allocator experiment and
one C++ allocator experiment.

Compare application throughput, tail latency, metadata RSS, total RSS,
fragmentation, remote frees, and initialization/mutation cost.

A subsystem lookup win accompanied by an allocator regression is not a product
win. Integration results determine whether hints, granularity, and record shape
are practical.

## Gate 11: NUMA and deployment

Only after single-node success, evaluate per-node directories, region affinity,
reader-local roots, and replicated read-mostly metadata.

Measure remote memory traffic and replication bytes. Do not add NUMA structure
based solely on topology theory.

Stabilize the C ABI after representation-independent semantics, concurrency,
and allocator integration are proven. Bindings follow ABI stability rather than
driving core design.

## Permanent kill rules

- No representation without a measured winning layout.
- No assembly without a profiler-identified hotspot.
- No performance claim without equal semantics.
- No architecture support claim without executed correctness tests.
- No allocator superiority claim from a standalone microbenchmark.
- No unbounded retired metadata to protect writer throughput.
- No whole-index snapshot solely to make read-side reasoning easier.
- No feature work used to distract from a failed earlier gate.
