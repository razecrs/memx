#if !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "memx/allocator.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct bench_options {
    size_t operations;
    size_t slots;
    size_t max_size;
    uint64_t seed;
} bench_options_t;

typedef struct bench_result {
    double nanoseconds_per_operation;
    uint64_t checksum;
    size_t completed_operations;
    memx_heap_stats_t stats;
} bench_result_t;

static uint64_t
rng_next(uint64_t *state) {
    uint64_t value = *state;
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static uint64_t
nanoseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000)
        + (uint64_t)value.tv_nsec;
}

static bool
parse_size(const char *text, size_t *out_value) {
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (text[0] == '\0' || end == NULL || *end != '\0' || value > SIZE_MAX) {
        return false;
    }
    *out_value = (size_t)value;
    return true;
}

static bool
parse_u64(const char *text, uint64_t *out_value) {
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (text[0] == '\0' || end == NULL || *end != '\0') {
        return false;
    }
    *out_value = (uint64_t)value;
    return true;
}

static bool
parse_options(int argc, char **argv, bench_options_t *options) {
    int index;
    options->operations = 2000000U;
    options->slots = 4096U;
    options->max_size = 2048U;
    options->seed = UINT64_C(0x6865617062656e63);
    for (index = 1; index < argc; ++index) {
        if (index + 1 >= argc) {
            return false;
        }
        if (strcmp(argv[index], "--operations") == 0) {
            if (!parse_size(argv[++index], &options->operations)) {
                return false;
            }
        } else if (strcmp(argv[index], "--slots") == 0) {
            if (!parse_size(argv[++index], &options->slots)) {
                return false;
            }
        } else if (strcmp(argv[index], "--max-size") == 0) {
            if (!parse_size(argv[++index], &options->max_size)) {
                return false;
            }
        } else if (strcmp(argv[index], "--seed") == 0) {
            if (!parse_u64(argv[++index], &options->seed)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return options->operations != 0U && options->slots != 0U
        && options->max_size != 0U && options->seed != 0U;
}

static bench_result_t
run_system(const bench_options_t *options) {
    bench_result_t result;
    void **slots = calloc(options->slots, sizeof(*slots));
    size_t *sizes = calloc(options->slots, sizeof(*sizes));
    uint64_t state = options->seed;
    uint64_t begin;
    uint64_t end;
    size_t operation;
    memset(&result, 0, sizeof(result));
    if (slots == NULL || sizes == NULL) {
        free(slots);
        free(sizes);
        return result;
    }
    begin = nanoseconds();
    for (operation = 0U; operation < options->operations; ++operation) {
        const size_t slot = (size_t)(rng_next(&state) % options->slots);
        if (slots[slot] == NULL) {
            const size_t size = 1U
                + (size_t)(rng_next(&state) % options->max_size);
            slots[slot] = malloc(size);
            if (slots[slot] == NULL) {
                break;
            }
            sizes[slot] = size;
            ((unsigned char *)slots[slot])[0] = (unsigned char)size;
            result.checksum ^= (uint64_t)size + operation;
        } else {
            result.checksum += ((const unsigned char *)slots[slot])[0]
                + (uint64_t)sizes[slot];
            free(slots[slot]);
            slots[slot] = NULL;
            sizes[slot] = 0U;
        }
        result.completed_operations = operation + 1U;
    }
    end = nanoseconds();
    for (operation = 0U; operation < options->slots; ++operation) {
        free(slots[operation]);
    }
    if (result.completed_operations != 0U) {
        result.nanoseconds_per_operation = (double)(end - begin)
            / (double)result.completed_operations;
    }
    free(slots);
    free(sizes);
    return result;
}

static bench_result_t
run_memx(const bench_options_t *options) {
    bench_result_t result;
    memx_heap_t *heap = NULL;
    void **slots = calloc(options->slots, sizeof(*slots));
    size_t *sizes = calloc(options->slots, sizeof(*sizes));
    uint64_t state = options->seed;
    uint64_t begin;
    uint64_t end;
    size_t operation;
    memset(&result, 0, sizeof(result));
    if (slots == NULL || sizes == NULL
        || memx_heap_create(NULL, &heap) != MEMX_HEAP_OK) {
        free(slots);
        free(sizes);
        return result;
    }
    begin = nanoseconds();
    for (operation = 0U; operation < options->operations; ++operation) {
        const size_t slot = (size_t)(rng_next(&state) % options->slots);
        if (slots[slot] == NULL) {
            const size_t size = 1U
                + (size_t)(rng_next(&state) % options->max_size);
            slots[slot] = memx_heap_malloc(heap, size);
            if (slots[slot] == NULL) {
                break;
            }
            sizes[slot] = size;
            ((unsigned char *)slots[slot])[0] = (unsigned char)size;
            result.checksum ^= (uint64_t)size + operation;
        } else {
            result.checksum += ((const unsigned char *)slots[slot])[0]
                + (uint64_t)sizes[slot];
            if (!memx_heap_free(heap, slots[slot])) {
                break;
            }
            slots[slot] = NULL;
            sizes[slot] = 0U;
        }
        result.completed_operations = operation + 1U;
    }
    end = nanoseconds();
    for (operation = 0U; operation < options->slots; ++operation) {
        (void)memx_heap_free(heap, slots[operation]);
    }
    memx_heap_get_stats(heap, &result.stats);
    if (result.completed_operations != 0U) {
        result.nanoseconds_per_operation = (double)(end - begin)
            / (double)result.completed_operations;
    }
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
    free(slots);
    free(sizes);
    return result;
}

int
main(int argc, char **argv) {
    bench_options_t options;
    bench_result_t system_result;
    bench_result_t memx_result;
    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr, "usage: %s [--operations N] [--slots N] "
            "[--max-size N] [--seed N]\n", argv[0]);
        return EXIT_FAILURE;
    }
    system_result = run_system(&options);
    memx_result = run_memx(&options);
    if (system_result.completed_operations != options.operations
        || memx_result.completed_operations != options.operations) {
        fprintf(stderr,
            "allocator benchmark did not complete: system=%zu memx=%zu "
            "requested=%zu\n",
            system_result.completed_operations,
            memx_result.completed_operations,
            options.operations);
        return EXIT_FAILURE;
    }
    if (system_result.checksum != memx_result.checksum) {
        fprintf(stderr, "allocator benchmark checksum mismatch\n");
        return EXIT_FAILURE;
    }
    printf("# operations=%zu slots=%zu max_size=%zu seed=0x%" PRIx64 "\n",
        options.operations, options.slots, options.max_size, options.seed);
    puts("allocator,ns_per_operation,checksum,arena_committed_bytes,"
        "committed_spans,cache_hits,cache_misses");
    printf("system_malloc,%.4f,0x%" PRIx64 ",0,0,0,0\n",
        system_result.nanoseconds_per_operation, system_result.checksum);
    printf("memx_heap,%.4f,0x%" PRIx64 ",%zu,%zu,%zu,%zu\n",
        memx_result.nanoseconds_per_operation, memx_result.checksum,
        memx_result.stats.arena_committed_bytes,
        memx_result.stats.committed_spans,
        memx_result.stats.cache_hits,
        memx_result.stats.cache_misses);
    return EXIT_SUCCESS;
}
