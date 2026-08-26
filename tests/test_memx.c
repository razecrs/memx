#include "memx/memx.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_REGION_SHIFT 16U
#define TEST_GRANULE_SHIFT 12U
#define TEST_REGION_SIZE (((uintptr_t)1U) << TEST_REGION_SHIFT)
#define TEST_GRANULE_SIZE (((uintptr_t)1U) << TEST_GRANULE_SHIFT)

static unsigned tests_run;
static unsigned tests_failed;

#define EXPECT_TRUE(expression)                                                \
    do {                                                                       \
        tests_run += 1U;                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "%s:%d: expectation failed: %s\n",              \
                __FILE__, __LINE__, #expression);                              \
            tests_failed += 1U;                                                \
        }                                                                      \
    } while (0)

#define EXPECT_STATUS(expected, expression)                                    \
    do {                                                                       \
        memx_status_t actual_status = (expression);                            \
        tests_run += 1U;                                                       \
        if (actual_status != (expected)) {                                     \
            fprintf(stderr,                                                    \
                "%s:%d: expected status %s, got %s from %s\n",               \
                __FILE__, __LINE__, memx_status_string(expected),              \
                memx_status_string(actual_status), #expression);               \
            tests_failed += 1U;                                                \
        }                                                                      \
    } while (0)

#define EXPECT_HANDLE(expected, expression)                                    \
    do {                                                                       \
        memx_handle_t actual_handle = (expression);                            \
        tests_run += 1U;                                                       \
        if (actual_handle != (expected)) {                                     \
            fprintf(stderr,                                                    \
                "%s:%d: expected handle 0x%" PRIxPTR                         \
                ", got 0x%" PRIxPTR " from %s\n",                           \
                __FILE__, __LINE__, (uintptr_t)(expected),                     \
                (uintptr_t)actual_handle, #expression);                        \
            tests_failed += 1U;                                                \
        }                                                                      \
    } while (0)

static memx_config_t
bounded_config(uintptr_t base, size_t regions) {
    memx_config_t config = memx_config_default();
    config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    config.region_shift = TEST_REGION_SHIFT;
    config.granule_shift = TEST_GRANULE_SHIFT;
    config.base_address = base;
    config.managed_size = regions * (size_t)TEST_REGION_SIZE;
    return config;
}

static memx_config_t
sparse_config(void) {
    memx_config_t config = memx_config_default();
    config.directory_mode = MEMX_DIRECTORY_SPARSE;
    config.region_shift = TEST_REGION_SHIFT;
    config.granule_shift = TEST_GRANULE_SHIFT;
    config.base_address = 0U;
    config.managed_size = 0U;
    config.address_bits = 28U;
    return config;
}

static void
test_default_config_requires_range(void) {
    memx_config_t config = memx_config_default();
    memx_index_t *index = NULL;
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_index_create(&config, &index));
    EXPECT_TRUE(index == NULL);
}

static void
test_create_argument_validation(void) {
    memx_config_t config = bounded_config(0x100000U, 4U);
    memx_index_t *index = NULL;

    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_index_create(NULL, &index));
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_index_create(&config, NULL));

    config.region_shift = TEST_GRANULE_SHIFT - 1U;
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_index_create(&config, &index));

    config = bounded_config(0x100001U, 4U);
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_index_create(&config, &index));

    config = bounded_config(0x100000U, 4U);
    config.managed_size -= 1U;
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_index_create(&config, &index));

    config = sparse_config();
    config.address_bits = TEST_REGION_SHIFT;
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_index_create(&config, &index));

    config = sparse_config();
    config.address_bits = TEST_REGION_SHIFT + 33U;
    if (config.address_bits <= sizeof(uintptr_t) * 8U) {
        EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
            memx_index_create(&config, &index));
    }
}

