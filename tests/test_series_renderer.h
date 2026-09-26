#pragma once

#include <vnm_plot/rhi/series_renderer.h>

namespace vnm::plot {

// Expose only the state used by renderer tests, without redefining C++ keywords.
// Instances remain ordinary Series_renderer objects with the same draw path.
class Test_series_renderer final : public Series_renderer
{
public:
    using Series_renderer::vbo_view_state_t;
    using Series_renderer::m_vbo_states;
    using Series_renderer::m_last_qrhi_layer_cache_size;
    using Series_renderer::m_last_recorded_draw_colors;
    using Series_renderer::m_last_recorded_draw_series_ids;
    using Series_renderer::m_last_recorded_draw_styles;
    using Series_renderer::m_last_recorded_draw_view_kinds;
    using Series_renderer::m_last_recorded_draw_z_orders;
    using Series_renderer::m_last_recorded_line_widths;
    using Series_renderer::m_last_recorded_stack_sum_overlays;
    using Series_renderer::stack_view_statuses;
};

} // namespace vnm::plot
