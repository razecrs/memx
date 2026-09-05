/*
 * Benchmark-only malloc interposer for the experimental MemX heap.
 *
 * It exists so MemX can be measured through the same LD_PRELOAD path as
 * snmalloc and mimalloc, running the identical benchmark binary. It is not a
 * production interposer: nested allocations made by the allocator itself, and
 * anything allocated before the heap exists, are served from a bump arena and
 * never reclaimed.
 */
#define _GNU_SOURCE
#include "memx/allocator.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

#define BOOTSTRAP_BYTES (64U * 1024U * 1024U)
#define BOOTSTRAP_ALIGNMENT 16U

typedef struct bootstrap_header {
    size_t requested_size;
} bootstrap_header_t;

static unsigned char *bootstrap_base;
static atomic_size_t bootstrap_used;
static _Atomic(memx_heap_t *) shim_heap;

static pthread_once_t shim_once = PTHREAD_ONCE_INIT;
/* Nested allocations from inside the allocator must not re-enter it.
 * initial-exec keeps this a single %fs-relative access; the shim is always
 * LD_PRELOADed at startup, never dlopened. */
#if defined(__GNUC__) || defined(__clang__)
static _Thread_local int shim_busy
    __attribute__((tls_model("initial-exec")));
#else
static _Thread_local int shim_busy;
#endif

static void *
bootstrap_alloc(size_t size, size_t alignment) {
    size_t current;
    size_t start;
    size_t payload_size;
    size_t needed;
    uintptr_t absolute;
    bootstrap_header_t *header;
    if (bootstrap_base == NULL) {
        return NULL;
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U
        || alignment < sizeof(void *)
        || size > SIZE_MAX - (BOOTSTRAP_ALIGNMENT - 1U)) {
        return NULL;
    }
    payload_size = (size + (BOOTSTRAP_ALIGNMENT - 1U))
        & ~(size_t)(BOOTSTRAP_ALIGNMENT - 1U);
    if (payload_size == 0U) {
        payload_size = BOOTSTRAP_ALIGNMENT;
    }
    for (;;) {
        current = atomic_load_explicit(&bootstrap_used, memory_order_relaxed);
        if (current > BOOTSTRAP_BYTES
            || current > SIZE_MAX - sizeof(*header)
            || current + sizeof(*header) > SIZE_MAX - (alignment - 1U)) {
            return NULL;
        }
        absolute = (uintptr_t)bootstrap_base + current + sizeof(*header);
        if (absolute > UINTPTR_MAX - (alignment - 1U)) {
            return NULL;
        }
        start = (size_t)(((absolute + alignment - 1U)
            & ~((uintptr_t)alignment - 1U)) - (uintptr_t)bootstrap_base);
        if (start > BOOTSTRAP_BYTES
            || start < current || payload_size > BOOTSTRAP_BYTES - start) {
            return NULL;
        }
        needed = start - current + payload_size;
        if (needed > BOOTSTRAP_BYTES - current) {
            return NULL;
        }
        if (atomic_compare_exchange_weak_explicit(&bootstrap_used, &current,
                current + needed, memory_order_relaxed, memory_order_relaxed)) {
            header = (bootstrap_header_t *)(void *)
                (bootstrap_base + start - sizeof(*header));
            header->requested_size = size;
            return bootstrap_base + start;
        }
    }
}

static bool
from_bootstrap(const void *pointer) {
    const uintptr_t p = (uintptr_t)pointer;
    const uintptr_t base = (uintptr_t)bootstrap_base;
    return bootstrap_base != NULL && p >= base
        && p - base < BOOTSTRAP_BYTES;
}

static bool
bootstrap_requested_size(const void *pointer, size_t *out_size) {
    const uintptr_t address = (uintptr_t)pointer;
    const uintptr_t base = (uintptr_t)bootstrap_base;
    bootstrap_header_t *header;
    if (!from_bootstrap(pointer) || address - base < sizeof(*header)) {
        return false;
    }
    header = (bootstrap_header_t *)(void *)(address - sizeof(*header));
    *out_size = header->requested_size;
    return true;
}

static void
shim_init(void) {
    memx_heap_config_t config = memx_heap_config_default();
    void *mapping = mmap(NULL, BOOTSTRAP_BYTES, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping != MAP_FAILED) {
        bootstrap_base = mapping;
    }
    shim_busy = 1;
    {
        memx_heap_t *heap = NULL;
        if (memx_heap_create(&config, &heap) != MEMX_HEAP_OK) {
            heap = NULL;
        }
        atomic_store_explicit(&shim_heap, heap, memory_order_release);
    }
    shim_busy = 0;
}

