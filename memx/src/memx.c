#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "memx/memx.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef struct memx_dense_region {
    memx_handle_t entries[1];
} memx_dense_region_t;

/*
 * Portable one-word region descriptor.  EMPTY is zero, small enough uniform
 * handles are shifted inline, and aligned dense pointers carry a low-bit tag.
 * A uniform handle that cannot be shifted losslessly uses a dense table, so
 * the public handle domain is not reduced beyond MEMX_HANDLE_INVALID.
 */
typedef uintptr_t memx_region_t;

#define MEMX_DESCRIPTOR_TAG_MASK ((uintptr_t)3U)
#define MEMX_DESCRIPTOR_UNIFORM_TAG ((uintptr_t)1U)
#define MEMX_DESCRIPTOR_DENSE_TAG ((uintptr_t)2U)
#define MEMX_INLINE_HANDLE_MAX (UINTPTR_MAX >> 2U)
#define MEMX_INTERNAL_ALIGNMENT                                             \
    ((_Alignof(uintptr_t) > 4U) ? _Alignof(uintptr_t) : 4U)

typedef struct memx_sparse_leaf {
    memx_region_t regions[1];
} memx_sparse_leaf_t;

struct memx_index {
    memx_config_t config;
    size_t region_count;
    size_t granules_per_region;
    size_t dense_bytes;

    unsigned sparse_region_bits;
    unsigned sparse_leaf_bits;
    size_t sparse_root_count;
    size_t sparse_leaf_count;

    union {
        memx_region_t *bounded;
        memx_sparse_leaf_t **sparse_root;
    } directory;

    size_t allocated_sparse_leaves;
    size_t dense_regions;
    size_t uniform_regions;
    bool flat_mode;
    bool overlay_mode;
    memx_handle_t *flat_entries;
    memx_handle_t *overlay_entries;
    void *overlay_mapping;
    size_t overlay_bytes;
    size_t overlay_private_bytes;
};

_Static_assert(_Alignof(memx_dense_region_t) <= MEMX_INTERNAL_ALIGNMENT,
    "dense regions must satisfy the allocator alignment contract");
_Static_assert(_Alignof(memx_sparse_leaf_t) <= MEMX_INTERNAL_ALIGNMENT,
    "sparse leaves must satisfy the allocator alignment contract");
_Static_assert(_Alignof(memx_index_t) <= MEMX_INTERNAL_ALIGNMENT,
    "indexes must satisfy the allocator alignment contract");

static void *
memx_default_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void
memx_default_deallocate(void *context, void *pointer) {
    (void)context;
    free(pointer);
}

static void *
memx_allocate(const memx_index_t *index, size_t size) {
    void *pointer = index->config.allocator.allocate(
        index->config.allocator.context, size);
    if (pointer != NULL
        && ((uintptr_t)pointer % (uintptr_t)MEMX_INTERNAL_ALIGNMENT) != 0U) {
        index->config.allocator.deallocate(
            index->config.allocator.context, pointer);
        return NULL;
    }
    return pointer;
}

static void
memx_deallocate(const memx_index_t *index, void *pointer) {
    index->config.allocator.deallocate(
        index->config.allocator.context, pointer);
}

