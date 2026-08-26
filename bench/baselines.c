#include "baselines.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <unistd.h>
#endif

struct baseline_flat {
    baseline_config_t config;
    size_t entry_count;
    memx_handle_t entries[];
};

struct baseline_two_level {
    baseline_config_t config;
    size_t entry_count;
    unsigned leaf_bits;
    size_t root_count;
    size_t leaf_count;
    size_t allocated_leaves;
    memx_handle_t **root;
};

typedef struct baseline_radix_middle {
    memx_handle_t **leaves;
} baseline_radix_middle_t;

struct baseline_radix {
    baseline_config_t config;
    size_t entry_count;
    unsigned leaf_bits;
    unsigned middle_bits;
    unsigned root_bits;
    size_t leaf_count;
    size_t middle_count;
    size_t root_count;
    size_t allocated_middles;
    size_t allocated_leaves;
    baseline_radix_middle_t **root;
};

static size_t
baseline_page_size(void) {
#if defined(__linux__)
    const long result = sysconf(_SC_PAGESIZE);
    if (result > 0
        && (((size_t)result & ((size_t)result - 1U)) == 0U)) {
        return (size_t)result;
    }
#endif
    return 4096U;
}

static size_t
baseline_page_account(size_t bytes) {
    const size_t page_size = baseline_page_size();
    if (bytes == 0U || bytes > SIZE_MAX - (page_size - 1U)) {
        return bytes == 0U ? 0U : SIZE_MAX;
    }
    return (bytes + page_size - 1U) & ~(page_size - 1U);
}

static size_t
baseline_saturating_add(size_t left, size_t right) {
    return left > SIZE_MAX - right ? SIZE_MAX : left + right;
}

static size_t
baseline_saturating_multiply(size_t left, size_t right) {
    return left != 0U && right > SIZE_MAX / left
        ? SIZE_MAX : left * right;
}

static int
baseline_config_valid(const baseline_config_t *config, size_t *entry_count) {
    const unsigned word_bits = (unsigned)(sizeof(uintptr_t) * CHAR_BIT);
    uintptr_t granule_size;
    if (config == NULL || config->managed_size == 0U
        || config->granule_shift >= word_bits) {
        return 0;
    }
    granule_size = ((uintptr_t)1U) << config->granule_shift;
    if ((config->base & (granule_size - 1U)) != 0U
        || (config->managed_size & (size_t)(granule_size - 1U)) != 0U
        || config->managed_size > (size_t)(UINTPTR_MAX - config->base)) {
        return 0;
    }
    *entry_count = config->managed_size >> config->granule_shift;
    return *entry_count != 0U;
}

static int
baseline_index(
    const baseline_config_t *config,
    size_t entry_count,
    uintptr_t address,
    size_t *out_index) {
    uintptr_t relative;
    if (address < config->base) {
        return 0;
    }
    relative = address - config->base;
    if (relative >= (uintptr_t)config->managed_size) {
        return 0;
    }
    *out_index = (size_t)(relative >> config->granule_shift);
    return *out_index < entry_count;
}

static void
baseline_fill_invalid(memx_handle_t *entries, size_t count) {
    size_t i;
    for (i = 0U; i < count; ++i) {
        entries[i] = MEMX_HANDLE_INVALID;
    }
}

baseline_flat_t *
baseline_flat_create(const baseline_config_t *config) {
    baseline_flat_t *map;
    size_t count;
    size_t bytes;
    if (!baseline_config_valid(config, &count)
        || count > (SIZE_MAX - sizeof(*map)) / sizeof(memx_handle_t)) {
        return NULL;
    }
    bytes = sizeof(*map) + count * sizeof(memx_handle_t);
    map = malloc(bytes);
    if (map == NULL) {
        return NULL;
    }
    map->config = *config;
    map->entry_count = count;
    baseline_fill_invalid(map->entries, count);
    return map;
}

