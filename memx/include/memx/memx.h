#ifndef MEMX_MEMX_H
#define MEMX_MEMX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define MEMX_LIKELY(condition) __builtin_expect(!!(condition), 1)
#define MEMX_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#else
#define MEMX_LIKELY(condition) (condition)
#define MEMX_UNLIKELY(condition) (condition)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MemX v0 maps an address to allocator-defined granule/span metadata.  It does
 * not distinguish individual objects that share a granule; the returned
 * handle may lead to metadata that performs that second-level derivation.
 */
typedef uintptr_t memx_handle_t;

#define MEMX_HANDLE_INVALID UINTPTR_MAX

typedef struct memx_index memx_index_t;

typedef enum memx_status {
    MEMX_OK = 0,
    MEMX_ERROR_INVALID_ARGUMENT,
    MEMX_ERROR_OUT_OF_RANGE,
    MEMX_ERROR_OVERLAP,
    MEMX_ERROR_NOT_FOUND,
    MEMX_ERROR_OUT_OF_MEMORY,
    MEMX_ERROR_BUSY,
    MEMX_ERROR_UNSUPPORTED
} memx_status_t;

typedef enum memx_directory_mode {
    MEMX_DIRECTORY_BOUNDED = 0,
    MEMX_DIRECTORY_SPARSE = 1
} memx_directory_mode_t;

typedef enum memx_policy {
    MEMX_POLICY_FAST = 0,
    MEMX_POLICY_BALANCED = 1,
    MEMX_POLICY_COMPACT = 2
} memx_policy_t;

typedef enum memx_hint_mode {
    MEMX_HINT_VALIDATED = 0,
    MEMX_HINT_TRUSTED = 1
} memx_hint_mode_t;

typedef enum memx_region_type {
    MEMX_REGION_EMPTY = 0,
    MEMX_REGION_UNIFORM = 1,
    MEMX_REGION_DENSE = 2
} memx_region_type_t;

typedef enum memx_storage_mode {
    MEMX_STORAGE_ADAPTIVE = 0,
    MEMX_STORAGE_FLAT = 1,
    MEMX_STORAGE_OVERLAY = 2
} memx_storage_mode_t;

/*
 * Every non-NULL allocator result must be aligned to at least
 * max(_Alignof(uintptr_t), 4). The native-word alignment covers all v0
 * internal objects and the four-byte minimum reserves two descriptor tag
 * bits. A callback that violates this contract is treated as an allocation
 * failure rather than dereferenced. Requiring max_align_t here would be
 * unnecessarily strict: some conforming platform allocators use smaller
 * alignments for small allocations.
 */
typedef void *(*memx_allocate_fn)(void *context, size_t size);
typedef void (*memx_deallocate_fn)(void *context, void *pointer);

typedef struct memx_allocator {
    void *context;
    memx_allocate_fn allocate;
    memx_deallocate_fn deallocate;
} memx_allocator_t;

typedef struct memx_config {
    memx_directory_mode_t directory_mode;
    memx_policy_t policy;
    memx_hint_mode_t hint_mode;

    /* Both shifts must be powers-of-two exponents; region >= granule. */
    unsigned region_shift;
    unsigned granule_shift;

    /* BOUNDED: aligned managed range. SPARSE: base must be zero. */
    uintptr_t base_address;
    size_t managed_size;

    /* SPARSE only: usable low address bits, e.g. 48 on x86-64. */
    unsigned address_bits;

    /*
     * Back a frozen dense overlay with transparent huge pages where the
     * platform supports them. A huge page becomes resident as soon as any
     * granule inside it is dense, and memx_stats_t reports the resulting
     * footprint at that granularity.
     *
     * Off by default because it only pays off for overlays that are already
     * near fully dense. Measured on a 2048-region mixed layout at 25% dense
     * (see docs/benchmark.md), it cost 4x the resident metadata and made
     * trusted lookup about 1.8x slower, because inflating the overlay past the
     * cache-resident working set costs more than the saved TLB misses. On a
     * fully dense layout, where nothing is inflated, it made trusted lookup
     * about 29% faster at identical footprint.
     */
    bool overlay_huge_pages;

    /* Optional allocator; both callbacks must be supplied or both omitted. */
    memx_allocator_t allocator;
} memx_config_t;

typedef struct memx_stats {
    size_t directory_bytes;
    size_t representation_bytes;
    size_t total_bytes;
    /* Per-live-allocation page backing model used for equal-category gates. */
    size_t page_accounted_bytes;
    /* Virtual address space reserved by representations such as OVERLAY. */
    size_t reserved_bytes;
    size_t region_count;
    size_t empty_regions;
    size_t uniform_regions;
    size_t dense_regions;
    memx_storage_mode_t storage_mode;
} memx_stats_t;

/*
 * Immutable hot-path view for callers that already proved an address belongs
 * to a bounded index.  The descriptor encoding is versioned with the library;
 * callers must use the inline accessor rather than decode it themselves.
 * Any mutation of the index invalidates an existing view; callers must fetch
 * a fresh view before using it again.
 */
typedef struct memx_bounded_view {
    uintptr_t base_address;
    unsigned region_shift;
    unsigned granule_shift;
    size_t granules_per_region;
    const uintptr_t *descriptors;
} memx_bounded_view_t;

