# Gate 1 overlay run — 2026-08-26

This directory preserves the first numeric MemX Gate 1 candidate run.

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

`raw.csv` contains every timed row. `summary.csv` and `summary.json` contain the
bootstrap summaries. `environment.json` records the command arguments and host
environment. `gate.csv` is the output of the two explicit gate comparisons.

Post-run review identified a heterogeneous metadata category: overlay rows use
requested directory bytes plus page-rounded private overlay intervals, whereas
flat rows use requested allocation bytes. `gate.csv` therefore records what the
script returned, not a formal memory-gate pass. It remains checked in so the
mistake and the latency evidence are reproducible rather than erased.

Re-check with:

```bash
python3 tools/check_gate.py \
  bench/results/gate1-overlay-2026-08-26/summary.csv \
  --low-entropy-candidate memx_overlay_trusted \
  --dense-candidate memx_flat_fallback
```

This is Level 0 synthetic evidence from one host. The powersave governor and
single-machine scope prevent treating it as a publication or broad performance
claim.
