#!/usr/bin/env python3
"""Reject invalid benchmark evidence before calculating a speedup."""

from __future__ import annotations

from argparse import Namespace
import copy
from dataclasses import replace
import json
from pathlib import Path
import tempfile
import unittest

import run_suite as suite
from phase_trace import expected_phases


def constant_observation(value: float, count: int = 1) -> dict:
    return {"count": count, "total": value * count, "mean": value,
            "min": value, "max": value, "p95": value}


class EvidenceValidation(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.case = suite.Case("warm-line-1", series=1)
        self.args = Namespace(frames=3, warmup_frames=2, graphics_backend="opengl", frame_timing_only=True)
        metadata = {name: "fixture" for name in suite.ENVIRONMENT_KEYS}
        metadata.update(suite.expected_metadata(self.case, self.args))
        metadata["actual_graphics_backend"] = "OpenGLES2"
        trace = self.directory / "trace.jsonl"
        trace.write_text("\n".join(
            json.dumps({"phase": phase, "frame": frame, "elapsed_ms": index})
            for index, (phase, frame) in enumerate(expected_phases(2, 3))
        ), encoding="utf-8")
        metadata["phase_trace_path"] = str(trace)
        observations = {name: constant_observation(2, 3) for name in suite.METRICS}
        observations.update({
            "benchmark.frame.output_count": constant_observation(1, 3),
            "benchmark.ring.published_samples": constant_observation(0),
            "benchmark.ring.overwritten_samples": constant_observation(0),
            "benchmark.ring.occupancy": constant_observation(10_000),
        })
        self.payload = {"schema_version": 1, "metadata": metadata,
                        "invocation_args": ["benchmark"], "observations": observations}

    def validate(self, payload: dict | None = None) -> None:
        suite.validate_run(payload or self.payload, self.case, self.args, ["benchmark"], self.directory)

    def test_timing_only_still_requires_exact_work_and_complete_frames(self) -> None:
        self.validate()  # No internal scopes or planning metric are required.
        for name, replacement in (
            ("benchmark.frame.total_ms", constant_observation(2, 2)),
            ("benchmark.ring.published_samples", constant_observation(1)),
            ("benchmark.ring.occupancy", constant_observation(0)),
        ):
            with self.subTest(name=name):
                payload = copy.deepcopy(self.payload)
                payload["observations"][name] = replacement
                with self.assertRaises(suite.BenchmarkError):
                    self.validate(payload)

    def test_missing_counter_is_not_zero(self) -> None:
        del self.payload["observations"]["benchmark.ring.published_samples"]
        with self.assertRaisesRegex(suite.BenchmarkError, "required observation is missing"):
            self.validate()

    def test_old_contract_wrong_backend_and_readback_are_rejected(self) -> None:
        for key, value in (("workload_contract_version", "1"),
                           ("actual_graphics_backend", "Null"), ("pixel_readback", "true"),
                           ("access_semantics", "conservative")):
            with self.subTest(key=key):
                payload = copy.deepcopy(self.payload)
                payload["metadata"][key] = value
                with self.assertRaises(suite.BenchmarkError):
                    self.validate(payload)

    def test_nonfinite_and_inconsistent_statistics_are_rejected(self) -> None:
        for key, value in (("mean", float("nan")), ("total", 999), ("p95", 999), ("count", True)):
            with self.subTest(key=key):
                payload = copy.deepcopy(self.payload)
                payload["observations"]["benchmark.frame.total_ms"][key] = value
                with self.assertRaises(suite.BenchmarkError):
                    self.validate(payload)

    def test_profiling_enabled_requires_internal_counters(self) -> None:
        self.args.frame_timing_only = False
        self.payload["metadata"]["internal_profiling"] = "true"
        self.payload["observations"][suite.PLANNING_METRIC] = constant_observation(1, 3)
        with self.assertRaisesRegex(suite.BenchmarkError, "renderer.frame.upload.primary_count"):
            self.validate()

    def test_stable_range_cache_and_conservative_control_are_distinguished(self) -> None:
        self.args.frame_timing_only = False
        self.payload["metadata"]["internal_profiling"] = "true"
        observations = self.payload["observations"]
        observations[suite.PLANNING_METRIC] = constant_observation(1, 3)
        for name in (
            "renderer.frame.upload.primary_count", "renderer.frame.upload.primary_bytes",
            "renderer.frame.upload.line_window_count", "renderer.frame.upload.line_window_bytes",
            "renderer.series_window.monotonicity_scan_count",
            "renderer.series_window.monotonicity_scan_samples",
            "renderer.frame.gpu_buffer_allocation_count", "benchmark.snapshot.copied_bytes",
            *suite.AUTO_RANGE_COUNTERS,
        ):
            observations[name] = constant_observation(0)
        self.validate()
        observations["renderer.auto_range.query_count"] = constant_observation(2, 3)
        with self.assertRaisesRegex(suite.BenchmarkError, "settled stable-access range"):
            self.validate()
        self.case = replace(self.case, stable_access=False)
        self.payload["metadata"]["access_semantics"] = "conservative"
        with self.assertRaisesRegex(suite.BenchmarkError, "conservative-access control"):
            self.validate()
        observations["renderer.auto_range.range_scan_count"] = constant_observation(2, 3)
        self.validate()


class ComparisonValidation(unittest.TestCase):
    def test_pairing_is_preserved_instead_of_ratio_of_pooled_medians(self) -> None:
        comparison = suite.paired_changes([1.0, 100.0, 100.0], [2.0, 50.0, 100.0])
        # Paired changes are +100%, -50%, 0%. A ratio of medians says -50%.
        self.assertEqual(comparison["percent_change"]["median"], 0.0)
        self.assertEqual(comparison["percent_change"]["n"], 3)
        with self.assertRaises(suite.BenchmarkError):
            suite.paired_changes([1.0, 2.0], [1.0])

    def test_zero_baseline_has_absolute_change_without_invented_percentage(self) -> None:
        comparison = suite.paired_changes([0.0, 0.0], [1.0, 2.0])
        self.assertIsNone(comparison["percent_change"])
        self.assertEqual(comparison["absolute_change"]["median"], 1.5)

    def test_incomparable_environment_is_rejected(self) -> None:
        expected = {"device_name": "hardware GPU", "compiler": "GNU 13", "internal_profiling": "true"}
        for key, replacement in (("device_name", "llvmpipe"), ("compiler", "GNU 14"),
                                 ("internal_profiling", "false")):
            with self.subTest(key=key):
                changed = dict(expected, **{key: replacement})
                with self.assertRaises(suite.BenchmarkError):
                    suite.check_metadata(changed, expected, "comparison")


if __name__ == "__main__":
    unittest.main()
