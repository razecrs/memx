#include "memx/allocator.h"
#include "memx/memx.h"

#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>

#define MEMX_THP_THREADS 8U
#define MEMX_THP_BASE ((uintptr_t)0x40000000U)
#define MEMX_THP_REGION_SIZE ((size_t)1U << 21U)

typedef struct memx_thp_test_state {
    atomic_uint failures;
    atomic_uint ready;
    atomic_bool go;
} memx_thp_test_state_t;

static void
memx_thp_wait_for_start(memx_thp_test_state_t *state) {
    (void)atomic_fetch_add_explicit(&state->ready, 1U, memory_order_release);
    while (!atomic_load_explicit(&state->go, memory_order_acquire)) {
    }
}

static void *
memx_thp_heap_worker(void *argument) {
    memx_thp_test_state_t *state = argument;
    memx_heap_t *heap = NULL;
    memx_thp_wait_for_start(state);
    if (memx_heap_create(NULL, &heap) != MEMX_HEAP_OK || heap == NULL) {
        (void)atomic_fetch_add_explicit(
            &state->failures, 1U, memory_order_relaxed);
        return NULL;
    }
    memx_heap_destroy(heap);
    return NULL;
}

static void *
memx_thp_overlay_worker(void *argument) {
    memx_thp_test_state_t *state = argument;
    memx_config_t config = memx_config_default();
    memx_index_t *index = NULL;
    memx_thp_wait_for_start(state);
    config.base_address = MEMX_THP_BASE;
    config.managed_size = MEMX_THP_REGION_SIZE;
    config.overlay_huge_pages = true;
    if (memx_index_create(&config, &index) != MEMX_OK || index == NULL) {
        (void)atomic_fetch_add_explicit(
            &state->failures, 1U, memory_order_relaxed);
        return NULL;
    }
    if (memx_insert(index, MEMX_THP_BASE, MEMX_THP_REGION_SIZE, 1U)
            != MEMX_OK
        || memx_index_optimize_overlay(index) != MEMX_OK) {
        (void)atomic_fetch_add_explicit(
            &state->failures, 1U, memory_order_relaxed);
    }
    memx_index_destroy(index);
    return NULL;
}

int
main(void) {
#if defined(__linux__)
    pthread_t heap_threads[MEMX_THP_THREADS];
    pthread_t overlay_threads[MEMX_THP_THREADS];
    memx_thp_test_state_t heap_state;
    memx_thp_test_state_t overlay_state;
    unsigned index;
    unsigned heap_created = 0U;
    unsigned overlay_created = 0U;
    bool ok = true;
    atomic_init(&heap_state.failures, 0U);
    atomic_init(&overlay_state.failures, 0U);
    atomic_init(&heap_state.ready, 0U);
    atomic_init(&overlay_state.ready, 0U);
    atomic_init(&heap_state.go, false);
    atomic_init(&overlay_state.go, false);
    for (index = 0U; index < MEMX_THP_THREADS; ++index) {
        if (pthread_create(&heap_threads[index], NULL,
                memx_thp_heap_worker, &heap_state) != 0) {
            ok = false;
            break;
        }
        heap_created += 1U;
        if (pthread_create(&overlay_threads[index], NULL,
                memx_thp_overlay_worker, &overlay_state) != 0) {
            ok = false;
            break;
        }
        overlay_created += 1U;
    }
    while (atomic_load_explicit(&heap_state.ready, memory_order_acquire)
            != heap_created
        || atomic_load_explicit(
            &overlay_state.ready, memory_order_acquire) != overlay_created) {
    }
    atomic_store_explicit(&heap_state.go, true, memory_order_release);
    atomic_store_explicit(&overlay_state.go, true, memory_order_release);
    for (index = 0U; index < heap_created; ++index) {
        if (pthread_join(heap_threads[index], NULL) != 0) {
            ok = false;
        }
    }
    for (index = 0U; index < overlay_created; ++index) {
        if (pthread_join(overlay_threads[index], NULL) != 0) {
            ok = false;
        }
    }
    if (!ok
        || atomic_load_explicit(&heap_state.failures, memory_order_relaxed) != 0U
        || atomic_load_explicit(
            &overlay_state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr, "THP concurrency test failed\n");
        return 1;
    }
    puts("THP concurrency test passed");
#else
    puts("THP concurrency test skipped: non-Linux target");
#endif
    return 0;
}
