#include "memx/memx.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * This suite deliberately uses tiny regions.  It can enumerate every mapping
 * over eight granules while still exercising the same descriptor transitions
 * as production-sized regions.  The array model has no representation logic,
 * so agreement is a useful differential oracle rather than a second copy of
 * the MemX algorithm.
 */
#define EX_GRANULE_SHIFT 12U
#define EX_REGION_SHIFT 15U
#define EX_GRANULE_SIZE (((uintptr_t)1U) << EX_GRANULE_SHIFT)
#define EX_REGION_SIZE (((uintptr_t)1U) << EX_REGION_SHIFT)
#define EX_GRANULES_PER_REGION 8U
#define EX_BOUNDED_BASE ((uintptr_t)0x200000U)
#define EX_REGIONS 3U
#define EX_TOTAL_GRANULES (EX_REGIONS * EX_GRANULES_PER_REGION)

typedef struct model {
    uintptr_t base;
    size_t granules;
    memx_handle_t entries[EX_TOTAL_GRANULES];
} model_t;

typedef struct test_state {
    size_t checks;
    size_t failures;
} test_state_t;

static test_state_t test_state;

static void
record_failure(const char *file, int line, const char *message) {
    test_state.failures += 1U;
    fprintf(stderr, "%s:%d: %s\n", file, line, message);
}

