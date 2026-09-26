#include <vnm_plot/rhi/chrome_renderer.h>
#include "grid_helpers.h"
#include <vnm_plot/rhi/primitive_renderer.h>
#include <vnm_plot/rhi/text_renderer.h>
#include <vnm_plot/core/color_palette.h>
#include <vnm_plot/core/constants.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/time_grid.h>
#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/time_units.h>
#include "lcd_policy.h"
#include "label_pane_geometry.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace vnm::plot {
using detail::get_shift;
using detail::k_cell_span_max_factor;
using detail::k_cell_span_min_factor;
using detail::k_grid_line_alpha_base;
using detail::k_preview_band_max_px;
using detail::k_preview_min_window_px;

namespace {

grid_layer_params_t flip_grid_levels_y(const grid_layer_params_t& levels, float height_px)
{
    grid_layer_params_t flipped = levels;
    for (int i = 0; i < flipped.count; ++i) {
        flipped.start_px[i] = height_px - flipped.start_px[i];
    }
    return flipped;
}

glm::vec2 to_gl_origin(
    const frame_context_t& ctx,
    const glm::vec2&       top_left,
    const glm::vec2&       size)
{
    const float win_h = static_cast<float>(ctx.win_h);
    return {top_left.x, win_h - (top_left.y + size.y)};
}

} // anonymous namespace

grid_layer_params_t Chrome_renderer::calculate_grid_params(
    double min,
    double max,
    double pixel_span,
    double font_px)
{
    grid_layer_params_t levels;
    const double range = max - min;
    if (!(range > 0.0) || !(pixel_span > 0.0)) {
        return levels;
    }

    double step = 0.0;
    int idx = 0;
    detail::select_decade_step(range, step, idx);

    const double cell_span_min = font_px * k_cell_span_min_factor;
    const double fade_den      = std::max<double>(1e-6, font_px * (k_cell_span_max_factor - k_cell_span_min_factor));
    const double px_per_unit   = pixel_span / range;

    while (levels.count < grid_layer_params_t::k_max_levels && step * px_per_unit >= cell_span_min) {
        const float  spacing_px  = static_cast<float>(step * px_per_unit);
        const double shift_units = get_shift(step, min);
        detail::append_grid_level(levels, spacing_px,
            static_cast<float>(pixel_span - shift_units * px_per_unit), cell_span_min, fade_den);

        step /= detail::decade_step_factor(--idx);
    }

    return levels;
}

