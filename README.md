# vnm_plot

A GPU-accelerated 2D time-series plotting library for Qt Quick, using Qt RHI.
It provides interactive plots with automatic level-of-detail selection for
large datasets, line and area styles, stacked series, previews, and shared
time axes. Applications supply their own sample types and data sources.

[![CI Linux](https://github.com/Varinomics/vnm_plot/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_plot/actions/workflows/ci-linux.yml) [![CI macOS](https://github.com/Varinomics/vnm_plot/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_plot/actions/workflows/ci-macos.yml) [![CI Windows](https://github.com/Varinomics/vnm_plot/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_plot/actions/workflows/ci-windows.yml) [![CI FreeBSD](https://github.com/Varinomics/vnm_plot/actions/workflows/ci-freebsd.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_plot/actions/workflows/ci-freebsd.yml)

![Function Plotter](function_plotter_example.png)
<sub>function_plotter example</sub>

## Getting started

You need a C++20 compiler, CMake, and Qt 6.12 or newer with Core, Gui, Qml,
Quick, GuiPrivate, and ShaderTools. The source build obtains its other
dependencies when they are not already supplied.

To build the library and the hello/preview examples:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVNM_PLOT_BUILD_EXAMPLES=ON
cmake --build build --config Release
```

Start with [hello_plot](examples/hello_plot) for a runnable Qt Quick application.
[Building and integration](docs/building.md) covers dependency selection,
other examples, and installed packages.

To use a source checkout in an existing CMake application:

```cmake
add_subdirectory(vnm_plot)
target_link_libraries(your_app PRIVATE vnm_plot::qtquick)
```

## Plotting samples

Given a `vnm::plot::Plot_widget* plot_widget` from your Qt Quick view, create
a data source and describe how to read its timestamps and values:

```cpp
#include <vnm_plot/vnm_plot.h>

struct sample_t
{
    std::int64_t t_ns;
    float        value;
};

std::vector<sample_t> samples;
samples.push_back({0, 0.0f});
samples.push_back({1'000'000'000, 1.0f});

auto source = std::make_shared<vnm::plot::Vector_data_source<sample_t>>();
source->set_data(std::move(samples));

auto access = vnm::plot::make_access_policy<sample_t>(
    &sample_t::t_ns,
    &sample_t::value);

auto series = vnm::plot::Series_builder()
    .style(vnm::plot::Display_style::LINE)
    .color(vnm::plot::rgba_u8(51, 153, 255))
    .data_source(source)
    .access(access)
    .build_shared();

plot_widget->add_series(0, series);
```

Sample timestamps must be non-decreasing in snapshot order;
[Using vnm_plot](docs/usage.md#data-sources-and-snapshots) states the
data-source contract.

The widget renders on Qt's RHI render thread. Keep registered series
configuration immutable, replace it with `add_series` when it changes, and
make data sources safe to read from that thread.

[Using vnm_plot](docs/usage.md) explains QML setup, custom samples, rendering
configuration, and stacking. The [benchmark guide](benchmark/README.md)
covers performance measurements.

## License

[BSD-2-Clause](LICENSE.txt). Embedded fonts keep their own licences; see
[third-party notices](THIRD_PARTY_NOTICES.md) and [licence texts](LICENSES/).