static bool
memx_overlay_platform_supported(void) {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

static memx_handle_t *
memx_overlay_allocate(
    size_t bytes,
    size_t *out_reserved_bytes,
    size_t *out_page_size,
    void **out_mapping) {
#if defined(__linux__)
    const long page_result = sysconf(_SC_PAGESIZE);
    size_t page_size;
    size_t rounded;
    unsigned char *mapping;
    if (page_result <= 0) {
        return NULL;
    }
    page_size = (size_t)page_result;
    if ((page_size & (page_size - 1U)) != 0U
        || bytes > SIZE_MAX - (page_size - 1U)) {
        return NULL;
    }
    rounded = (bytes + page_size - 1U) & ~(page_size - 1U);
    if (page_size > SIZE_MAX / 2U
        || rounded > SIZE_MAX - page_size * 2U) {
        return NULL;
    }
    *out_reserved_bytes = rounded + page_size * 2U;
    mapping = mmap(NULL, *out_reserved_bytes, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    if (mprotect(mapping + page_size, rounded,
            PROT_READ | PROT_WRITE) != 0) {
        (void)munmap(mapping, *out_reserved_bytes);
        return NULL;
    }
    *out_page_size = page_size;
    *out_mapping = mapping;
    return (memx_handle_t *)(void *)(mapping + page_size);
#else
    (void)bytes;
    (void)out_reserved_bytes;
    (void)out_page_size;
    (void)out_mapping;
    return NULL;
#endif
}

static void
memx_overlay_deallocate(void *mapping, size_t bytes) {
#if defined(__linux__)
    if (mapping != NULL) {
        (void)munmap(mapping, bytes);
    }
#else
    (void)mapping;
    (void)bytes;
#endif
}

static bool
memx_is_aligned(uintptr_t value, unsigned shift) {
    const uintptr_t mask = (((uintptr_t)1U) << shift) - 1U;
    return (value & mask) == 0U;
}

static bool
memx_add_overflows(uintptr_t base, size_t size, uintptr_t *out_end) {
    if ((uintmax_t)size > (uintmax_t)(UINTPTR_MAX - base)) {
        return true;
    }
    *out_end = base + (uintptr_t)size;
    return false;
}

static bool
memx_shift_valid(unsigned shift) {
    const unsigned address_bits =
        (unsigned)(sizeof(uintptr_t) * CHAR_BIT);
    const unsigned index_bits =
        (unsigned)(sizeof(size_t) * CHAR_BIT);
    const unsigned usable_bits = address_bits < index_bits
        ? address_bits : index_bits;
    return shift < usable_bits;
}

static size_t
memx_host_page_size(void) {
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
memx_page_account(size_t bytes, size_t page_size) {
    if (bytes == 0U) {
        return 0U;
    }
    if (bytes > SIZE_MAX - (page_size - 1U)) {
        return SIZE_MAX;
    }
    return (bytes + page_size - 1U) & ~(page_size - 1U);
}

memx_config_t
memx_config_default(void) {
    memx_config_t config;
    memset(&config, 0, sizeof(config));
    config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    config.policy = MEMX_POLICY_BALANCED;
    config.hint_mode = MEMX_HINT_VALIDATED;
    config.region_shift = 21U;
    config.granule_shift = 12U;
    config.address_bits = (unsigned)(sizeof(uintptr_t) * CHAR_BIT);
    return config;
}

static memx_status_t
memx_validate_config(memx_config_t *config) {
    const unsigned word_bits = (unsigned)(sizeof(uintptr_t) * CHAR_BIT);

    if (!memx_shift_valid(config->region_shift)
        || !memx_shift_valid(config->granule_shift)
        || config->region_shift < config->granule_shift) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (config->policy > MEMX_POLICY_COMPACT
        || config->hint_mode > MEMX_HINT_TRUSTED) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if ((config->allocator.allocate == NULL)
        != (config->allocator.deallocate == NULL)) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (config->allocator.allocate == NULL) {
        config->allocator.allocate = memx_default_allocate;
        config->allocator.deallocate = memx_default_deallocate;
    }

    if (config->directory_mode == MEMX_DIRECTORY_BOUNDED) {
        const uintptr_t region_size = ((uintptr_t)1U) << config->region_shift;
        if (config->managed_size == 0U
            || !memx_is_aligned(config->base_address, config->region_shift)
            || (config->managed_size & (size_t)(region_size - 1U)) != 0U) {
            return MEMX_ERROR_INVALID_ARGUMENT;
        }
        if ((uintmax_t)config->managed_size
            > (uintmax_t)(UINTPTR_MAX - config->base_address)) {
            return MEMX_ERROR_INVALID_ARGUMENT;
        }
    } else if (config->directory_mode == MEMX_DIRECTORY_SPARSE) {
        if (config->base_address != 0U || config->address_bits == 0U
            || config->address_bits > word_bits
            || config->address_bits <= config->region_shift) {
            return MEMX_ERROR_INVALID_ARGUMENT;
        }
        /* A two-level direct directory must stay reasonably sized in v0. */
        if ((config->address_bits - config->region_shift) > 32U) {
            return MEMX_ERROR_UNSUPPORTED;
        }
    } else {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    return MEMX_OK;
}

static void
memx_region_init(memx_region_t *region) {
    *region = 0U;
}

static memx_region_type_t
memx_region_type_decode(memx_region_t region) {
    const uintptr_t tag = region & MEMX_DESCRIPTOR_TAG_MASK;
    if (region == 0U) {
        return MEMX_REGION_EMPTY;
    }
    if (tag == MEMX_DESCRIPTOR_UNIFORM_TAG) {
        return MEMX_REGION_UNIFORM;
    }
    if (tag == MEMX_DESCRIPTOR_DENSE_TAG) {
        return MEMX_REGION_DENSE;
    }
    return MEMX_REGION_EMPTY;
}

static memx_handle_t
memx_uniform_decode(memx_region_t region) {
    return (memx_handle_t)(region >> 2U);
}

static memx_region_t
memx_uniform_encode(memx_handle_t handle) {
    return ((memx_region_t)handle << 2U) | MEMX_DESCRIPTOR_UNIFORM_TAG;
}

static memx_dense_region_t *
memx_dense_decode(memx_region_t region) {
    return (memx_dense_region_t *)(region & ~MEMX_DESCRIPTOR_TAG_MASK);
}

static memx_region_t
memx_dense_encode(memx_dense_region_t *dense) {
    return ((memx_region_t)dense) | MEMX_DESCRIPTOR_DENSE_TAG;
}

memx_status_t
memx_index_create(const memx_config_t *input, memx_index_t **out_index) {
    memx_config_t config;
    memx_index_t temporary;
    memx_index_t *index;
    memx_status_t status;
    size_t i;

    if (input == NULL || out_index == NULL) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    *out_index = NULL;
    config = *input;
    status = memx_validate_config(&config);
    if (status != MEMX_OK) {
        return status;
    }

    memset(&temporary, 0, sizeof(temporary));
    temporary.config = config;
    index = config.allocator.allocate(config.allocator.context, sizeof(*index));
    if (index == NULL) {
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    if (((uintptr_t)index % (uintptr_t)MEMX_INTERNAL_ALIGNMENT) != 0U) {
        config.allocator.deallocate(config.allocator.context, index);
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    *index = temporary;
    index->granules_per_region = (size_t)1U
        << (config.region_shift - config.granule_shift);
    if (index->granules_per_region > (SIZE_MAX / sizeof(memx_handle_t))) {
        memx_deallocate(index, index);
        return MEMX_ERROR_UNSUPPORTED;
    }
    if (config.directory_mode == MEMX_DIRECTORY_BOUNDED) {
        index->region_count = config.managed_size >> config.region_shift;
        if (index->region_count > (SIZE_MAX / sizeof(memx_region_t))) {
            memx_deallocate(index, index);
            return MEMX_ERROR_OUT_OF_MEMORY;
        }
        index->directory.bounded = memx_allocate(
            index, index->region_count * sizeof(memx_region_t));
        if (index->directory.bounded == NULL) {
            memx_deallocate(index, index);
            return MEMX_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0U; i < index->region_count; ++i) {
            memx_region_init(&index->directory.bounded[i]);
        }
    } else {
        index->sparse_region_bits = config.address_bits - config.region_shift;
        index->sparse_leaf_bits = index->sparse_region_bits / 2U;
        index->sparse_root_count = (size_t)1U
            << (index->sparse_region_bits - index->sparse_leaf_bits);
        index->sparse_leaf_count = (size_t)1U << index->sparse_leaf_bits;
        if (index->sparse_root_count
            > (SIZE_MAX / sizeof(memx_sparse_leaf_t *))) {
            memx_deallocate(index, index);
            return MEMX_ERROR_OUT_OF_MEMORY;
        }
        index->directory.sparse_root = memx_allocate(index,
            index->sparse_root_count * sizeof(memx_sparse_leaf_t *));
        if (index->directory.sparse_root == NULL) {
            memx_deallocate(index, index);
            return MEMX_ERROR_OUT_OF_MEMORY;
        }
        memset(index->directory.sparse_root, 0,
            index->sparse_root_count * sizeof(memx_sparse_leaf_t *));
        index->region_count = (size_t)1U << index->sparse_region_bits;
    }

    if ((index->granules_per_region - 1U) * sizeof(memx_handle_t)
        > SIZE_MAX - sizeof(memx_dense_region_t)) {
        if (config.directory_mode == MEMX_DIRECTORY_BOUNDED) {
            memx_deallocate(index, index->directory.bounded);
        } else {
            memx_deallocate(index, index->directory.sparse_root);
        }
        memx_deallocate(index, index);
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    index->dense_bytes = sizeof(memx_dense_region_t)
        + (index->granules_per_region - 1U) * sizeof(memx_handle_t);

    *out_index = index;
    return MEMX_OK;
}

static void
memx_region_destroy(memx_index_t *index, memx_region_t *region) {
    if (memx_region_type_decode(*region) == MEMX_REGION_DENSE) {
        memx_deallocate(index, memx_dense_decode(*region));
    }
    memx_region_init(region);
}

void
memx_index_destroy(memx_index_t *index) {
    size_t i;
    size_t j;

    if (index == NULL) {
        return;
    }
    if (index->flat_mode) {
        memx_deallocate(index, index->flat_entries);
    } else if (index->overlay_mode) {
        memx_overlay_deallocate(index->overlay_mapping, index->overlay_bytes);
        memx_deallocate(index, index->directory.bounded);
    } else if (index->config.directory_mode == MEMX_DIRECTORY_BOUNDED) {
        for (i = 0U; i < index->region_count; ++i) {
            memx_region_destroy(index, &index->directory.bounded[i]);
        }
        memx_deallocate(index, index->directory.bounded);
    } else {
        for (i = 0U; i < index->sparse_root_count; ++i) {
            memx_sparse_leaf_t *leaf = index->directory.sparse_root[i];
            if (leaf == NULL) {
                continue;
            }
            for (j = 0U; j < index->sparse_leaf_count; ++j) {
                memx_region_destroy(index, &leaf->regions[j]);
            }
            memx_deallocate(index, leaf);
        }
        memx_deallocate(index, index->directory.sparse_root);
    }
    memx_deallocate(index, index);
}

static bool
memx_address_to_region(
    const memx_index_t *index,
    uintptr_t address,
    size_t *out_region_index,
    size_t *out_granule_index) {
    uintptr_t relative;

    if (index->config.directory_mode == MEMX_DIRECTORY_BOUNDED) {
        if (address < index->config.base_address) {
            return false;
        }
        relative = address - index->config.base_address;
        if (relative >= (uintptr_t)index->config.managed_size) {
            return false;
        }
    } else {
        if (index->config.address_bits < (unsigned)(sizeof(uintptr_t) * CHAR_BIT)
            && (address >> index->config.address_bits) != 0U) {
            return false;
        }
        relative = address;
    }

    *out_region_index = (size_t)(relative >> index->config.region_shift);
    *out_granule_index = (size_t)
        ((relative >> index->config.granule_shift)
        & (uintptr_t)(index->granules_per_region - 1U));
    return true;
}

static memx_region_t *
memx_get_region_mut(memx_index_t *index, size_t region_index, bool create) {
    size_t root_index;
    size_t leaf_index;
    memx_sparse_leaf_t *leaf;
    size_t bytes;
    size_t i;

    if (index->config.directory_mode == MEMX_DIRECTORY_BOUNDED) {
        return &index->directory.bounded[region_index];
    }

    root_index = region_index >> index->sparse_leaf_bits;
    leaf_index = region_index & (index->sparse_leaf_count - 1U);
    leaf = index->directory.sparse_root[root_index];
    if (leaf == NULL && create) {
        bytes = sizeof(memx_sparse_leaf_t)
            + (index->sparse_leaf_count - 1U) * sizeof(memx_region_t);
        leaf = memx_allocate(index, bytes);
        if (leaf == NULL) {
            return NULL;
        }
        for (i = 0U; i < index->sparse_leaf_count; ++i) {
            memx_region_init(&leaf->regions[i]);
        }
        index->directory.sparse_root[root_index] = leaf;
        index->allocated_sparse_leaves += 1U;
    }
    return leaf == NULL ? NULL : &leaf->regions[leaf_index];
}

static void
memx_sparse_prune_leaf(memx_index_t *index, size_t region_index) {
    size_t root_index;
    size_t i;
    memx_sparse_leaf_t *leaf;

    if (index->config.directory_mode != MEMX_DIRECTORY_SPARSE) {
        return;
    }
    root_index = region_index >> index->sparse_leaf_bits;
    leaf = index->directory.sparse_root[root_index];
    if (leaf == NULL) {
        return;
    }
    for (i = 0U; i < index->sparse_leaf_count; ++i) {
        if (leaf->regions[i] != 0U) {
            return;
        }
    }
    index->directory.sparse_root[root_index] = NULL;
    index->allocated_sparse_leaves -= 1U;
    memx_deallocate(index, leaf);
}

/* Return the next representable region boundary. The final address-space
 * region has a mathematical end of 2^word_bits; in that overflow case return
 * the requested exclusive end. Callers clamp ordinary boundaries to end. */
static uintptr_t
memx_region_boundary(
    uintptr_t cursor,
    uintptr_t region_size,
    uintptr_t end) {
    const uintptr_t region_mask = region_size - 1U;
    const uintptr_t aligned = cursor & ~region_mask;
    if (aligned > UINTPTR_MAX - region_size) {
        return end;
    }
    {
        const uintptr_t boundary = aligned + region_size;
        return boundary;
    }
}

static const memx_region_t *
memx_get_region(const memx_index_t *index, size_t region_index) {
    size_t root_index;
    size_t leaf_index;
    const memx_sparse_leaf_t *leaf;

    if (index->config.directory_mode == MEMX_DIRECTORY_BOUNDED) {
        return &index->directory.bounded[region_index];
    }
    root_index = region_index >> index->sparse_leaf_bits;
    leaf_index = region_index & (index->sparse_leaf_count - 1U);
    leaf = index->directory.sparse_root[root_index];
    return leaf == NULL ? NULL : &leaf->regions[leaf_index];
}

static memx_handle_t
memx_region_lookup(const memx_region_t *region, size_t granule_index) {
    memx_region_type_t type;
    if (region == NULL) {
        return MEMX_HANDLE_INVALID;
    }
    type = memx_region_type_decode(*region);
    if (type == MEMX_REGION_EMPTY) {
        return MEMX_HANDLE_INVALID;
    }
    if (type == MEMX_REGION_UNIFORM) {
        return memx_uniform_decode(*region);
    }
    return memx_dense_decode(*region)->entries[granule_index];
}

memx_handle_t
memx_lookup_address(const memx_index_t *index, uintptr_t address) {
    size_t region_index;
    size_t granule_index;
    const memx_region_t *region;

    if (index == NULL
        || !memx_address_to_region(
            index, address, &region_index, &granule_index)) {
        return MEMX_HANDLE_INVALID;
    }
    if (index->flat_mode) {
        const uintptr_t relative = address - index->config.base_address;
        return index->flat_entries[relative >> index->config.granule_shift];
    }
    region = memx_get_region(index, region_index);
    if (region == NULL) {
        return MEMX_HANDLE_INVALID;
    }
    if (index->overlay_mode) {
        const memx_region_type_t type = memx_region_type_decode(*region);
        const uintptr_t relative = address - index->config.base_address;
        if (type == MEMX_REGION_EMPTY) {
            return MEMX_HANDLE_INVALID;
        }
        if (type == MEMX_REGION_UNIFORM) {
            return memx_uniform_decode(*region);
        }
        return index->overlay_entries[
            relative >> index->config.granule_shift];
    }
    return memx_region_lookup(region, granule_index);
}

memx_status_t
memx_index_bounded_view(
    const memx_index_t *index,
    memx_bounded_view_t *out_view) {
    if (index == NULL || out_view == NULL) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (index->config.directory_mode != MEMX_DIRECTORY_BOUNDED
        || index->flat_mode || index->overlay_mode) {
        return MEMX_ERROR_UNSUPPORTED;
    }
    out_view->base_address = index->config.base_address;
    out_view->region_shift = index->config.region_shift;
    out_view->granule_shift = index->config.granule_shift;
    out_view->granules_per_region = index->granules_per_region;
    out_view->descriptors = index->directory.bounded;
    return MEMX_OK;
}

memx_status_t
memx_index_flat_view(
    const memx_index_t *index,
    memx_flat_view_t *out_view) {
    if (index == NULL || out_view == NULL) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (!index->flat_mode) {
        return MEMX_ERROR_UNSUPPORTED;
    }
    out_view->base_address = index->config.base_address;
    out_view->granule_shift = index->config.granule_shift;
    out_view->entry_count = index->config.managed_size
        >> index->config.granule_shift;
    out_view->entries = index->flat_entries;
    return MEMX_OK;
}

memx_status_t
memx_index_overlay_view(
    const memx_index_t *index,
    memx_overlay_view_t *out_view) {
    if (index == NULL || out_view == NULL) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (!index->overlay_mode) {
        return MEMX_ERROR_UNSUPPORTED;
    }
    out_view->base_address = index->config.base_address;
    out_view->region_shift = index->config.region_shift;
    out_view->granule_shift = index->config.granule_shift;
    out_view->entry_count = index->config.managed_size
        >> index->config.granule_shift;
    out_view->descriptors = index->directory.bounded;
    out_view->dense_overlay = index->overlay_entries;
    return MEMX_OK;
}

memx_status_t
memx_index_optimize(memx_index_t *index) {
    memx_handle_t *entries;
    size_t entry_count;
    size_t region_index;
    size_t granule_index;

    if (index == NULL) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (index->flat_mode) {
        return MEMX_OK;
    }
    if (index->overlay_mode) {
        return MEMX_ERROR_BUSY;
    }
    if (index->config.directory_mode != MEMX_DIRECTORY_BOUNDED
        || index->dense_regions != index->region_count) {
        return MEMX_ERROR_UNSUPPORTED;
    }
    if (index->region_count > SIZE_MAX / index->granules_per_region) {
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    entry_count = index->region_count * index->granules_per_region;
    if (entry_count > SIZE_MAX / sizeof(memx_handle_t)) {
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    entries = memx_allocate(index, entry_count * sizeof(memx_handle_t));
    if (entries == NULL) {
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    for (region_index = 0U;
         region_index < index->region_count;
         ++region_index) {
        memx_dense_region_t *dense = memx_dense_decode(
            index->directory.bounded[region_index]);
        for (granule_index = 0U;
             granule_index < index->granules_per_region;
             ++granule_index) {
            entries[region_index * index->granules_per_region + granule_index]
                = dense->entries[granule_index];
        }
    }
    for (region_index = 0U;
         region_index < index->region_count;
         ++region_index) {
        memx_deallocate(index, memx_dense_decode(
            index->directory.bounded[region_index]));
    }
    memx_deallocate(index, index->directory.bounded);
    index->directory.bounded = NULL;
    index->flat_entries = entries;
    index->flat_mode = true;
    index->dense_regions = 0U;
    return MEMX_OK;
}

memx_status_t
memx_index_optimize_overlay(memx_index_t *index) {
    memx_handle_t *entries;
    size_t entry_count;
    size_t bytes;
    size_t reserved_bytes = 0U;
    size_t page_size = 0U;
    size_t private_bytes = 0U;
    size_t previous_page_end = 0U;
    size_t overlay_region_bytes;
    void *mapping = NULL;
    size_t region_index;

    if (index == NULL) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (index->overlay_mode) {
        return MEMX_OK;
    }
    if (index->flat_mode
        || index->config.directory_mode != MEMX_DIRECTORY_BOUNDED
        || !memx_overlay_platform_supported()) {
        return MEMX_ERROR_UNSUPPORTED;
    }
    if (index->region_count > SIZE_MAX / index->granules_per_region) {
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    entry_count = index->region_count * index->granules_per_region;
    if (entry_count > SIZE_MAX / sizeof(memx_handle_t)) {
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    bytes = entry_count * sizeof(memx_handle_t);
    overlay_region_bytes = index->granules_per_region
        * sizeof(memx_handle_t);

    /*
     * Phase one is read-only: validate every tag, create the demand-zero
     * reservation, and copy dense values. EMPTY and UNIFORM overlay slots may
     * remain zero because every lookup selects them through the descriptor;
     * zero remains a valid public handle and is never used as an occupancy
     * sentinel.
     */
    for (region_index = 0U;
         region_index < index->region_count;
         ++region_index) {
        const memx_region_t descriptor =
            index->directory.bounded[region_index];
        const memx_region_type_t type =
            memx_region_type_decode(descriptor);
        if (descriptor != 0U && type == MEMX_REGION_EMPTY) {
            return MEMX_ERROR_INVALID_ARGUMENT;
        }
    }
    entries = memx_overlay_allocate(bytes, &reserved_bytes, &page_size,
        &mapping);
    if (entries == NULL) {
        return MEMX_ERROR_OUT_OF_MEMORY;
    }
    for (region_index = 0U;
         region_index < index->region_count;
         ++region_index) {
        const memx_region_t descriptor =
            index->directory.bounded[region_index];
        if (memx_region_type_decode(descriptor) == MEMX_REGION_DENSE) {
            const size_t dense_start = region_index * overlay_region_bytes;
            const size_t dense_end = dense_start + overlay_region_bytes;
            const size_t page_start = dense_start & ~(page_size - 1U);
            const size_t page_end = (dense_end + page_size - 1U)
                & ~(page_size - 1U);
            memcpy(&entries[region_index * index->granules_per_region],
                memx_dense_decode(descriptor)->entries,
                overlay_region_bytes);
            if (page_end > previous_page_end) {
                private_bytes += page_end
                    - (page_start < previous_page_end
                        ? previous_page_end : page_start);
                previous_page_end = page_end;
            }
        }
    }

    /* No operation below this publication point can fail. */
    index->overlay_entries = entries;
    index->overlay_mapping = mapping;
    index->overlay_bytes = reserved_bytes;
    index->overlay_private_bytes = private_bytes;
    index->overlay_mode = true;
    for (region_index = 0U;
         region_index < index->region_count;
         ++region_index) {
        memx_region_t *descriptor =
            &index->directory.bounded[region_index];
        if (memx_region_type_decode(*descriptor) == MEMX_REGION_DENSE) {
            memx_dense_region_t *old_dense = memx_dense_decode(*descriptor);
            *descriptor = MEMX_DESCRIPTOR_DENSE_TAG;
            memx_deallocate(index, old_dense);
        }
    }
    return MEMX_OK;
}

static memx_dense_region_t *
memx_dense_create(memx_index_t *index, memx_handle_t initial) {
    memx_dense_region_t *dense = memx_allocate(index, index->dense_bytes);
    size_t i;
    if (dense == NULL) {
        return NULL;
    }
    if ((((uintptr_t)dense) & MEMX_DESCRIPTOR_TAG_MASK) != 0U) {
        memx_deallocate(index, dense);
        return NULL;
    }
    for (i = 0U; i < index->granules_per_region; ++i) {
        dense->entries[i] = initial;
    }
    return dense;
}

static void
memx_region_reclassify(memx_index_t *index, memx_region_t *region) {
    memx_handle_t first;
    memx_dense_region_t *dense;
    size_t i;
    bool all_equal = true;

    if (memx_region_type_decode(*region) != MEMX_REGION_DENSE) {
        return;
    }
    dense = memx_dense_decode(*region);
    first = dense->entries[0];
    for (i = 1U; i < index->granules_per_region; ++i) {
        if (dense->entries[i] != first) {
            all_equal = false;
            break;
        }
    }
    if (all_equal && (first == MEMX_HANDLE_INVALID
            || first <= MEMX_INLINE_HANDLE_MAX)) {
        index->dense_regions -= 1U;
        if (first == MEMX_HANDLE_INVALID) {
            *region = 0U;
        } else {
            *region = memx_uniform_encode(first);
            index->uniform_regions += 1U;
        }
        memx_deallocate(index, dense);
    }
}

static memx_status_t
memx_validate_range(
    const memx_index_t *index,
    uintptr_t base,
    size_t size,
    uintptr_t *out_end) {
    size_t ignored_region;
    size_t ignored_granule;

    if (index == NULL || size == 0U
        || !memx_is_aligned(base, index->config.granule_shift)
        || (size & ((((size_t)1U) << index->config.granule_shift) - 1U)) != 0U
        || memx_add_overflows(base, size, out_end)) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (!memx_address_to_region(
            index, base, &ignored_region, &ignored_granule)
        || !memx_address_to_region(
            index, *out_end - 1U, &ignored_region, &ignored_granule)) {
        return MEMX_ERROR_OUT_OF_RANGE;
    }
    return MEMX_OK;
}

static memx_status_t
memx_preflight_range(
    memx_index_t *index,
    uintptr_t base,
    uintptr_t end,
    memx_handle_t expected,
    bool inserting) {
    const uintptr_t step = ((uintptr_t)1U) << index->config.granule_shift;
    uintptr_t address;
    memx_handle_t actual;

    for (address = base; address < end; address += step) {
        actual = memx_lookup_address(index, address);
        if (inserting) {
            if (actual != MEMX_HANDLE_INVALID) {
                return MEMX_ERROR_OVERLAP;
            }
        } else if (actual != expected) {
            return MEMX_ERROR_NOT_FOUND;
        }
    }
    return MEMX_OK;
}

static memx_status_t
memx_prepare_insert_regions(
    memx_index_t *index,
    uintptr_t base,
    uintptr_t end,
    memx_handle_t handle) {
    const uintptr_t region_size = ((uintptr_t)1U) << index->config.region_shift;
    const uintptr_t region_mask = region_size - 1U;
    uintptr_t cursor = base;

    while (cursor < end) {
        size_t region_index = 0U;
        size_t granule_index = 0U;
        uintptr_t region_end = memx_region_boundary(cursor, region_size, end);
        uintptr_t part_end = region_end < end ? region_end : end;
        memx_region_t *region;
        bool whole_region = (cursor & region_mask) == 0U
            && part_end - cursor == region_size;

        (void)memx_address_to_region(
            index, cursor, &region_index, &granule_index);
        region = memx_get_region_mut(index, region_index, true);
        if (region == NULL) {
            return MEMX_ERROR_OUT_OF_MEMORY;
        }
        if (memx_region_type_decode(*region) == MEMX_REGION_EMPTY
            && (!whole_region || handle > MEMX_INLINE_HANDLE_MAX)) {
            memx_dense_region_t *dense = memx_dense_create(
                index, MEMX_HANDLE_INVALID);
            if (dense == NULL) {
                return MEMX_ERROR_OUT_OF_MEMORY;
            }
            *region = memx_dense_encode(dense);
            index->dense_regions += 1U;
        }
        cursor = part_end;
    }
    return MEMX_OK;
}

memx_status_t
memx_insert(
    memx_index_t *index,
    uintptr_t base,
    size_t size,
    memx_handle_t handle) {
    uintptr_t end;
    uintptr_t cursor;
    const uintptr_t region_size = index == NULL ? 0U
        : ((uintptr_t)1U) << index->config.region_shift;
    const uintptr_t granule_size = index == NULL ? 0U
        : ((uintptr_t)1U) << index->config.granule_shift;
    memx_status_t status;

    if (handle == MEMX_HANDLE_INVALID) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (index != NULL && index->overlay_mode) {
        return MEMX_ERROR_BUSY;
    }
    status = memx_validate_range(index, base, size, &end);
    if (status != MEMX_OK) {
        return status;
    }
    if (index->flat_mode) {
        uintptr_t step = ((uintptr_t)1U) << index->config.granule_shift;
        for (cursor = base; cursor < end; cursor += step) {
            size_t position = (size_t)
                ((cursor - index->config.base_address)
                 >> index->config.granule_shift);
            if (index->flat_entries[position] != MEMX_HANDLE_INVALID) {
                return MEMX_ERROR_OVERLAP;
            }
        }
        for (cursor = base; cursor < end; cursor += step) {
            size_t position = (size_t)
                ((cursor - index->config.base_address)
                 >> index->config.granule_shift);
            index->flat_entries[position] = handle;
        }
        return MEMX_OK;
    }
    status = memx_preflight_range(
        index, base, end, MEMX_HANDLE_INVALID, true);
    if (status != MEMX_OK) {
        return status;
    }
    status = memx_prepare_insert_regions(index, base, end, handle);
    if (status != MEMX_OK) {
        return status;
    }

    cursor = base;
    while (cursor < end) {
        size_t region_index = 0U;
        size_t granule_index = 0U;
        uintptr_t region_offset = cursor & (region_size - 1U);
        uintptr_t region_end = memx_region_boundary(cursor, region_size, end);
        uintptr_t part_end = region_end < end ? region_end : end;
        memx_region_t *region;

        (void)memx_address_to_region(
            index, cursor, &region_index, &granule_index);
        region = memx_get_region_mut(index, region_index, false);
        if (region == NULL) {
            return MEMX_ERROR_OUT_OF_MEMORY;
        }
        if (memx_region_type_decode(*region) == MEMX_REGION_EMPTY
            && region_offset == 0U && part_end == region_end
            && handle <= MEMX_INLINE_HANDLE_MAX) {
            *region = memx_uniform_encode(handle);
            index->uniform_regions += 1U;
        } else {
            size_t count = (size_t)((part_end - cursor) / granule_size);
            size_t i;
            if (memx_region_type_decode(*region) == MEMX_REGION_EMPTY) {
                memx_dense_region_t *dense = memx_dense_create(
                    index, MEMX_HANDLE_INVALID);
                if (dense == NULL) {
                    return MEMX_ERROR_OUT_OF_MEMORY;
                }
                *region = memx_dense_encode(dense);
                index->dense_regions += 1U;
            }
            for (i = 0U; i < count; ++i) {
                memx_dense_decode(*region)->entries[granule_index + i] = handle;
            }
            memx_region_reclassify(index, region);
        }
        cursor = part_end;
    }
    return MEMX_OK;
}

memx_status_t
memx_remove(
    memx_index_t *index,
    uintptr_t base,
    size_t size,
    memx_handle_t expected_handle) {
    uintptr_t end;
    uintptr_t cursor;
    uintptr_t granule_size;
    uintptr_t region_size;
    memx_status_t status;

    if (expected_handle == MEMX_HANDLE_INVALID) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (index != NULL && index->overlay_mode) {
        return MEMX_ERROR_BUSY;
    }
    status = memx_validate_range(index, base, size, &end);
    if (status != MEMX_OK) {
        return status;
    }
    if (index->flat_mode) {
        uintptr_t step = ((uintptr_t)1U) << index->config.granule_shift;
        for (cursor = base; cursor < end; cursor += step) {
            size_t position = (size_t)
                ((cursor - index->config.base_address)
                 >> index->config.granule_shift);
            if (index->flat_entries[position] != expected_handle) {
                return MEMX_ERROR_NOT_FOUND;
            }
        }
        for (cursor = base; cursor < end; cursor += step) {
            size_t position = (size_t)
                ((cursor - index->config.base_address)
                 >> index->config.granule_shift);
            index->flat_entries[position] = MEMX_HANDLE_INVALID;
        }
        return MEMX_OK;
    }
    status = memx_preflight_range(
        index, base, end, expected_handle, false);
    if (status != MEMX_OK) {
        return status;
    }

    /*
     * Convert every partially removed uniform region before publishing any
     * removal.  Allocation failure can change representation shape but never
     * the visible mapping, and the mutation loop below then cannot fail.
     */
    region_size = ((uintptr_t)1U) << index->config.region_shift;
    cursor = base;
    while (cursor < end) {
        size_t region_index = 0U;
        size_t granule_index = 0U;
        uintptr_t region_offset = cursor & (region_size - 1U);
        uintptr_t region_end = memx_region_boundary(cursor, region_size, end);
        uintptr_t part_end = region_end < end ? region_end : end;
        memx_region_t *region;
        (void)memx_address_to_region(
            index, cursor, &region_index, &granule_index);
        (void)granule_index;
        region = memx_get_region_mut(index, region_index, false);
        if (region == NULL) {
            return MEMX_ERROR_NOT_FOUND;
        }
        if (memx_region_type_decode(*region) == MEMX_REGION_UNIFORM
            && !(region_offset == 0U && part_end == region_end)) {
            memx_dense_region_t *dense = memx_dense_create(
                index, memx_uniform_decode(*region));
            if (dense == NULL) {
                return MEMX_ERROR_OUT_OF_MEMORY;
            }
            index->uniform_regions -= 1U;
            index->dense_regions += 1U;
            *region = memx_dense_encode(dense);
        }
        cursor = part_end;
    }

    granule_size = ((uintptr_t)1U) << index->config.granule_shift;
    region_size = ((uintptr_t)1U) << index->config.region_shift;
    cursor = base;
    while (cursor < end) {
        size_t region_index = 0U;
        size_t granule_index = 0U;
        uintptr_t region_offset = cursor & (region_size - 1U);
        uintptr_t region_end = memx_region_boundary(cursor, region_size, end);
        uintptr_t part_end = region_end < end ? region_end : end;
        size_t count = (size_t)((part_end - cursor) / granule_size);
        memx_region_t *region;
        size_t i;

        (void)memx_address_to_region(
            index, cursor, &region_index, &granule_index);
        region = memx_get_region_mut(index, region_index, false);
        if (region == NULL) {
            return MEMX_ERROR_NOT_FOUND;
        }
        if (memx_region_type_decode(*region) == MEMX_REGION_UNIFORM
            && region_offset == 0U && part_end == region_end) {
            *region = 0U;
            index->uniform_regions -= 1U;
        } else {
            for (i = 0U; i < count; ++i) {
                memx_dense_decode(*region)->entries[granule_index + i]
                    = MEMX_HANDLE_INVALID;
            }
            memx_region_reclassify(index, region);
        }
        cursor = part_end;
        if (index->config.directory_mode == MEMX_DIRECTORY_SPARSE) {
            const size_t root_index = region_index
                >> index->sparse_leaf_bits;
            size_t next_region_index = 0U;
            size_t ignored_granule_index = 0U;
            const bool leaves_root = cursor == end
                || (memx_address_to_region(index, cursor,
                        &next_region_index, &ignored_granule_index)
                    && (next_region_index >> index->sparse_leaf_bits)
                        != root_index);
            if (leaves_root) {
                memx_sparse_prune_leaf(index, region_index);
            }
        }
    }
    return MEMX_OK;
}

void
memx_index_stats(const memx_index_t *index, memx_stats_t *out_stats) {
    size_t sparse_leaf_bytes = 0U;
    size_t directory_bytes;
    size_t representation_bytes;
    size_t flat_entry_bytes;
    size_t page_size;
    size_t page_accounted;
    static const size_t max_size = SIZE_MAX;

    /* Stats have no status return; saturating arithmetic is safer than
     * publishing a wrapped footprint that could make a benchmark claim look
     * artificially compact. */
    #define MEMX_SAT_ADD(left, right) \
        ((left) > max_size - (right) ? max_size : (left) + (right))
    #define MEMX_SAT_MUL(left, right) \
        ((right) != 0U && (left) > max_size / (right) \
            ? max_size : (left) * (right))
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (index == NULL) {
        return;
    }
    page_size = memx_host_page_size();
    out_stats->region_count = index->region_count;
    out_stats->storage_mode = index->flat_mode
        ? MEMX_STORAGE_FLAT
        : (index->overlay_mode
            ? MEMX_STORAGE_OVERLAY : MEMX_STORAGE_ADAPTIVE);
    if (index->flat_mode) {
        flat_entry_bytes = MEMX_SAT_MUL(
            index->config.managed_size >> index->config.granule_shift,
            sizeof(memx_handle_t));
        out_stats->directory_bytes = MEMX_SAT_ADD(
            sizeof(*index), flat_entry_bytes);
        out_stats->dense_regions = index->region_count;
        out_stats->total_bytes = out_stats->directory_bytes;
        out_stats->page_accounted_bytes = MEMX_SAT_ADD(
            memx_page_account(sizeof(*index), page_size),
            memx_page_account(flat_entry_bytes, page_size));
        return;
    }
    out_stats->uniform_regions = index->uniform_regions;
    out_stats->dense_regions = index->dense_regions;
    if (index->config.directory_mode == MEMX_DIRECTORY_BOUNDED) {
        directory_bytes = MEMX_SAT_MUL(
            index->region_count, sizeof(memx_region_t));
        out_stats->directory_bytes = MEMX_SAT_ADD(
            sizeof(*index), directory_bytes);
        out_stats->empty_regions = index->region_count
            - index->uniform_regions - index->dense_regions;
    } else {
        sparse_leaf_bytes = MEMX_SAT_ADD(sizeof(memx_sparse_leaf_t),
            MEMX_SAT_MUL(index->sparse_leaf_count - 1U,
                sizeof(memx_region_t)));
        directory_bytes = MEMX_SAT_ADD(sizeof(*index), MEMX_SAT_MUL(
            index->sparse_root_count, sizeof(memx_sparse_leaf_t *)));
        directory_bytes = MEMX_SAT_ADD(directory_bytes, MEMX_SAT_MUL(
            index->allocated_sparse_leaves, sparse_leaf_bytes));
        out_stats->directory_bytes = directory_bytes;
        {
            size_t covered = MEMX_SAT_MUL(index->allocated_sparse_leaves,
                index->sparse_leaf_count);
            size_t used = MEMX_SAT_ADD(index->uniform_regions,
                index->dense_regions);
            out_stats->empty_regions = covered > used ? covered - used : 0U;
        }
    }
    representation_bytes = index->overlay_mode
        ? index->overlay_private_bytes
        : MEMX_SAT_MUL(index->dense_regions, index->dense_bytes);
    out_stats->representation_bytes = representation_bytes;
    out_stats->reserved_bytes = index->overlay_mode
        ? index->overlay_bytes : 0U;
    out_stats->total_bytes = MEMX_SAT_ADD(
        out_stats->directory_bytes, representation_bytes);
    page_accounted = memx_page_account(sizeof(*index), page_size);
    if (index->config.directory_mode == MEMX_DIRECTORY_BOUNDED) {
        page_accounted = MEMX_SAT_ADD(page_accounted,
            memx_page_account(MEMX_SAT_MUL(index->region_count,
                sizeof(memx_region_t)), page_size));
    } else {
        page_accounted = MEMX_SAT_ADD(page_accounted,
            memx_page_account(MEMX_SAT_MUL(index->sparse_root_count,
                sizeof(memx_sparse_leaf_t *)), page_size));
        page_accounted = MEMX_SAT_ADD(page_accounted,
            MEMX_SAT_MUL(index->allocated_sparse_leaves,
                memx_page_account(sparse_leaf_bytes, page_size)));
    }
    if (index->overlay_mode) {
        page_accounted = MEMX_SAT_ADD(
            page_accounted, index->overlay_private_bytes);
    } else {
        page_accounted = MEMX_SAT_ADD(page_accounted,
            MEMX_SAT_MUL(index->dense_regions,
                memx_page_account(index->dense_bytes, page_size)));
    }
    out_stats->page_accounted_bytes = page_accounted;
    #undef MEMX_SAT_ADD
    #undef MEMX_SAT_MUL
}

memx_status_t
memx_region_type_at(
    const memx_index_t *index,
    uintptr_t address,
    memx_region_type_t *out_type) {
    size_t region_index;
    size_t granule_index;
    const memx_region_t *region;

    if (index == NULL || out_type == NULL) {
        return MEMX_ERROR_INVALID_ARGUMENT;
    }
    if (!memx_address_to_region(
            index, address, &region_index, &granule_index)) {
        return MEMX_ERROR_OUT_OF_RANGE;
    }
    (void)granule_index;
    if (index->flat_mode) {
        *out_type = MEMX_REGION_DENSE;
        return MEMX_OK;
    }
    region = memx_get_region(index, region_index);
    *out_type = region == NULL
        ? MEMX_REGION_EMPTY : memx_region_type_decode(*region);
    return MEMX_OK;
}

const char *
memx_status_string(memx_status_t status) {
    switch (status) {
        case MEMX_OK: return "ok";
        case MEMX_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case MEMX_ERROR_OUT_OF_RANGE: return "out of range";
        case MEMX_ERROR_OVERLAP: return "overlap";
        case MEMX_ERROR_NOT_FOUND: return "not found";
        case MEMX_ERROR_OUT_OF_MEMORY: return "out of memory";
        case MEMX_ERROR_BUSY: return "busy";
        case MEMX_ERROR_UNSUPPORTED: return "unsupported";
        default: return "unknown";
    }
}
