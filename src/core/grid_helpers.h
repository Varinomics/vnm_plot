#pragma once

#include <vnm_plot/core/constants.h>
#include <vnm_plot/core/types.h>

#include <algorithm>

namespace vnm::plot::detail {

inline int decade_step_factor(int index)
{
    return (index & 1) ? 2 : 5;
}

inline void select_decade_step(double range, double& step, int& index)
{
    step = 1.0;
    index = 16;
    double probe = 0.0;
    for (; (probe = step * decade_step_factor(index)) < range; ++index) {
        step = probe;
    }
    for (; (probe = step / decade_step_factor(index - 1)) > range; --index) {
        step = probe;
    }
}

inline void append_grid_level(
    grid_layer_params_t& levels,
    float                spacing_px,
    float                start_px,
    double               cell_span_min,
    double               fade_den)
{
    const double fade = (double(spacing_px) - cell_span_min) / fade_den;
    const float alpha = static_cast<float>(std::clamp(fade, 0.0, 1.0) * k_grid_line_alpha_base);
    levels.spacing_px[levels.count]   = spacing_px;
    levels.start_px[levels.count]     = start_px;
    levels.alpha[levels.count]        = alpha;
    levels.thickness_px[levels.count] = 0.6f + 0.6f * (alpha / k_grid_line_alpha_base);
    ++levels.count;
}

} // namespace vnm::plot::detail
