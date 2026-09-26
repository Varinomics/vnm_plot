#pragma once

#include "plot_render_feedback.h"
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/types.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>

class QRhi;
class QRhiCommandBuffer;
class QRhiRenderTarget;

namespace vnm::plot::detail {

// Value snapshot consumed by both Qt Quick and the offscreen benchmark.
struct Plot_render_snapshot
{
    Plot_config            config;
    data_config_t          data_cfg;
    std::map<int, std::shared_ptr<const series_data_t>>
                           series;
    bool                   v_auto                  = true;
    int                    visible_info_flags      = k_visible_info_none;
    double                 adjusted_font_px        = 10.0;
    double                 base_label_height_px    = 14.0;
    double                 adjusted_preview_height = 0.0;
    double                 vbar_width_pixels       = 0.0;
    glm::vec4              window_background       = glm::vec4(0.f, 0.f, 0.f, 1.f);
    lcd_subpixel_order_t   auto_lcd_subpixel_order = lcd_subpixel_order_t::NONE;
    std::uint64_t          config_revision         = 0;
    std::uint64_t          series_revision         = 0;
};

struct Plot_frame_result
{
    plot_render_feedback_t feedback;
    bool                  needs_update = false;
};

// Owns the shipping frame pipeline and its caches, independently of the Qt
// Quick item lifecycle. Destroy it before the QRhi supplying its resources.
class Plot_frame_renderer
{
public:
    Plot_frame_renderer();
    ~Plot_frame_renderer();

    void initialize();
    Plot_frame_result render(
        const Plot_render_snapshot& snapshot,
        QRhi*                       rhi,
        QRhiRenderTarget*           target,
        QRhiCommandBuffer*          cb);

private:
    struct impl_t;
    std::unique_ptr<impl_t> m_impl;
};

} // namespace vnm::plot::detail
