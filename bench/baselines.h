#ifndef MEMX_BENCH_BASELINES_H
#define MEMX_BENCH_BASELINES_H

#include "memx/memx.h"

#include <stddef.h>
#include <stdint.h>

typedef struct baseline_flat baseline_flat_t;
typedef struct baseline_two_level baseline_two_level_t;
typedef struct baseline_radix baseline_radix_t;

typedef struct baseline_config {
    uintptr_t base;
    size_t managed_size;
    unsigned granule_shift;
} baseline_config_t;

typedef struct baseline_radix_cache_entry {
    size_t leaf_key;
    const memx_handle_t *leaf;
} baseline_radix_cache_entry_t;

typedef struct baseline_radix_cache {
    baseline_radix_cache_entry_t entries[16];
} baseline_radix_cache_t;

baseline_flat_t *baseline_flat_create(const baseline_config_t *config);
void baseline_flat_destroy(baseline_flat_t *map);
int baseline_flat_set(baseline_flat_t *map, uintptr_t address, memx_handle_t value);
memx_handle_t baseline_flat_lookup(const baseline_flat_t *map, uintptr_t address);
/* Caller has already proved address is within the configured flat range. */
memx_handle_t baseline_flat_lookup_assume_mapped(
    const baseline_flat_t *map,
    uintptr_t address);
size_t baseline_flat_bytes(const baseline_flat_t *map);
size_t baseline_flat_page_accounted_bytes(const baseline_flat_t *map);

baseline_two_level_t *baseline_two_level_create(const baseline_config_t *config);
void baseline_two_level_destroy(baseline_two_level_t *map);
int baseline_two_level_set(
    baseline_two_level_t *map, uintptr_t address, memx_handle_t value);
memx_handle_t baseline_two_level_lookup(
    const baseline_two_level_t *map, uintptr_t address);
size_t baseline_two_level_bytes(const baseline_two_level_t *map);
size_t baseline_two_level_page_accounted_bytes(
    const baseline_two_level_t *map);

baseline_radix_t *baseline_radix_create(const baseline_config_t *config);
void baseline_radix_destroy(baseline_radix_t *map);
int baseline_radix_set(
    baseline_radix_t *map, uintptr_t address, memx_handle_t value);
memx_handle_t baseline_radix_lookup(
    const baseline_radix_t *map, uintptr_t address);
size_t baseline_radix_bytes(const baseline_radix_t *map);
size_t baseline_radix_page_accounted_bytes(const baseline_radix_t *map);

void baseline_radix_cache_init(baseline_radix_cache_t *cache);
memx_handle_t baseline_radix_cached_lookup(
    const baseline_radix_t *map,
    baseline_radix_cache_t *cache,
    uintptr_t address);

#endif
