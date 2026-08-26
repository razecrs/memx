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
   commits spans with `mprotect`, and creates independent large mappings.
3. A fixed-span manager commits 64 KiB spans on demand from the bounded arena.
4. Ten payload size classes cover 16 through 8192 bytes. The 4096- and
   8192-byte classes keep common medium allocations off the mapping path.
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
reserved arena: 256 MiB
span:            64 KiB
MemX region:     64 KiB
MemX granule:     4 KiB
cache refill:      128 blocks
cache ceiling:     256 blocks / class / thread
activity counters: disabled by default
```

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
anonymous mapping recorded in a heap-owned list. That makes the large path
simple and correct, not yet asymptotically optimal: lookup/free of a large
allocation is linear in the number of live large mappings. Small spans are not
yet decommitted or recycled within the arena.

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
