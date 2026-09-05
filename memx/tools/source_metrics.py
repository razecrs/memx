#!/usr/bin/env python3
"""Report reproducible first-party source and binary footprint metrics."""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import subprocess
import sys
from collections import Counter
from typing import Iterable, Sequence


SOURCE_SUFFIXES = {".c", ".h", ".md", ".py", ".yml", ".yaml"}
EXCLUDED_DIRECTORIES = {
    ".git",
    "research",
    "__pycache__",
    ".pytest_cache",
    ".mypy_cache",
}


@dataclasses.dataclass(frozen=True)
class SourceMetric:
    path: str
    suffix: str
    physical_lines: int
    blank_lines: int
    nonblank_lines: int
    bytes: int


@dataclasses.dataclass(frozen=True)
class BinarySection:
    name: str
    size: int
    address: int


def is_excluded(relative: pathlib.Path) -> bool:
    return any(
        part in EXCLUDED_DIRECTORIES or part.startswith("build")
        for part in relative.parts
    )


def source_paths(root: pathlib.Path) -> Iterable[pathlib.Path]:
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if is_excluded(relative):
            continue
        if path.name == "CMakeLists.txt" or path.suffix in SOURCE_SUFFIXES:
            yield path


def measure_source(root: pathlib.Path) -> list[SourceMetric]:
    rows: list[SourceMetric] = []
    for path in source_paths(root):
        data = path.read_bytes()
        text = data.decode("utf-8")
        lines = text.splitlines()
        blank = sum(1 for line in lines if not line.strip())
        rows.append(
            SourceMetric(
                path=str(path.relative_to(root)),
                suffix=path.suffix or path.name,
                physical_lines=len(lines),
                blank_lines=blank,
                nonblank_lines=len(lines) - blank,
                bytes=len(data),
            )
        )
    return rows


def command_output(command: Sequence[str]) -> str:
    completed = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout


def binary_sections(path: pathlib.Path) -> list[BinarySection]:
    output = command_output(["size", "-A", "-d", str(path)])
    rows: list[BinarySection] = []
    for line in output.splitlines()[2:]:
        fields = line.split()
        if len(fields) != 3:
            continue
        try:
            rows.append(
                BinarySection(
                    name=fields[0],
                    size=int(fields[1], 10),
                    address=int(fields[2], 10),
                )
            )
        except ValueError:
            continue
    if not rows:
        raise ValueError(f"could not parse section sizes for {path}")
    return rows


def symbol_sizes(path: pathlib.Path, prefix: str) -> list[dict[str, object]]:
    output = command_output(
        ["nm", "--print-size", "--size-sort", "--radix=d", str(path)]
    )
    symbols: list[dict[str, object]] = []
    for line in output.splitlines():
        fields = line.split(maxsplit=3)
        if len(fields) != 4 or not fields[3].startswith(prefix):
            continue
        try:
            symbols.append(
                {
                    "address": int(fields[0], 10),
                    "size": int(fields[1], 10),
                    "type": fields[2],
                    "name": fields[3],
                }
            )
        except ValueError:
            continue
    return symbols


def source_report(root: pathlib.Path) -> dict[str, object]:
    rows = measure_source(root)
    by_suffix: dict[str, Counter[str]] = {}
    for suffix in sorted({row.suffix for row in rows}):
        selected = [row for row in rows if row.suffix == suffix]
        by_suffix[suffix] = Counter(
            files=len(selected),
            physical_lines=sum(row.physical_lines for row in selected),
            blank_lines=sum(row.blank_lines for row in selected),
            nonblank_lines=sum(row.nonblank_lines for row in selected),
            bytes=sum(row.bytes for row in selected),
        )
    return {
        "root": str(root.resolve()),
        "exclusions": sorted(EXCLUDED_DIRECTORIES),
        "totals": {
            "files": len(rows),
            "physical_lines": sum(row.physical_lines for row in rows),
            "blank_lines": sum(row.blank_lines for row in rows),
            "nonblank_lines": sum(row.nonblank_lines for row in rows),
            "bytes": sum(row.bytes for row in rows),
        },
        "by_suffix": {key: dict(value) for key, value in by_suffix.items()},
        "files": [dataclasses.asdict(row) for row in rows],
    }


def binary_report(path: pathlib.Path, prefix: str) -> dict[str, object]:
    sections = binary_sections(path)
    return {
        "binary": str(path.resolve()),
        "sections": [dataclasses.asdict(section) for section in sections],
        "loadable_footprint": sum(
            section.size
            for section in sections
            if section.name in {".text", ".rodata", ".data", ".bss"}
        ),
        "symbols": symbol_sizes(path, prefix),
    }


def arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    source = subparsers.add_parser("source")
    source.add_argument("root", nargs="?", type=pathlib.Path, default=pathlib.Path("."))
    binary = subparsers.add_parser("binary")
    binary.add_argument("path", type=pathlib.Path)
    binary.add_argument("--symbol-prefix", default="memx_")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    parsed = arguments(sys.argv[1:] if argv is None else argv)
    try:
        if parsed.command == "source":
            report = source_report(parsed.root)
        else:
            report = binary_report(parsed.path, parsed.symbol_prefix)
    except (OSError, UnicodeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"cannot collect metrics: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