static void
test_empty_lookup(void) {
    const uintptr_t base = 0x200000U;
    memx_config_t config = bounded_config(base, 2U);
    memx_index_t *index = NULL;
    memx_stats_t stats;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, base));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, base + TEST_REGION_SIZE - 1U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, base - 1U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, base + 2U * TEST_REGION_SIZE));

    memx_index_stats(index, &stats);
    EXPECT_TRUE(stats.region_count == 2U);
    EXPECT_TRUE(stats.empty_regions == 2U);
    EXPECT_TRUE(stats.uniform_regions == 0U);
    EXPECT_TRUE(stats.dense_regions == 0U);
    EXPECT_TRUE(stats.total_bytes >= stats.directory_bytes);
    memx_index_destroy(index);
}

static void
test_full_region_uniform(void) {
    const uintptr_t base = 0x300000U;
    const memx_handle_t handle = 42U;
    memx_config_t config = bounded_config(base, 2U);
    memx_index_t *index = NULL;
    memx_region_type_t type = MEMX_REGION_EMPTY;
    memx_stats_t stats;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, base, (size_t)TEST_REGION_SIZE, handle));
    EXPECT_HANDLE(handle, memx_lookup_address(index, base));
    EXPECT_HANDLE(handle,
        memx_lookup_address(index, base + TEST_GRANULE_SIZE + 7U));
    EXPECT_HANDLE(handle,
        memx_lookup_address(index, base + TEST_REGION_SIZE - 1U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, base + TEST_REGION_SIZE));
    EXPECT_STATUS(MEMX_OK, memx_region_type_at(index, base, &type));
    EXPECT_TRUE(type == MEMX_REGION_UNIFORM);

    memx_index_stats(index, &stats);
    EXPECT_TRUE(stats.uniform_regions == 1U);
    EXPECT_TRUE(stats.dense_regions == 0U);

    EXPECT_STATUS(MEMX_OK,
        memx_remove(index, base, (size_t)TEST_REGION_SIZE, handle));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, base));
    EXPECT_STATUS(MEMX_OK, memx_region_type_at(index, base, &type));
    EXPECT_TRUE(type == MEMX_REGION_EMPTY);
    memx_index_destroy(index);
}

static void
test_partial_region_dense(void) {
    const uintptr_t base = 0x400000U;
    const uintptr_t target = base + 3U * TEST_GRANULE_SIZE;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;
    memx_region_type_t type = MEMX_REGION_EMPTY;
    memx_stats_t stats;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, target, (size_t)TEST_GRANULE_SIZE, 7U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, base));
    EXPECT_HANDLE(7U, memx_lookup_address(index, target));
    EXPECT_HANDLE(7U, memx_lookup_address(index, target + 99U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, target + TEST_GRANULE_SIZE));
    EXPECT_STATUS(MEMX_OK, memx_region_type_at(index, target, &type));
    EXPECT_TRUE(type == MEMX_REGION_DENSE);

    memx_index_stats(index, &stats);
    EXPECT_TRUE(stats.dense_regions == 1U);
    EXPECT_TRUE(stats.representation_bytes
        == (TEST_REGION_SIZE / TEST_GRANULE_SIZE) * sizeof(memx_handle_t));

    EXPECT_STATUS(MEMX_OK,
        memx_remove(index, target, (size_t)TEST_GRANULE_SIZE, 7U));
    EXPECT_STATUS(MEMX_OK, memx_region_type_at(index, target, &type));
    EXPECT_TRUE(type == MEMX_REGION_EMPTY);
    memx_index_destroy(index);
}

static void
test_zero_handle_is_valid(void) {
    const uintptr_t base = 0x500000U;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, base, (size_t)TEST_REGION_SIZE, 0U));
    EXPECT_HANDLE(0U, memx_lookup_address(index, base + 123U));
    EXPECT_STATUS(MEMX_OK,
        memx_remove(index, base, (size_t)TEST_REGION_SIZE, 0U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, base));
    memx_index_destroy(index);
}

