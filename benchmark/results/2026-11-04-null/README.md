# vnm_plot: benchmark foundation and allocation experiment

**Date:** 2026-11-04

**Status:** benchmark infrastructure implemented and exercised; production change retained as a separate experiment.

**Scope:** QRhi Null/offscreen on Linux. No raster or GPU performance claim.

## Result and recommendation

The primary deliverable is a repeatable suite of **14 named workloads**, with explicit input schedules, workload validation, paired process comparisons, machine-readable evidence, and documented measurement boundaries. It extends the existing production-frame benchmark rather than introducing an unrelated synthetic renderer.

A small label-processing change demonstrates that the suite can identify and verify useful work: an ordinary settled eight-series LINE or AREA frame falls from **190 to 152 render-thread C++ allocations** and from **13,952 to 13,056 requested allocation bytes**. That is **38 fewer allocations (20%) and 896 fewer requested bytes per frame**. The corresponding text-disabled control stays at **114 allocations and 9,312 bytes** in both binaries.

Timing is less conclusive. Longer, reduced-instrumentation runs show median paired mean-time reductions of **3.75%–7.18%** across the selected settled text workloads, but process variation is substantial and tail results are mixed. A range-scan-heavy conservative-access workload is slower, including when text is disabled. The benchmark foundation is useful now; the production change should remain an **experimental, separately reviewable commit**, pending investigation of that timing regression and validation on a raster backend. Allocation savings alone do not establish that the candidate is generally faster.

The evidence consists of **284 fresh processes and 675,200 measured frames** across three completed experiments. Statistical replicates are the paired processes, not the individual frames. The full archive retains every final raw JSON/text report, invocation, stdout/stderr log, phase trace, manifest, build provenance, and validation log. Compact results are in [measurements.json](measurements.json); operational instructions are in [the benchmark README](../../README.md).

## Workload coverage

Ordinary cases use 10,000 samples per series, a fixed seed of 42, a 1200 × 720 framebuffer, 4× MSAA request, preview enabled, explicit line/dot widths, and QRhi completion enabled. Timing runs disable pixel readback. Sources expose LOD 0.

| Cases | Purpose and controlled work |
|---|---|
| `warm-line-1`, `warm-line-8`, `warm-line-32` | Settled per-frame cost as series count grows |
| `dense-line-8` | 100,000 samples per series, all within the visible ten-second range; checks warm reuse with a denser uploaded dataset |
| `warm-area-8`, `warm-dots-8` | AREA and DOTS preparation/submission paths |
| `warm-stacked-area-8` | Stack composition and reuse |
| `pan-line-8` | A quarter-width view moving across fixed history on a 120-frame triangular cycle |
| `zoom-line-8` | Centered view changing from 10% to 100% of the available history on the same cycle |
| `append-line-8` | Exactly 16 new samples per series before each measured frame, with no ring overwrite |
| `wrap-line-8` | The same publication schedule into a full 10,000-sample ring, exercising overwrite and split spans |
| `warm-line-8-no-text` | Stable-access control without the text path |
| `warm-line-8-conservative` | Immutable fixture callbacks deliberately left without a stable semantic identity, forcing range queries/scans |
| `warm-line-8-conservative-no-text` | Matched range-scan control without the text path |

The dense settled case measures reuse after warmup, not rebuilding or uploading 100,000 samples on every frame. The appended data is generated synchronously before the render timer; its cost is separately reported as `benchmark.workload.publish_ms`. This separates rendering cost from producer scheduling. It does not measure producer-thread contention.

Input schedules are deterministic. Label fades still use elapsed real time, so moving/updating cases can retain different outgoing labels at different execution speeds. Those cases are not guaranteed pixel-identical frame sequences. Settled cases provide stronger timing controls, while fixed-clock algorithm tests cover fade behavior.

## Changes that make the measurements meaningful

