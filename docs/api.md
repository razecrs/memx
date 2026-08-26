# MemX Gate 1 API Contract

## Handle model

`memx_handle_t` is `uintptr_t`. It is an opaque identity, not necessarily a
pointer. `MEMX_HANDLE_INVALID` reserves `UINTPTR_MAX` as the sole missing value.
Every other bit pattern, including zero and handles too large for inline
descriptor packing, is valid.

On strict-provenance or capability systems, a handle must normally be a stable
integer slot ID. MemX does not promise that converting a metadata capability to
a handle and back preserves authority or provenance.

## Granule semantics

One mapping describes an allocator-defined granule or span. MemX does not claim
that each granule is one allocation.

For example, if a 4 KiB granule contains three small objects, lookup returns the
metadata handle for that 4 KiB unit. The allocator uses the original pointer,
the handle, its size class, and its own layout to recover the individual object.

## Configuration

`memx_config_default` selects:

```text
directory       BOUNDED
policy          BALANCED
hint semantics  VALIDATED
region shift    21 (2 MiB)
granule shift   12 (4 KiB)
```

It intentionally leaves `managed_size` zero. A caller must select a bounded
range or switch to sparse mode before creation.

### Directory mode

`MEMX_DIRECTORY_BOUNDED` manages one aligned contiguous interval.

`MEMX_DIRECTORY_SPARSE` covers addresses below `2^address_bits` using a
two-level on-demand directory. Gate 1 supports no more than 32 region-index
bits.

### Policy

The three policy names reserve the eventual optimization behavior. With only
EMPTY, UNIFORM, and DENSE, their representation decisions are identical.

### Hint mode

`MEMX_HINT_VALIDATED` means MemX must verify every hint property that can be
verified without destroying the intended fast path.

`MEMX_HINT_TRUSTED` means the caller guarantees alignment, coverage, repetition,
and other declared properties. Violating a trusted hint is caller error and may
produce an undefined MemX result.

Gate 1 stores the configured semantic mode but exposes no arithmetic layout
hint yet. Future hint structures must always carry one of these modes; there is
no ambiguous middle behavior.

### Allocator callbacks

Both allocation callbacks must be supplied together or both omitted. Omission
uses `malloc` and `free`.

Custom allocations must satisfy normal C object alignment. Gate 1 uses two low
pointer bits in dense descriptors and checks returned dense pointers before
publishing them.

The deallocator receives the same context passed to the allocator. MemX never
mixes custom and default allocation within an index.

## Index lifetime

`memx_index_create` either returns a complete index or leaves the output null.

`memx_index_destroy(NULL)` is allowed. Destroying an index invalidates all
bounded views and every mapping. Gate 1 requires external exclusion of readers
and writers during destruction.

## Insert

`memx_insert(index, base, size, handle)` requires:

- a non-null index;
- nonzero size;
- aligned base and size;
- a covered range without unsigned overflow;
- a handle other than `MEMX_HANDLE_INVALID`;
- no already mapped granule in the range.

Overlap returns `MEMX_ERROR_OVERLAP`, even when the existing handle equals the
requested handle. Insert is not an idempotent “ensure” operation.

Insertion has semantic failure atomicity: a failed operation does not expose
the requested handle in only part of the range. Preparation may retain empty
capacity after allocation failure.

## Remove

`memx_remove(index, base, size, expected_handle)` uses the same alignment and
coverage requirements as insert. Every granule must equal `expected_handle`.

Any mismatch returns `MEMX_ERROR_NOT_FOUND` and leaves all handles intact.

The caller must not use remove as concurrent lifetime reclamation in Gate 1.
The later concurrent contract is:

```text
make allocation unreachable
-> mark RETIRED
-> wait for prior readers
-> remove mappings
-> reclaim record and storage
```

## Checked lookup

`memx_lookup_address` accepts an integer address. `memx_lookup` is a convenience
wrapper for a pointer.

Checked lookup returns invalid for:

