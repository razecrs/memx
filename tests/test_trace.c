#if !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        checks += 1U;                                                          \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: failed: %s\n",                           \
                __FILE__, __LINE__, #condition);                               \
            failures += 1U;                                                    \
        }                                                                      \
    } while (0)

static memx_trace_event_t
event(
    uint64_t sequence,
    uint32_t thread,
    memx_trace_operation_t operation,
    uintptr_t address,
    size_t size,
    memx_handle_t handle) {
    memx_trace_event_t value;
    value.sequence = sequence;
    value.thread_id = thread;
    value.operation = operation;
    value.address = address;
    value.size = size;
    value.handle = handle;
    return value;
}

static void
append(memx_trace_t *trace, memx_trace_event_t value) {
    CHECK(memx_trace_append(trace, &value) == MEMX_TRACE_OK);
}

static memx_trace_t
valid_trace(void) {
    memx_trace_t trace;
    memx_trace_init(&trace);
    trace.region_shift = 16U;
    trace.granule_shift = 12U;
    append(&trace, event(1U, 0U, MEMX_TRACE_INSERT,
        0x10000U, 8192U, 7U));
    append(&trace, event(2U, 0U, MEMX_TRACE_LOOKUP,
        0x10011U, 0U, 7U));
    append(&trace, event(3U, 1U, MEMX_TRACE_LOOKUP,
        0x11022U, 0U, 7U));
    append(&trace, event(4U, 0U, MEMX_TRACE_RETIRE,
        0x10000U, 8192U, 7U));
    append(&trace, event(5U, 0U, MEMX_TRACE_QUIESCENT,
        0U, 0U, 0U));
    append(&trace, event(6U, 1U, MEMX_TRACE_QUIESCENT,
        0U, 0U, 0U));
    append(&trace, event(7U, 0U, MEMX_TRACE_REMOVE,
        0x10000U, 8192U, 7U));
    return trace;
}

static void
test_init_destroy(void) {
    memx_trace_t trace;
    memx_trace_init(&trace);
    CHECK(trace.version == MEMX_TRACE_FORMAT_VERSION);
    CHECK(trace.region_shift == 21U);
    CHECK(trace.granule_shift == 12U);
    CHECK(trace.events == NULL);
    CHECK(trace.event_count == 0U);
    memx_trace_destroy(&trace);
    memx_trace_destroy(NULL);
}

static void
test_append_growth(void) {
    memx_trace_t trace;
    size_t i;
    memx_trace_init(&trace);
    for (i = 0U; i < 5000U; ++i) {
        memx_trace_event_t value = event(i + 1U, (uint32_t)(i % 8U),
            MEMX_TRACE_LOOKUP, (uintptr_t)i, 0U, 0U);
        CHECK(memx_trace_append(&trace, &value) == MEMX_TRACE_OK);
    }
    CHECK(trace.event_count == 5000U);
    CHECK(trace.event_capacity >= trace.event_count);
    CHECK(trace.events[4999].sequence == 5000U);
    memx_trace_destroy(&trace);
}

static void
test_validation(void) {
    memx_trace_t trace = valid_trace();
    size_t error = SIZE_MAX;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_OK);

    trace.version = 2U;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_ERROR_SEMANTIC);
    trace.version = 1U;
    trace.region_shift = 11U;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_ERROR_SEMANTIC);
    trace.region_shift = 16U;

    trace.events[1].sequence = trace.events[0].sequence;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_ERROR_SEMANTIC);
    CHECK(error == 1U);
    trace.events[1].sequence = 2U;

    trace.events[0].address += 1U;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_ERROR_SEMANTIC);
    CHECK(error == 0U);
    trace.events[0].address -= 1U;

    trace.events[0].size = 4095U;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_ERROR_SEMANTIC);
    trace.events[0].size = 8192U;

    trace.events[0].handle = MEMX_HANDLE_INVALID;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_ERROR_SEMANTIC);
    trace.events[0].handle = 7U;

    trace.events[1].size = 4096U;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_ERROR_SEMANTIC);
    trace.events[1].size = 0U;

    trace.events[4].address = 1U;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_ERROR_SEMANTIC);
    memx_trace_destroy(&trace);
}

static void
test_stats(void) {
    memx_trace_t trace = valid_trace();
    memx_trace_stats_t stats;
    CHECK(memx_trace_calculate_stats(&trace, &stats) == MEMX_TRACE_OK);
    CHECK(stats.inserts == 1U);
    CHECK(stats.lookups == 2U);
    CHECK(stats.retires == 1U);
    CHECK(stats.removes == 1U);
    CHECK(stats.quiescent_events == 2U);
    CHECK(stats.distinct_threads == 2U);
    CHECK(stats.minimum_address == 0x10000U);
    CHECK(stats.maximum_address_exclusive == 0x12000U);
    CHECK(stats.peak_live_bytes == 8192U);
    CHECK(stats.final_live_bytes == 0U);
    memx_trace_destroy(&trace);
}

