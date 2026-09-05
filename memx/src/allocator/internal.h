#ifndef MEMX_ALLOCATOR_INTERNAL_H
#define MEMX_ALLOCATOR_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct memx_os_range {
    void *base;
    size_t size;
} memx_os_range_t;

size_t memx_os_page_size(void);

/* Transparent-huge-page size, or 0 when the platform cannot back the arena
 * with large pages. Callers align reservations and commits to this value so a
 * first touch can fault in a whole huge page instead of collapsing later. */
size_t memx_os_huge_page_size(void);

/* Best-effort request that a reserved range be backed by huge pages. */
void memx_os_advise_huge_pages(void *base, size_t size);

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