#define CHECK(expression)                                                     \
    do {                                                                      \
        test_state.checks += 1U;                                              \
        if (!(expression)) {                                                  \
            record_failure(__FILE__, __LINE__, #expression);                  \
        }                                                                     \
    } while (0)

static void
check_status_at(
    memx_status_t expected,
    memx_status_t actual,
    const char *expression,
    const char *file,
    int line) {
    test_state.checks += 1U;
    if (expected != actual) {
        char message[256];
        (void)snprintf(message, sizeof(message),
            "%s: expected %s, received %s", expression,
            memx_status_string(expected), memx_status_string(actual));
        record_failure(file, line, message);
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
    test_state.checks += 1U;
    if (expected != actual) {
        char message[256];
        (void)snprintf(message, sizeof(message),
            "%s: expected 0x%" PRIxPTR ", received 0x%" PRIxPTR,
            expression, (uintptr_t)expected, (uintptr_t)actual);
        record_failure(file, line, message);
    }
}

#define CHECK_HANDLE(expected, expression)                                    \
    check_handle_at((expected), (expression), #expression, __FILE__, __LINE__)

static memx_config_t
bounded_config(void) {
    memx_config_t config = memx_config_default();
    config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    config.region_shift = EX_REGION_SHIFT;
    config.granule_shift = EX_GRANULE_SHIFT;
    config.base_address = EX_BOUNDED_BASE;
    config.managed_size = (size_t)(EX_REGIONS * EX_REGION_SIZE);
    return config;
}

static memx_config_t
sparse_config(void) {
    memx_config_t config = memx_config_default();
    config.directory_mode = MEMX_DIRECTORY_SPARSE;
    config.region_shift = EX_REGION_SHIFT;
    config.granule_shift = EX_GRANULE_SHIFT;
    config.base_address = 0U;
    config.managed_size = 0U;
    config.address_bits = 24U;
    return config;
}

static void
model_init(model_t *model, uintptr_t base, size_t granules) {
    size_t i;
    model->base = base;
    model->granules = granules;
    for (i = 0U; i < EX_TOTAL_GRANULES; ++i) {
        model->entries[i] = MEMX_HANDLE_INVALID;
    }
}

static memx_status_t
model_range(
    const model_t *model,
    uintptr_t base,
    size_t size,
    size_t *out_first,
    size_t *out_count) {
    uintptr_t relative;
    size_t first;
    size_t count;
    if (size == 0U || (base & (EX_GRANULE_SIZE - 1U)) != 0U
        || (size & (size_t)(EX_GRANULE_SIZE - 1U)) != 0U
        || base < model->base) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    relative = base - model->base;
    first = (size_t)(relative >> EX_GRANULE_SHIFT);
    count = size >> EX_GRANULE_SHIFT;
    if (first >= model->granules || count > model->granules - first) {
        return MEMX_ERROR_OUT_OF_RANGE;
    }
    *out_first = first;
    *out_count = count;
    return MEMX_OK;
}

static memx_status_t
model_insert(
    model_t *model,
    uintptr_t base,
    size_t size,
    memx_handle_t handle) {
    size_t first;
    size_t count;
    size_t i;
    memx_status_t status;
    if (handle == MEMX_HANDLE_INVALID) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    status = model_range(model, base, size, &first, &count);
    if (status != MEMX_OK) {
        return status;
    }
    for (i = 0U; i < count; ++i) {
        if (model->entries[first + i] != MEMX_HANDLE_INVALID) {
            return MEMX_ERROR_OVERLAP;
        }
    }
    for (i = 0U; i < count; ++i) {
        model->entries[first + i] = handle;
    }
    return MEMX_OK;
}

static memx_status_t
model_remove(
    model_t *model,
    uintptr_t base,
    size_t size,
    memx_handle_t expected) {
    size_t first;
    size_t count;
    size_t i;
    memx_status_t status;
    if (expected == MEMX_HANDLE_INVALID) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    status = model_range(model, base, size, &first, &count);
    if (status != MEMX_OK) {
        return status;
    }
    for (i = 0U; i < count; ++i) {
        if (model->entries[first + i] != expected) {
            return MEMX_ERROR_NOT_FOUND;
        }
    }
    for (i = 0U; i < count; ++i) {
        model->entries[first + i] = MEMX_HANDLE_INVALID;
    }
    return MEMX_OK;
}

static memx_handle_t
model_lookup(const model_t *model, uintptr_t address) {
    size_t slot;
    if (address < model->base) {
        return MEMX_HANDLE_INVALID;
    }
    slot = (size_t)((address - model->base) >> EX_GRANULE_SHIFT);
    if (slot >= model->granules) {
        return MEMX_HANDLE_INVALID;
    }
    return model->entries[slot];
}

static void
compare_every_byte_sample(
    const memx_index_t *index,
    const model_t *model) {
    size_t slot;
    static const uintptr_t offsets[] = {
        0U, 1U, 17U, EX_GRANULE_SIZE / 2U, EX_GRANULE_SIZE - 1U
    };
    size_t sample;
    CHECK_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index, model->base - 1U));
    for (slot = 0U; slot < model->granules; ++slot) {
        uintptr_t base = model->base + slot * EX_GRANULE_SIZE;
        for (sample = 0U;
             sample < sizeof(offsets) / sizeof(offsets[0]);
             ++sample) {
            uintptr_t address = base + offsets[sample];
            CHECK_HANDLE(model_lookup(model, address),
                memx_lookup_address(index, address));
        }
    }
    CHECK_HANDLE(MEMX_HANDLE_INVALID,
        memx_lookup_address(index,
            model->base + model->granules * EX_GRANULE_SIZE));
}

static void
populate_runs(
    memx_index_t *index,
    model_t *model,
    const memx_handle_t *desired,
    size_t granules) {
    size_t start = 0U;
    while (start < granules) {
        memx_handle_t handle = desired[start];
        size_t end = start + 1U;
        if (handle == MEMX_HANDLE_INVALID) {
            start += 1U;
            continue;
        }
        while (end < granules && desired[end] == handle) {
            end += 1U;
        }
        CHECK_STATUS(MEMX_OK, memx_insert(index,
            model->base + start * EX_GRANULE_SIZE,
            (end - start) * (size_t)EX_GRANULE_SIZE,
            handle));
        CHECK_STATUS(MEMX_OK, model_insert(model,
            model->base + start * EX_GRANULE_SIZE,
            (end - start) * (size_t)EX_GRANULE_SIZE,
            handle));
        start = end;
    }
}

static void
remove_runs(
    memx_index_t *index,
    model_t *model,
    const memx_handle_t *desired,
    size_t granules) {
    size_t start = 0U;
    while (start < granules) {
        memx_handle_t handle = desired[start];
        size_t end = start + 1U;
        if (handle == MEMX_HANDLE_INVALID) {
            start += 1U;
            continue;
        }
        while (end < granules && desired[end] == handle) {
            end += 1U;
        }
        CHECK_STATUS(MEMX_OK, memx_remove(index,
            model->base + start * EX_GRANULE_SIZE,
            (end - start) * (size_t)EX_GRANULE_SIZE,
            handle));
        CHECK_STATUS(MEMX_OK, model_remove(model,
            model->base + start * EX_GRANULE_SIZE,
            (end - start) * (size_t)EX_GRANULE_SIZE,
            handle));
        start = end;
    }
}

static void
test_every_ternary_single_region_layout(void) {
    /* EMPTY, handle 0, and a large non-inline handle: 3^8 layouts. */
    const memx_handle_t alphabet[] = {
        MEMX_HANDLE_INVALID,
        0U,
        (UINTPTR_MAX >> 1U) ^ (uintptr_t)0x5a5aU
    };
    size_t encoded;
    for (encoded = 0U; encoded < 6561U; ++encoded) {
        memx_config_t config = bounded_config();
        memx_index_t *index = NULL;
        model_t model;
        memx_handle_t desired[EX_GRANULES_PER_REGION];
        size_t value = encoded;
        size_t slot;
        config.managed_size = (size_t)EX_REGION_SIZE;
        model_init(&model, EX_BOUNDED_BASE, EX_GRANULES_PER_REGION);
        CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
        for (slot = 0U; slot < EX_GRANULES_PER_REGION; ++slot) {
            desired[slot] = alphabet[value % 3U];
            value /= 3U;
        }
        populate_runs(index, &model, desired, EX_GRANULES_PER_REGION);
        compare_every_byte_sample(index, &model);
        remove_runs(index, &model, desired, EX_GRANULES_PER_REGION);
        compare_every_byte_sample(index, &model);
        memx_index_destroy(index);
    }
}

static uint64_t
next_random(uint64_t *state) {
    uint64_t value = *state;
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static void
exercise_state_machine(memx_directory_mode_t mode, uint64_t seed) {
    enum { STEPS = 40000 };
    memx_config_t config = mode == MEMX_DIRECTORY_BOUNDED
        ? bounded_config() : sparse_config();
    uintptr_t base = mode == MEMX_DIRECTORY_BOUNDED ? EX_BOUNDED_BASE : 0U;
    memx_index_t *index = NULL;
    model_t model;
    size_t step;
    model_init(&model, base, EX_TOTAL_GRANULES);
    CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
    for (step = 0U; step < STEPS; ++step) {
        size_t first = (size_t)(next_random(&seed) % EX_TOTAL_GRANULES);
        size_t maximum = EX_TOTAL_GRANULES - first;
        size_t count = 1U + (size_t)(next_random(&seed) % maximum);
        uintptr_t address = base + first * EX_GRANULE_SIZE;
        size_t bytes = count * (size_t)EX_GRANULE_SIZE;
        memx_handle_t handle = (memx_handle_t)(next_random(&seed) & 0xffffU);
        memx_status_t expected;
        memx_status_t actual;
        if ((next_random(&seed) & 1U) == 0U) {
            expected = model_insert(&model, address, bytes, handle);
            actual = memx_insert(index, address, bytes, handle);
        } else {
            if ((next_random(&seed) & 3U) != 0U) {
                handle = model.entries[first] == MEMX_HANDLE_INVALID
                    ? handle : model.entries[first];
            }
            expected = model_remove(&model, address, bytes, handle);
            actual = memx_remove(index, address, bytes, handle);
        }
        CHECK_STATUS(expected, actual);
        if ((step & 127U) == 0U) {
            compare_every_byte_sample(index, &model);
        } else {
            size_t probe = (size_t)(next_random(&seed) % EX_TOTAL_GRANULES);
            uintptr_t probe_address = base + probe * EX_GRANULE_SIZE
                + (uintptr_t)(next_random(&seed) & (EX_GRANULE_SIZE - 1U));
            CHECK_HANDLE(model_lookup(&model, probe_address),
                memx_lookup_address(index, probe_address));
        }
    }
    compare_every_byte_sample(index, &model);
    memx_index_destroy(index);
}

static void
test_interval_argument_matrix(void) {
    memx_config_t config = bounded_config();
    memx_index_t *index = NULL;
    model_t model;
    size_t first;
    size_t count;
    model_init(&model, EX_BOUNDED_BASE, EX_TOTAL_GRANULES);
    CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
    for (first = 0U; first < EX_TOTAL_GRANULES; ++first) {
        for (count = 1U; count <= EX_TOTAL_GRANULES - first; ++count) {
            uintptr_t address = EX_BOUNDED_BASE + first * EX_GRANULE_SIZE;
            size_t bytes = count * (size_t)EX_GRANULE_SIZE;
            memx_status_t expected = model_insert(&model,
                address, bytes, (memx_handle_t)(first + count));
            memx_status_t actual = memx_insert(index,
                address, bytes, (memx_handle_t)(first + count));
            CHECK_STATUS(expected, actual);
            if (expected == MEMX_OK) {
                CHECK_STATUS(MEMX_OK, model_remove(&model,
                    address, bytes, (memx_handle_t)(first + count)));
                CHECK_STATUS(MEMX_OK, memx_remove(index,
                    address, bytes, (memx_handle_t)(first + count)));
            }
            compare_every_byte_sample(index, &model);
        }
    }
    memx_index_destroy(index);
}

static void
test_representation_classification(void) {
    memx_config_t config = bounded_config();
    memx_index_t *index = NULL;
    memx_region_type_t type = MEMX_REGION_EMPTY;
    memx_stats_t stats;
    size_t slot;
    CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
    memx_index_stats(index, &stats);
    CHECK(stats.empty_regions == EX_REGIONS);
    CHECK(stats.uniform_regions == 0U);
    CHECK(stats.dense_regions == 0U);

    CHECK_STATUS(MEMX_OK, memx_insert(index, EX_BOUNDED_BASE,
        (size_t)EX_REGION_SIZE, 0U));
    CHECK_STATUS(MEMX_OK,
        memx_region_type_at(index, EX_BOUNDED_BASE, &type));
    CHECK(type == MEMX_REGION_UNIFORM);

    CHECK_STATUS(MEMX_OK, memx_remove(index,
        EX_BOUNDED_BASE + 3U * EX_GRANULE_SIZE,
        (size_t)EX_GRANULE_SIZE, 0U));
    CHECK_STATUS(MEMX_OK,
        memx_region_type_at(index, EX_BOUNDED_BASE, &type));
    CHECK(type == MEMX_REGION_DENSE);
    CHECK_STATUS(MEMX_OK, memx_insert(index,
        EX_BOUNDED_BASE + 3U * EX_GRANULE_SIZE,
        (size_t)EX_GRANULE_SIZE, 0U));
    CHECK_STATUS(MEMX_OK,
        memx_region_type_at(index, EX_BOUNDED_BASE, &type));
    CHECK(type == MEMX_REGION_UNIFORM);

    CHECK_STATUS(MEMX_OK, memx_remove(index, EX_BOUNDED_BASE,
        (size_t)EX_REGION_SIZE, 0U));
    for (slot = 0U; slot < EX_GRANULES_PER_REGION; ++slot) {
        CHECK_STATUS(MEMX_OK, memx_insert(index,
            EX_BOUNDED_BASE + slot * EX_GRANULE_SIZE,
            (size_t)EX_GRANULE_SIZE,
            (memx_handle_t)(slot + 1U)));
    }
    CHECK_STATUS(MEMX_OK,
        memx_region_type_at(index, EX_BOUNDED_BASE, &type));
    CHECK(type == MEMX_REGION_DENSE);
    memx_index_stats(index, &stats);
    CHECK(stats.empty_regions == EX_REGIONS - 1U);
    CHECK(stats.uniform_regions == 0U);
    CHECK(stats.dense_regions == 1U);
    CHECK(stats.total_bytes == stats.directory_bytes
        + stats.representation_bytes);
    memx_index_destroy(index);
}

static void
test_hot_views_match_checked_lookup(void) {
    memx_config_t config = bounded_config();
    memx_index_t *index = NULL;
    memx_bounded_view_t bounded_view;
    memx_flat_view_t flat_view;
    size_t slot;
    CHECK_STATUS(MEMX_OK, memx_index_create(&config, &index));
    for (slot = 0U; slot < EX_TOTAL_GRANULES; ++slot) {
        CHECK_STATUS(MEMX_OK, memx_insert(index,
            EX_BOUNDED_BASE + slot * EX_GRANULE_SIZE,
            (size_t)EX_GRANULE_SIZE,
            (memx_handle_t)(100U + slot)));
    }
    CHECK_STATUS(MEMX_OK, memx_index_bounded_view(index, &bounded_view));
    for (slot = 0U; slot < EX_TOTAL_GRANULES; ++slot) {
        uintptr_t address = EX_BOUNDED_BASE + slot * EX_GRANULE_SIZE + 31U;
        CHECK_HANDLE(memx_lookup_address(index, address),
            memx_bounded_lookup_assume_mapped(&bounded_view, address));
    }
    CHECK_STATUS(MEMX_OK, memx_index_optimize(index));
    CHECK_STATUS(MEMX_OK, memx_index_flat_view(index, &flat_view));
    CHECK(flat_view.entry_count == EX_TOTAL_GRANULES);
    for (slot = 0U; slot < EX_TOTAL_GRANULES; ++slot) {
        uintptr_t address = EX_BOUNDED_BASE + slot * EX_GRANULE_SIZE + 73U;
        CHECK_HANDLE(memx_lookup_address(index, address),
            memx_flat_lookup_assume_mapped(&flat_view, address));
    }
    memx_index_destroy(index);
}

int
main(void) {
    test_every_ternary_single_region_layout();
    test_interval_argument_matrix();
    test_representation_classification();
    test_hot_views_match_checked_lookup();
    exercise_state_machine(MEMX_DIRECTORY_BOUNDED,
        UINT64_C(0x58b01d5eed));
    exercise_state_machine(MEMX_DIRECTORY_SPARSE,
        UINT64_C(0x58a5e5eed));
    if (test_state.failures != 0U) {
        fprintf(stderr, "%zu of %zu exhaustive checks failed\n",
            test_state.failures, test_state.checks);
        return EXIT_FAILURE;
    }
    printf("all %zu exhaustive checks passed\n", test_state.checks);
    return EXIT_SUCCESS;
}