static void
test_lifecycle_validation(void) {
    memx_trace_t trace = valid_trace();
    size_t error = SIZE_MAX;
    CHECK(memx_trace_validate_lifecycle(&trace, &error) == MEMX_TRACE_OK);

    /* RETIRED remains visible to lookup until REMOVE. */
    trace.events[5] = event(6U, 1U, MEMX_TRACE_LOOKUP,
        0x10001U, 0U, 7U);
    CHECK(memx_trace_validate_lifecycle(&trace, &error) == MEMX_TRACE_OK);
    trace.events[5] = event(6U, 1U, MEMX_TRACE_QUIESCENT,
        0U, 0U, 0U);

    trace.events[1].handle = 8U;
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_OK);
    CHECK(memx_trace_validate_lifecycle(&trace, &error)
        == MEMX_TRACE_ERROR_SEMANTIC);
    CHECK(error == 1U);
    trace.events[1].handle = 7U;

    trace.events[3].handle = 9U;
    CHECK(memx_trace_validate_lifecycle(&trace, &error)
        == MEMX_TRACE_ERROR_SEMANTIC);
    CHECK(error == 3U);
    trace.events[3].handle = 7U;

    trace.events[6].handle = 9U;
    CHECK(memx_trace_validate_lifecycle(&trace, &error)
        == MEMX_TRACE_ERROR_SEMANTIC);
    CHECK(error == 6U);
    trace.events[6].handle = 7U;

    /* Duplicate retirement of any granule is rejected. */
    trace.events[5] = event(6U, 1U, MEMX_TRACE_RETIRE,
        0x10000U, 4096U, 7U);
    CHECK(memx_trace_validate_lifecycle(&trace, &error)
        == MEMX_TRACE_ERROR_SEMANTIC);
    CHECK(error == 5U);
    trace.events[5] = event(6U, 1U, MEMX_TRACE_QUIESCENT,
        0U, 0U, 0U);

    /* A syntactically valid overlapping insert is semantically invalid. */
    trace.events[1] = event(2U, 0U, MEMX_TRACE_INSERT,
        0x11000U, 4096U, 10U);
    CHECK(memx_trace_validate(&trace, &error) == MEMX_TRACE_OK);
    CHECK(memx_trace_validate_lifecycle(&trace, &error)
        == MEMX_TRACE_ERROR_SEMANTIC);
    CHECK(error == 1U);
    trace.events[1] = event(2U, 0U, MEMX_TRACE_LOOKUP,
        0x10011U, 0U, 7U);

    /* Removing an unmapped subrange cannot underflow live-byte statistics. */
    trace.events[6] = event(7U, 0U, MEMX_TRACE_REMOVE,
        0x12000U, 4096U, 7U);
    CHECK(memx_trace_validate_lifecycle(&trace, &error)
        == MEMX_TRACE_ERROR_SEMANTIC);
    CHECK(error == 6U);
    {
        memx_trace_stats_t stats;
        CHECK(memx_trace_calculate_stats(&trace, &stats)
            == MEMX_TRACE_ERROR_SEMANTIC);
    }
    memx_trace_destroy(&trace);
}

static void
test_lifecycle_reuse_after_remove(void) {
    memx_trace_t trace;
    size_t error = SIZE_MAX;
    memx_trace_init(&trace);
    trace.region_shift = 16U;
    trace.granule_shift = 12U;
    append(&trace, event(1U, 0U, MEMX_TRACE_INSERT,
        0x20000U, 4096U, 1U));
    append(&trace, event(2U, 0U, MEMX_TRACE_REMOVE,
        0x20000U, 4096U, 1U));
    append(&trace, event(3U, 1U, MEMX_TRACE_LOOKUP,
        0x20080U, 0U, MEMX_HANDLE_INVALID));
    append(&trace, event(4U, 1U, MEMX_TRACE_INSERT,
        0x20000U, 4096U, 2U));
    append(&trace, event(5U, 0U, MEMX_TRACE_LOOKUP,
        0x20fffU, 0U, 2U));
    CHECK(memx_trace_validate_lifecycle(&trace, &error) == MEMX_TRACE_OK);
    memx_trace_destroy(&trace);
}

