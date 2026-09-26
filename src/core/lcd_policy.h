#pragma once

#include <vnm_plot/core/lcd.h>

namespace vnm::plot::detail {

enum class text_lcd_draw_surface_t
{
    VERTICAL_AXIS_LABEL,
    HORIZONTAL_AXIS_LABEL,
};

constexpr float k_lcd_opaque_alpha_cutoff = 0.999f;

using vnm::msdf_text::lcd::lcd_sanitize_resolved_order;
using vnm::msdf_text::lcd::lcd_auto_order_from_detections;
using vnm::msdf_text::lcd::lcd_effective_order;

constexpr lcd_subpixel_order_t lcd_effective_order_for_frame(
    lcd_request_t        requested,
    lcd_subpixel_order_t auto_resolved_order)
{
    return lcd_effective_order(requested, auto_resolved_order);
}

constexpr lcd_subpixel_order_t lcd_effective_order_for_frame(
    const lcd_request_t* requested,
    lcd_subpixel_order_t auto_resolved_order)
{
    return requested != nullptr
        ? lcd_effective_order(*requested, auto_resolved_order)
        : lcd_sanitize_resolved_order(auto_resolved_order);
}

constexpr lcd_subpixel_order_t grid_lcd_subpixel_order(
    lcd_subpixel_order_t frame_order,
    float                background_alpha,
    bool                 has_opaque_backing)
{
    if (!has_opaque_backing || !(background_alpha >= k_lcd_opaque_alpha_cutoff)) {
        return lcd_subpixel_order_t::NONE;
    }

    return frame_order == lcd_subpixel_order_t::RGB ||
           frame_order == lcd_subpixel_order_t::BGR
        ? frame_order
        : lcd_subpixel_order_t::NONE;
}

constexpr bool text_lcd_draw_is_eligible(
    text_lcd_draw_surface_t            surface,
    lcd_subpixel_order_t               frame_order,
    float                              background_alpha,
    bool                               has_opaque_backing)
{
    if (!vnm::msdf_text::lcd::is_display_specific(frame_order) ||
        !(background_alpha >= k_lcd_opaque_alpha_cutoff))
    {
        return false;
    }

    switch (surface) {
        case text_lcd_draw_surface_t::VERTICAL_AXIS_LABEL:
        case text_lcd_draw_surface_t::HORIZONTAL_AXIS_LABEL: return has_opaque_backing;
        default:                                             return false;
    }
}

} // namespace vnm::plot::detail
