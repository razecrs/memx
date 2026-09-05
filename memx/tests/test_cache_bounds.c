/* White-box regression: counts and tails are intentionally private, not ABI. */
#include "../src/allocator/heap.c"

#include <stdio.h>

#define POINTER_COUNT 513U

static int
check_segment(memx_heap_block_t *head, memx_heap_block_t *tail, size_t count) {
    memx_heap_block_t *last = NULL;
    size_t seen = 0U;
    while (head != NULL && seen <= count) {
        last = head;
        head = head->fields.link.next;
        ++seen;
    }
    return head == NULL && seen == count && last == tail;
}

static int
check_cache(memx_heap_t *heap) {
    memx_heap_cache_t *cache = memx_heap_find_cache(heap);
    size_t c;
    if (cache == NULL) {
        return 0;
    }
    for (c = 0U; c < MEMX_HEAP_CLASS_COUNT; ++c) {
        if (cache->local_count[c] > heap->config.cache_limit
            || cache->spare_count[c]
                > heap->config.cache_limit - cache->local_count[c]
            || !check_segment(cache->local[c], cache->local_tail[c],
                cache->local_count[c])
            || !check_segment(cache->spare[c], cache->spare_tail[c],
                cache->spare_count[c])) {
            fprintf(stderr, "cache limit=%zu class=%zu local=%zu spare=%zu\n",
                heap->config.cache_limit, c,
                cache->local_count[c], cache->spare_count[c]);
            return 0;
        }
    }
    return 1;
}

typedef struct remote_job {
    memx_heap_t *heap;
    void **pointers;
} remote_job_t;

static void *
remote_free(void *argument) {
    remote_job_t *job = argument;
    size_t i;
    for (i = 0U; i < POINTER_COUNT; ++i) {
        memx_heap_free_unchecked(job->heap, job->pointers[i]);
    }
    return NULL;
}

static int
exercise(size_t limit, size_t batch) {
    memx_heap_config_t config = memx_heap_config_default();
    memx_heap_t *heap = NULL;
    void *pointers[POINTER_COUNT];
    const size_t sizes[] = {16U, 24U, 192U, 8192U};
    size_t shape;
    int ok = 1;
    config.cache_limit = limit;
    config.cache_batch = batch;
    if (memx_heap_create(&config, &heap) != MEMX_HEAP_OK) {
        return 0;
    }
    for (shape = 0U; shape < sizeof(sizes) / sizeof(sizes[0]); ++shape) {
        size_t i;
        size_t pass;
        for (pass = 0U; pass < 3U; ++pass) {
            for (i = 0U; i < POINTER_COUNT; ++i) {
                pointers[i] = memx_heap_malloc(heap, sizes[shape]);
                if (pointers[i] == NULL || !check_cache(heap)) {
                    memx_heap_destroy(heap);
                    return 0;
                }
                memset(pointers[i], (int)(i % 251U), sizes[shape]);
            }
            if (pass == 1U) {
                pthread_t worker;
                remote_job_t job = {heap, pointers};
                if (pthread_create(&worker, NULL, remote_free, &job) != 0) {
                    memx_heap_destroy(heap);
                    return 0;
                }
                if (pthread_join(worker, NULL) != 0) {
                    return 0;
                }
                /* Pending remote queues are separate from the local budget.
                 * Once drained, both cached segments must obey that budget. */
                memx_heap_drain_remote(heap, memx_heap_find_cache(heap),
                    memx_heap_class_for_size(sizes[shape]));
                ok &= check_cache(heap);
            } else {
                for (i = 0U; i < POINTER_COUNT; ++i) {
                    memx_heap_free_unchecked(heap, pointers[i]);
                    ok &= check_cache(heap);
                }
            }
        }
    }
    memx_heap_thread_detach(heap);
    /* After detach, every carved block must be back in a central chunk. */
    {
        memx_heap_span_t *span;
        size_t carved = 0U;
        size_t returned = 0U;
        size_t c;
        for (span = heap->spans; span != NULL; span = span->next) {
            carved += span->block_count;
        }
        for (c = 0U; c < MEMX_HEAP_CLASS_COUNT; ++c) {
            returned += heap->central_count[c];
        }
        ok &= carved == returned;
    }
    memx_heap_destroy(heap);
    return ok;
}

int
main(void) {
    static const size_t limits[] = {2U, 3U, 17U, 255U, 256U, 257U};
    static const size_t batches[] = {1U, 16U, 256U, 1024U};
    size_t i;
    size_t j;
    for (i = 0U; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        for (j = 0U; j < sizeof(batches) / sizeof(batches[0]); ++j) {
            if (!exercise(limits[i], batches[j])) {
                return 1;
            }
        }
    }
    puts("cache-bound regressions passed");
    return 0;
}
