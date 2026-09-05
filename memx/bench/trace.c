#if !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "trace.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TRACE_LINE_CAPACITY 1024U
#define TRACE_INITIAL_EVENTS 1024U
#define TRACE_MAX_LIFECYCLE_GRANULES ((size_t)1U << 26U)

typedef enum trace_slot_state {
    TRACE_SLOT_EMPTY = 0,
    TRACE_SLOT_LIVE = 1,
    TRACE_SLOT_TOMBSTONE = 2
} trace_slot_state_t;

typedef struct trace_lifecycle_slot {
    uintptr_t granule;
    memx_handle_t handle;
    trace_slot_state_t state;
    bool retired;
} trace_lifecycle_slot_t;

typedef struct trace_lifecycle_map {
    trace_lifecycle_slot_t *slots;
    size_t capacity;
    size_t live_count;
} trace_lifecycle_map_t;

static char *
trim_left(char *text) {
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text += 1;
    }
    return text;
}

static void
trim_right(char *text) {
    size_t length = strlen(text);
    while (length > 0U && isspace((unsigned char)text[length - 1U])) {
        text[length - 1U] = '\0';
        length -= 1U;
    }
}

static int
parse_u64(const char *text, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }
    *out = (uint64_t)value;
    return 1;
}

static int
parse_size_value(const char *text, size_t *out) {
    uint64_t value;
    if (!parse_u64(text, &value) || value > SIZE_MAX) {
        return 0;
    }
    *out = (size_t)value;
    return 1;
}

static int
parse_uintptr(const char *text, uintptr_t *out) {
    uint64_t value;
    if (!parse_u64(text, &value) || value > UINTPTR_MAX) {
        return 0;
    }
    *out = (uintptr_t)value;
    return 1;
}

static int
parse_unsigned_value(const char *text, unsigned *out) {
    uint64_t value;
    if (!parse_u64(text, &value) || value > UINT_MAX) {
        return 0;
    }
    *out = (unsigned)value;
    return 1;
}

static int
parse_operation(const char *text, memx_trace_operation_t *out) {
    if (strcmp(text, "INSERT") == 0) {
        *out = MEMX_TRACE_INSERT;
    } else if (strcmp(text, "LOOKUP") == 0) {
        *out = MEMX_TRACE_LOOKUP;
    } else if (strcmp(text, "RETIRE") == 0) {
        *out = MEMX_TRACE_RETIRE;
    } else if (strcmp(text, "REMOVE") == 0) {
        *out = MEMX_TRACE_REMOVE;
    } else if (strcmp(text, "QUIESCENT") == 0) {
        *out = MEMX_TRACE_QUIESCENT;
    } else {
        return 0;
    }
    return 1;
}

void
memx_trace_init(memx_trace_t *trace) {
    if (trace == NULL) {
        return;
    }
    memset(trace, 0, sizeof(*trace));
    trace->version = MEMX_TRACE_FORMAT_VERSION;
    trace->region_shift = 21U;
    trace->granule_shift = 12U;
}

void
memx_trace_destroy(memx_trace_t *trace) {
    if (trace == NULL) {
        return;
    }
    free(trace->events);
    memx_trace_init(trace);
}

memx_trace_status_t
memx_trace_append(memx_trace_t *trace, const memx_trace_event_t *event) {
    memx_trace_event_t *replacement;
    size_t capacity;
    if (trace == NULL || event == NULL) {
        return MEMX_TRACE_ERROR_ARGUMENT;
    }
    if (trace->event_count == trace->event_capacity) {
        capacity = trace->event_capacity == 0U
            ? TRACE_INITIAL_EVENTS : trace->event_capacity * 2U;
        if (capacity < trace->event_capacity
            || capacity > SIZE_MAX / sizeof(*replacement)) {
            return MEMX_TRACE_ERROR_MEMORY;
        }
        replacement = realloc(trace->events, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            return MEMX_TRACE_ERROR_MEMORY;
        }
        trace->events = replacement;
        trace->event_capacity = capacity;
    }
    trace->events[trace->event_count++] = *event;
    return MEMX_TRACE_OK;
}

