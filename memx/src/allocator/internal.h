#ifndef MEMX_ALLOCATOR_INTERNAL_H
#define MEMX_ALLOCATOR_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct memx_os_range {
    void *base;
    size_t size;
} memx_os_range_t;

size_t memx_os_page_size(void);

bool memx_os_reserve_aligned(
    size_t size,
    size_t alignment,
    memx_os_range_t *out_range);

bool memx_os_commit(void *base, size_t size);
bool memx_os_decommit(void *base, size_t size);
void memx_os_release(memx_os_range_t range);

bool memx_os_large_allocate(
    size_t size,
    size_t alignment,
    memx_os_range_t *out_mapping,
    void **out_pointer);

#endif
