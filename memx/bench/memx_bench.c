#define _GNU_SOURCE

#include "baselines.h"
#include "memx/memx.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum workload_kind {
    WORKLOAD_UNIFORM,
    WORKLOAD_MIXED,
    WORKLOAD_RANDOM
} workload_kind_t;

typedef struct options {
    size_t regions;
    size_t lookups;
    unsigned dense_percent;
    unsigned region_shift;
    unsigned granule_shift;
    uint64_t seed;
    workload_kind_t workload;
} options_t;

typedef struct dataset {
    uintptr_t base;
    size_t managed_size;
    size_t granule_count;
    size_t granules_per_region;
    memx_handle_t *truth;
    uintptr_t *queries;
} dataset_t;

typedef memx_handle_t (*lookup_fn)(void *context, uintptr_t address);

typedef struct benchmark_result {
    const char *name;
    double nanoseconds_per_lookup;
    size_t metadata_bytes;
    uint64_t checksum;
} benchmark_result_t;

typedef struct overlay_residency {
    size_t rss_bytes;
    size_t private_bytes;
    size_t shared_bytes;
} overlay_residency_t;

static overlay_residency_t
overlay_residency_query(const void *address, size_t bytes) {
    overlay_residency_t result = {0U, 0U, 0U};
#if defined(__linux__)
    FILE *maps = fopen("/proc/self/smaps", "r");
    char line[512];
    const uintptr_t wanted_start = (uintptr_t)address;
    const uintptr_t wanted_end = wanted_start + bytes;
    int selected = 0;
    if (maps == NULL) {
        return result;
    }
    while (fgets(line, sizeof(line), maps) != NULL) {
        uintptr_t start;
        uintptr_t end;
        size_t kibibytes;
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &start, &end) == 2) {
            if (selected) {
                break;
            }
            selected = start == wanted_start && end == wanted_end;
            continue;
        }
        if (!selected) {
            continue;
        }
        if (sscanf(line, "Rss: %zu kB", &kibibytes) == 1) {
            result.rss_bytes = kibibytes * 1024U;
        } else if (sscanf(line, "Private_Clean: %zu kB", &kibibytes) == 1
            || sscanf(line, "Private_Dirty: %zu kB", &kibibytes) == 1) {
            result.private_bytes += kibibytes * 1024U;
        } else if (sscanf(line, "Shared_Clean: %zu kB", &kibibytes) == 1
            || sscanf(line, "Shared_Dirty: %zu kB", &kibibytes) == 1) {
            result.shared_bytes += kibibytes * 1024U;
        }
    }
    (void)fclose(maps);
#else
    (void)address;
    (void)bytes;
#endif
    return result;
}

static uint64_t
rng_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static uint64_t
time_nanoseconds(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)time.tv_sec * UINT64_C(1000000000)
        + (uint64_t)time.tv_nsec;
}

static int
parse_size(const char *text, size_t *out) {
    char *end = NULL;
    unsigned long long value;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > SIZE_MAX) {
        return 0;
    }
    *out = (size_t)value;
    return 1;
}

static int
parse_unsigned(const char *text, unsigned *out) {
    size_t value;
    if (!parse_size(text, &value) || value > UINT32_MAX) {
        return 0;
    }
    *out = (unsigned)value;
    return 1;
}

static int
parse_seed(const char *text, uint64_t *out) {
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

static void
usage(const char *program) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --regions N          managed regions (default 256)\n"
        "  --lookups N          lookups per structure (default 10000000)\n"
        "  --workload NAME      uniform, mixed, or random (default mixed)\n"
        "  --dense-percent N    dense regions in mixed workload (default 25)\n"
        "  --region-shift N     region exponent (default 21)\n"
        "  --granule-shift N    granule exponent (default 12)\n"
        "  --seed N             deterministic RNG seed\n"
        "  --help               show this help\n",
        program);
}

static int
parse_workload(const char *text, workload_kind_t *out) {
    if (strcmp(text, "uniform") == 0) {
        *out = WORKLOAD_UNIFORM;
    } else if (strcmp(text, "mixed") == 0) {
        *out = WORKLOAD_MIXED;
    } else if (strcmp(text, "random") == 0) {
        *out = WORKLOAD_RANDOM;
    } else {
        return 0;
    }
    return 1;
}

