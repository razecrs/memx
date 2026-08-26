# MemX Gate 1 Algorithm Specification

## 1. Purpose

MemX maps arbitrary addresses to allocator-defined granule or span handles. It
is a random-access metadata compression experiment, not a general-purpose
compressor and not an object allocator.

The exact v0 mapping is:

```text
lookup(index, address) -> native-word opaque handle or INVALID
```

A handle describes the configured granule. Individual allocation recovery is a
second-level allocator operation when multiple allocations share a granule.

## 2. Terminology

An **address** is the integer address used to select metadata. Public pointer
lookup converts a pointer to `uintptr_t` only on targets where that operation is
defined by the platform contract.

A **granule** is the smallest address interval independently mapped by one
MemX index. Its size is `2^granule_shift` bytes.

A **region** is a power-of-two group of granules represented together. Its size
is `2^region_shift` bytes.

A **descriptor** is the one-native-word top-level value selecting a region
representation.

A **handle** is caller-defined metadata identity. MemX does not dereference or
interpret raw handles in Gate 1.

## 3. Configuration invariants

`region_shift` and `granule_shift` are exponents, not sizes. Both must be less
than the number of bits in `uintptr_t`, and `region_shift` must be greater than
or equal to `granule_shift`.

The number of granules in a region is:

```text
granules_per_region = 1 << (region_shift - granule_shift)
```

Bounded indexes require:

```text
managed_size > 0
base_address % region_size == 0
managed_size % region_size == 0
base_address + managed_size does not overflow
```

Sparse indexes require a zero base and an explicit usable address width. The v0
two-level implementation accepts at most 32 significant region-index bits, to
prevent an accidentally enormous direct root or leaf.

## 4. Address decomposition

For a bounded index:

```text
relative = address - base_address
region   = relative >> region_shift
granule  = (relative >> granule_shift) & (granules_per_region - 1)
```

Checked lookup first verifies that `relative < managed_size`.

For a sparse index, `relative` is the address itself after validation against
the configured usable address width.

No byte-order-sensitive memory representation participates in this
calculation. It is integer shifting and masking only.

## 5. Descriptor encoding

The portable Gate 1 descriptor occupies one `uintptr_t`:

```text
descriptor == 0              EMPTY
descriptor & 3 == 1          inline UNIFORM handle
descriptor & 3 == 2          aligned DENSE table pointer
```

After an index is explicitly frozen into OVERLAY storage, tag `10` becomes a
DENSE marker rather than a pointer. Storage mode is therefore part of the
internal decoding invariant; old bounded views are invalid after conversion.

### EMPTY

An empty descriptor returns `MEMX_HANDLE_INVALID` for every granule.

### UNIFORM

If a handle can be shifted left by two bits without losing information, it is
encoded as:

```text
descriptor = (handle << 2) | 1
handle     = descriptor >> 2
```

This means uniform lookup requires no secondary memory access.

The public handle domain is not truncated to make this encoding possible. A
uniform value too large to encode inline is represented by a dense table filled
with the same full-width handle.

### DENSE

A dense descriptor points to an array containing one native-word handle per
granule. The allocation must be aligned so its bottom two address bits are zero.
The descriptor is:

```text
descriptor = dense_pointer | 2
```

Lookup masks off the tag, loads `entries[granule]`, and returns the full handle.

Custom allocation callbacks must provide alignment suitable for any C object.
MemX defensively rejects an allocation that cannot carry descriptor tags.

## 6. Directory modes

### Bounded directory

The bounded directory is a contiguous descriptor array with one entry per
managed region. Its lookup has one directory load followed by an optional dense
entry load.

This mode exists for allocators or runtimes that manage one known address range.
It is the primary latency baseline for MemX itself.

### Sparse directory

The sparse directory divides region-index bits into root and leaf halves:

```text
root_index = region_index >> leaf_bits
leaf_index = region_index & ((1 << leaf_bits) - 1)
```

The root is a direct array of leaf pointers. Leaves are allocated on demand.
Missing leaves imply empty regions.

This is intentionally simple. Its root and leaf size are fully counted. Gate 1
does not claim that this split is optimal.

## 7. Lookup semantics

### Checked lookup

Checked lookup:

1. rejects a null index;
2. validates the address against the directory coverage;
3. computes region and granule indices;
4. loads the region descriptor;
5. decodes EMPTY, UNIFORM, or DENSE;
6. returns a handle or `MEMX_HANDLE_INVALID`.

