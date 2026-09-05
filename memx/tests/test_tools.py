#!/usr/bin/env python3
"""Unit tests for dependency-free benchmark analysis utilities."""

from __future__ import annotations

import csv
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_module(name: str, relative: str):
    path = ROOT / relative
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


pareto = load_module("memx_pareto", "tools/pareto.py")
source_metrics = load_module("memx_source_metrics", "tools/source_metrics.py")
run_benchmarks = load_module("memx_run_benchmarks", "tools/run_benchmarks.py")
check_gate = load_module("memx_check_gate", "tools/check_gate.py")


class ParetoTests(unittest.TestCase):
    def point(self, name: str, latency: float, memory: int):
        return pareto.Point(
            case="case-a",
            structure=name,
            latency_ns=latency,
            metadata_bytes=memory,
            bytes_per_managed_mib=float(memory),
            runs=15,
            ci_low_ns=latency - 0.1,
            ci_high_ns=latency + 0.1,
        )

    def test_strict_pareto_dominance(self) -> None:
        fast_large = self.point("fast-large", 1.0, 100)
        slow_small = self.point("slow-small", 2.0, 50)
        slow_large = self.point("slow-large", 2.0, 100)
        equal = self.point("equal", 1.0, 100)
        self.assertFalse(pareto.dominates(fast_large, slow_small))
        self.assertFalse(pareto.dominates(slow_small, fast_large))
        self.assertTrue(pareto.dominates(fast_large, slow_large))
        self.assertTrue(pareto.dominates(slow_small, slow_large))
        self.assertFalse(pareto.dominates(fast_large, equal))

    def test_classification_and_flat_ratios(self) -> None:
        points = [
            self.point("flat", 2.0, 200),
            self.point("memx", 2.1, 50),
            self.point("bad", 3.0, 300),
        ]
        indexed = {row.point.structure: row for row in pareto.classify_case(points)}
        self.assertTrue(indexed["flat"].pareto)
        self.assertTrue(indexed["memx"].pareto)
        self.assertFalse(indexed["bad"].pareto)
        self.assertEqual(indexed["bad"].dominated_by, ("flat", "memx"))
        self.assertAlmostEqual(indexed["memx"].latency_ratio_to_flat, 1.05)
        self.assertAlmostEqual(indexed["memx"].memory_ratio_to_flat, 0.25)

    def test_duplicate_structure_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate structure"):
            pareto.classify_case(
                [self.point("same", 1.0, 1), self.point("same", 2.0, 2)]
            )

    def test_csv_and_markdown_reports(self) -> None:
        rows = pareto.classify_case(
            [self.point("flat", 1.0, 100), self.point("compact", 1.5, 20)]
        )
        csv_output = io.StringIO()
        markdown_output = io.StringIO()
        pareto.write_csv_report(csv_output, rows)
        pareto.write_markdown_report(markdown_output, rows)
        parsed = list(csv.DictReader(io.StringIO(csv_output.getvalue())))
        self.assertEqual(len(parsed), 2)
        self.assertEqual({row["pareto"] for row in parsed}, {"yes"})
        self.assertIn("# MemX Pareto Report", markdown_output.getvalue())
        self.assertIn("`compact`", markdown_output.getvalue())

    def test_load_summary_rejects_invalid_values(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "summary.csv"
            path.write_text(
                "case,structure,median_ns,metadata_bytes,bytes_per_managed_mib\n"
                "x,flat,-1,10,10\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "invalid values"):
                pareto.load_summary(path)


class StatisticsTests(unittest.TestCase):
    def test_percentile_endpoints_and_interpolation(self) -> None:
        values = [4.0, 1.0, 3.0, 2.0]
        self.assertEqual(run_benchmarks.percentile(values, 0.0), 1.0)
        self.assertEqual(run_benchmarks.percentile(values, 1.0), 4.0)
        self.assertEqual(run_benchmarks.percentile(values, 0.5), 2.5)
        self.assertEqual(run_benchmarks.percentile([7.0], 0.95), 7.0)
        with self.assertRaises(ValueError):
            run_benchmarks.percentile([], 0.5)

    def test_median_absolute_deviation(self) -> None:
        self.assertEqual(
            run_benchmarks.median_absolute_deviation([1.0, 2.0, 100.0]),
            1.0,
        )

    def test_bootstrap_is_deterministic(self) -> None:
        values = [1.0, 2.0, 3.0, 5.0, 8.0]
        first = run_benchmarks.bootstrap_median_interval(values, 0.95, 500, 9)
        second = run_benchmarks.bootstrap_median_interval(values, 0.95, 500, 9)
        self.assertEqual(first, second)
        self.assertLessEqual(first[0], first[1])

    def test_parse_benchmark_output_requires_equal_checksums(self) -> None:
        case = run_benchmarks.Case("uniform", 0, 2, 100, 16, 12, 1)
        output = (
            "structure,ns_per_lookup,metadata_bytes,bytes_per_managed_mib,checksum\n"
            "flat,1.0,100,10.0,0x1\n"
            "memx,1.1,20,2.0,0x2\n"
        )
        with self.assertRaisesRegex(ValueError, "different checksums"):
            run_benchmarks.parse_benchmark_output(output, case, 0, 0.1)

    def test_parse_benchmark_output_preserves_overlay_residency(self) -> None:
        case = run_benchmarks.Case("uniform", 0, 2, 100, 16, 12, 1)
        output = (
            "# overlay_reserved_bytes=8192 overlay_rss_bytes=4096 "
            "overlay_private_bytes=4096 overlay_shared_bytes=0\n"
            "# memory_category=page_accounted_backing\n"
            "structure,ns_per_lookup,metadata_bytes,bytes_per_managed_mib,checksum\n"
            "flat,1.0,100,10.0,0x1\n"
            "memx,1.1,20,2.0,0x1\n"
        )
        rows = run_benchmarks.parse_benchmark_output(output, case, 0, 0.1)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0].overlay_reserved_bytes, 8192)
        self.assertEqual(rows[0].overlay_rss_bytes, 4096)
        self.assertEqual(rows[0].overlay_private_bytes, 4096)
        self.assertEqual(rows[0].overlay_shared_bytes, 0)
        self.assertEqual(rows[0].memory_category, "page_accounted_backing")