static void
test_invalid_handle_rejected(void) {
    const uintptr_t base = 0x600000U;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;
    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_insert(index, base, (size_t)TEST_GRANULE_SIZE,
            MEMX_HANDLE_INVALID));
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_remove(index, base, (size_t)TEST_GRANULE_SIZE,
            MEMX_HANDLE_INVALID));
    memx_index_destroy(index);
}

static void
test_large_handle_uses_dense_without_losing_bits(void) {
    const uintptr_t base = 0x680000U;
    const memx_handle_t handle = (UINTPTR_MAX >> 1U) ^ (uintptr_t)0x1234U;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;
    memx_region_type_t type;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, base, (size_t)TEST_REGION_SIZE, handle));
    EXPECT_HANDLE(handle, memx_lookup_address(index, base));
    EXPECT_HANDLE(handle,
        memx_lookup_address(index, base + TEST_REGION_SIZE - 1U));
    EXPECT_STATUS(MEMX_OK, memx_region_type_at(index, base, &type));
    EXPECT_TRUE(type == MEMX_REGION_DENSE);
    EXPECT_STATUS(MEMX_OK,
        memx_remove(index, base, (size_t)TEST_REGION_SIZE, handle));
    memx_index_destroy(index);
}

static void
test_overlap_is_non_destructive(void) {
    const uintptr_t base = 0x700000U;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;
    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, base, 2U * (size_t)TEST_GRANULE_SIZE, 11U));
    EXPECT_STATUS(MEMX_ERROR_OVERLAP,
        memx_insert(index, base + TEST_GRANULE_SIZE,
            2U * (size_t)TEST_GRANULE_SIZE, 12U));
    EXPECT_HANDLE(11U, memx_lookup_address(index, base));
    EXPECT_HANDLE(11U,
        memx_lookup_address(index, base + TEST_GRANULE_SIZE));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, base + 2U * TEST_GRANULE_SIZE));
    memx_index_destroy(index);
}

static void
test_remove_requires_expected_handle(void) {
    const uintptr_t base = 0x800000U;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;
    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, base, (size_t)TEST_GRANULE_SIZE, 21U));
    EXPECT_STATUS(MEMX_ERROR_NOT_FOUND,
        memx_remove(index, base, (size_t)TEST_GRANULE_SIZE, 22U));
    EXPECT_HANDLE(21U, memx_lookup_address(index, base));
    EXPECT_STATUS(MEMX_OK,
        memx_remove(index, base, (size_t)TEST_GRANULE_SIZE, 21U));
    memx_index_destroy(index);
}

static void
test_range_validation(void) {
    const uintptr_t base = 0x900000U;
    memx_config_t config = bounded_config(base, 2U);
    memx_index_t *index = NULL;
    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));

    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_insert(index, base, 0U, 1U));
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_insert(index, base + 1U, (size_t)TEST_GRANULE_SIZE, 1U));
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_insert(index, base, (size_t)TEST_GRANULE_SIZE - 1U, 1U));
    EXPECT_STATUS(MEMX_ERROR_OUT_OF_RANGE,
        memx_insert(index, base - TEST_GRANULE_SIZE,
            (size_t)TEST_GRANULE_SIZE, 1U));
    EXPECT_STATUS(MEMX_ERROR_OUT_OF_RANGE,
        memx_insert(index, base + 2U * TEST_REGION_SIZE,
            (size_t)TEST_GRANULE_SIZE, 1U));
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_insert(index, UINTPTR_MAX - TEST_GRANULE_SIZE + 1U,
            (size_t)TEST_GRANULE_SIZE, 1U));
    memx_index_destroy(index);
}

