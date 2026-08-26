#include "memx/allocator.h"

#include <pthread.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define EXPECT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: expected %s\n", \
            __FILE__, __LINE__, #condition); \
        failures += 1; \
    } \
} while (0)

#define EXPECT_EQ_SIZE(actual, expected) do { \
    const size_t memx_actual = (actual); \
    const size_t memx_expected = (expected); \
    if (memx_actual != memx_expected) { \
        fprintf(stderr, "%s:%d: got %zu, expected %zu\n", \
            __FILE__, __LINE__, memx_actual, memx_expected); \
        failures += 1; \
    } \
} while (0)

static memx_heap_t *
create_heap(void) {
    memx_heap_config_t config = memx_heap_config_default();
    memx_heap_t *heap = NULL;
    config.collect_activity_statistics = true;
    EXPECT_TRUE(memx_heap_create(&config, &heap) == MEMX_HEAP_OK);
    EXPECT_TRUE(heap != NULL);
    return heap;
}

static void
test_configuration(void) {
    memx_heap_config_t config = memx_heap_config_default();
    memx_heap_t *heap = NULL;
    EXPECT_TRUE(config.reserve_size != 0U);
    EXPECT_TRUE(memx_heap_create(NULL, NULL)
        == MEMX_HEAP_ERROR_INVALID_ARGUMENT);
    config.reserve_size += 1U;
    EXPECT_TRUE(memx_heap_create(&config, &heap)
        == MEMX_HEAP_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(heap == NULL);
    config = memx_heap_config_default();
    config.span_shift = 12U;
    EXPECT_TRUE(memx_heap_create(&config, &heap)
        == MEMX_HEAP_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(heap == NULL);
    EXPECT_TRUE(strcmp(memx_heap_status_string(MEMX_HEAP_OK), "ok") == 0);
}

static void
test_size_classes_and_large_allocations(void) {
    static const size_t sizes[] = {
        0U, 1U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U,
        127U, 128U, 255U, 256U, 511U, 512U, 1023U, 1024U,
        2047U, 2048U, 2049U, 4095U, 4096U, 4097U,
        8191U, 8192U, 8193U, 65536U
    };
    void *pointers[sizeof(sizes) / sizeof(sizes[0])];
    memx_heap_stats_t stats;
    memx_heap_t *heap = create_heap();
    size_t index;
    if (heap == NULL) {
        return;
    }
    for (index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
        const size_t requested = sizes[index] == 0U ? 1U : sizes[index];
        pointers[index] = memx_heap_malloc(heap, sizes[index]);
        EXPECT_TRUE(pointers[index] != NULL);
        if (pointers[index] != NULL) {
            EXPECT_TRUE(((uintptr_t)pointers[index]
                % (uintptr_t)alignof(max_align_t)) == 0U);
            EXPECT_TRUE(memx_heap_usable_size(heap, pointers[index])
                >= requested);
            memset(pointers[index], (int)(index + 1U), requested);
        }
    }
    memx_heap_get_stats(heap, &stats);
    EXPECT_TRUE(stats.committed_spans > 0U);
    EXPECT_TRUE(stats.small_allocations > 0U);
    EXPECT_TRUE(stats.large_allocations >= 2U);
    EXPECT_TRUE(stats.live_requested_bytes > 0U);
    for (index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
        EXPECT_TRUE(memx_heap_free(heap, pointers[index]));
    }
    memx_heap_get_stats(heap, &stats);
    EXPECT_EQ_SIZE(stats.live_requested_bytes, 0U);
    EXPECT_EQ_SIZE(stats.active_large_allocations, 0U);
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

static void
test_calloc_realloc_and_alignment(void) {
    memx_heap_t *heap = create_heap();
    unsigned char *bytes;
    unsigned char *grown;
    void *aligned_pointer;
    size_t index;
    if (heap == NULL) {
        return;
    }
    bytes = memx_heap_calloc(heap, 128U, 3U);
    EXPECT_TRUE(bytes != NULL);
    if (bytes != NULL) {
        for (index = 0U; index < 384U; ++index) {
            EXPECT_TRUE(bytes[index] == 0U);
            bytes[index] = (unsigned char)(index & 0xffU);
        }
    }
    grown = memx_heap_realloc(heap, bytes, 8192U);
    EXPECT_TRUE(grown != NULL);
    if (grown != NULL) {
        for (index = 0U; index < 384U; ++index) {
            EXPECT_TRUE(grown[index] == (unsigned char)(index & 0xffU));
        }
    }
    aligned_pointer = memx_heap_aligned_alloc(heap, 4096U, 12345U);
    EXPECT_TRUE(aligned_pointer != NULL);
    if (aligned_pointer != NULL) {
        EXPECT_TRUE(((uintptr_t)aligned_pointer & 4095U) == 0U);
        memset(aligned_pointer, 0xa5, 12345U);
    }
    EXPECT_TRUE(memx_heap_aligned_alloc(heap, 24U, 64U) == NULL);
    EXPECT_TRUE(memx_heap_calloc(heap, SIZE_MAX, 2U) == NULL);
    EXPECT_TRUE(memx_heap_free(heap, grown));
    EXPECT_TRUE(memx_heap_free(heap, aligned_pointer));
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

static void
test_realloc_copies_only_requested_bytes(void) {
    memx_heap_t *heap = create_heap();
    unsigned char *destination_seed;
    unsigned char *source;
    unsigned char *replacement;
    size_t index;
    if (heap == NULL) {
        return;
    }

    destination_seed = memx_heap_malloc(heap, 32U);
    EXPECT_TRUE(destination_seed != NULL);
    if (destination_seed != NULL) {
        memset(destination_seed, 0xcc, 32U);
        EXPECT_TRUE(memx_heap_free(heap, destination_seed));
    }

    source = memx_heap_malloc(heap, 1U);
    EXPECT_TRUE(source != NULL);
    if (source != NULL) {
        const size_t capacity = memx_heap_usable_size(heap, source);
        EXPECT_EQ_SIZE(capacity, 16U);
        memset(source, 0xa5, capacity);
        source[0] = 0x11U;
    }
    replacement = memx_heap_realloc(heap, source, 32U);
    EXPECT_TRUE(replacement != NULL);
    if (replacement != NULL) {
        EXPECT_TRUE(replacement[0] == 0x11U);
        for (index = 1U; index < 16U; ++index) {
            EXPECT_TRUE(replacement[index] == 0xccU);
        }
        EXPECT_TRUE(memx_heap_free(heap, replacement));
    }
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

static void
test_failed_realloc_preserves_allocation(void) {
    memx_heap_t *heap = create_heap();
    unsigned char *pointer;
    void *replacement;
    size_t index;
    if (heap == NULL) {
        return;
    }
    pointer = memx_heap_malloc(heap, 64U);
    EXPECT_TRUE(pointer != NULL);
    if (pointer != NULL) {
        for (index = 0U; index < 64U; ++index) {
            pointer[index] = (unsigned char)(index ^ 0xa5U);
        }
        replacement = memx_heap_realloc(heap, pointer, SIZE_MAX);
        EXPECT_TRUE(replacement == NULL);
        EXPECT_EQ_SIZE(memx_heap_usable_size(heap, pointer), 64U);
        for (index = 0U; index < 64U; ++index) {
            EXPECT_TRUE(pointer[index] == (unsigned char)(index ^ 0xa5U));
        }
        EXPECT_TRUE(memx_heap_free(heap, pointer));
    }
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

static void
test_realloc_in_place(void) {
    memx_heap_t *heap = create_heap();
    memx_heap_stats_t before;
    memx_heap_stats_t after;
    unsigned char *pointer;
    void *replacement;
    if (heap == NULL) {
        return;
    }
    pointer = memx_heap_malloc(heap, 33U);
    EXPECT_TRUE(pointer != NULL);
    if (pointer != NULL) {
        memset(pointer, 0x5a, 33U);
        memx_heap_get_stats(heap, &before);
        replacement = memx_heap_realloc(heap, pointer, 63U);
        EXPECT_TRUE(replacement == pointer);
        EXPECT_EQ_SIZE(memx_heap_usable_size(heap, replacement), 64U);
        memx_heap_get_stats(heap, &after);
        EXPECT_EQ_SIZE(after.live_requested_bytes,
            before.live_requested_bytes + 30U);
        replacement = memx_heap_realloc(heap, replacement, 17U);
        EXPECT_TRUE(replacement == pointer);
        memx_heap_get_stats(heap, &after);
        EXPECT_EQ_SIZE(after.live_requested_bytes, 17U);
        EXPECT_TRUE(memx_heap_free(heap, replacement));
    }
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

static void
test_unchecked_realloc(void) {
    memx_heap_t *heap = create_heap();
    unsigned char *pointer;
    unsigned char *replacement;
    size_t index;
    if (heap == NULL) {
        return;
    }
    pointer = memx_heap_realloc_unchecked(heap, NULL, 33U);
    EXPECT_TRUE(pointer != NULL);
    if (pointer != NULL) {
        for (index = 0U; index < 33U; ++index) {
            pointer[index] = (unsigned char)(index ^ 0x6dU);
        }
        replacement = memx_heap_realloc_unchecked(heap, pointer, 4096U);
        EXPECT_TRUE(replacement != NULL);
        if (replacement != NULL) {
            for (index = 0U; index < 33U; ++index) {
                EXPECT_TRUE(replacement[index]
                    == (unsigned char)(index ^ 0x6dU));
            }
            EXPECT_TRUE(memx_heap_realloc_unchecked(
                heap, replacement, 0U) == NULL);
        }
    }
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

static void
test_invalid_and_duplicate_free(void) {
    memx_heap_t *heap = create_heap();
    memx_heap_stats_t stats;
    unsigned char *pointer;
    int foreign = 0;
    if (heap == NULL) {
        return;
    }
    pointer = memx_heap_malloc(heap, 64U);
    EXPECT_TRUE(pointer != NULL);
    if (pointer != NULL) {
        EXPECT_TRUE(memx_heap_realloc(heap, pointer + 1U, 128U) == NULL);
        EXPECT_EQ_SIZE(memx_heap_usable_size(heap, pointer), 64U);
        EXPECT_TRUE(memx_heap_realloc(heap, &foreign, 128U) == NULL);
        EXPECT_EQ_SIZE(memx_heap_usable_size(heap, pointer), 64U);
        EXPECT_TRUE(!memx_heap_free(heap, pointer + 1U));
        EXPECT_TRUE(memx_heap_free(heap, pointer));
        EXPECT_TRUE(!memx_heap_free(heap, pointer));
        EXPECT_EQ_SIZE(memx_heap_usable_size(heap, pointer), 0U);
    }
    EXPECT_TRUE(!memx_heap_free(heap, &foreign));
    EXPECT_TRUE(memx_heap_free(heap, NULL));
    memx_heap_get_stats(heap, &stats);
    EXPECT_TRUE(stats.invalid_frees >= 3U);
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

typedef struct pointer_batch {
    memx_heap_t *heap;
    void **pointers;
    size_t count;
    bool allocate;
    bool ok;
} pointer_batch_t;

static void *
batch_thread(void *argument) {
    pointer_batch_t *batch = argument;
    size_t index;
    batch->ok = true;
    if (batch->allocate) {
        for (index = 0U; index < batch->count; ++index) {
            batch->pointers[index] = memx_heap_malloc(batch->heap, 64U);
            if (batch->pointers[index] == NULL) {
                batch->ok = false;
                break;
            }
        }
    } else {
        for (index = 0U; index < batch->count; ++index) {
            if (!memx_heap_free(batch->heap, batch->pointers[index])) {
                batch->ok = false;
            }
        }
    }
    memx_heap_thread_detach(batch->heap);
    return NULL;
}

static void
test_remote_free_and_detached_owner(void) {
    enum { POINTER_COUNT = 512 };
    void *pointers[POINTER_COUNT];
    pointer_batch_t batch;
    pthread_t thread;
    memx_heap_stats_t stats;
    memx_heap_t *heap = create_heap();
    size_t index;
    if (heap == NULL) {
        return;
    }
    for (index = 0U; index < POINTER_COUNT; ++index) {
        pointers[index] = memx_heap_malloc(heap, 64U);
        EXPECT_TRUE(pointers[index] != NULL);
    }
    batch.heap = heap;
    batch.pointers = pointers;
    batch.count = POINTER_COUNT;
    batch.allocate = false;
    batch.ok = false;
    {
        const int create_result = pthread_create(
            &thread, NULL, batch_thread, &batch);
        EXPECT_TRUE(create_result == 0);
        if (create_result == 0) {
            EXPECT_TRUE(pthread_join(thread, NULL) == 0);
            EXPECT_TRUE(batch.ok);
        }
    }
    memx_heap_get_stats(heap, &stats);
    EXPECT_EQ_SIZE(stats.remote_frees, POINTER_COUNT);

    /* The original owner drains its remote queue on refill. */
    for (index = 0U; index < POINTER_COUNT; ++index) {
        pointers[index] = memx_heap_malloc(heap, 64U);
        EXPECT_TRUE(pointers[index] != NULL);
    }
    for (index = 0U; index < POINTER_COUNT; ++index) {
        EXPECT_TRUE(memx_heap_free(heap, pointers[index]));
    }

    /* A producer detaches while its allocations remain live. */
    batch.allocate = true;
    batch.ok = false;
    {
        const int create_result = pthread_create(
            &thread, NULL, batch_thread, &batch);
        EXPECT_TRUE(create_result == 0);
        if (create_result == 0) {
            EXPECT_TRUE(pthread_join(thread, NULL) == 0);
            EXPECT_TRUE(batch.ok);
        }
    }
    for (index = 0U; index < POINTER_COUNT; ++index) {
        EXPECT_TRUE(memx_heap_free(heap, pointers[index]));
    }
    memx_heap_thread_detach(heap);
    memx_heap_get_stats(heap, &stats);
    EXPECT_EQ_SIZE(stats.live_requested_bytes, 0U);
    memx_heap_destroy(heap);
}

typedef struct stress_case {
    memx_heap_t *heap;
    size_t seed;
    bool ok;
} stress_case_t;

static void *
stress_thread(void *argument) {
    stress_case_t *test = argument;
    void *slots[64] = {0};
    size_t iteration;
    test->ok = true;
    for (iteration = 0U; iteration < 20000U; ++iteration) {
        const size_t slot = (iteration * 17U + test->seed) % 64U;
        if (slots[slot] == NULL) {
            const size_t size = 1U + ((iteration * 29U + test->seed) % 2048U);
            slots[slot] = memx_heap_malloc(test->heap, size);
            if (slots[slot] == NULL) {
                test->ok = false;
                break;
            }
            memset(slots[slot], (int)(iteration & 0xffU), size);
        } else {
            if (!memx_heap_free(test->heap, slots[slot])) {
                test->ok = false;
            }
            slots[slot] = NULL;
        }
    }
    for (iteration = 0U; iteration < 64U; ++iteration) {
        if (slots[iteration] != NULL
            && !memx_heap_free(test->heap, slots[iteration])) {
            test->ok = false;
        }
    }
    memx_heap_thread_detach(test->heap);
    return NULL;
}

static void
test_multithread_stress(void) {
    enum { THREAD_COUNT = 4 };
    pthread_t threads[THREAD_COUNT];
    bool created[THREAD_COUNT] = {false};
    stress_case_t tests[THREAD_COUNT];
    memx_heap_stats_t stats;
    memx_heap_t *heap = create_heap();
    size_t index;
    if (heap == NULL) {
        return;
    }
    for (index = 0U; index < THREAD_COUNT; ++index) {
        int create_result;
        tests[index].heap = heap;
        tests[index].seed = index + 1U;
        tests[index].ok = false;
        create_result = pthread_create(
            &threads[index], NULL, stress_thread, &tests[index]);
        EXPECT_TRUE(create_result == 0);
        created[index] = create_result == 0;
    }
    for (index = 0U; index < THREAD_COUNT; ++index) {
        if (created[index]) {
            EXPECT_TRUE(pthread_join(threads[index], NULL) == 0);
            EXPECT_TRUE(tests[index].ok);
        }
    }
    memx_heap_get_stats(heap, &stats);
    EXPECT_EQ_SIZE(stats.live_requested_bytes, 0U);
    EXPECT_TRUE(stats.cache_hits > 0U);
    memx_heap_destroy(heap);
}

static void
test_arena_exhaustion(void) {
    memx_heap_config_t config = memx_heap_config_default();
    memx_heap_t *heap = NULL;
    void *pointers[256];
    size_t count = 0U;
    config.reserve_size = (size_t)1U << config.span_shift;
    EXPECT_TRUE(memx_heap_create(&config, &heap) == MEMX_HEAP_OK);
    if (heap == NULL) {
        return;
    }
    while (count < sizeof(pointers) / sizeof(pointers[0])) {
        pointers[count] = memx_heap_malloc(heap, 2048U);
        if (pointers[count] == NULL) {
            break;
        }
        count += 1U;
    }
    EXPECT_TRUE(count > 0U);
    EXPECT_TRUE(count < sizeof(pointers) / sizeof(pointers[0]));
    while (count != 0U) {
        count -= 1U;
        EXPECT_TRUE(memx_heap_free(heap, pointers[count]));
    }
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

static uint64_t
random_next(uint64_t *state) {
    uint64_t value = *state;
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static void
test_randomized_payload_differential(void) {
    enum { SLOT_COUNT = 256, OPERATION_COUNT = 100000 };
    void *slots[SLOT_COUNT] = {0};
    size_t sizes[SLOT_COUNT] = {0};
    unsigned char patterns[SLOT_COUNT] = {0};
    uint64_t state = UINT64_C(0x6d656d7868656170);
    memx_heap_t *heap = create_heap();
    size_t operation;
    if (heap == NULL) {
        return;
    }
    for (operation = 0U; operation < OPERATION_COUNT; ++operation) {
        const size_t slot = (size_t)(random_next(&state) % SLOT_COUNT);
        if (slots[slot] == NULL) {
            const size_t size = 1U + (size_t)(random_next(&state) % 8192U);
            const unsigned char pattern = (unsigned char)random_next(&state);
            slots[slot] = memx_heap_malloc(heap, size);
            EXPECT_TRUE(slots[slot] != NULL);
            if (slots[slot] != NULL) {
                sizes[slot] = size;
                patterns[slot] = pattern;
                memset(slots[slot], (int)pattern, size);
            }
        } else if ((random_next(&state) & 3U) == 0U) {
            const size_t new_size = 1U
                + (size_t)(random_next(&state) % 8192U);
            unsigned char *bytes = slots[slot];
            size_t index;
            void *replacement;
            for (index = 0U; index < sizes[slot]; ++index) {
                EXPECT_TRUE(bytes[index] == patterns[slot]);
            }
            replacement = memx_heap_realloc(heap, slots[slot], new_size);
            EXPECT_TRUE(replacement != NULL);
            if (replacement != NULL) {
                const size_t preserved = sizes[slot] < new_size
                    ? sizes[slot] : new_size;
                bytes = replacement;
                for (index = 0U; index < preserved; ++index) {
                    EXPECT_TRUE(bytes[index] == patterns[slot]);
                }
                slots[slot] = replacement;
                sizes[slot] = new_size;
                memset(slots[slot], (int)patterns[slot], new_size);
            }
        } else {
            unsigned char *bytes = slots[slot];
            size_t index;
            for (index = 0U; index < sizes[slot]; ++index) {
                EXPECT_TRUE(bytes[index] == patterns[slot]);
            }
            EXPECT_TRUE(memx_heap_free(heap, slots[slot]));
            slots[slot] = NULL;
            sizes[slot] = 0U;
        }
    }
    for (operation = 0U; operation < SLOT_COUNT; ++operation) {
        if (slots[operation] != NULL) {
            EXPECT_TRUE(memx_heap_free(heap, slots[operation]));
        }
    }
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
}

int
main(void) {
    test_configuration();
    test_size_classes_and_large_allocations();
    test_calloc_realloc_and_alignment();
    test_realloc_copies_only_requested_bytes();
    test_failed_realloc_preserves_allocation();
    test_realloc_in_place();
    test_unchecked_realloc();
    test_invalid_and_duplicate_free();
    test_remote_free_and_detached_owner();
    test_multithread_stress();
    test_arena_exhaustion();
    test_randomized_payload_differential();
    if (failures != 0) {
        fprintf(stderr, "%d allocator test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("allocator tests passed");
    return EXIT_SUCCESS;
}