void
baseline_flat_destroy(baseline_flat_t *map) {
    free(map);
}

int
baseline_flat_set(
    baseline_flat_t *map, uintptr_t address, memx_handle_t value) {
    size_t index;
    if (map == NULL
        || !baseline_index(&map->config, map->entry_count, address, &index)) {
        return 0;
    }
    map->entries[index] = value;
    return 1;
}

memx_handle_t
baseline_flat_lookup(const baseline_flat_t *map, uintptr_t address) {
    size_t index;
    if (map == NULL
        || !baseline_index(&map->config, map->entry_count, address, &index)) {
        return MEMX_HANDLE_INVALID;
    }
    return map->entries[index];
}

memx_handle_t
baseline_flat_lookup_assume_mapped(
    const baseline_flat_t *map,
    uintptr_t address) {
    const size_t index = (size_t)
        ((address - map->config.base) >> map->config.granule_shift);
    return map->entries[index];
}

size_t
baseline_flat_bytes(const baseline_flat_t *map) {
    return map == NULL ? 0U
        : sizeof(*map) + map->entry_count * sizeof(memx_handle_t);
}

size_t
baseline_flat_page_accounted_bytes(const baseline_flat_t *map) {
    return map == NULL ? 0U
        : baseline_page_account(baseline_flat_bytes(map));
}

baseline_two_level_t *
baseline_two_level_create(const baseline_config_t *config) {
    baseline_two_level_t *map;
    size_t count;
    unsigned entry_bits = 0U;
    size_t value;
    if (!baseline_config_valid(config, &count)) {
        return NULL;
    }
    value = count - 1U;
    while (value != 0U) {
        entry_bits += 1U;
        value >>= 1U;
    }
    map = calloc(1U, sizeof(*map));
    if (map == NULL) {
        return NULL;
    }
    map->config = *config;
    map->entry_count = count;
    map->leaf_bits = entry_bits / 2U;
    map->leaf_count = (size_t)1U << map->leaf_bits;
    map->root_count = (count + map->leaf_count - 1U) / map->leaf_count;
    map->root = calloc(map->root_count, sizeof(*map->root));
    if (map->root == NULL) {
        free(map);
        return NULL;
    }
    return map;
}

void
baseline_two_level_destroy(baseline_two_level_t *map) {
    size_t i;
    if (map == NULL) {
        return;
    }
    for (i = 0U; i < map->root_count; ++i) {
        free(map->root[i]);
    }
    free(map->root);
    free(map);
}

int
baseline_two_level_set(
    baseline_two_level_t *map, uintptr_t address, memx_handle_t value) {
    size_t index;
    size_t root_index;
    size_t leaf_index;
    if (map == NULL
        || !baseline_index(&map->config, map->entry_count, address, &index)) {
        return 0;
    }
    root_index = index >> map->leaf_bits;
    leaf_index = index & (map->leaf_count - 1U);
    if (map->root[root_index] == NULL) {
        map->root[root_index] = malloc(
            map->leaf_count * sizeof(memx_handle_t));
        if (map->root[root_index] == NULL) {
            return 0;
        }
        baseline_fill_invalid(map->root[root_index], map->leaf_count);
        map->allocated_leaves += 1U;
    }
    map->root[root_index][leaf_index] = value;
    return 1;
}

memx_handle_t
baseline_two_level_lookup(
    const baseline_two_level_t *map, uintptr_t address) {
    size_t index;
    size_t root_index;
    size_t leaf_index;
    if (map == NULL
        || !baseline_index(&map->config, map->entry_count, address, &index)) {
        return MEMX_HANDLE_INVALID;
    }
    root_index = index >> map->leaf_bits;
    leaf_index = index & (map->leaf_count - 1U);
    return map->root[root_index] == NULL ? MEMX_HANDLE_INVALID
        : map->root[root_index][leaf_index];
}

