#!/usr/bin/env python3
"""Build latency/metadata Pareto reports from MemX benchmark summaries.

The report is intentionally dependency-free.  It produces a machine-readable
CSV and, optionally, a Markdown table that can be plotted by any later tool.
Points are compared only within one benchmark case; combining workloads into
one frontier would hide conditional wins and losses.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import pathlib
import sys
from collections import defaultdict
from typing import Iterable, Sequence, TextIO


@dataclasses.dataclass(frozen=True)
class Point:
    case: str
    structure: str
    latency_ns: float
    metadata_bytes: int
    bytes_per_managed_mib: float
    runs: int
    ci_low_ns: float | None
    ci_high_ns: float | None


@dataclasses.dataclass(frozen=True)
class ClassifiedPoint:
    point: Point
    pareto: bool
    dominated_by: tuple[str, ...]
    latency_ratio_to_flat: float | None
    memory_ratio_to_flat: float | None


def optional_float(row: dict[str, str], name: str) -> float | None:
    value = row.get(name, "").strip()
    return None if not value else float(value)


def load_summary(path: pathlib.Path) -> list[Point]:
    points: list[Point] = []
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        required = {
            "case",
            "structure",
            "median_ns",
            "metadata_bytes",
            "bytes_per_managed_mib",
        }
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"summary is missing required columns: {sorted(required)}")
        for line, row in enumerate(reader, start=2):
            try:
                point = Point(
                    case=row["case"],
                    structure=row["structure"],
                    latency_ns=float(row["median_ns"]),
                    metadata_bytes=int(row["metadata_bytes"]),
                    bytes_per_managed_mib=float(row["bytes_per_managed_mib"]),
                    runs=int(row.get("runs", "1")),
                    ci_low_ns=optional_float(row, "median_ci_low_ns"),
                    ci_high_ns=optional_float(row, "median_ci_high_ns"),
                )
            except (KeyError, ValueError) as error:
                raise ValueError(f"invalid summary row {line}: {error}") from error
            if (
                not point.case
                or not point.structure
                or point.latency_ns <= 0.0
                or point.metadata_bytes < 0
                or point.bytes_per_managed_mib < 0.0
                or point.runs <= 0
            ):
                raise ValueError(f"invalid values in summary row {line}")
            points.append(point)
    if not points:
        raise ValueError("summary contains no points")
    return points


def dominates(left: Point, right: Point) -> bool:
    """True when left is no worse in both dimensions and better in one."""
    return (
        left.latency_ns <= right.latency_ns
        and left.metadata_bytes <= right.metadata_bytes
        and (
            left.latency_ns < right.latency_ns
            or left.metadata_bytes < right.metadata_bytes
        )
    )


def classify_case(points: Sequence[Point]) -> list[ClassifiedPoint]:
    names: set[str] = set()
    for point in points:
        if point.structure in names:
            raise ValueError(f"duplicate structure {point.structure} in {point.case}")
        names.add(point.structure)
    flat = next((point for point in points if point.structure == "flat"), None)
    classified: list[ClassifiedPoint] = []
    for point in points:
        dominators = tuple(
            sorted(other.structure for other in points if dominates(other, point))
        )
        classified.append(
            ClassifiedPoint(
                point=point,
                pareto=not dominators,
                dominated_by=dominators,
                latency_ratio_to_flat=(
                    point.latency_ns / flat.latency_ns if flat is not None else None
                ),
                memory_ratio_to_flat=(
                    point.metadata_bytes / flat.metadata_bytes
                    if flat is not None and flat.metadata_bytes != 0
                    else None
                ),
            )
        )
    return sorted(
        classified,
        key=lambda item: (
            not item.pareto,
            item.point.metadata_bytes,
            item.point.latency_ns,
            item.point.structure,
        ),
    )


def classify(points: Iterable[Point]) -> list[ClassifiedPoint]:
    groups: dict[str, list[Point]] = defaultdict(list)
    for point in points:
        groups[point.case].append(point)
    classified: list[ClassifiedPoint] = []
    for case in sorted(groups):
        classified.extend(classify_case(groups[case]))
    return classified


def ratio_text(value: float | None) -> str:
    return "" if value is None else f"{value:.6f}"


def write_csv_report(destination: TextIO, rows: Sequence[ClassifiedPoint]) -> None:
    writer = csv.writer(destination)
    writer.writerow(
        [
            "case",
            "structure",
            "pareto",
            "median_ns",
            "metadata_bytes",
            "bytes_per_managed_mib",
            "latency_ratio_to_flat",
            "memory_ratio_to_flat",
            "dominated_by",
            "runs",
            "median_ci_low_ns",
            "median_ci_high_ns",
        ]
    )
    for row in rows:
        point = row.point
        writer.writerow(
            [
                point.case,
                point.structure,
                "yes" if row.pareto else "no",
                f"{point.latency_ns:.9f}",
                point.metadata_bytes,
                f"{point.bytes_per_managed_mib:.9f}",
                ratio_text(row.latency_ratio_to_flat),
                ratio_text(row.memory_ratio_to_flat),
                ";".join(row.dominated_by),
                point.runs,
                "" if point.ci_low_ns is None else f"{point.ci_low_ns:.9f}",
                "" if point.ci_high_ns is None else f"{point.ci_high_ns:.9f}",
            ]
        )


def write_markdown_report(destination: TextIO, rows: Sequence[ClassifiedPoint]) -> None:
    destination.write("# MemX Pareto Report\n\n")
    destination.write(
        "A point is Pareto-efficient when no structure in the same case is "
        "both at least as fast and at least as small, with one strict win.\n\n"
    )
    current_case: str | None = None
    for row in rows:
        if row.point.case != current_case:
            current_case = row.point.case
            destination.write(f"## `{current_case}`\n\n")
            destination.write(
                "| Structure | Frontier | ns/lookup | Metadata bytes | "
                "Latency / flat | Memory / flat | Dominated by |\n"
            )
            destination.write(
                "|---|---:|---:|---:|---:|---:|---|\n"
            )
        destination.write(
            f"| `{row.point.structure}` "
            f"| {'yes' if row.pareto else 'no'} "
            f"| {row.point.latency_ns:.4f} "
            f"| {row.point.metadata_bytes} "
            f"| {ratio_text(row.latency_ratio_to_flat)} "
            f"| {ratio_text(row.memory_ratio_to_flat)} "
            f"| {', '.join(row.dominated_by)} |\n"
        )
    destination.write("\n")


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=pathlib.Path)
    parser.add_argument(
        "--csv",
        type=pathlib.Path,
        default=None,
        help="write classified points to this path (stdout when omitted)",
    )
    parser.add_argument(
        "--markdown",
        type=pathlib.Path,
        default=None,
        help="also write a human-readable Markdown report",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        rows = classify(load_summary(arguments.summary))
        if arguments.csv is None:
            write_csv_report(sys.stdout, rows)
        else:
            arguments.csv.parent.mkdir(parents=True, exist_ok=True)
            with arguments.csv.open("w", newline="", encoding="utf-8") as output:
                write_csv_report(output, rows)
        if arguments.markdown is not None:
            arguments.markdown.parent.mkdir(parents=True, exist_ok=True)
            with arguments.markdown.open("w", encoding="utf-8") as output:
                write_markdown_report(output, rows)
    except (OSError, ValueError) as error:
        print(f"cannot build Pareto report: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
