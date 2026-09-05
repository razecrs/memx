# Experimental MemX Heap

`memx_heap` is the first allocator integration experiment built on the MemX
index. It is intentionally a separate library from the representation engine:

```text
application allocation API
        |
        v
thread cache / central bins / large mappings
        |
        v
MemX: address -> span metadata
```

It implements allocator steps 1 through 8. It does not implement concurrent
MemX representation replacement, epoch reclamation, purging, NUMA policy, or
production interposition.

## Implemented stages

1. MemX indexes every committed small-object span.
2. The Linux/Android OS provider reserves an aligned `PROT_NONE` arena,
   commits arena chunks with `mprotect`, and creates independent large
   mappings. Commits are rounded up to the transparent-huge-page size where
   the platform reports one, so a first touch can fault in a whole huge page
   rather than depending on later collapse.
3. A fixed-span manager hands out 64 KiB spans on demand from the bounded
   arena, backed by those larger commit chunks.
4. Nineteen payload size classes cover 16 through 8192 bytes at two classes per
   power of two (2^n and 1.5 * 2^n), bounding internal fragmentation at 50% of
   the request instead of 100%. The 4096-, 6144- and 8192-byte classes keep
   common medium allocations off the mapping path.
5. Small allocations use intrusive free blocks and per-span metadata.
6. Large/aligned allocation, `calloc`, `realloc`, checked free, and usable-size
   APIs are present with overflow checks.
7. `_Thread_local` caches refill in bounded batches from central class bins.
8. A free performed by a non-owner thread enters the owner's mutex-protected
   remote queue. Explicit thread detach closes that queue and returns cached
   blocks to the central bins.

An attached but inactive owner can therefore retain remotely freed blocks
until it allocates again or explicitly detaches. The queue is correctness-safe
but not presently bounded; production-grade abandoned-cache adoption belongs
after the current eight-stage experiment. Cache records themselves remain
allocated until heap destruction.

## Concurrency boundary

Small-block allocation state and statistics use C atomics. Central bins,
span creation, large-allocation records, and every MemX lookup/mutation are
protected by the heap mutex. Remote queues have a per-cache mutex.

The lock order avoids holding a remote-queue mutex while acquiring the heap
mutex. Cache metadata is retained until heap destruction, so a remote free
never observes a reclaimed owner-cache record.

This is not Gate 9/concurrent MemX. The embedding program must join allocator
threads, call `memx_heap_thread_detach` from participating threads, and stop all
heap operations before `memx_heap_destroy`.

## Layout

The default geometry is:

```text
reserved arena:  256 MiB
commit chunk:      huge page size, else span size
span:             64 KiB
MemX region:      64 KiB
MemX granule:      4 KiB
size classes:     19 (16..8192, two per power of two)
cache refill:      256 blocks
cache ceiling:     256 blocks / class / thread
activity counters: disabled by default
```

A one-byte-per-span side table maps an arena offset to its size class. The
free path reads that table instead of the span header, so releasing a block
touches only the block's own page; the table for the whole arena is 4 KiB and
stays cache resident. `arena_committed_bytes` reports actual committed bytes,
which with huge-page commit granularity can exceed `committed_spans` times the
span size.

Thread caches keep a tail pointer per class, so returning an over-full cache to
the central bins is a constant-time list splice rather than a walk. The
allocation path prefetches the next free block when it pops one.

Each committed span is represented by one MemX handle pointing to immutable
span metadata embedded at the beginning of the span. A maximally aligned block
header precedes every small user allocation. While allocated, it stores the
current cache owner, requested size, and atomic allocation state; while free,
the owner slot becomes the intrusive next pointer. The containing span is
derived arithmetically from the aligned arena instead of being repeated in
every block. This makes the header 16 bytes on the primary x86-64 build (down
from 32 bytes) while retaining checked geometry validation.

Exact allocation, free, byte, cache-hit, and peak counters are opt-in through
`collect_activity_statistics`; enabling them intentionally adds atomic
bookkeeping to hot operations. Arena, span, and cache structure accounting
remains available independently.

Pointers larger than 8192 bytes or requiring over-alignment use a separate
anonymous mapping recorded in a heap-owned hash index. Checked size/free/realloc
hash the base address without dereferencing it, then compare full pointers in
one collision chain. Expected lookup cost is constant for well-distributed
addresses; pathological collisions still have linear worst-case cost. The hash
is not keyed or a defense against deliberate collision attacks.

The bucket array is allocated lazily with 64 entries, doubles when live records
reach bucket capacity, and attempts to halve when occupancy falls to one quarter.
The last removal releases the array. Rehashing and chain publication use the
existing heap mutex; readers never observe partial chains. Growth failure
releases the unpublished mapping and leaves existing allocations intact. Shrink
failure leaves a valid larger table and does not fail free. Mutation can include
an O(n) rehash and temporarily holds both bucket arrays.

`large_index_bytes` reports current requested bucket bytes, excluding allocator
overhead, large records, mappings, and transient rehash space.
`active_large_allocations` is structural accounting and remains available even
when activity statistics are disabled. The added stats field changes the public
struct size; rebuild callers together with this experimental library.
Small spans are not yet decommitted or recycled within the arena.

## API

```c
#include <memx/allocator.h>

memx_heap_t *heap = NULL;

if (memx_heap_create(NULL, &heap) != MEMX_HEAP_OK) {
    return 1;
}

void *pointer = memx_heap_malloc(heap, 96U);
pointer = memx_heap_realloc(heap, pointer, 512U);
(void)memx_heap_free(heap, pointer);

memx_heap_thread_detach(heap);
memx_heap_destroy(heap);
```

`memx_heap_free` returns false for a foreign, interior, or already freed
pointer. As with conventional allocators, racing access to the same allocation
after one thread frees it remains caller error.

`memx_heap_realloc` and `memx_heap_free` are checked APIs: they reject foreign,
interior, and stale small-allocation pointers. The `_unchecked` variants are
allocator-internal fast paths with the conventional contract that the pointer
is null or the live base address of an allocation owned by that heap. Violating
that contract is undefined. Competitor measurements must disclose which API
contract they use and keep checked-path sensitivity results separate.

## Evidence and non-claims

`memx_heap_test` covers all class boundaries, large/aligned allocation,
overflow, payload-preserving randomized `realloc`, arena exhaustion, invalid
free detection, remote free, owner detach, and multithread stress.

`memx_heap_fuzz` drives bounded allocation/free/reallocation sequences under
libFuzzer, ASan, and UBSan. The test suite also runs under ThreadSanitizer.

`memx_heap_bench` replays one deterministic slot workload through system
`malloc` and `memx_heap`. It is an engineering smoke benchmark only. It does
not compare fragmentation, RSS, tail latency, application throughput, or the
full feature set of mimalloc/jemalloc.

`memx_large_heap_bench LIVE_COUNT QUERIES` measures checked large-object index
scaling separately. The [2026-09-05 before/after report](../bench/results/large-index-2026-09-05.json)
contains raw samples, build fingerprints, successful/missing size lookups,
in-place shrink, mapping allocation and shuffled frees, plus a separate
small-object mimalloc comparison. The matching [math note](allocator-math-2026-09-05.md)
derives the hash-table costs and identifies future size-class and contention
experiments.
