# Benchmark continuation — 4 November 2026

## Published checkpoint

This follows the original benchmark report in `../2026-11-04-null/README.md`.
The original measurements and source remain intact. The production label-membership
experiment remains separate from the benchmark foundation.

The retained-binary comparison reproduces the conservative no-text slowdown:
**all eight paired runs are slower, with a +5.45% median paired change**.
The identical-binary control has a +0.71% median change on that workload, with
mixed signs. This supports investigating a difference between these particular
binaries; it does not identify the cause or qualify the production experiment.

The stable no-text identical-binary comparison has a +5.72% median change,
showing that small changes in this very short workload are unresolved on this host.
No general speedup is inferred from those timings.

## Fixed initial experiment

Before running, `initial-plan.json` specified eight alternating A/B and B/A pairs,
100 warmup frames and 1,000 measured frames per process, CPU 8 affinity, explicit
QRhi Null, and reduced internal profiling. There were 32 A/A and 48 A/B processes.
All 80 invocations completed and passed the existing workload-v2 validator.
No repetitions were discarded or added after seeing the outcome.

The A/A labels refer to the same baseline executable; they do not represent
changed code. Mean columns below are medians of per-process means. Changes are
medians of within-pair percentages; min–max is descriptive spread, not a confidence
interval. Positive means more time. IQRs, per-process metrics and exact order are
retained in `measurements.json`.

| Experiment | Case | Baseline mean ms | Candidate mean ms | Paired change | Paired min–max | Slower pairs |
|---|---|---:|---:|---:|---:|---:|
| retained-aa | `warm-line-8-conservative-no-text` | 2.6358 | 2.6464 | +0.71% | -2.20 to +8.47% | 5/8 |
| retained-aa | `warm-line-8-no-text` | 0.0111 | 0.0117 | +5.72% | -2.18 to +21.36% | 5/8 |
| retained-ab | `warm-line-8-conservative` | 2.7276 | 2.9174 | +7.25% | -13.17 to +16.16% | 7/8 |
| retained-ab | `warm-line-8-conservative-no-text` | 2.7849 | 2.8971 | +5.45% | +0.49 to +17.45% | 8/8 |
| retained-ab | `warm-line-8-no-text` | 0.0115 | 0.0109 | -2.36% | -14.40 to +0.16% | 1/8 |

The conservative text case retains the exact allocation reduction, 206 to 168
allocations/frame. Both conservative no-text variants retain 130 allocations/frame;
both stable no-text variants retain 114. Timing and allocation conclusions are
kept separate.

## Inputs and measurement boundary

- Baseline executable SHA-256: `a421c9d8832519110d6bc0eb2253916798eef0d30512f5359f46673c51f29f5d`.
- Candidate executable SHA-256: `77cfcf554e536ae0adfb4575ae8e7cf89623aa13fa99003e7df5088b96b21c9d`.
- Original compiled revisions: baseline `88d459dbbad9cff1a292b3ffa87be97970d371e6`, candidate `70638b6e65103b657daaa16e00bcea7fe8daf800`.
- Runner: foundation revision `a15293f39b52bbee54e129d59e6739e74a2970e6`.
- Qt 6.12, GCC 13.3 Release, original dependencies and settings; executable hashes
  were checked before use. Owned dependency default heads still match the retained
  inputs when checked at continuation start.

These are QRhi Null CPU-preparation wall times and render-thread C++ allocation
traffic. They do not establish hardware GPU cost, application FPS, presentation
latency or pixel equivalence. The short settled control is particularly sensitive
to observed host variation. Internal profiling is reduced, not zero-overhead.

## Concrete benchmark defect found

The workload-v2 `--frame-timing-only` path still emits `renderer.frame.finish`,
reads the preparation profiler through its mutex, and stores submission/finish
observations before capturing the total-frame timestamp. This violates the stated
absence of internal renderer scopes and includes avoidable measurement bookkeeping
inside the frame total. Existing tests check only the exact `renderer.frame` scope
and miss the finish scope.

A bounded benchmark-only correction is being prepared: guard the remaining
internal profiler work, record durations locally, stop the timing/allocation
boundary before storing observations, and reject every `renderer.` scope or
observation in frame-only validation. The workload contract will advance to v3,
so v2 and v3 timings cannot silently be mixed. The v2 evidence above remains
historical and is not relabeled after the correction.

This bookkeeping discrepancy is far too small to explain the millisecond-scale
conservative slowdown. It is an independent measurement-quality issue.

## Native-rendering limit

One fresh SDK Xvfb startup attempt exited 1 because local/unix sockets could not
be opened. A minimal AF_UNIX capability check returned EPERM. The display process
is finished. The previous EGL and Vulkan platform-initialization limitations remain
recorded; no permission or syscall workaround was attempted.

The maintained `FontRendererDrawRanges` and `SampleUploadReuse` raster/readback
gates still require a supported native display. They have not passed in this
continuation. Null is not substituted into those raster gates.

## Next bounded diagnostic

Read-only disassembly inspection finds the same range-scan, sample-reader,
callback and frame-renderer instructions after relocation normalization, at
different code addresses. The additional label-state flag fits the existing map
node allocation size. The changed label algorithm is skipped with text disabled.
This supports code placement as a hypothesis, not a proven cache mechanism.

One diagnostic executable will contain both exact label implementations, selected
before measured work. Keeping a single executable holds code placement constant
while varying the label algorithm. The no-text control must not consult the switch.
Its source and switch will remain diagnostic-only. The corrected benchmark harness
will be used on both modes; source, binary and mode identities will be retained.

The production experiment remains unpromoted while this question and native
rendering qualification are open.
