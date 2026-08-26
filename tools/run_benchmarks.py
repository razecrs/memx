#!/usr/bin/env python3
"""Run reproducible MemX benchmark sweeps without third-party dependencies.

This script does not make results publication-quality by itself. It captures
the command, environment, host details, raw rows, and robust summary statistics
so that smoke runs and later pinned-hardware runs use one data format.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import datetime as dt
import json
import math
import os
import pathlib
import platform
import random
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from typing import Iterable, Sequence


@dataclasses.dataclass(frozen=True)
class Case:
    workload: str
    dense_percent: int
    regions: int
    lookups: int
    region_shift: int
    granule_shift: int
    seed: int

    @property
    def name(self) -> str:
        return (
            f"{self.workload}-d{self.dense_percent}-r{self.regions}"
            f"-rs{self.region_shift}-gs{self.granule_shift}"
        )


@dataclasses.dataclass(frozen=True)
class RawResult:
    case: str
    repetition: int
    structure: str
    ns_per_lookup: float
    metadata_bytes: int
    bytes_per_managed_mib: float
    checksum: str
    elapsed_seconds: float
    overlay_reserved_bytes: int
    overlay_rss_bytes: int
    overlay_private_bytes: int
    overlay_shared_bytes: int
    memory_category: str


def percentile(values: Sequence[float], fraction: float) -> float:
    """Return a linearly interpolated percentile for a non-empty sequence."""
    if not values:
        raise ValueError("percentile requires at least one value")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def median_absolute_deviation(values: Sequence[float]) -> float:
    median = statistics.median(values)
    return statistics.median([abs(value - median) for value in values])


def bootstrap_median_interval(
    values: Sequence[float],
    confidence: float,
    samples: int,
    seed: int,
) -> tuple[float, float]:
    if not values:
        raise ValueError("bootstrap requires at least one value")
    if len(values) == 1 or samples <= 1:
        return values[0], values[0]
    generator = random.Random(seed)
    medians: list[float] = []
    for _ in range(samples):
        resample = [generator.choice(values) for _ in values]
        medians.append(statistics.median(resample))
    tail = (1.0 - confidence) / 2.0
    return percentile(medians, tail), percentile(medians, 1.0 - tail)


def read_text_command(command: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        return completed.stdout.strip()
    except (OSError, subprocess.CalledProcessError) as error:
        return f"unavailable: {error}"


def host_metadata() -> dict[str, object]:
    metadata: dict[str, object] = {
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "cpu_count": os.cpu_count(),
        "uname": dataclasses.asdict(
            dataclasses.make_dataclass(
                "Uname",
                [(name, str) for name in platform.uname()._fields],
            )(*platform.uname())
        ),
        "lscpu": read_text_command(["lscpu"]),
        "governor": read_text_command(
            ["sh", "-c", "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"]
        ),
        "compiler": read_text_command(["cc", "--version"]),
        "git_head": read_text_command(["git", "rev-parse", "HEAD"]),
        "git_status": read_text_command(["git", "status", "--short"]),
    }
    return metadata


def benchmark_command(executable: pathlib.Path, case: Case) -> list[str]:
    return [
        str(executable),
        "--regions",
        str(case.regions),
        "--lookups",
        str(case.lookups),
        "--workload",
        case.workload,
        "--dense-percent",
        str(case.dense_percent),
        "--region-shift",
        str(case.region_shift),
        "--granule-shift",
        str(case.granule_shift),
        "--seed",
        hex(case.seed),
    ]


def with_affinity(command: list[str], cpu: int | None) -> list[str]:
    if cpu is None:
        return command
    return ["taskset", "--cpu-list", str(cpu), *command]


def parse_benchmark_output(
    output: str,
    case: Case,
    repetition: int,
    elapsed_seconds: float,
) -> list[RawResult]:
    memory = {
        "overlay_reserved_bytes": 0,
        "overlay_rss_bytes": 0,
        "overlay_private_bytes": 0,
        "overlay_shared_bytes": 0,
    }
    memory_category = "unspecified"
    lines: list[str] = []
    for line in output.splitlines():
        if line.startswith("# overlay_"):
            for field in line.removeprefix("# ").split():
                name, separator, value = field.partition("=")
                if separator and name in memory:
                    memory[name] = int(value)
        elif line.startswith("# memory_category="):
            memory_category = line.partition("=")[2].strip()
        elif not line.startswith("#"):
            lines.append(line)
    if len(lines) < 2:
        raise ValueError(f"benchmark returned no CSV rows:\n{output}")
    reader = csv.DictReader(lines)
    expected = {
        "structure",
        "ns_per_lookup",
        "metadata_bytes",
        "bytes_per_managed_mib",
        "checksum",
    }
    if reader.fieldnames is None or set(reader.fieldnames) != expected:
        raise ValueError(f"unexpected benchmark columns: {reader.fieldnames}")
    results: list[RawResult] = []
    for row in reader:
        results.append(
            RawResult(
                case=case.name,
                repetition=repetition,
                structure=row["structure"],
                ns_per_lookup=float(row["ns_per_lookup"]),
                metadata_bytes=int(row["metadata_bytes"]),
                bytes_per_managed_mib=float(row["bytes_per_managed_mib"]),
                checksum=row["checksum"],
                elapsed_seconds=elapsed_seconds,
                **memory,
                memory_category=memory_category,
            )
        )
    if not results:
        raise ValueError("benchmark CSV contained only a header")
    checksums = {result.checksum for result in results}
    if len(checksums) != 1:
        raise ValueError(
            f"structures returned different checksums in {case.name}: {checksums}"
        )
    return results


def run_case_once(
    executable: pathlib.Path,
    case: Case,
    repetition: int,
    cpu: int | None,
    timeout: float,
) -> list[RawResult]:
    command = with_affinity(benchmark_command(executable, case), cpu)
    before = time.monotonic()
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )
    elapsed = time.monotonic() - before
    if completed.returncode != 0:
        raise RuntimeError(
            f"benchmark failed with status {completed.returncode}\n"
            f"command: {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    return parse_benchmark_output(completed.stdout, case, repetition, elapsed)


def default_cases(arguments: argparse.Namespace) -> list[Case]:
    workloads: list[tuple[str, int]] = []
    if "uniform" in arguments.workloads:
        workloads.append(("uniform", 0))
    if "mixed" in arguments.workloads:
        workloads.extend(("mixed", dense) for dense in arguments.dense_percent)
    if "random" in arguments.workloads:
        workloads.append(("random", 100))
    return [
        Case(
            workload=workload,
            dense_percent=dense,
            regions=arguments.regions,
            lookups=arguments.lookups,
            region_shift=region_shift,
            granule_shift=granule_shift,
            seed=arguments.seed,
        )
        for region_shift in arguments.region_shift
        for granule_shift in arguments.granule_shift
        if granule_shift <= region_shift
        for workload, dense in workloads
    ]


def write_raw_csv(path: pathlib.Path, rows: Sequence[RawResult]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=[field.name for field in dataclasses.fields(RawResult)],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(dataclasses.asdict(row))


def summarize(
    rows: Sequence[RawResult],
    bootstrap_samples: int,
    confidence: float,
    seed: int,
) -> list[dict[str, object]]:
    groups: dict[tuple[str, str], list[RawResult]] = defaultdict(list)
    for row in rows:
        groups[(row.case, row.structure)].append(row)

    summaries: list[dict[str, object]] = []
    for (case, structure), group in sorted(groups.items()):
        timings = [row.ns_per_lookup for row in group]
        byte_counts = {row.metadata_bytes for row in group}
        bytes_per_mib = {row.bytes_per_managed_mib for row in group}
        checksums = {row.checksum for row in group}
        memory_categories = {row.memory_category for row in group}
        if (len(byte_counts) != 1 or len(bytes_per_mib) != 1
                or len(checksums) != 1 or len(memory_categories) != 1):
            raise ValueError(f"non-timing output varied for {case}/{structure}")
        low, high = bootstrap_median_interval(
            timings,
            confidence=confidence,
            samples=bootstrap_samples,
            seed=seed ^ hash((case, structure)),
        )
        summaries.append(
            {
                "case": case,
                "structure": structure,
                "runs": len(group),
                "median_ns": statistics.median(timings),
                "p95_ns": percentile(timings, 0.95),
                "p99_ns": percentile(timings, 0.99),
                "min_ns": min(timings),
                "max_ns": max(timings),
                "mad_ns": median_absolute_deviation(timings),
                "median_ci_low_ns": low,
                "median_ci_high_ns": high,
                "metadata_bytes": next(iter(byte_counts)),
                "bytes_per_managed_mib": next(iter(bytes_per_mib)),
                "checksum": next(iter(checksums)),
                "overlay_reserved_bytes": max(
                    row.overlay_reserved_bytes for row in group
                ),
                "overlay_rss_median_bytes": statistics.median(
                    row.overlay_rss_bytes for row in group
                ),
                "overlay_private_median_bytes": statistics.median(
                    row.overlay_private_bytes for row in group
                ),
                "overlay_shared_median_bytes": statistics.median(
                    row.overlay_shared_bytes for row in group
                ),
                "memory_category": next(iter(memory_categories)),
            }
        )
    return summaries


def write_summary_csv(path: pathlib.Path, rows: Sequence[dict[str, object]]) -> None:
    if not rows:
        raise ValueError("cannot write empty summary")
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def parse_integer_list(value: str) -> list[int]:
    values: list[int] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        parsed = int(item, 0)
        values.append(parsed)
    if not values:
        raise argparse.ArgumentTypeError("list cannot be empty")
    return values


def parse_workload_list(value: str) -> list[str]:
    valid = {"uniform", "mixed", "random"}
    values = [item.strip() for item in value.split(",") if item.strip()]
    unknown = set(values) - valid
    if not values or unknown:
        raise argparse.ArgumentTypeError(
            f"workloads must be comma-separated values from {sorted(valid)}"
        )
    return values


def arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--executable",
        type=pathlib.Path,
        default=pathlib.Path("build-release/memx_bench"),
    )
    parser.add_argument("--output", type=pathlib.Path, default=None)
    parser.add_argument("--repetitions", type=int, default=15)
    parser.add_argument("--regions", type=int, default=256)
    parser.add_argument("--lookups", type=int, default=10_000_000)
    parser.add_argument(
        "--workloads", type=parse_workload_list, default=["uniform", "mixed", "random"]
    )
    parser.add_argument(
        "--dense-percent",
        type=parse_integer_list,
        default=[0, 1, 5, 10, 25, 50, 75, 90, 99, 100],
    )
    parser.add_argument("--region-shift", type=parse_integer_list, default=[21])
    parser.add_argument("--granule-shift", type=parse_integer_list, default=[12])
    parser.add_argument("--seed", type=lambda text: int(text, 0), default=0x6D656D78)
    parser.add_argument("--cpu", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--bootstrap-samples", type=int, default=10_000)
    parser.add_argument("--confidence", type=float, default=0.95)
    parser.add_argument("--keep-going", action="store_true")
    parsed = parser.parse_args(argv)
    if parsed.repetitions <= 0 or parsed.regions <= 0 or parsed.lookups <= 0:
        parser.error("repetitions, regions, and lookups must be positive")
    if not 0.0 < parsed.confidence < 1.0:
        parser.error("confidence must be between zero and one")
    if any(not 0 <= dense <= 100 for dense in parsed.dense_percent):
        parser.error("dense percentages must be between 0 and 100")
    return parsed


def main(argv: Sequence[str] | None = None) -> int:
    parsed = arguments(sys.argv[1:] if argv is None else argv)
    executable = parsed.executable.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        print(f"benchmark executable is missing or not executable: {executable}", file=sys.stderr)
        return 2

    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = parsed.output or pathlib.Path("bench/results") / timestamp
    output.mkdir(parents=True, exist_ok=False)

    cases = default_cases(parsed)
    metadata = host_metadata()
    metadata.update(
        {
            "executable": str(executable),
            "arguments": vars(parsed) | {"executable": str(parsed.executable), "output": str(output)},
            "cases": [dataclasses.asdict(case) for case in cases],
        }
    )
    (output / "environment.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True), encoding="utf-8"
    )

    raw: list[RawResult] = []
    errors: list[dict[str, object]] = []
    total = len(cases) * parsed.repetitions
    completed_count = 0
    for repetition in range(parsed.repetitions):
        order = list(cases)
        random.Random(parsed.seed + repetition).shuffle(order)
        for case in order:
            completed_count += 1
            print(
                f"[{completed_count}/{total}] repetition={repetition} case={case.name}",
                flush=True,
            )
            try:
                raw.extend(
                    run_case_once(
                        executable,
                        case,
                        repetition,
                        parsed.cpu,
                        parsed.timeout,
                    )
                )
            except Exception as error:  # preserve partial experiment evidence
                errors.append(
                    {
                        "case": case.name,
                        "repetition": repetition,
                        "error": repr(error),
                    }
                )
                print(f"benchmark error: {error}", file=sys.stderr)
                if not parsed.keep_going:
                    write_raw_csv(output / "raw.csv", raw)
                    (output / "errors.json").write_text(
                        json.dumps(errors, indent=2), encoding="utf-8"
                    )
                    return 1

    write_raw_csv(output / "raw.csv", raw)
    summary = summarize(
        raw,
        bootstrap_samples=parsed.bootstrap_samples,
        confidence=parsed.confidence,
        seed=parsed.seed,
    )
    write_summary_csv(output / "summary.csv", summary)
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )
    (output / "errors.json").write_text(
        json.dumps(errors, indent=2), encoding="utf-8"
    )
    print(f"results written to {output}")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
