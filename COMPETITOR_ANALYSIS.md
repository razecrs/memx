# MemX Competitor Source Analysis

Date: 2026-08-25

This is a source-level feasibility check, not a performance result. MemX does
not yet have an implementation, so no claim that it beats these allocators is
currently justified.

## Repositories and snapshots

The repositories are shallow clones under `research/`.

| Project | Primary implementation language | Snapshot | Commit date |
|---|---:|---|---:|
| jemalloc | C | `e36a0fa5bc1e1090362505ac4af4408466ba5163` | 2026-08-10 |
| mimalloc | C | `cd69707c3ca01a4c5fb358e8b92a710554f15356` | 2026-08-19 |
| TCMalloc | C++ | `ea99e09225e4f518d3e201f2e9c33ea2f85d0413` | 2026-08-25 |
| snmalloc | C++ | `a5f10eb6c048fc32bca2383daa8d6acbf82cfb7f` | 2026-08-13 |

## LOC count

These are physical lines in tracked C-family source files. “Production” omits
paths named test, tests, testing, bench, benchmark, benchmarks, doc, docs,
example, examples, third_party, vendor, and deps, as well as common test,
benchmark, and fuzz filename suffixes. Generated code and comments are not
removed, so these figures are reproducible scale indicators rather than logical
LOC.

| Project | Production C-family LOC | All tracked C-family LOC |
|---|---:|---:|
| jemalloc | 61,226 | 105,563 |
| mimalloc | 25,225 | 30,628 |
| TCMalloc | 51,710 | 105,223 |
| snmalloc | 21,977 | 29,584 |

Production language split:

| Project | C | C++ | Headers | Assembly |
|---|---:|---:|---:|---:|
| jemalloc | 36,071 | 324 | 24,831 | 0 |
| mimalloc | 19,010 | 0 | 6,215 | 0 |
| TCMalloc | 0 | 18,772 | 32,150 | 788 |
| snmalloc | 0 | 1,044 | 20,933 | 0 |

snmalloc is predominantly header-implemented C++; counting only `.cc` files
would badly misrepresent its size.

### Comparable address-index subsystem size

| Project | Files counted | Physical lines |
|---|---|---:|
| jemalloc rtree core | `rtree.h`, `rtree_tsd.h`, `rtree.c` | 909 |
| jemalloc rtree plus emap integration | above plus `emap.h`, `emap.c` | 1,699 |
| mimalloc page map | `page-map.c` plus inline lookup block in `internal.h` | about 655 |
| TCMalloc page map | `pagemap.h`, `pagemap.cc` | 516 |
| snmalloc page map plumbing | `ds/pagemap.h`, backend wrapper and default entry | 635 |

The subsystem figures are not directly comparable feature counts. For example,
snmalloc's two-word metadata semantics live partly in the much larger
`mem/metadata.h`, while jemalloc's emap performs extent transition operations
beyond basic lookup.

## What each implementation actually does

### jemalloc

Relevant code:

- `include/jemalloc/internal/rtree.h`
- `include/jemalloc/internal/rtree_tsd.h`
- `src/rtree.c`
- `include/jemalloc/internal/emap.h`
- `src/emap.c`

jemalloc uses an extent map backed by a radix tree of one to three levels,
depending on usable virtual-address bits. Its hot path is materially stronger
than “walk a generic radix tree” suggests:

- Each thread has a 16-entry direct-mapped leaf cache and an 8-entry L2 cache.
- A leaf-cache hit computes a subkey and reads the leaf directly, avoiding the
  full tree walk.
- On common 64-bit configurations, one pointer-sized atomic leaf word packs the
  `edata` pointer, size index, extent state, head flag, and slab flag.
- Dependent lookups can use relaxed atomic loads; independent arbitrary-pointer
  queries use acquire semantics.
- Non-slab extents primarily register boundaries. Slabs register their
  interiors because arbitrary object pointers must resolve there.

Consequence for MemX: comparing only against an uncached radix tree would be a
straw baseline. A fair benchmark needs cold rtree, hot per-thread rtree-cache,
dependent, and independent lookup cases.