static size_t
split_csv(char *line, char **fields, size_t capacity) {
    size_t count = 0U;
    char *cursor = line;
    while (count < capacity) {
        char *comma;
        fields[count++] = trim_left(cursor);
        comma = strchr(cursor, ',');
        if (comma == NULL) {
            break;
        }
        *comma = '\0';
        trim_right(cursor);
        cursor = comma + 1;
    }
    if (count > 0U) {
        trim_right(fields[count - 1U]);
    }
    return count;
}

static memx_trace_status_t
parse_header_line(memx_trace_t *trace, char *line) {
    char *equals = strchr(line, '=');
    char *name;
    char *value;
    unsigned parsed;
    if (equals == NULL) {
        return MEMX_TRACE_ERROR_FORMAT;
    }
    *equals = '\0';
    name = trim_left(line);
    trim_right(name);
    value = trim_left(equals + 1);
    trim_right(value);
    if (!parse_unsigned_value(value, &parsed)) {
        return MEMX_TRACE_ERROR_FORMAT;
    }
    if (strcmp(name, "version") == 0) {
        trace->version = parsed;
    } else if (strcmp(name, "region_shift") == 0) {
        trace->region_shift = parsed;
    } else if (strcmp(name, "granule_shift") == 0) {
        trace->granule_shift = parsed;
    } else {
        return MEMX_TRACE_ERROR_FORMAT;
    }
    return MEMX_TRACE_OK;
}

static memx_trace_status_t
parse_event_line(memx_trace_t *trace, char *line) {
    char *fields[6];
    size_t count = split_csv(line, fields, 6U);
    memx_trace_event_t event;
    uint64_t thread;
    if (count != 6U
        || !parse_u64(fields[0], &event.sequence)
        || !parse_u64(fields[1], &thread)
        || thread > UINT32_MAX
        || !parse_operation(fields[2], &event.operation)
        || !parse_uintptr(fields[3], &event.address)
        || !parse_size_value(fields[4], &event.size)
        || !parse_uintptr(fields[5], &event.handle)) {
        return MEMX_TRACE_ERROR_FORMAT;
    }
    event.thread_id = (uint32_t)thread;
    return memx_trace_append(trace, &event);
}

memx_trace_status_t
memx_trace_read_file(
    const char *path,
    memx_trace_t *out_trace,
    size_t *out_error_line) {
    FILE *input;
    char line[TRACE_LINE_CAPACITY];
    size_t line_number = 0U;
    int saw_magic = 0;
    int saw_columns = 0;
    memx_trace_t trace;
    memx_trace_status_t status = MEMX_TRACE_OK;

    if (path == NULL || out_trace == NULL) {
        return MEMX_TRACE_ERROR_ARGUMENT;
    }
    if (out_error_line != NULL) {
        *out_error_line = 0U;
    }
    input = fopen(path, "r");
    if (input == NULL) {
        return MEMX_TRACE_ERROR_IO;
    }
    memx_trace_init(&trace);
    while (fgets(line, sizeof(line), input) != NULL) {
        char *content;
        line_number += 1U;
        if (strchr(line, '\n') == NULL && !feof(input)) {
            status = MEMX_TRACE_ERROR_FORMAT;
            break;
        }
        trim_right(line);
        content = trim_left(line);
        if (*content == '\0' || *content == '#') {
            continue;
        }
        if (!saw_magic) {
            if (strcmp(content, "MEMX_TRACE") != 0) {
                status = MEMX_TRACE_ERROR_FORMAT;
                break;
            }
            saw_magic = 1;
            continue;
        }
        if (!saw_columns && strchr(content, '=') != NULL) {
            status = parse_header_line(&trace, content);
            if (status != MEMX_TRACE_OK) {
                break;
            }
            continue;
        }
        if (!saw_columns) {
            if (strcmp(content,
                    "sequence,thread,operation,address,size,handle") != 0) {
                status = MEMX_TRACE_ERROR_FORMAT;
                break;
            }
            saw_columns = 1;
            continue;
        }
        status = parse_event_line(&trace, content);
        if (status != MEMX_TRACE_OK) {
            break;
        }
    }
    if (ferror(input)) {
        status = MEMX_TRACE_ERROR_IO;
    }
    if (fclose(input) != 0 && status == MEMX_TRACE_OK) {
        status = MEMX_TRACE_ERROR_IO;
    }
    if (status == MEMX_TRACE_OK && (!saw_magic || !saw_columns)) {
        status = MEMX_TRACE_ERROR_FORMAT;
    }
    if (status == MEMX_TRACE_OK && trace.version != MEMX_TRACE_FORMAT_VERSION) {
        status = MEMX_TRACE_ERROR_VERSION;
    }
    if (status == MEMX_TRACE_OK) {
        size_t error_event = 0U;
        status = memx_trace_validate_lifecycle(&trace, &error_event);
        if (status != MEMX_TRACE_OK && out_error_line != NULL) {
            *out_error_line = error_event + 1U;
        }
    } else if (out_error_line != NULL) {
        *out_error_line = line_number;
    }
    if (status != MEMX_TRACE_OK) {
        memx_trace_destroy(&trace);
        return status;
    }
    memx_trace_destroy(out_trace);
    *out_trace = trace;
    return MEMX_TRACE_OK;
}