class GateTests(unittest.TestCase):
    def measurement(self, case: str, name: str, latency: float, memory: int):
        return check_gate.Measurement(case, name, latency, memory)

    def test_gate_boundaries_are_inclusive(self) -> None:
        case = "uniform-d0-r1-rs16-gs12"
        structures = {
            "flat": self.measurement(case, "flat", 10.0, 1000),
            "memx": self.measurement(case, "memx", 11.0, 500),
        }
        result = check_gate.compare(case, structures, "memx", "flat", 1.10, 0.50)
        self.assertTrue(result.passed)

    def test_gate_missing_structure_is_clear_error(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing structure memx"):
            check_gate.compare("case", {}, "memx", "flat", 1.1, 0.5)

    def test_gate_requires_page_accounted_memory_category(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "summary.csv"
            path.write_text(
                "case,structure,median_ns,metadata_bytes,memory_category\n"
                "uniform-d0-r1-rs16-gs12,flat,1.0,100,requested_bytes\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                ValueError, "requires memory_category=page_accounted_backing"
            ):
                check_gate.load(path)

    def test_gate_rejects_legacy_summary_without_memory_category(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "summary.csv"
            path.write_text(
                "case,structure,median_ns,metadata_bytes\n"
                "uniform-d0-r1-rs16-gs12,flat,1.0,100\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "lacks required columns"):
                check_gate.load(path)


class SourceMetricTests(unittest.TestCase):
    def test_exclusion_rules(self) -> None:
        self.assertTrue(source_metrics.is_excluded(pathlib.Path("research/x.c")))
        self.assertTrue(source_metrics.is_excluded(pathlib.Path("build-debug/x.c")))
        self.assertTrue(source_metrics.is_excluded(pathlib.Path("x/__pycache__/y.py")))
        self.assertFalse(source_metrics.is_excluded(pathlib.Path("src/memx.c")))

    def test_measure_source_counts_blank_and_nonblank(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()
            (root / "src" / "x.c").write_text("one\n\ntwo\n", encoding="utf-8")
            (root / "research").mkdir()
            (root / "research" / "ignored.c").write_text("ignored\n", encoding="utf-8")
            rows = source_metrics.measure_source(root)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0].physical_lines, 3)
            self.assertEqual(rows[0].blank_lines, 1)
            self.assertEqual(rows[0].nonblank_lines, 2)

    def test_cli_source_output_is_json(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools/source_metrics.py"), "source", str(ROOT)],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
        report = json.loads(completed.stdout)
        self.assertGreater(report["totals"]["physical_lines"], 0)
        self.assertGreater(report["totals"]["files"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