static void
test_cross_region_range(void) {
    const uintptr_t base = 0xa00000U;
    const uintptr_t start = base + TEST_REGION_SIZE - 2U * TEST_GRANULE_SIZE;
    const size_t size = 4U * (size_t)TEST_GRANULE_SIZE;
    memx_config_t config = bounded_config(base, 2U);
    memx_index_t *index = NULL;
    memx_region_type_t left;
    memx_region_type_t right;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK, memx_insert(index, start, size, 55U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, start - TEST_GRANULE_SIZE));
    EXPECT_HANDLE(55U, memx_lookup_address(index, start));
    EXPECT_HANDLE(55U,
        memx_lookup_address(index, base + TEST_REGION_SIZE));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, start + size));
    EXPECT_STATUS(MEMX_OK, memx_region_type_at(index, start, &left));
    EXPECT_STATUS(MEMX_OK,
        memx_region_type_at(index, base + TEST_REGION_SIZE, &right));
    EXPECT_TRUE(left == MEMX_REGION_DENSE);
    EXPECT_TRUE(right == MEMX_REGION_DENSE);
    EXPECT_STATUS(MEMX_OK, memx_remove(index, start, size, 55U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, start));
    memx_index_destroy(index);
}

static void
test_fill_dense_reclassifies_uniform(void) {
    const uintptr_t base = 0xb00000U;
    const size_t granules = TEST_REGION_SIZE / TEST_GRANULE_SIZE;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;
    memx_region_type_t type;
    size_t i;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    for (i = 0U; i < granules; ++i) {
        EXPECT_STATUS(MEMX_OK,
            memx_insert(index, base + i * TEST_GRANULE_SIZE,
                (size_t)TEST_GRANULE_SIZE, 99U));
    }
    EXPECT_STATUS(MEMX_OK, memx_region_type_at(index, base, &type));
    EXPECT_TRUE(type == MEMX_REGION_UNIFORM);
    EXPECT_HANDLE(99U,
        memx_lookup_address(index, base + TEST_REGION_SIZE - 1U));
    memx_index_destroy(index);
}

static void
test_partial_remove_from_uniform(void) {
    const uintptr_t base = 0xc00000U;
    const uintptr_t hole = base + 5U * TEST_GRANULE_SIZE;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;
    memx_region_type_t type;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, base, (size_t)TEST_REGION_SIZE, 101U));
    EXPECT_STATUS(MEMX_OK,
        memx_remove(index, hole, (size_t)TEST_GRANULE_SIZE, 101U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, hole));
    EXPECT_HANDLE(101U, memx_lookup_address(index, base));
    EXPECT_HANDLE(101U,
        memx_lookup_address(index, hole + TEST_GRANULE_SIZE));
    EXPECT_STATUS(MEMX_OK, memx_region_type_at(index, base, &type));
    EXPECT_TRUE(type == MEMX_REGION_DENSE);
    memx_index_destroy(index);
}

static void
test_sparse_directory(void) {
    const uintptr_t low = 0x10000U;
    const uintptr_t high = 0x0f000000U;
    memx_config_t config = sparse_config();
    memx_index_t *index = NULL;
    memx_stats_t before;
    memx_stats_t after;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    memx_index_stats(index, &before);
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, low));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, low, (size_t)TEST_GRANULE_SIZE, 1U));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, high, (size_t)TEST_GRANULE_SIZE, 2U));
    EXPECT_HANDLE(1U, memx_lookup_address(index, low + 8U));
    EXPECT_HANDLE(2U, memx_lookup_address(index, high + 8U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, ((uintptr_t)1U) << config.address_bits));
    memx_index_stats(index, &after);
    EXPECT_TRUE(after.directory_bytes > before.directory_bytes);
    EXPECT_TRUE(after.dense_regions == 2U);
    EXPECT_STATUS(MEMX_OK,
        memx_remove(index, low, (size_t)TEST_GRANULE_SIZE, 1U));
    EXPECT_STATUS(MEMX_OK,
        memx_remove(index, high, (size_t)TEST_GRANULE_SIZE, 2U));
    memx_index_stats(index, &after);
    EXPECT_TRUE(after.directory_bytes == before.directory_bytes);
    EXPECT_TRUE(after.dense_regions == 0U);
    memx_index_destroy(index);
}