static int
parse_options(int argc, char **argv, options_t *options) {
    int i;
    options->regions = 256U;
    options->lookups = 10000000U;
    options->dense_percent = 25U;
    options->region_shift = 21U;
    options->granule_shift = 12U;
    options->seed = UINT64_C(0x6d656d782d62656e);
    options->workload = WORKLOAD_MIXED;

    for (i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (strcmp(argument, "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value after %s\n", argument);
            return -1;
        }
        if (strcmp(argument, "--regions") == 0) {
            if (!parse_size(argv[++i], &options->regions)) return -1;
        } else if (strcmp(argument, "--lookups") == 0) {
            if (!parse_size(argv[++i], &options->lookups)) return -1;
        } else if (strcmp(argument, "--dense-percent") == 0) {
            if (!parse_unsigned(argv[++i], &options->dense_percent)) return -1;
        } else if (strcmp(argument, "--region-shift") == 0) {
            if (!parse_unsigned(argv[++i], &options->region_shift)) return -1;
        } else if (strcmp(argument, "--granule-shift") == 0) {
            if (!parse_unsigned(argv[++i], &options->granule_shift)) return -1;
        } else if (strcmp(argument, "--seed") == 0) {
            if (!parse_seed(argv[++i], &options->seed)) return -1;
        } else if (strcmp(argument, "--workload") == 0) {
            if (!parse_workload(argv[++i], &options->workload)) return -1;
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            return -1;
        }
    }

    if (options->regions == 0U || options->lookups == 0U
        || options->dense_percent > 100U
        || options->region_shift >= sizeof(uintptr_t) * 8U
        || options->granule_shift >= sizeof(uintptr_t) * 8U
        || options->region_shift < options->granule_shift
        || options->region_shift - options->granule_shift > 20U) {
        fprintf(stderr, "invalid benchmark configuration\n");
        return -1;
    }
    return 1;
}

static void
dataset_destroy(dataset_t *dataset) {
    free(dataset->truth);
    free(dataset->queries);
    memset(dataset, 0, sizeof(*dataset));
}

static int
dataset_create(const options_t *options, dataset_t *dataset) {
    uint64_t random_state = options->seed;
    size_t region;
    size_t granule;
    size_t query;
    uintptr_t region_size = ((uintptr_t)1U) << options->region_shift;
    uintptr_t granule_size = ((uintptr_t)1U) << options->granule_shift;

    memset(dataset, 0, sizeof(*dataset));
    dataset->base = region_size * 16U;
    dataset->granules_per_region = (size_t)1U
        << (options->region_shift - options->granule_shift);
    if (options->regions > SIZE_MAX / (size_t)region_size
        || options->regions > SIZE_MAX / dataset->granules_per_region) {
        return 0;
    }
    dataset->managed_size = options->regions * (size_t)region_size;
    dataset->granule_count = options->regions * dataset->granules_per_region;
    dataset->truth = malloc(dataset->granule_count * sizeof(*dataset->truth));
    dataset->queries = malloc(options->lookups * sizeof(*dataset->queries));
    if (dataset->truth == NULL || dataset->queries == NULL) {
        dataset_destroy(dataset);
        return 0;
    }

    for (region = 0U; region < options->regions; ++region) {
        int dense = options->workload == WORKLOAD_RANDOM;
        memx_handle_t uniform_handle = (memx_handle_t)(region + 1U);
        if (options->workload == WORKLOAD_MIXED) {
            dense = (rng_next(&random_state) % 100U) < options->dense_percent;
        }
        for (granule = 0U;
             granule < dataset->granules_per_region;
             ++granule) {
            size_t index = region * dataset->granules_per_region + granule;
            memx_handle_t value = dense
                ? (memx_handle_t)(1U + (rng_next(&random_state) & 0xfffffU))
                : uniform_handle;
            if (value == MEMX_HANDLE_INVALID) {
                value -= 1U;
            }
            dataset->truth[index] = value;
        }
    }

    for (query = 0U; query < options->lookups; ++query) {
        size_t index = (size_t)(rng_next(&random_state)
            % dataset->granule_count);
        uintptr_t within = (uintptr_t)(rng_next(&random_state)
            & (granule_size - 1U));
        dataset->queries[query] = dataset->base
            + (uintptr_t)index * granule_size + within;
    }
    return 1;
}

