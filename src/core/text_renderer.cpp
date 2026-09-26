#include <vnm_plot/rhi/text_renderer.h>
#include <vnm_plot/core/color_palette.h>
#include <vnm_plot/core/constants.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/lcd.h>
#include <vnm_plot/rhi/font_renderer.h>
#include <vnm_plot/core/time_units.h>
#include "label_fade_tracker.h"
#include "label_pane_geometry.h"
#include "lcd_policy.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace vnm::plot {
using detail::k_h_label_vertical_nudge_px;
using detail::k_line_spacing;
using detail::k_overlay_left_px;
using detail::k_pixel_snap;
using detail::k_scissor_pad_px;
using detail::k_text_margin_px;
using detail::k_v_label_horizontal_padding_px;
using detail::k_v_label_vertical_nudge_px;
using detail::k_value_decimals;

namespace {

constexpr float k_text_shadow_alpha         = 0.76f;
constexpr float k_text_shadow_min_radius_px = 1.05f;
constexpr float k_text_shadow_max_radius_px = 2.25f;
constexpr float k_text_shadow_radius_factor = 0.19f;

bool rect_contains(const glm::vec4& outer, const glm::vec4& inner)
{
    return
        outer.x <= inner.x &&
        outer.y <= inner.y &&
        outer.z >= inner.z &&
        outer.w >= inner.w;
}

bool fit_rect_vertically_within(glm::vec4& rect, float& baseline_y, const glm::vec4& clip)
{
    const float clip_height = clip.w - clip.y;
    const float rect_height = rect.w - rect.y;
    if (!(clip_height > 0.0f) || !(rect_height > 0.0f) || rect_height > clip_height) {
        return rect_contains(clip, rect);
    }

    float dy = 0.0f;
    if (rect.y      < clip.y) { dy =  clip.y - rect.y;        }
    if (rect.w + dy > clip.w) { dy += clip.w - (rect.w + dy); }

    const glm::vec4 shifted(
        rect.x,
        rect.y + dy,
        rect.z,
        rect.w + dy);
    if (!rect_contains(clip, shifted)) {
        return false;
    }

    rect = shifted;
    baseline_y += dy;
    return true;
}

glm::vec4 text_color_for_theme(bool dark_mode)
{
    return dark_mode
        ? glm::vec4(1.f, 1.f, 1.f, 1.f)
        : glm::vec4(0.f, 0.f, 0.f, 1.f);
}

text_shadow_t text_shadow_for_background(const glm::vec4& background, double font_px)
{
    text_shadow_t shadow;
    shadow.color      = background;
    shadow.color.a   *= k_text_shadow_alpha;
    shadow.radius_px  = std::clamp(
        static_cast<float>(font_px) * k_text_shadow_radius_factor,
        k_text_shadow_min_radius_px,
        k_text_shadow_max_radius_px);
    return shadow;
}

lcd_subpixel_order_t lcd_frame_order(const frame_context_t& ctx)
{
    // Keep request resolution at the draw boundary: Qt supplies the AUTO result
    // in the frame context, while direct-RHI explicit requests can rely on config.
    return detail::lcd_effective_order_for_frame(
        ctx.config ? &ctx.config->lcd_request : nullptr,
        ctx.lcd_subpixel_order);
}

text_lcd_t text_lcd_for_background(
    const frame_context_t& ctx,
    const glm::vec4&       background,
    bool                   draw_lcd_eligible)
{
    text_lcd_t lcd;
    lcd.subpixel_order   = draw_lcd_eligible
        ? lcd_frame_order(ctx)
        : lcd_subpixel_order_t::NONE;
    lcd.background_color = background;
    return lcd;
}

} // anonymous namespace

Text_renderer::Text_renderer(Font_renderer* fr)
    : m_fonts(fr)
{
}

bool Text_renderer::prepare(const frame_context_t& ctx, bool fade_v_labels, bool fade_h_labels)
{
    return prepare(ctx, fade_v_labels, fade_h_labels, false, false);
}

bool Text_renderer::prepare(
    const frame_context_t& ctx,
    bool                   fade_v_labels,
    bool                   fade_h_labels,
    bool                   vertical_axis_label_pane_is_opaque,
    bool                   horizontal_axis_label_pane_is_opaque)
{
    if (!m_fonts) {
        return false;
    }

    m_fonts->rhi_begin_frame();
    bool any_active = false;
    any_active |= render_axis_labels(ctx, fade_v_labels, vertical_axis_label_pane_is_opaque);
    any_active |= render_info_overlay(ctx, fade_h_labels, horizontal_axis_label_pane_is_opaque);
    m_fonts->rhi_finalize_frame(ctx);
    return any_active;
}