static void
test_invalid_descriptor_tags_are_rejected(void) {
    const uintptr_t base = 0xca0000U;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;
    memx_bounded_view_t view;
    uintptr_t *descriptors;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_OK, memx_index_bounded_view(index, &view));
    /* Deliberately corrupt an internal descriptor for the diagnostic test.
     * memcpy makes the test-only const escape explicit without teaching the
     * production API to expose mutable descriptors. */
    memcpy(&descriptors, &view.descriptors, sizeof(descriptors));
    descriptors[0] = (uintptr_t)3U;
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, base));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_bounded_lookup_assume_mapped(&view, base));
#if defined(__linux__)
    EXPECT_STATUS(MEMX_ERROR_INVALID_ARGUMENT,
        memx_index_optimize_overlay(index));
#else
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_optimize_overlay(index));
#endif
    memx_index_destroy(index);
}

static void
test_dense_index_flat_optimization(void) {
    const uintptr_t base = 0xc80000U;
    const size_t regions = 2U;
    const size_t granules = regions * TEST_REGION_SIZE / TEST_GRANULE_SIZE;
    memx_config_t config = bounded_config(base, regions);
    memx_index_t *index = NULL;
    memx_bounded_view_t bounded_view;
    memx_flat_view_t flat_view;
    memx_stats_t before;
    memx_stats_t after;
    size_t i;

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_flat_view(index, &flat_view));
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED, memx_index_optimize(index));

    for (i = 0U; i < granules; ++i) {
        EXPECT_STATUS(MEMX_OK, memx_insert(index,
            base + i * TEST_GRANULE_SIZE,
            (size_t)TEST_GRANULE_SIZE,
            (memx_handle_t)(1000U + i)));
    }
    memx_index_stats(index, &before);
    EXPECT_TRUE(before.storage_mode == MEMX_STORAGE_ADAPTIVE);
    EXPECT_TRUE(before.dense_regions == regions);
    EXPECT_STATUS(MEMX_OK, memx_index_optimize(index));
    EXPECT_STATUS(MEMX_OK, memx_index_optimize(index));
    memx_index_stats(index, &after);
    EXPECT_TRUE(after.storage_mode == MEMX_STORAGE_FLAT);
    EXPECT_TRUE(after.dense_regions == regions);
    EXPECT_TRUE(after.total_bytes <= before.total_bytes);
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_bounded_view(index, &bounded_view));
    EXPECT_STATUS(MEMX_OK, memx_index_flat_view(index, &flat_view));

    for (i = 0U; i < granules; ++i) {
        uintptr_t address = base + i * TEST_GRANULE_SIZE;
        memx_handle_t expected = (memx_handle_t)(1000U + i);
        EXPECT_HANDLE(expected, memx_lookup_address(index, address));
        EXPECT_HANDLE(expected,
            memx_flat_lookup_assume_mapped(&flat_view, address));
    }

    EXPECT_STATUS(MEMX_OK, memx_remove(index, base,
        (size_t)TEST_GRANULE_SIZE, 1000U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, base));
    EXPECT_STATUS(MEMX_OK, memx_insert(index, base,
        (size_t)TEST_GRANULE_SIZE, 77U));
    EXPECT_HANDLE(77U, memx_lookup_address(index, base));
    memx_index_destroy(index);
}

static void
test_flat_optimization_rejects_sparse(void) {
    memx_config_t config = sparse_config();
    memx_index_t *index = NULL;
    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED, memx_index_optimize(index));
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_optimize_overlay(index));
    memx_index_destroy(index);
}

