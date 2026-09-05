#ifndef MEMX_BENCH_TRACE_H
#define MEMX_BENCH_TRACE_H

#include "memx/memx.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MEMX_TRACE_FORMAT_VERSION 1U

typedef enum memx_trace_operation {
    MEMX_TRACE_INSERT = 0,
    MEMX_TRACE_LOOKUP = 1,
    MEMX_TRACE_RETIRE = 2,
    MEMX_TRACE_REMOVE = 3,
    MEMX_TRACE_QUIESCENT = 4
} memx_trace_operation_t;

typedef struct memx_trace_event {
    uint64_t sequence;
    uint32_t thread_id;
    memx_trace_operation_t operation;
    uintptr_t address;
    size_t size;
    memx_handle_t handle;
} memx_trace_event_t;

typedef struct memx_trace {
    unsigned version;
    unsigned region_shift;
    unsigned granule_shift;
    memx_trace_event_t *events;
    size_t event_count;
    size_t event_capacity;
} memx_trace_t;

typedef struct memx_trace_stats {
    size_t inserts;
    size_t lookups;
    size_t retires;
    size_t removes;
    size_t quiescent_events;
    size_t distinct_threads;
    uintptr_t minimum_address;
    uintptr_t maximum_address_exclusive;
    size_t peak_live_bytes;
    size_t final_live_bytes;
} memx_trace_stats_t;

typedef enum memx_trace_status {
    MEMX_TRACE_OK = 0,
    MEMX_TRACE_ERROR_ARGUMENT,
    MEMX_TRACE_ERROR_IO,
    MEMX_TRACE_ERROR_FORMAT,
    MEMX_TRACE_ERROR_VERSION,
    MEMX_TRACE_ERROR_MEMORY,
    MEMX_TRACE_ERROR_SEMANTIC
} memx_trace_status_t;

void memx_trace_init(memx_trace_t *trace);
void memx_trace_destroy(memx_trace_t *trace);

memx_trace_status_t memx_trace_append(
    memx_trace_t *trace,
    const memx_trace_event_t *event);

memx_trace_status_t memx_trace_read_file(
    const char *path,
    memx_trace_t *out_trace,
    size_t *out_error_line);

memx_trace_status_t memx_trace_write_file(
    const char *path,
    const memx_trace_t *trace);

memx_trace_status_t memx_trace_validate(
    const memx_trace_t *trace,
    size_t *out_error_event);

/*
 * Replays allocation state in an independent hash-map oracle.  This rejects
 * overlaps, incorrect range handles, duplicate retirements, and lookup
 * expectations that disagree with the current mapping.  Retired mappings
 * remain lookup-visible until REMOVE.
 */
memx_trace_status_t memx_trace_validate_lifecycle(
    const memx_trace_t *trace,
    size_t *out_error_event);

memx_trace_status_t memx_trace_calculate_stats(
    const memx_trace_t *trace,
    memx_trace_stats_t *out_stats);

const char *memx_trace_operation_name(memx_trace_operation_t operation);
const char *memx_trace_status_string(memx_trace_status_t status);

#endif