memx_trace_status_t
memx_trace_write_file(const char *path, const memx_trace_t *trace) {
    FILE *output;
    size_t i;
    memx_trace_status_t status;
    if (path == NULL || trace == NULL) {
        return MEMX_TRACE_ERROR_ARGUMENT;
    }
    status = memx_trace_validate_lifecycle(trace, NULL);
    if (status != MEMX_TRACE_OK) {
        return status;
    }
    output = fopen(path, "w");
    if (output == NULL) {
        return MEMX_TRACE_ERROR_IO;
    }
    if (fprintf(output,
            "MEMX_TRACE\nversion=%u\nregion_shift=%u\n"
            "granule_shift=%u\n"
            "sequence,thread,operation,address,size,handle\n",
            trace->version, trace->region_shift, trace->granule_shift) < 0) {
        fclose(output);
        return MEMX_TRACE_ERROR_IO;
    }
    for (i = 0U; i < trace->event_count; ++i) {
        const memx_trace_event_t *event = &trace->events[i];
        if (fprintf(output, "%" PRIu64 ",%" PRIu32 ",%s,0x%" PRIxPTR
                ",%zu,0x%" PRIxPTR "\n",
                event->sequence, event->thread_id,
                memx_trace_operation_name(event->operation), event->address,
                event->size, (uintptr_t)event->handle) < 0) {
            fclose(output);
            return MEMX_TRACE_ERROR_IO;
        }
    }
    return fclose(output) == 0 ? MEMX_TRACE_OK : MEMX_TRACE_ERROR_IO;
}

memx_trace_status_t
memx_trace_validate(const memx_trace_t *trace, size_t *out_error_event) {
    const unsigned address_bits = (unsigned)(sizeof(uintptr_t) * CHAR_BIT);
    const unsigned size_bits = (unsigned)(sizeof(size_t) * CHAR_BIT);
    uintptr_t granule_mask;
    size_t i;
    uint64_t previous_sequence = 0U;
    if (out_error_event != NULL) {
        *out_error_event = 0U;
    }
    if (trace == NULL || trace->version != MEMX_TRACE_FORMAT_VERSION
        || trace->region_shift >= address_bits
        || trace->region_shift >= size_bits
        || trace->granule_shift >= address_bits
        || trace->granule_shift >= size_bits
        || trace->region_shift < trace->granule_shift) {
        return MEMX_TRACE_ERROR_SEMANTIC;
    }
    granule_mask = (((uintptr_t)1U) << trace->granule_shift) - 1U;
    for (i = 0U; i < trace->event_count; ++i) {
        const memx_trace_event_t *event = &trace->events[i];
        int range_operation = event->operation == MEMX_TRACE_INSERT
            || event->operation == MEMX_TRACE_RETIRE
            || event->operation == MEMX_TRACE_REMOVE;
        if ((i != 0U && event->sequence <= previous_sequence)
            || event->operation > MEMX_TRACE_QUIESCENT
            || (range_operation && (event->size == 0U
                || (event->address & granule_mask) != 0U
                || (event->size & (size_t)granule_mask) != 0U
                || (uintmax_t)event->size
                    > (uintmax_t)(UINTPTR_MAX - event->address)))
            || ((event->operation == MEMX_TRACE_INSERT
                    || event->operation == MEMX_TRACE_REMOVE)
                && event->handle == MEMX_HANDLE_INVALID)
            || (event->operation == MEMX_TRACE_LOOKUP
                && (event->size != 0U || event->address == UINTPTR_MAX))
            || (event->operation == MEMX_TRACE_QUIESCENT
                && (event->address != 0U || event->size != 0U))) {
            if (out_error_event != NULL) {
                *out_error_event = i;
            }
            return MEMX_TRACE_ERROR_SEMANTIC;
        }
        previous_sequence = event->sequence;
    }
    return MEMX_TRACE_OK;
}

