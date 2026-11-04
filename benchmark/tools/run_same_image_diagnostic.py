#!/usr/bin/env python3
"""Temporary causal diagnostic: compare both label algorithms in one executable.

Use the normal run_suite.py arguments, with --baseline and --candidate pointing
to the SAME diagnostic executable. Baseline selects environment value 0;
candidate selects 1. An optional --suite-script selects the corrected harness.
This driver and the dual-algorithm header are not production functionality.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys


MODE_ENVIRONMENT = "VNM_PLOT_LABEL_MEMBERSHIP_DIAGNOSTIC"
MODES = {"baseline": "0", "candidate": "1"}
DEFAULT_CASES = [
    "warm-line-8", "warm-line-8-conservative", "warm-line-8-conservative-no-text",
]


def file_identity(path: Path) -> dict:
    data = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def load_suite(path: Path):
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location("same_image_suite", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot import suite: {path}")
    suite = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = suite
    spec.loader.exec_module(suite)
    return suite


def main() -> int:
    options = argparse.ArgumentParser(add_help=False)
    options.add_argument("--suite-script", type=Path,
                         default=Path(__file__).with_name("run_suite.py"))
    driver_args, suite_args = options.parse_known_args()
    suite_path = driver_args.suite_script.resolve(strict=True)
    suite = load_suite(suite_path)
    if not any(arg == "--cases" or arg.startswith("--cases=") for arg in suite_args):
        suite_args += ["--cases", *DEFAULT_CASES]
    sys.argv = [str(suite_path), *suite_args]
    args = suite.parse_args()
    if args.baseline is None or args.baseline != args.candidate:
        raise suite.BenchmarkError("both variants must point to the same diagnostic executable")
    if args.repetitions < 2 or args.repetitions % 2:
        raise suite.BenchmarkError("use an even number of pairs to balance A/B and B/A order")
    if {case.name for case in args.selected_cases} - set(DEFAULT_CASES):
        raise suite.BenchmarkError("this bounded diagnostic supports only its three declared cases")

    binary = file_identity(args.candidate)
    source_root = Path(__file__).resolve().parents[2]
    source_paths = [
        Path(__file__).resolve(), suite_path, suite_path.with_name("phase_trace.py"),
        source_root / "src/core/label_fade_tracker.h",
        source_root / "include/vnm_plot/rhi/text_renderer.h",
        source_root / "benchmark/src/main.cpp",
        source_root / "benchmark/src/benchmark_window.cpp",
    ]
    provenance = {
        "schema_version": 1, "status": "running",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "purpose": "diagnostic-only runtime choice in one unchanged executable",
        "executable": binary,
        "environment_variable": MODE_ENVIRONMENT,
        "mode_mapping": MODES,
        "choice_boundary": "first text helper call during warmup; retained for process",
        "no_text_contract": "variable is not consulted and no selected-mode marker is emitted",
        "interpretation": (
            "Equal code placement controls the original two-binary comparison confound. "
            "This does not establish native rendering performance or identify a CPU mechanism."
        ),
        "source_capture_note": (
            "These are file hashes at run time; separate build provenance establishes "
            "which sources produced the executable."
        ),
        "source_files": [file_identity(path) for path in source_paths],
    }
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    provenance_path = output / "same-image-diagnostic.json"
    suite.write_json(provenance_path, provenance)
    original_run_one = suite.run_one

    def diagnostic_run_one(executable, variant, case, repetition, run_args, run_output):
        if executable != args.candidate:
            raise suite.BenchmarkError("diagnostic executable changed between variants")
        if file_identity(executable)["sha256"] != binary["sha256"]:
            raise suite.BenchmarkError("diagnostic executable changed during the experiment")
        mode = MODES[variant]
        directory = run_output / case.name / f"{repetition + 1:02d}-{variant}"
        previous = os.environ.get(MODE_ENVIRONMENT)
        diagnostic = {
            "requested_environment": {MODE_ENVIRONMENT: mode},
            "executable_sha256": binary["sha256"],
            "mode_read_expected": case.text,
            "validation": "pending",
        }
        result = None
        try:
            os.environ[MODE_ENVIRONMENT] = mode
            result = original_run_one(executable, variant, case, repetition, run_args, run_output)
            markers = [line for line in (directory / "stderr.log").read_text().splitlines()
                       if line.startswith(MODE_ENVIRONMENT + "=")]
            diagnostic["actual_mode_markers"] = markers
            expected = [f"{MODE_ENVIRONMENT}={mode}"] if case.text else []
            if markers != expected:
                raise suite.BenchmarkError(
                    f"{case.name}/{variant} actual mode markers {markers!r} != {expected!r}")
            diagnostic["validation"] = "valid"
            result["same_image_diagnostic"] = diagnostic
            return result
        except BaseException as exc:
            diagnostic["validation"] = "failed"
            diagnostic["error"] = str(exc)
            raise
        finally:
            if previous is None:
                os.environ.pop(MODE_ENVIRONMENT, None)
            else:
                os.environ[MODE_ENVIRONMENT] = previous
            if directory.is_dir():
                suite.write_json(directory / "diagnostic-mode.json", diagnostic)
                invocation_path = directory / "invocation.json"
                if invocation_path.is_file():
                    invocation = json.loads(invocation_path.read_text())
                    invocation["same_image_diagnostic"] = diagnostic
                    if diagnostic["validation"] == "failed":
                        invocation["status"] = "failed"
                        invocation["error"] = diagnostic["error"]
                    suite.write_json(invocation_path, invocation)

    # Reuse all current suite scheduling, workload/phase validation, environment
    # comparison, checkpointing and summaries. The suite arguments were already
    # validated before creating the provenance sidecar in its empty output tree.
    suite.parse_args = lambda: args
    suite.run_one = diagnostic_run_one
    try:
        result = suite.main()
        if file_identity(args.candidate)["sha256"] != binary["sha256"]:
            raise suite.BenchmarkError("diagnostic executable changed during the experiment")
        provenance["status"] = "complete"
        return result
    except BaseException as exc:
        provenance["status"] = "interrupted" if isinstance(exc, KeyboardInterrupt) else "failed"
        provenance["error"] = str(exc)
        raise
    finally:
        provenance["ended_at"] = datetime.now(timezone.utc).isoformat()
        suite.write_json(provenance_path, provenance)
        manifest_path = output / "manifest.json"
        if manifest_path.is_file():
            manifest = json.loads(manifest_path.read_text())
            manifest["same_image_diagnostic_provenance"] = provenance_path.name
            if provenance["status"] != "complete":
                manifest["status"] = provenance["status"]
                manifest["error"] = provenance.get("error", "diagnostic failed")
            suite.write_json(manifest_path, manifest)


if __name__ == "__main__":
    raise SystemExit(main())
