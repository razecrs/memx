#include "memx/allocator.h"

#include "internal.h"
#include "memx/memx.h"

#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MEMX_HEAP_CLASS_COUNT 10U
#define MEMX_HEAP_SPAN_MAGIC UINT64_C(0x6d656d787370616e)

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

_Static_assert(
    _Alignof(memx_heap_block_t) >= _Alignof(max_align_t),
    "small-allocation headers must preserve max_align_t alignment");
_Static_assert(
    sizeof(memx_heap_block_t) % _Alignof(max_align_t) == 0U,
    "small-allocation payloads must remain maximally aligned");

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
    size_t local_count[MEMX_HEAP_CLASS_COUNT];
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
    memx_index_t *index;
    pthread_mutex_t lock;
    memx_heap_block_t *central[MEMX_HEAP_CLASS_COUNT];
    size_t central_count[MEMX_HEAP_CLASS_COUNT];
    memx_heap_span_t *spans;
    memx_heap_large_t *large_allocations;
    memx_heap_cache_t *caches;

    atomic_size_t live_requested_bytes;
    atomic_size_t peak_live_requested_bytes;
    atomic_size_t committed_spans;
    atomic_size_t large_allocation_count;
    atomic_size_t frees;
    atomic_size_t remote_frees;
    atomic_size_t invalid_frees;
    atomic_size_t active_large_allocations;
    atomic_size_t thread_cache_count;
};

