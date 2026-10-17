# Using vnm_plot

This guide covers data sources, Qt Quick setup, styles, and stacked plots.
For a first sample, see [Plotting samples](../README.md#plotting-samples);
for CMake targets and dependencies, see [Building and integration](building.md).

## Data sources and snapshots

The library uses a type-erased data interface (`vnm::plot::Data_source` and
`vnm::plot::Data_access_policy`) so it can work with any sample type without
templates in the rendering code. Data sources decide whether snapshots are
copies or direct views; buffering, if needed, lives in the data source.
The renderer automatically selects a level of detail for the current zoom.

Within every snapshot a source returns, sample timestamps must be
non-decreasing in index order. Equal timestamps are allowed, so ticks or
commits that share a timestamp are valid. The renderer finds the visible window
by binary search on that order, so a source that cannot guarantee it must sort,
or drop out-of-order samples, before handing them to the library. The renderer
checks the order once per snapshot revision (`sequence`), and on every frame for
a snapshot without one. A snapshot with a decrease is rejected whole: nothing is
drawn for that series while the violation lasts, and the optional
`Plot_config::log_error` callback receives one message per violation, naming the
series and the decreasing timestamps. The series draws again from the first
ordered snapshot. A stack group that contains such a series is suppressed with
the `NONMONOTONIC_TIMESTAMPS` reason described under Stacked series.

## Rendering and thread safety

`Plot_widget` renders on a separate RHI render thread. Treat `series_data_t`
as immutable once added. To change series config (style, access policy,
preview config, color), update a copy and call `add_series` again with the
same id to replace it. Make sure your `Data_source` implementation is safe
to read from the render thread.

`Plot_renderer` coordinates the sub-renderers on the Qt RHI render thread.
`Series_renderer` handles lines, dots, and area fills with VBO management,
while `Chrome_renderer` draws the grid and axes. `Font_renderer` owns plot
font assets, metric policy, and disk caching; `vnm_msdf_text::rhi` owns
immutable baked fonts, text geometry, and QRhi drawing.

## QML setup

Register the module on the GUI thread before creating the QML engine:

```cpp
#include <vnm_plot/vnm_plot.h>

vnm::plot::register_qml_types();
```

This registers `PlotWidget`, `PlotTimeAxis`, `PlotInteractionItem`, `PlotView`,
and `PlotIndicator` and loads their embedded resources. No extra QML import path
is required. Link the application to `vnm_plot::qtquick`.

Use it in QML:

```qml
import VnmPlot 1.0

PlotWidget {
    id: plot
    anchors.fill: parent
}
```

Shared time axis across plots:

```qml
import VnmPlot 1.0

PlotTimeAxis { id: sharedAxis }

Column {
    PlotView { time_axis: sharedAxis }
    PlotView { time_axis: sharedAxis }
}
```

## Custom sample types

Implement a `vnm::plot::Data_access_policy` to tell the renderer how to read your samples:

```cpp
struct my_sample_t
{
    double timestamp;
    float  value;
    float  low;
    float  high;
};

auto policy = vnm::plot::make_access_policy<my_sample_t>(
    &my_sample_t::timestamp,
    &my_sample_t::value,
    &my_sample_t::low,
    &my_sample_t::high);

// Assign policy to a series
auto series = std::make_shared<vnm::plot::series_data_t>();
series->access = policy.erase();
```

## Display styles

- `DOTS` - points
- `LINE` - connected line
- `AREA` - filled area
- `COLORMAP_AREA` - area colored by auxiliary metric
- Combinations: `DOTS_LINE`, `LINE_AREA`, `DOTS_LINE_AREA`

## Configuration

The `my_format_time` callback below is supplied by the host application.

Customize rendering via `Plot_config`:

```cpp
vnm::plot::Plot_config config;
config.dark_mode = true;
config.line_width_px = 2.0;
config.stack_sum_line_width_extra_px = 1.0;
config.dark_color_palette.stack_sum_line = vnm::plot::rgba_u8(0x9a, 0x96, 0x8b);
config.auto_v_range_mode = vnm::plot::Auto_v_range_mode::VISIBLE;
config.format_timestamp = [](
    std::int64_t timestamp_ns,
    std::int64_t step_ns) {
    return my_format_time(timestamp_ns, step_ns);
};
plot_widget->set_config(config);
```

## Stacked series

The example uses `sample_t` from [Plotting samples](../README.md#plotting-samples).

Give two or more ordinary series the same non-zero stack group. Group `0`
keeps a series independent.

```cpp
auto f_source = std::make_shared<vnm::plot::Vector_data_source<sample_t>>();
f_source->set_data(std::vector<sample_t>{{0, 1.0f}, {1'000'000'000, 2.0f}});

auto g_source = std::make_shared<vnm::plot::Vector_data_source<sample_t>>();
g_source->set_data(std::vector<sample_t>{{0, 3.0f}, {1'000'000'000, 4.0f}});

const auto access = vnm::plot::make_access_policy<sample_t>(
    &sample_t::t_ns,
    &sample_t::value);

auto f = vnm::plot::Series_builder()
    .style(vnm::plot::Display_style::AREA)
    .stack_group(1)
    .data_source(f_source)
    .access(access)
    .build_shared();

auto g = vnm::plot::Series_builder()
    .style(vnm::plot::Display_style::AREA)
    .stack_group(1)
    .data_source(g_source)
    .access(access)
    .build_shared();

plot_widget->add_series(10, f); // bottom
plot_widget->add_series(20, g); // top; cumulative value is f + g
```

Series are stacked bottom-to-top in ascending plot-ID order. Reordering a
stack therefore also means updating the caller's plot-ID mapping and series
registrations. `LINE` and `DOTS` draw cumulative values; `AREA` fills each
component's band between its cumulative base and top. Preview sources and
styles are composed the same way in the preview view.

Each source retains its independently selected LOD and timestamp grid. The
renderer uses their exact timestamp union when it fits the view's composition
budget. Larger unions are interpolated onto a shared deterministic grid that
targets half-pixel spacing, clipped to the common selected-source time domain.
The grid retains both domain endpoints and is capped so one stack view never
materializes more than 1,048,576 cumulative samples across all layers; very
large layer counts therefore use a coarser grid. Main and preview use their own
horizontal pixel widths and budgets. Members must use
matching `Series_interpolation` modes and, after sample and nonfinite-policy
handling, produce a single continuous drawable selected window with monotonic
timestamps. A rejected group fails closed: the affected stack view is omitted
instead of drawing misleading unstacked data, and its reason is queryable via
`stack_status` or `get_stack_status`. When configured, the optional
`Plot_config::log_error` callback may also receive a human-readable message.
Bounded resampling is an accepted `ACTIVE` result, not a rejection. Features
narrower than half a pixel may be omitted, and a `STEP_AFTER` transition may
move to the next grid timestamp; indicators and auto-fit continue to match the
geometry actually rendered.

Stack acceptance is also queryable without installing a log callback. C++ can
call `Plot_widget::stack_status(group, Series_view_kind::MAIN)` (or `PREVIEW`)
and inspect its typed `state`, `reason`, and `affected_series_ids`. `PENDING`
means no renderer result matches the current data and range, `ACTIVE` means the
view was accepted, and `SUPPRESSED` includes the specific rejection reason. A
singleton group is `ACTIVE` with no rejection because no composition is needed.
QML can call `get_stack_status(group, preview)`; the returned map contains
`group`, uppercase `view`, `state`, and `reason` strings, plus an
`affected_series_ids` list.

The final cumulative value gets an automatic line overlay. By default it is two
pixels thicker than `Plot_config::line_width_px` and painted `#E6DFCC` in dark
mode, `#192033` in light. A host that paints its own plot background can restate
both: `Plot_config::stack_sum_line_width_extra_px` sets the added width, and
`Color_palette::stack_sum_line` - reached through `Plot_config`'s
`dark_color_palette` and `light_color_palette` - sets the color per theme.
Soften the overlay by moving that color toward the background rather than by
lowering its alpha; segment quads overlap by half a line width, so a translucent
stroke beads at every join. This applies to the main and preview views for
`LINE`, `DOTS`, and `AREA` stacks.

`PlotIndicator` keeps component text values raw while placing their markers on
the cumulative rendered layers. It adds a text-only `Σ` total and the note
"Markers show cumulative stack positions". `Plot_widget::auto_adjust_view()`
(used by the function plotter's **Fit Vertical** button) fits current cumulative
stack geometry, including `AREA` bases.