static memx_handle_t
lookup_memx(void *context, uintptr_t address) {
    return memx_lookup_address(context, address);
}

static memx_handle_t
lookup_memx_bounded_fast(void *context, uintptr_t address) {
    return memx_bounded_lookup_assume_mapped(context, address);
}

/*
 * Gate-1 experiments for valid, fully mapped bounded views.  These deliberately
 * do not become public API until a density sweep proves that the extra unsafe
 * contract buys enough latency to justify it.
 */
static memx_handle_t
lookup_memx_bounded_trusted(void *context, uintptr_t address) {
    const memx_bounded_view_t *view = context;
    const uintptr_t relative = address - view->base_address;
    const size_t region_index = (size_t)(relative >> view->region_shift);
    const uintptr_t descriptor = view->descriptors[region_index];

    if ((descriptor & (uintptr_t)2U) == 0U) {
        return (memx_handle_t)(descriptor >> 2U);
    }
    {
        const size_t granule_index = (size_t)
            ((relative >> view->granule_shift)
             & (uintptr_t)(view->granules_per_region - 1U));
        return ((const memx_handle_t *)(descriptor & ~(uintptr_t)3U))
            [granule_index];
    }
}

typedef struct branchless_lookup_context {
    memx_bounded_view_t view;
    memx_handle_t dummy;
} branchless_lookup_context_t;

static memx_handle_t
lookup_memx_bounded_branchless(void *context, uintptr_t address) {
    const branchless_lookup_context_t *experiment = context;
    const memx_bounded_view_t *view = &experiment->view;
    const uintptr_t relative = address - view->base_address;
    const size_t region_index = (size_t)(relative >> view->region_shift);
    const size_t granule_index = (size_t)
        ((relative >> view->granule_shift)
         & (uintptr_t)(view->granules_per_region - 1U));
    const uintptr_t descriptor = view->descriptors[region_index];
    const uintptr_t dense_mask = (uintptr_t)0U
        - ((descriptor >> 1U) & (uintptr_t)1U);
    const uintptr_t selected_base =
        ((descriptor & ~(uintptr_t)3U) & dense_mask)
        | (((uintptr_t)&experiment->dummy) & ~dense_mask);
    const size_t selected_index = granule_index & (size_t)dense_mask;
    const memx_handle_t selected =
        ((const memx_handle_t *)selected_base)[selected_index];
    const memx_handle_t uniform = (memx_handle_t)(descriptor >> 2U);

    return (selected & (memx_handle_t)dense_mask)
        | (uniform & (memx_handle_t)~dense_mask);
}

static memx_handle_t
lookup_memx_overlay_fast(void *context, uintptr_t address) {
    return memx_overlay_lookup_trusted(context, address);
}

static memx_handle_t
lookup_memx_flat_fast(void *context, uintptr_t address) {
    return memx_flat_lookup_assume_mapped(context, address);
}

static memx_handle_t
lookup_flat(void *context, uintptr_t address) {
    return baseline_flat_lookup(context, address);
}

static memx_handle_t
lookup_flat_fast(void *context, uintptr_t address) {
    return baseline_flat_lookup_assume_mapped(context, address);
}

static memx_handle_t
lookup_two_level(void *context, uintptr_t address) {
    return baseline_two_level_lookup(context, address);
}

static memx_handle_t
lookup_radix(void *context, uintptr_t address) {
    return baseline_radix_lookup(context, address);
}

typedef struct cached_radix_context {
    baseline_radix_t *map;
    baseline_radix_cache_t cache;
} cached_radix_context_t;

static memx_handle_t
lookup_cached_radix(void *context, uintptr_t address) {
    cached_radix_context_t *cached = context;
    return baseline_radix_cached_lookup(
        cached->map, &cached->cache, address);
}