size_t
baseline_two_level_bytes(const baseline_two_level_t *map) {
    if (map == NULL) {
        return 0U;
    }
    return sizeof(*map) + map->root_count * sizeof(*map->root)
        + map->allocated_leaves * map->leaf_count * sizeof(memx_handle_t);
}

size_t
baseline_two_level_page_accounted_bytes(const baseline_two_level_t *map) {
    size_t total;
    if (map == NULL) {
        return 0U;
    }
    total = baseline_saturating_add(
        baseline_page_account(sizeof(*map)),
        baseline_page_account(baseline_saturating_multiply(
            map->root_count, sizeof(*map->root))));
    return baseline_saturating_add(total, baseline_saturating_multiply(
        map->allocated_leaves,
        baseline_page_account(baseline_saturating_multiply(
            map->leaf_count, sizeof(memx_handle_t)))));
}

baseline_radix_t *
baseline_radix_create(const baseline_config_t *config) {
    baseline_radix_t *map;
    size_t count;
    unsigned bits = 0U;
    size_t value;
    if (!baseline_config_valid(config, &count)) {
        return NULL;
    }
    value = count - 1U;
    while (value != 0U) {
        bits += 1U;
        value >>= 1U;
    }
    map = calloc(1U, sizeof(*map));
    if (map == NULL) {
        return NULL;
    }
    map->config = *config;
    map->entry_count = count;
    map->leaf_bits = bits / 3U;
    map->middle_bits = bits / 3U;
    map->root_bits = bits - map->leaf_bits - map->middle_bits;
    map->leaf_count = (size_t)1U << map->leaf_bits;
    map->middle_count = (size_t)1U << map->middle_bits;
    map->root_count = (size_t)1U << map->root_bits;
    map->root = calloc(map->root_count, sizeof(*map->root));
    if (map->root == NULL) {
        free(map);
        return NULL;
    }
    return map;
}

void
baseline_radix_destroy(baseline_radix_t *map) {
    size_t root_index;
    size_t middle_index;
    if (map == NULL) {
        return;
    }
    for (root_index = 0U; root_index < map->root_count; ++root_index) {
        baseline_radix_middle_t *middle = map->root[root_index];
        if (middle == NULL) {
            continue;
        }
        for (middle_index = 0U;
             middle_index < map->middle_count;
             ++middle_index) {
            free(middle->leaves[middle_index]);
        }
        free(middle->leaves);
        free(middle);
    }
    free(map->root);
    free(map);
}

static int
baseline_radix_indices(
    const baseline_radix_t *map,
    uintptr_t address,
    size_t *root_index,
    size_t *middle_index,
    size_t *leaf_index,
    size_t *flat_index) {
    size_t index;
    if (!baseline_index(&map->config, map->entry_count, address, &index)) {
        return 0;
    }
    *leaf_index = index & (map->leaf_count - 1U);
    *middle_index = (index >> map->leaf_bits) & (map->middle_count - 1U);
    *root_index = index >> (map->leaf_bits + map->middle_bits);
    if (flat_index != NULL) {
        *flat_index = index;
    }
    return *root_index < map->root_count;
}

int
baseline_radix_set(
    baseline_radix_t *map, uintptr_t address, memx_handle_t value) {
    size_t root_index;
    size_t middle_index;
    size_t leaf_index;
    baseline_radix_middle_t *middle;
    if (map == NULL || !baseline_radix_indices(
            map, address, &root_index, &middle_index, &leaf_index, NULL)) {
        return 0;
    }
    middle = map->root[root_index];
    if (middle == NULL) {
        middle = calloc(1U, sizeof(*middle));
        if (middle == NULL) {
            return 0;
        }
        middle->leaves = calloc(map->middle_count, sizeof(*middle->leaves));
        if (middle->leaves == NULL) {
            free(middle);
            return 0;
        }
        map->root[root_index] = middle;
        map->allocated_middles += 1U;
    }
    if (middle->leaves[middle_index] == NULL) {
        middle->leaves[middle_index] = malloc(
            map->leaf_count * sizeof(memx_handle_t));
        if (middle->leaves[middle_index] == NULL) {
            return 0;
        }
        baseline_fill_invalid(middle->leaves[middle_index], map->leaf_count);
        map->allocated_leaves += 1U;
    }
    middle->leaves[middle_index][leaf_index] = value;
    return 1;
}

