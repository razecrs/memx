#if !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "memx/allocator.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t
now_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000)
        + (uint64_t)value.tv_nsec;
}

static uint64_t
random_next(uint64_t *state) {
    uint64_t value = *state;
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    *state = value;
    return value * UINT64_C(2685821657738717);
}

static bool
parse_count(const char *text, size_t *out_count) {
    char *end;
    uintmax_t value;
    if (*text < '0' || *text > '9') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0U || value > SIZE_MAX) {
        return false;
    }
    *out_count = (size_t)value;
    return true;
}

int
main(int argc, char **argv) {
    enum { ALLOCATION_SIZE = 16384, SHRUNK_SIZE = 12288 };
    size_t count;
    size_t queries;
    void **pointers;
    size_t *order;
    memx_heap_t *heap = NULL;
    uint64_t state = UINT64_C(0x6c6172676562656e);
    uint64_t begin;
    uint64_t allocation_ns;
    uint64_t hit_ns;
    uint64_t miss_ns;
    uint64_t resize_ns;
    uint64_t free_ns;
    uint64_t checksum = 0U;
    size_t index;
    int result = EXIT_FAILURE;

    if (argc != 3 || !parse_count(argv[1], &count)
        || !parse_count(argv[2], &queries)
        || count > SIZE_MAX / sizeof(*pointers)
        || count > SIZE_MAX / sizeof(*order)) {
        fprintf(stderr, "usage: %s live-allocations queries\n", argv[0]);
        return EXIT_FAILURE;
    }
    pointers = calloc(count, sizeof(*pointers));
    order = calloc(count, sizeof(*order));
    if (pointers == NULL || order == NULL
        || memx_heap_create(NULL, &heap) != MEMX_HEAP_OK) {
        goto cleanup;
    }
    for (index = 0U; index < count; ++index) {
        order[index] = index;
    }
    for (index = count; index > 1U; --index) {
        const size_t other = (size_t)(random_next(&state) % index);
        const size_t saved = order[index - 1U];
        order[index - 1U] = order[other];
        order[other] = saved;
    }
    begin = now_ns();
    for (index = 0U; index < count; ++index) {
        unsigned char *bytes = memx_heap_malloc(heap, ALLOCATION_SIZE);
        if (bytes == NULL) {
            goto cleanup;
        }
        pointers[index] = bytes;
        bytes[0] = (unsigned char)index;
        bytes[SHRUNK_SIZE - 1U] = (unsigned char)(index + 1U);
    }
    allocation_ns = now_ns() - begin;
    begin = now_ns();
    for (index = 0U; index < queries; ++index) {
        checksum += memx_heap_usable_size(heap, pointers[order[index % count]]);
    }
    hit_ns = now_ns() - begin;
    if (checksum != (uint64_t)queries * ALLOCATION_SIZE) {
        goto cleanup;
    }
    begin = now_ns();
    for (index = 0U; index < queries; ++index) {
        const unsigned char *bytes = pointers[order[index % count]];
        checksum += memx_heap_usable_size(heap, bytes + 1U);
    }
    miss_ns = now_ns() - begin;
    if (checksum != (uint64_t)queries * ALLOCATION_SIZE) {
        goto cleanup;
    }
    begin = now_ns();
    for (index = 0U; index < count; ++index) {
        void *pointer = pointers[order[index]];
        if (memx_heap_realloc(heap, pointer, SHRUNK_SIZE) != pointer) {
            goto cleanup;
        }
    }
    resize_ns = now_ns() - begin;
    for (index = 0U; index < count; ++index) {
        const unsigned char *bytes = pointers[index];
        if (memx_heap_usable_size(heap, bytes) != SHRUNK_SIZE
            || bytes[0] != (unsigned char)index
            || bytes[SHRUNK_SIZE - 1U] != (unsigned char)(index + 1U)) {
            goto cleanup;
        }
    }
    begin = now_ns();
    for (index = 0U; index < count; ++index) {
        if (!memx_heap_free(heap, pointers[order[index]])) {
            goto cleanup;
        }
        pointers[order[index]] = NULL;
    }
    free_ns = now_ns() - begin;
    printf("{\"live_allocations\":%zu,\"queries\":%zu,"
        "\"allocate_ns_per_op\":%.3f,\"hit_ns_per_op\":%.3f,"
        "\"miss_ns_per_op\":%.3f,\"resize_ns_per_op\":%.3f,"
        "\"free_ns_per_op\":%.3f,\"checksum\":%" PRIu64 "}\n",
        count, queries, (double)allocation_ns / (double)count,
        (double)hit_ns / (double)queries, (double)miss_ns / (double)queries,
        (double)resize_ns / (double)count, (double)free_ns / (double)count,
        checksum);
    result = EXIT_SUCCESS;
cleanup:
    /* Destruction also releases allocations after an incomplete run. */
    memx_heap_destroy(heap);
    free(pointers);
    free(order);
    if (result != EXIT_SUCCESS) {
        fputs("large allocation benchmark failed\n", stderr);
    }
    return result;
}