void Text_renderer::record(const frame_context_t& ctx)
{
    if (m_fonts) {
        m_fonts->rhi_record_frame(ctx);
    }
}

bool Text_renderer::render_axis_labels(
    const frame_context_t& ctx,
    bool                   fade_labels,
    bool                   vertical_axis_label_pane_is_opaque)
{
    const auto&                pl          = ctx.layout;
    const bool                 dark_mode   = ctx.dark_mode;
    const glm::vec4            font_color  = text_color_for_theme(dark_mode);
    const Color_palette        palette     = resolved_color_palette(ctx.config, dark_mode);
    const lcd_subpixel_order_t frame_order = lcd_frame_order(ctx);
    const bool label_lcd_possible = detail::text_lcd_draw_is_eligible(
        detail::text_lcd_draw_surface_t::VERTICAL_AXIS_LABEL,
        frame_order,
        palette.v_label_background.a,
        vertical_axis_label_pane_is_opaque);

    const float right_edge_x = static_cast<float>(
        pl.usable_width + pl.v_bar_width - k_v_label_horizontal_padding_px);
    const float  min_x        = static_cast<float>(pl.usable_width + k_text_margin_px);
    const float  baseline_off = m_fonts->baseline_offset_px();
    const double v_span       = double(ctx.v1) - double(ctx.v0);

    text_scissor_t label_scissor;
    glm::vec4 label_backing_rect {};
    bool have_label_backing = false;
    if (ctx.rhi) {
        have_label_backing =
            detail::vertical_axis_label_pane_rect(ctx, label_backing_rect) &&
            detail::framebuffer_scissor_from_top_left_rect(
                label_backing_rect, ctx.win_w, ctx.win_h, label_scissor);
        if (!label_scissor.enabled) {
            return detail::update_and_draw_faded_labels(
                pl.v_labels,
                m_vertical_fade,
                k_label_fade_duration_ms,
                fade_labels,
                std::chrono::steady_clock::now(),
                [](const v_label_t& label) { return label.value; },
                [](double, const std::string&, float) {},
                true);
        }
    }

    const auto draw_label = [&](double value, const std::string& text, float alpha) {
        if (!(v_span > 0.0) || !(pl.usable_height > 0.0)) {
            return;
        }

        const double px_per_unit = pl.usable_height / v_span;
        const float  label_y     = static_cast<float>(pl.usable_height - (value - double(ctx.v0)) * px_per_unit);

        const float baseline_target =
            label_y - k_scissor_pad_px
        - k_v_label_vertical_nudge_px * static_cast<float>(ctx.adjusted_font_px);
        const float pen_y = baseline_target - baseline_off;

        const float text_width = m_fonts->measure_text_px(text.c_str());
        float       pen_x      = right_edge_x - text_width;
        if (pen_x < min_x) {
            pen_x = min_x;
        }

        const float snapped_x = std::floor(pen_x + k_pixel_snap);
        const float snapped_y = std::floor(pen_y + k_pixel_snap);

        glm::vec4 text_bounds;
        bool have_text_bounds        = false;
        bool text_fits_label_backing = false;
        if (label_lcd_possible &&
            label_scissor.enabled &&
            have_label_backing &&
            alpha >= detail::k_lcd_opaque_alpha_cutoff)
        {
            have_text_bounds = m_fonts->text_visual_bounds_px(
                text.c_str(), snapped_x, snapped_y, text_bounds);
            if (have_text_bounds) {
                text_fits_label_backing = rect_contains(label_backing_rect, text_bounds);
            }
        }

        m_fonts->batch_text(snapped_x, snapped_y, text.c_str());
        if (ctx.rhi) {
            const bool label_lcd_eligible =
                label_lcd_possible                         &&
                label_scissor.enabled                      &&
                alpha >= detail::k_lcd_opaque_alpha_cutoff &&
                text_fits_label_backing;
            const text_lcd_t label_lcd =
                text_lcd_for_background(ctx, palette.v_label_background, label_lcd_eligible);
            glm::vec4 color = font_color;
            color.a *= alpha;
            m_fonts->rhi_queue_draw(ctx, color, label_scissor, {}, label_lcd);
        }
    };

    return detail::update_and_draw_faded_labels(
        pl.v_labels,
        m_vertical_fade,
        k_label_fade_duration_ms,
        fade_labels,
        std::chrono::steady_clock::now(),
        [](const v_label_t& label) { return label.value; },
        draw_label,
        true);
}

