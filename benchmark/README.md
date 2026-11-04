# vnm_plot benchmark

For an initial measured baseline and a small allocation experiment, see the
[2026-11-04 results](results/2026-11-04-null/README.md), including the unresolved
conservative-access timing regression and the limits of the Null backend.

`vnm_plot_benchmark` measures the complete vnm_plot render path with synthetic
Bars or Trades data. It supports multiple independent series, static data, and
live publication through ring buffers.

The executable has two backend controls:

- `--backend qrhi|qrhi-offscreen` selects the presentation path.
- `--graphics-backend native|d3d11|metal|vulkan|opengl|null` selects the
  graphics API. `native` accepts whichever non-Null API Qt selects; `null` is
  used only when requested explicitly.

For repeatable runs, use an exact frame count and explicit workload settings:

```powershell
build-text\benchmark\vnm_plot_benchmark.exe `
  --backend qrhi-offscreen `
  --graphics-backend native `
  --static --data-type bars --render-style line `
  --series-count 8 --seed 42 `
  --warmup-frames 2 --frames 120 --finish `
  --scenario static-bars-line-8 `
  --output-dir benchmark-results
```

Each successful run writes a human-readable `.txt` report, a raw `.json`
report, and `benchmark_phase_trace.jsonl`. The JSON report records the command,
build type, compiler, Qt version, requested and actual QRhi backend/device,
renderer environment, workload settings, raw observations, and calculated
p50/p95/p99 values. High-rate metrics use deterministic reservoir sampling;
aggregate count/total/min/max values are not sampled.

Snapshot accounting distinguishes logical `benchmark.snapshot.view_bytes` from
physical `benchmark.snapshot.copied_bytes`. Render-thread CPU allocation
count/bytes cover the measured frame while excluding the profiler's own
storage. `renderer.frame.gpu_buffer_allocation_*` separately counts QRhi buffer
creation.

The phase trace flushes aggregate setup, backend creation, warm-up,
measurement, generator shutdown, and completion boundaries outside the
measured frame loop. A frame-specific record is emitted only on failure, so a
terminated run shows its last completed phase without adding filesystem work
to measured frames.

On Windows, benchmark file operations support extended-length native paths
while reports keep their ordinary logical path spelling.

## Repeatable workload suite

Build the existing executable and its contract tests with Qt 6.12 and the
dependencies described in [Building](../docs/building.md):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DVNM_PLOT_BUILD_BENCHMARK=ON -DVNM_PLOT_BUILD_TESTS=ON
cmake --build build --config Release --target vnm_plot_benchmark
python3 benchmark/tools/run_suite.py \
  --candidate build/benchmark/vnm_plot_benchmark \
  --graphics-backend opengl --output-dir results/baseline
```

Use `d3d11` on Windows, `metal` on macOS, or another explicitly supported API.
`native` accepts Qt's platform default but rejects Null fallback. On a host
without raster rendering, `QT_QPA_PLATFORM=offscreen` and
`--graphics-backend null` exercise the production CPU pipeline only. Null
does not execute shaders, rasterize, measure GPU work, or validate pixels.

The suite runs these named cases, selectable with `--cases NAME ...`:

| Case | Purpose |
|---|---|
| `warm-line-1`, `warm-line-8`, `warm-line-32` | Settled frame cost as series count grows |
| `dense-line-8` | 100,000 visible samples per series, ten times the ordinary case |
| `warm-area-8`, `warm-dots-8` | Other production primitive paths |
| `warm-stacked-area-8` | Stack composition and reuse |
| `pan-line-8` | A quarter-width window moving across fixed history |
| `zoom-line-8` | A centered view changing from 10% to 100% of history |
| `append-line-8` | Exactly 16 new samples per series before each measured frame |
| `wrap-line-8` | The same publication schedule into a full ring, including split spans |
| `warm-line-8-no-text` | Control for text/layout changes |
| `warm-line-8-conservative` | Callbacks without a declared stable semantic identity |
| `warm-line-8-conservative-no-text` | Range-scan control without the text path |

Ordinary cases contain 10,000 samples per series. The seed, dimensions, series
count, rates, data/style, warmup and measured frames are explicit in every
command/report. The dense case also increases sample rate, keeping its entire
history within the ten-second view instead of only increasing invisible data.
Preview rendering is enabled in the suite; individual runs can use
`--no-preview` to isolate the main pane.

The suite passes `--stable-access` except in the conservative controls. The
benchmark's stateless callbacks then declare their actual immutable semantics
through the public policy API, allowing production range-cache reuse. A
byte-layout key alone does not establish callback semantics. This is a
workload distinction, not a production performance improvement. The original
ad-hoc conservative default remains available without the flag.

