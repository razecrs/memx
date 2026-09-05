# Benchmark result storage

`tools/run_benchmarks.py` creates one timestamped directory here unless an
explicit output path is supplied.

Each run contains:

- `environment.json`: host, toolchain, arguments, cases, and repository state;
- `raw.csv`: every structure row from every repetition;
- `summary.csv`: median, tails, dispersion, confidence interval, and bytes;
- `summary.json`: the same summary in structured form;
- `errors.json`: preserved failures from keep-going runs.

Generated result directories are experimental evidence. Do not hand-edit them.
Smoke results should not be committed as publication results without reviewing
frequency control, affinity, thermal state, and hardware-counter availability.