### mimalloc

Relevant code:

- `src/page-map.c`
- the page-map lookup block in `include/mimalloc/internal.h`
- configuration in `include/mimalloc/bits.h`

Current mimalloc is not accurately described only as “mask to an aligned
segment.” It has several paths:

- A flat map uses one byte per 64 KiB slice. The byte is an offset back to the
  beginning of the mimalloc page, so lookup reconstructs the page address
  without storing a pointer in every entry.
- The flat map reserves address space and commits its contents on demand. It is
  selected by default only for suitable smaller virtual-address configurations.
- A two-level map is used for larger address spaces. Its leaf submaps contain
  page pointers at 64 KiB granularity and are allocated/committed on demand.
- When page metadata is placed at a derivable aligned location, the common path
  can recover it arithmetically and avoid the page map; checked/debug/secure
  paths still consult the map as required.

Consequence for MemX: mimalloc's one-byte flat entry is a very difficult density
baseline, but it achieves that by exploiting mimalloc-specific layout. A fair
comparison must distinguish generic metadata lookup from constrained page-base
reconstruction.

### TCMalloc

Relevant code:

- `tcmalloc/pagemap.h`
- `tcmalloc/pagemap.cc`

TCMalloc's current `PageMap` wraps a fixed-depth `PageMap3` radix structure:

- Address page-number bits are split across root, middle, and leaf indices.
- Root and middle entries point to lazily allocated nodes/leaves.
- A leaf has a compact size-class array, a packed span-pointer-plus-size-class
  array, and coarse huge-page information.
- The packed word allows a span and size class to be returned from one leaf
  load on the combined lookup path.
- Checked lookup tests address bounds and missing nodes. Existing-descriptor
  variants omit these checks when the caller proves the page is registered.
- Lookups require no lock; mutation uses external synchronization.

Consequence for MemX: the relevant baseline is not merely a classic pointer-only
radix tree. It includes checked and proven-valid fast paths and a packed result.

### snmalloc

Relevant code:

- `src/snmalloc/ds/pagemap.h`
- `src/snmalloc/backend_helpers/pagemap.h`
- `src/snmalloc/backend_helpers/defaultpagemapentry.h`
- `src/snmalloc/mem/metadata.h`
- `docs/AddressSpace.md`

snmalloc is the closest direct challenge to the MemX hypothesis:

- It uses a flat, virtually reserved, lazily committed page map.
- Normal granularity is approximately 16 KiB (`MIN_CHUNK_BITS` is normally 14).
- Each entry is two pointer-sized words.
- Direct lookup is essentially base plus shifted-address indexing.
- The entry packs slab metadata, owning remote allocator, size class, ownership,
  backend state, and a boundary bit.
- Backend code also reuses page-map entries as nodes in its large buddy
  allocator representation.
- It explicitly supports bounded maps and capability/provenance concerns.

Consequence for MemX: “one namespace plus packed ownership and size metadata” is
not novel on its own. MemX's actual differentiator must be compression by
region representation while retaining competitive random lookup.

## First-order MemX cost model

Assume the first experiment uses:

- 2 MiB regions;
- 4 KiB metadata granularity;
- 512 local entries per region;
- 8-byte metadata entries or handles;
- one 8-byte region descriptor;
- only `UNIFORM` and `DENSE`.

Then:

```text
flat bytes per region       = 512 * 8 = 4096
MemX UNIFORM bytes          = 8
MemX DENSE bytes            = 8 + 4096
average MemX bytes          = 8 + dense_region_fraction * 4096
ratio to flat               = dense_region_fraction + 1/512
```

Ignoring allocator overhead and the directory implementation:

| Dense-region fraction | MemX bytes/region average | Fraction of flat |
|---:|---:|---:|
| 0% | 8 | 0.20% |
| 10% | about 418 | 10.20% |
| 50% | 2,056 | 50.20% |
| 90% | about 3,694 | 90.20% |
| 100% | 4,104 | 100.20% |

This confirms a real but conditional memory hypothesis: savings are almost
exactly determined by the fraction of regions that avoid `DENSE`. Dense MemX
cannot beat an equivalent flat table locally; it pays a small descriptor tax.