static benchmark_result_t
run_benchmark(
    const char *name,
    lookup_fn lookup,
    void *context,
    const dataset_t *dataset,
    size_t lookup_count,
    size_t metadata_bytes) {
    benchmark_result_t result;
    uint64_t begin;
    uint64_t end;
    uint64_t checksum = 0U;
    size_t i;

    for (i = 0U; i < lookup_count && i < 10000U; ++i) {
        checksum ^= (uint64_t)lookup(context, dataset->queries[i]);
    }
    begin = time_nanoseconds();
    for (i = 0U; i < lookup_count; ++i) {
        memx_handle_t value = lookup(context, dataset->queries[i]);
        checksum += (uint64_t)value;
        checksum ^= checksum << 7U;
    }
    end = time_nanoseconds();

    result.name = name;
    result.nanoseconds_per_lookup = (double)(end - begin)
        / (double)lookup_count;
    result.metadata_bytes = metadata_bytes;
    result.checksum = checksum;
    return result;
}

static int
populate_all(
    const options_t *options,
    const dataset_t *dataset,
    memx_index_t *bounded,
    memx_index_t *overlay,
    memx_index_t *sparse,
    baseline_flat_t *flat,
    baseline_two_level_t *two_level,
    baseline_radix_t *radix) {
    const uintptr_t granule_size = ((uintptr_t)1U) << options->granule_shift;
    size_t region;
    size_t granule;

    for (region = 0U; region < options->regions; ++region) {
        size_t first = region * dataset->granules_per_region;
        memx_handle_t first_value = dataset->truth[first];
        int uniform = 1;
        for (granule = 1U;
             granule < dataset->granules_per_region;
             ++granule) {
            if (dataset->truth[first + granule] != first_value) {
                uniform = 0;
                break;
            }
        }
        if (uniform) {
            uintptr_t address = dataset->base
                + (uintptr_t)first * granule_size;
            size_t bytes = dataset->granules_per_region * (size_t)granule_size;
            if (memx_insert(bounded, address, bytes, first_value) != MEMX_OK
                || memx_insert(overlay, address, bytes, first_value) != MEMX_OK
                || memx_insert(sparse, address, bytes, first_value) != MEMX_OK) {
                return 0;
            }
        } else {
            for (granule = 0U;
                 granule < dataset->granules_per_region;
                 ++granule) {
                size_t index = first + granule;
                uintptr_t address = dataset->base
                    + (uintptr_t)index * granule_size;
                memx_handle_t value = dataset->truth[index];
                if (memx_insert(bounded, address, (size_t)granule_size, value)
                        != MEMX_OK
                    || memx_insert(overlay, address,
                        (size_t)granule_size, value) != MEMX_OK
                    || memx_insert(sparse, address, (size_t)granule_size, value)
                        != MEMX_OK) {
                    return 0;
                }
            }
        }
    }

    for (granule = 0U; granule < dataset->granule_count; ++granule) {
        uintptr_t address = dataset->base
            + (uintptr_t)granule * granule_size;
        memx_handle_t value = dataset->truth[granule];
        if (!baseline_flat_set(flat, address, value)
            || !baseline_two_level_set(two_level, address, value)
            || !baseline_radix_set(radix, address, value)) {
            return 0;
        }
    }
    return 1;
}

static int
verify_all(
    const options_t *options,
    const dataset_t *dataset,
    const memx_index_t *bounded,
    const memx_index_t *overlay,
    const memx_index_t *sparse,
    const baseline_flat_t *flat,
    const baseline_two_level_t *two_level,
    const baseline_radix_t *radix) {
    const uintptr_t granule_size = ((uintptr_t)1U) << options->granule_shift;
    size_t i;
    for (i = 0U; i < dataset->granule_count; ++i) {
        uintptr_t address = dataset->base + (uintptr_t)i * granule_size;
        memx_handle_t expected = dataset->truth[i];
        if (memx_lookup_address(bounded, address) != expected
            || memx_lookup_address(overlay, address) != expected
            || memx_lookup_address(sparse, address) != expected
            || baseline_flat_lookup(flat, address) != expected
            || baseline_two_level_lookup(two_level, address) != expected
            || baseline_radix_lookup(radix, address) != expected) {
            fprintf(stderr, "verification failed at granule %zu\n", i);
            return 0;
        }
    }
    return 1;
}

