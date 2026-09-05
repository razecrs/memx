#include "memx/memx.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAULT_GRANULE_SHIFT 12U
#define FAULT_REGION_SHIFT 16U
#define FAULT_GRANULE_SIZE (((uintptr_t)1U) << FAULT_GRANULE_SHIFT)
#define FAULT_REGION_SIZE (((uintptr_t)1U) << FAULT_REGION_SHIFT)
#define FAULT_GRANULES_PER_REGION 16U
#define FAULT_REGIONS 4U
#define FAULT_BASE ((uintptr_t)0x400000U)
#define MAX_TRACKED_ALLOCATIONS 4096U

typedef struct allocation_record {
    void *pointer;
    size_t size;
    bool live;
} allocation_record_t;

typedef struct fault_allocator {
    allocation_record_t records[MAX_TRACKED_ALLOCATIONS];
    size_t record_count;
    size_t allocation_calls;
    size_t deallocation_calls;
    size_t live_allocations;
    size_t live_bytes;
    size_t peak_live_bytes;
    size_t fail_call;
    bool invalid_free;
    bool record_overflow;
} fault_allocator_t;

typedef struct counters {
    size_t checks;
    size_t failures;
} counters_t;

static counters_t counters;

static void
fail_check(const char *file, int line, const char *expression) {
    counters.failures += 1U;
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

#define CHECK(expression)                                                     \
    do {                                                                      \
        counters.checks += 1U;                                                \
        if (!(expression)) {                                                  \
            fail_check(__FILE__, __LINE__, #expression);                      \
        }                                                                     \
    } while (0)

static void
check_status_at(
    memx_status_t expected,
    memx_status_t actual,
    const char *expression,
    const char *file,
    int line) {
    counters.checks += 1U;
    if (actual != expected) {
        char message[256];
        (void)snprintf(message, sizeof(message),
            "%s expected %s, received %s", expression,
            memx_status_string(expected), memx_status_string(actual));
        fail_check(file, line, message);
    }
}

#define CHECK_STATUS(expected, expression)                                    \
    check_status_at((expected), (expression), #expression, __FILE__, __LINE__)

static void
check_handle_at(
    memx_handle_t expected,
    memx_handle_t actual,
    const char *expression,
    const char *file,
    int line) {
    counters.checks += 1U;
    if (actual != expected) {
        char message[256];
        (void)snprintf(message, sizeof(message),
            "%s expected 0x%" PRIxPTR ", received 0x%" PRIxPTR,
            expression, (uintptr_t)expected, (uintptr_t)actual);
        fail_check(file, line, message);
    }
}

#define CHECK_HANDLE(expected, expression)                                    \
    check_handle_at((expected), (expression), #expression, __FILE__, __LINE__)

static void
fault_allocator_init(fault_allocator_t *allocator) {
    memset(allocator, 0, sizeof(*allocator));
    allocator->fail_call = SIZE_MAX;
}

static void *
fault_allocate(void *context, size_t size) {
    fault_allocator_t *allocator = context;
    allocation_record_t *record;
    void *pointer;
    allocator->allocation_calls += 1U;
    if (allocator->allocation_calls == allocator->fail_call) {
        return NULL;
    }
    if (allocator->record_count == MAX_TRACKED_ALLOCATIONS) {
        allocator->record_overflow = true;
        return NULL;
    }
    pointer = malloc(size);
    if (pointer == NULL) {
        return NULL;
    }
    record = &allocator->records[allocator->record_count++];
    record->pointer = pointer;
    record->size = size;
    record->live = true;
    allocator->live_allocations += 1U;
    allocator->live_bytes += size;
    if (allocator->live_bytes > allocator->peak_live_bytes) {
        allocator->peak_live_bytes = allocator->live_bytes;
    }
    return pointer;
}

static void
fault_deallocate(void *context, void *pointer) {
    fault_allocator_t *allocator = context;
    size_t i;
    if (pointer == NULL) {
        return;
    }
    allocator->deallocation_calls += 1U;
    for (i = allocator->record_count; i > 0U; --i) {
        allocation_record_t *record = &allocator->records[i - 1U];
        if (record->pointer == pointer) {
            if (!record->live) {
                allocator->invalid_free = true;
                return;
            }
            record->live = false;
            allocator->live_allocations -= 1U;
            allocator->live_bytes -= record->size;
            free(pointer);
            return;
        }
    }
    allocator->invalid_free = true;
}

static memx_config_t
bounded_config(fault_allocator_t *allocator) {
    memx_config_t config = memx_config_default();
    config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    config.region_shift = FAULT_REGION_SHIFT;
    config.granule_shift = FAULT_GRANULE_SHIFT;
    config.base_address = FAULT_BASE;
    config.managed_size = (size_t)(FAULT_REGIONS * FAULT_REGION_SIZE);
    config.allocator.context = allocator;
    config.allocator.allocate = fault_allocate;
    config.allocator.deallocate = fault_deallocate;
    return config;
}

static memx_config_t
sparse_config(fault_allocator_t *allocator) {
    memx_config_t config = memx_config_default();
    config.directory_mode = MEMX_DIRECTORY_SPARSE;
    config.region_shift = FAULT_REGION_SHIFT;
    config.granule_shift = FAULT_GRANULE_SHIFT;
    config.base_address = 0U;
    config.managed_size = 0U;
    config.address_bits = 28U;
    config.allocator.context = allocator;
    config.allocator.allocate = fault_allocate;
    config.allocator.deallocate = fault_deallocate;
    return config;
}

static void
check_allocator_clean(const fault_allocator_t *allocator) {
    CHECK(!allocator->invalid_free);
    CHECK(!allocator->record_overflow);
    CHECK(allocator->live_allocations == 0U);
    CHECK(allocator->live_bytes == 0U);
    CHECK(allocator->deallocation_calls <= allocator->allocation_calls);
}

static void
check_range(
    const memx_index_t *index,
    uintptr_t begin,
    size_t granules,
    memx_handle_t expected) {
    size_t i;
    for (i = 0U; i < granules; ++i) {
        uintptr_t address = begin + i * FAULT_GRANULE_SIZE;
        CHECK_HANDLE(expected, memx_lookup_address(index, address));
        CHECK_HANDLE(expected,
            memx_lookup_address(index, address + FAULT_GRANULE_SIZE - 1U));
    }
}

static void
test_create_failure_at_every_allocation(void) {
    size_t fail_call;
    for (fail_call = 1U; fail_call <= 2U; ++fail_call) {
        fault_allocator_t allocator;
        memx_config_t config;
        memx_index_t *index = (memx_index_t *)(uintptr_t)1U;
        fault_allocator_init(&allocator);
        allocator.fail_call = fail_call;
        config = bounded_config(&allocator);
        CHECK_STATUS(MEMX_ERROR_OUT_OF_MEMORY,
            memx_index_create(&config, &index));
        CHECK(index == NULL);
        check_allocator_clean(&allocator);
    }
}

static void
test_cross_region_insert_failure_is_semantically_atomic(void) {
    /* Only the partial edge regions need dense tables; full middle regions
     * use inline UNIFORM descriptors and therefore allocate nothing. */
    size_t fail_call;
    for (fail_call = 3U; fail_call <= 6U; ++fail_call) {
        fault_allocator_t allocator;
        memx_config_t config;
        memx_index_t *index = NULL;
        uintptr_t start = FAULT_BASE + FAULT_REGION_SIZE
            - FAULT_GRANULE_SIZE;
        size_t granules = 2U * FAULT_GRANULES_PER_REGION + 2U;
        size_t bytes = granules * (size_t)FAULT_GRANULE_SIZE;
        memx_status_t status;
        fault_allocator_init(&allocator);
        config = bounded_config(&allocator);
        CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
        allocator.fail_call = fail_call;
        status = memx_insert(index, start, bytes, 77U);
        if (fail_call <= 4U) {
            CHECK_STATUS(MEMX_ERROR_OUT_OF_MEMORY, status);
            check_range(index, start, granules, MEMX_HANDLE_INVALID);
        } else {
            CHECK_STATUS(MEMX_OK, status);
            check_range(index, start, granules, 77U);
        }
        memx_index_destroy(index);
        check_allocator_clean(&allocator);
    }
}

static void
test_uniform_remove_failure_is_semantically_atomic(void) {
    /* Partial removal crosses three uniform regions.  The two edge regions
     * need tables; the whole middle region can be cleared inline. */
    size_t fail_call;
    for (fail_call = 3U; fail_call <= 6U; ++fail_call) {
        fault_allocator_t allocator;
        memx_config_t config;
        memx_index_t *index = NULL;
        uintptr_t remove_start = FAULT_BASE + FAULT_GRANULE_SIZE;
        size_t remove_granules = 3U * FAULT_GRANULES_PER_REGION - 2U;
        size_t remove_bytes = remove_granules * (size_t)FAULT_GRANULE_SIZE;
        size_t total_granules = 3U * FAULT_GRANULES_PER_REGION;
        memx_status_t status;
        fault_allocator_init(&allocator);
        config = bounded_config(&allocator);
        CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
        CHECK_STATUS(MEMX_OK, memx_insert(index, FAULT_BASE,
            3U * (size_t)FAULT_REGION_SIZE, 41U));
        allocator.fail_call = fail_call;
        status = memx_remove(index,
            remove_start, remove_bytes, 41U);
        if (fail_call <= 4U) {
            CHECK_STATUS(MEMX_ERROR_OUT_OF_MEMORY, status);
            check_range(index, FAULT_BASE, total_granules, 41U);
        } else {
            CHECK_STATUS(MEMX_OK, status);
            CHECK_HANDLE(41U, memx_lookup_address(index, FAULT_BASE));
            check_range(index, remove_start,
                remove_granules, MEMX_HANDLE_INVALID);
            CHECK_HANDLE(41U, memx_lookup_address(index,
                FAULT_BASE + 3U * FAULT_REGION_SIZE
                    - FAULT_GRANULE_SIZE));
        }
        memx_index_destroy(index);
        check_allocator_clean(&allocator);
    }
}

static void
fill_distinct(memx_index_t *index) {
    size_t slot;
    for (slot = 0U;
         slot < FAULT_REGIONS * FAULT_GRANULES_PER_REGION;
         ++slot) {
        CHECK_STATUS(MEMX_OK, memx_insert(index,
            FAULT_BASE + slot * FAULT_GRANULE_SIZE,
            (size_t)FAULT_GRANULE_SIZE,
            (memx_handle_t)(slot + 1U)));
    }
}

static void
check_distinct(const memx_index_t *index) {
    size_t slot;
    for (slot = 0U;
         slot < FAULT_REGIONS * FAULT_GRANULES_PER_REGION;
         ++slot) {
        CHECK_HANDLE((memx_handle_t)(slot + 1U),
            memx_lookup_address(index,
                FAULT_BASE + slot * FAULT_GRANULE_SIZE + 99U));
    }
}

static void
test_optimize_allocation_failure_preserves_adaptive_index(void) {
    fault_allocator_t allocator;
    memx_config_t config;
    memx_index_t *index = NULL;
    memx_bounded_view_t bounded_view;
    memx_flat_view_t flat_view;
    memx_stats_t stats;
    fault_allocator_init(&allocator);
    config = bounded_config(&allocator);
    CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
    fill_distinct(index);
    allocator.fail_call = allocator.allocation_calls + 1U;
    CHECK_STATUS(MEMX_ERROR_OUT_OF_MEMORY, memx_index_optimize(index));
    check_distinct(index);
    CHECK_STATUS(MEMX_OK, memx_index_bounded_view(index, &bounded_view));
    CHECK_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_flat_view(index, &flat_view));
    memx_index_stats(index, &stats);
    CHECK(stats.storage_mode == MEMX_STORAGE_ADAPTIVE);
    CHECK(stats.dense_regions == FAULT_REGIONS);
    allocator.fail_call = SIZE_MAX;
    CHECK_STATUS(MEMX_OK, memx_index_optimize(index));
    check_distinct(index);
    memx_index_stats(index, &stats);
    CHECK(stats.storage_mode == MEMX_STORAGE_FLAT);
    memx_index_destroy(index);
    check_allocator_clean(&allocator);
}

static void
test_overlay_releases_allocator_owned_dense_tables(void) {
    fault_allocator_t allocator;
    memx_config_t config;
    memx_index_t *index = NULL;
#if defined(__linux__)
    memx_overlay_view_t view;
    memx_stats_t stats;
#endif
    fault_allocator_init(&allocator);
    config = bounded_config(&allocator);
    CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
    fill_distinct(index);
#if defined(__linux__)
    CHECK(allocator.live_allocations == 2U + FAULT_REGIONS);
    CHECK_STATUS(MEMX_OK, memx_index_optimize_overlay(index));
    CHECK(allocator.live_allocations == 2U);
    CHECK(!allocator.invalid_free);
    CHECK_STATUS(MEMX_OK, memx_index_overlay_view(index, &view));
    check_distinct(index);
    CHECK_HANDLE(1U,
        memx_overlay_lookup_trusted(&view, FAULT_BASE + 7U));
    memx_index_stats(index, &stats);
    CHECK(stats.storage_mode == MEMX_STORAGE_OVERLAY);
    CHECK(stats.reserved_bytes > 0U);
#else
    CHECK_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_optimize_overlay(index));
#endif
    memx_index_destroy(index);
    check_allocator_clean(&allocator);
}

static void
test_sparse_leaf_and_dense_failures(void) {
    size_t fail_offset;
    for (fail_offset = 1U; fail_offset <= 3U; ++fail_offset) {
        fault_allocator_t allocator;
        memx_config_t config;
        memx_index_t *index = NULL;
        uintptr_t address = 0x0f010000U;
        memx_status_t status;
        fault_allocator_init(&allocator);
        config = sparse_config(&allocator);
        CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
        allocator.fail_call = allocator.allocation_calls + fail_offset;
        status = memx_insert(index, address,
            (size_t)FAULT_GRANULE_SIZE, 19U);
        if (fail_offset <= 2U) {
            CHECK_STATUS(MEMX_ERROR_OUT_OF_MEMORY, status);
            CHECK_HANDLE(MEMX_HANDLE_INVALID,
                memx_lookup_address(index, address));
        } else {
            CHECK_STATUS(MEMX_OK, status);
            CHECK_HANDLE(19U, memx_lookup_address(index, address));
        }
        memx_index_destroy(index);
        check_allocator_clean(&allocator);
    }
}

static void
test_failed_overlap_allocates_nothing(void) {
    fault_allocator_t allocator;
    memx_config_t config;
    memx_index_t *index = NULL;
    size_t before;
    fault_allocator_init(&allocator);
    config = bounded_config(&allocator);
    CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
    CHECK_STATUS(MEMX_OK, memx_insert(index, FAULT_BASE,
        (size_t)FAULT_GRANULE_SIZE, 3U));
    before = allocator.allocation_calls;
    CHECK_STATUS(MEMX_ERROR_OVERLAP, memx_insert(index, FAULT_BASE,
        2U * (size_t)FAULT_GRANULE_SIZE, 4U));
    CHECK(allocator.allocation_calls == before);
    CHECK_HANDLE(3U, memx_lookup_address(index, FAULT_BASE));
    CHECK_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, FAULT_BASE + FAULT_GRANULE_SIZE));
    memx_index_destroy(index);
    check_allocator_clean(&allocator);
}