At snmalloc-like 16 KiB granularity with 16-byte entries, a 2 MiB region has
128 entries and a flat cost of 2,048 bytes. The same conclusion holds: uniform
regions compress strongly; dense regions cost slightly more than flat.

## The directory problem

The v0 plan currently hides its most important cost in `regions[region]`.

For a 48-bit virtual address space and 2 MiB regions:

```text
region count                 = 2^(48 - 21) = 134,217,728
8-byte flat descriptor map   = 1 GiB virtual
16-byte flat descriptor map  = 2 GiB virtual
```

Demand paging can keep resident memory much lower, but this is still part of
the algorithm and its lookup behavior. A hierarchical directory adds a
dependent load. A bounded index changes API semantics. A huge flat virtual
reservation depends on OS and address-space assumptions.

The first prototype must therefore benchmark at least two explicit top-level
choices:

1. a bounded contiguous descriptor array for controlled experiments;
2. a sparse/two-level descriptor directory for general virtual addresses.

Without this, a one-load `UNIFORM` claim accidentally excludes the cost of
finding the descriptor.

## Can the current plan beat them?

### Plausible wins

- Metadata footprint versus pointer-sized flat maps when real layouts contain
  many uniform regions.
- Cold lookup versus multi-level maps when a descriptor is directly reachable
  and resolves inline.
- A useful latency/footprint Pareto point between a fully dense map and a radix
  tree.
- Large wins for allocator traces whose chunk/span metadata repeats across most
  of each region.

### Likely losses or very hard targets

- `DENSE` lookup versus a flat table: MemX adds descriptor decode and normally
  another dependent load.
- Uniform lookup versus mimalloc's arithmetic aligned-metadata path.
- Dense footprint versus mimalloc's one-byte offset map, unless MemX offers an
  equally compact representation with equivalent semantics.
- Hot lookup versus jemalloc when its per-thread rtree leaf cache hits.
- Generality versus allocator-specific structures: the competitors can exploit
  invariants that an arbitrary-layout MemX API cannot assume.
- Mutation-heavy traces if conversions or copy/publish rebuilding occur often.

### Verdict before implementation

The plan does **not** beat these implementations on paper across the board, and
it should not try to. It has one defensible research wedge:

> Compress repeated address metadata at a region level, then determine whether
> the additional dispatch and directory costs preserve a useful Pareto point.

The strongest initial comparison is against snmalloc-style flat metadata and a
TCMalloc-style fixed-depth map, not against entire allocator throughput.
jemalloc's cached rtree and mimalloc's layout-derived page lookup should be
included as tougher specialized reference points.

## Changes recommended before coding v0

1. Define exact benchmark semantics: pointer to an 8-byte opaque metadata
   handle at a fixed granularity. Do not let one baseline return less
   information than another.
2. Make the top-level directory an explicit experimental variable.
3. Start with a bounded address range so `UNIFORM + DENSE` can test the core
   hypothesis without conflating it with full-VA indexing.
4. Use static representation selection in v0. Exclude transition and
   reclamation machinery.
5. Parameterize region size, granularity, and entry width.
6. Generate layouts by **region entropy and run length**, not just overall
   occupancy.
7. Measure memory as allocated bytes, resident bytes, virtual bytes, and bytes
   per managed MiB separately.
8. Implement fair baselines: flat, two-level, fixed-depth radix, and cached
   radix. Include checked and proven-valid variants where relevant.
9. Record dependent loads and cache misses in addition to cycles.
10. Establish a kill criterion: if MemX does not approach the flat-table
    latency/footprint Pareto frontier on low-entropy traces, stop before adding
    `SPARSE`, `RUN`, concurrency, or assembly.

## Bottom line

MemX is not obviously redundant, but its novelty and advantage are narrower
than the original plan suggests. The experiment worth building is not “a
faster pointer lookup than four production allocators.” It is:

> Does region-level uniform compression save enough resident metadata on real
> allocator-shaped traces to pay for descriptor lookup and representation
> dispatch?

That question remains credible after inspecting the four codebases.
