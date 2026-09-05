# MemX Correctness Invariants

## 1. Mapping invariants

For every index and covered address, lookup returns exactly one handle or the
invalid sentinel.

One granule cannot contain two active mappings in the core index.

All bytes in one granule resolve to the same handle.

Addresses outside configured coverage resolve to invalid through checked
lookup and are forbidden inputs to proven-valid lookup.

The invalid sentinel can never be inserted. Zero and every other native-word
value can be inserted.

## 2. Region equivalence

Every representation has a conceptual dense expansion:

```text
expand(EMPTY)   = INVALID repeated N times
expand(UNIFORM) = handle repeated N times
expand(DENSE)   = stored entries[0..N)
```

Representation changes are correct only when their dense expansions are
identical before and after the change.

The current valid transitions are:

```text
EMPTY   -> UNIFORM
EMPTY   -> DENSE
UNIFORM -> EMPTY
UNIFORM -> DENSE
DENSE   -> EMPTY
DENSE   -> UNIFORM
DENSE   -> DENSE
ADAPTIVE -> OVERLAY (whole bounded index, lookup-equivalent and immutable)
```

There is no representation-specific lookup result.

## 3. Descriptor invariants

Zero is the unique empty descriptor.

An inline uniform descriptor has tag one and shifts back to the original
handle without information loss.

A dense descriptor has tag two. Clearing the low two bits yields the exact base
address returned by the metadata allocator.

No published descriptor uses tag three.

Every dense descriptor points to an allocation containing exactly
`granules_per_region` initialized handles.

In adaptive mode, every dense descriptor points to a live table. In overlay
mode, tag two is a representation marker and is never dereferenced as a
pointer. No destroy or statistics path may decode an overlay marker as an
allocator-owned address.

The descriptor array remains alive until destruction. Adaptive dense tables
remain alive until successful overlay conversion or destruction.

## 4. Overlay invariants

Overlay optimization applies only to bounded adaptive indexes on a supported
platform. Failure leaves the original index and all lookups unchanged.

Before publication, conversion validates every descriptor, reserves a guarded
mapping, copies every DENSE entry, and computes page-rounded private intervals.
EMPTY and UNIFORM overlay slots may remain demand-zero because descriptor
selection never obtains their semantic result from those slots.

After publication:

- lookup expansion is identical to the pre-conversion index;
- mutation returns `MEMX_ERROR_BUSY`;
- a second overlay optimization is idempotent;
- flat optimization returns `MEMX_ERROR_BUSY`;
- old bounded views are invalid and new bounded/flat views are unavailable;
- the overlay view and backing mapping remain live until index destruction;
- `reserved_bytes` includes both inaccessible guard pages;
- representation bytes include only page-rounded intervals intersecting DENSE
  regions, while directory bytes still include all descriptors.

Trusted overlay lookup additionally requires a covered, currently mapped
address and an uncorrupted live view. EMPTY addresses are not valid trusted
inputs. Checked lookup returns invalid for EMPTY and out-of-range addresses.

## 5. Directory invariants

Bounded descriptor index zero corresponds exactly to `base_address`.

Bounded descriptor index `i` covers:

```text
[base + i * region_size, base + (i + 1) * region_size)
```

The final interval endpoint does not wrap `uintptr_t`.

For sparse mode, root and leaf bit partitions together cover every configured
region-index bit exactly once. No bit is ignored or counted twice.

A null sparse leaf is semantically equivalent to a leaf containing only empty
descriptors.

Sparse lookup never allocates or commits metadata.

## 6. Insert invariants

Insertion base and size are granule-aligned and size is nonzero.

Insertion preflight observes invalid in every target granule before any target
handle is written.

After successful insertion, every granule in the half-open input range returns
the requested handle.

Every granule outside the input range has the same conceptual value it had
before insertion.

Failed insertion never exposes the requested handle in a strict subset of the
target range. Retained empty capacity is allowed and included in statistics.

## 7. Remove invariants