void Chrome_renderer::render_grid_and_backgrounds(
    const frame_context_t& ctx,
    Primitive_renderer&    prims,
    const Text_renderer*   prepared_text)
{
    vnm::plot::Profiler* profiler = ctx.config ? ctx.config->profiler.get() : nullptr;
    VNM_PLOT_PROFILE_SCOPE(
        profiler,
        "renderer.frame.chrome.grid_and_backgrounds");

    const auto&         pl      = ctx.layout;
    const Color_palette palette = resolved_color_palette(ctx.config, ctx.dark_mode);

    const glm::vec4 h_label_color   = palette.h_label_background;
    const glm::vec4 v_label_color   = palette.v_label_background;
    const double    grid_visibility = ctx.config ? ctx.config->grid_visibility : 1.0;
    const glm::vec4 grid_rgb = glm::vec4(
        palette.grid_line.r,
        palette.grid_line.g,
        palette.grid_line.b,
        palette.grid_line.a * static_cast<float>(grid_visibility));
    const glm::vec4 tick_rgb           = grid_rgb;
    const glm::vec4 preview_background = palette.preview_background;
    const glm::vec4 separator_color    = palette.separator;
    const lcd_subpixel_order_t frame_order = detail::lcd_effective_order_for_frame(
        ctx.config ? &ctx.config->lcd_request : nullptr,
        ctx.lcd_subpixel_order);
    const bool plot_body_is_opaque =
        (ctx.config == nullptr || !ctx.config->clear_to_transparent) &&
        ctx.plot_body_background.a >= detail::k_lcd_opaque_alpha_cutoff;
    const lcd_subpixel_order_t grid_order = detail::grid_lcd_subpixel_order(
        frame_order,
        ctx.plot_body_background.a,
        plot_body_is_opaque);

    // Background panes and separators (batch_rect is CPU work)
    prims.batch_rect(preview_background,
        glm::vec4(0.f, float(ctx.win_h) - float(ctx.adjusted_preview_height), float(ctx.win_w), float(ctx.win_h)));

    glm::vec4 label_pane_rect;
    const bool has_h_label_pane = detail::horizontal_axis_label_pane_rect(ctx, label_pane_rect);
    if (has_h_label_pane) {
        prims.batch_rect(h_label_color, label_pane_rect);
    }
    if (detail::vertical_axis_label_pane_rect(ctx, label_pane_rect)) {
        prims.batch_rect(v_label_color, label_pane_rect);
    }
    prims.batch_rect(separator_color,
        glm::vec4(0.f, float(ctx.win_h) - float(ctx.adjusted_reserved_height),
            float(ctx.win_w), float(ctx.win_h - ctx.adjusted_reserved_height + 1.0)));
    prims.batch_rect(separator_color,
        glm::vec4(0.f, float(ctx.win_h) - float(ctx.adjusted_preview_height + 1.0),
            float(ctx.win_w), float(ctx.win_h) - float(ctx.adjusted_preview_height)));

    prims.flush_rects(ctx, ctx.pmv);

    // Grid - CPU calculations
    const glm::vec2 main_top_left{0.0f, 0.0f};
    const glm::vec2 main_size{float(pl.usable_width), float(pl.usable_height)};
    const glm::vec2 main_origin = to_gl_origin(ctx, main_top_left, main_size);

    const grid_layer_params_t vertical_levels = calculate_grid_params(
        double(ctx.v0),
        double(ctx.v1),
        pl.usable_height,
        ctx.adjusted_font_px);
    // Grid spacing math runs in fp64 seconds; the shift origin is the
    // (rebased) seconds-domain t_min that the axis uniforms already use.
    constexpr double k_seconds_per_ns          = 1.0e-9;
    const double     t0_seconds                = static_cast<double>(ctx.t0) * k_seconds_per_ns;
    const double     t1_seconds                = static_cast<double>(ctx.t1) * k_seconds_per_ns;
    bool             dropped_non_multiple_step = false;
    const grid_layer_params_t horizontal_levels = build_time_grid_layers(
        t0_seconds,
        t1_seconds,
        pl.usable_width,
        ctx.adjusted_font_px,
        &dropped_non_multiple_step);
    if (dropped_non_multiple_step && ctx.config && ctx.config->log_debug_info) {
        ctx.config->log_debug_info(
            "vnm_plot: dropping non-multiple time grid step; adjust "
            "build_time_steps_covering() subdivision levels to exact multiples.");
    }

    const grid_layer_params_t vertical_levels_gl = flip_grid_levels_y(vertical_levels, main_size.y);

    if (grid_visibility > 0.0) {
        prims.draw_grid_shader(
            ctx,
            main_origin,
            main_size,
            grid_rgb,
            vertical_levels_gl,
            horizontal_levels,
            grid_order,
            ctx.plot_body_background);
    }

    const auto match_level_properties = [](
        float                      pos,
        const grid_layer_params_t& levels) -> std::pair<float, float>
    {
        float alpha = k_grid_line_alpha_base;
        float thick = 0.8f;
        for (int i = 0; i < levels.count; ++i) {
            const float spacing = levels.spacing_px[i];
            if (spacing <= 0.0f) {
                continue;
            }
            double wrapped = std::fmod(double(pos) - double(levels.start_px[i]), double(spacing));
            if (wrapped < 0.0) {
                wrapped += double(spacing);
            }
            const double dist = std::min(wrapped, double(spacing) - wrapped);
            if (dist <= 0.35) {
                alpha = levels.alpha[i];
                thick = levels.thickness_px[i];
                break;
            }
        }
        return {alpha, thick};
    };

    const auto build_tick_levels = [&](
        auto&&                     get_pos,
        auto&&                     get_alpha,
        const auto&                labels,
        const grid_layer_params_t& main_levels)
    {
        grid_layer_params_t ticks;
        for (const auto& label : labels) {
            if (ticks.count >= grid_layer_params_t::k_max_levels) {
                break;
            }
            const float label_alpha = std::clamp(get_alpha(label), 0.0f, 1.0f);
            if (label_alpha <= 0.0f) {
                continue;
            }
            const float pos   = get_pos(label);
            const auto  props = match_level_properties(pos, main_levels);
            ticks.spacing_px[ticks.count]   = 1e6f;
            ticks.start_px[ticks.count]     = pos;
            ticks.alpha[ticks.count]        = props.first * label_alpha;
            ticks.thickness_px[ticks.count] = props.second;
            ++ticks.count;
        }
        return ticks;
    };

    grid_layer_params_t empty_levels;
    grid_layer_params_t vertical_tick_levels;
    grid_layer_params_t horizontal_tick_levels;
    if (prepared_text) {
        const double v_span = double(ctx.v1) - double(ctx.v0);
        if (v_span > 0.0) {
            const double px_per_unit = pl.usable_height / v_span;
            vertical_tick_levels = build_tick_levels(
                [&](const auto& entry) {
                    return static_cast<float>(
                        pl.usable_height - (entry.first - double(ctx.v0)) * px_per_unit);
                },
                [](const auto& entry) { return entry.second; },
                prepared_text->m_vertical_fade.visible_alphas,
                vertical_levels);
        }

        const auto t_span = positive_span_ns_as_long_double(ctx.t0, ctx.t1);
        if (t_span) {
            const long double px_per_unit =
                static_cast<long double>(pl.usable_width) / *t_span;
            horizontal_tick_levels = build_tick_levels(
                [&](const auto& entry) {
                    return static_cast<float>(
                        span_ns_as_long_double(ctx.t0, entry.first) * px_per_unit);
                },
                [](const auto& entry) { return entry.second; },
                prepared_text->m_horizontal_fade.visible_alphas,
                horizontal_levels);
        }
    }
    else {
        vertical_tick_levels   = build_tick_levels(
            [](const v_label_t& label) { return label.y; },
            [](const v_label_t&) { return 1.0f; },
            pl.v_labels,
            vertical_levels);
        horizontal_tick_levels = build_tick_levels(
            [](const h_label_t& label) { return label.position.x; },
            [](const h_label_t&) { return 1.0f; },
            pl.h_labels,
            horizontal_levels);
    }
    const grid_layer_params_t vertical_tick_levels_gl = flip_grid_levels_y(vertical_tick_levels, main_size.y);

    if (grid_visibility > 0.0 && pl.v_bar_width > 0.5 && vertical_tick_levels_gl.count > 0) {
        const glm::vec2 top_left{float(pl.usable_width), 0.0f};
        const glm::vec2 size{float(pl.v_bar_width), float(pl.usable_height)};
        const glm::vec2 origin = to_gl_origin(ctx, top_left, size);
        prims.draw_grid_shader(
            ctx,
            origin,
            size,
            tick_rgb,
            vertical_tick_levels_gl,
            empty_levels,
            lcd_subpixel_order_t::NONE,
            v_label_color);
    }

    if (grid_visibility > 0.0 && ctx.base_label_height_px > 0.5 && horizontal_tick_levels.count > 0) {
        const glm::vec2 top_left{0.0f, float(pl.usable_height)};
        const glm::vec2 size{float(pl.usable_width), float(ctx.base_label_height_px)};
        const glm::vec2 origin = to_gl_origin(ctx, top_left, size);
        const lcd_subpixel_order_t gutter_order = detail::grid_lcd_subpixel_order(
            frame_order,
            h_label_color.a,
            has_h_label_pane && grid_order != lcd_subpixel_order_t::NONE);
        prims.draw_grid_shader(
            ctx,
            origin,
            size,
            tick_rgb,
            empty_levels,
            horizontal_tick_levels,
            gutter_order,
            h_label_color);
    }
}