static int
temporary_path(char *buffer, size_t size) {
    int descriptor;
    if (size < 32U) {
        return -1;
    }
    strcpy(buffer, "/tmp/memx-trace-test-XXXXXX");
    descriptor = mkstemp(buffer);
    if (descriptor >= 0) {
        close(descriptor);
    }
    return descriptor;
}

static void
test_round_trip(void) {
    char path[64];
    memx_trace_t original = valid_trace();
    memx_trace_t loaded;
    size_t error_line = 0U;
    size_t i;
    memx_trace_init(&loaded);
    CHECK(temporary_path(path, sizeof(path)) >= 0);
    CHECK(memx_trace_write_file(path, &original) == MEMX_TRACE_OK);
    CHECK(memx_trace_read_file(path, &loaded, &error_line) == MEMX_TRACE_OK);
    CHECK(loaded.version == original.version);
    CHECK(loaded.region_shift == original.region_shift);
    CHECK(loaded.granule_shift == original.granule_shift);
    CHECK(loaded.event_count == original.event_count);
    for (i = 0U; i < original.event_count && i < loaded.event_count; ++i) {
        CHECK(memcmp(&loaded.events[i], &original.events[i],
            sizeof(original.events[i])) == 0);
    }
    CHECK(unlink(path) == 0);
    memx_trace_destroy(&loaded);
    memx_trace_destroy(&original);
}

static void
write_text(const char *path, const char *text) {
    FILE *output = fopen(path, "w");
    CHECK(output != NULL);
    if (output != NULL) {
        CHECK(fputs(text, output) >= 0);
        CHECK(fclose(output) == 0);
    }
}

static void
test_parse_errors(void) {
    char path[64];
    memx_trace_t trace;
    size_t line = 0U;
    CHECK(temporary_path(path, sizeof(path)) >= 0);
    memx_trace_init(&trace);

    write_text(path, "wrong\n");
    CHECK(memx_trace_read_file(path, &trace, &line)
        == MEMX_TRACE_ERROR_FORMAT);
    CHECK(line == 1U);

    write_text(path,
        "MEMX_TRACE\nversion=2\nregion_shift=16\ngranule_shift=12\n"
        "sequence,thread,operation,address,size,handle\n");
    CHECK(memx_trace_read_file(path, &trace, &line)
        == MEMX_TRACE_ERROR_VERSION);

    write_text(path,
        "MEMX_TRACE\nversion=1\nregion_shift=16\ngranule_shift=12\n"
        "sequence,thread,operation,address,size,handle\n"
        "1,0,NOPE,0x10000,4096,1\n");
    CHECK(memx_trace_read_file(path, &trace, &line)
        == MEMX_TRACE_ERROR_FORMAT);

    write_text(path,
        "MEMX_TRACE\nversion=1\nregion_shift=16\ngranule_shift=12\n"
        "sequence,thread,operation,address,size,handle\n"
        "1,0,INSERT,0x10001,4096,1\n");
    CHECK(memx_trace_read_file(path, &trace, &line)
        == MEMX_TRACE_ERROR_SEMANTIC);

    CHECK(unlink(path) == 0);
    memx_trace_destroy(&trace);
}

static void
test_names(void) {
    CHECK(strcmp(memx_trace_operation_name(MEMX_TRACE_INSERT), "INSERT") == 0);
    CHECK(strcmp(memx_trace_operation_name(MEMX_TRACE_LOOKUP), "LOOKUP") == 0);
    CHECK(strcmp(memx_trace_operation_name(MEMX_TRACE_RETIRE), "RETIRE") == 0);
    CHECK(strcmp(memx_trace_operation_name(MEMX_TRACE_REMOVE), "REMOVE") == 0);
    CHECK(strcmp(memx_trace_operation_name(MEMX_TRACE_QUIESCENT),
        "QUIESCENT") == 0);
    CHECK(strcmp(memx_trace_operation_name((memx_trace_operation_t)99),
        "UNKNOWN") == 0);
    CHECK(strcmp(memx_trace_status_string(MEMX_TRACE_OK), "ok") == 0);
    CHECK(strcmp(memx_trace_status_string((memx_trace_status_t)99),
        "unknown") == 0);
}

int
main(void) {
    test_init_destroy();
    test_append_growth();
    test_validation();
    test_stats();
    test_lifecycle_validation();
    test_lifecycle_reuse_after_remove();
    test_round_trip();
    test_parse_errors();
    test_names();
    if (failures != 0U) {
        fprintf(stderr, "%u of %u trace checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }
    printf("all %u trace checks passed\n", checks);
    return EXIT_SUCCESS;
}
