# Historical allocator workload (x86-64 Linux)

This page preserves an earlier five-run snapshot. The current pinned,
interleaved 15-run comparison—including the experimental MemX heap and raw
samples—is stored in
`bench/results/allocator-comparison-2026-08-26/results.json`. Do not combine
the medians below with that newer run.

This is an executable reality check, separate from MemX's pointer-metadata
microbenchmark. The same `bench/allocator_workload.c` binary was run with
100,000 live slots for three rounds. Each round performs deterministic
size-class allocation, writes, shuffled frees, and approximately 25% reallocs.
Every run produced checksum `2887`; the table reports the median of five runs.

| allocator | ns / allocation operation | execution mode |
| --- | ---: | --- |
| glibc | 282.475 | system default |
| jemalloc | 187.707 | system `libjemalloc.so.2` preload |
| mimalloc | 60.160 | pinned source snapshot shared library preload |
| snmalloc | 154.947 | pinned source snapshot shim preload |
| TCMalloc | 174.985 | pinned source snapshot statically linked harness |

These numbers measure complete allocator behavior, not MemX lookup. They are
not a claim that MemX replaces an allocator: the core remains an indexing
subsystem, while `memx_heap` is a separate experimental integration with only
stages 1 through 8. In particular, the jemalloc row uses the installed system
library because the checked-out snapshot requires an unavailable autotools
bootstrap; the source snapshot remains included for source-level comparison.

For metadata lookup, the checked-in MemX benchmark was run separately with
8 regions and 1,000,000 lookups. On the uniform workload, the median observed
`memx_bounded_assume_mapped` result was about 2.05 ns/lookup with 232 bytes of
metadata, versus 1.89 ns and 32,800 bytes for the equal-precondition flat
table. On the random workload, MemX's direct-table fallback was about 1.87
ns/lookup with 32,936 bytes, while the same flat baseline was about 1.83 ns
with 32,800 bytes. The bounded checked path is intentionally slower because
it validates address coverage; it is not the fair hot-path comparison.

Reproduce the allocator run with:

```sh
cc -O3 -std=c17 -Wall -Wextra -Wconversion -Wshadow -Werror \
  bench/allocator_workload.c -o /tmp/memx-allocator-workload
/tmp/memx-allocator-workload 100000 3
LD_PRELOAD=/tmp/memx-build-mimalloc/libmimalloc.so.3.5 \
  /tmp/memx-allocator-workload 100000 3
```
