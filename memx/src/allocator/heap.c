#include "memx/allocator.h"

#include "internal.h"
#include "memx/memx.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MEMX_HEAP_CLASS_COUNT 19U
#define MEMX_HEAP_LARGE_MIN_BUCKETS 64U
#define MEMX_HEAP_SPAN_MAGIC UINT64_C(0x6d656d787370616e)

#if defined(__GNUC__) || defined(__clang__)
#define MEMX_HEAP_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define MEMX_HEAP_NOINLINE __declspec(noinline)
#else
#define MEMX_HEAP_NOINLINE
#endif

typedef struct memx_heap_span memx_heap_span_t;
typedef struct memx_heap_cache memx_heap_cache_t;
typedef struct memx_heap_block memx_heap_block_t;

struct memx_heap_block {
    struct {
        _Alignas(max_align_t) union {
            memx_heap_block_t *next;
            memx_heap_cache_t *owner;
        } link;
        uint32_t requested_size;
        atomic_uint state;
    } fields;
};

typedef struct memx_heap_central_chunk {
    memx_heap_block_t *next;
    memx_heap_block_t *tail;
} memx_heap_central_chunk_t;

_Static_assert(
    _Alignof(memx_heap_block_t) >= _Alignof(max_align_t),
    "small-allocation headers must preserve max_align_t alignment");
_Static_assert(
    sizeof(memx_heap_block_t) % _Alignof(max_align_t) == 0U,
    "small-allocation payloads must remain maximally aligned");
_Static_assert(
    16U >= sizeof(memx_heap_central_chunk_t),
    "the smallest size class must hold central chunk metadata");

struct memx_heap_span {
    uint64_t magic;
    struct memx_heap *heap;
    memx_heap_span_t *next;
    void *base;
    size_t block_stride;
    size_t block_count;
    size_t class_index;
};

typedef struct memx_heap_large {
    struct memx_heap_large *next;
    memx_os_range_t mapping;
    void *pointer;
    size_t requested_size;
    size_t alignment;
} memx_heap_large_t;

struct memx_heap_cache {
    struct memx_heap *heap;
    memx_heap_cache_t *heap_next;
    memx_heap_cache_t *tls_next;
    memx_heap_block_t *local[MEMX_HEAP_CLASS_COUNT];
    memx_heap_block_t *local_tail[MEMX_HEAP_CLASS_COUNT];
    size_t local_count[MEMX_HEAP_CLASS_COUNT];
    memx_heap_block_t *spare[MEMX_HEAP_CLASS_COUNT];
    memx_heap_block_t *spare_tail[MEMX_HEAP_CLASS_COUNT];
    size_t spare_count[MEMX_HEAP_CLASS_COUNT];
    memx_heap_block_t *remote[MEMX_HEAP_CLASS_COUNT];
    size_t remote_count[MEMX_HEAP_CLASS_COUNT];
    size_t allocation_count;
    size_t cache_hits;
    size_t cache_misses;
    pthread_mutex_t remote_lock;
    bool accepting_remote;
};

struct memx_heap {
    memx_heap_config_t config;
    memx_os_range_t arena;
    size_t span_size;
    size_t span_capacity;
    size_t next_span;
    size_t commit_granularity;
    size_t committed_bytes;
    uint8_t *span_classes;
    memx_index_t *index;
    /*
     * Three independent lock domains. The only permitted nesting is
     * class_locks[c] -> arena_lock, taken when a refill has to create a span;
     * nothing else is ever held together.
     *
     *   class_locks[c]  central[c], central_count[c]
     *   arena_lock      next_span, committed_bytes, span_classes, spans,
     *                   and every MemX index mutation and lookup
     *   admin_lock      large_buckets, large_count, caches
     */
    pthread_mutex_t admin_lock;
    pthread_mutex_t arena_lock;
    pthread_mutex_t class_locks[MEMX_HEAP_CLASS_COUNT];
    size_t initialized_class_locks;
    memx_heap_block_t *central[MEMX_HEAP_CLASS_COUNT];
    size_t central_count[MEMX_HEAP_CLASS_COUNT];
    memx_heap_span_t *spans;
    memx_heap_large_t **large_buckets;
    size_t large_bucket_count;
    size_t large_count;
    memx_heap_cache_t *caches;

    atomic_size_t live_requested_bytes;
    atomic_size_t peak_live_requested_bytes;
    atomic_size_t committed_spans;
    atomic_size_t large_allocation_count;
    atomic_size_t frees;
    atomic_size_t remote_frees;
    atomic_size_t invalid_frees;
    atomic_size_t thread_cache_count;
};

/* Two classes per power of two above 16 bytes: 2^n and 1.5 * 2^n. This halves
 * worst-case internal fragmentation from 100% to 50% of the requested size,
 * which matters most for the small classes that dominate object counts. */
static const size_t memx_heap_class_sizes[MEMX_HEAP_CLASS_COUNT] = {
    16U,
    24U, 32U,
    48U, 64U,
    96U, 128U,
    192U, 256U,
    384U, 512U,
    768U, 1024U,
    1536U, 2048U,
    3072U, 4096U,
    6144U, 8192U
};

static _Thread_local memx_heap_cache_t *memx_heap_tls_caches;
static _Thread_local memx_heap_cache_t *memx_heap_tls_last_cache;

