#!/usr/bin/env python3
"""Exercise benchmark workload/measurement contracts, without timing thresholds."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(executable: Path, output: Path, *extra: str) -> dict:
    command = [
        str(executable.resolve()), "--backend", "qrhi-offscreen",
        "--graphics-backend", "null", "--static", "--stable-access", "--seed", "42",
        "--static-samples", "1024", "--ring-size", "1024",
        "--series-count", "2", "--render-style", "line",
        "--warmup-frames", "4", "--frames", "8", "--finish", "--quiet",
        "--output-dir", str(output), *extra,
    ]
    environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=45, env=environment)
    if result.returncode:
        raise RuntimeError(f"{command!r}\n{result.stdout}\n{result.stderr}")
    reports = list(output.glob("inspector_benchmark_*.json"))
    if len(reports) != 1:
        raise RuntimeError("Expected one raw report per run")
    report = json.loads(reports[0].read_text(encoding="utf-8"))
    observations = report["observations"]
    assert report["metadata"]["workload_contract_version"] == "3"
    assert observations["benchmark.frame.output_count"]["total"] == 8
    assert observations["benchmark.frame.total_ms"]["count"] == 8
    assert observations["benchmark.frame.cpu_allocation_count"]["count"] == 8
    return report


def total(report: dict, metric: str) -> float:
    return report["observations"][metric]["total"]


def view_spans(report: dict) -> list[float]:
    observations = report["observations"]
    return [last - first for first, last in zip(
        observations["benchmark.view.t_min_ns"]["samples"],
        observations["benchmark.view.t_max_ns"]["samples"], strict=True)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vnm_plot_workloads_") as directory:
        root = Path(directory)
        fixed = run(args.executable, root / "fixed")
        # A settled plot consumes measured axis-width feedback, as Plot_widget
        # does. Reusing the initial 60px width must not force layout misses.
        assert "renderer.frame.calculate_layout.impl.cache_miss.pass1" not in fixed["scopes"]
        assert total(fixed, "renderer.frame.upload.primary_count") == 0
        assert total(fixed, "renderer.frame.upload.line_window_count") == 0
        assert total(fixed, "benchmark.ring.published_samples") == 0
        assert total(fixed, "renderer.auto_range.query_count") == 0
        assert total(fixed, "renderer.auto_range.range_scan_count") == 0

        pan = run(args.executable, root / "pan", "--view-motion", "pan")
        positions = pan["observations"]["benchmark.view.t_min_ns"]["samples"]
        assert all(a < b for a, b in zip(positions, positions[1:]))
        assert max(view_spans(pan)) - min(view_spans(pan)) <= 1
        assert total(pan, "benchmark.ring.published_samples") == 0

        zoom = run(args.executable, root / "zoom", "--view-motion", "zoom")
        spans = view_spans(zoom)
        assert all(a < b for a, b in zip(spans, spans[1:]))

        append = run(args.executable, root / "append", "--append-per-frame", "32")
        assert total(append, "benchmark.ring.published_samples") == 8 * 32 * 2
        assert total(append, "benchmark.ring.overwritten_samples") == 8 * 32 * 2
        assert total(append, "renderer.frame.upload.primary_count") > 0
        assert append["observations"]["benchmark.workload.publish_ms"]["count"] == 8

        outer = run(args.executable, root / "outer", "--frame-timing-only", "--no-preview")
        assert outer["metadata"]["internal_profiling"] == "false"
        assert outer["metadata"]["preview_enabled"] == "false"
        assert not any(name.startswith("renderer.") for name in outer["scopes"])
        assert "renderer.frame.upload.primary_count" not in outer["observations"]
        assert not any(name.startswith("renderer.") for name in outer["observations"])
        assert "benchmark.planning.time_ms" not in outer["observations"]
        assert total(outer, "benchmark.ring.published_samples") == 0

        for invalid in (["--view-motion", "unknown"],
                        ["--view-motion", "pan", "--append-per-frame", "1"],
                        ["--append-per-frame", "1"],
                        ["--view-motion", "zoom", "--static"]):
            result = subprocess.run([str(args.executable.resolve()), *invalid],
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 1, (invalid, result.returncode, result.stderr)
    print("Benchmark fixed/pan/zoom/publication, feedback, and timing-only contracts passed")


if __name__ == "__main__":
    main()
