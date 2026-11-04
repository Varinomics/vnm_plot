#!/usr/bin/env python3
"""Run repeatable vnm_plot workloads and compare paired executable runs.

Each process is one replicate. Frames from a process are not treated as
independent replicates. The executables must contain the same version of the
benchmark harness; only the library implementation under study should differ.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
from typing import Any

from phase_trace import parse_and_validate_phase_trace


class BenchmarkError(RuntimeError):
    """An incomplete, invalid, or incomparable benchmark run."""


@dataclass(frozen=True)
class Case:
    name: str
    series: int = 8
    samples: int = 10_000
    data_type: str = "bars"
    style: str = "line"
    stacked: bool = False
    motion: str = "fixed"
    append: int = 0
    wrap: bool = False
    text: bool = True
    stable_access: bool = True

    def capacity(self, frames: int) -> int:
        if self.wrap:
            return self.samples
        return max(100_000, self.samples + self.append * frames)

    @property
    def rate(self) -> float:
        # The renderer normally shows the last ten seconds. Keep denser
        # workloads visible instead of increasing only offscreen history.
        return max(1000.0, self.samples / 10.0)


CASES = (
    Case("warm-line-1", series=1),
    Case("warm-line-8"),
    Case("warm-line-32", series=32),
    Case("dense-line-8", samples=100_000),
    Case("warm-area-8", style="area"),
    Case("warm-stacked-area-8", style="area", stacked=True),
    Case("warm-dots-8", data_type="trades", style="dots"),
    Case("pan-line-8", motion="pan"),
    Case("zoom-line-8", motion="zoom"),
    Case("append-line-8", append=16),
    Case("wrap-line-8", append=16, wrap=True),
    Case("warm-line-8-no-text", text=False),
    Case("warm-line-8-conservative", stable_access=False),
)

METRICS = (
    "benchmark.frame.total_ms",
    "benchmark.frame.submission_ms",
    "benchmark.frame.gpu_finish_ms",
    "benchmark.frame.cpu_allocation_count",
    "benchmark.frame.cpu_allocation_bytes",
)
PLANNING_METRIC = "benchmark.planning.time_ms"
AUTO_RANGE_COUNTERS = (
    "renderer.auto_range.query_count", "renderer.auto_range.range_scan_count",
)

# Compare the rendering and build environment independently of the workload.
# Invocation paths, timestamps, checksums of executables, and source revisions
# intentionally differ between the two variants.
ENVIRONMENT_KEYS = (
    "actual_graphics_backend", "device_name", "device_type", "device_id",
    "vendor_id", "driver_identity", "driver_version", "build_type", "compiler",
    "qt_version", "os", "kernel_type", "kernel_version", "cpu_architecture",
    "build_cpu_architecture", "presentation_backend", "framebuffer",
    "sample_count", "context_sample_count", "context_profile_request",
    "context_version_request", "fallback_surface_requested_format",
    "fallback_surface_resolved_format", "env.QT_QPA_PLATFORM",
    "env.QSG_RHI_BACKEND", "env.GALLIUM_DRIVER", "env.LP_NUM_THREADS",
    "workload_contract_version", "finish_state", "pixel_readback",
    "internal_profiling",
)
RENDER_ENVIRONMENT = (
    "QT_QPA_PLATFORM", "QSG_RHI_BACKEND", "QSG_RENDER_LOOP", "QT_SCALE_FACTOR",
    "QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI", "LIBGL_ALWAYS_SOFTWARE",
    "GALLIUM_DRIVER", "LP_NUM_THREADS", "MESA_LOADER_DRIVER_OVERRIDE",
    "XDG_CACHE_HOME",
)


def write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def finite_number(value: object, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BenchmarkError(f"{name} is not a number")
    result = float(value)
    if not math.isfinite(result):
        raise BenchmarkError(f"{name} is not finite")
    return result


def observation(payload: dict[str, Any], name: str) -> dict[str, Any]:
    observations = payload.get("observations")
    if not isinstance(observations, dict) or name not in observations:
        raise BenchmarkError(f"required observation is missing: {name}")
    result = observations[name]
    if not isinstance(result, dict):
        raise BenchmarkError(f"observation is not an object: {name}")
    for key in ("count", "total", "mean", "min", "max"):
        finite_number(result.get(key), f"{name}.{key}")
    count = result["count"]
    if count < 1 or int(count) != count:
        raise BenchmarkError(f"{name}.count must be a positive integer")
    if result["min"] < 0 or result["max"] < result["min"]:
        raise BenchmarkError(f"{name} has invalid nonnegative bounds")
    if not result["min"] <= result["mean"] <= result["max"]:
        raise BenchmarkError(f"{name}.mean is outside its bounds")
    if not math.isclose(result["mean"] * count, result["total"], rel_tol=1e-7, abs_tol=1e-7):
        raise BenchmarkError(f"{name}.mean and total disagree")
    return result


def total(payload: dict[str, Any], name: str) -> float:
    return float(observation(payload, name)["total"])


def normalized_backend(value: str) -> str:
    value = "".join(character.lower() for character in value if character.isalnum())
    return {"opengles2": "opengl", "gles2": "opengl", "gles": "opengl",
            "direct3d11": "d3d11"}.get(value, value)


def expected_metadata(case: Case, args: argparse.Namespace) -> dict[str, str]:
    return {
        "workload_contract_version": "2",
        "data_type": case.data_type.title(),
        "render_style": case.style.title(),
        "series_count": str(case.series),
        "static_sample_count": str(case.samples),
        "ring_capacity": str(case.capacity(args.frames)),
        "rate": f"{case.rate:.6f}",
        "seed": "42",
        "static_data": "true",
        "access_semantics": "stable" if case.stable_access else "conservative",
        "stack_series": str(case.stacked).lower(),
        "view_motion": case.motion,
        "append_per_frame": str(case.append),
        "show_text": str(case.text).lower(),
        "preview_enabled": "true",
        "internal_profiling": str(not args.frame_timing_only).lower(),
        "pixel_readback": "false",
        "finish_state": "enabled",
        "presentation_backend": "qrhi-offscreen",
        "requested_graphics_backend": args.graphics_backend,
        "measured_frames": str(args.frames),
        "warmup_frames": str(args.warmup_frames),
        "scenario": case.name,
        "framebuffer": "1200x720",
        "sample_count": "4",
    }


def check_metadata(actual: dict[str, Any], expected: dict[str, Any], context: str) -> None:
    mismatches = {
        key: {"expected": value, "actual": actual.get(key)}
        for key, value in expected.items()
        if key not in actual or actual[key] != value
    }
    if mismatches:
        raise BenchmarkError(f"{context} metadata mismatch: {json.dumps(mismatches, sort_keys=True)}")


def validate_run(
    payload: dict[str, Any], case: Case, args: argparse.Namespace,
    command: list[str], directory: Path,
) -> tuple[dict[str, Any], dict[str, float]]:
    if payload.get("schema_version") != 1:
        raise BenchmarkError("unsupported raw report schema")
    metadata = payload.get("metadata")
    if not isinstance(metadata, dict):
        raise BenchmarkError("report metadata is missing")
    check_metadata(metadata, expected_metadata(case, args), case.name)
    for key in ENVIRONMENT_KEYS:
        if key not in metadata or not isinstance(metadata[key], str):
            raise BenchmarkError(f"required environment metadata is missing: {key}")
    if payload.get("invocation_args") != command:
        raise BenchmarkError("raw report invocation does not match the executed command")
    actual_backend = normalized_backend(metadata["actual_graphics_backend"])
    if args.graphics_backend == "native":
        if actual_backend in {"", "null", "uninitialized"}:
            raise BenchmarkError(f"native graphics request selected {actual_backend}")
    elif actual_backend != normalized_backend(args.graphics_backend):
        raise BenchmarkError(f"graphics backend mismatch: {actual_backend}")

    metrics = list(METRICS)
    if not args.frame_timing_only:
        metrics.append(PLANNING_METRIC)
    extracted = {}
    for name in metrics:
        values = observation(payload, name)
        if values["count"] != args.frames:
            raise BenchmarkError(f"{name}: expected {args.frames} measured frames, got {values['count']}")
        p95 = finite_number(values.get("p95"), f"{name}.p95")
        if not values["min"] <= p95 <= values["max"]:
            raise BenchmarkError(f"{name}.p95 is outside its bounds")
        if name.endswith("_ms") and values["mean"] <= 0:
            raise BenchmarkError(f"{name} did not measure positive elapsed work")
        extracted[name] = {"mean": values["mean"], "p95": p95}

    counters = {}
    for name in (
        "benchmark.frame.output_count", "benchmark.ring.published_samples",
        "benchmark.ring.overwritten_samples", "benchmark.ring.occupancy",
    ):
        counters[name] = total(payload, name)
    if counters["benchmark.frame.output_count"] != args.frames:
        raise BenchmarkError("completed frame counter mismatch")
    expected_publications = args.frames * case.append * case.series
    if counters["benchmark.ring.published_samples"] != expected_publications:
        raise BenchmarkError("deterministic publication count mismatch")
    expected_overwrites = expected_publications if case.wrap else 0
    if counters["benchmark.ring.overwritten_samples"] != expected_overwrites:
        raise BenchmarkError("ring overwrite count does not match the workload")
    expected_occupancy = min(case.capacity(args.frames), case.samples + args.frames * case.append)
    if counters["benchmark.ring.occupancy"] != expected_occupancy * case.series:
        raise BenchmarkError("final ring occupancy does not match the workload")

    if not args.frame_timing_only:
        for name in (
            "renderer.frame.upload.primary_count", "renderer.frame.upload.primary_bytes",
            "renderer.frame.upload.line_window_count", "renderer.frame.upload.line_window_bytes",
            "renderer.series_window.monotonicity_scan_count",
            "renderer.series_window.monotonicity_scan_samples",
            "renderer.frame.gpu_buffer_allocation_count", "benchmark.snapshot.copied_bytes",
            *AUTO_RANGE_COUNTERS,
        ):
            counters[name] = total(payload, name)
        if counters["benchmark.snapshot.copied_bytes"] != 0:
            raise BenchmarkError("the ring snapshot unexpectedly copied sample data")
        if not case.append and case.motion == "fixed":
            for name in AUTO_RANGE_COUNTERS:
                if case.stable_access and counters[name] != 0:
                    raise BenchmarkError(f"settled stable-access range repeated work: {name}")
                if not case.stable_access and counters[name] <= 0:
                    raise BenchmarkError(f"conservative-access control did not exercise range queries: {name}")
        if case.append:
            if counters["renderer.frame.upload.primary_count"] <= 0:
                raise BenchmarkError("append workload did not upload changed sample data")
        elif case.motion == "fixed" and not case.stacked:
            for name in (
                "renderer.frame.upload.primary_count", "renderer.frame.upload.primary_bytes",
                "renderer.frame.upload.line_window_count", "renderer.frame.upload.line_window_bytes",
                "renderer.series_window.monotonicity_scan_count",
                "renderer.series_window.monotonicity_scan_samples",
                "renderer.frame.gpu_buffer_allocation_count",
            ):
                if counters[name] != 0:
                    raise BenchmarkError(f"settled static frame unexpectedly repeated work: {name}")
        if case.stacked:
            for name in ("renderer.stacking.output_sample_count", "renderer.stacking.cache_hit_count"):
                counters[name] = total(payload, name)
                if counters[name] <= 0:
                    raise BenchmarkError(f"static stacked composition was not exercised: {name}")

    trace = Path(metadata.get("phase_trace_path", "")).resolve()
    if trace.parent != directory.resolve() or not trace.is_file():
        raise BenchmarkError("phase trace is missing or is outside this run")
    parse_and_validate_phase_trace(trace.read_text(encoding="utf-8"), args.warmup_frames, args.frames)
    return extracted, counters


def command_for(executable: Path, case: Case, args: argparse.Namespace, directory: Path) -> list[str]:
    command = [
        str(executable), "--backend", "qrhi-offscreen", "--graphics-backend", args.graphics_backend,
        "--static", "--seed", "42", "--data-type", case.data_type, "--render-style", case.style,
        "--series-count", str(case.series), "--static-samples", str(case.samples),
        "--ring-size", str(case.capacity(args.frames)), "--rate", str(case.rate),
        "--view-motion", case.motion, "--append-per-frame", str(case.append),
        "--warmup-frames", str(args.warmup_frames), "--frames", str(args.frames),
        "--finish", "--quiet", "--scenario", case.name, "--output-dir", str(directory),
    ]
    if case.stacked:
        command.append("--stack-series")
    if case.stable_access:
        command.append("--stable-access")
    if not case.text:
        command.append("--no-text")
    if args.frame_timing_only:
        command.append("--frame-timing-only")
    return command


def executable_identity(path: Path) -> dict[str, Any]:
    digest = hashlib.sha256()
    with path.open("rb") as executable:
        for chunk in iter(lambda: executable.read(1024 * 1024), b""):
            digest.update(chunk)
    result: dict[str, Any] = {"path": str(path), "sha256": digest.hexdigest(), "bytes": path.stat().st_size}
    try:
        def git(*arguments: str) -> str:
            return subprocess.run(
                ["git", "-C", str(path.parent), *arguments], check=True,
                capture_output=True, text=True, timeout=10,
            ).stdout.strip()
        result["git_at_binary_location"] = {
            "root": git("rev-parse", "--show-toplevel"),
            "head": git("rev-parse", "HEAD"),
            "dirty": bool(git("status", "--porcelain", "--untracked-files=no")),
            "note": "Repository state at run time; this does not establish the executable's build revision.",
        }
    except (OSError, subprocess.SubprocessError):
        result["git_at_binary_location"] = None
    return result


def host_information() -> dict[str, Any]:
    result: dict[str, Any] = {
        "platform": platform.platform(), "python": platform.python_version(),
        "machine": platform.machine(), "cpu_count": os.cpu_count(),
        "renderer_environment": {key: os.environ.get(key) for key in RENDER_ENVIRONMENT},
    }
    if hasattr(os, "sched_getaffinity"):
        result["cpu_affinity"] = sorted(os.sched_getaffinity(0))
    try:
        cpuinfo = Path("/proc/cpuinfo").read_text(encoding="utf-8")
        result["cpu_model"] = next(
            line.split(":", 1)[1].strip() for line in cpuinfo.splitlines() if line.startswith("model name")
        )
    except (OSError, StopIteration):
        result["cpu_model"] = platform.processor() or "unavailable"
    return result


def run_one(
    executable: Path, variant: str, case: Case, repetition: int,
    args: argparse.Namespace, output: Path,
) -> dict[str, Any]:
    directory = output / case.name / f"{repetition + 1:02d}-{variant}"
    directory.mkdir(parents=True, exist_ok=False)
    command = command_for(executable, case, args, directory)
    invocation: dict[str, Any] = {"command": command, "timeout_seconds": args.timeout_seconds}
    write_json(directory / "invocation.json", invocation)
    started = time.monotonic()
    try:
        with (directory / "stdout.log").open("w", encoding="utf-8") as stdout:
            with (directory / "stderr.log").open("w", encoding="utf-8") as stderr:
                completed = subprocess.run(
                    command, stdout=stdout, stderr=stderr, text=True,
                    timeout=args.timeout_seconds, check=False,
                )
        invocation["returncode"] = completed.returncode
        if completed.returncode != 0:
            raise BenchmarkError(f"{case.name}/{variant} exited {completed.returncode}; see {directory / 'stderr.log'}")
        reports = list(directory.glob("inspector_benchmark_*.json"))
        if len(reports) != 1:
            raise BenchmarkError(f"expected one raw report in {directory}, found {len(reports)}")
        payload = json.loads(reports[0].read_text(encoding="utf-8"))
        metrics, counters = validate_run(payload, case, args, command, directory)
        invocation["status"] = "valid"
        return {
            "case": case.name, "variant": variant, "repetition": repetition,
            "directory": str(directory.relative_to(output)),
            "raw_report": str(reports[0].relative_to(output)),
            "metadata": payload["metadata"], "metrics": metrics, "counters": counters,
            "scopes": payload.get("scopes", {}),
        }
    except BaseException as exc:
        invocation["status"] = "failed"
        invocation["error"] = str(exc)
        raise
    finally:
        invocation["process_wall_seconds"] = time.monotonic() - started
        write_json(directory / "invocation.json", invocation)


def distribution(values: list[float]) -> dict[str, float | int]:
    if not values:
        raise BenchmarkError("cannot summarize an empty set of process replicates")
    ordered = sorted(values)
    def quantile(fraction: float) -> float:
        index = (len(ordered) - 1) * fraction
        low = int(index)
        high = min(low + 1, len(ordered) - 1)
        return ordered[low] + (ordered[high] - ordered[low]) * (index - low)
    q25, q75 = quantile(0.25), quantile(0.75)
    return {
        "n": len(values), "median": statistics.median(values),
        "q25": q25, "q75": q75, "iqr": q75 - q25,
        "min": ordered[0], "max": ordered[-1],
    }


def paired_changes(baseline: list[float], candidate: list[float]) -> dict[str, Any]:
    if len(baseline) != len(candidate) or not baseline:
        raise BenchmarkError("comparison requires complete paired process replicates")
    differences = [after - before for before, after in zip(baseline, candidate)]
    percentages = None
    if all(before > 0 for before in baseline):
        percentages = distribution([100.0 * (after / before - 1.0) for before, after in zip(baseline, candidate)])
    return {"absolute_change": distribution(differences), "percent_change": percentages}


def summarize(manifest: dict[str, Any]) -> dict[str, Any]:
    runs = manifest["runs"]
    variants = list(manifest["executables"])
    cases: dict[str, Any] = {}
    for case in manifest["cases"]:
        name = case["name"]
        selected = {
            variant: sorted(
                (run for run in runs if run["case"] == name and run["variant"] == variant),
                key=lambda run: run["repetition"],
            )
            for variant in variants
        }
        for variant, items in selected.items():
            if [item["repetition"] for item in items] != list(range(manifest["repetitions"])):
                raise BenchmarkError(f"incomplete repetition set for {name}/{variant}")
        metrics = {}
        metric_names = list(next(iter(selected.values()))[0]["metrics"])
        for metric in metric_names:
            item: dict[str, Any] = {}
            for variant, items in selected.items():
                item[variant] = {
                    "run_means": distribution([run["metrics"][metric]["mean"] for run in items]),
                    "run_p95s": distribution([run["metrics"][metric]["p95"] for run in items]),
                }
            if "baseline" in selected:
                item["paired_means"] = paired_changes(
                    [run["metrics"][metric]["mean"] for run in selected["baseline"]],
                    [run["metrics"][metric]["mean"] for run in selected["candidate"]],
                )
                item["paired_p95s"] = paired_changes(
                    [run["metrics"][metric]["p95"] for run in selected["baseline"]],
                    [run["metrics"][metric]["p95"] for run in selected["candidate"]],
                )
            metrics[metric] = item
        scopes = {}
        all_names = sorted({scope for items in selected.values() for run in items for scope in run["scopes"]})
        for scope in all_names:
            item = {}
            for variant, items in selected.items():
                # A removed scope is reported as missing, not invented as zero.
                available = [run["scopes"][scope] for run in items if scope in run["scopes"]]
                item[variant] = {"available_runs": len(available)}
                if available:
                    times = [finite_number(value["total_ms"], scope) / manifest["frames"] for value in available]
                    item[variant]["inclusive_ms_per_frame"] = distribution(times)
            scopes[scope] = item
        counter_names = sorted({counter for items in selected.values() for run in items for counter in run["counters"]})
        counters = {
            counter: {
                variant: distribution([run["counters"][counter] for run in items])
                for variant, items in selected.items()
            }
            for counter in counter_names
        }
        cases[name] = {"metrics": metrics, "counter_totals": counters, "inclusive_scope_costs": scopes}
    return {
        "schema_version": 1, "cases": cases,
        "repetitions": manifest["repetitions"], "frames_per_process": manifest["frames"],
        "interpretation": {
            "replicate": "One fresh process. No pooling of frames as independent replicates.",
            "percent_change": "100 * (candidate / paired baseline - 1); negative means lower cost.",
            "spread": "IQR and min/max describe process-to-process variation, not a confidence interval.",
            "frame_total": "Wall time for offscreen frame work, including QRhi completion with --finish; not application FPS or CPU time.",
            "submission": "Includes begin/endOffscreenFrame and any backend synchronization they perform.",
            "scope_costs": "Inclusive instrumented CPU-side scope wall times; nested scopes must not be added.",
            "allocations": "C++ allocations observed on the render thread; excludes profiler storage, malloc-only and other-thread allocations.",
            "cold": "Setup and cold observations remain in raw reports and are not mixed into warm-frame statistics.",
            "validation": "Timing runs exclude pixel readback. Validate pixels/semantics separately before accepting an optimization.",
            "access_semantics": "Stable access identifies immutable benchmark callbacks so production caches can be exercised. The conservative control preserves uncached callback semantics; this workload correction is not a production optimization.",
        },
    }


def markdown_summary(summary: dict[str, Any], manifest: dict[str, Any]) -> str:
    paired = "baseline" in manifest["executables"]
    first = manifest["runs"][0]["metadata"]
    lines = [
        "# vnm_plot benchmark suite", "",
        f"{summary['repetitions']} fresh process replicates per variant and case; "
        f"{summary['frames_per_process']} measured frames per process after {manifest['warmup_frames']} warmup frames.", "",
        f"Backend: **{first['actual_graphics_backend']}**, device: **{first['device_name']}**. "
        f"Qt {first['qt_version']}; {first['compiler']}; {first['build_type']}. "
        f"Internal profiling: {first['internal_profiling']}. Pixel readback: disabled.", "",
        "Values below are medians of per-process statistics. A/B order alternates between paired repetitions. "
        "IQR and min/max describe variation between processes; they are not confidence intervals or automatic pass/fail thresholds.", "",
        "## Complete offscreen frame wall time", "",
    ]
    if paired:
        lines += [
            "| Case | Baseline mean ms | Candidate mean ms | Paired change | Paired range | Paired IQR | Baseline p95 ms | Candidate p95 ms |",
            "|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
    else:
        lines += ["| Case | Mean ms | Run-mean IQR ms | Run-mean range ms | p95 ms |", "|---|---:|---:|---:|---:|"]
    for name, values in summary["cases"].items():
        metric = values["metrics"]["benchmark.frame.total_ms"]
        candidate = metric["candidate"]
        if paired:
            baseline = metric["baseline"]
            change = metric["paired_means"]["percent_change"]
            lines.append(
                f"| {name} | {baseline['run_means']['median']:.4f} | {candidate['run_means']['median']:.4f} | "
                f"{change['median']:+.2f}% | {change['min']:+.2f} to {change['max']:+.2f}% | {change['iqr']:.2f} pp | "
                f"{baseline['run_p95s']['median']:.4f} | {candidate['run_p95s']['median']:.4f} |"
            )
        else:
            means = candidate["run_means"]
            lines.append(
                f"| {name} | {means['median']:.4f} | {means['iqr']:.4f} | "
                f"{means['min']:.4f} to {means['max']:.4f} | {candidate['run_p95s']['median']:.4f} |"
            )
    lines += ["", "## Render-thread C++ allocations per frame", ""]
    if paired:
        lines += ["| Case | Baseline count | Candidate count | Baseline bytes | Candidate bytes |", "|---|---:|---:|---:|---:|"]
    else:
        lines += ["| Case | Count | Bytes |", "|---|---:|---:|"]
    for name, values in summary["cases"].items():
        count = values["metrics"]["benchmark.frame.cpu_allocation_count"]
        size = values["metrics"]["benchmark.frame.cpu_allocation_bytes"]
        columns = [name]
        if paired:
            columns.append(f"{count['baseline']['run_means']['median']:.2f}")
        columns.append(f"{count['candidate']['run_means']['median']:.2f}")
        if paired:
            columns.append(f"{size['baseline']['run_means']['median']:.2f}")
        columns.append(f"{size['candidate']['run_means']['median']:.2f}")
        lines.append("| " + " | ".join(columns) + " |")
    lines += ["", "## Measurement boundaries", ""]
    for description in summary["interpretation"].values():
        lines.append(f"- {description}")
    lines += [
        "", "All workload and metadata checks passed. This verifies completion and comparability, "
        "not rendering correctness or statistical significance. `summary.json` contains paired p95 changes, "
        "submission/finish/preparation metrics, counter totals, and inclusive scope costs. `manifest.json` links every raw report "
        "and records executable hashes, host information, and run order. Full stdout/stderr and phase traces are retained per run.", "",
    ]
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, help="Baseline executable containing the same benchmark harness")
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="New or empty directory; results are never overwritten")
    parser.add_argument("--graphics-backend", choices=("native", "opengl", "vulkan", "d3d11", "metal", "null"), default="native")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--warmup-frames", type=int, default=30)
    parser.add_argument("--timeout-seconds", type=float, default=180)
    parser.add_argument("--frame-timing-only", action="store_true", help="Disable internal profiling for an outer-timing confirmation run")
    parser.add_argument("--cases", nargs="+", help="Case names, space or comma separated: " + ", ".join(case.name for case in CASES))
    args = parser.parse_args()
    if args.repetitions < 1 or args.frames < 1 or args.warmup_frames < 1:
        parser.error("repetitions, frames and warmup-frames must be positive")
    if args.timeout_seconds <= 0 or not math.isfinite(args.timeout_seconds):
        parser.error("timeout-seconds must be finite and positive")
    requested = {name for item in args.cases or [] for name in item.split(",")}
    unknown = requested - {case.name for case in CASES}
    if unknown:
        parser.error("unknown cases: " + ", ".join(sorted(unknown)))
    args.selected_cases = [case for case in CASES if not requested or case.name in requested]
    for name in ("candidate", "baseline"):
        executable = getattr(args, name)
        if executable is not None:
            executable = executable.resolve()
            if not executable.is_file():
                parser.error(f"{name} executable does not exist: {executable}")
            setattr(args, name, executable)
    args.output_dir = args.output_dir.resolve()
    if args.output_dir.exists() and (not args.output_dir.is_dir() or any(args.output_dir.iterdir())):
        parser.error("output-dir must be new or empty; choose another directory to preserve existing results")
    return args


def main() -> int:
    args = parse_args()
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    executables = {"candidate": args.candidate}
    if args.baseline:
        executables = {"baseline": args.baseline, "candidate": args.candidate}
    manifest: dict[str, Any] = {
        "schema_version": 1, "status": "running", "created_at": datetime.now(timezone.utc).isoformat(),
        "host": host_information(),
        "executables": {variant: executable_identity(path) for variant, path in executables.items()},
        "cases": [asdict(case) for case in args.selected_cases], "repetitions": args.repetitions,
        "frames": args.frames, "warmup_frames": args.warmup_frames,
        "graphics_backend": args.graphics_backend, "internal_profiling": not args.frame_timing_only,
        "run_order": [], "runs": [],
    }
    manifest_path = output / "manifest.json"
    write_json(manifest_path, manifest)
    reference_environment = None
    try:
        for repetition in range(args.repetitions):
            order = list(executables)
            if repetition % 2:
                order.reverse()
            for case in args.selected_cases:
                for variant in order:
                    print(f"[{repetition + 1}/{args.repetitions}] {case.name}: {variant}", flush=True)
                    manifest["run_order"].append({"case": case.name, "repetition": repetition, "variant": variant})
                    write_json(manifest_path, manifest)
                    result = run_one(executables[variant], variant, case, repetition, args, output)
                    environment = {key: result["metadata"][key] for key in ENVIRONMENT_KEYS}
                    if reference_environment is None:
                        reference_environment = environment
                    else:
                        check_metadata(environment, reference_environment, "build/renderer comparability")
                    manifest["runs"].append(result)
                    write_json(manifest_path, manifest)
        summary = summarize(manifest)
        write_json(output / "summary.json", summary)
        (output / "summary.md").write_text(markdown_summary(summary, manifest), encoding="utf-8")
        manifest["status"] = "complete"
    except BaseException as exc:
        manifest["status"] = "interrupted" if isinstance(exc, KeyboardInterrupt) else "failed"
        manifest["error"] = str(exc)
        raise
    finally:
        manifest["ended_at"] = datetime.now(timezone.utc).isoformat()
        write_json(manifest_path, manifest)
    print(output / "summary.md")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("benchmark suite interrupted; completed run artifacts were retained", file=sys.stderr)
        raise SystemExit(130)
    except (BenchmarkError, OSError, subprocess.SubprocessError, ValueError, KeyError) as exc:
        print(f"benchmark suite failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
