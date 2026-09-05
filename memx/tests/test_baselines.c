#include "baselines.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        checks += 1U;                                                          \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: failed: %s\n",                           \
                __FILE__, __LINE__, #condition);                               \
            failures += 1U;                                                    \
        }                                                                      \
    } while (0)

typedef struct maps {
    baseline_flat_t *flat;
    baseline_two_level_t *two;
    baseline_radix_t *radix;
    baseline_radix_cache_t cache;
    baseline_config_t config;
} maps_t;

static maps_t
maps_create(uintptr_t base, size_t bytes, unsigned shift) {
    maps_t maps;
    maps.config.base = base;
    maps.config.managed_size = bytes;
    maps.config.granule_shift = shift;
    maps.flat = baseline_flat_create(&maps.config);
    maps.two = baseline_two_level_create(&maps.config);
    maps.radix = baseline_radix_create(&maps.config);
    baseline_radix_cache_init(&maps.cache);
    CHECK(maps.flat != NULL);
    CHECK(maps.two != NULL);
    CHECK(maps.radix != NULL);
    return maps;
}

static void
maps_destroy(maps_t *maps) {
    baseline_flat_destroy(maps->flat);
    baseline_two_level_destroy(maps->two);
    baseline_radix_destroy(maps->radix);
    maps->flat = NULL;
    maps->two = NULL;
    maps->radix = NULL;
}

static void
maps_expect(const maps_t *maps, uintptr_t address, memx_handle_t expected) {
    baseline_radix_cache_t *cache =
        (baseline_radix_cache_t *)(uintptr_t)&maps->cache;
    memx_handle_t flat = baseline_flat_lookup(maps->flat, address);
    memx_handle_t two = baseline_two_level_lookup(maps->two, address);
    memx_handle_t radix = baseline_radix_lookup(maps->radix, address);
    memx_handle_t cached = baseline_radix_cached_lookup(
        maps->radix, cache, address);
    CHECK(flat == expected);
    CHECK(two == expected);
    CHECK(radix == expected);
    CHECK(cached == expected);
}

static void
maps_set(maps_t *maps, uintptr_t address, memx_handle_t value) {
    CHECK(baseline_flat_set(maps->flat, address, value));
    CHECK(baseline_two_level_set(maps->two, address, value));
    CHECK(baseline_radix_set(maps->radix, address, value));
}

static void
test_invalid_configuration(void) {
    baseline_config_t config;
    config.base = 0U;
    config.managed_size = 0U;
    config.granule_shift = 12U;
    CHECK(baseline_flat_create(&config) == NULL);
    CHECK(baseline_two_level_create(&config) == NULL);
    CHECK(baseline_radix_create(&config) == NULL);

    config.managed_size = 4096U;
    config.base = 1U;
    CHECK(baseline_flat_create(&config) == NULL);
    CHECK(baseline_two_level_create(&config) == NULL);
    CHECK(baseline_radix_create(&config) == NULL);

    config.base = 0U;
    config.managed_size = 4095U;
    CHECK(baseline_flat_create(&config) == NULL);
    CHECK(baseline_two_level_create(&config) == NULL);
    CHECK(baseline_radix_create(&config) == NULL);

    config.managed_size = 4096U;
    config.granule_shift = (unsigned)(sizeof(uintptr_t) * 8U);
    CHECK(baseline_flat_create(&config) == NULL);
    CHECK(baseline_two_level_create(&config) == NULL);
    CHECK(baseline_radix_create(&config) == NULL);
}

static void
test_empty_and_bounds(void) {
    const uintptr_t base = 0x4000000U;
    const size_t bytes = 1U << 20U;
    maps_t maps = maps_create(base, bytes, 12U);
    maps_expect(&maps, base, MEMX_HANDLE_INVALID);
    maps_expect(&maps, base + bytes - 1U, MEMX_HANDLE_INVALID);
    maps_expect(&maps, base - 1U, MEMX_HANDLE_INVALID);
    maps_expect(&maps, base + bytes, MEMX_HANDLE_INVALID);
    CHECK(!baseline_flat_set(maps.flat, base - 1U, 1U));
    CHECK(!baseline_two_level_set(maps.two, base - 1U, 1U));
    CHECK(!baseline_radix_set(maps.radix, base - 1U, 1U));
    CHECK(!baseline_flat_set(maps.flat, base + bytes, 1U));
    CHECK(!baseline_two_level_set(maps.two, base + bytes, 1U));
    CHECK(!baseline_radix_set(maps.radix, base + bytes, 1U));
    maps_destroy(&maps);
}