static void
test_failed_expected_handle_is_non_destructive(void) {
    fault_allocator_t allocator;
    memx_config_t config;
    memx_index_t *index = NULL;
    size_t before;
    fault_allocator_init(&allocator);
    config = bounded_config(&allocator);
    CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
    CHECK_STATUS(MEMX_OK, memx_insert(index, FAULT_BASE,
        (size_t)FAULT_REGION_SIZE, 8U));
    before = allocator.allocation_calls;
    CHECK_STATUS(MEMX_ERROR_NOT_FOUND, memx_remove(index,
        FAULT_BASE + FAULT_GRANULE_SIZE,
        2U * (size_t)FAULT_GRANULE_SIZE, 9U));
    CHECK(allocator.allocation_calls == before);
    check_range(index, FAULT_BASE, FAULT_GRANULES_PER_REGION, 8U);
    memx_index_destroy(index);
    check_allocator_clean(&allocator);
}

int
main(void) {
    test_create_failure_at_every_allocation();
    test_cross_region_insert_failure_is_semantically_atomic();
    test_uniform_remove_failure_is_semantically_atomic();
    test_optimize_allocation_failure_preserves_adaptive_index();
    test_overlay_releases_allocator_owned_dense_tables();
    test_sparse_leaf_and_dense_failures();
    test_failed_overlap_allocates_nothing();
    test_failed_expected_handle_is_non_destructive();
    if (counters.failures != 0U) {
        fprintf(stderr, "%zu of %zu fault checks failed\n",
            counters.failures, counters.checks);
        return EXIT_FAILURE;
    }
    printf("all %zu fault checks passed\n", counters.checks);
    return EXIT_SUCCESS;
}