static uintptr_t
trace_hash_address(uintptr_t value) {
#if UINTPTR_MAX == UINT64_MAX
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31U;
#else
    value ^= value >> 16U;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15U;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16U;
#endif
    return value;
}

static trace_lifecycle_slot_t *
trace_lifecycle_find(
    trace_lifecycle_map_t *map,
    uintptr_t granule,
    bool for_insert) {
    size_t position = (size_t)(trace_hash_address(granule)
        & (uintptr_t)(map->capacity - 1U));
    trace_lifecycle_slot_t *first_tombstone = NULL;
    size_t probes;
    for (probes = 0U; probes < map->capacity; ++probes) {
        trace_lifecycle_slot_t *slot = &map->slots[position];
        if (slot->state == TRACE_SLOT_EMPTY) {
            return for_insert && first_tombstone != NULL
                ? first_tombstone : slot;
        }
        if (slot->state == TRACE_SLOT_TOMBSTONE) {
            if (for_insert && first_tombstone == NULL) {
                first_tombstone = slot;
            }
        } else if (slot->granule == granule) {
            return slot;
        }
        position = (position + 1U) & (map->capacity - 1U);
    }
    return first_tombstone;
}

static memx_trace_status_t
trace_lifecycle_capacity(
    const memx_trace_t *trace,
    size_t *out_capacity) {
    size_t possible_live = 0U;
    size_t capacity = 8U;
    size_t i;
    const size_t granule_size = (size_t)1U << trace->granule_shift;
    for (i = 0U; i < trace->event_count; ++i) {
        const memx_trace_event_t *event = &trace->events[i];
        if (event->operation == MEMX_TRACE_INSERT) {
            size_t count = event->size / granule_size;
            if (count > TRACE_MAX_LIFECYCLE_GRANULES - possible_live) {
                return MEMX_TRACE_ERROR_MEMORY;
            }
            possible_live += count;
        }
    }
    while (capacity < possible_live * 2U) {
        if (capacity > SIZE_MAX / 2U) {
            return MEMX_TRACE_ERROR_MEMORY;
        }
        capacity *= 2U;
    }
    *out_capacity = capacity;
    return MEMX_TRACE_OK;
}

static bool
trace_lifecycle_range_matches(
    trace_lifecycle_map_t *map,
    const memx_trace_event_t *event,
    uintptr_t granule_size,
    bool require_active) {
    uintptr_t address;
    uintptr_t end = event->address + (uintptr_t)event->size;
    for (address = event->address; address < end; address += granule_size) {
        trace_lifecycle_slot_t *slot = trace_lifecycle_find(
            map, address, false);
        if (slot == NULL || slot->state != TRACE_SLOT_LIVE
            || slot->handle != event->handle
            || (require_active && slot->retired)) {
            return false;
        }
    }
    return true;
}

