# Building and integration

This guide covers source builds, dependency selection, component targets,
and installed packages. See the [README](../README.md) for a first
integration and [Using vnm_plot](usage.md) for plotting and QML setup.

## Source builds

A C++20 compiler and Qt 6.12 or newer are required. Building from source
needs Core, Gui, Qml, Quick, GuiPrivate, and ShaderTools. Qt ShaderTools bakes
the shaders; installed consumers do not need it.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

A Qt SDK for FreeBSD is available from
[qt-freebsd](https://github.com/Varinomics/qt-freebsd).

To disable text rendering, which skips the full MSDF atlas/text renderer path
while retaining the public LCD contract dependency:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVNM_PLOT_ENABLE_TEXT=OFF
cmake --build build --config Release
```

## Dependencies

The build uses an existing glm target when available and otherwise fetches
glm. With `VNM_PLOT_USE_SYSTEM_LIBS=ON`, it tries `find_package` before
fetching missing libraries.

Public vnm_plot headers use the dependency-light `vnm_msdf_text`
`lcd_contract` component. The Qt wrapper uses its `qt_lcd` display
resolver, and text-enabled builds use `vnm_msdf_text::rhi` for atlas ownership
and rendering. CMake uses a sibling `../vnm_msdf_text` checkout when present,
otherwise it fetches the GitHub `master` branch. The atlas path fetches
FreeType and msdfgen when they are not already available as targets.

The fonts come from [vnm_fonts](https://github.com/Varinomics/vnm_fonts): the
monospace face `vnm_plot_rhi` embeds, and the icon face the function plotter
example loads. A tree that has already added vnm_fonts publishes the verbatim
files in `VNM_FONTS_DIRECTORY`, which is used as it stands. Otherwise CMake
uses a sibling `../vnm_fonts` checkout when present and fetches the GitHub
`master` branch when it is not. `FETCHCONTENT_SOURCE_DIR_VNM_FONTS` overrides
that pair. Font licensing is described in
[third-party notices](../THIRD_PARTY_NOTICES.md).

## Components and headers

Public headers are:

- `#include <vnm_plot/vnm_plot.h>`: umbrella header
- `#include <vnm_plot/core.h>`: data and layout API
- `#include <vnm_plot/rhi.h>`: QRhi renderer and custom-layer API
- `#include <vnm_plot/qt.h>`: Qt Quick API, enabled by `VNM_PLOT_WITH_QT`

The component dependencies are:

```
vnm_plot_data
  -> Data_source, snapshots, access policies, time units, query APIs

vnm_plot_layout
  -> Layout_calculator, range/window planning, formatting helpers
  -> vnm_plot_data

vnm_plot_rhi
  -> Asset_loader, Chrome_renderer, Series_renderer, Text_renderer, Font_renderer
  -> custom QRhi series layers
  -> vnm_plot_layout

vnm_plot_qtquick
  -> Plot_widget (QQuickRhiItem), Plot_time_axis, interactions, QML resources
  -> vnm_plot_rhi
```

`vnm_plot_data` and `vnm_plot_layout` have Qt-private-free link interfaces.
`vnm_plot_rhi` owns the built-in QRhi renderers, shader resources, font/text
rendering, asset loading, and custom QRhi series-layer API.
`vnm_plot_qtquick` supplies the QML-friendly `Plot_widget` wrapper.
See [Rendering and thread safety](usage.md#rendering-and-thread-safety) for
renderer responsibilities and data lifetime requirements.

## Source integration

As a subdirectory:

```cmake
add_subdirectory(vnm_plot)
target_link_libraries(your_app PRIVATE vnm_plot::qtquick)
```

Via FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(vnm_plot
    GIT_REPOSITORY https://github.com/Varinomics/vnm_plot.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(vnm_plot)
target_link_libraries(your_app PRIVATE vnm_plot::qtquick)
```

For data/layout code or Qt RHI rendering without Qt Quick, link the target needed:

```cmake
target_link_libraries(data_tool PRIVATE vnm_plot::data)
target_link_libraries(layout_tool PRIVATE vnm_plot::layout)
target_link_libraries(rhi_tool PRIVATE vnm_plot::rhi)
```

## Installed packages

The CMake project version is `0.1.0`. External consumers can use installed
or package-manager-provided builds:

```cmake
find_package(vnm_plot CONFIG REQUIRED COMPONENTS qtquick)
target_link_libraries(your_app PRIVATE vnm_plot::qtquick)
```

Install exports are `find_package`-ready only when exported dependencies are
already imported package targets. Configure with
`-DVNM_PLOT_USE_SYSTEM_LIBS=ON` and provide `find_package`-able `glm` and
`vnm_msdf_text`. The `lcd_contract` component is needed for public vnm_plot
headers and `vnm_plot::data` / `vnm_plot::layout` consumers. Text-enabled
`vnm_plot::rhi` and `vnm_plot::qtquick` package consumers need the
`vnm_msdf_text` `rhi` component, and Qt Quick consumers also need `qt_lcd`.
When dependencies are built locally through FetchContent, install still
installs headers and libraries, but skips the CMake package export because
CMake cannot export those local dependency targets from this project.

Installed package targets are `vnm_plot::data`, `vnm_plot::layout`,
`vnm_plot::rhi`, and `vnm_plot::qtquick`. Requesting the `data` or `layout`
components does not require Qt private modules. Requesting `rhi` or
`qtquick` requires Qt GuiPrivate because those targets use QRhi.

## Examples

Enable examples with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVNM_PLOT_BUILD_EXAMPLES=ON
cmake --build build --config Release
```

- `vnm_plot_hello` - renders a sample curve using the example function-source helper
- `vnm_plot_preview_config` - preview uses a separate data source and AREA style via `preview_config`
- `function_plotter` - expressions, per-series styles, and a stacked-series demo via mexce

`function_plotter` is opt-in because it depends on Qt Multimedia and `mexce`.
Build it with `-DVNM_PLOT_BUILD_EXAMPLES=ON -DVNM_PLOT_BUILD_FUNCTION_PLOTTER=ON`;
point at a local mexce checkout with `-DMEXCE_LOCAL_PATH=...`.
Use its **Σ** control to stack functions, drag a function's color accent or
label to change bottom-to-top order, and use **Fit Vertical** to fit the stack.
