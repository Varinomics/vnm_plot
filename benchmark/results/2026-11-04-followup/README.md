# Benchmark continuation — 4 November 2026

## Outcome

This continuation fixes the frame-only measurement boundary and completes **144 additional measured processes**. The original evidence remains in [the first report](../2026-11-04-null/README.md). All new process-level results, plans, paired summaries and source identities are in [measurements.json](measurements.json). The accompanying follow-up archive contains full raw reports, commands, phase traces, assembly analysis and validation logs.

The label-membership experiment consistently removes **38 render-thread C++ allocations and 896 requested bytes per frame** in the measured text workloads. Timing does not establish a general speedup. The retained two-binary comparison reproduces a conservative no-text slowdown, whereas the same-executable diagnostic does not show a consistent penalty on that control. This narrows the investigation without identifying a hardware-level cause.

The benchmark correction is on [PR #30](https://github.com/Varinomics/vnm_plot/pull/30). The production optimization remains separate. The diagnostic dispatch exists only on a reproduction branch; it is not a proposed production setting. Native raster qualification remains outstanding.

## Measurement correction: workload contract v3

The old `--frame-timing-only` path still created `renderer.frame.finish`, read the preparation profiler through its mutex, and recorded submission/finish observations before stopping the frame timer. Its cold preparation observation also reported zero when internal profiling was disabled. Existing validation rejected only the exact `renderer.frame` scope and missed the finish scope.

The corrected benchmark now:

- Uses the snapshot's disabled profiler for the finish scope and skips the internal preparation query when profiling is off.
- Holds submission and finish durations in local variables, then records observations after the frame timestamp and allocation boundary have ended.
- Omits unavailable cold preparation timing instead of reporting a synthetic zero.
- Rejects every `renderer.*` scope or observation, internal planning timing and internal cold preparation timing in a frame-only report.
- Requires workload contract **3**, with tests rejecting historical contract 2.

Optional pixel readback and pixel validation retain their previous position inside the measurement boundary. They were disabled in these timing campaigns. Diagnostic profiling remains available as a separate mode. Frame-only measurement still has clock and allocation-observer overhead.

The new validator rejects the original retained raw finish scope and, independently, the unavailable cold preparation metric. The archive preserves both reproductions. The correction addresses the measurement contract; no isolated measurement attributes the original two-binary slowdown to this bookkeeping defect.

**Historical v2 and corrected v3 timings must not be directly compared.** Both variants in each new diagnostic campaign use exactly the same corrected harness.

## Fixed experiments and interpretation

Two plans were recorded before their respective campaigns. Each case uses eight pairs of fresh processes, alternating A/B and B/A order, CPU 8 affinity, explicit QRhi Null and frame-only timing. No measured repetition was discarded or added after inspecting its outcome.

| Campaign | Comparison | Cases | Warmup / measured frames per process | Processes | Contract |
|---|---|---:|---:|---:|---:|
| `retained-aa` | Original baseline executable against itself | 2 | 100 / 1,000 | 32 | 2 |
| `retained-ab` | Original baseline and candidate executables | 3 | 100 / 1,000 | 48 | 2 |
| `same-image-aa` | One diagnostic executable, original algorithm in both variants | 1 | 20,000 / 50,000 | 16 | 3 |
| `same-image-ab-settled` | One diagnostic executable, original versus candidate algorithm | 1 | 20,000 / 50,000 | 16 | 3 |
| `same-image-ab-conservative` | Same executable and algorithm selection, text and no-text controls | 2 | 100 / 1,000 | 32 | 3 |

Mean columns below are medians of per-process means. Percentage changes are medians of **within-pair** percentages, not ratios of the displayed medians. Positive means more time. Ranges and IQRs describe observed variation; they are not confidence intervals. Each fresh process is the replicate. Frames are not pooled as independent replicates, and no significance threshold was selected after viewing results.

Per-process means, counts, minima and maxima include all measured observations. The existing deterministic reservoir retains up to 8,192 samples per metric for percentiles. Thus the 50,000-frame runs' p95 values are reservoir estimates. Full paired p95 results and IQRs remain in `measurements.json`.

These are offscreen CPU-preparation-path wall times, including QRhi completion with `--finish`, and observed render-thread C++ allocation traffic. They do not measure hardware GPU throughput, application FPS, presentation latency or pixel equivalence. Allocation observations exclude malloc-only traffic and other threads. Synthetic producer input generation is outside the render timer and reported separately.

## Retained binaries: the original penalty reproduces

| Case and comparison | Baseline mean ms | Candidate mean ms | Paired change | Paired min–max | Slower pairs |
|---|---:|---:|---:|---:|---:|
| A/A conservative, no text | 2.6358 | 2.6464 | +0.71% | -2.20 to +8.47% | 5/8 |
| A/A stable, no text | 0.0111 | 0.0117 | +5.72% | -2.18 to +21.36% | 5/8 |
| A/B conservative, text | 2.7276 | 2.9174 | +7.25% | -13.17 to +16.16% | 7/8 |
| A/B conservative, no text | 2.7849 | 2.8971 | **+5.45%** | +0.49 to +17.45% | **8/8** |
| A/B stable, no text | 0.0115 | 0.0109 | -2.36% | -14.40 to +0.16% | 1/8 |

The conservative no-text penalty occurs in all eight pairs despite balanced process order. The same-baseline-executable control has mixed signs and a much smaller median on that workload. A simple assumption that the second process is always slower does not explain this observation.

The roughly 11-microsecond stable no-text A/A control varies substantially in relative terms. This motivated longer measurement and warmup windows for the settled-text same-executable diagnostic. The two A/A workloads are different; their percentage ranges are not interchangeable noise estimates.

The retained conservative text case still has exactly 206 versus 168 allocations per frame. Conservative no-text remains 130 in both binaries; stable no-text remains 114 in both. Allocation and timing evidence support different levels of certainty.

## One executable: algorithm comparison and controls

The diagnostic executable contains both original helper definitions, copied exactly apart from their function names. A cached environment selection is initialized on the first text-helper call during warmup. Mode `0` selects the original temporary set; mode `1` selects the state flag. A single stderr marker records the actual selection. With text disabled, the helper never reads the switch and emits no marker.

All 64 diagnostic processes use the same executable path and SHA-256. Text runs have the expected marker, no-text runs have none, and source and mode identities are retained. Both modes share the candidate state structure. The additional flag fits existing padding: the retained binaries both allocate 120-byte label-map nodes. The six corrected benchmark files are identical between the foundation and diagnostic source trees.

| Case and comparison | Original-mode mean ms | Other-mode mean ms | Paired change | Paired min–max | Slower pairs |
|---|---:|---:|---:|---:|---:|
| A/A stable text, original mode twice | 0.03440 | 0.03411 | -1.42% | -10.27 to +6.72% | 3/8 |
| A/B stable text | 0.03508 | 0.03352 | **-4.24%** | -24.07 to +6.97% | 1/8 |
| A/B conservative text | 2.81591 | 2.75131 | -2.22% | -3.78 to +10.79% | 2/8 |
| A/B conservative, no text | 2.75946 | 2.73162 | **-0.71%** | -3.83 to +5.55% | **4/8** |

The stable-text algorithm comparison is faster in seven of eight pairs, but its range overlaps the identical-mode control and includes a slower result. The paired p95 median change is +0.47%, versus +0.34% for the identical-mode control. These observations do not justify a general frame-time or tail-latency improvement claim.

The conservative no-text comparison is evenly split in sign. It does not reproduce the consistent retained-binary penalty. Conservative text also has mixed outcomes; the large range relative to its small median limits timing conclusions.

Allocation results are exact across the measured frames:

| Same-executable workload | Original count/frame | Candidate count/frame | Original requested bytes/frame | Candidate requested bytes/frame |
|---|---:|---:|---:|---:|
| Stable text | 190 | 152 | 13,952 | 13,056 |
| Conservative text | 206 | 168 | 14,464 | 13,568 |
| Conservative, no text | 130 | 130 | 9,824 | 9,824 |

Every v3 process has identical allocation minimum and maximum throughout its measurement window. The 20,000-frame warmups last 641–971 ms; the conservative 100-frame warmups last 265–392 ms (299–392 ms for the text cases). All text warmups exceed the configured 180 ms label-fade interval, and measured allocation traffic remains constant throughout each process.

### What this narrows down

Inspection of the original binaries finds identical normalized instruction sequences for the range scan, sample reader, callbacks and frame renderer, at different code addresses. Relocation normalization is documented in the archived analysis helper; this does not mean their raw instruction bytes are all identical. The inspected dependencies and RUNPATH agree, and plot code is statically linked into each binary.

The changed label algorithm is skipped by the no-text workload. Holding the executable fixed removes build-to-build relative code placement as a difference between modes; the resulting control has no consistent penalty. Code placement remains a plausible explanation for the retained-binary behavior, but no instruction-cache, branch-predictor or other hardware mechanism was measured. Process ASLR and host variation remain.

The diagnostic also uses v3 while the retained binaries use v2. Consequently these campaigns cannot isolate code placement as the cause. The supported conclusion is that the allocation reduction is repeatable and the original slowdown is not a consistent consequence of selecting the candidate helper in this diagnostic build.

## Build identity and validation

Both fresh builds use Linux development SDK `2026.10.23.1`: GCC 13.3.0, Qt 6.12.0, CMake 3.31.10, Ninja 1.13.2, Release `-O3 -DNDEBUG`, text enabled, and the same explicit dependency source overrides. Configuration uses one queued slot and builds use four slots with matching parallelism.

Owned dependency default heads were checked at continuation start and still match the retained inputs. Full dependency revisions are:

| Dependency | Revision |
|---|---|
| `vnm_msdf_text` | `3e89f1358c88b46f7c68b99dd9bf76003b5a3db3` |
| `vnm_fonts` | `c9adbd52cd3bc983d313993436381522e15864e1` |
| `vnm_cmake` | `66d5ee14f4f69c952daa58a95a4759ad77a636f4` |
| `glm` | `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed` |
| `msdfgen` | `1874bcf7d9624ccc85b4bc9a85d78116f690f35b` |

The selected FreeType is the SDK's 2.13.2. Source inventories covering the project and dependencies matched after builds and focused tests: 2,101 files for the foundation and 2,100 for the diagnostic. Executable hashes remained unchanged after all campaigns.

| Built source | Published commit with identical Git tree | Local compiled revision |
|---|---|---|
| Corrected benchmark foundation | [`ca15d8c`](https://github.com/Varinomics/vnm_plot/commit/ca15d8cea331fb70a923bcb1a8c75ede3493882f) | `043e0a61d6e0a4127399aa341c397249634e4791` |
| Diagnostic-only reproduction | [`94a3d43`](https://github.com/Varinomics/vnm_plot/commit/94a3d43388f91ea5213c2057c3f5f20fad2091a2) | `41a0088fe1faf4a85b21638631e32b80501c6c70` |

The API-created publication commits have new identifiers; the Git tree IDs match exactly. Later foundation commits update only the continuation report and measurements. The diagnostic source is on `diagnostic/label-membership-same-image-20261104`.

Executable SHA-256 values:

```text
retained baseline  a421c9d8832519110d6bc0eb2253916798eef0d30512f5359f46673c51f29f5d
retained candidate 77cfcf554e536ae0adfb4575ae8e7cf89623aa13fa99003e7df5088b96b21c9d
v3 foundation     7eb10990e79e5215968eb14e546b799c20d5dac605ab922481c1fd2478ea477b
v3 diagnostic     4e5c71ad16441df5337e5dfd28b5fda018cc9cb5848beca4bf68c12893189473
```

The historical compiled revisions are baseline `88d459d` and candidate `70638b6`, preserved in the original recovery bundle. The v2 continuation runner is published initial foundation `a15293f`.

Validation completed:

- **10/10 selected foundation CTest registrations:** ring buffer, Brownian generator, profiler, data source, phase-trace flush, shared frame pipeline, workload contracts, suite validation, `CoreAlgo` and `LayoutCalculator`.
- **3/3 diagnostic registrations in each mode:** `CoreAlgo`, workload contracts and suite validation, for six successful mode-specific registrations.
- The runner's **10 Python tests** pass, including frame-only and v2-rejection coverage. These are included in the suite-validation registrations above.
- All 144 measured processes complete their workload and phase checks. All 64 v3 reports satisfy the corrected frame-only contract and constant allocation bounds.
- An independent read-only review recomputed paired results and checked all raw reports, mode markers, hashes, allocation bounds and warmup phases. Its script and JSON results are included in the archive.

An early build command used CTest name `ring_buffer_tests` as a Ninja target; it failed before compilation. The corrected target names built successfully. The failed invocation and successful build/test logs are both preserved. This was a command-name error, not a compiler or test failure.

## Native rendering and remaining qualification

One bounded SDK Xvfb startup attempt exited 1 because local/unix sockets could not be opened. A minimal AF_UNIX capability check returned EPERM. The display process ended. Previous EGL and Vulkan initialization failures remain recorded in the first report. No permission or syscall workaround was attempted.

`FontRendererDrawRanges` and `SampleUploadReuse` still require a supported native display and have not passed in this continuation. Null is not substituted into these raster/readback gates. Hosted native CI was not dispatched.

The next production decision requires these native gates and a corrected-v3 comparison of separately built production baseline/candidate executables on a supported rendering host, retaining text and no-text controls. The same-executable diagnostic can help interpret that comparison, but does not replace it. The current foundation is useful for further measurements while the production experiment remains unpromoted.

## Reproduction

Use the published source revisions above with the repository's [Linux SDK instructions](https://github.com/Varinomics/logonomic/blob/master/docs/linux_assistant_sdk.md). The archive retains full configure/build logs, source inventories and both fixed plans. Build both revisions with the recorded dependency overrides and Release configuration. Do not combine a v2 executable with the v3 suite.

After activating the SDK, set paths for the diagnostic checkout and its built executable. CPU 8 must be available; record any changed affinity.

```bash
export QT_QPA_PLATFORM=offscreen
VNM_DIAGNOSTIC_SOURCE=/absolute/path/to/diagnostic-checkout
VNM_DIAGNOSTIC_BIN=/absolute/path/to/build/benchmark/vnm_plot_benchmark

# Identical-mode control.
VNM_PLOT_LABEL_MEMBERSHIP_DIAGNOSTIC=0 taskset -c 8 python3 \
  "$VNM_DIAGNOSTIC_SOURCE/benchmark/tools/run_suite.py" \
  --baseline "$VNM_DIAGNOSTIC_BIN" --candidate "$VNM_DIAGNOSTIC_BIN" \
  --graphics-backend null --repetitions 8 --warmup-frames 20000 --frames 50000 \
  --frame-timing-only --cases warm-line-8 --output-dir same-image-aa

# The diagnostic driver sets and verifies the mode for each variant.
taskset -c 8 python3 \
  "$VNM_DIAGNOSTIC_SOURCE/benchmark/tools/run_same_image_diagnostic.py" \
  --baseline "$VNM_DIAGNOSTIC_BIN" --candidate "$VNM_DIAGNOSTIC_BIN" \
  --graphics-backend null --repetitions 8 --warmup-frames 20000 --frames 50000 \
  --frame-timing-only --cases warm-line-8 --output-dir same-image-ab-settled

taskset -c 8 python3 \
  "$VNM_DIAGNOSTIC_SOURCE/benchmark/tools/run_same_image_diagnostic.py" \
  --baseline "$VNM_DIAGNOSTIC_BIN" --candidate "$VNM_DIAGNOSTIC_BIN" \
  --graphics-backend null --repetitions 8 --warmup-frames 100 --frames 1000 \
  --frame-timing-only \
  --cases warm-line-8-conservative warm-line-8-conservative-no-text \
  --output-dir same-image-ab-conservative
```

For the diagnostic CTest runs, set `VNM_PLOT_LABEL_MEMBERSHIP_DIAGNOSTIC` to `0` and then `1`, running `CoreAlgo`, `benchmark_workload_contracts` and `benchmark_suite_validation` in each mode. The ordinary foundation has no mode environment variable. Retained v2 comparison commands are in the original manifests; their executable files and original Git objects remain in the original recovery archive. The new archive excludes SDKs, dependency checkouts and duplicated binaries.