static void
test_mixed_index_overlay_optimization(void) {
    const uintptr_t base = 0xce0000U;
    const uintptr_t uniform = base + TEST_REGION_SIZE;
    const uintptr_t dense = uniform + TEST_REGION_SIZE;
    const uintptr_t dense_last = dense + TEST_REGION_SIZE
        - TEST_GRANULE_SIZE;
    memx_config_t config = bounded_config(base, 3U);
    memx_index_t *index = NULL;
    memx_bounded_view_t old_view;
    memx_overlay_view_t overlay_view;
#if defined(__linux__)
    memx_flat_view_t flat_view;
#endif
    memx_stats_t before;
#if defined(__linux__)
    memx_stats_t after;
#endif

    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_overlay_view(index, &overlay_view));
    EXPECT_STATUS(MEMX_OK,
        memx_insert(index, uniform, (size_t)TEST_REGION_SIZE, 0U));
    EXPECT_STATUS(MEMX_OK, memx_insert(index, dense,
        (size_t)TEST_GRANULE_SIZE, MEMX_HANDLE_INVALID - 1U));
    EXPECT_STATUS(MEMX_OK, memx_insert(index, dense_last,
        (size_t)TEST_GRANULE_SIZE, 42U));
    EXPECT_STATUS(MEMX_OK, memx_index_bounded_view(index, &old_view));
    memx_index_stats(index, &before);
    EXPECT_TRUE(before.storage_mode == MEMX_STORAGE_ADAPTIVE);

#if defined(__linux__)
    EXPECT_STATUS(MEMX_OK, memx_index_optimize_overlay(index));
    EXPECT_STATUS(MEMX_OK, memx_index_optimize_overlay(index));
    memx_index_stats(index, &after);
    EXPECT_TRUE(after.storage_mode == MEMX_STORAGE_OVERLAY);
    EXPECT_TRUE(after.empty_regions == 1U);
    EXPECT_TRUE(after.uniform_regions == 1U);
    EXPECT_TRUE(after.dense_regions == 1U);
    EXPECT_TRUE(after.reserved_bytes >= 3U * TEST_REGION_SIZE
        / TEST_GRANULE_SIZE * sizeof(memx_handle_t));
    EXPECT_TRUE(after.representation_bytes >= before.representation_bytes);
    EXPECT_TRUE(after.total_bytes == after.directory_bytes
        + after.representation_bytes);
    EXPECT_TRUE(after.page_accounted_bytes >= after.total_bytes);
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_bounded_view(index, &old_view));
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_flat_view(index, &flat_view));
    EXPECT_STATUS(MEMX_OK, memx_index_overlay_view(index, &overlay_view));

    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, base));
    EXPECT_HANDLE(0U, memx_lookup_address(index, uniform + 17U));
    EXPECT_HANDLE(0U,
        memx_overlay_lookup_trusted(&overlay_view, uniform + 31U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, dense + TEST_GRANULE_SIZE));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID - 1U,
        memx_lookup_address(index, dense + 7U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID - 1U,
        memx_overlay_lookup_trusted(&overlay_view, dense + 9U));
    EXPECT_HANDLE(42U, memx_lookup_address(index, dense_last + 99U));
    EXPECT_HANDLE(42U,
        memx_overlay_lookup_trusted(&overlay_view, dense_last + 101U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, base - 1U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index,
        base + 3U * TEST_REGION_SIZE));
    EXPECT_STATUS(MEMX_ERROR_BUSY,
        memx_insert(index, base, (size_t)TEST_GRANULE_SIZE, 7U));
    EXPECT_STATUS(MEMX_ERROR_BUSY,
        memx_remove(index, uniform, (size_t)TEST_GRANULE_SIZE, 0U));
    EXPECT_STATUS(MEMX_ERROR_BUSY, memx_index_optimize(index));
#else
    EXPECT_STATUS(MEMX_ERROR_UNSUPPORTED,
        memx_index_optimize_overlay(index));
#endif
    memx_index_destroy(index);
}

typedef struct fail_allocator_state {
    size_t allocation_count;
    size_t fail_at;
} fail_allocator_state_t;

static void *
fail_allocate(void *context, size_t size) {
    fail_allocator_state_t *state = context;
    state->allocation_count += 1U;
    if (state->allocation_count == state->fail_at) {
        return NULL;
    }
    return malloc(size);
}

static void
fail_deallocate(void *context, void *pointer) {
    (void)context;
    free(pointer);
}

