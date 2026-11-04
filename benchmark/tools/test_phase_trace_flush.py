#!/usr/bin/env python3
"""Verify an externally terminated benchmark retains its latest phase."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
import time
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    # This checks durable phase output and termination, not rasterization.
    # Native rendering remains covered by validate_smoke.py.
    parser.add_argument("--graphics-backend", default="null")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        output_dir = Path(temporary)
        process = subprocess.Popen(
            [
                str(args.executable.resolve()),
                "--backend",
                "qrhi-offscreen",
                "--graphics-backend",
                args.graphics_backend,
                "--static",
                "--data-type",
                "bars",
                "--render-style",
                "line",
                "--static-samples",
                "10000",
                "--warmup-frames",
                "0",
                "--frames",
                "10000000",
                "--scenario",
                "phase-trace-flush-test",
                "--stream",
                "phase-trace-flush-test",
                "--output-dir",
                str(output_dir),
                "--finish",
                "--quiet",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 30.0
        retained_text = ""
        try:
            while time.monotonic() < deadline:
                traces = list(output_dir.glob("benchmark_phase_trace_*.jsonl"))
                if traces:
                    retained_text = traces[0].read_text(encoding="utf-8")
                    if '"phase":"measure.begin"' in retained_text:
                        break
                if process.poll() is not None:
                    raise RuntimeError("benchmark exited before external termination")
                time.sleep(0.01)
            else:
                raise RuntimeError("measure.begin was not durably retained")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5.0)

        if '"phase":"measure.begin"' not in retained_text:
            raise RuntimeError("terminated trace lost the measurement boundary")
        if '"phase":"complete"' in retained_text:
            raise RuntimeError("flush test completed instead of exercising termination")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