memx_handle_t
baseline_radix_lookup(const baseline_radix_t *map, uintptr_t address) {
    size_t root_index;
    size_t middle_index;
    size_t leaf_index;
    const baseline_radix_middle_t *middle;
    if (map == NULL || !baseline_radix_indices(
            map, address, &root_index, &middle_index, &leaf_index, NULL)) {
        return MEMX_HANDLE_INVALID;
    }
    middle = map->root[root_index];
    if (middle == NULL || middle->leaves[middle_index] == NULL) {
        return MEMX_HANDLE_INVALID;
    }
    return middle->leaves[middle_index][leaf_index];
}

size_t
baseline_radix_bytes(const baseline_radix_t *map) {
    if (map == NULL) {
        return 0U;
    }
    return sizeof(*map) + map->root_count * sizeof(*map->root)
        + map->allocated_middles
            * (sizeof(baseline_radix_middle_t)
               + map->middle_count * sizeof(memx_handle_t *))
        + map->allocated_leaves * map->leaf_count * sizeof(memx_handle_t);
}

size_t
baseline_radix_page_accounted_bytes(const baseline_radix_t *map) {
    size_t total;
    size_t middle_bytes;
    if (map == NULL) {
        return 0U;
    }
    total = baseline_saturating_add(
        baseline_page_account(sizeof(*map)),
        baseline_page_account(baseline_saturating_multiply(
            map->root_count, sizeof(*map->root))));
    middle_bytes = baseline_saturating_add(
        baseline_page_account(sizeof(baseline_radix_middle_t)),
        baseline_page_account(baseline_saturating_multiply(
            map->middle_count, sizeof(memx_handle_t *))));
    total = baseline_saturating_add(total, baseline_saturating_multiply(
        map->allocated_middles, middle_bytes));
    return baseline_saturating_add(total, baseline_saturating_multiply(
        map->allocated_leaves,
        baseline_page_account(baseline_saturating_multiply(
            map->leaf_count, sizeof(memx_handle_t)))));
}

void
baseline_radix_cache_init(baseline_radix_cache_t *cache) {
    size_t i;
    if (cache == NULL) {
        return;
    }
    for (i = 0U; i < 16U; ++i) {
        cache->entries[i].leaf_key = SIZE_MAX;
        cache->entries[i].leaf = NULL;
    }
}

memx_handle_t
baseline_radix_cached_lookup(
    const baseline_radix_t *map,
    baseline_radix_cache_t *cache,
    uintptr_t address) {
    size_t root_index;
    size_t middle_index;
    size_t leaf_index;
    size_t flat_index;
    size_t leaf_key;
    size_t slot;
    const baseline_radix_middle_t *middle;
    const memx_handle_t *leaf;

    if (map == NULL || cache == NULL || !baseline_radix_indices(
            map, address, &root_index, &middle_index, &leaf_index, &flat_index)) {
        return MEMX_HANDLE_INVALID;
    }
    leaf_key = flat_index >> map->leaf_bits;
    slot = leaf_key & 15U;
    if (cache->entries[slot].leaf_key == leaf_key
        && cache->entries[slot].leaf != NULL) {
        leaf = cache->entries[slot].leaf;
        return leaf[leaf_index];
    }
    middle = map->root[root_index];
    leaf = middle == NULL ? NULL : middle->leaves[middle_index];
    if (leaf != NULL) {
        cache->entries[slot].leaf_key = leaf_key;
        cache->entries[slot].leaf = leaf;
    }
    return leaf == NULL ? MEMX_HANDLE_INVALID : leaf[leaf_index];
}
