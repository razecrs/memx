/*
 * Single-threaded allocator comparison across several allocation shapes.
 *
 * One shape is not enough to characterise an allocator: thread-cache hit
 * paths, free-list locality, size-class fit, and reuse of recently freed
 * memory each dominate under different patterns. This harness runs seven.
 *
 * Built twice. memx_pattern_bench calls the MemX heap API directly.
 * memx_pattern_bench_system calls whatever malloc the loader supplies, so a
 * competitor can be measured through LD_PRELOAD in the identical binary. To
 * compare like with like, measure MemX through LD_PRELOAD as well using
 * memx_malloc_shim rather than comparing the direct-API build against an
 * interposed competitor: static linking lets the MemX calls inline, which is
 * worth over 10% on some patterns.
 */

#if !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(MEMX_PATTERNS_USE_MEMX_HEAP)
#include "memx/allocator.h"

static memx_heap_t *pattern_heap;

static bool
allocator_initialize(void) {
    memx_heap_config_t config = memx_heap_config_default();
    return memx_heap_create(&config, &pattern_heap) == MEMX_HEAP_OK;
}

static void *
allocator_allocate(size_t size) {
    return memx_heap_malloc(pattern_heap, size);
}

static void *
allocator_reallocate(void *pointer, size_t size) {
    return memx_heap_realloc_unchecked(pattern_heap, pointer, size);
}

static void
allocator_release(void *pointer) {
    memx_heap_free_unchecked(pattern_heap, pointer);
}

static void
allocator_finalize(void) {
    memx_heap_thread_detach(pattern_heap);
    memx_heap_destroy(pattern_heap);
    pattern_heap = NULL;
}
#else
static bool
allocator_initialize(void) {
    return true;
}

static void *
allocator_allocate(size_t size) {
    return malloc(size);
}

static void *
allocator_reallocate(void *pointer, size_t size) {
    return realloc(pointer, size);
}

static void
allocator_release(void *pointer) {
    free(pointer);
}

static void
allocator_finalize(void) {
}
#endif

typedef enum pattern_kind {
    PATTERN_MIXED,
    PATTERN_SMALL,
    PATTERN_LARGE,
    PATTERN_LIFO,
    PATTERN_FIFO,
    PATTERN_CHURN,
    PATTERN_REALLOC
} pattern_kind_t;

static const size_t pattern_sizes_mixed[] = {
    16U, 24U, 32U, 48U, 64U, 96U, 128U, 192U,
    256U, 512U, 1024U, 2048U, 4096U, 8192U
};

static const size_t pattern_sizes_small[] = {
    8U, 16U, 24U, 32U, 40U, 48U, 64U, 80U, 96U, 112U, 128U
};

static const size_t pattern_sizes_large[] = {
    1024U, 1536U, 2048U, 3072U, 4096U, 6144U, 8192U
};

static uint64_t
next_random(uint64_t *state) {
    uint64_t value = *state;
    value ^= value << 13U;
    value ^= value >> 7U;
    value ^= value << 17U;
    *state = value;
    return value;
}

static uint64_t
now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000)
        + (uint64_t)now.tv_nsec;
}

static bool
parse_pattern(const char *name, pattern_kind_t *out_kind) {
    if (strcmp(name, "mixed") == 0) {
        *out_kind = PATTERN_MIXED;
    } else if (strcmp(name, "small") == 0) {
        *out_kind = PATTERN_SMALL;
    } else if (strcmp(name, "large") == 0) {
        *out_kind = PATTERN_LARGE;
    } else if (strcmp(name, "lifo") == 0) {
        *out_kind = PATTERN_LIFO;
    } else if (strcmp(name, "fifo") == 0) {
        *out_kind = PATTERN_FIFO;
    } else if (strcmp(name, "churn") == 0) {
        *out_kind = PATTERN_CHURN;
    } else if (strcmp(name, "realloc") == 0) {
        *out_kind = PATTERN_REALLOC;
    } else {
        return false;
    }
    return true;
}