void Chrome_renderer::render_zero_line(
    const frame_context_t& ctx,
    Primitive_renderer&    prims)
{
    const double range_v = double(ctx.v1) - double(ctx.v0);
    if (!(range_v > 0.0)) {
        return;
    }

    // Pixel position of value 0.0 measured from the bottom of the plot area.
    const float zero_y_gl = static_cast<float>(
        ctx.layout.usable_height * (0.0 - double(ctx.v0)) / range_v);

    if (zero_y_gl < 0.0f || zero_y_gl > static_cast<float>(ctx.layout.usable_height)) {
        return;
    }

    const Color_palette palette = resolved_color_palette(ctx.config, ctx.dark_mode);
    const glm::vec4     color   = palette.grid_line;

    const auto& pl = ctx.layout;
    const glm::vec2 main_top_left{0.0f, 0.0f};
    const glm::vec2 main_size{float(pl.usable_width), float(pl.usable_height)};
    const glm::vec2 main_origin = to_gl_origin(ctx, main_top_left, main_size);

    grid_layer_params_t zero_level;
    zero_level.count           = 1;
    zero_level.spacing_px[0]   = 1e6f;
    zero_level.start_px[0]     = zero_y_gl;
    zero_level.alpha[0]        = k_grid_line_alpha_base;
    zero_level.thickness_px[0] = 1.2f;

    grid_layer_params_t empty_levels;
    prims.draw_grid_shader(
        ctx,
        main_origin,
        main_size,
        color,
        zero_level,
        empty_levels,
        lcd_subpixel_order_t::NONE,
        ctx.plot_body_background);

    if (pl.v_bar_width > 0.5) {
        const glm::vec2 v_bar_top_left{float(pl.usable_width), 0.0f};
        const glm::vec2 v_bar_size{float(pl.v_bar_width), float(pl.usable_height)};
        const glm::vec2 v_bar_origin = to_gl_origin(ctx, v_bar_top_left, v_bar_size);
        prims.draw_grid_shader(
            ctx,
            v_bar_origin,
            v_bar_size,
            color,
            zero_level,
            empty_levels,
            lcd_subpixel_order_t::NONE,
            palette.v_label_background);
    }
}