bool Text_renderer::render_info_overlay(
    const frame_context_t& ctx,
    bool                   fade_labels,
    bool                   horizontal_axis_label_pane_is_opaque)
{
    const auto&                pl          = ctx.layout;
    const bool                 dark_mode   = ctx.dark_mode;
    const glm::vec4            font_color  = text_color_for_theme(dark_mode);
    const Color_palette        palette     = resolved_color_palette(ctx.config, dark_mode);
    const lcd_subpixel_order_t frame_order = lcd_frame_order(ctx);
    const bool label_lcd_possible = detail::text_lcd_draw_is_eligible(
        detail::text_lcd_draw_surface_t::HORIZONTAL_AXIS_LABEL,
        frame_order,
        palette.h_label_background.a,
        horizontal_axis_label_pane_is_opaque);
    text_scissor_t label_scissor;
    glm::vec4 label_backing_rect;
    bool have_label_backing = false;
    if (ctx.rhi) {
        have_label_backing =
            detail::horizontal_axis_label_pane_rect(ctx, label_backing_rect) &&
            detail::framebuffer_scissor_from_top_left_rect(
                label_backing_rect, ctx.win_w, ctx.win_h, label_scissor);
    }
    const text_lcd_t overlay_lcd =
        text_lcd_for_background(ctx, ctx.plot_body_background, false);
    const text_shadow_t overlay_shadow =
        text_shadow_for_background(ctx.plot_body_background, ctx.adjusted_font_px);
    const auto t_span = positive_span_ns_as_long_double(ctx.t0, ctx.t1);

    const auto draw_label = [&](std::int64_t t_ns, const std::string& text, float alpha) {
        if (!t_span || !(pl.usable_width > 0.0)) {
            return;
        }

        const long double px_per_unit =
            static_cast<long double>(pl.usable_width) / *t_span;
        const long double t_delta = span_ns_as_long_double(ctx.t0, t_ns);
        const float x_anchor = static_cast<float>(
            t_delta * px_per_unit);
        const float pen_x = x_anchor + k_text_margin_px;
        float pen_y = static_cast<float>(
            pl.usable_height + k_h_label_vertical_nudge_px * ctx.adjusted_font_px);

        glm::vec4 text_bounds;
        bool have_text_bounds        = false;
        bool text_fits_label_scissor = false;
        bool text_fits_label_backing = false;
        if (ctx.rhi && label_scissor.enabled && have_label_backing) {
            have_text_bounds = m_fonts->text_visual_bounds_px(
                text.c_str(), pen_x, pen_y, text_bounds);
            if (have_text_bounds) {
                text_fits_label_scissor =
                    fit_rect_vertically_within(text_bounds, pen_y, label_backing_rect);
                text_fits_label_backing =
                    text_fits_label_scissor &&
                    rect_contains(label_backing_rect, text_bounds);
            }
        }

        m_fonts->batch_text(pen_x, pen_y, text.c_str());
        if (ctx.rhi) {
            const bool label_lcd_eligible =
                label_lcd_possible                         &&
                label_scissor.enabled                      &&
                alpha >= detail::k_lcd_opaque_alpha_cutoff &&
                text_fits_label_backing;
            const text_lcd_t label_lcd =
                text_lcd_for_background(ctx, palette.h_label_background, label_lcd_eligible);
            glm::vec4 color = font_color;
            color.a *= alpha;
            const text_scissor_t draw_scissor =
                label_scissor.enabled ? label_scissor : text_scissor_t{};
            m_fonts->rhi_queue_draw(ctx, color, draw_scissor, {}, label_lcd);
        }
    };

    const bool any_active = detail::update_and_draw_faded_labels(
        pl.h_labels,
        m_horizontal_fade,
        k_label_fade_duration_ms,
        fade_labels,
        std::chrono::steady_clock::now(),
        [](const h_label_t& label) { return label.value; },
        draw_label,
        true,
        !ctx.config || ctx.config->horizontal_axis_left_to_right);

    const bool show_value_range =
        (ctx.visible_info_flags & k_visible_info_value_range) != 0;
    const bool show_time_range =
        (ctx.visible_info_flags & k_visible_info_time_range) != 0;
    const int overlay_line_count = (show_value_range ? 2 : 0) + (show_time_range ? 2 : 0);

    if (overlay_line_count > 0) {
        const float overlay_baseline = m_fonts->compute_numeric_bottom();
        char buf[64];

        const double rh = ctx.adjusted_reserved_height;
        float llt = static_cast<float>(ctx.win_h) - static_cast<float>(rh)
                  - static_cast<float>(ctx.adjusted_font_px * overlay_line_count * k_line_spacing)
                  + overlay_baseline;

        const int info_value_digits = std::max(k_value_decimals, pl.v_label_fixed_digits);
        const auto format_info_value = [&](double value) {
            if (ctx.config && ctx.config->format_value) {
                value_format_context_t context;
                context.role                   = Value_format_role::INFO_OVERLAY;
                context.suggested_fixed_digits = info_value_digits;
                const std::string text = ctx.config->format_value(value, context);
                if (!text.empty()) {
                    return text;
                }
            }
            std::snprintf(buf, sizeof(buf), "%.*f", info_value_digits, value);
            return std::string(buf);
        };

        if (show_value_range) {
            const std::string high_text = "High: " + format_info_value(ctx.v1);
            m_fonts->batch_text(k_overlay_left_px, llt, high_text.c_str());

            llt += static_cast<float>(ctx.adjusted_font_px * k_line_spacing);
            const std::string low_text = "Low:  " + format_info_value(ctx.v0);
            m_fonts->batch_text(k_overlay_left_px, llt, low_text.c_str());

            llt += static_cast<float>(ctx.adjusted_font_px * k_line_spacing);
        }

        if (show_time_range) {
            const char* prefix_from = "From: ";
            m_fonts->batch_text(k_overlay_left_px, llt, prefix_from);
            const float offset_from = m_fonts->measure_text_px(prefix_from);

            // Preserve the visible time resolution even when the axis has no
            // labels. The formatter receives a positive nanoseconds-per-pixel
            // interval, clamped to the timestamp API's supported range.
            const long double timestamp_resolution = t_span
                ? *t_span / std::max(1.0, pl.usable_width)
                : 1.0L;
            constexpr auto max_timestamp_step = std::numeric_limits<std::int64_t>::max();
            const std::int64_t timestamp_step_ns = timestamp_resolution >=
                static_cast<long double>(max_timestamp_step)
                    ? max_timestamp_step
                    : static_cast<std::int64_t>(std::max(timestamp_resolution, 1.0L));
            const bool timestamp_style_changed = timestamp_step_ns != m_last_timestamp_step_ns;
            const bool timestamp_values_changed =
                (ctx.t0 != m_last_t0) || (ctx.t1 != m_last_t1);
            const std::uint64_t timestamp_revision = ctx.config
                ? ctx.config->format_timestamp_revision
                : 0;
            const bool timestamp_formatter_changed =
                timestamp_revision != m_last_timestamp_revision;

            if (timestamp_style_changed  || timestamp_values_changed || timestamp_formatter_changed ||
                m_cached_from_ts.empty() || m_cached_to_ts.empty())
            {
                const auto format_ts = (ctx.config && ctx.config->format_timestamp)
                    ? ctx.config->format_timestamp
                    : default_format_timestamp;
                m_cached_from_ts          = format_ts(ctx.t0, timestamp_step_ns);
                m_cached_to_ts            = format_ts(ctx.t1, timestamp_step_ns);
                m_last_t0                 = ctx.t0;
                m_last_t1                 = ctx.t1;
                m_last_timestamp_revision = timestamp_revision;
                m_last_timestamp_step_ns  = timestamp_step_ns;
            }
            m_fonts->batch_text(k_overlay_left_px + offset_from, llt, m_cached_from_ts.c_str());

            llt += static_cast<float>(ctx.adjusted_font_px * k_line_spacing);
            const char* prefix_to = "To:   ";
            m_fonts->batch_text(k_overlay_left_px, llt, prefix_to);
            const float offset_to = m_fonts->measure_text_px(prefix_to);
            m_fonts->batch_text(k_overlay_left_px + offset_to, llt, m_cached_to_ts.c_str());
        }
    }

    if (overlay_line_count > 0) {
        if (ctx.rhi) {
            m_fonts->rhi_queue_draw(ctx, font_color, {}, overlay_shadow, overlay_lcd);
        }
    }
    return any_active;
}

} // namespace vnm::plot