typedef struct misaligned_allocator_state {
    void *allocation;
    bool deallocated;
} misaligned_allocator_state_t;

static void *
misaligned_allocate(void *context, size_t size) {
    misaligned_allocator_state_t *state = context;
    unsigned char *allocation = malloc(size + _Alignof(max_align_t));
    if (allocation == NULL) {
        return NULL;
    }
    state->allocation = allocation;
    return allocation + 1U;
}

static void
misaligned_deallocate(void *context, void *pointer) {
    misaligned_allocator_state_t *state = context;
    (void)pointer;
    free(state->allocation);
    state->allocation = NULL;
    state->deallocated = true;
}

static void
test_custom_allocator_alignment_contract(void) {
    const uintptr_t base = 0xcf0000U;
    misaligned_allocator_state_t state;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;

    memset(&state, 0, sizeof(state));
    config.allocator.context = &state;
    config.allocator.allocate = misaligned_allocate;
    config.allocator.deallocate = misaligned_deallocate;
    EXPECT_STATUS(MEMX_ERROR_OUT_OF_MEMORY,
        memx_index_create(&config, &index));
    EXPECT_TRUE(index == NULL);
    EXPECT_TRUE(state.deallocated);
    EXPECT_TRUE(state.allocation == NULL);
}

static void
test_custom_allocator_failures(void) {
    const uintptr_t base = 0xd00000U;
    fail_allocator_state_t state;
    memx_config_t config = bounded_config(base, 1U);
    memx_index_t *index = NULL;

    memset(&state, 0, sizeof(state));
    state.fail_at = 1U;
    config.allocator.context = &state;
    config.allocator.allocate = fail_allocate;
    config.allocator.deallocate = fail_deallocate;
    EXPECT_STATUS(MEMX_ERROR_OUT_OF_MEMORY,
        memx_index_create(&config, &index));
    EXPECT_TRUE(index == NULL);

    memset(&state, 0, sizeof(state));
    state.fail_at = 2U;
    config.allocator.context = &state;
    EXPECT_STATUS(MEMX_ERROR_OUT_OF_MEMORY,
        memx_index_create(&config, &index));
    EXPECT_TRUE(index == NULL);

    memset(&state, 0, sizeof(state));
    state.fail_at = 3U;
    config.allocator.context = &state;
    EXPECT_STATUS(MEMX_OK, memx_index_create(&config, &index));
    EXPECT_STATUS(MEMX_ERROR_OUT_OF_MEMORY,
        memx_insert(index, base, (size_t)TEST_GRANULE_SIZE, 3U));
    EXPECT_HANDLE(MEMX_HANDLE_INVALID, memx_lookup_address(index, base));
    memx_index_destroy(index);
}