void Chrome_renderer::render_preview_overlay(
    const frame_context_t& ctx,
    Primitive_renderer&    prims)
{
    vnm::plot::Profiler* profiler = ctx.config ? ctx.config->profiler.get() : nullptr;
    VNM_PLOT_PROFILE_SCOPE(
        profiler,
        "renderer.frame.chrome.preview_overlay");

    if (ctx.adjusted_preview_height <= 0.) {
        return;
    }

    const auto t_avail_span_ns = positive_span_ns_as_long_double(
        ctx.t_available_min,
        ctx.t_available_max);
    if (!t_avail_span_ns) {
        return;
    }
    const long double t_avail_span = *t_avail_span_ns;

    const Color_palette palette         = resolved_color_palette(ctx.config, ctx.dark_mode);
    const glm::vec4     cover_color     = palette.preview_cover;
    const glm::vec4     cover_color2    = palette.preview_cover_secondary;
    const glm::vec4     separator_color = palette.separator;

    // CPU calculations
    const long double scaled_left_span =
        static_cast<long double>(ctx.win_w) *
        span_ns_as_long_double(ctx.t_available_min, ctx.t0);
    const double x0 = static_cast<double>(scaled_left_span / t_avail_span);
    const double x1 = static_cast<double>(
        (static_cast<long double>(ctx.win_w) *
            (1.0L - span_ns_as_long_double(ctx.t1, ctx.t_available_max) / t_avail_span)));

    const double dd    = x1 - x0;
    const double win_w = ctx.win_w;
    // Top of the preview band, derived directly from win_h and the band's
    // own height. The earlier formulation went via usable_height + label
    // height, which only matches when the label band is rendered into the
    // strip between data and preview; with the label band currently empty
    // on the RHI path that path leaves usable_height pointing higher than
    // the preview band's top. Anchoring on the band itself stays correct
    // either way.
    const double ptop = ctx.win_h - ctx.adjusted_preview_height;
    const double pbtm = ctx.win_h;

    const double pband_h = std::min(k_preview_band_max_px, ctx.adjusted_preview_height);

    // batch_rect is CPU work
    if (dd >= k_preview_min_window_px) {
        prims.batch_rect(cover_color, {0, float(ptop + pband_h), float(x0), float(pbtm - pband_h)});
        prims.batch_rect(cover_color, {float(x1), float(ptop + pband_h), float(win_w), float(pbtm - pband_h)});
    }
    else {
        const double xx = 0.5 * (k_preview_min_window_px - dd);
        prims.batch_rect(cover_color, {float(0 - xx), float(ptop + pband_h), float(x0 - xx), float(pbtm - pband_h)});
        prims.batch_rect(
            cover_color, {float(x1 + xx), float(ptop + pband_h), float(win_w + xx), float(pbtm - pband_h)});
        prims.batch_rect(cover_color2, {float(x0 - xx), float(ptop + pband_h), float(x0), float(pbtm - pband_h)});
        prims.batch_rect(cover_color2, {float(x1), float(ptop + pband_h), float(x1 + xx), float(pbtm - pband_h)});
    }

    prims.batch_rect(cover_color, {0, float(ptop), float(win_w), float(ptop + pband_h)});
    prims.batch_rect(cover_color, {0, float(pbtm - pband_h), float(win_w), float(pbtm)});

    prims.batch_rect(separator_color, {float(x0 - 1), float(ptop + pband_h - 1), float(x1 + 1), float(ptop + pband_h)});
    prims.batch_rect(separator_color, {float(x0 - 1), float(pbtm - pband_h), float(x1 + 1), float(pbtm - pband_h + 1)});
    prims.batch_rect(separator_color, {float(x0 - 1), float(ptop + pband_h), float(x0), float(pbtm - pband_h)});
    prims.batch_rect(separator_color, {float(x1), float(ptop + pband_h), float(x1 + 1), float(pbtm - pband_h)});
    prims.batch_rect(separator_color, {0, float(pbtm - 1), float(win_w), float(pbtm)});

    // The overlay must paint after the series so it dims any out-of-window
    // samples. Closing the rect batch here keeps the API self-contained: the
    // caller doesn't have to flush_rects after every chrome step.
    prims.flush_rects(ctx, ctx.pmv);
}

} // namespace vnm::plot