1. **Match production layout feedback.** The offscreen runner now consumes `measured_vbar_width`, as the shipping widget does. Previously it kept its initial width, causing a layout miss on every otherwise static frame. An actual-renderer contract test checks that settled layout work is reused.
2. **Separate stable and conservative data access.** The fixture's stateless callbacks can now declare their real immutable semantics with `--stable-access`. A byte-layout key alone was insufficient, so the original fixture repeatedly scanned ranges even for unchanged data. Stable-access cases exercise production caching; conservative controls preserve the uncached contract. This is a benchmark-workload correction, not a claimed production optimization. Both compared binaries contain it.
3. **Specify work by frame count.** Publication, pan, and zoom advance by measured-frame index rather than wall-clock producer timing. Warmup does not consume the measured input sequence. Ring occupancy, publications, and overwrites must match exact expected totals.
4. **Reject incomparable runs.** The runner checks exit status, phase completion, exact frame counts, required metrics, finite/internally consistent observations, the executed command, workload metadata, renderer identity, build configuration, and backend selection. It rejects unexpected static scans/uploads and missing evidence. Null is accepted only when explicitly requested.
5. **Preserve pairing and variation.** Each variant runs in a fresh process. A/B order alternates with B/A between repetitions. Results include per-process means/p95s, paired changes, IQR and min/max; frame samples are not pooled as independent experiments. No machine-specific timing threshold is installed in CTest.
6. **Distinguish diagnostic and reduced-instrumentation runs.** Normal runs collect internal scopes and work counters. `--frame-timing-only` disables production renderer/source profiling while retaining outer timings, allocation hooks, and exact publication accounting. Unavailable internal counters remain absent rather than being reported as zero.

## Measurement boundaries

| Metric | Interpretation |
|---|---|
| `benchmark.frame.total_ms` | Wall time of the timed offscreen frame path, including QRhi begin/end calls, production planning/layout/text/series preparation, submission, and `--finish`; excludes setup and synchronous input generation |
| `benchmark.frame.submission_ms` | Work through end-of-frame; QRhi calls may synchronize, so this is not pure CPU submission time |
| `benchmark.frame.gpu_finish_ms` | Wall time of the extra finish call, not a hardware GPU timer |
| C++ allocation count/bytes | Render-thread allocation requests inside the measured scope, excluding profiler storage; not all `malloc` calls, other threads, GPU memory, retained memory, or peak memory |
| Upload/scan counters | Amount of renderer work requested; useful for explaining repeated work and validating equivalent workloads, even on Null |
| Internal scopes | Inclusive CPU-side wall times, aggregated by full scope name; parent/child times must not be summed |
| Setup/warmup observations | Preserved separately from measured frames; existing font/disk caches were not controlled for a cold-start study |

QRhi Null executes the production CPU preparation path and submission bookkeeping without rasterization. It cannot establish GPU throughput, application FPS, native presentation latency, or rendered-pixel equivalence. The suite also does not cover LOD switching, custom layers, the Qt Quick event/synchronization loop, or complete Logonomic integration.

## Experimental setup

| Experiment | Cases | Pairs per case | Measured frames/process | Warmup/process | Internal profiling | Processes |
|---|---:|---:|---:|---:|---|---:|
| `final-paired-outer` | 7 settled/control | 9 | 5,000 | 100 | Disabled | 126 |
| `final-paired-diagnostic` | 13 original suite cases | 5 | 240 | 30 | Enabled | 130 |
| `final-conservative-controls` | 2 conservative controls | 7 | 500 | 30 | Disabled | 28 |

All runs used the same two retained binaries, pinned to logical CPU 8, on an AMD EPYC 9V74 Linux host. OS: Ubuntu 24.04.3, kernel 6.18.57, glibc 2.39. Build: Release, GCC/G++ 13.3.0, `-O3 -DNDEBUG`, Qt 6.12.0, CMake 3.31.10, Ninja 1.13.2, ccache 4.9.1. Actual FreeType: **2.13.2 from the SDK**. A prepared 2.14.3 checkout was not selected by CMake. `QT_QPA_PLATFORM=offscreen`; actual QRhi backend/device: Null.

| Input | Exact revision or SHA-256 |
|---|---|
| Original repository base | `727eb9d085a37524c3a6ae67e16ba6a4def5d6dd` |
| Compiled benchmark baseline | `88d459dbbad9cff1a292b3ffa87be97970d371e6` |
| Compiled production candidate | `70638b6e65103b657daaa16e00bcea7fe8daf800` |
| Final Python suite and workload documentation | `5b40d72ed32b04c52bab15372acccd060c650dc9` |
| Baseline executable SHA-256 | `a421c9d8832519110d6bc0eb2253916798eef0d30512f5359f46673c51f29f5d` |
| Candidate executable SHA-256 | `77cfcf554e536ae0adfb4575ae8e7cf89623aa13fa99003e7df5088b96b21c9d` |
| SDK archive SHA-256 | `56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2` |
| vnm_msdf_text | `3e89f1358c88b46f7c68b99dd9bf76003b5a3db3` |
| vnm_fonts | `c9adbd52cd3bc983d313993436381522e15864e1` |
| vnm_cmake | `66d5ee14f4f69c952daa58a95a4759ad77a636f4` |
| glm 1.0.3 | `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed` |
| msdfgen 1.13 | `1874bcf7d9624ccc85b4bc9a85d78116f690f35b` |