static const size_t memx_heap_class_sizes[MEMX_HEAP_CLASS_COUNT] = {
    16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U, 8192U
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

static memx_heap_span_t *
memx_heap_span_for_address(memx_heap_t *heap, uintptr_t address) {
    const uintptr_t arena = (uintptr_t)heap->arena.base;
    const uintptr_t offset = address - arena;
    const uintptr_t span_offset = offset
        & ~((uintptr_t)heap->span_size - 1U);
    return (memx_heap_span_t *)(void *)(arena + span_offset);
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
memx_heap_add_live(memx_heap_t *heap, size_t bytes) {
    size_t previous;
    if (!heap->config.collect_activity_statistics) {
        return;
    }
    previous = atomic_fetch_add_explicit(
        &heap->live_requested_bytes, bytes, memory_order_relaxed);
    memx_heap_update_peak(heap, previous + bytes);
}

static size_t
memx_heap_class_for_size(size_t size) {
    unsigned bit_width;
    size_t value;
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
#endif
    if (bit_width < 4U) {
        bit_width = 4U;
    }
    return (size_t)(bit_width - 4U);
}

static memx_heap_cache_t *
memx_heap_find_cache(memx_heap_t *heap) {
    memx_heap_cache_t *cache = memx_heap_tls_last_cache;
    if (cache != NULL && cache->heap == heap && cache->accepting_remote) {
        return cache;
    }
    for (cache = memx_heap_tls_caches;
         cache != NULL;
         cache = cache->tls_next) {
        if (cache->heap == heap && cache->accepting_remote) {
            memx_heap_tls_last_cache = cache;
            return cache;
        }
    }
    return NULL;
}

static memx_heap_cache_t *
memx_heap_get_cache(memx_heap_t *heap) {
    memx_heap_cache_t *cache = memx_heap_find_cache(heap);
    if (cache != NULL) {
        return cache;
    }
    cache = calloc(1U, sizeof(*cache));
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
    (void)pthread_mutex_lock(&heap->lock);
    cache->heap_next = heap->caches;
    heap->caches = cache;
    (void)pthread_mutex_unlock(&heap->lock);
    (void)atomic_fetch_add_explicit(
        &heap->thread_cache_count, 1U, memory_order_relaxed);
    return cache;
}

static void
memx_heap_push_central_locked(
    memx_heap_t *heap,
    size_t class_index,
    memx_heap_block_t *block) {
    block->fields.link.next = heap->central[class_index];
    heap->central[class_index] = block;
    heap->central_count[class_index] += 1U;
}

static memx_heap_block_t *
memx_heap_pop_central_locked(memx_heap_t *heap, size_t class_index) {
    memx_heap_block_t *block = heap->central[class_index];
    if (block != NULL) {
        heap->central[class_index] = block->fields.link.next;
        heap->central_count[class_index] -= 1U;
        block->fields.link.next = NULL;
    }
    return block;
}

static bool
memx_heap_create_span_locked(memx_heap_t *heap, size_t class_index) {
    const size_t alignment = _Alignof(max_align_t);
    size_t header_size = memx_heap_align_up(sizeof(memx_heap_span_t), alignment);
    size_t stride = memx_heap_align_up(
        sizeof(memx_heap_block_t) + memx_heap_class_sizes[class_index],
        alignment);
    size_t block_count;
    size_t index;
    unsigned char *base;
    memx_heap_span_t *span;
    memx_status_t status;

    if (header_size == 0U || stride == 0U
        || header_size >= heap->span_size
        || heap->next_span >= heap->span_capacity) {
        return false;
    }
    block_count = (heap->span_size - header_size) / stride;
    if (block_count == 0U) {
        return false;
    }
    base = (unsigned char *)heap->arena.base
        + heap->next_span * heap->span_size;
    if (!memx_os_commit(base, heap->span_size)) {
        return false;
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
    status = memx_insert(heap->index, (uintptr_t)base, heap->span_size,
        (memx_handle_t)(uintptr_t)span);
    if (status != MEMX_OK) {
        (void)memx_os_decommit(base, heap->span_size);
        return false;
    }
    for (index = 0U; index < block_count; ++index) {
        memx_heap_block_t *block = (memx_heap_block_t *)(void *)
            ((unsigned char *)span->base + index * stride);
        memset(block, 0, sizeof(*block));
        atomic_init(&block->fields.state, 0U);
        memx_heap_push_central_locked(heap, class_index, block);
    }
    heap->spans = span;
    heap->next_span += 1U;
    (void)atomic_fetch_add_explicit(
        &heap->committed_spans, 1U, memory_order_relaxed);
    return true;
}

static void
memx_heap_drain_remote(memx_heap_cache_t *cache, size_t class_index) {
    memx_heap_block_t *block;
    (void)pthread_mutex_lock(&cache->remote_lock);
    block = cache->remote[class_index];
    while (block != NULL) {
        memx_heap_block_t *next = block->fields.link.next;
        block->fields.link.next = cache->local[class_index];
        cache->local[class_index] = block;
        cache->local_count[class_index] += 1U;
        block = next;
    }
    cache->remote[class_index] = NULL;
    cache->remote_count[class_index] = 0U;
    (void)pthread_mutex_unlock(&cache->remote_lock);
}

static void
memx_heap_flush_excess(
    memx_heap_t *heap,
    memx_heap_cache_t *cache,
    size_t class_index) {
    if (cache->local_count[class_index] <= heap->config.cache_limit) {
        return;
    }
    (void)pthread_mutex_lock(&heap->lock);
    while (cache->local_count[class_index]
        > heap->config.cache_limit / 2U) {
        memx_heap_block_t *block = cache->local[class_index];
        if (block == NULL) {
            break;
        }
        cache->local[class_index] = block->fields.link.next;
        cache->local_count[class_index] -= 1U;
        memx_heap_push_central_locked(heap, class_index, block);
    }
    (void)pthread_mutex_unlock(&heap->lock);
}

static memx_heap_block_t *
memx_heap_refill(
    memx_heap_t *heap,
    memx_heap_cache_t *cache,
    size_t class_index) {
    size_t moved = 0U;
    memx_heap_block_t *block;

    memx_heap_drain_remote(cache, class_index);
    if (cache->local[class_index] != NULL) {
        return cache->local[class_index];
    }
    if (heap->config.collect_activity_statistics) {
        cache->cache_misses += 1U;
    }
    (void)pthread_mutex_lock(&heap->lock);
    if (heap->central[class_index] == NULL
        && !memx_heap_create_span_locked(heap, class_index)) {
        (void)pthread_mutex_unlock(&heap->lock);
        return NULL;
    }
    while (moved < heap->config.cache_batch) {
        block = memx_heap_pop_central_locked(heap, class_index);
        if (block == NULL) {
            break;
        }
        block->fields.link.next = cache->local[class_index];
        cache->local[class_index] = block;
        cache->local_count[class_index] += 1U;
        moved += 1U;
    }
    (void)pthread_mutex_unlock(&heap->lock);
    return cache->local[class_index];
}

static void *
memx_heap_allocate_small(
    memx_heap_t *heap,
    size_t size,
    size_t class_index) {
    memx_heap_cache_t *cache = memx_heap_get_cache(heap);
    memx_heap_block_t *block;

    if (cache == NULL || class_index >= MEMX_HEAP_CLASS_COUNT) {
        return NULL;
    }
    block = cache->local[class_index];
    if (block == NULL) {
        block = memx_heap_refill(heap, cache, class_index);
        if (block == NULL) {
            return NULL;
        }
    } else {
        if (heap->config.collect_activity_statistics) {
            cache->cache_hits += 1U;
        }
    }
    cache->local[class_index] = block->fields.link.next;
    cache->local_count[class_index] -= 1U;
    block->fields.link.owner = cache;
    block->fields.requested_size = (uint32_t)size;
    if (atomic_load_explicit(
            &block->fields.state, memory_order_relaxed) != 0U) {
        return NULL;
    }
    atomic_store_explicit(&block->fields.state, 1U, memory_order_release);
    if (heap->config.collect_activity_statistics) {
        cache->allocation_count += 1U;
    }
    memx_heap_add_live(heap, size);
    return (void *)(block + 1);
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
    (void)pthread_mutex_lock(&heap->lock);
    large->next = heap->large_allocations;
    heap->large_allocations = large;
    (void)pthread_mutex_unlock(&heap->lock);
    if (heap->config.collect_activity_statistics) {
        (void)atomic_fetch_add_explicit(
            &heap->large_allocation_count, 1U, memory_order_relaxed);
        (void)atomic_fetch_add_explicit(
            &heap->active_large_allocations, 1U, memory_order_relaxed);
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
    memx_heap_large_t **out_previous) {
    memx_heap_large_t *previous = NULL;
    memx_heap_large_t *large;
    for (large = heap->large_allocations;
         large != NULL;
         large = large->next) {
        if (large->pointer == pointer) {
            if (out_previous != NULL) {
                *out_previous = previous;
            }
            return large;
        }
        previous = large;
    }
    return NULL;
}

static size_t
memx_heap_requested_size(memx_heap_t *heap, const void *pointer) {
    memx_heap_span_t *span = NULL;
    memx_heap_block_t *block;
    memx_heap_large_t *large;
    size_t result = 0U;

    (void)pthread_mutex_lock(&heap->lock);
    block = memx_heap_small_block_locked(heap, pointer, &span);
    if (block != NULL
        && atomic_load_explicit(
            &block->fields.state, memory_order_acquire) == 1U) {
        result = block->fields.requested_size;
    } else if (block == NULL) {
        large = memx_heap_find_large_locked(heap, pointer, NULL);
        result = large == NULL ? 0U : large->requested_size;
    }
    (void)pthread_mutex_unlock(&heap->lock);
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

    (void)pthread_mutex_lock(&heap->lock);
    block = memx_heap_small_block_locked(heap, pointer, &span);
    if (block != NULL
        && atomic_load_explicit(
            &block->fields.state, memory_order_acquire) == 1U
        && new_size <= memx_heap_class_sizes[span->class_index]) {
        old_size = block->fields.requested_size;
        block->fields.requested_size = (uint32_t)new_size;
        resized = true;
    } else if (block == NULL) {
        large = memx_heap_find_large_locked(heap, pointer, NULL);
        if (large != NULL && new_size <= large->requested_size) {
            old_size = large->requested_size;
            large->requested_size = new_size;
            resized = true;
        }
    }
    (void)pthread_mutex_unlock(&heap->lock);

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

memx_heap_config_t
memx_heap_config_default(void) {
    memx_heap_config_t config;
    config.reserve_size = 256U * 1024U * 1024U;
    config.span_shift = 16U;
    config.cache_batch = 128U;
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
        || config.cache_batch == 0U || config.cache_limit < 2U) {
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
    if (pthread_mutex_init(&heap->lock, NULL) != 0) {
        free(heap);
        return MEMX_HEAP_ERROR_OUT_OF_MEMORY;
    }
    if (!memx_os_reserve_aligned(config.reserve_size,
            heap->span_size, &heap->arena)) {
        (void)pthread_mutex_destroy(&heap->lock);
        free(heap);
        return MEMX_HEAP_ERROR_OUT_OF_MEMORY;
    }
    index_config = memx_config_default();
    index_config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    index_config.base_address = (uintptr_t)heap->arena.base;
    index_config.managed_size = heap->arena.size;
    index_config.region_shift = config.span_shift;
    index_config.granule_shift = 12U;
    if (memx_index_create(&index_config, &heap->index) != MEMX_OK) {
        memx_os_release(heap->arena);
        (void)pthread_mutex_destroy(&heap->lock);
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
    atomic_init(&heap->active_large_allocations, 0U);
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

    (void)pthread_mutex_lock(&heap->lock);
    for (class_index = 0U;
         class_index < MEMX_HEAP_CLASS_COUNT;
         ++class_index) {
        memx_heap_block_t *block = cache->local[class_index];
        while (block != NULL) {
            memx_heap_block_t *next = block->fields.link.next;
            memx_heap_push_central_locked(heap, class_index, block);
            block = next;
        }
        cache->local[class_index] = NULL;
        cache->local_count[class_index] = 0U;
    }
    (void)pthread_mutex_unlock(&heap->lock);
}

void
memx_heap_destroy(memx_heap_t *heap) {
    memx_heap_large_t *large;
    memx_heap_span_t *span;
    memx_heap_cache_t *cache;
    if (heap == NULL) {
        return;
    }
    memx_heap_thread_detach(heap);
    large = heap->large_allocations;
    while (large != NULL) {
        memx_heap_large_t *next = large->next;
        memx_os_release(large->mapping);
        free(large);
        large = next;
    }
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
    (void)pthread_mutex_destroy(&heap->lock);
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
    (void)pthread_mutex_lock(&heap->lock);
    block = memx_heap_small_block_locked(heap, pointer, &span);
    if (block != NULL
        && atomic_load_explicit(
            &block->fields.state, memory_order_acquire) == 1U) {
        result = memx_heap_class_sizes[span->class_index];
    } else if (block == NULL) {
        large = memx_heap_find_large_locked(heap, pointer, NULL);
        result = large == NULL ? 0U : large->requested_size;
    }
    (void)pthread_mutex_unlock(&heap->lock);
    return result;
}

static bool
memx_heap_release_small(
    memx_heap_t *heap,
    memx_heap_block_t *block,
    memx_heap_span_t *span,
    bool checked) {
    memx_heap_cache_t *owner;
    memx_heap_cache_t *current;
    size_t requested_size;
    unsigned expected = 1U;

    if (checked && !atomic_compare_exchange_strong_explicit(
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
    if (!checked) {
        atomic_store_explicit(&block->fields.state, 0U, memory_order_release);
    }
    requested_size = block->fields.requested_size;
    owner = block->fields.link.owner;
    if (heap->config.collect_activity_statistics) {
        (void)atomic_fetch_sub_explicit(
            &heap->live_requested_bytes, requested_size, memory_order_relaxed);
        (void)atomic_fetch_add_explicit(
            &heap->frees, 1U, memory_order_relaxed);
    }

    current = memx_heap_find_cache(heap);
    if (owner == current && current != NULL) {
        block->fields.link.next = current->local[span->class_index];
        current->local[span->class_index] = block;
        current->local_count[span->class_index] += 1U;
        memx_heap_flush_excess(heap, current, span->class_index);
        return true;
    }
    if (owner != NULL) {
        (void)pthread_mutex_lock(&owner->remote_lock);
        if (owner->accepting_remote) {
            block->fields.link.next = owner->remote[span->class_index];
            owner->remote[span->class_index] = block;
            owner->remote_count[span->class_index] += 1U;
            (void)pthread_mutex_unlock(&owner->remote_lock);
            if (heap->config.collect_activity_statistics) {
                (void)atomic_fetch_add_explicit(
                    &heap->remote_frees, 1U, memory_order_relaxed);
            }
            return true;
        }
        (void)pthread_mutex_unlock(&owner->remote_lock);
    }
    (void)pthread_mutex_lock(&heap->lock);
    memx_heap_push_central_locked(heap, span->class_index, block);
    (void)pthread_mutex_unlock(&heap->lock);
    return true;
}

bool
memx_heap_free(memx_heap_t *heap, void *pointer) {
    memx_heap_span_t *span = NULL;
    memx_heap_block_t *block;
    memx_heap_large_t *large;
    memx_heap_large_t *previous = NULL;

    if (pointer == NULL) {
        return true;
    }
    if (heap == NULL) {
        return false;
    }
    (void)pthread_mutex_lock(&heap->lock);
    block = memx_heap_small_block_locked(heap, pointer, &span);
    if (block == NULL) {
        large = memx_heap_find_large_locked(heap, pointer, &previous);
        if (large == NULL) {
            (void)pthread_mutex_unlock(&heap->lock);
            if (heap->config.collect_activity_statistics) {
                (void)atomic_fetch_add_explicit(
                    &heap->invalid_frees, 1U, memory_order_relaxed);
            }
            return false;
        }
        if (previous == NULL) {
            heap->large_allocations = large->next;
        } else {
            previous->next = large->next;
        }
        (void)pthread_mutex_unlock(&heap->lock);
        if (heap->config.collect_activity_statistics) {
            (void)atomic_fetch_sub_explicit(
                &heap->live_requested_bytes,
                large->requested_size,
                memory_order_relaxed);
            (void)atomic_fetch_sub_explicit(
                &heap->active_large_allocations, 1U, memory_order_relaxed);
            (void)atomic_fetch_add_explicit(
                &heap->frees, 1U, memory_order_relaxed);
        }
        memx_os_release(large->mapping);
        free(large);
        return true;
    }
    (void)pthread_mutex_unlock(&heap->lock);
    return memx_heap_release_small(heap, block, span, true);
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
        memx_heap_span_t *span = memx_heap_span_for_address(heap, address);
        (void)memx_heap_release_small(heap, block, span, false);
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
    memx_heap_block_t *block;
    memx_heap_span_t *span;

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
    span = memx_heap_span_for_address(heap, address);
    old_size = (size_t)block->fields.requested_size;
    if (size <= memx_heap_class_sizes[span->class_index]) {
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
    out_stats->arena_committed_bytes = out_stats->committed_spans
        * heap->span_size;
    out_stats->live_requested_bytes = atomic_load_explicit(
        &heap->live_requested_bytes, memory_order_relaxed);
    out_stats->peak_live_requested_bytes = atomic_load_explicit(
        &heap->peak_live_requested_bytes, memory_order_relaxed);
    (void)pthread_mutex_lock(&heap->lock);
    for (cache = heap->caches; cache != NULL; cache = cache->heap_next) {
        out_stats->small_allocations += cache->allocation_count;
        out_stats->cache_hits += cache->cache_hits;
        out_stats->cache_misses += cache->cache_misses;
    }
    (void)pthread_mutex_unlock(&heap->lock);
    out_stats->large_allocations = atomic_load_explicit(
        &heap->large_allocation_count, memory_order_relaxed);
    out_stats->frees = atomic_load_explicit(&heap->frees, memory_order_relaxed);
    out_stats->remote_frees = atomic_load_explicit(
        &heap->remote_frees, memory_order_relaxed);
    out_stats->invalid_frees = atomic_load_explicit(
        &heap->invalid_frees, memory_order_relaxed);
    out_stats->active_large_allocations = atomic_load_explicit(
        &heap->active_large_allocations, memory_order_relaxed);
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