### Bounded proven-valid lookup

`memx_bounded_lookup_assume_mapped` accepts an immutable bounded view. The
caller guarantees the address is within the configured range.

The function performs no bounds check and no directory-mode dispatch. An
out-of-range address can index outside the descriptor array. This is an
allocator-internal contract, not a safe public query for untrusted pointers.

### Frozen overlay lookup

The Linux/Android overlay reserves one direct entry position per covered
granule but dirties only pages belonging to DENSE regions. UNIFORM handles stay
inline in the descriptor array. For a trusted mapped address, lookup computes
the descriptor and direct overlay entry independently, then uses the dense tag
as a native-word selection mask:

```text
dense_mask = -((descriptor >> 1) & 1)
uniform    = descriptor >> 2
dense      = overlay[relative >> granule_shift]
result     = (dense & dense_mask) | (uniform & ~dense_mask)
```

On the measured x86-64 compiler this becomes a conditional move rather than a
representation branch. EMPTY is intentionally excluded from the trusted
contract; checked lookup validates EMPTY before selecting an overlay entry.

Conversion is two-phase. MemX first validates tags, maps the complete virtual
overlay, and copies every dense table without modifying the index. Only after
all fallible work succeeds does it publish frozen mode, replace dense pointers
with markers, and free the old tables. Conversion requires external exclusion
of all readers and writers. Frozen indexes reject mutation.

## 8. Insertion

Insertion maps every touched granule to one handle. Base and size must be
granule-aligned. The invalid handle is rejected.

The operation first scans the full range and rejects any overlap. It then
ensures every partially populated empty region has a dense table. Whole empty
regions become inline uniform descriptors when the handle fits.

Incremental insertion into a dense region may eventually make every entry
equal. MemX scans the table after mutation and collapses it to UNIFORM when the
handle can be represented inline.

An allocation failure during sparse-directory or dense preparation does not
publish a handle. It may leave an allocated empty leaf or an all-invalid dense
table, which is semantically empty but consumes metadata. This retained
capacity is reported by statistics and reused by later operations.

## 9. Removal

Removal requires the expected handle. The full range is preflighted, and any
mismatch returns `NOT_FOUND` without clearing mappings.

Removing an entire uniform region converts it directly to EMPTY. Removing only
part of a uniform region first allocates a dense table initialized with the
uniform handle, then clears the selected granules.

All partial uniform conversions are prepared before removal begins. Therefore
an allocation failure may change a representation from UNIFORM to equivalent
DENSE, but it does not partially remove the requested mapping.

Dense tables are scanned after removal. An all-invalid table becomes EMPTY. An
all-equal valid table becomes UNIFORM if its handle is inline-encodable.

## 10. Static policy behavior

Gate 1 has only three representations, so every preset makes the same
lossless choice:

```text
all invalid                  -> EMPTY
all equal and inline-safe    -> UNIFORM
otherwise                    -> DENSE
```

The public policy setting is retained because later representations need an
explicit optimization contract. Overlay and flat conversion remain explicit;
the presets do not silently freeze or transform an index.

Future preset definitions are constraints:

```text
FAST:
    minimize predicted L subject to M <= configured memory ceiling

BALANCED:
    minimize M subject to predicted L <= 1.10 * L_flat

COMPACT:
    minimize M subject to predicted L < L_radix
```

Expert weights evaluate:

```text
Cost = alpha * lookup latency
     + beta  * resident metadata
     + gamma * mutation cost
     + delta * update memory amplification
```

## 11. Complexity

Bounded lookup uses fixed work and at most two metadata loads. Sparse lookup
adds a root-to-leaf pointer load. All Gate 1 lookup paths are O(1) for a fixed
configuration.

Insertion and removal preflight every affected granule, so mutation cost is
O(number of granules touched). Reclassification scans one region and is
O(granules per region).

These asymptotic labels are secondary. Benchmarks report actual cycles, cache
behavior, and footprint.

## 12. Deliberate omissions

Gate 1 descriptors are not atomic. Mutations are single-threaded and lookup may
not race with mutation.

There is no epoch state, reader context, lifetime transition, rich record store,
layout-derived hint path, `RUN`, region-local sparse representation, NUMA
placement, runtime dispatch, intrinsics, or assembly.

Those omissions preserve interpretability of the first Pareto measurement.
