#ifndef MEMX_ALLOCATOR_H
#define MEMX_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct memx_heap memx_heap_t;

typedef enum memx_heap_status {
    MEMX_HEAP_OK = 0,
    MEMX_HEAP_ERROR_INVALID_ARGUMENT,
    MEMX_HEAP_ERROR_OUT_OF_MEMORY,
    MEMX_HEAP_ERROR_UNSUPPORTED,
    MEMX_HEAP_ERROR_BUSY
} memx_heap_status_t;

typedef struct memx_heap_config {
    /*
     * Reserved contiguous arena. Must be a multiple of 2^span_shift; a span
     * must fit its metadata plus one header and the largest (8192-byte) class.
     */
    size_t reserve_size;
    unsigned span_shift;

    /* Objects moved between a central bin and a thread cache per refill. */
    size_t cache_batch;

    /* Maximum cached free objects per class and thread. */
    size_t cache_limit;

    /* Exact activity/byte counters add synchronization to hot operations. */
    bool collect_activity_statistics;
} memx_heap_config_t;

typedef struct memx_heap_stats {
    size_t arena_reserved_bytes;
    size_t arena_committed_bytes;
    size_t live_requested_bytes;
    size_t peak_live_requested_bytes;
    size_t committed_spans;
    size_t small_allocations;
    size_t large_allocations;
    size_t frees;
    size_t remote_frees;
    size_t cache_hits;
    size_t cache_misses;
    size_t invalid_frees;
    size_t active_large_allocations;
    size_t thread_caches;
} memx_heap_stats_t;

memx_heap_config_t memx_heap_config_default(void);

memx_heap_status_t memx_heap_create(
    const memx_heap_config_t *config,
    memx_heap_t **out_heap);

/*
 * The caller must stop all threads using the heap and detach their caches
 * before destruction. MemX v0 intentionally does not provide concurrent heap
 * destruction or concurrent index reclamation.
 */
void memx_heap_destroy(memx_heap_t *heap);

/* A zero-size malloc/calloc request is represented by a live 1-byte object. */
void *memx_heap_malloc(memx_heap_t *heap, size_t size);
void *memx_heap_calloc(memx_heap_t *heap, size_t count, size_t size);

/*
 * A null pointer behaves like malloc. A zero size frees a live pointer and
 * returns null. On allocation failure, the original allocation and its
 * requested payload remain unchanged.
 */
void *memx_heap_realloc(memx_heap_t *heap, void *pointer, size_t size);

/*
 * Fast conventional realloc path. Pointer must be null or a currently live
 * base pointer returned by this heap; violating that contract is undefined.
 * The null, zero-size, failure, and payload-preservation semantics otherwise
 * match memx_heap_realloc().
 */
void *memx_heap_realloc_unchecked(
    memx_heap_t *heap,
    void *pointer,
    size_t size);

/* Alignment must be a nonzero power of two and at least sizeof(void *). */
void *memx_heap_aligned_alloc(
    memx_heap_t *heap,
    size_t alignment,
    size_t size);

/* Returns false for a pointer not owned by heap or for a duplicate free. */
bool memx_heap_free(memx_heap_t *heap, void *pointer);

/*
 * Fast conventional free path. Pointer must be null or a currently live base
 * pointer returned by this heap; violating that contract is undefined.
 * Checked ownership and duplicate detection remain available via
 * memx_heap_free().
 */
void memx_heap_free_unchecked(memx_heap_t *heap, void *pointer);

/*
 * Returns the size-class capacity for a live small allocation and the
 * requested size for a live large allocation. Returns zero for other
 * pointers. Realloc preserves only the originally requested byte count.
 */
size_t memx_heap_usable_size(memx_heap_t *heap, const void *pointer);

/*
 * Flush the calling thread's local/remote cache into the heap's central bins.
 * The cache object remains allocated until heap destruction so concurrent
 * remote frees never observe reclaimed cache metadata.
 */
void memx_heap_thread_detach(memx_heap_t *heap);

/* Call after allocator threads are quiescent or from the sole active thread. */
void memx_heap_get_stats(memx_heap_t *heap, memx_heap_stats_t *out_stats);

const char *memx_heap_status_string(memx_heap_status_t status);

#ifdef __cplusplus
}
#endif

#endif
