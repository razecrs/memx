#include "memx/memx.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum {
    FUZZ_REGION_SHIFT = 16,
    FUZZ_GRANULE_SHIFT = 12,
    FUZZ_REGIONS = 16,
    FUZZ_GRANULES = FUZZ_REGIONS << (FUZZ_REGION_SHIFT - FUZZ_GRANULE_SHIFT)
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint16_t
read_u16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0]
        | (uint16_t)((uint16_t)data[1] << 8U));
}

static uint32_t
read_u32(const uint8_t *data) {
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8U)
        | ((uint32_t)data[2] << 16U)
        | ((uint32_t)data[3] << 24U);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const uintptr_t base = 0x1000000U;
    const size_t granule_size = (size_t)1U << FUZZ_GRANULE_SHIFT;
    memx_config_t bounded_config = memx_config_default();
    memx_config_t sparse_config = memx_config_default();
    memx_index_t *bounded = NULL;
    memx_index_t *sparse = NULL;
    memx_handle_t oracle[FUZZ_GRANULES];
    size_t offset = 0U;
    size_t i;

    bounded_config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    bounded_config.region_shift = FUZZ_REGION_SHIFT;
    bounded_config.granule_shift = FUZZ_GRANULE_SHIFT;
    bounded_config.base_address = base;
    bounded_config.managed_size = FUZZ_REGIONS
        * ((size_t)1U << FUZZ_REGION_SHIFT);

    sparse_config = bounded_config;
    sparse_config.directory_mode = MEMX_DIRECTORY_SPARSE;
    sparse_config.base_address = 0U;
    sparse_config.managed_size = 0U;
    sparse_config.address_bits = 28U;

    if (memx_index_create(&bounded_config, &bounded) != MEMX_OK
        || memx_index_create(&sparse_config, &sparse) != MEMX_OK) {
        memx_index_destroy(bounded);
        memx_index_destroy(sparse);
        return 0;
    }
    for (i = 0U; i < FUZZ_GRANULES; ++i) {
        oracle[i] = MEMX_HANDLE_INVALID;
    }

    while (offset + 8U <= size) {
        unsigned operation = data[offset] % 5U;
        size_t first = read_u16(&data[offset + 1U]) % FUZZ_GRANULES;
        size_t count = 1U + data[offset + 3U] % 8U;
        memx_handle_t handle = (memx_handle_t)read_u32(&data[offset + 4U]);
        uintptr_t bounded_address;
        uintptr_t sparse_address;
        size_t j;
        int range_empty = 1;
        int range_matches = 1;

        if (first + count > FUZZ_GRANULES) {
            count = FUZZ_GRANULES - first;
        }
        bounded_address = base + first * granule_size;
        sparse_address = first * granule_size;
        for (j = 0U; j < count; ++j) {
            if (oracle[first + j] != MEMX_HANDLE_INVALID) {
                range_empty = 0;
            }
            if (oracle[first + j] != handle) {
                range_matches = 0;
            }
        }

        if (operation == 0U || operation == 1U) {
            memx_status_t a = memx_insert(bounded, bounded_address,
                count * granule_size, handle);
            memx_status_t b = memx_insert(sparse, sparse_address,
                count * granule_size, handle);
            if (a != b) abort();
            if (range_empty && handle != MEMX_HANDLE_INVALID) {
                if (a != MEMX_OK) abort();
                for (j = 0U; j < count; ++j) oracle[first + j] = handle;
            }
        } else if (operation == 2U) {
            memx_status_t a = memx_remove(bounded, bounded_address,
                count * granule_size, handle);
            memx_status_t b = memx_remove(sparse, sparse_address,
                count * granule_size, handle);
            if (a != b) abort();
            if (range_matches && handle != MEMX_HANDLE_INVALID) {
                if (a != MEMX_OK) abort();
                for (j = 0U; j < count; ++j) {
                    oracle[first + j] = MEMX_HANDLE_INVALID;
                }
            }
        } else {
            size_t slot = first;
            uintptr_t within = (uintptr_t)(read_u16(&data[offset + 2U])
                & (granule_size - 1U));
            memx_handle_t expected = oracle[slot];
            if (memx_lookup_address(bounded,
                    base + slot * granule_size + within) != expected
                || memx_lookup_address(sparse,
                    slot * granule_size + within) != expected) {
                abort();
            }
        }

        for (j = 0U; j < FUZZ_GRANULES; ++j) {
            if (memx_lookup_address(bounded,
                    base + j * granule_size) != oracle[j]
                || memx_lookup_address(sparse,
                    j * granule_size) != oracle[j]) {
                abort();
            }
        }
        offset += 8U;
    }

#if defined(__linux__)
    {
        memx_overlay_view_t view;
        if (memx_index_optimize_overlay(bounded) != MEMX_OK
            || memx_index_overlay_view(bounded, &view) != MEMX_OK) {
            abort();
        }
        for (i = 0U; i < FUZZ_GRANULES; ++i) {
            uintptr_t address = base + i * granule_size;
            if (memx_lookup_address(bounded, address) != oracle[i]) {
                abort();
            }
            if (oracle[i] != MEMX_HANDLE_INVALID
                && memx_overlay_lookup_trusted(&view, address + 3U)
                    != oracle[i]) {
                abort();
            }
        }
        if (memx_insert(bounded, base, granule_size, 1U)
                != MEMX_ERROR_BUSY
            || memx_remove(bounded, base, granule_size, 1U)
                != MEMX_ERROR_BUSY) {
            abort();
        }
    }
#endif

    memx_index_destroy(bounded);
    memx_index_destroy(sparse);
    return 0;
}