The C++ benchmark harness is identical in the two compiled variants; only the label-membership production change differs. The same Python runner/explicit parameter values were used for each pair. The final runner commit also adds the second conservative control; that control was measured in its focused experiment, not retrospectively inserted into the original 13-case matrix. Executable hashes establish which binaries ran; an enclosing checkout's revision at run time alone would not establish their build provenance.

## Small production experiment

The existing fade tracker built a temporary `unordered_set` of current label keys on each update. The candidate reuses the retained per-label state: reset a membership flag during the existing state pass, mark each current label, and consult the flag when retiring absent labels. It removes temporary hash-set allocations without changing the intended fade progression or key ordering.

The production change is confined to `src/core/label_fade_tracker.h` and one field in `include/vnm_plot/rhi/text_renderer.h`: eight inserted lines and five removed lines. It is commit `70638b6`, separate from the benchmark commits. Behavior tests cover duplicate/unordered keys, reordering, empty layouts, immediate repopulation, animated disappearance, return during a fade, fade reversal, old-key expiry and replacement, for both fractional `double` and `int64_t` keys. The tests check emitted/grid alpha and retained keys rather than the new flag.

### Allocation results

These settled-workload counts are exact and unchanged across the repetitions of the reported experiments. They are allocation traffic, not retained memory savings.

| Case | Baseline allocations/frame | Candidate allocations/frame | Baseline requested bytes/frame | Candidate requested bytes/frame |
|---|---:|---:|---:|---:|
| `warm-line-1` | 99 | 61 | 7,400 | 6,504 |
| `warm-line-8` | 190 | 152 | 13,952 | 13,056 |
| `warm-line-32` | 502 | 464 | 36,416 | 35,520 |
| `dense-line-8` | 190 | 152 | 13,952 | 13,056 |
| `warm-area-8` | 190 | 152 | 13,952 | 13,056 |
| `warm-dots-8` | 186 | 142 | 14,464 | 13,424 |
| `warm-line-8-no-text` | 114 | 114 | 9,312 | 9,312 |
| `warm-stacked-area-8` | 246 | 204 | 24,128 | 23,152 |
| `warm-line-8-conservative` | 206 | 168 | 14,464 | 13,568 |
| `warm-line-8-conservative-no-text` | 130 | 130 | 9,824 | 9,824 |

In these fixtures, the ordinary LINE/AREA allocation reduction is independent of the series count because it removes label-membership bookkeeping. The text-disabled controls remain unchanged. DOTS and stacked AREA have different label populations and save 44 and 42 allocations per frame respectively. Dynamic allocation summaries remain in the complete diagnostic output; different wall-clock fade states make their frame-to-frame totals less suitable as an exact causal control.

### Longer settled-frame timing comparison

Nine pairs, 5,000 measured frames/process, internal profiling disabled. Mean columns are medians of per-process means. “Paired change” is the median of `100 × (candidate_i / baseline_i − 1)`; it need not equal the ratio of the two displayed medians. “Paired p95 change” applies the same calculation to each pair's per-process p95. Negative means lower cost. IQR/min–max describe process variation, not confidence intervals.

| Case | Baseline mean (ms) | Candidate mean (ms) | Median paired change | Paired min–max | Paired IQR | Paired p95 change |
|---|---:|---:|---:|---:|---:|---:|
| `warm-line-1` | 0.0226 | 0.0215 | -3.75% | -33.48 to +2.26% | 1.71 pp | -0.69% |
| `warm-line-8` | 0.0314 | 0.0298 | -7.18% | -16.50 to +4.81% | 13.00 pp | -20.50% |
| `warm-line-32` | 0.0640 | 0.0634 | -3.95% | -16.46 to +4.28% | 13.33 pp | -0.03% |
| `dense-line-8` | 0.0321 | 0.0302 | -6.62% | -23.18 to +4.92% | 5.24 pp | -8.49% |
| `warm-area-8` | 0.0309 | 0.0286 | -7.18% | -36.86 to +4.91% | 9.79 pp | -4.56% |
| `warm-dots-8` | 0.0335 | 0.0326 | -4.40% | -21.63 to +24.63% | 5.72 pp | -4.78% |
| `warm-line-8-no-text` | 0.0117 | 0.0116 | +0.58% | -38.05 to +21.69% | 10.88 pp | +2.17% |