static uint64_t
random_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void
test_randomized_bounded_sparse_differential(void) {
    enum { GRANULES = 4096, OPERATIONS = 25000 };
    const uintptr_t bounded_base = 0x1000000U;
    memx_config_t bounded = bounded_config(bounded_base,
        (GRANULES * TEST_GRANULE_SIZE) / TEST_REGION_SIZE);
    memx_config_t sparse = sparse_config();
    memx_index_t *bounded_index = NULL;
    memx_index_t *sparse_index = NULL;
    memx_handle_t *oracle = malloc(GRANULES * sizeof(*oracle));
    uint64_t random_state = UINT64_C(0x4d656d5874657374);
    size_t i;

    EXPECT_TRUE(oracle != NULL);
    if (oracle == NULL) {
        return;
    }
    for (i = 0U; i < GRANULES; ++i) {
        oracle[i] = MEMX_HANDLE_INVALID;
    }
    EXPECT_STATUS(MEMX_OK, memx_index_create(&bounded, &bounded_index));
    EXPECT_STATUS(MEMX_OK, memx_index_create(&sparse, &sparse_index));

    for (i = 0U; i < OPERATIONS; ++i) {
        size_t slot = (size_t)(random_next(&random_state) % GRANULES);
        uintptr_t bounded_address = bounded_base + slot * TEST_GRANULE_SIZE;
        uintptr_t sparse_address = slot * TEST_GRANULE_SIZE;
        if ((random_next(&random_state) & 3U) == 0U
            && oracle[slot] != MEMX_HANDLE_INVALID) {
            memx_handle_t expected = oracle[slot];
            EXPECT_STATUS(MEMX_OK, memx_remove(bounded_index,
                bounded_address, (size_t)TEST_GRANULE_SIZE, expected));
            EXPECT_STATUS(MEMX_OK, memx_remove(sparse_index,
                sparse_address, (size_t)TEST_GRANULE_SIZE, expected));
            oracle[slot] = MEMX_HANDLE_INVALID;
        } else if (oracle[slot] == MEMX_HANDLE_INVALID) {
            memx_handle_t value = (memx_handle_t)
                (random_next(&random_state) & UINT64_C(0xffff));
            EXPECT_STATUS(MEMX_OK, memx_insert(bounded_index,
                bounded_address, (size_t)TEST_GRANULE_SIZE, value));
            EXPECT_STATUS(MEMX_OK, memx_insert(sparse_index,
                sparse_address, (size_t)TEST_GRANULE_SIZE, value));
            oracle[slot] = value;
        }

        EXPECT_HANDLE(oracle[slot],
            memx_lookup_address(bounded_index, bounded_address));
        EXPECT_HANDLE(oracle[slot],
            memx_lookup_address(sparse_index, sparse_address));
    }

    for (i = 0U; i < GRANULES; ++i) {
        EXPECT_HANDLE(oracle[i], memx_lookup_address(
            bounded_index, bounded_base + i * TEST_GRANULE_SIZE));
        EXPECT_HANDLE(oracle[i], memx_lookup_address(
            sparse_index, i * TEST_GRANULE_SIZE));
    }
#if defined(__linux__)
    {
        memx_overlay_view_t view;
        EXPECT_STATUS(MEMX_OK,
            memx_index_optimize_overlay(bounded_index));
        EXPECT_STATUS(MEMX_OK,
            memx_index_overlay_view(bounded_index, &view));
        for (i = 0U; i < GRANULES; ++i) {
            uintptr_t address = bounded_base + i * TEST_GRANULE_SIZE;
            EXPECT_HANDLE(oracle[i],
                memx_lookup_address(bounded_index, address));
            if (oracle[i] != MEMX_HANDLE_INVALID) {
                EXPECT_HANDLE(oracle[i],
                    memx_overlay_lookup_trusted(&view, address + 127U));
            }
        }
    }
#endif
    memx_index_destroy(bounded_index);
    memx_index_destroy(sparse_index);
    free(oracle);
}

int
main(void) {
    test_default_config_requires_range();
    test_create_argument_validation();
    test_empty_lookup();
    test_full_region_uniform();
    test_partial_region_dense();
    test_zero_handle_is_valid();
    test_invalid_handle_rejected();
    test_large_handle_uses_dense_without_losing_bits();
    test_overlap_is_non_destructive();
    test_remove_requires_expected_handle();
    test_range_validation();
    test_cross_region_range();
    test_fill_dense_reclassifies_uniform();
    test_partial_remove_from_uniform();
    test_sparse_directory();
    test_invalid_descriptor_tags_are_rejected();
    test_dense_index_flat_optimization();
    test_flat_optimization_rejects_sparse();
    test_mixed_index_overlay_optimization();
    test_custom_allocator_failures();
    test_custom_allocator_alignment_contract();
    test_randomized_bounded_sparse_differential();

    if (tests_failed != 0U) {
        fprintf(stderr, "%u of %u expectations failed\n",
            tests_failed, tests_run);
        return EXIT_FAILURE;
    }
    printf("all %u expectations passed\n", tests_run);
    return EXIT_SUCCESS;
}
