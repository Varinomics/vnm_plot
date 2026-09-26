# vnm_plot Synthetic Benchmark - Architecture and Implementation

## Purpose
This benchmark is a standalone Qt + QRhi application that generates synthetic
time-series data and measures vnm_plot rendering performance. It emits a stable
profiling report format for reproducible comparisons.

## High-Level Flow
1. A generator thread produces Bar or Trade samples using a Brownian motion
   model and writes into a ring buffer.
2. The render thread (Qt UI) snapshots the ring buffer through a vnm_plot
   Data_source and renders the series using vnm_plot RHI renderers.
3. A custom profiler collects nested vnm_plot scope timings and writes a
   benchmark report at the end of the run.

## Major Components
- Ring_buffer<T>
  - Thread-safe circular buffer with overwrite semantics.
  - Exposes split-span zero-copy views protected by a shared lock.
  - Keeps `copy_to()` only as an explicit copying helper; snapshots do not use it.

- Brownian_generator
  - Generates Bar_sample or Trade_sample using geometric Brownian motion.
  - Deterministic when seeded; timestamps advance at the configured rate.

- Benchmark_data_source<T>
  - Implements vnm::plot::Data_source.
  - Publishes zero-copy snapshots that may contain two ring-buffer spans.
  - Reports logical view bytes separately from exact copied bytes (zero).
  - Tracks the stable ring revision used by `current_sequence()`.

- Benchmark_profiler
  - Implements vnm::plot::Profiler.
  - Aggregates scopes by name for deterministic output.
  - Writes a fixed-width, hierarchical report with UTC timestamps.

- Benchmark_window (Qt RHI window/offscreen runner)
  - Supplies frame snapshots to the same internal Plot_frame_renderer used by
    the Qt Quick Plot_widget, and owns the offscreen QRhi target.
  - Starts the generator thread and drives rendering at ~60 Hz.
  - Advances the time window from the latest data snapshot; Frame_range_planner
    computes vertical ranges using the production source/cache semantics.

## Threading Model
- Generator thread:
  - Produces samples at the configured rate using steady_clock pacing.
  - Writes into the ring buffer only; no rendering work.
  - A coordinated publication barrier bounds complete logical publications
    across every series to the measurement interval.

- UI/render thread:
  - The offscreen path drives QRhi frames directly.
  - Requests zero-copy snapshots from the Data_source and renders with vnm_plot.
  - Records profiling scopes, render-thread CPU allocations, and QRhi buffer
    allocations through Plot_config::profiler.

## Rendering Pipeline
- Plot_frame_renderer owns range planning, layout and measured value-bar relayout,
  frame context construction, text fades, and pass ordering for both backends.
- Shaders load from Qt resources; Asset_loader supplies the embedded font.
- Text is prepared before chrome so grids use the same fade state and axis-pane
  opacity/LCD policy as the shipping widget.
- Series_renderer draws each instance through a dedicated vertex shader:
  - Bars: plot_area.vert / plot_area.frag (instanced 6-vert fill +
    4-vert zero-axis emphasis bar; samples i and i+1 fed through two
    instanced vertex-attribute bindings on the same VBO).
  - Trades: plot_dot_quad.vert / plot_dot_quad.frag (one instance per
    sample, expanded to a 4-vert quad via gl_VertexID).
- Chrome_renderer draws grids and preview overlays.
- Primitive_renderer flushes batched primitives.

## Data Access Policies
Bar_sample and Trade_sample are packed structs. Each Data_access_policy defines:
- Timestamp extraction (int64 nanoseconds)
- Primary value (close or price)
- Range (low/high for Bars, price/price for Trades)
- Optional aux metric (volume or size)
- A stable layout_key for renderer-internal cache identity. The renderer
  owns the GPU sample layout (gpu_sample_t) and rebases timestamps on
  upload, so policies do not describe vertex attributes or sample byte
  offsets.

## Profiling and Report Output
Benchmark_profiler produces a stable benchmark report:
- Fixed header fields (session, UTC timestamps, compiler)
- Sorted metadata keys
- Fixed-width table and tree glyphs
- Optional extended metadata behind --extended-metadata
- Unit-neutral observation counters for non-timing measurements, including
  uploaded sample bytes per frame, sample upload counts, auto-range query and
  scan counts, monotonicity scans, and LOD switches.

The report is written at the end of the benchmark run to the configured output
folder with the inspector_benchmark_YYYYMMDD_HHMMSS_<STREAM>_<DataType>.txt
naming convention. Windows file operations adapt logical paths to the native
extended-length namespace without exposing that transport spelling in reports.

The phase trace flushes a fixed eleven aggregate success boundaries outside the
measured frame loop. Only a failure adds a frame-specific boundary, so external
termination retains progress without per-frame trace I/O.

## Configuration and CLI
The executable accepts CLI options for duration, data type, seed, rate, ring
capacity, volatility, output directory, and report session/stream. The seed is
resolved once at startup to keep runs reproducible.

## Key Files
- benchmark/include/ring_buffer.h
- benchmark/include/brownian_generator.h
- benchmark/include/benchmark_data_source.h
- benchmark/include/benchmark_profiler.h
- benchmark/include/path_io.h
- benchmark/include/benchmark_window.h
- benchmark/src/benchmark_window.cpp
- benchmark/src/main.cpp