The text-workload median paired mean reductions amount to roughly **0.85–2.32 microseconds/frame**. Every selected text workload has both faster and slower individual pairs. Several individual paired p95 comparisons are worse, even though the displayed text-workload median paired p95 changes are negative. The no-text control has a +0.58% median paired mean change with much wider spread. These observations support a modest possible CPU-path benefit, not a general speedup or improved tail-latency claim.

### Complete diagnostic matrix

Five pairs, 240 measured frames/process, internal profiling enabled. These shorter, instrumented results help explain work and identify cases for follow-up. They are not substituted for the longer confirmation runs.

| Case | Baseline mean (ms) | Candidate mean (ms) | Median paired change | Paired min–max | Paired IQR | Paired p95 change |
|---|---:|---:|---:|---:|---:|---:|
| `warm-line-1` | 0.0278 | 0.0251 | -9.63% | -38.77 to -5.12% | 9.16 pp | -22.68% |
| `warm-line-8` | 0.0460 | 0.0358 | -8.18% | -37.81 to +9.18% | 29.42 pp | -13.66% |
| `warm-line-32` | 0.0700 | 0.0670 | +0.67% | -7.01 to +7.73% | 3.07 pp | +13.46% |
| `dense-line-8` | 0.0350 | 0.0343 | -7.54% | -22.03 to +0.76% | 16.61 pp | -10.67% |
| `warm-area-8` | 0.0381 | 0.0348 | -2.99% | -39.37 to +24.30% | 17.16 pp | -9.55% |
| `warm-stacked-area-8` | 0.0478 | 0.0456 | -3.62% | -6.42 to +2.47% | 2.69 pp | -2.58% |
| `warm-dots-8` | 0.0365 | 0.0347 | -8.56% | -11.68 to +52.96% | 48.32 pp | -13.35% |
| `pan-line-8` | 0.7136 | 0.6487 | -0.66% | -25.36 to +0.92% | 3.20 pp | -2.71% |
| `zoom-line-8` | 1.5489 | 1.4255 | +0.23% | -24.28 to +6.89% | 3.16 pp | -2.44% |
| `append-line-8` | 10.6955 | 11.0468 | +3.88% | -6.61 to +10.68% | 6.39 pp | +9.81% |
| `wrap-line-8` | 9.8657 | 9.1146 | -7.00% | -8.96 to -1.60% | 5.30 pp | -11.77% |
| `warm-line-8-no-text` | 0.0158 | 0.0146 | -9.39% | -37.59 to +39.04% | 6.89 pp | -21.40% |
| `warm-line-8-conservative` | 2.6868 | 2.9864 | +8.72% | +2.72 to +27.40% | 4.64 pp | +6.01% |

The append workload's +3.88% median paired change has mixed-sign pairs and remains unresolved. The conservative workload's +8.72% median paired change is positive in **all five pairs**, so it cannot be dismissed as harmless noise. Its range query and range scan counts are identical between variants: 3,840 of each per 240-frame run. Internal scopes put the additional time primarily before render passes, outside the changed text-label path. This localizes the observation but does not establish a cause.

### Follow-up on the conservative slowdown

Seven pairs, 500 measured frames/process, internal profiling disabled. The added no-text control keeps the same conservative range-scan contract while skipping the changed text path.

| Case | Baseline mean (ms) | Candidate mean (ms) | Median paired change | Paired min–max | Paired IQR | Paired p95 change |
|---|---:|---:|---:|---:|---:|---:|
| `warm-line-8-conservative` | 2.6562 | 2.8748 | +4.12% | -1.33 to +16.22% | 6.72 pp | +3.25% |
| `warm-line-8-conservative-no-text` | 2.7007 | 2.7973 | +7.50% | +0.99 to +9.77% | 4.30 pp | +3.90% |

The slowdown persists, including **+7.50% median paired change with text disabled**, with all seven no-text pairs slower. That control retains exactly 130 allocations and 9,824 requested bytes per frame in both binaries. Executing the changed label logic is therefore not necessary for this observed slowdown. Binary layout, cache/allocator effects, or environmental effects remain possible explanations; none is established by these data. The result is a reason to investigate before accepting the production change as a general performance improvement. The allocation finding remains directly demonstrated.

## What the work counters suggest for later optimization

Settled stable-access LINE/AREA/DOTS cases have zero measured primary uploads, line-window uploads, monotonicity scans and auto-range scans after warmup. The dense case also reuses this work; its similar warm CPU cost is expected. A future change that accidentally invalidates those caches will fail the workload checks even if wall-time noise hides the regression.

