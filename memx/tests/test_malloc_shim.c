/* Include libc declarations before renaming the interposer entry points. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <limits.h>

#define malloc memx_test_malloc
#define calloc memx_test_calloc
#define realloc memx_test_realloc
#define free memx_test_free
#define aligned_alloc memx_test_aligned_alloc
#define posix_memalign memx_test_posix_memalign
#define memalign memx_test_memalign
#define malloc_usable_size memx_test_malloc_usable_size
#define free_sized memx_test_free_sized
#define free_aligned_sized memx_test_free_aligned_sized
#include "../bench/memx_malloc_shim.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef aligned_alloc
#undef posix_memalign
#undef memalign
#undef malloc_usable_size
#undef free_sized
#undef free_aligned_sized

typedef struct reservation_job {
    void **results;
    size_t first;
} reservation_job_t;

static void *
reserve_worker(void *argument) {
    reservation_job_t *job = argument;
    size_t index;
    for (index = 0U; index < 128U; ++index) {
        job->results[job->first + index] = bootstrap_alloc(17U, 16U);
    }
    return NULL;
}

static int
check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "malloc shim test: %s\n", message);
        return 0;
    }
    return 1;
}

int
main(void) {
    unsigned char *base;
    unsigned char *source;
    unsigned char *replacement;
    void *results[1024];
    pthread_t threads[8];
    reservation_job_t jobs[8];
    size_t index;
    size_t created_threads = 0U;
    void *aligned_pointer;
    void *aligned_out;
    void *heap_pointer;
    size_t used_before;
    const size_t huge_alignment = (size_t)1U
        << (sizeof(size_t) * CHAR_BIT - 1U);
    int result = 1;

    base = mmap(NULL, BOOTSTRAP_BYTES, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return 2;
    }
    bootstrap_base = base;
    atomic_init(&bootstrap_used, 0U);

    source = bootstrap_alloc(8U, 16U);
    result &= check(source != NULL, "bootstrap source allocation");
    if (source != NULL) {
        memset(source, 0x11, 8U);
        memset(source + 8U, 0xa5, 8U);
        shim_busy = 1;
        replacement = memx_test_realloc(source, 32U);
        shim_busy = 0;
        result &= check(replacement != NULL, "bootstrap realloc");
        result &= check(replacement != NULL
                && replacement[0] == 0x11U && replacement[7] == 0x11U,
            "bootstrap realloc preserves payload");
        result &= check(replacement != NULL && replacement[8] != 0xa5U,
            "bootstrap realloc does not copy padding");
    }

    atomic_store(&bootstrap_used, 0U);
    result &= check(bootstrap_alloc(SIZE_MAX - 7U, 16U) == NULL,
        "bootstrap size overflow rejected");
    used_before = atomic_load(&bootstrap_used);
    result &= check(bootstrap_alloc(64U, 16U) != NULL,
        "bootstrap exhaustion setup");
    atomic_store(&bootstrap_used, BOOTSTRAP_BYTES - 8U);
    used_before = atomic_load(&bootstrap_used);
    result &= check(bootstrap_alloc(64U, 16U) == NULL
            && atomic_load(&bootstrap_used) == used_before,
        "bootstrap exhaustion preserves reservation counter");
    atomic_store(&bootstrap_used, 0U);
    used_before = atomic_load(&bootstrap_used);
    result &= check(bootstrap_alloc(1U, huge_alignment) == NULL
            && atomic_load(&bootstrap_used) == used_before,
        "huge alignment rejected without reservation");
    atomic_store(&bootstrap_used, 0U);
    shim_busy = 1;
    replacement = memx_test_aligned_alloc(1U, 1U);
    shim_busy = 0;
    result &= check(replacement != NULL
        && ((uintptr_t)replacement % sizeof(void *)) == 0U,
        "small aligned_alloc alignment strengthened");
    aligned_out = NULL;
    aligned_pointer = memx_test_posix_memalign(&aligned_out, 64U, 17U) == 0
        ? aligned_out : NULL;
    result &= check(aligned_pointer != NULL
            && ((uintptr_t)aligned_pointer % 64U) == 0U,
        "bootstrap posix_memalign allows arbitrary size");
    aligned_pointer = memx_test_memalign(64U, 17U);
    result &= check(aligned_pointer != NULL
            && ((uintptr_t)aligned_pointer % 64U) == 0U,
        "bootstrap memalign allows arbitrary size");

    shim_busy = 0;
    heap_pointer = memx_test_malloc(1U);
    result &= check(heap_pointer != NULL, "shim heap initialization");
    aligned_out = NULL;
    aligned_pointer = memx_test_posix_memalign(&aligned_out, 64U, 17U) == 0
        ? aligned_out : NULL;
    result &= check(aligned_pointer != NULL
            && ((uintptr_t)aligned_pointer % 64U) == 0U,
        "initialized posix_memalign allows arbitrary size");
    aligned_pointer = memx_test_memalign(64U, 17U);
    result &= check(aligned_pointer != NULL
            && ((uintptr_t)aligned_pointer % 64U) == 0U,
        "initialized memalign allows arbitrary size");
    result &= check(memx_test_calloc(SIZE_MAX, 2U) == NULL,
        "calloc overflow rejected");
    replacement = memx_test_calloc(4U, 8U);
    result &= check(replacement != NULL
            && ((unsigned char *)replacement)[0] == 0U
            && ((unsigned char *)replacement)[31] == 0U,
        "calloc zeroes initialized allocation");
    {
        unsigned char *large = memx_test_malloc(9000U);
        unsigned char *shrunk = large == NULL ? NULL
            : memx_test_realloc(large, 4000U);
        result &= check(shrunk == large, "large realloc shrinks in place");
        if (shrunk != NULL) {
            memset(shrunk, 0x3c, 4000U);
            result &= check(memx_test_realloc(shrunk, SIZE_MAX) == NULL
                    && ((unsigned char *)shrunk)[0] == 0x3c,
                "failed growth preserves original allocation");
            memx_test_free(shrunk);
        }
    }
    memx_test_free(heap_pointer);
    memx_test_free(aligned_pointer);
    memx_test_free(replacement);

    atomic_store(&bootstrap_used, 0U);
    for (index = 0U; index < 8U; ++index) {
        jobs[index].results = results;
        jobs[index].first = index * 128U;
        if (pthread_create(&threads[index], NULL, reserve_worker,
                &jobs[index]) != 0) {
            result &= check(0, "reservation thread create");
            break;
        }
        created_threads += 1U;
    }
    for (index = 0U; index < created_threads; ++index) {
        result &= check(pthread_join(threads[index], NULL) == 0,
            "reservation thread join");
    }
    for (index = 0U; index < created_threads * 128U; ++index) {
        result &= check(results[index] != NULL, "reservation succeeds");
        result &= check(((uintptr_t)results[index] % 16U) == 0U,
            "reservation alignment");
        for (size_t other = 0U; other < index; ++other) {
            result &= check(results[index] != results[other],
                "reservations do not overlap");
        }
    }
    {
        memx_heap_t *heap = atomic_load_explicit(&shim_heap,
            memory_order_acquire);
        if (heap != NULL) {
            memx_heap_thread_detach(heap);
            memx_heap_destroy(heap);
        }
    }
    (void)munmap(base, BOOTSTRAP_BYTES);
    return result ? 0 : 1;
}