int
main(int argc, char **argv) {
    const char *pattern_name = argc > 1 ? argv[1] : "mixed";
    size_t live = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 50000U;
    size_t rounds = argc > 3 ? (size_t)strtoull(argv[3], NULL, 10) : 3U;
    pattern_kind_t pattern = PATTERN_MIXED;
    const size_t *sizes = pattern_sizes_mixed;
    size_t size_count = sizeof(pattern_sizes_mixed)
        / sizeof(pattern_sizes_mixed[0]);
    void **slots = NULL;
    size_t *order = NULL;
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t checksum = 0U;
    uint64_t operations = 0U;
    uint64_t start;
    uint64_t elapsed;
    size_t round;
    size_t index;
    int exit_code = EXIT_SUCCESS;

    if (!parse_pattern(pattern_name, &pattern)) {
        fprintf(stderr,
            "usage: %s [mixed|small|large|lifo|fifo|churn|realloc]"
            " [live 1..10000000] [rounds >=1]\n",
            argv[0]);
        return EXIT_FAILURE;
    }
    if (live == 0U || live > 10000000U || rounds == 0U) {
        fprintf(stderr, "live must be 1..10000000 and rounds >= 1\n");
        return EXIT_FAILURE;
    }
    if (pattern == PATTERN_SMALL) {
        sizes = pattern_sizes_small;
        size_count = sizeof(pattern_sizes_small)
            / sizeof(pattern_sizes_small[0]);
    } else if (pattern == PATTERN_LARGE) {
        sizes = pattern_sizes_large;
        size_count = sizeof(pattern_sizes_large)
            / sizeof(pattern_sizes_large[0]);
    }
    if (!allocator_initialize()) {
        fputs("allocator initialization failed\n", stderr);
        return EXIT_FAILURE;
    }
    slots = calloc(live, sizeof(*slots));
    order = calloc(live, sizeof(*order));
    if (slots == NULL || order == NULL) {
        perror("harness arrays");
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    start = now_ns();
    for (round = 0U; round < rounds; ++round) {
        if (pattern == PATTERN_LIFO) {
            /* Allocate and release immediately: the pure thread-cache path,
             * with no opportunity for a cache or TLB miss to hide behind. */
            for (index = 0U; index < live; ++index) {
                size_t size = sizes[next_random(&state) % size_count];
                void *pointer = allocator_allocate(size);
                if (pointer == NULL) {
                    fprintf(stderr, "allocation failed at %zu\n", index);
                    exit_code = EXIT_FAILURE;
                    goto cleanup;
                }
                memset(pointer, (int)index, size);
                checksum += ((unsigned char *)pointer)[0];
                allocator_release(pointer);
                operations += 2U;
            }
            continue;
        }
        if (pattern == PATTERN_CHURN) {
            /* A stable live set with random replacement: the steady state a
             * long-running program actually spends its time in. */
            for (index = 0U; index < live; ++index) {
                size_t size = sizes[next_random(&state) % size_count];
                if (slots[index] != NULL) {
                    continue;
                }
                slots[index] = allocator_allocate(size);
                if (slots[index] == NULL) {
                    fprintf(stderr, "allocation failed at %zu\n", index);
                    exit_code = EXIT_FAILURE;
                    goto cleanup;
                }
                memset(slots[index], (int)index, size);
                checksum += ((unsigned char *)slots[index])[0];
                operations += 1U;
            }
            for (index = 0U; index < live * 2U; ++index) {
                size_t slot = (size_t)(next_random(&state) % live);
                size_t size = sizes[next_random(&state) % size_count];
                allocator_release(slots[slot]);
                slots[slot] = allocator_allocate(size);
                if (slots[slot] == NULL) {
                    fputs("allocation failed during churn\n", stderr);
                    exit_code = EXIT_FAILURE;
                    goto cleanup;
                }
                memset(slots[slot], (int)index, size);
                checksum += ((unsigned char *)slots[slot])[0];
                operations += 2U;
            }
            continue;
        }
        for (index = 0U; index < live; ++index) {
            size_t size = sizes[next_random(&state) % size_count];
            slots[index] = allocator_allocate(size);
            if (slots[index] == NULL) {
                fprintf(stderr, "allocation failed at %zu\n", index);
                exit_code = EXIT_FAILURE;
                goto cleanup;
            }
            memset(slots[index], (int)(index + round), size);
            checksum += ((unsigned char *)slots[index])[0];
            order[index] = index;
            operations += 1U;
        }
        if (pattern == PATTERN_FIFO) {
            /* Release in allocation order: adversarial for a LIFO cache. */
            for (index = 0U; index < live; ++index) {
                allocator_release(slots[index]);
                slots[index] = NULL;
                operations += 1U;
            }
            continue;
        }
        for (index = live; index > 1U; --index) {
            size_t swap = (size_t)(next_random(&state) % index);
            size_t temporary = order[index - 1U];
            order[index - 1U] = order[swap];
            order[swap] = temporary;
        }
        for (index = 0U; index < live; ++index) {
            size_t slot = order[index];
            bool resize = pattern == PATTERN_REALLOC
                || (pattern == PATTERN_MIXED
                    && (next_random(&state) & 3U) == 0U);
            if (resize) {
                size_t size = sizes[next_random(&state) % size_count];
                void *replacement = allocator_reallocate(slots[slot], size);
                if (replacement == NULL) {
                    fprintf(stderr, "reallocation failed at %zu\n", slot);
                    exit_code = EXIT_FAILURE;
                    goto cleanup;
                }
                slots[slot] = replacement;
                memset(replacement, (int)round, size);
                checksum += ((unsigned char *)replacement)[size - 1U];
                operations += 1U;
            }
            allocator_release(slots[slot]);
            slots[slot] = NULL;
            operations += 1U;
        }
    }
    elapsed = now_ns() - start;
    printf("pattern=%s live=%zu rounds=%zu operations=%llu elapsed_ns=%llu"
        " ns_per_operation=%.3f checksum=%llu\n",
        pattern_name, live, rounds, (unsigned long long)operations,
        (unsigned long long)elapsed,
        (double)elapsed / (double)operations,
        (unsigned long long)checksum);

cleanup:
    if (slots != NULL) {
        for (index = 0U; index < live; ++index) {
            if (slots[index] != NULL) {
                allocator_release(slots[index]);
            }
        }
    }
    free(order);
    free(slots);
    allocator_finalize();
    return exit_code;
}