Streaming cases expose a substantially different cost. In `append-line-8`, each measured frame publishes only **128 new samples** in total, but the diagnostic baseline performs **16 primary uploads, 16 line-window uploads, 16 auto-range queries and 16 range scans per frame**. Across 240 measured frames it requests 673,689,600 primary-upload bytes and 673,812,480 line-window-upload bytes, about **5.35 MiB of combined requested upload traffic per frame**, and visits 45,803,520 samples in monotonicity scans. Both variants perform the same counted work. Those numbers describe renderer requests on Null, not hardware transfer bandwidth.

This makes incremental range/window processing a concrete later investigation: reduce repeated work while holding publications, occupancy, overwrites and visible data fixed, then confirm frame-time improvement on a native backend. No such production optimization is included here. The conservative-access cases independently reveal the cost of repeated range scans, without conflating the public semantic contract with an implementation improvement.

## Validation and remaining limits

All **10 selected CTest registrations passed**: `ring_buffer_tests`, `brownian_generator_tests`, `benchmark_profiler_tests`, `benchmark_data_source_tests`, `benchmark_phase_trace_flush`, `benchmark_shared_frame_pipeline`, `benchmark_workload_contracts`, `benchmark_suite_validation`, `CoreAlgo`, and `LayoutCalculator`. The suite-validation registration runs nine Python checks; those nine checks were rerun successfully after the final runner changes. `git diff --check` also passed.

The workload tests exercise the real Null renderer and check exact frame/publication counts, wrapped occupancy, motion, settled layout/range/upload reuse, frame-only mode and invalid input. The phase-trace flush test now defaults to Null because it tests durable output and external termination. Existing native smoke/readback tests retain their raster-backend requirement.

This is focused validation, not a claim that every repository test or platform passed. Raster initialization was unavailable here: Xvfb could not create local sockets, minimal-EGL failed QPA surface configuration, and Qt's offscreen platform rejected Vulkan instance creation. Consequently **pixel equivalence, native presentation and GPU performance remain unverified**. The algorithm tests support fade semantics but do not replace native rendering/readback checks.

Before promoting the experimental production change, reproduce the conservative control on a quieter machine/native renderer, investigate the pre-pass slowdown, and run the existing native text/readback tests. For future dynamic text optimizations, a controlled animation clock would make paired visual sequences stronger. These are limits on accepting that optimization, not blockers to using the benchmark suite.

## Reproduce and extend

Build the baseline at `88d459d` and the candidate at `70638b6` with identical dependencies and Release configuration. Use the final suite script from `5b40d72` (or its documentation-only descendants) for both. On a normal supported desktop, select an explicit backend such as `d3d11`, `metal`, `opengl` or `vulkan`; the runner rejects silent Null fallback. Refer to [building instructions](../../../docs/building.md) for dependency setup.

Run the diagnostic suite with both executable paths:

```bash
python3 benchmark/tools/run_suite.py \
  --baseline /absolute/baseline/vnm_plot_benchmark \
  --candidate /absolute/candidate/vnm_plot_benchmark \
  --graphics-backend opengl --repetitions 5 \
  --warmup-frames 30 --frames 240 \
  --output-dir results/diagnostic
```

The current default catalog includes all 14 cases. To reproduce the original 13-case diagnostic experiment exactly, pass those cases explicitly, excluding `warm-line-8-conservative-no-text`. Every original command is retained in the evidence archive's per-run `invocation.json`.

For the longer confirmation experiment:

```bash
python3 benchmark/tools/run_suite.py \
  --baseline /absolute/baseline/vnm_plot_benchmark \
  --candidate /absolute/candidate/vnm_plot_benchmark \
  --graphics-backend opengl --repetitions 9 \
  --warmup-frames 100 --frames 5000 --frame-timing-only \
  --cases warm-line-1 warm-line-8 warm-line-32 dense-line-8 \
    warm-area-8 warm-dots-8 warm-line-8-no-text \
  --output-dir results/outer
```

For the focused conservative controls, use seven repetitions, 30 warmup frames, 500 measured frames, `--frame-timing-only`, and `--cases warm-line-8-conservative warm-line-8-conservative-no-text`.

For the Linux CPU-only environment used here, prefix the command with `taskset -c 8`, export `QT_QPA_PLATFORM=offscreen`, and replace the graphics backend with explicit `null`. Do not compare the resulting numbers directly against a raster-backend run. Use a new output directory each time; the runner deliberately refuses to overwrite evidence.

Keep correctness checks separate from timing runs, retain raw evidence and exact build revisions, inspect work counters before interpreting elapsed time, and repeat without internal profiling when an apparent improvement matters. Accept a production optimization only after considering the complete workload matrix and its controls.