- a null index;
- a bounded address below or above the managed interval;
- a sparse address outside the configured usable width;
- a missing sparse leaf;
- an empty region or granule.

Checked lookup never allocates directory nodes.

## Proven-valid lookup

`memx_index_bounded_view` succeeds only for a bounded index. It captures the
base, shifts, region geometry, and descriptor array.

`memx_bounded_lookup_assume_mapped` omits coverage and mode checks. Its caller
must prove:

```text
base_address <= address < base_address + managed_size
```

The view is valid until mutation or index destruction in Gate 1. Concurrent
publication will later define when a reader may retain a view.

## Immutable dense overlay

`memx_index_optimize_overlay` is an explicit bounded-index optimization on
Linux-family targets, including the tested Android NDK build. It reserves a
direct handle array in virtual memory, copies only DENSE region pages, replaces
dense descriptor pointers with an internal marker, and frees the old dense
tables. EMPTY and UNIFORM overlay slots remain demand-zero; the descriptor,
not the zero value, selects their semantics, so handle zero remains valid.

Optimization requires external exclusion of readers and writers. It
invalidates every earlier bounded view. Once successful, the index is frozen:
insert and remove return `MEMX_ERROR_BUSY`.

`memx_index_overlay_view` exposes the immutable geometry. The branchless
`memx_overlay_lookup_trusted` accessor requires all of the following:

- the address is inside the configured bounded interval;
- the address is currently mapped, not EMPTY;
- the descriptor and view are uncorrupted;
- the index remains alive and frozen.

Violating those conditions is undefined. `memx_lookup_address` remains the
checked alternative and safely returns invalid for EMPTY or out-of-range
addresses. Unsupported targets return `MEMX_ERROR_UNSUPPORTED` from overlay
optimization without changing the index.

## Statistics

Statistics distinguish directory and secondary-representation bytes.

For a bounded index, directory bytes include the index object and the complete
descriptor array.

For a sparse index, directory bytes include the index object, root pointer
array, and every allocated descriptor leaf. Dense table bytes are reported as
representation bytes in adaptive mode. For an overlay, representation bytes
report private page-granularity storage touched by dense regions.

`empty_regions` means allocated empty descriptors in sparse mode; unallocated
sparse coverage is implicit and not counted as resident region objects.

Adaptive and flat statistics report allocator-requested bytes. Overlay
statistics additionally expose `reserved_bytes` for the full virtual mapping,
including guard pages. They do not include kernel page-table memory; the Linux
benchmark reads `smaps` to report overlay RSS independently.

`page_accounted_bytes` is the equal-category benchmark model. It rounds every
live heap allocation independently to the host page size and, in overlay mode,
adds the page-rounded DENSE intervals. It is deliberately separate from
`total_bytes`, actual RSS, allocator usable size, and `reserved_bytes`.

## Status codes

`MEMX_OK` indicates completion.

`MEMX_ERROR_INVALID_ARGUMENT` covers null required parameters, zero or
misaligned ranges, invalid shifts, overflow, invalid handles, and incomplete
allocator callbacks.

`MEMX_ERROR_OUT_OF_RANGE` means a validly formed range lies outside index
coverage.

`MEMX_ERROR_OVERLAP` means insertion found an existing mapping.

`MEMX_ERROR_NOT_FOUND` means removal did not find the expected mapping across
the complete range.

`MEMX_ERROR_OUT_OF_MEMORY` means metadata allocation failed.

`MEMX_ERROR_BUSY` is reserved for bounded concurrent reclamation pressure.

`MEMX_ERROR_UNSUPPORTED` means a valid request is outside the implementation's
current capability, such as an impractically wide v0 sparse directory.

## Future reader API

The required semantic will be wait-free guarded lookup. Epoch/QSBR is the
default planned implementation, not part of the algorithm's public identity.

Embedding allocators will be able to provide an equivalent lifetime domain and
use a proven-protected lookup path without redundant reader bookkeeping.

No future reclamation implementation may introduce locks, allocation, helping,
or unbounded retry into guarded lookup.