Panning and zooming use a 120-frame triangular cycle. Append/motion require
`--static --frames N`; warmup uses the initial data and view, and the measured
sequence does not depend on the number of warmup frames. `--static` means
prefilled data with no producer thread; `--append-per-frame` then advances it
synchronously before each render timer starts. Synthetic generation and
publication cost is reported separately as `benchmark.workload.publish_ms`.
The existing time-paced producer mode remains available for contention and
live-rate experiments, but is not used for paired performance comparisons.

Input schedules are deterministic. Label fades still use elapsed real time,
so motion/update cases may retain different outgoing labels on differently
paced runs. They are not pixel-identical frame sequences. Settled cases are
the strongest timing controls; fixed-clock CoreAlgo tests cover fade behavior.
The sources currently expose LOD 0 only, so this suite does not cover LOD
switching, custom layers, or the Qt Quick event/synchronization loop.

## Compare an optimization

Build baseline and candidate with the **same benchmark code, compiler,
dependencies and configuration**. Keep both executables, then run:

```bash
python3 benchmark/tools/run_suite.py \
  --baseline /absolute/path/to/baseline/vnm_plot_benchmark \
  --candidate /absolute/path/to/candidate/vnm_plot_benchmark \
  --graphics-backend opengl --output-dir results/comparison \
  --repetitions 5 --warmup-frames 30 --frames 240
```

Each process is a replicate. The runner executes sequentially, alternates
baseline/candidate order by repetition, preserves every raw report and log,
and rejects failed frames, wrong publication/occupancy/overwrite counts,
unexpected static uploads/scans, mismatched metadata or backend fallback.
It records executable hashes and host/renderer details. Record the exact
source/dependency revisions alongside results: a binary's enclosing checkout
at run time does not prove what was compiled into it.

`summary.md` gives medians of per-process means and p95s. `summary.json` also
contains paired changes, IQR, min/max, counter totals and inclusive scope
costs. `manifest.json` links raw reports and preserves run order. Negative
paired percentages mean lower cost. Frame samples are not pooled as independent
replicates, and spread is not presented as a confidence interval. There are
no machine-dependent performance thresholds in CTest.

Use a second run with `--frame-timing-only` to check whether a result survives
without internal renderer/source profiling. It retains outer frame timings
and render-thread C++ allocation accounting, plus exact publication counts.
Internal counters/scopes are absent in this mode and are not inferred as zero.
Keep internal-profiling mode identical within each comparison.

The current workload contract is **version 3**. It stops frame timing and
allocation measurement before storing harness observations, and removes the
finish profiler scope and preparation-time lookup from frame-only mode.
The retained version 2 results are historical evidence with the earlier
measurement boundary. Do not compare timings across contract versions;
rebuild both variants with the same version 3 benchmark code for new comparisons.

### Measurement boundaries

- `benchmark.frame.total_ms` is offscreen frame **wall time**, including
  begin/end-frame, production planning/layout/text/series preparation,
  recording and `--finish`. Harness observation storage happens after the
  timer stops. It is not application FPS or isolated CPU time.
- `benchmark.frame.submission_ms` includes QRhi begin/end calls, which may
  synchronize. `gpu_finish_ms` is the extra finish call; it is not a GPU timer.
- Internal named scopes are inclusive and aggregated by full scope name in
  the raw JSON `scopes` map. Do not sum parent and child times.
- C++ allocation counters cover the measured render thread and exclude
  profiler storage. They do not count all `malloc` calls, other threads, GPU
  memory, or retained memory. QRhi buffer creation has separate counters.
- Setup/first submission and warmup are separate from measured frames.
  Existing font/disk caches can affect cold results; the suite reports warm
  frames and does not claim controlled cold-start performance.
- Timing runs disable readback. Run `--pixel-checksum` separately on a raster
  backend and compare appropriate stable frames when validating a change.
  When requested, readback and pixel validation remain inside frame timing
  and allocation measurement.
  Faster execution alone is not evidence that rendering stayed correct.

The offscreen runner consumes measured value-axis width feedback, as the
shipping widget does. Reusing the initial width would cause layout misses in
otherwise settled frames; the workload contract test checks this invariant.

## Maintained checks

With benchmark tests enabled, CTest includes `benchmark_workload_contracts`
and `benchmark_suite_validation`. They check actual fixed/pan/zoom/publication
behavior, settled layout/range/upload reuse, frame-only mode, invalid input,
and rejection of incomplete or incomparable results. The phase-flush test
uses Null by default because it tests durable output and external termination;
native smoke/readback tests retain their raster backend requirement.

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure \
  -R '^(benchmark_workload_contracts|benchmark_suite_validation|benchmark_shared_frame_pipeline|benchmark_phase_trace_flush)$'
```