static bool
memx_heap_power_of_two(size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

static size_t
memx_heap_align_up(size_t value, size_t alignment) {
    if (value > SIZE_MAX - (alignment - 1U)) {
        return 0U;
    }
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static void
memx_heap_update_peak(memx_heap_t *heap, size_t current) {
    size_t peak = atomic_load_explicit(
        &heap->peak_live_requested_bytes, memory_order_relaxed);
    while (current > peak
        && !atomic_compare_exchange_weak_explicit(
            &heap->peak_live_requested_bytes,
            &peak,
            current,
            memory_order_relaxed,
            memory_order_relaxed)) {
    }
}

static void
memx_heap_add_live_counted(memx_heap_t *heap, size_t bytes) {
    size_t previous;
    previous = atomic_fetch_add_explicit(
        &heap->live_requested_bytes, bytes, memory_order_relaxed);
    memx_heap_update_peak(heap, previous + bytes);
}

static void
memx_heap_add_live(memx_heap_t *heap, size_t bytes) {
    if (heap->config.collect_activity_statistics) {
        memx_heap_add_live_counted(heap, bytes);
    }
}

/* Maps a request onto the two-per-octave class table. For size > 16 the top
 * set bit of size-1 selects the octave and the bit below it selects the half,
 * so the whole computation is one leading-zero count and a shift. Sizes past
 * the last class return an index >= MEMX_HEAP_CLASS_COUNT and take the large
 * mapping path. */
static size_t
memx_heap_class_for_size(size_t size) {
    unsigned bit_width;
    size_t value;
    size_t half;
    if (size <= 16U) {
        return 0U;
    }
    value = size - 1U;
#if defined(__GNUC__) || defined(__clang__)
    bit_width = (unsigned)(sizeof(unsigned long long) * CHAR_BIT)
        - (unsigned)__builtin_clzll((unsigned long long)value);
#else
    bit_width = 0U;
    while (value != 0U) {
        value >>= 1U;
        bit_width += 1U;
    }
    value = size - 1U;
#endif
    if (bit_width < 5U) {
        bit_width = 5U;
    }
    half = (value >> (bit_width - 2U)) & (size_t)1U;
    return (size_t)((bit_width - 5U) * 2U) + half + 1U;
}

static memx_heap_cache_t *
memx_heap_find_cache(memx_heap_t *heap) {
    memx_heap_cache_t *cache = memx_heap_tls_last_cache;
    if (cache != NULL && cache->heap == heap) {
        return cache;
    }
    for (cache = memx_heap_tls_caches;
         cache != NULL;
         cache = cache->tls_next) {
        if (cache->heap == heap) {
            memx_heap_tls_last_cache = cache;
            return cache;
        }
    }
    return NULL;
}

static MEMX_HEAP_NOINLINE memx_heap_cache_t *
memx_heap_create_cache(memx_heap_t *heap) {
    memx_heap_cache_t *cache = calloc(1U, sizeof(*cache));
    if (cache == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&cache->remote_lock, NULL) != 0) {
        free(cache);
        return NULL;
    }
    cache->heap = heap;
    cache->accepting_remote = true;
    cache->tls_next = memx_heap_tls_caches;
    memx_heap_tls_caches = cache;
    memx_heap_tls_last_cache = cache;
    (void)pthread_mutex_lock(&heap->admin_lock);
    cache->heap_next = heap->caches;
    heap->caches = cache;
    (void)pthread_mutex_unlock(&heap->admin_lock);
    (void)atomic_fetch_add_explicit(
        &heap->thread_cache_count, 1U, memory_order_relaxed);
    return cache;
}

static memx_heap_cache_t *
memx_heap_get_cache(memx_heap_t *heap) {
    memx_heap_cache_t *cache = memx_heap_find_cache(heap);
    return cache == NULL ? memx_heap_create_cache(heap) : cache;
}

static void
memx_heap_push_central_chunk_locked(
    memx_heap_t *heap,
    size_t class_index,
    memx_heap_block_t *head,
    memx_heap_block_t *tail,
    size_t count) {
    memx_heap_central_chunk_t *chunk;
    assert(head != NULL);
    assert(tail != NULL);
    assert(count != 0U);
#if SIZE_MAX > UINT32_MAX
    assert(count <= (size_t)UINT32_MAX);
#endif
    chunk = (memx_heap_central_chunk_t *)(void *)(head + 1);
    tail->fields.link.next = NULL;
    chunk->next = heap->central[class_index];
    chunk->tail = tail;
    head->fields.requested_size = (uint32_t)count;
    heap->central[class_index] = head;
    heap->central_count[class_index] += count;
}

static void
memx_heap_push_central_locked(
    memx_heap_t *heap,
    size_t class_index,
    memx_heap_block_t *block) {
    memx_heap_push_central_chunk_locked(
        heap, class_index, block, block, 1U);
}

/* Commit arena bytes in whole commit-granularity chunks. When the granularity
 * is the transparent-huge-page size, the read/write mapping already covers a
 * full aligned huge page before anything faults it in, so the kernel can back
 * the first touch with one huge page instead of relying on later collapse. */
static bool
memx_heap_commit_through_locked(memx_heap_t *heap, size_t required_bytes) {
    size_t target;
    if (required_bytes <= heap->committed_bytes) {
        return true;
    }
    target = memx_heap_align_up(required_bytes, heap->commit_granularity);
    if (target == 0U || target > heap->arena.size) {
        target = heap->arena.size;
    }
    if (!memx_os_commit(
            (unsigned char *)heap->arena.base + heap->committed_bytes,
            target - heap->committed_bytes)) {
        return false;
    }
    heap->committed_bytes = target;
    return true;
}

/*
 * Claims one span from the arena bump pointer. Runs entirely under arena_lock
 * and returns the span with its blocks still unpublished, so the caller can
 * carve them under its own class lock without holding the arena.
 */
static memx_heap_span_t *
memx_heap_claim_span(
    memx_heap_t *heap,
    size_t class_index,
    size_t header_size,
    size_t stride,
    size_t block_count) {
    unsigned char *base;
    memx_heap_span_t *span;

    (void)pthread_mutex_lock(&heap->arena_lock);
    if (heap->next_span >= heap->span_capacity) {
        (void)pthread_mutex_unlock(&heap->arena_lock);
        return NULL;
    }
    base = (unsigned char *)heap->arena.base
        + heap->next_span * heap->span_size;
    if (!memx_heap_commit_through_locked(
            heap, heap->next_span * heap->span_size + heap->span_size)) {
        (void)pthread_mutex_unlock(&heap->arena_lock);
        return NULL;
    }
    span = (memx_heap_span_t *)(void *)base;
    memset(span, 0, sizeof(*span));
    span->magic = MEMX_HEAP_SPAN_MAGIC;
    span->heap = heap;
    span->base = base + header_size;
    span->block_stride = stride;
    span->block_count = block_count;
    span->class_index = class_index;
    span->next = heap->spans;
    if (memx_insert(heap->index, (uintptr_t)base, heap->span_size,
            (memx_handle_t)(uintptr_t)span) != MEMX_OK) {
        /* next_span is unchanged, so the committed chunk backs the same span
         * on the next attempt. Punching a hole here would split the arena VMA
         * and disable huge-page backing for the whole commit chunk. */
        (void)pthread_mutex_unlock(&heap->arena_lock);
        return NULL;
    }
    heap->span_classes[heap->next_span] = (uint8_t)class_index;
    heap->spans = span;
    heap->next_span += 1U;
    (void)atomic_fetch_add_explicit(
        &heap->committed_spans, 1U, memory_order_relaxed);
    (void)pthread_mutex_unlock(&heap->arena_lock);
    return span;
}

/* Caller holds class_locks[class_index]. */
static bool
memx_heap_create_span_locked(memx_heap_t *heap, size_t class_index) {
    const size_t alignment = _Alignof(max_align_t);
    size_t header_size = memx_heap_align_up(sizeof(memx_heap_span_t), alignment);
    size_t stride = memx_heap_align_up(
        sizeof(memx_heap_block_t) + memx_heap_class_sizes[class_index],
        alignment);
    size_t block_count;
    size_t index;
    size_t chunk_count = 0U;
    memx_heap_span_t *span;
    memx_heap_block_t *chunk_head = NULL;
    memx_heap_block_t *chunk_tail = NULL;

    if (header_size == 0U || stride == 0U
        || header_size >= heap->span_size) {
        return false;
    }
    block_count = (heap->span_size - header_size) / stride;
    if (block_count == 0U) {
        return false;
    }
    span = memx_heap_claim_span(
        heap, class_index, header_size, stride, block_count);
    if (span == NULL) {
        return false;
    }
    for (index = 0U; index < block_count; ++index) {
        memx_heap_block_t *block = (memx_heap_block_t *)(void *)
            ((unsigned char *)span->base + index * stride);
        memset(block, 0, sizeof(*block));
        atomic_init(&block->fields.state, 0U);
        block->fields.link.next = chunk_head;
        chunk_head = block;
        if (chunk_tail == NULL) {
            chunk_tail = block;
        }
        chunk_count += 1U;
        if (chunk_count == heap->config.cache_batch) {
            memx_heap_push_central_chunk_locked(
                heap, class_index, chunk_head, chunk_tail, chunk_count);
            chunk_head = NULL;
            chunk_tail = NULL;
            chunk_count = 0U;
        }
    }
    if (chunk_count != 0U) {
        memx_heap_push_central_chunk_locked(
            heap, class_index, chunk_head, chunk_tail, chunk_count);
    }
    return true;
}

static void
memx_heap_drain_remote(memx_heap_cache_t *cache, size_t class_index) {
    memx_heap_block_t *block;
    (void)pthread_mutex_lock(&cache->remote_lock);
    block = cache->remote[class_index];
    while (block != NULL) {
        memx_heap_block_t *next = block->fields.link.next;
        if (cache->local[class_index] == NULL) {
            cache->local_tail[class_index] = block;
        }
        block->fields.link.next = cache->local[class_index];
        cache->local[class_index] = block;
        cache->local_count[class_index] += 1U;
        block = next;
    }
    cache->remote[class_index] = NULL;
    cache->remote_count[class_index] = 0U;
    (void)pthread_mutex_unlock(&cache->remote_lock);
}

static MEMX_HEAP_NOINLINE void
memx_heap_flush_excess_slow(
    memx_heap_t *heap,
    memx_heap_cache_t *cache,
    size_t class_index) {
    memx_heap_block_t *first = cache->spare[class_index];
    memx_heap_block_t *tail = cache->spare_tail[class_index];
    size_t moved = cache->spare_count[class_index];

    /* The filled segment becomes the spare and the previous spare, if any,
     * goes back to the central bin as one splice. The thread therefore keeps
     * up to cache_limit blocks and never walks the list to find a split
     * point. */
    if (first != NULL && tail != NULL && moved != 0U
#if SIZE_MAX > UINT32_MAX
        && moved <= (size_t)UINT32_MAX
#endif
        ) {
        (void)pthread_mutex_lock(&heap->class_locks[class_index]);
        memx_heap_push_central_chunk_locked(
            heap, class_index, first, tail, moved);
        (void)pthread_mutex_unlock(&heap->class_locks[class_index]);
    }
    cache->spare[class_index] = cache->local[class_index];
    cache->spare_tail[class_index] = cache->local_tail[class_index];
    cache->spare_count[class_index] = cache->local_count[class_index];
    cache->local[class_index] = NULL;
    cache->local_tail[class_index] = NULL;
    cache->local_count[class_index] = 0U;
}

/* Promote the spare segment when the hot segment runs dry. O(1) and needs no
 * lock, so an allocation only reaches the central bin once both segments are
 * empty. */
static bool
memx_heap_adopt_spare(memx_heap_cache_t *cache, size_t class_index) {
    if (cache->spare[class_index] == NULL) {
        return false;
    }
    cache->local[class_index] = cache->spare[class_index];
    cache->local_tail[class_index] = cache->spare_tail[class_index];
    cache->local_count[class_index] = cache->spare_count[class_index];
    cache->spare[class_index] = NULL;
    cache->spare_tail[class_index] = NULL;
    cache->spare_count[class_index] = 0U;
    return true;
}

static MEMX_HEAP_NOINLINE memx_heap_block_t *
memx_heap_refill(
    memx_heap_t *heap,
    memx_heap_cache_t *cache,
    size_t class_index) {
    memx_heap_block_t *first;
    memx_heap_block_t *tail;
    memx_heap_central_chunk_t *chunk;
    size_t moved;

    if (memx_heap_adopt_spare(cache, class_index)) {
        return cache->local[class_index];
    }
    memx_heap_drain_remote(cache, class_index);
    if (cache->local[class_index] != NULL) {
        return cache->local[class_index];
    }
    if (heap->config.collect_activity_statistics) {
        cache->cache_misses += 1U;
    }
    (void)pthread_mutex_lock(&heap->class_locks[class_index]);
    if (heap->central[class_index] == NULL
        && !memx_heap_create_span_locked(heap, class_index)) {
        (void)pthread_mutex_unlock(&heap->class_locks[class_index]);
        return NULL;
    }
    first = heap->central[class_index];
    chunk = (memx_heap_central_chunk_t *)(void *)(first + 1);
    tail = chunk->tail;
    moved = first->fields.requested_size;
    heap->central[class_index] = chunk->next;
    tail->fields.link.next = cache->local[class_index];
    if (cache->local[class_index] == NULL) {
        cache->local_tail[class_index] = tail;
    }
    heap->central_count[class_index] -= moved;
    cache->local[class_index] = first;
    cache->local_count[class_index] += moved;
    (void)pthread_mutex_unlock(&heap->class_locks[class_index]);
    return cache->local[class_index];
}

static void *
memx_heap_allocate_small(
    memx_heap_t *heap,
    size_t size,
    size_t class_index) {
    memx_heap_cache_t *cache = memx_heap_get_cache(heap);
    memx_heap_block_t *block;
    bool cache_hit;

    assert(class_index < MEMX_HEAP_CLASS_COUNT);
    if (cache == NULL) {
        return NULL;
    }
    block = cache->local[class_index];
    cache_hit = block != NULL;
    if (block == NULL) {
        block = memx_heap_refill(heap, cache, class_index);
        if (block == NULL) {
            return NULL;
        }
    }
    cache->local[class_index] = block->fields.link.next;
    if (block->fields.link.next == NULL) {
        cache->local_tail[class_index] = NULL;
    }
#if defined(__GNUC__) || defined(__clang__)
    if (cache->local[class_index] != NULL) {
        __builtin_prefetch(cache->local[class_index], 1, 3);
    }
#endif
    cache->local_count[class_index] -= 1U;
    block->fields.link.owner = cache;
    block->fields.requested_size = (uint32_t)size;
#ifndef NDEBUG
    assert(atomic_load_explicit(
        &block->fields.state, memory_order_relaxed) == 0U);
#endif
    atomic_store_explicit(&block->fields.state, 1U, memory_order_release);
    if (heap->config.collect_activity_statistics) {
        cache->cache_hits += cache_hit ? 1U : 0U;
        cache->allocation_count += 1U;
        memx_heap_add_live_counted(heap, size);
    }
    return (void *)(block + 1);
}

/* Hash the address without reading caller memory, including invalid pointers.
 * All bucket/chain access and rehashing is serialized by admin_lock. */
static size_t
memx_heap_large_bucket(const void *pointer, size_t bucket_count) {
    uint64_t value = (uint64_t)(uintptr_t)pointer;
    value ^= value >> 33U;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33U;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33U;
    return (size_t)value & (bucket_count - 1U);
}

static bool
memx_heap_large_rehash_locked(memx_heap_t *heap, size_t bucket_count) {
    memx_heap_large_t **buckets;
    size_t index;
    if (bucket_count > SIZE_MAX / sizeof(*buckets)) {
        return false;
    }
    buckets = calloc(bucket_count, sizeof(*buckets));
    if (buckets == NULL) {
        return false;
    }
    for (index = 0U; index < heap->large_bucket_count; ++index) {
        memx_heap_large_t *large = heap->large_buckets[index];
        while (large != NULL) {
            memx_heap_large_t *next = large->next;
            const size_t bucket = memx_heap_large_bucket(
                large->pointer, bucket_count);
            large->next = buckets[bucket];
            buckets[bucket] = large;
            large = next;
        }
    }
    free(heap->large_buckets);
    heap->large_buckets = buckets;
    heap->large_bucket_count = bucket_count;
    return true;
}

static bool
memx_heap_large_reserve_locked(memx_heap_t *heap) {
    size_t capacity = heap->large_bucket_count;
    if (heap->large_count < capacity) {
        return true;
    }
    if (capacity > SIZE_MAX / 2U) {
        return false;
    }
    capacity = capacity == 0U ? MEMX_HEAP_LARGE_MIN_BUCKETS : capacity * 2U;
    return memx_heap_large_rehash_locked(heap, capacity);
}

static void
memx_heap_large_shrink_locked(memx_heap_t *heap) {
    if (heap->large_count == 0U) {
        free(heap->large_buckets);
        heap->large_buckets = NULL;
        heap->large_bucket_count = 0U;
    } else if (heap->large_bucket_count > MEMX_HEAP_LARGE_MIN_BUCKETS
        && heap->large_count <= heap->large_bucket_count / 4U) {
        /* Failure to shrink preserves the old table and cannot fail free. */
        (void)memx_heap_large_rehash_locked(heap, heap->large_bucket_count / 2U);
    }
}

static void *
memx_heap_allocate_large(
    memx_heap_t *heap,
    size_t alignment,
    size_t size) {
    memx_heap_large_t *large = calloc(1U, sizeof(*large));
    if (large == NULL) {
        return NULL;
    }
    if (!memx_os_large_allocate(
            size, alignment, &large->mapping, &large->pointer)) {
        free(large);
        return NULL;
    }
    large->requested_size = size;
    large->alignment = alignment;
    (void)pthread_mutex_lock(&heap->admin_lock);
    if (!memx_heap_large_reserve_locked(heap)) {
        (void)pthread_mutex_unlock(&heap->admin_lock);
        memx_os_release(large->mapping);
        free(large);
        return NULL;
    }
    {
        const size_t bucket = memx_heap_large_bucket(
            large->pointer, heap->large_bucket_count);
        large->next = heap->large_buckets[bucket];
        heap->large_buckets[bucket] = large;
        heap->large_count += 1U;
    }
    (void)pthread_mutex_unlock(&heap->admin_lock);
    if (heap->config.collect_activity_statistics) {
        (void)atomic_fetch_add_explicit(
            &heap->large_allocation_count, 1U, memory_order_relaxed);
    }
    memx_heap_add_live(heap, size);
    return large->pointer;
}

static memx_heap_block_t *
memx_heap_small_block_locked(
    memx_heap_t *heap,
    const void *pointer,
    memx_heap_span_t **out_span) {
    const uintptr_t address = (uintptr_t)pointer;
    const uintptr_t arena = (uintptr_t)heap->arena.base;
    memx_handle_t handle;
    memx_heap_span_t *span;
    uintptr_t span_base;
    uintptr_t header_address;
    size_t offset;

    if (address < arena || address - arena >= heap->arena.size) {
        return NULL;
    }
    handle = memx_lookup_address(heap->index, address);
    if (handle == MEMX_HANDLE_INVALID) {
        return NULL;
    }
    span = (memx_heap_span_t *)(uintptr_t)handle;
    if (span == NULL || span->magic != MEMX_HEAP_SPAN_MAGIC
        || span->heap != heap || span->class_index >= MEMX_HEAP_CLASS_COUNT) {
        return NULL;
    }
    span_base = (uintptr_t)span->base;
    if (address < span_base + sizeof(memx_heap_block_t)) {
        return NULL;
    }
    header_address = address - sizeof(memx_heap_block_t);
    offset = (size_t)(header_address - span_base);
    if (offset % span->block_stride != 0U
        || offset / span->block_stride >= span->block_count
        || address != header_address + sizeof(memx_heap_block_t)) {
        return NULL;
    }
    {
        memx_heap_block_t *block = (memx_heap_block_t *)header_address;
        *out_span = span;
        return block;
    }
}

static memx_heap_large_t *
memx_heap_find_large_locked(
    memx_heap_t *heap,
    const void *pointer,
    memx_heap_large_t ***out_link) {
    memx_heap_large_t **link;
    if (heap->large_bucket_count == 0U) {
        return NULL;
    }
    link = &heap->large_buckets[memx_heap_large_bucket(
        pointer, heap->large_bucket_count)];
    while (*link != NULL) {
        memx_heap_large_t *large = *link;
        if (large->pointer == pointer) {
            if (out_link != NULL) {
                *out_link = link;
            }
            return large;
        }
        link = &large->next;
    }
    return NULL;
}

static size_t
memx_heap_requested_size(memx_heap_t *heap, const void *pointer) {
    memx_heap_span_t *span = NULL;
    memx_heap_block_t *block;
    memx_heap_large_t *large;
    size_t result = 0U;

    (void)pthread_mutex_lock(&heap->arena_lock);
    block = memx_heap_small_block_locked(heap, pointer, &span);
    if (block != NULL
        && atomic_load_explicit(
            &block->fields.state, memory_order_acquire) == 1U) {
        result = block->fields.requested_size;
    }
    (void)pthread_mutex_unlock(&heap->arena_lock);
    if (block == NULL) {
        (void)pthread_mutex_lock(&heap->admin_lock);
        large = memx_heap_find_large_locked(heap, pointer, NULL);
        result = large == NULL ? 0U : large->requested_size;
        (void)pthread_mutex_unlock(&heap->admin_lock);
    }
    return result;
}

static bool
memx_heap_resize_in_place(
    memx_heap_t *heap,
    void *pointer,
    size_t new_size) {
    memx_heap_span_t *span = NULL;
    memx_heap_block_t *block;
    memx_heap_large_t *large;
    size_t old_size = 0U;
    bool resized = false;

    (void)pthread_mutex_lock(&heap->arena_lock);
    block = memx_heap_small_block_locked(heap, pointer, &span);
    if (block != NULL
        && atomic_load_explicit(
            &block->fields.state, memory_order_acquire) == 1U
        && new_size <= memx_heap_class_sizes[span->class_index]) {
        old_size = block->fields.requested_size;
        block->fields.requested_size = (uint32_t)new_size;
        resized = true;
    }
    (void)pthread_mutex_unlock(&heap->arena_lock);
    if (block == NULL) {
        (void)pthread_mutex_lock(&heap->admin_lock);
        large = memx_heap_find_large_locked(heap, pointer, NULL);
        if (large != NULL && new_size <= large->requested_size) {
            old_size = large->requested_size;
            large->requested_size = new_size;
            resized = true;
        }
        (void)pthread_mutex_unlock(&heap->admin_lock);
    }

    if (resized && heap->config.collect_activity_statistics) {
        if (new_size > old_size) {
            memx_heap_add_live(heap, new_size - old_size);
        } else {
            (void)atomic_fetch_sub_explicit(
                &heap->live_requested_bytes,
                old_size - new_size,
                memory_order_relaxed);
        }
    }
    return resized;
}

static void
memx_heap_account_resize(
    memx_heap_t *heap,
    size_t old_size,
    size_t new_size) {
    if (!heap->config.collect_activity_statistics) {
        return;
    }
    if (new_size > old_size) {
        memx_heap_add_live(heap, new_size - old_size);
    } else {
        (void)atomic_fetch_sub_explicit(
            &heap->live_requested_bytes,
            old_size - new_size,
            memory_order_relaxed);
    }
}

static bool
memx_heap_init_locks(memx_heap_t *heap) {
    size_t index;
    if (pthread_mutex_init(&heap->admin_lock, NULL) != 0) {
        return false;
    }
    heap->initialized_class_locks = 0U;
    if (pthread_mutex_init(&heap->arena_lock, NULL) != 0) {
        (void)pthread_mutex_destroy(&heap->admin_lock);
        return false;
    }
    heap->initialized_class_locks = SIZE_MAX;
    for (index = 0U; index < MEMX_HEAP_CLASS_COUNT; ++index) {
        if (pthread_mutex_init(&heap->class_locks[index], NULL) != 0) {
            heap->initialized_class_locks = index;
            return false;
        }
    }
    return true;
}

static void
memx_heap_destroy_locks(memx_heap_t *heap) {
    size_t index;
    size_t initialized = heap->initialized_class_locks;
    if (initialized == 0U) {
        return;
    }
    if (initialized == SIZE_MAX) {
        initialized = MEMX_HEAP_CLASS_COUNT;
    }
    for (index = 0U; index < initialized; ++index) {
        (void)pthread_mutex_destroy(&heap->class_locks[index]);
    }
    (void)pthread_mutex_destroy(&heap->arena_lock);
    (void)pthread_mutex_destroy(&heap->admin_lock);
    heap->initialized_class_locks = 0U;
}

memx_heap_config_t
memx_heap_config_default(void) {
    memx_heap_config_t config;
    config.reserve_size = 256U * 1024U * 1024U;
    config.span_shift = 16U;
    config.cache_batch = 256U;
    config.cache_limit = 256U;
    config.collect_activity_statistics = false;
    return config;
}

memx_heap_status_t
memx_heap_create(
    const memx_heap_config_t *requested_config,
    memx_heap_t **out_heap) {
    memx_heap_config_t config = requested_config == NULL
        ? memx_heap_config_default() : *requested_config;
    memx_heap_t *heap;
    memx_config_t index_config;
    const unsigned size_bits = (unsigned)(sizeof(size_t) * CHAR_BIT);

    if (out_heap == NULL) {
        return MEMX_HEAP_ERROR_INVALID_ARGUMENT;
    }
    *out_heap = NULL;
    if (config.span_shift >= size_bits
        || config.cache_batch == 0U
#if SIZE_MAX > UINT32_MAX
        || config.cache_batch > (size_t)UINT32_MAX
#endif
        || config.cache_limit < 2U) {
        return MEMX_HEAP_ERROR_INVALID_ARGUMENT;
    }
    {
        const size_t span_size = (size_t)1U << config.span_shift;
        const size_t page_size = memx_os_page_size();
        const size_t span_header_size = memx_heap_align_up(
            sizeof(memx_heap_span_t), _Alignof(max_align_t));
        const size_t largest_block_stride = memx_heap_align_up(
            sizeof(memx_heap_block_t)
                + memx_heap_class_sizes[MEMX_HEAP_CLASS_COUNT - 1U],
            _Alignof(max_align_t));
        if (!memx_heap_power_of_two(page_size)
            || span_size < page_size || span_size % page_size != 0U
            || !memx_heap_power_of_two(span_size)
            || span_header_size == 0U || largest_block_stride == 0U
            || span_header_size > span_size
            || largest_block_stride > span_size - span_header_size
            || config.reserve_size == 0U
            || config.reserve_size % span_size != 0U) {
            return MEMX_HEAP_ERROR_INVALID_ARGUMENT;
        }
    }
    heap = calloc(1U, sizeof(*heap));
    if (heap == NULL) {
        return MEMX_HEAP_ERROR_OUT_OF_MEMORY;
    }
    heap->config = config;
    heap->span_size = (size_t)1U << config.span_shift;
    heap->span_capacity = config.reserve_size / heap->span_size;
    {
        const size_t huge_page_size = memx_os_huge_page_size();
        heap->commit_granularity =
            (huge_page_size > heap->span_size
                && config.reserve_size % huge_page_size == 0U)
            ? huge_page_size : heap->span_size;
    }
    if (!memx_heap_init_locks(heap)) {
        memx_heap_destroy_locks(heap);
        free(heap);
        return MEMX_HEAP_ERROR_OUT_OF_MEMORY;
    }
    heap->span_classes = calloc(heap->span_capacity, sizeof(*heap->span_classes));
    if (heap->span_classes == NULL) {
        memx_heap_destroy_locks(heap);
        free(heap);
        return MEMX_HEAP_ERROR_OUT_OF_MEMORY;
    }
    if (!memx_os_reserve_aligned(config.reserve_size,
            heap->commit_granularity, &heap->arena)) {
        free(heap->span_classes);
        memx_heap_destroy_locks(heap);
        free(heap);
        return MEMX_HEAP_ERROR_OUT_OF_MEMORY;
    }
    if (heap->commit_granularity != heap->span_size) {
        memx_os_advise_huge_pages(heap->arena.base, heap->arena.size);
    }
    index_config = memx_config_default();
    index_config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    index_config.base_address = (uintptr_t)heap->arena.base;
    index_config.managed_size = heap->arena.size;
    index_config.region_shift = config.span_shift;
    index_config.granule_shift = 12U;
    if (memx_index_create(&index_config, &heap->index) != MEMX_OK) {
        memx_os_release(heap->arena);
        free(heap->span_classes);
        memx_heap_destroy_locks(heap);
        free(heap);
        return MEMX_HEAP_ERROR_OUT_OF_MEMORY;
    }
    atomic_init(&heap->live_requested_bytes, 0U);
    atomic_init(&heap->peak_live_requested_bytes, 0U);
    atomic_init(&heap->committed_spans, 0U);
    atomic_init(&heap->large_allocation_count, 0U);
    atomic_init(&heap->frees, 0U);
    atomic_init(&heap->remote_frees, 0U);
    atomic_init(&heap->invalid_frees, 0U);
    atomic_init(&heap->thread_cache_count, 0U);
    *out_heap = heap;
    return MEMX_HEAP_OK;
}

void
memx_heap_thread_detach(memx_heap_t *heap) {
    memx_heap_cache_t **link;
    memx_heap_cache_t *cache;
    size_t class_index;

    if (heap == NULL) {
        return;
    }
    link = &memx_heap_tls_caches;
    while (*link != NULL && (*link)->heap != heap) {
        link = &(*link)->tls_next;
    }
    if (*link == NULL) {
        return;
    }
    cache = *link;
    *link = cache->tls_next;
    cache->tls_next = NULL;
    if (memx_heap_tls_last_cache == cache) {
        memx_heap_tls_last_cache = NULL;
    }

    (void)pthread_mutex_lock(&cache->remote_lock);
    cache->accepting_remote = false;
    for (class_index = 0U;
         class_index < MEMX_HEAP_CLASS_COUNT;
         ++class_index) {
        memx_heap_block_t *remote = cache->remote[class_index];
        while (remote != NULL) {
            memx_heap_block_t *next = remote->fields.link.next;
            remote->fields.link.next = cache->local[class_index];
            cache->local[class_index] = remote;
            cache->local_count[class_index] += 1U;
            remote = next;
        }
        cache->remote[class_index] = NULL;
        cache->remote_count[class_index] = 0U;
    }
    (void)pthread_mutex_unlock(&cache->remote_lock);

    for (class_index = 0U;
         class_index < MEMX_HEAP_CLASS_COUNT;
         ++class_index) {
        memx_heap_block_t *block;
        (void)pthread_mutex_lock(&heap->class_locks[class_index]);
        block = cache->local[class_index];
        while (block != NULL) {
            memx_heap_block_t *next = block->fields.link.next;
            memx_heap_push_central_locked(heap, class_index, block);
            block = next;
        }
        block = cache->spare[class_index];
        while (block != NULL) {
            memx_heap_block_t *next = block->fields.link.next;
            memx_heap_push_central_locked(heap, class_index, block);
            block = next;
        }
        (void)pthread_mutex_unlock(&heap->class_locks[class_index]);
        cache->local[class_index] = NULL;
        cache->local_tail[class_index] = NULL;
        cache->local_count[class_index] = 0U;
        cache->spare[class_index] = NULL;
        cache->spare_tail[class_index] = NULL;
        cache->spare_count[class_index] = 0U;
    }
}

void
memx_heap_destroy(memx_heap_t *heap) {
    size_t bucket;
    memx_heap_span_t *span;
    memx_heap_cache_t *cache;
    if (heap == NULL) {
        return;
    }
    memx_heap_thread_detach(heap);
    for (bucket = 0U; bucket < heap->large_bucket_count; ++bucket) {
        memx_heap_large_t *large = heap->large_buckets[bucket];
        while (large != NULL) {
            memx_heap_large_t *next = large->next;
            memx_os_release(large->mapping);
            free(large);
            large = next;
        }
    }
    free(heap->large_buckets);
    cache = heap->caches;
    while (cache != NULL) {
        memx_heap_cache_t *next = cache->heap_next;
        (void)pthread_mutex_destroy(&cache->remote_lock);
        free(cache);
        cache = next;
    }
    span = heap->spans;
    while (span != NULL) {
        memx_heap_span_t *next = span->next;
        span->magic = 0U;
        span = next;
    }
    memx_index_destroy(heap->index);
    memx_os_release(heap->arena);
    free(heap->span_classes);
    memx_heap_destroy_locks(heap);
    free(heap);
}

void *
memx_heap_malloc(memx_heap_t *heap, size_t requested_size) {
    size_t size = requested_size == 0U ? 1U : requested_size;
    size_t class_index;
    if (heap == NULL) {
        return NULL;
    }
    class_index = memx_heap_class_for_size(size);
    if (class_index < MEMX_HEAP_CLASS_COUNT) {
        return memx_heap_allocate_small(heap, size, class_index);
    }
    return memx_heap_allocate_large(heap, _Alignof(max_align_t), size);
}

void *
memx_heap_aligned_alloc(
    memx_heap_t *heap,
    size_t alignment,
    size_t requested_size) {
    size_t size = requested_size == 0U ? 1U : requested_size;
    size_t class_index;
    if (heap == NULL || alignment < sizeof(void *)
        || !memx_heap_power_of_two(alignment)) {
        return NULL;
    }
    class_index = memx_heap_class_for_size(size);
    if (alignment <= _Alignof(max_align_t)
        && class_index < MEMX_HEAP_CLASS_COUNT) {
        return memx_heap_allocate_small(heap, size, class_index);
    }
    return memx_heap_allocate_large(heap, alignment, size);
}

void *
memx_heap_calloc(memx_heap_t *heap, size_t count, size_t size) {
    size_t bytes;
    void *pointer;
    if (count != 0U && size > SIZE_MAX / count) {
        return NULL;
    }
    bytes = count * size;
    pointer = memx_heap_malloc(heap, bytes);
    if (pointer != NULL) {
        memset(pointer, 0, bytes);
    }
    return pointer;
}

size_t
memx_heap_usable_size(memx_heap_t *heap, const void *pointer) {
    memx_heap_span_t *span = NULL;
    memx_heap_block_t *block;
    memx_heap_large_t *large;
    size_t result = 0U;
    if (heap == NULL || pointer == NULL) {
        return 0U;
    }
    (void)pthread_mutex_lock(&heap->arena_lock);
    block = memx_heap_small_block_locked(heap, pointer, &span);
    if (block != NULL
        && atomic_load_explicit(
            &block->fields.state, memory_order_acquire) == 1U) {
        result = memx_heap_class_sizes[span->class_index];
    }
    (void)pthread_mutex_unlock(&heap->arena_lock);
    if (block == NULL) {
        (void)pthread_mutex_lock(&heap->admin_lock);
        large = memx_heap_find_large_locked(heap, pointer, NULL);
        result = large == NULL ? 0U : large->requested_size;
        (void)pthread_mutex_unlock(&heap->admin_lock);
    }
    return result;
}

static void
memx_heap_route_small(
    memx_heap_t *heap,
    memx_heap_block_t *block,
    size_t class_index) {
    memx_heap_cache_t *owner;
    memx_heap_cache_t *current;
    size_t requested_size;
    requested_size = block->fields.requested_size;
    owner = block->fields.link.owner;
    if (heap->config.collect_activity_statistics) {
        (void)atomic_fetch_sub_explicit(
            &heap->live_requested_bytes, requested_size, memory_order_relaxed);
        (void)atomic_fetch_add_explicit(
            &heap->frees, 1U, memory_order_relaxed);
    }

    current = memx_heap_tls_last_cache;
    if (owner != current) {
        current = memx_heap_find_cache(heap);
    }
    if (owner == current && current != NULL) {
        if (current->local[class_index] == NULL) {
            current->local_tail[class_index] = block;
        }
        block->fields.link.next = current->local[class_index];
        current->local[class_index] = block;
        current->local_count[class_index] += 1U;
        if (current->local_count[class_index]
            >= heap->config.cache_limit / 2U) {
            memx_heap_flush_excess_slow(heap, current, class_index);
        }
        return;
    }
    if (owner != NULL) {
        (void)pthread_mutex_lock(&owner->remote_lock);
        if (owner->accepting_remote) {
            block->fields.link.next = owner->remote[class_index];
            owner->remote[class_index] = block;
            owner->remote_count[class_index] += 1U;
            (void)pthread_mutex_unlock(&owner->remote_lock);
            if (heap->config.collect_activity_statistics) {
                (void)atomic_fetch_add_explicit(
                    &heap->remote_frees, 1U, memory_order_relaxed);
            }
            return;
        }
        (void)pthread_mutex_unlock(&owner->remote_lock);
    }
    (void)pthread_mutex_lock(&heap->class_locks[class_index]);
    memx_heap_push_central_locked(heap, class_index, block);
    (void)pthread_mutex_unlock(&heap->class_locks[class_index]);
}

static bool
memx_heap_release_small_checked(
    memx_heap_t *heap,
    memx_heap_block_t *block,
    size_t class_index) {
    unsigned expected = 1U;
    if (!atomic_compare_exchange_strong_explicit(
            &block->fields.state,
            &expected,
            0U,
            memory_order_acq_rel,
            memory_order_relaxed)) {
        if (heap->config.collect_activity_statistics) {
            (void)atomic_fetch_add_explicit(
                &heap->invalid_frees, 1U, memory_order_relaxed);
        }
        return false;
    }
    memx_heap_route_small(heap, block, class_index);
    return true;
}

static void
memx_heap_release_small_unchecked(
    memx_heap_t *heap,
    memx_heap_block_t *block,
    size_t class_index) {
    atomic_store_explicit(&block->fields.state, 0U, memory_order_release);
    memx_heap_route_small(heap, block, class_index);
}

/* Size class of a small allocation, read from a compact side table instead of
 * the span header. The table is one byte per span, so the whole arena's class
 * map stays cache resident; reading the span header would touch a second,
 * usually cold, page on every free. */
static size_t
memx_heap_class_for_arena_offset(const memx_heap_t *heap, uintptr_t offset) {
    return heap->span_classes[offset >> heap->config.span_shift];
}

bool
memx_heap_free(memx_heap_t *heap, void *pointer) {
    memx_heap_span_t *span = NULL;
    memx_heap_block_t *block;
    memx_heap_large_t *large;
    memx_heap_large_t **link = NULL;

    if (pointer == NULL) {
        return true;
    }
    if (heap == NULL) {
        return false;
    }
    (void)pthread_mutex_lock(&heap->arena_lock);
    block = memx_heap_small_block_locked(heap, pointer, &span);
    (void)pthread_mutex_unlock(&heap->arena_lock);
    if (block != NULL) {
        /* Span metadata is immutable once published, so reading the class
         * after releasing the arena lock is safe. */
        return memx_heap_release_small_checked(heap, block, span->class_index);
    }
    (void)pthread_mutex_lock(&heap->admin_lock);
    large = memx_heap_find_large_locked(heap, pointer, &link);
    if (large == NULL) {
        (void)pthread_mutex_unlock(&heap->admin_lock);
        if (heap->config.collect_activity_statistics) {
            (void)atomic_fetch_add_explicit(
                &heap->invalid_frees, 1U, memory_order_relaxed);
        }
        return false;
    }
    assert(link != NULL);
    *link = large->next;
    heap->large_count -= 1U;
    memx_heap_large_shrink_locked(heap);
    (void)pthread_mutex_unlock(&heap->admin_lock);
    if (heap->config.collect_activity_statistics) {
        (void)atomic_fetch_sub_explicit(
            &heap->live_requested_bytes,
            large->requested_size,
            memory_order_relaxed);
        (void)atomic_fetch_add_explicit(
            &heap->frees, 1U, memory_order_relaxed);
    }
    memx_os_release(large->mapping);
    free(large);
    return true;
}

void
memx_heap_free_unchecked(memx_heap_t *heap, void *pointer) {
    const uintptr_t address = (uintptr_t)pointer;
    uintptr_t arena;
    if (pointer == NULL) {
        return;
    }
    if (heap == NULL) {
        return;
    }
    arena = (uintptr_t)heap->arena.base;
    if (address >= arena && address - arena < heap->arena.size) {
        memx_heap_block_t *block = (memx_heap_block_t *)pointer - 1;
        memx_heap_release_small_unchecked(heap, block,
            memx_heap_class_for_arena_offset(heap, address - arena));
        return;
    }
    (void)memx_heap_free(heap, pointer);
}

void *
memx_heap_realloc(memx_heap_t *heap, void *pointer, size_t size) {
    size_t old_size;
    void *replacement;
    if (heap == NULL) {
        return NULL;
    }
    if (pointer == NULL) {
        return memx_heap_malloc(heap, size);
    }
    if (size == 0U) {
        (void)memx_heap_free(heap, pointer);
        return NULL;
    }
    if (memx_heap_resize_in_place(heap, pointer, size)) {
        return pointer;
    }
    old_size = memx_heap_requested_size(heap, pointer);
    if (old_size == 0U) {
        return NULL;
    }
    replacement = memx_heap_malloc(heap, size);
    if (replacement == NULL) {
        return NULL;
    }
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    if (!memx_heap_free(heap, pointer)) {
        memx_heap_free_unchecked(heap, replacement);
        return NULL;
    }
    return replacement;
}

void *
memx_heap_realloc_unchecked(memx_heap_t *heap, void *pointer, size_t size) {
    size_t old_size;
    void *replacement;
    uintptr_t address;
    uintptr_t arena;
    size_t class_index;
    memx_heap_block_t *block;

    if (heap == NULL) {
        return NULL;
    }
    if (pointer == NULL) {
        return memx_heap_malloc(heap, size);
    }
    if (size == 0U) {
        memx_heap_free_unchecked(heap, pointer);
        return NULL;
    }

    address = (uintptr_t)pointer;
    arena = (uintptr_t)heap->arena.base;
    if (address < arena || address - arena >= heap->arena.size) {
        return memx_heap_realloc(heap, pointer, size);
    }

    block = (memx_heap_block_t *)pointer - 1;
    class_index = memx_heap_class_for_arena_offset(heap, address - arena);
    old_size = (size_t)block->fields.requested_size;
    if (size <= memx_heap_class_sizes[class_index]) {
        block->fields.requested_size = (uint32_t)size;
        memx_heap_account_resize(heap, old_size, size);
        return pointer;
    }
    replacement = memx_heap_malloc(heap, size);
    if (replacement == NULL) {
        return NULL;
    }
    memcpy(replacement, pointer, old_size);
    memx_heap_free_unchecked(heap, pointer);
    return replacement;
}

void
memx_heap_get_stats(memx_heap_t *heap, memx_heap_stats_t *out_stats) {
    memx_heap_cache_t *cache;
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (heap == NULL) {
        return;
    }
    out_stats->arena_reserved_bytes = heap->arena.size;
    out_stats->committed_spans = atomic_load_explicit(
        &heap->committed_spans, memory_order_relaxed);
    (void)pthread_mutex_lock(&heap->arena_lock);
    out_stats->arena_committed_bytes = heap->committed_bytes;
    (void)pthread_mutex_unlock(&heap->arena_lock);
    out_stats->live_requested_bytes = atomic_load_explicit(
        &heap->live_requested_bytes, memory_order_relaxed);
    out_stats->peak_live_requested_bytes = atomic_load_explicit(
        &heap->peak_live_requested_bytes, memory_order_relaxed);
    (void)pthread_mutex_lock(&heap->admin_lock);
    out_stats->active_large_allocations = heap->large_count;
    out_stats->large_index_bytes = heap->large_bucket_count
        * sizeof(*heap->large_buckets);
    for (cache = heap->caches; cache != NULL; cache = cache->heap_next) {
        out_stats->small_allocations += cache->allocation_count;
        out_stats->cache_hits += cache->cache_hits;
        out_stats->cache_misses += cache->cache_misses;
    }
    (void)pthread_mutex_unlock(&heap->admin_lock);
    out_stats->large_allocations = atomic_load_explicit(
        &heap->large_allocation_count, memory_order_relaxed);
    out_stats->frees = atomic_load_explicit(&heap->frees, memory_order_relaxed);
    out_stats->remote_frees = atomic_load_explicit(
        &heap->remote_frees, memory_order_relaxed);
    out_stats->invalid_frees = atomic_load_explicit(
        &heap->invalid_frees, memory_order_relaxed);
    out_stats->thread_caches = atomic_load_explicit(
        &heap->thread_cache_count, memory_order_relaxed);
}

const char *
memx_heap_status_string(memx_heap_status_t status) {
    switch (status) {
        case MEMX_HEAP_OK:
            return "ok";
        case MEMX_HEAP_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case MEMX_HEAP_ERROR_OUT_OF_MEMORY:
            return "out of memory";
        case MEMX_HEAP_ERROR_UNSUPPORTED:
            return "unsupported";
        case MEMX_HEAP_ERROR_BUSY:
            return "busy";
        default:
            return "unknown status";
    }
}