Removal preflight observes the expected handle in every target granule before
any target handle is cleared.

After successful removal, every granule in the range returns invalid.

Every granule outside the range retains its conceptual value.

A failed expected-handle check leaves all mappings unchanged.

Allocation needed for partial-uniform removal is completed for all affected
regions before clearing starts. Allocation failure may leave equivalent dense
representations but cannot leave partial removals.

## 8. Statistics invariants

`total_bytes` equals `directory_bytes + representation_bytes` without unsigned
overflow for every constructible index.

`reserved_bytes` is zero outside overlay mode. In overlay mode it is a virtual
address quantity and is deliberately excluded from `total_bytes`.

`page_accounted_bytes` uses one model in every storage mode: independently
page-round each live heap allocation and add page-rounded private overlay
intervals. Guard pages, untouched demand-zero overlay slots, and the full
virtual reservation are excluded. The field is an equal-category benchmark
model, not a claim of exact process RSS.

`uniform_regions` counts inline uniform descriptors.

`dense_regions` counts dense descriptor allocations, including dense tables
whose values happen to be equal but cannot be encoded inline.

For bounded mode:

```text
empty + uniform + dense = region_count
```

For sparse mode, empty counts only descriptors in allocated leaves. Unallocated
coverage is implicit.

Destroy frees every allocated dense table, sparse leaf, root/directory, and the
index object exactly once through the configured deallocator.

## 9. API invariants

Creation sets the output pointer to null before any fallible work.

Creation publishes an index only after all required root metadata is
initialized.

Destroy accepts null.

No API dereferences a null required output parameter.

Status string conversion accepts unknown enum values and returns `"unknown"`.

Public checked operations reject unsigned range overflow before calculating an
end address.

## 10. Benchmark invariants

Every baseline is populated from one truth array.

Every managed granule is compared against that truth before timing begins.

Every timed result contributes to an observable checksum.

Every reported byte count uses the same requested-byte category.

Checked and proven-valid paths have distinct names and result rows.

The benchmark seed, layout parameters, and lookup count are printed with every
run.

## 11. Future concurrency invariants

These are not implemented in Gate 1 but constrain compatible evolution.

Guarded lookup completes in a statically bounded number of steps. It takes no
lock, allocates nothing, helps no writer, and contains no retry loop.

Published region representations are immutable to readers.

Writers replace only affected regions, never the whole index solely for reader
simplicity.

A retired representation remains allocated until no protected reader can hold
its descriptor.

Retired bytes have an explicit ceiling. Readers remain wait-free at the
ceiling; writers receive backpressure.

The reclamation mechanism is replaceable. Epoch/QSBR is a default
implementation of the lifetime contract, not an observable lookup semantic.

Multi-region mutation is consistent per region, not transactional across the
whole range.

No legitimate reader observes partial insertion because callers publish an
allocation pointer only after insertion completes.

Removal follows:

```text
ACTIVE -> RETIRED -> grace period -> per-region removal -> RECLAIMABLE
```

## 12. Proof obligations for new representations

A proposed representation must define:

- its complete dense expansion;
- creation preconditions;
- lookup procedure;
- exact byte formula;
- failure behavior;
- conversion to and from every reachable representation;
- concurrency publication behavior;
- target-specific encoding restrictions;
- exhaustive small-state tests;
- randomized differential tests;
- a workload family where it is Pareto-optimal.

If any item is missing, the representation is not ready for the core library.

## 13. Proof obligations for architecture paths

An ISA-specific lookup accepts the same view and address preconditions as its
portable oracle.

It returns the same handle for every descriptor, including invalid, zero,
maximum inline, full-width dense, and corrupted-tag diagnostic cases.

It cannot assume a virtual-address width, endianness, alignment, or pointer tag
policy not declared by the architecture configuration.

It must survive differential tests on actual target execution. Cross-compiling
alone is not correctness evidence; emulation is correctness evidence but not
performance evidence.

Its speed result is considered with `.text` size, instruction count, and
instruction-cache behavior.
