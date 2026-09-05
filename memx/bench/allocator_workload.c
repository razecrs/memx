#if !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(MEMX_WORKLOAD_USE_MEMX_HEAP)
#include "memx/allocator.h"

static memx_heap_t *workload_heap;

static bool workload_initialize(void)
{
    return memx_heap_create(NULL, &workload_heap) == MEMX_HEAP_OK;
}

static void *workload_allocate(size_t size)
{
    return memx_heap_malloc(workload_heap, size);
}

static void *workload_reallocate(void *pointer, size_t size)
{
#if defined(MEMX_WORKLOAD_USE_CHECKED_API)
    return memx_heap_realloc(workload_heap, pointer, size);
#else
    return memx_heap_realloc_unchecked(workload_heap, pointer, size);
#endif
}

static void workload_deallocate(void *pointer)
{
#if defined(MEMX_WORKLOAD_USE_CHECKED_API)
    (void)memx_heap_free(workload_heap, pointer);
#else
    memx_heap_free_unchecked(workload_heap, pointer);
#endif
}

static void workload_finalize(void)
{
    memx_heap_thread_detach(workload_heap);
    memx_heap_destroy(workload_heap);
    workload_heap = NULL;
}
#else
static bool workload_initialize(void)
{
    return true;
}

static void *workload_allocate(size_t size)
{
    return malloc(size);
}

static void *workload_reallocate(void *pointer, size_t size)
{
    return realloc(pointer, size);
}

static void workload_deallocate(void *pointer)
{
    free(pointer);
}

static void workload_finalize(void)
{
}
#endif

typedef struct {
    void *pointer;
    size_t size;
} slot_t;

static uint64_t next_random(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static size_t choose_size(uint64_t value)
{
    static const size_t sizes[] = {16, 24, 32, 48, 64, 96, 128, 192,
                                   256, 512, 1024, 2048, 4096, 8192};
    return sizes[value % (sizeof(sizes) / sizeof(sizes[0]))];
}

static void shuffle(size_t *order, size_t count, uint64_t *state)
{
    for (size_t i = count; i > 1; --i) {
        size_t j = (size_t)(next_random(state) % i);
        size_t temporary = order[i - 1];
        order[i - 1] = order[j];
        order[j] = temporary;
    }
}

int main(int argc, char **argv)
{
    size_t slots_count = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 100000;
    size_t rounds = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 5;
    slot_t *slots = NULL;
    size_t *order = NULL;
    uint64_t random_state;
    uint64_t checksum;
    uint64_t operations;
    uint64_t start;
    uint64_t elapsed;
    int exit_code = EXIT_SUCCESS;
    if (slots_count == 0 || rounds == 0 || slots_count > 10000000) {
        fprintf(stderr, "usage: %s [slots 1..10000000] [rounds >=1]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!workload_initialize()) {
        fputs("allocator initialization failed\n", stderr);
        return EXIT_FAILURE;
    }

    slots = calloc(slots_count, sizeof(*slots));
    order = malloc(slots_count * sizeof(*order));
    if (slots == NULL || order == NULL) {
        perror("workload arrays");
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    random_state = UINT64_C(0x9e3779b97f4a7c15);
    checksum = 0;
    operations = 0;
    start = now_ns();

    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < slots_count; ++i) {
            size_t size = choose_size(next_random(&random_state));
            slots[i].pointer = workload_allocate(size);
            slots[i].size = size;
            if (slots[i].pointer == NULL) {
                fprintf(stderr, "malloc failed at slot %zu\n", i);
                exit_code = EXIT_FAILURE;
                goto cleanup;
            }
            memset(slots[i].pointer, (int)(i + round), size);
            checksum ^= (uint64_t)((unsigned char *)slots[i].pointer)[0] + size;
            order[i] = i;
            ++operations;
        }

        shuffle(order, slots_count, &random_state);
        for (size_t i = 0; i < slots_count; ++i) {
            size_t slot_index = order[i];
            slot_t *slot = &slots[slot_index];
            if ((next_random(&random_state) & 3U) == 0U) {
                size_t new_size = choose_size(next_random(&random_state));
                void *replacement = workload_reallocate(
                    slot->pointer, new_size);
                if (replacement == NULL) {
                    fprintf(stderr, "realloc failed at slot %zu\n", slot_index);
                    exit_code = EXIT_FAILURE;
                    goto cleanup;
                }
                slot->pointer = replacement;
                slot->size = new_size;
                memset(slot->pointer, (int)(round + 17), new_size);
                checksum ^= (uint64_t)((unsigned char *)slot->pointer)[new_size - 1] + new_size;
                ++operations;
            }
            checksum ^= (uint64_t)((unsigned char *)slot->pointer)[0];
            workload_deallocate(slot->pointer);
            slot->pointer = NULL;
            ++operations;
        }
    }

    elapsed = now_ns() - start;
    printf("slots=%zu rounds=%zu operations=%llu elapsed_ns=%llu ns_per_operation=%.3f checksum=%llu\n",
           slots_count, rounds, (unsigned long long)operations,
           (unsigned long long)elapsed,
           (double)elapsed / (double)operations,
           (unsigned long long)checksum);
cleanup:
    if (slots != NULL) {
        for (size_t index = 0; index < slots_count; ++index) {
            if (slots[index].pointer != NULL) {
                workload_deallocate(slots[index].pointer);
            }
        }
    }
    free(order);
    free(slots);
    workload_finalize();
    return exit_code;
}