static bool
trace_lifecycle_insert(
    trace_lifecycle_map_t *map,
    const memx_trace_event_t *event,
    uintptr_t granule_size) {
    uintptr_t address;
    uintptr_t end = event->address + (uintptr_t)event->size;
    for (address = event->address; address < end; address += granule_size) {
        trace_lifecycle_slot_t *slot = trace_lifecycle_find(
            map, address, false);
        if (slot != NULL && slot->state == TRACE_SLOT_LIVE) {
            return false;
        }
    }
    for (address = event->address; address < end; address += granule_size) {
        trace_lifecycle_slot_t *slot = trace_lifecycle_find(
            map, address, true);
        if (slot == NULL) {
            return false;
        }
        slot->granule = address;
        slot->handle = event->handle;
        slot->state = TRACE_SLOT_LIVE;
        slot->retired = false;
        map->live_count += 1U;
    }
    return true;
}

static bool
trace_lifecycle_retire(
    trace_lifecycle_map_t *map,
    const memx_trace_event_t *event,
    uintptr_t granule_size) {
    uintptr_t address;
    uintptr_t end = event->address + (uintptr_t)event->size;
    for (address = event->address; address < end; address += granule_size) {
        trace_lifecycle_slot_t *slot = trace_lifecycle_find(
            map, address, false);
        if (slot == NULL) {
            return false;
        }
        slot->retired = true;
    }
    return true;
}

static bool
trace_lifecycle_remove(
    trace_lifecycle_map_t *map,
    const memx_trace_event_t *event,
    uintptr_t granule_size) {
    uintptr_t address;
    uintptr_t end = event->address + (uintptr_t)event->size;
    for (address = event->address; address < end; address += granule_size) {
        trace_lifecycle_slot_t *slot = trace_lifecycle_find(
            map, address, false);
        if (slot == NULL || map->live_count == 0U) {
            return false;
        }
        slot->state = TRACE_SLOT_TOMBSTONE;
        slot->retired = false;
        map->live_count -= 1U;
    }
    return true;
}

static bool
trace_lifecycle_lookup_matches(
    trace_lifecycle_map_t *map,
    const memx_trace_event_t *event,
    uintptr_t granule_mask) {
    uintptr_t granule = event->address & ~granule_mask;
    trace_lifecycle_slot_t *slot = trace_lifecycle_find(map, granule, false);
    memx_handle_t actual = slot == NULL || slot->state != TRACE_SLOT_LIVE
        ? MEMX_HANDLE_INVALID : slot->handle;
    return actual == event->handle;
}

memx_trace_status_t
memx_trace_validate_lifecycle(
    const memx_trace_t *trace,
    size_t *out_error_event) {
    trace_lifecycle_map_t map;
    memx_trace_status_t status;
    uintptr_t granule_size;
    uintptr_t granule_mask;
    size_t i;
    if (out_error_event != NULL) {
        *out_error_event = 0U;
    }
    status = memx_trace_validate(trace, out_error_event);
    if (status != MEMX_TRACE_OK) {
        return status;
    }
    memset(&map, 0, sizeof(map));
    status = trace_lifecycle_capacity(trace, &map.capacity);
    if (status != MEMX_TRACE_OK) {
        return status;
    }
    map.slots = calloc(map.capacity, sizeof(*map.slots));
    if (map.slots == NULL) {
        return MEMX_TRACE_ERROR_MEMORY;
    }
    granule_size = ((uintptr_t)1U) << trace->granule_shift;
    granule_mask = granule_size - 1U;
    for (i = 0U; i < trace->event_count; ++i) {
        const memx_trace_event_t *event = &trace->events[i];
        bool valid = true;
        switch (event->operation) {
            case MEMX_TRACE_INSERT:
                valid = trace_lifecycle_insert(&map, event, granule_size);
                break;
            case MEMX_TRACE_LOOKUP:
                valid = trace_lifecycle_lookup_matches(
                    &map, event, granule_mask);
                break;
            case MEMX_TRACE_RETIRE:
                valid = trace_lifecycle_range_matches(
                    &map, event, granule_size, true);
                if (valid) {
                    valid = trace_lifecycle_retire(
                        &map, event, granule_size);
                }
                break;
            case MEMX_TRACE_REMOVE:
                valid = trace_lifecycle_range_matches(
                    &map, event, granule_size, false);
                if (valid) {
                    valid = trace_lifecycle_remove(
                        &map, event, granule_size);
                }
                break;
            case MEMX_TRACE_QUIESCENT:
                break;
            default:
                valid = false;
                break;
        }
        if (!valid) {
            if (out_error_event != NULL) {
                *out_error_event = i;
            }
            free(map.slots);
            return MEMX_TRACE_ERROR_SEMANTIC;
        }
    }
    free(map.slots);
    return MEMX_TRACE_OK;
}

