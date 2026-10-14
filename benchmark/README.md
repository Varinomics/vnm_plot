# vnm_plot benchmark

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
