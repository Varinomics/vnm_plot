#pragma once

#include <vnm_plot/core/types.h>

namespace vnm::plot {

/**
 * Select the smallest time-grid ladder step whose pixel spacing meets the
 * requested minimum. Returns the largest available ladder step if no step
 * meets the minimum. All inputs must be finite and positive; otherwise returns
 * one second. The minimum ladder step is one millisecond.
 */
double select_time_step_seconds(
    double span_seconds,
    double width_px,
    double min_spacing_px);

/**
 * Build layered time-grid parameters for a visible seconds-domain range.
 */
grid_layer_params_t build_time_grid_layers(
    double t_min_seconds,
    double t_max_seconds,
    double width_px,
    double font_px,
    bool*  out_dropped_non_multiple_step_or_null = nullptr);

} // namespace vnm::plot