memx_trace_status_t
memx_trace_calculate_stats(
    const memx_trace_t *trace, memx_trace_stats_t *out_stats) {
    uint32_t *threads;
    size_t thread_count = 0U;
    size_t i;
    size_t live_bytes = 0U;
    memx_trace_status_t status;
    if (trace == NULL || out_stats == NULL) {
        return MEMX_TRACE_ERROR_ARGUMENT;
    }
    status = memx_trace_validate_lifecycle(trace, NULL);
    if (status != MEMX_TRACE_OK) {
        return status;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->minimum_address = UINTPTR_MAX;
    threads = trace->event_count == 0U ? NULL
        : malloc(trace->event_count * sizeof(*threads));
    if (trace->event_count != 0U && threads == NULL) {
        return MEMX_TRACE_ERROR_MEMORY;
    }
    for (i = 0U; i < trace->event_count; ++i) {
        const memx_trace_event_t *event = &trace->events[i];
        size_t t;
        int seen = 0;
        for (t = 0U; t < thread_count; ++t) {
            if (threads[t] == event->thread_id) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            threads[thread_count++] = event->thread_id;
        }
        if (event->operation != MEMX_TRACE_QUIESCENT) {
            uintptr_t end = event->operation == MEMX_TRACE_LOOKUP
                ? event->address + 1U
                : event->address + (uintptr_t)event->size;
            if (event->address < out_stats->minimum_address) {
                out_stats->minimum_address = event->address;
            }
            if (end > out_stats->maximum_address_exclusive) {
                out_stats->maximum_address_exclusive = end;
            }
        }
        switch (event->operation) {
            case MEMX_TRACE_INSERT:
                out_stats->inserts += 1U;
                live_bytes += event->size;
                if (live_bytes > out_stats->peak_live_bytes) {
                    out_stats->peak_live_bytes = live_bytes;
                }
                break;
            case MEMX_TRACE_LOOKUP:
                out_stats->lookups += 1U;
                break;
            case MEMX_TRACE_RETIRE:
                out_stats->retires += 1U;
                break;
            case MEMX_TRACE_REMOVE:
                out_stats->removes += 1U;
                live_bytes = event->size > live_bytes
                    ? 0U : live_bytes - event->size;
                break;
            case MEMX_TRACE_QUIESCENT:
                out_stats->quiescent_events += 1U;
                break;
            default:
                free(threads);
                return MEMX_TRACE_ERROR_SEMANTIC;
        }
    }
    if (out_stats->minimum_address == UINTPTR_MAX) {
        out_stats->minimum_address = 0U;
    }
    out_stats->distinct_threads = thread_count;
    out_stats->final_live_bytes = live_bytes;
    free(threads);
    return MEMX_TRACE_OK;
}

const char *
memx_trace_operation_name(memx_trace_operation_t operation) {
    switch (operation) {
        case MEMX_TRACE_INSERT: return "INSERT";
        case MEMX_TRACE_LOOKUP: return "LOOKUP";
        case MEMX_TRACE_RETIRE: return "RETIRE";
        case MEMX_TRACE_REMOVE: return "REMOVE";
        case MEMX_TRACE_QUIESCENT: return "QUIESCENT";
        default: return "UNKNOWN";
    }
}

const char *
memx_trace_status_string(memx_trace_status_t status) {
    switch (status) {
        case MEMX_TRACE_OK: return "ok";
        case MEMX_TRACE_ERROR_ARGUMENT: return "invalid argument";
        case MEMX_TRACE_ERROR_IO: return "I/O error";
        case MEMX_TRACE_ERROR_FORMAT: return "format error";
        case MEMX_TRACE_ERROR_VERSION: return "unsupported version";
        case MEMX_TRACE_ERROR_MEMORY: return "out of memory";
        case MEMX_TRACE_ERROR_SEMANTIC: return "semantic error";
        default: return "unknown";
    }
}
