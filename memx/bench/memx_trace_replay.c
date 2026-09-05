#if !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "memx/memx.h"
#include "trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct replay_result {
    size_t applied_events;
    size_t lookup_count;
    size_t lookup_failures;
    size_t mutation_failures;
    uint64_t lookup_nanoseconds;
    uint64_t checksum;
    memx_stats_t index_stats;
    size_t peak_page_accounted_bytes;
} replay_result_t;

static uint64_t
nanoseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000)
        + (uint64_t)value.tv_nsec;
}

static int
align_coverage(
    const memx_trace_t *trace,
    const memx_trace_stats_t *stats,
    uintptr_t *out_base,
    size_t *out_size) {
    uintptr_t region_size = ((uintptr_t)1U) << trace->region_shift;
    uintptr_t mask = region_size - 1U;
    uintptr_t base = stats->minimum_address & ~mask;
    uintptr_t high;
    if (stats->maximum_address_exclusive == 0U) {
        return 0;
    }
    if (stats->maximum_address_exclusive > UINTPTR_MAX - mask) {
        return 0;
    }
    high = (stats->maximum_address_exclusive + mask) & ~mask;
    if (high <= base || high - base > SIZE_MAX) {
        return 0;
    }
    *out_base = base;
    *out_size = (size_t)(high - base);
    return 1;
}

static int
replay(
    const memx_trace_t *trace,
    memx_directory_mode_t mode,
    uintptr_t bounded_base,
    size_t bounded_size,
    replay_result_t *result) {
    memx_config_t config = memx_config_default();
    memx_index_t *index = NULL;
    size_t i;
    memset(result, 0, sizeof(*result));
    config.directory_mode = mode;
    config.region_shift = trace->region_shift;
    config.granule_shift = trace->granule_shift;
    if (mode == MEMX_DIRECTORY_BOUNDED) {
        config.base_address = bounded_base;
        config.managed_size = bounded_size;
    } else {
        config.base_address = 0U;
        config.managed_size = 0U;
        config.address_bits = 48U;
    }
    if (memx_index_create(&config, &index) != MEMX_OK) {
        return 0;
    }

    for (i = 0U; i < trace->event_count; ++i) {
        const memx_trace_event_t *event = &trace->events[i];
        memx_status_t status = MEMX_OK;
        result->applied_events += 1U;
        switch (event->operation) {
            case MEMX_TRACE_INSERT:
                status = memx_insert(index, event->address,
                    event->size, event->handle);
                break;
            case MEMX_TRACE_LOOKUP: {
                uint64_t begin = nanoseconds();
                memx_handle_t handle = memx_lookup_address(
                    index, event->address);
                uint64_t end = nanoseconds();
                result->lookup_nanoseconds += end - begin;
                result->lookup_count += 1U;
                result->checksum += (uint64_t)handle;
                result->checksum ^= result->checksum << 9U;
                if (handle != event->handle) {
                    result->lookup_failures += 1U;
                }
                break;
            }
            case MEMX_TRACE_RETIRE:
                /* Lifetime event: the Gate 1 index remains mapped. */
                break;
            case MEMX_TRACE_REMOVE:
                status = memx_remove(index, event->address,
                    event->size, event->handle);
                break;
            case MEMX_TRACE_QUIESCENT:
                /* Recorded for future lifetime-domain replay. */
                break;
            default:
                status = MEMX_ERROR_INVALID_ARGUMENT;
                break;
        }
        if (status != MEMX_OK) {
            result->mutation_failures += 1U;
        }
        if (event->operation == MEMX_TRACE_INSERT
            || event->operation == MEMX_TRACE_REMOVE) {
            memx_stats_t current;
            memx_index_stats(index, &current);
            if (current.page_accounted_bytes
                > result->peak_page_accounted_bytes) {
                result->peak_page_accounted_bytes =
                    current.page_accounted_bytes;
            }
        }
    }
    memx_index_stats(index, &result->index_stats);
    memx_index_destroy(index);
    return 1;
}

static void
print_result(const char *name, const replay_result_t *result) {
    double ns_per_lookup = result->lookup_count == 0U ? 0.0
        : (double)result->lookup_nanoseconds / (double)result->lookup_count;
    printf("%s,%zu,%zu,%zu,%zu,%.4f,%zu,%zu,0x%" PRIx64 "\n",
        name, result->applied_events, result->lookup_count,
        result->lookup_failures, result->mutation_failures,
        ns_per_lookup, result->index_stats.page_accounted_bytes,
        result->peak_page_accounted_bytes, result->checksum);
}

int
main(int argc, char **argv) {
    memx_trace_t trace;
    memx_trace_stats_t stats;
    replay_result_t bounded;
    replay_result_t sparse;
    uintptr_t base;
    size_t size;
    size_t error_line = 0U;
    memx_trace_status_t status;

    if (argc != 2) {
        fprintf(stderr, "usage: %s TRACE_FILE\n", argv[0]);
        return EXIT_FAILURE;
    }
    memx_trace_init(&trace);
    status = memx_trace_read_file(argv[1], &trace, &error_line);
    if (status != MEMX_TRACE_OK) {
        fprintf(stderr, "%s:%zu: %s\n", argv[1], error_line,
            memx_trace_status_string(status));
        return EXIT_FAILURE;
    }
    if (memx_trace_calculate_stats(&trace, &stats) != MEMX_TRACE_OK
        || !align_coverage(&trace, &stats, &base, &size)) {
        fprintf(stderr, "trace has no replayable address coverage\n");
        memx_trace_destroy(&trace);
        return EXIT_FAILURE;
    }
    if (!replay(&trace, MEMX_DIRECTORY_BOUNDED, base, size, &bounded)
        || !replay(&trace, MEMX_DIRECTORY_SPARSE, base, size, &sparse)) {
        fprintf(stderr, "failed to construct replay indexes\n");
        memx_trace_destroy(&trace);
        return EXIT_FAILURE;
    }

    printf("# trace=%s version=%u events=%zu threads=%zu peak_live=%zu\n",
        argv[1], trace.version, trace.event_count, stats.distinct_threads,
        stats.peak_live_bytes);
    printf("directory,events,lookups,lookup_failures,mutation_failures,"
           "instrumented_ns_per_lookup,final_page_accounted_bytes,"
           "peak_page_accounted_bytes,checksum\n");
    print_result("bounded", &bounded);
    print_result("sparse", &sparse);
    memx_trace_destroy(&trace);
    return bounded.lookup_failures == 0U && sparse.lookup_failures == 0U
        && bounded.mutation_failures == 0U && sparse.mutation_failures == 0U
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