/* Keep pthread_once off the hot path: once shim_heap is published it never
 * changes, so a plain load decides the common case. */
static inline memx_heap_t *
shim_ready(void) {
    memx_heap_t *heap = atomic_load_explicit(
        &shim_heap, memory_order_acquire);
    if (heap != NULL) {
        return heap;
    }
    (void)pthread_once(&shim_once, shim_init);
    return atomic_load_explicit(&shim_heap, memory_order_acquire);
}

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
void *aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void **out, size_t alignment, size_t size);
void *memalign(size_t alignment, size_t size);
size_t malloc_usable_size(void *pointer);
void free_sized(void *pointer, size_t size);
void free_aligned_sized(void *pointer, size_t alignment, size_t size);

void *
malloc(size_t size) {
    memx_heap_t *heap;
    void *result;
    if (shim_busy) {
        return bootstrap_alloc(size, 16U);
    }
    heap = shim_ready();
    if (heap == NULL) {
        return bootstrap_alloc(size, 16U);
    }
    shim_busy = 1;
    result = memx_heap_malloc(heap, size);
    shim_busy = 0;
    return result;
}

void *
calloc(size_t count, size_t size) {
    memx_heap_t *heap;
    void *result;
    size_t bytes;
    if (count != 0U && size > (size_t)-1 / count) {
        return NULL;
    }
    bytes = count * size;
    if (shim_busy) {
        return bootstrap_alloc(bytes, 16U); /* mmap memory is already zero */
    }
    heap = shim_ready();
    if (heap == NULL) {
        return bootstrap_alloc(bytes, 16U);
    }
    shim_busy = 1;
    result = memx_heap_calloc(heap, count, size);
    shim_busy = 0;
    return result;
}

void
free(void *pointer) {
    memx_heap_t *heap;
    if (pointer == NULL || from_bootstrap(pointer)) {
        return;
    }
    heap = atomic_load_explicit(&shim_heap, memory_order_acquire);
    if (heap == NULL) {
        return;
    }
    shim_busy = 1;
    memx_heap_free_unchecked(heap, pointer);
    shim_busy = 0;
}

void
free_sized(void *pointer, size_t size) {
    (void)size;
    free(pointer);
}

void
free_aligned_sized(void *pointer, size_t alignment, size_t size) {
    (void)alignment;
    (void)size;
    free(pointer);
}

void *
realloc(void *pointer, size_t size) {
    memx_heap_t *heap;
    void *result;
    if (pointer != NULL && from_bootstrap(pointer)) {
        size_t old_size;
        void *replacement = malloc(size);
        if (replacement != NULL && bootstrap_requested_size(pointer, &old_size)) {
            memcpy(replacement, pointer, old_size < size ? old_size : size);
        }
        return replacement;
    }
    if (pointer == NULL) {
        return malloc(size);
    }
    heap = shim_ready();
    if (heap == NULL) {
        return NULL;
    }
    shim_busy = 1;
    result = memx_heap_realloc_unchecked(heap, pointer, size);
    shim_busy = 0;
    return result;
}

static void *
shim_aligned_allocate(size_t alignment, size_t size, bool exact_size) {
    memx_heap_t *heap;
    void *result;
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U
        || (exact_size && size % alignment != 0U)) {
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    if (shim_busy) {
        return bootstrap_alloc(size, alignment);
    }
    heap = shim_ready();
    if (heap == NULL) {
        return bootstrap_alloc(size, alignment);
    }
    shim_busy = 1;
    result = memx_heap_aligned_alloc(heap, alignment, size);
    shim_busy = 0;
    return result;
}

void *
aligned_alloc(size_t alignment, size_t size) {
    return shim_aligned_allocate(alignment, size, true);
}

void *
memalign(size_t alignment, size_t size) {
    return shim_aligned_allocate(alignment, size, false);
}

int
posix_memalign(void **out, size_t alignment, size_t size) {
    void *result;
    if (alignment < sizeof(void *)
        || (alignment & (alignment - 1U)) != 0U) {
        return EINVAL;
    }
    result = shim_aligned_allocate(alignment, size, false);
    if (result == NULL) {
        return ENOMEM;
    }
    *out = result;
    return 0;
}

size_t
malloc_usable_size(void *pointer) {
    memx_heap_t *heap;
    size_t result;
    if (pointer == NULL || from_bootstrap(pointer)) {
        return 0U;
    }
    heap = atomic_load_explicit(&shim_heap, memory_order_acquire);
    if (heap == NULL) {
        return 0U;
    }
    shim_busy = 1;
    result = memx_heap_usable_size(heap, pointer);
    shim_busy = 0;
    return result;
}
