#pragma once

// VNM Plot Library - RHI Frame Context
// Per-frame QRhi render state passed across RHI renderers and custom layers.

#include <vnm_plot/core/lcd.h>
#include <vnm_plot/core/types.h>

#include <glm/mat4x4.hpp>

#include <optional>

class QRhi;
class QRhiCommandBuffer;
class QRhiRenderTarget;
class QRhiResourceUpdateBatch;

namespace vnm::plot {

struct frame_context_t
{
    const frame_layout_result_t& layout;

    float          v0         = 0.0f;
    float          v1         = 1.0f;
    float          preview_v0 = 0.0f;
    float          preview_v1 = 0.0f;

    // Timestamps are int64_t nanoseconds (API convention).
    std::int64_t   t0 = 0;
    std::int64_t   t1 = 1;

    std::int64_t   t_available_min = 0;
    std::int64_t   t_available_max = 1;

    int            win_w = 0;
    int            win_h = 0;

    glm::mat4 pmv{1.0f};

    double                     adjusted_font_px         = 10.0;
    double                     base_label_height_px     = 14.0;
    double                     adjusted_reserved_height = 0.0;
    double                     adjusted_preview_height  = 0.0;

    int                        visible_info_flags   = k_visible_info_none;
    bool                       dark_mode            = false;
    glm::vec4                  plot_body_background = glm::vec4(0.f, 0.f, 0.f, 1.f);
    // With config, this is the host-resolved AUTO order; explicit requests in
    // config still take precedence. Without config, it is a manual frame order.
    lcd_subpixel_order_t lcd_subpixel_order = lcd_subpixel_order_t::NONE;

    const Plot_config*         config = nullptr;

    // Optional main-plot area hover in render-target pixels, from the top left.
    std::optional<glm::dvec2>   area_hover_position;

    // RHI handles for the active frame. The renderer routes uploads through
    // the RHI resource-update batch and records draws through `cb`.
    QRhi*                      rhi = nullptr;
    QRhiCommandBuffer*         cb  = nullptr;
    // Render target the host already opened a pass on. The renderer reads
    // the render-pass descriptor and sample count off it when building
    // graphics-pipeline state objects.
    QRhiRenderTarget*          render_target = nullptr;
    // Resource-update batch the host hands the renderer to fill. The host
    // owns its lifetime and submits it via beginPass's 4th argument; the
    // renderer must NOT call cb->resourceUpdate(batch) itself, because that
    // call is illegal once the host's render pass is open.
    QRhiResourceUpdateBatch*   rhi_updates = nullptr;
};

} // namespace vnm::plot
