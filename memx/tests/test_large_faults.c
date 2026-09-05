#include "memx/allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The test executable is linked with -Wl,--wrap=calloc. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-identifier"
#endif
extern void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);

static size_t calloc_calls;
static size_t fail_call;

void *
__wrap_calloc(size_t count, size_t size) {
    calloc_calls += 1U;
    if (fail_call != 0U && calloc_calls == fail_call) {
        return NULL;
    }
    return __real_calloc(count, size);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static void
fault_reset(void) {
    calloc_calls = 0U;
    fail_call = 0U;
}

static memx_heap_t *
new_heap(void) {
    memx_heap_config_t config = memx_heap_config_default();
    memx_heap_t *heap = NULL;
    config.reserve_size = 16U * 1024U * 1024U;
    return memx_heap_create(&config, &heap) == MEMX_HEAP_OK ? heap : NULL;
}

static int
check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "large fault test: %s\n", message);
        return 0;
    }
    return 1;
}

static int
test_first_table_failure(void) {
    memx_heap_t *heap = new_heap();
    memx_heap_stats_t stats;
    void *pointer;
    int result = check(heap != NULL, "heap creation");
    if (!result) {
        return 0;
    }
    fault_reset();
    fail_call = 2U; /* large record, then first bucket table */
    pointer = memx_heap_malloc(heap, 9001U);
    result &= check(pointer == NULL, "first bucket allocation fails");
    memx_heap_get_stats(heap, &stats);
    result &= check(stats.active_large_allocations == 0U,
        "failed first allocation leaves no live record");
    result &= check(stats.large_index_bytes == 0U,
        "failed first allocation leaves no table");
    memx_heap_destroy(heap);
    return result;
}

static int
test_growth_failure(void) {
    memx_heap_t *heap = new_heap();
    memx_heap_stats_t stats;
    void *pointers[65];
    size_t index;
    int result = check(heap != NULL, "heap creation");
    if (!result) {
        return 0;
    }
    fault_reset();
    for (index = 0U; index < 64U; ++index) {
        pointers[index] = memx_heap_malloc(heap, 9001U + index);
        result &= check(pointers[index] != NULL, "initial large allocation");
    }
    fault_reset();
    fail_call = 2U; /* record allocation, then growth rehash */
    pointers[64] = memx_heap_malloc(heap, 10000U);
    result &= check(pointers[64] == NULL, "growth allocation fails");
    memx_heap_get_stats(heap, &stats);
    result &= check(stats.active_large_allocations == 64U,
        "growth failure preserves live records");
    result &= check(stats.large_index_bytes == 64U * sizeof(void *),
        "growth failure preserves old table");
    fault_reset();
    for (index = 0U; index < 64U; ++index) {
        result &= check(memx_heap_free(heap, pointers[index]),
            "free after failed growth");
    }
    memx_heap_get_stats(heap, &stats);
    result &= check(stats.large_index_bytes == 0U,
        "final release frees the large table");
    memx_heap_destroy(heap);
    return result;
}

static int
test_realloc_and_shrink_failures(void) {
    memx_heap_t *heap = new_heap();
    memx_heap_stats_t stats;
    unsigned char *pointer;
    unsigned char *replacement;
    void *pointers[65];
    void *growth[64];
    size_t index;
    int result = check(heap != NULL, "heap creation");
    if (!result) {
        return 0;
    }
    fault_reset();
    pointer = memx_heap_malloc(heap, 9001U);
    result &= check(pointer != NULL, "realloc source allocation");
    if (pointer != NULL) {
        memset(pointer, 0x5a, 9001U);
        fault_reset();
        fail_call = 1U; /* metadata calloc in replacement allocation */
        replacement = memx_heap_realloc(heap, pointer, 20000U);
        result &= check(replacement == NULL, "realloc metadata failure");
        result &= check(pointer[0] == 0x5a && pointer[9000] == 0x5a,
            "failed realloc preserves source payload");
        fault_reset();
        result &= check(memx_heap_free(heap, pointer), "free realloc source");
    }

    /* A failed replacement at table growth must preserve the whole source. */
    fault_reset();
    for (index = 0U; index < 64U; ++index) {
        growth[index] = memx_heap_malloc(heap, 15000U + index);
        result &= check(growth[index] != NULL, "growth realloc setup");
    }
    if (growth[0] != NULL) {
        memset(growth[0], 0x6b, 15000U);
        fault_reset();
        fail_call = 2U; /* replacement record, then 64->128 rehash */
        replacement = memx_heap_realloc(heap, growth[0], 20000U);
        result &= check(replacement == NULL, "growth realloc rehash failure");
        result &= check(((unsigned char *)growth[0])[0] == 0x6b
                && ((unsigned char *)growth[0])[14999] == 0x6b,
            "growth realloc preserves full source payload");
        fault_reset();
        replacement = memx_heap_realloc(heap, growth[0], 20000U);
        result &= check(replacement != NULL,
            "growth realloc recovers after failed rehash");
        growth[0] = replacement;
    }
    fault_reset();
    for (index = 0U; index < 64U; ++index) {
        if (growth[index] != NULL) {
            result &= check(memx_heap_free(heap, growth[index]),
                "free after growth realloc recovery");
        }
    }

    fault_reset();
    for (index = 0U; index < 65U; ++index) {
        pointers[index] = memx_heap_malloc(heap, 12000U + index);
        result &= check(pointers[index] != NULL, "shrink setup allocation");
    }
    memx_heap_get_stats(heap, &stats);
    result &= check(stats.large_index_bytes == 128U * sizeof(void *),
        "growth creates 128 bucket table");
    for (index = 0U; index < 32U; ++index) {
        result &= check(memx_heap_free(heap, pointers[index]),
            "shrink setup free");
    }
    fault_reset();
    fail_call = 1U; /* shrink rehash; failure must not fail free */
    result &= check(memx_heap_free(heap, pointers[32]),
        "free succeeds when shrink allocation fails");
    memx_heap_get_stats(heap, &stats);
    result &= check(stats.active_large_allocations == 32U,
        "shrink failure keeps remaining records");
    result &= check(stats.large_index_bytes == 128U * sizeof(void *),
        "shrink failure preserves old table");
    fault_reset();
    for (index = 33U; index < 65U; ++index) {
        result &= check(memx_heap_free(heap, pointers[index]),
            "free after failed shrink");
    }
    memx_heap_get_stats(heap, &stats);
    result &= check(stats.large_index_bytes == 0U,
        "table released after shrink test");
    memx_heap_destroy(heap);
    return result;
}

int
main(void) {
    int result = test_first_table_failure();
    result &= test_growth_failure();
    result &= test_realloc_and_shrink_failures();
    return result ? 0 : 1;
}
