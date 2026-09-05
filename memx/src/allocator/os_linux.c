#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "internal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

static bool
memx_os_power_of_two(size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

size_t
memx_os_page_size(void) {
    const long result = sysconf(_SC_PAGESIZE);
    return result > 0 ? (size_t)result : 4096U;
}

/* Read the kernel's PMD-backed transparent huge page size and publish the
 * first completed result atomically. Concurrent cold callers may duplicate
 * the probe; a missing or unparsable value caches unavailable (zero). */
size_t
memx_os_huge_page_size(void) {
#if defined(MADV_HUGEPAGE)
    static atomic_size_t cached_size = SIZE_MAX;
    FILE *file;
    size_t value = 0U;
    size_t expected = SIZE_MAX;
    const size_t cached = atomic_load_explicit(
        &cached_size, memory_order_relaxed);

    if (cached != SIZE_MAX) {
        return cached;
    }
    file = fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "re");
    if (file != NULL) {
        if (fscanf(file, "%zu", &value) != 1) {
            value = 0U;
        }
        (void)fclose(file);
    }
    if (!memx_os_power_of_two(value) || value <= memx_os_page_size()) {
        value = 0U;
    }
    (void)atomic_compare_exchange_strong_explicit(
        &cached_size, &expected, value,
        memory_order_relaxed, memory_order_relaxed);
    return atomic_load_explicit(&cached_size, memory_order_relaxed);
#else
    return 0U;
#endif
}

void
memx_os_advise_huge_pages(void *base, size_t size) {
#if defined(MADV_HUGEPAGE)
    if (base != NULL && size != 0U) {
        (void)madvise(base, size, MADV_HUGEPAGE);
    }
#else
    (void)base;
    (void)size;
#endif
}

bool
memx_os_reserve_aligned(
    size_t size,
    size_t alignment,
    memx_os_range_t *out_range) {
    size_t request;
    unsigned char *mapping;
    uintptr_t aligned_address;
    size_t prefix;
    size_t suffix;

    if (out_range == NULL || size == 0U || !memx_os_power_of_two(alignment)
        || alignment < memx_os_page_size()
        || size > SIZE_MAX - alignment) {
        return false;
    }
    request = size + alignment;
    mapping = mmap(NULL, request, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return false;
    }
    aligned_address = ((uintptr_t)mapping + (uintptr_t)alignment - 1U)
        & ~((uintptr_t)alignment - 1U);
    prefix = (size_t)(aligned_address - (uintptr_t)mapping);
    suffix = request - prefix - size;
    if (prefix != 0U) {
        (void)munmap(mapping, prefix);
    }
    if (suffix != 0U) {
        (void)munmap((void *)(aligned_address + (uintptr_t)size), suffix);
    }
    out_range->base = (void *)aligned_address;
    out_range->size = size;
    return true;
}

bool
memx_os_commit(void *base, size_t size) {
    return base != NULL && size != 0U
        && mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
}

bool
memx_os_decommit(void *base, size_t size) {
    void *replacement;
    if (base == NULL || size == 0U) {
        return false;
    }
    replacement = mmap(base, size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return replacement == base;
}

void
memx_os_release(memx_os_range_t range) {
    if (range.base != NULL && range.size != 0U) {
        (void)munmap(range.base, range.size);
    }
}

bool
memx_os_large_allocate(
    size_t size,
    size_t alignment,
    memx_os_range_t *out_mapping,
    void **out_pointer) {
    const size_t page_size = memx_os_page_size();
    size_t effective_alignment = alignment < page_size ? page_size : alignment;
    size_t request;
    unsigned char *mapping;
    uintptr_t aligned_address;

    if (out_mapping == NULL || out_pointer == NULL || size == 0U
        || !memx_os_power_of_two(effective_alignment)
        || size > SIZE_MAX - effective_alignment) {
        return false;
    }
    request = size + effective_alignment;
    mapping = mmap(NULL, request, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return false;
    }
    aligned_address = ((uintptr_t)mapping
        + (uintptr_t)effective_alignment - 1U)
        & ~((uintptr_t)effective_alignment - 1U);
    out_mapping->base = mapping;
    out_mapping->size = request;
    *out_pointer = (void *)aligned_address;
    return true;
}