typedef struct memx_flat_view {
    uintptr_t base_address;
    unsigned granule_shift;
    size_t entry_count;
    const memx_handle_t *entries;
} memx_flat_view_t;

/*
 * Immutable bounded view produced by memx_index_optimize_overlay(). Dense
 * values use a direct global offset while uniform values remain inline in the
 * descriptor array. The virtual reservation is intentionally distinct from
 * private metadata bytes in memx_stats_t.
 */
typedef struct memx_overlay_view {
    uintptr_t base_address;
    unsigned region_shift;
    unsigned granule_shift;
    size_t entry_count;
    const uintptr_t *descriptors;
    const memx_handle_t *dense_overlay;
} memx_overlay_view_t;

/* Defaults: BOUNDED, BALANCED, 2 MiB regions, 4 KiB granules. */
memx_config_t memx_config_default(void);

memx_status_t memx_index_create(
    const memx_config_t *config,
    memx_index_t **out_index);

void memx_index_destroy(memx_index_t *index);

/*
 * Inserts one handle for every granule touched by [base, base + size).
 * base and size must be granule-aligned and the handle may not be INVALID.
 * Overlap with an existing mapping is rejected.
 */
memx_status_t memx_insert(
    memx_index_t *index,
    uintptr_t base,
    size_t size,
    memx_handle_t handle);

/* Removes only granules currently equal to expected_handle. */
memx_status_t memx_remove(
    memx_index_t *index,
    uintptr_t base,
    size_t size,
    memx_handle_t expected_handle);

/* One native-word result; INVALID is the sole not-found value. */
memx_handle_t memx_lookup_address(
    const memx_index_t *index,
    uintptr_t address);

static inline memx_handle_t
memx_lookup(const memx_index_t *index, const void *pointer) {
    return memx_lookup_address(index, (uintptr_t)pointer);
}

memx_status_t memx_index_bounded_view(
    const memx_index_t *index,
    memx_bounded_view_t *out_view);

/*
 * Converts a fully dense bounded adaptive index to a direct flat table.
 * Other layouts return UNSUPPORTED without changing lookup semantics.
 */
memx_status_t memx_index_optimize(memx_index_t *index);

/*
 * Freezes a bounded adaptive index into a VM-backed dense overlay. The
 * operation is currently supported on Linux-family kernels, including
 * Android. Once frozen, insert/remove return MEMX_ERROR_BUSY and lookups
 * remain valid until destruction.
 */
memx_status_t memx_index_optimize_overlay(memx_index_t *index);

memx_status_t memx_index_flat_view(
    const memx_index_t *index,
    memx_flat_view_t *out_view);

memx_status_t memx_index_overlay_view(
    const memx_index_t *index,
    memx_overlay_view_t *out_view);

static inline memx_handle_t
memx_bounded_lookup_assume_mapped(
    const memx_bounded_view_t *view,
    uintptr_t address) {
    const uintptr_t relative = address - view->base_address;
    const size_t region_index = (size_t)(relative >> view->region_shift);
    const size_t granule_index = (size_t)
        ((relative >> view->granule_shift)
         & (uintptr_t)(view->granules_per_region - 1U));
    const uintptr_t descriptor = view->descriptors[region_index];
    const uintptr_t tag = descriptor & (uintptr_t)3U;

    if (MEMX_UNLIKELY(descriptor == 0U)) {
        return MEMX_HANDLE_INVALID;
    }
    if (MEMX_LIKELY(tag == (uintptr_t)1U)) {
        return (memx_handle_t)(descriptor >> 2U);
    }
    if (MEMX_UNLIKELY(tag != (uintptr_t)2U)) {
        return MEMX_HANDLE_INVALID;
    }
    return ((const memx_handle_t *)(descriptor & ~(uintptr_t)3U))
        [granule_index];
}

static inline memx_handle_t
memx_flat_lookup_assume_mapped(
    const memx_flat_view_t *view,
    uintptr_t address) {
    const size_t index = (size_t)
        ((address - view->base_address) >> view->granule_shift);
    return view->entries[index];
}

/*
 * Branchless overlay lookup for allocator-internal hot paths. The caller must
 * prove that address is covered and currently mapped, and that the view came
 * from an uncorrupted frozen index. Violating that contract is undefined.
 */
static inline memx_handle_t
memx_overlay_lookup_trusted(
    const memx_overlay_view_t *view,
    uintptr_t address) {
    const uintptr_t relative = address - view->base_address;
    const size_t region_index = (size_t)(relative >> view->region_shift);
    const size_t granule_index = (size_t)(relative >> view->granule_shift);
    const uintptr_t descriptor = view->descriptors[region_index];
    const uintptr_t dense_mask = (uintptr_t)0U
        - ((descriptor >> 1U) & (uintptr_t)1U);
    const memx_handle_t dense = view->dense_overlay[granule_index];
    const memx_handle_t uniform = (memx_handle_t)(descriptor >> 2U);

    return (dense & (memx_handle_t)dense_mask)
        | (uniform & (memx_handle_t)~dense_mask);
}

void memx_index_stats(const memx_index_t *index, memx_stats_t *out_stats);

/* Diagnostic API used by tests and representation-policy experiments. */
memx_status_t memx_region_type_at(
    const memx_index_t *index,
    uintptr_t address,
    memx_region_type_t *out_type);

const char *memx_status_string(memx_status_t status);

#ifdef __cplusplus
}
#endif

#endif
