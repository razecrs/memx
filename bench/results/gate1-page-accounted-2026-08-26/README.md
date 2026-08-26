# Gate 1 equal-category run — 2026-08-26

This is the first Gate 1 run using one memory category for every structure:
`page_accounted_backing`.

Each live heap allocation is rounded independently to the actual host page
size. The frozen overlay adds only page-rounded intervals intersecting DENSE
regions. Reserved virtual address space and measured overlay RSS/private/shared
pages remain separate raw fields. The model is deliberately conservative and
may overcount heap allocations that share a physical page.

Configuration:

```text
host: Intel Core i7-9750H, Linux x86-64
compiler: GCC 16.2.1, Release build
CPU: pinned to logical CPU 2
regions: 256
region/granule: 2 MiB / 4 KiB
lookups per timed row: 5,000,000
repetitions: 15
seed: 0x6d656d78
governor: powersave
```

The low-entropy overlay passed at 0.753251× flat latency and 0.007782× flat
page-accounted bytes. The dense flat fallback passed at 0.974340× flat latency
and exactly 1.0× its page-accounted bytes.

`raw.csv` preserves every timing row, its literal memory category, and all four
overlay residency fields. `summary.csv`/`summary.json` contain bootstrap
summaries; `environment.json` captures the host and arguments; `gate.csv`
preserves the gate result.

Re-check with:

```bash
python3 tools/check_gate.py \
  bench/results/gate1-page-accounted-2026-08-26/summary.csv \
  --low-entropy-candidate memx_overlay_trusted \
  --dense-candidate memx_flat_fallback
```

This remains Level 0 synthetic evidence on one host, not an allocator or broad
hardware performance claim.