static void
test_same_granule(void) {
    const uintptr_t base = 0x5000000U;
    maps_t maps = maps_create(base, 16U * 4096U, 12U);
    maps_set(&maps, base + 3U * 4096U, 0U);
    maps_expect(&maps, base + 3U * 4096U, 0U);
    maps_expect(&maps, base + 3U * 4096U + 2048U, 0U);
    maps_expect(&maps, base + 4U * 4096U, MEMX_HANDLE_INVALID);
    maps_set(&maps, base + 3U * 4096U + 17U, UINTPTR_MAX - 1U);
    maps_expect(&maps, base + 3U * 4096U, UINTPTR_MAX - 1U);
    maps_destroy(&maps);
}

static uint64_t
rng_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void
test_randomized_equivalence(void) {
    enum { ENTRY_COUNT = 16384, OPERATIONS = 100000 };
    const uintptr_t base = 0x8000000U;
    const unsigned shift = 10U;
    const uintptr_t granule = (uintptr_t)1U << shift;
    maps_t maps = maps_create(base, ENTRY_COUNT * (size_t)granule, shift);
    memx_handle_t *oracle = malloc(ENTRY_COUNT * sizeof(*oracle));
    uint64_t state = UINT64_C(0x626173656c696e65);
    size_t i;
    CHECK(oracle != NULL);
    if (oracle == NULL) {
        maps_destroy(&maps);
        return;
    }
    for (i = 0U; i < ENTRY_COUNT; ++i) {
        oracle[i] = MEMX_HANDLE_INVALID;
    }
    for (i = 0U; i < OPERATIONS; ++i) {
        size_t index = (size_t)(rng_next(&state) % ENTRY_COUNT);
        uintptr_t address = base + (uintptr_t)index * granule
            + (uintptr_t)(rng_next(&state) & (granule - 1U));
        if ((rng_next(&state) & 3U) == 0U) {
            memx_handle_t value = (memx_handle_t)rng_next(&state);
            if (value == MEMX_HANDLE_INVALID) {
                value -= 1U;
            }
            maps_set(&maps, address, value);
            oracle[index] = value;
        }
        maps_expect(&maps, address, oracle[index]);
    }
    for (i = 0U; i < ENTRY_COUNT; ++i) {
        maps_expect(&maps, base + (uintptr_t)i * granule, oracle[i]);
    }
    CHECK(baseline_flat_bytes(maps.flat) > 0U);
    CHECK(baseline_two_level_bytes(maps.two) > 0U);
    CHECK(baseline_radix_bytes(maps.radix) > 0U);
    CHECK(baseline_flat_page_accounted_bytes(maps.flat)
        >= baseline_flat_bytes(maps.flat));
    CHECK(baseline_two_level_page_accounted_bytes(maps.two)
        >= baseline_two_level_bytes(maps.two));
    CHECK(baseline_radix_page_accounted_bytes(maps.radix)
        >= baseline_radix_bytes(maps.radix));
    free(oracle);
    maps_destroy(&maps);
}

static void
test_cache_collisions_and_updates(void) {
    const uintptr_t base = 0x10000000U;
    const unsigned shift = 12U;
    const uintptr_t granule = (uintptr_t)1U << shift;
    maps_t maps = maps_create(base, 65536U * (size_t)granule, shift);
    size_t i;

    for (i = 0U; i < 65536U; i += 257U) {
        maps_set(&maps, base + (uintptr_t)i * granule,
            (memx_handle_t)(i + 9U));
    }
    for (i = 0U; i < 65536U; i += 257U) {
        maps_expect(&maps, base + (uintptr_t)i * granule,
            (memx_handle_t)(i + 9U));
    }
    for (i = 0U; i < 65536U; i += 257U) {
        memx_handle_t replacement = (memx_handle_t)(i + 100000U);
        maps_set(&maps, base + (uintptr_t)i * granule, replacement);
        maps_expect(&maps, base + (uintptr_t)i * granule, replacement);
    }
    maps_destroy(&maps);
}

static void
test_small_entry_counts(void) {
    unsigned shift;
    for (shift = 4U; shift <= 12U; ++shift) {
        uintptr_t granule = (uintptr_t)1U << shift;
        size_t count;
        for (count = 1U; count <= 9U; ++count) {
            maps_t maps = maps_create(0U, count * (size_t)granule, shift);
            size_t i;
            for (i = 0U; i < count; ++i) {
                maps_set(&maps, (uintptr_t)i * granule,
                    (memx_handle_t)(i * 3U));
            }
            for (i = 0U; i < count; ++i) {
                maps_expect(&maps, (uintptr_t)i * granule,
                    (memx_handle_t)(i * 3U));
            }
            maps_destroy(&maps);
        }
    }
}

int
main(void) {
    test_invalid_configuration();
    test_empty_and_bounds();
    test_same_granule();
    test_randomized_equivalence();
    test_cache_collisions_and_updates();
    test_small_entry_counts();

    if (failures != 0U) {
        fprintf(stderr, "%u of %u baseline checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }
    printf("all %u baseline checks passed\n", checks);
    return EXIT_SUCCESS;
}
