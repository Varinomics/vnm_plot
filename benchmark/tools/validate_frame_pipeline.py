#!/usr/bin/env python3
"""Check that offscreen frames exercise production auto-range and preparation."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def run(executable: Path, output: Path, frames: int) -> dict:
    command = [
        str(executable.resolve()),
        "--backend", "qrhi-offscreen",
        "--graphics-backend", "null",
        "--static", "--no-text",
        "--static-samples", "10000",
        "--seed", "12345",
        "--warmup-frames", "0",
        "--frames", str(frames),
        "--quiet",
        "--output-dir", str(output),
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr or result.stdout)
    reports = list(output.glob("inspector_benchmark_*.json"))
    if len(reports) != 1:
        raise RuntimeError(f"Expected one benchmark report, found {len(reports)}")
    report = json.loads(reports[0].read_text(encoding="utf-8"))
    if int(report["metadata"]["measured_frames"]) != frames:
        raise RuntimeError("The requested offscreen frames did not complete")
    return report["observations"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vnm_plot_frame_pipeline_") as directory:
        root = Path(directory)
        observations = run(args.executable, root, 3)
        name = "renderer.auto_range.query_count"
        if observations.get(name, {}).get("total", 0) <= 0:
            raise RuntimeError("The offscreen frame bypassed production auto-range planning")
        prepare = observations["benchmark.planning.time_ms"]
        if prepare["count"] != 3 or prepare["min"] <= 0:
            raise RuntimeError("Production series preparation timings were not retained")
    print("Offscreen frames use production auto-range planning and preparation")


if __name__ == "__main__":
    main()
