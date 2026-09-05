#!/usr/bin/env python3
"""Evaluate MemX Gate 1 constraints from run_benchmarks.py summary CSV."""

from __future__ import annotations

import argparse
import csv
import dataclasses
import pathlib
import sys
from collections import defaultdict
from typing import Sequence


GATE_MEMORY_CATEGORY = "page_accounted_backing"


@dataclasses.dataclass(frozen=True)
class Measurement:
    case: str
    structure: str
    median_ns: float
    metadata_bytes: int


@dataclasses.dataclass(frozen=True)
class GateResult:
    case: str
    candidate: str
    baseline: str
    latency_ratio: float
    memory_ratio: float
    latency_limit: float
    memory_limit: float

    @property
    def passed(self) -> bool:
        return (
            self.latency_ratio <= self.latency_limit
            and self.memory_ratio <= self.memory_limit
        )


def load(path: pathlib.Path) -> list[Measurement]:
    rows: list[Measurement] = []
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        required = {
            "case",
            "structure",
            "median_ns",
            "metadata_bytes",
            "memory_category",
        }
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"summary lacks required columns: {required}")
        for row in reader:
            if row["memory_category"] != GATE_MEMORY_CATEGORY:
                raise ValueError(
                    "gate requires memory_category="
                    f"{GATE_MEMORY_CATEGORY}, got "
                    f"{row['memory_category']!r} for "
                    f"{row['case']}/{row['structure']}"
                )
            rows.append(
                Measurement(
                    case=row["case"],
                    structure=row["structure"],
                    median_ns=float(row["median_ns"]),
                    metadata_bytes=int(row["metadata_bytes"]),
                )
            )
    return rows


def index_measurements(
    rows: Sequence[Measurement],
) -> dict[str, dict[str, Measurement]]:
    indexed: dict[str, dict[str, Measurement]] = defaultdict(dict)
    for row in rows:
        if row.structure in indexed[row.case]:
            raise ValueError(f"duplicate row for {row.case}/{row.structure}")
        indexed[row.case][row.structure] = row
    return indexed


def compare(
    case: str,
    structures: dict[str, Measurement],
    candidate: str,
    baseline: str,
    latency_limit: float,
    memory_limit: float,
) -> GateResult:
    try:
        candidate_row = structures[candidate]
        baseline_row = structures[baseline]
    except KeyError as error:
        raise ValueError(f"{case} is missing structure {error.args[0]}") from error
    return GateResult(
        case=case,
        candidate=candidate,
        baseline=baseline,
        latency_ratio=candidate_row.median_ns / baseline_row.median_ns,
        memory_ratio=candidate_row.metadata_bytes / baseline_row.metadata_bytes,
        latency_limit=latency_limit,
        memory_limit=memory_limit,
    )


def evaluate(
    measurements: dict[str, dict[str, Measurement]],
    low_entropy_candidate: str,
    dense_candidate: str,
) -> list[GateResult]:
    results: list[GateResult] = []
    uniform_cases = [case for case in measurements if case.startswith("uniform-")]
    dense_cases = [case for case in measurements if case.startswith("random-")]
    if not uniform_cases:
        raise ValueError("summary has no uniform cases")
    if not dense_cases:
        raise ValueError("summary has no random cases")
    for case in sorted(uniform_cases):
        results.append(
            compare(
                case,
                measurements[case],
                low_entropy_candidate,
                "flat_assume_mapped",
                latency_limit=1.10,
                memory_limit=0.50,
            )
        )
    for case in sorted(dense_cases):
        results.append(
            compare(
                case,
                measurements[case],
                dense_candidate,
                "flat_assume_mapped",
                latency_limit=1.15,
                memory_limit=1.02,
            )
        )
    return results


def arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=pathlib.Path)
    parser.add_argument(
        "--low-entropy-candidate",
        default="memx_bounded_assume_mapped",
    )
    parser.add_argument(
        "--dense-candidate",
        default="memx_flat_fallback",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    parsed = arguments(sys.argv[1:] if argv is None else argv)
    try:
        rows = load(parsed.summary)
        results = evaluate(
            index_measurements(rows),
            parsed.low_entropy_candidate,
            parsed.dense_candidate,
        )
    except (OSError, ValueError) as error:
        print(f"cannot evaluate gate: {error}", file=sys.stderr)
        return 2

    print(
        "result,case,candidate,baseline,latency_ratio,latency_limit,"
        "memory_ratio,memory_limit"
    )
    for result in results:
        print(
            f"{'PASS' if result.passed else 'FAIL'},"
            f"{result.case},{result.candidate},{result.baseline},"
            f"{result.latency_ratio:.6f},{result.latency_limit:.6f},"
            f"{result.memory_ratio:.6f},{result.memory_limit:.6f}"
        )
    return 0 if all(result.passed for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
