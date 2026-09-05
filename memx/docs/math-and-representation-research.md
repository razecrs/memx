# Math and Representation Research

This note turns established data-structure results into falsifiable MemX
experiments. It is a candidate list, not a novelty claim and not permission to
add every structure.

## Symbols and lower bounds

For one region, let:

```text
G = granules in the region
k = exceptional granules or runs
U = position universe, usually G
d = number of distinct handles
```

Merely choosing `k` exception positions from `G` requires at least

```text
B(k, G) = ceil(log2(binomial(G, k))) bits.
```

If the region contains runs with `k - 1` internal boundaries, boundary choice
alone requires

```text
ceil(log2(binomial(G - 1, k - 1))) bits.
```

For a handle sequence with empirical probabilities `p_i`, zero-order symbol
entropy is

```text
H0 = -sum(p_i * log2(p_i)) bits/granule.
```

`G * H0` is a useful compression floor only after also counting the distinct
handle dictionary, position/index metadata, alignment, and random-access
machinery. It is not a realizable lookup structure by itself.

These bounds give each candidate an honesty check: if bookkeeping already
greatly exceeds the appropriate bound before cache-line rounding, it needs an
exceptionally strong latency result to survive.

## Bitmap plus rank for SPARSE

Store one occupancy bit per granule and a packed handle vector. For granule
`g`:

```text
if bitmap[g] == 0: return default
return handles[rank1(bitmap, g)]
```

Succinct indexable dictionaries show that a set of `k` positions in universe
`G` can approach `B(k,G)` bits while supporting constant-time rank/select in
the RAM model. A practical first prototype should be less ambitious: fixed
64-bit bitmap words, periodic rank samples, and hardware/compiler `popcount`.

Expected strengths:

- exact lookup with no false positives;
- compact isolated exceptions;
- predictable address calculation;
- natural branchless variants.

Risks:

- bitmap scan/rank metadata loses to a tiny sorted array at very small `k`;
- handle-vector access remains dependent on rank;
- mutations can shift packed values, so build/freeze-first semantics may win.

Gate experiment: compare sorted pairs, bitmap+rank, and DENSE over all `k` for
the actual region sizes, reporting lookup distributions and complete bytes.

Source: Raman, Raman, and Rao Satti, [Succinct Indexable Dictionaries](https://arxiv.org/abs/0705.0552).

## Elias–Fano boundaries for RUN

Run boundaries form a monotone integer sequence. Elias–Fano stores `k`
monotone values from universe `U` in approximately

```text
k * ceil(log2(U / k)) + 2k bits
```

while retaining direct access; predecessor lookup can identify the owning run.
This makes it a mathematically attractive boundary encoding when `k << U`.

Expected strengths:

- close-to-optimal monotone boundary space;
- direct access and predecessor operations;
- handle array stays compact at one value per run.

Risks:

- constant factors and predecessor work may dominate at MemX's small region
  sizes;
- a linear or branchless scan can beat sophisticated indexing for tiny `k`;
- rebuilding immutable encodings makes dynamic mutation expensive.

Gate experiment: compare linear scan, binary search, fixed bitset boundaries,
and Elias–Fano only on traces with measured run structure.

Source: Pibiri and Venturini, [Dynamic Elias–Fano Representation](https://drops.dagstuhl.de/opus/volltexte/2017/7324/pdf/LIPIcs-CPM-2017-30.pdf).

## Static perfect hashing for sparse exceptions

FKS perfect hashing gives linear expected construction space and worst-case
constant static membership lookup. It could index exception positions after a
region is frozen.

Risks are substantial for MemX: hash arithmetic, two-level metadata, poor tiny
set constants, and rebuild cost. It is a control for high, irregular sparse
sets—not the first SPARSE implementation.

Source: Fredman, Komlós, and Szemerédi, [Storing a Sparse Table with O(1) Worst Case Access Time](https://www.cs.dartmouth.edu/~ac/Teach/CS105-Winter05/Handouts/fks-perfecthash.pdf).

## Directly addressable handle coding

Directly Addressable Codes split variable-length integers into level arrays and
bitmaps so an element remains randomly accessible. They could compress dense
handle vectors when handle magnitudes are strongly skewed.

This is orthogonal to position compression and must count extra dependent
loads. Opaque allocator handles may be pointer-like and effectively
incompressible, so trace entropy must justify a prototype first.

Source: Brisaboa, Ladra, and Navarro, [DACs: Bringing Direct Access to Variable-Length Codes](https://doi.org/10.1016/j.ipm.2012.08.003).

## Adaptive radix nodes as an irregular control

ART changes node representation with local fanout to improve cache behavior
and avoid radix-tree worst-case space. That is philosophically close to MemX's
region adaptation and therefore important prior art, but its pointer/key lookup
shape is not automatically the best granule index.

Use ART as an irregular-layout control if real traces show hierarchical prefix
structure that EMPTY/UNIFORM/DENSE and position compression miss.

Source: Leis, Kemper, and Neumann, [The Adaptive Radix Tree](https://db.in.tum.de/~leis/papers/ART.pdf).

## Approximate filters are not authoritative indexes

Bloom, quotient, cuckoo, and xor filters permit false positives. MemX lookup
must return exact metadata, so none may be the final authority. A filter could
only reject obvious misses before an exact secondary structure, and its added
load/bytes must beat direct exact lookup.

That makes approximate filters low priority for v0. They become interesting
only if real traces contain expensive exact-negative lookups.

Source: Fan et al., [Cuckoo Filter: Practically Better Than Bloom](https://www.cs.cmu.edu/~dga/papers/cuckoo-conext2014.pdf).

## Immediate research order

1. Collect real allocator traces and calculate per-region `k`, run count, `H0`,
   and distinct-handle count.
2. Build bitmap+rank as the first exact SPARSE candidate if isolated exceptions
   are common.
3. Build simple RUN scans first; add Elias–Fano only if boundary arrays consume
   material memory and predecessor latency has room.
4. Consider handle coding only when value entropy—not position entropy—is the
   measured problem.
5. Keep perfect hashing and ART as controls for irregular frozen layouts.
6. Reject every candidate that adds no Pareto point on a named trace family.

The current VM overlay already addresses mixed lookup dispatch without adding a
new per-region representation. Gate 2 trace evidence comes before any of the
structures above enters the core library.
