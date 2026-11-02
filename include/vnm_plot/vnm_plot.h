#pragma once
// VNM Plot Library - Main Header
// A high-performance, GPU-accelerated plotting library.
//
// This library provides:
// - Generic data source interface (Data_source)
// - Support for streaming data with LOD (Level of Detail)
// - GPU-accelerated rendering via QRhi
// - Qt Quick integration for interactive plots
//
// Usage:
//   1. Create a data source (Vector_data_source, or implement Data_source)
//   2. Create a Data_access_policy for your sample type
//   3. Create a Plot_widget (Qt Quick) and add data sources
//
// Use core.h or qt.h directly when you only need one side.
#include <vnm_plot/core.h>

#if defined(VNM_PLOT_WITH_RHI)
#include <vnm_plot/rhi.h>
#endif

#if defined(VNM_PLOT_WITH_QT)
#include <vnm_plot/qt.h>
#endif

namespace vnm::plot {

// Library version
constexpr int k_version_major = 0;
constexpr int k_version_minor = 1;
constexpr int k_version_patch = 0;

constexpr const char* k_version_string = "0.1.0";

} // namespace vnm::plot