static const char *
workload_name(workload_kind_t workload) {
    switch (workload) {
        case WORKLOAD_UNIFORM: return "uniform";
        case WORKLOAD_MIXED: return "mixed";
        case WORKLOAD_RANDOM: return "random";
        default: return "unknown";
    }
}

int
main(int argc, char **argv) {
    options_t options;
    dataset_t dataset;
    baseline_config_t baseline_config;
    memx_config_t bounded_config;
    memx_config_t sparse_config;
    memx_index_t *bounded = NULL;
    memx_index_t *overlay = NULL;
    memx_index_t *sparse = NULL;
    baseline_flat_t *flat = NULL;
    baseline_two_level_t *two_level = NULL;
    baseline_radix_t *radix = NULL;
    cached_radix_context_t cached_radix;
    memx_stats_t bounded_stats;
    memx_stats_t overlay_stats;
    memx_stats_t sparse_stats;
    memx_bounded_view_t bounded_view;
    branchless_lookup_context_t branchless_context;
    memx_overlay_view_t overlay_view;
    overlay_residency_t overlay_residency;
    memx_flat_view_t flat_view;
    benchmark_result_t results[13];
    size_t result_count = 0U;
    size_t i;
    int parsed = parse_options(argc, argv, &options);

    if (parsed <= 0) {
        return parsed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (!dataset_create(&options, &dataset)) {
        fprintf(stderr, "failed to create dataset\n");
        return EXIT_FAILURE;
    }

    baseline_config.base = dataset.base;
    baseline_config.managed_size = dataset.managed_size;
    baseline_config.granule_shift = options.granule_shift;
    flat = baseline_flat_create(&baseline_config);
    two_level = baseline_two_level_create(&baseline_config);
    radix = baseline_radix_create(&baseline_config);

    bounded_config = memx_config_default();
    bounded_config.directory_mode = MEMX_DIRECTORY_BOUNDED;
    bounded_config.region_shift = options.region_shift;
    bounded_config.granule_shift = options.granule_shift;
    bounded_config.base_address = dataset.base;
    bounded_config.managed_size = dataset.managed_size;

    sparse_config = bounded_config;
    sparse_config.directory_mode = MEMX_DIRECTORY_SPARSE;
    sparse_config.base_address = 0U;
    sparse_config.managed_size = 0U;
    sparse_config.address_bits = 48U;

    if (flat == NULL || two_level == NULL || radix == NULL
        || memx_index_create(&bounded_config, &bounded) != MEMX_OK
        || memx_index_create(&bounded_config, &overlay) != MEMX_OK
        || memx_index_create(&sparse_config, &sparse) != MEMX_OK
        || memx_index_bounded_view(bounded, &bounded_view) != MEMX_OK) {
        fprintf(stderr, "failed to create indexes\n");
        goto failure;
    }
    if (!populate_all(&options, &dataset, bounded, overlay, sparse,
            flat, two_level, radix)
        || !verify_all(&options, &dataset, bounded, overlay, sparse,
            flat, two_level, radix)) {
        goto failure;
    }

    memx_index_stats(bounded, &bounded_stats);
    memx_index_stats(sparse, &sparse_stats);
    branchless_context.view = bounded_view;
    branchless_context.dummy = MEMX_HANDLE_INVALID;
    if (memx_index_optimize_overlay(overlay) != MEMX_OK
        || memx_index_overlay_view(overlay, &overlay_view) != MEMX_OK) {
        fprintf(stderr, "failed to optimize dense overlay\n");
        goto failure;
    }
    memx_index_stats(overlay, &overlay_stats);
    baseline_radix_cache_init(&cached_radix.cache);
    cached_radix.map = radix;

    results[result_count++] = run_benchmark("flat", lookup_flat, flat,
        &dataset, options.lookups,
        baseline_flat_page_accounted_bytes(flat));
    results[result_count++] = run_benchmark("flat_assume_mapped",
        lookup_flat_fast, flat, &dataset, options.lookups,
        baseline_flat_page_accounted_bytes(flat));
    results[result_count++] = run_benchmark("two_level", lookup_two_level,
        two_level, &dataset, options.lookups,
        baseline_two_level_page_accounted_bytes(two_level));
    results[result_count++] = run_benchmark("radix", lookup_radix, radix,
        &dataset, options.lookups,
        baseline_radix_page_accounted_bytes(radix));
    results[result_count++] = run_benchmark("cached_radix",
        lookup_cached_radix, &cached_radix, &dataset, options.lookups,
        baseline_radix_page_accounted_bytes(radix)
            + sizeof(cached_radix.cache));
    results[result_count++] = run_benchmark("memx_bounded", lookup_memx,
        bounded, &dataset, options.lookups,
        bounded_stats.page_accounted_bytes);
    results[result_count++] = run_benchmark("memx_bounded_assume_mapped",
        lookup_memx_bounded_fast, &bounded_view, &dataset, options.lookups,
        bounded_stats.page_accounted_bytes);
    results[result_count++] = run_benchmark("memx_bounded_trusted",
        lookup_memx_bounded_trusted, &bounded_view, &dataset, options.lookups,
        bounded_stats.page_accounted_bytes);
    results[result_count++] = run_benchmark("memx_bounded_branchless",
        lookup_memx_bounded_branchless, &branchless_context, &dataset,
        options.lookups,
        bounded_stats.page_accounted_bytes + sizeof(memx_handle_t));
    results[result_count++] = run_benchmark("memx_overlay_checked",
        lookup_memx, overlay, &dataset, options.lookups,
        overlay_stats.page_accounted_bytes);
    results[result_count++] = run_benchmark("memx_overlay_trusted",
        lookup_memx_overlay_fast, &overlay_view, &dataset,
        options.lookups, overlay_stats.page_accounted_bytes);
    results[result_count++] = run_benchmark("memx_sparse", lookup_memx,
        sparse, &dataset, options.lookups,
        sparse_stats.page_accounted_bytes);
    if (memx_index_optimize(bounded) == MEMX_OK
        && memx_index_flat_view(bounded, &flat_view) == MEMX_OK) {
        memx_index_stats(bounded, &bounded_stats);
        results[result_count++] = run_benchmark("memx_flat_fallback",
            lookup_memx_flat_fast, &flat_view, &dataset, options.lookups,
            bounded_stats.page_accounted_bytes);
    }

    printf("# workload=%s regions=%zu lookups=%zu dense_percent=%u "
           "region_shift=%u granule_shift=%u seed=0x%" PRIx64 "\n",
        workload_name(options.workload), options.regions, options.lookups,
        options.dense_percent, options.region_shift, options.granule_shift,
        options.seed);
    overlay_residency = overlay_residency_query(
        overlay_view.dense_overlay,
        overlay_view.entry_count * sizeof(memx_handle_t));
    printf("# overlay_reserved_bytes=%zu overlay_rss_bytes=%zu "
           "overlay_private_bytes=%zu overlay_shared_bytes=%zu\n",
        overlay_stats.reserved_bytes, overlay_residency.rss_bytes,
        overlay_residency.private_bytes, overlay_residency.shared_bytes);
    printf("# memory_category=page_accounted_backing\n");
    printf("structure,ns_per_lookup,metadata_bytes,bytes_per_managed_mib,checksum\n");
    for (i = 0U; i < result_count; ++i) {
        double bytes_per_mib = (double)results[i].metadata_bytes
            / ((double)dataset.managed_size / (1024.0 * 1024.0));
        printf("%s,%.4f,%zu,%.2f,0x%" PRIx64 "\n",
            results[i].name, results[i].nanoseconds_per_lookup,
            results[i].metadata_bytes, bytes_per_mib, results[i].checksum);
    }

    baseline_flat_destroy(flat);
    baseline_two_level_destroy(two_level);
    baseline_radix_destroy(radix);
    memx_index_destroy(bounded);
    memx_index_destroy(overlay);
    memx_index_destroy(sparse);
    dataset_destroy(&dataset);
    return EXIT_SUCCESS;

failure:
    baseline_flat_destroy(flat);
    baseline_two_level_destroy(two_level);
    baseline_radix_destroy(radix);
    memx_index_destroy(bounded);
    memx_index_destroy(overlay);
    memx_index_destroy(sparse);
    dataset_destroy(&dataset);
    return EXIT_FAILURE;
}
