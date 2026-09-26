#include "plot_frame_renderer.h"
#include <vnm_plot/core/color_palette.h>
#include <vnm_plot/core/constants.h>
#include <vnm_plot/core/layout_calculator.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/lcd.h>
#include <vnm_plot/core/time_units.h>
#include <vnm_plot/rhi/asset_loader.h>
#include <vnm_plot/rhi/chrome_renderer.h>
#include <vnm_plot/rhi/font_renderer.h>
#include <vnm_plot/rhi/primitive_renderer.h>
#include <vnm_plot/rhi/series_renderer.h>
#include <vnm_plot/rhi/text_renderer.h>
#include "../core/frame_range_planner.h"
#include "../core/label_pane_geometry.h"
#include "../core/lcd_policy.h"

#include <QColor>
#include <QMatrix4x4>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <utility>

namespace vnm::plot::detail {

namespace {

glm::mat4 to_glm_mat4(const QMatrix4x4& matrix)
{
    return glm::make_mat4(matrix.constData());
}

struct label_pane_opacity_t
{
    bool vertical_axis_label_pane_is_opaque   = false;
    bool horizontal_axis_label_pane_is_opaque = false;
};

bool color_is_opaque(const glm::vec4& color)
{
    return color.a >= detail::k_lcd_opaque_alpha_cutoff;
}

label_pane_opacity_t label_pane_opacity_for_text(const frame_context_t& ctx)
{
    const Color_palette palette = resolved_color_palette(ctx.config, ctx.dark_mode);

    glm::vec4 pane_rect;
    label_pane_opacity_t opacity;
    opacity.horizontal_axis_label_pane_is_opaque =
        color_is_opaque(palette.h_label_background) &&
        detail::horizontal_axis_label_pane_rect(ctx, pane_rect);
    opacity.vertical_axis_label_pane_is_opaque =
        color_is_opaque(palette.v_label_background) &&
        detail::vertical_axis_label_pane_rect(ctx, pane_rect);
    return opacity;
}

#if defined(VNM_PLOT_ENABLE_TEXT)
bool spans_approx_equal(long double a, long double b)
{
    const long double diff  = std::abs(a - b);
    const long double scale = std::max(1.0L, std::max(std::abs(a), std::abs(b)));
    return diff <= scale * detail::k_eps;
}
#endif

Layout_calculator::parameters_t build_layout_params(
    float                  v_min,
    float                  v_max,
    std::int64_t           t_min_ns,
    std::int64_t           t_max_ns,
    int                    win_w,
    double                 usable_height,
    double                 vbar_width,
    double                 preview_height,
    double                 font_px,
    const Plot_config&     config,
    const Font_renderer*   fonts)
{
    Layout_calculator::parameters_t params;
    params.v_min                         = v_min;
    params.v_max                         = v_max;
    params.t_min                         = t_min_ns;
    params.t_max                         = t_max_ns;
    params.usable_width                  = std::max(0.0, double(win_w) - vbar_width);
    params.usable_height                 = usable_height;
    params.vbar_width                    = vbar_width;
    params.label_visible_height          = usable_height + preview_height;
    params.adjusted_font_size_in_pixels  = font_px;
    params.h_label_vertical_nudge_factor = detail::k_h_label_vertical_nudge_px;

    if (fonts) {
        params.monospace_char_advance_px     = fonts->monospace_advance_px();
        params.monospace_advance_is_reliable = fonts->monospace_advance_is_reliable();
        params.measure_text_cache_key        = fonts->text_measure_cache_key();
        params.measure_text_func             = [fonts](const char* text) {
            return fonts->measure_text_px(text);
        };
    }

    const Plot_config* config_ptr = &config;
    params.format_timestamp_func = [config_ptr](
        std::int64_t   ts_ns,
        std::int64_t   step_ns) -> std::string
    {
        if (config_ptr->format_timestamp) {
            return config_ptr->format_timestamp(ts_ns, step_ns);
        }
        return default_format_timestamp(ts_ns, step_ns);
    };
    params.format_timestamp_revision     = config.format_timestamp_revision;
    params.horizontal_axis_left_to_right = config.horizontal_axis_left_to_right;
    params.format_value_func             = [config_ptr](
        double                         value,
        const value_format_context_t&  context) -> std::string
    {
        if (config_ptr->format_value) {
            return config_ptr->format_value(value, context);
        }
        return {};
    };
    params.format_value_revision = config.format_value_revision;
    params.profiler              = config.profiler.get();
    return params;
}

} // anonymous namespace

struct Plot_frame_renderer::impl_t
{
    Asset_loader                   asset_loader;
    Series_renderer                series;
    Primitive_renderer             primitives;
    Chrome_renderer                chrome;
    Layout_calculator              layout_calc;
    Layout_cache                   layout_cache;
    detail::Frame_range_planner    frame_range_planner;
    double                         last_vbar_width_pixels = detail::k_vbar_min_width_px_d;
#if defined(VNM_PLOT_ENABLE_TEXT)
    std::unique_ptr<Font_renderer> fonts;
    std::unique_ptr<Text_renderer> text;
    long double                    last_vertical_label_span   = 0.0L;
    long double                    last_horizontal_label_span = 0.0L;
    bool                           label_spans_initialized    = false;
#endif
    bool series_initialized = false;
};

Plot_frame_renderer::Plot_frame_renderer()

:
    m_impl(std::make_unique<impl_t>())
{
    init_embedded_assets(m_impl->asset_loader);
}

Plot_frame_renderer::~Plot_frame_renderer() = default;

void Plot_frame_renderer::initialize()
{
    if (!m_impl->series_initialized) {
        m_impl->series.initialize(m_impl->asset_loader);
        m_impl->series_initialized = true;
    }
#if defined(VNM_PLOT_ENABLE_TEXT)
    if (!m_impl->fonts) {
        m_impl->fonts = std::make_unique<Font_renderer>(m_impl->asset_loader);
        m_impl->text  = std::make_unique<Text_renderer>(m_impl->fonts.get());
    }
#endif
}

Plot_frame_result Plot_frame_renderer::render(
    const Plot_render_snapshot& snapshot,
    QRhi*                       rhi_ptr,
    QRhiRenderTarget*           rt,
    QRhiCommandBuffer*          cb)
{
    const Plot_config& config = snapshot.config;
    Profiler* profiler = config.profiler.get();
    Plot_frame_result result;

    VNM_PLOT_PROFILE_SCOPE(profiler, "renderer");
    VNM_PLOT_PROFILE_SCOPE(profiler, "renderer.frame");

    const Color_palette palette = resolved_color_palette(&config, config.dark_mode);
    const glm::vec4 plot_body_background =
        config.clear_to_transparent ? snapshot.window_background : palette.background;

    QColor clear_color;
    if (config.clear_to_transparent) {
        clear_color = QColor(0, 0, 0, 0);
    }
    else {
        clear_color = QColor::fromRgbF(
            palette.background.r,
            palette.background.g,
            palette.background.b,
            palette.background.a);
    }

    QSize pixel_size;
    {
        VNM_PLOT_PROFILE_SCOPE(profiler, "renderer.frame.fb_size");
        pixel_size = rt->pixelSize();
    }
    const int win_w = pixel_size.width();
    const int win_h = pixel_size.height();

    m_impl->primitives.set_profiler(profiler);
    const auto log_error = config.log_error;
    const auto log_debug_info = config.log_debug_info;
    m_impl->asset_loader.set_log_callback(log_error);
    m_impl->primitives.set_log_callback(log_error);

    const double reserved_h = snapshot.base_label_height_px + snapshot.adjusted_preview_height;
    const bool preview_enabled =
        snapshot.adjusted_preview_height > 0.0 && config.preview_visibility > 0.0;
    const Frame_range_plan frame_plan = m_impl->frame_range_planner.plan(
        snapshot.series,
        snapshot.data_cfg,
        config,
        snapshot.v_auto,
        preview_enabled);
    const float v_min         = frame_plan.main_v_range.min;
    const float v_max         = frame_plan.main_v_range.max;
    const float preview_v_min = frame_plan.preview_v_range.min;
    const float preview_v_max = frame_plan.preview_v_range.max;

#if defined(VNM_PLOT_ENABLE_TEXT)
    const long double vertical_label_span =
        static_cast<long double>(v_max) - static_cast<long double>(v_min);
    const auto horizontal_label_span = positive_span_ns_as_long_double(
        snapshot.data_cfg.t_min,
        snapshot.data_cfg.t_max);
    const bool fade_v_labels =
        m_impl->label_spans_initialized                                           &&
        vertical_label_span > 0.0L                                                &&
        m_impl->last_vertical_label_span > 0.0L                                   &&
        !spans_approx_equal(vertical_label_span, m_impl->last_vertical_label_span);
    const bool fade_h_labels =
        m_impl->label_spans_initialized                                                &&
        horizontal_label_span                                                          &&
        m_impl->last_horizontal_label_span > 0.0L                                      &&
        !spans_approx_equal(*horizontal_label_span, m_impl->last_horizontal_label_span);
    m_impl->last_vertical_label_span   = vertical_label_span;
    m_impl->last_horizontal_label_span = horizontal_label_span.value_or(0.0L);
    m_impl->label_spans_initialized    = true;

    if (m_impl->fonts) {
        m_impl->fonts->set_log_callbacks(log_error, log_debug_info);
    }
    if (m_impl->fonts && config.show_text) {
        const int font_px_int = static_cast<int>(std::lround(snapshot.adjusted_font_px));
        if (font_px_int > 0) {
            m_impl->fonts->initialize_metrics(font_px_int);
        }
    }
    const Font_renderer* layout_fonts =
        (config.show_text && m_impl->fonts && m_impl->fonts->text_measure_cache_key() != 0)
            ? m_impl->fonts.get()
            : nullptr;
#else
    const Font_renderer* layout_fonts = nullptr;
#endif

    const double usable_height = std::max(0.0, double(win_h) - reserved_h);
    const double max_vbar_width = std::max(
        detail::k_vbar_min_width_px_d,
        std::max(0.0, double(win_w) * 0.5));
    double vbar_width = snapshot.vbar_width_pixels;
    if (!std::isfinite(vbar_width) || vbar_width <= detail::k_vbar_min_width_px_d) {
        vbar_width = m_impl->last_vbar_width_pixels;
    }
    if (!std::isfinite(vbar_width) || vbar_width <= detail::k_vbar_min_width_px_d) {
        vbar_width = detail::k_vbar_min_width_px_d;
    }
    vbar_width = std::clamp(vbar_width, detail::k_vbar_min_width_px_d, max_vbar_width);

    const auto make_cache_key = [&](double width_px) {
        layout_cache_key_t key;
        key.v0                           = v_min;
        key.v1                           = v_max;
        key.t0                           = snapshot.data_cfg.t_min;
        key.t1                           = snapshot.data_cfg.t_max;
        key.viewport_size                = Size_2i{win_w, win_h};
        key.adjusted_reserved_height     = reserved_h;
        key.adjusted_preview_height      = snapshot.adjusted_preview_height;
        key.adjusted_font_size_in_pixels = snapshot.adjusted_font_px;
        key.vbar_width_pixels            = width_px;
        key.font_metrics_key             = layout_fonts ? layout_fonts->text_measure_cache_key() : 0;
        key.config_revision              = snapshot.config_revision;
        key.format_timestamp_revision    = config.format_timestamp_revision;
        key.format_value_revision        = config.format_value_revision;
        return key;
    };

    const auto calculate_layout = [&](double width_px) {
        const auto params = build_layout_params(
            v_min,
            v_max,
            snapshot.data_cfg.t_min,
            snapshot.data_cfg.t_max,
            win_w,
            usable_height,
            width_px,
            snapshot.adjusted_preview_height,
            snapshot.adjusted_font_px,
            config,
            layout_fonts);
        return m_impl->layout_calc.calculate(params);
    };

    const frame_layout_result_t* layout_ptr =
        m_impl->layout_cache.try_get(make_cache_key(vbar_width));

    if (!layout_ptr) {
        auto layout_result = calculate_layout(vbar_width);

        double measured_vbar_width = std::max(
            detail::k_vbar_min_width_px_d,
            double(layout_result.max_v_label_text_width) + detail::k_v_label_horizontal_padding_px);
        if (!std::isfinite(measured_vbar_width) || measured_vbar_width <= 0.0) {
            measured_vbar_width = detail::k_vbar_min_width_px_d;
        }
        measured_vbar_width = std::clamp(
            measured_vbar_width,
            detail::k_vbar_min_width_px_d,
            max_vbar_width);

        if (std::abs(measured_vbar_width - vbar_width) > detail::k_vbar_width_change_threshold_d) {
            vbar_width = measured_vbar_width;
            layout_ptr = m_impl->layout_cache.try_get(make_cache_key(vbar_width));
            if (!layout_ptr) {
                layout_result = calculate_layout(vbar_width);
            }
        }
        else {
            vbar_width = measured_vbar_width;
        }

        if (!layout_ptr) {
            frame_layout_result_t cached_layout;
            cached_layout.usable_width = std::max(0.0, double(win_w) - vbar_width);
            cached_layout.usable_height = usable_height;
            cached_layout.v_bar_width = vbar_width;
            cached_layout.h_bar_height = snapshot.base_label_height_px + detail::k_scissor_pad_px;
            cached_layout.max_v_label_text_width = layout_result.max_v_label_text_width;
            cached_layout.v_labels = std::move(layout_result.v_labels);
            cached_layout.h_labels = std::move(layout_result.h_labels);
            cached_layout.v_label_fixed_digits = layout_result.v_label_fixed_digits;
            cached_layout.h_labels_subsecond = layout_result.h_labels_subsecond;
            layout_ptr = &m_impl->layout_cache.store(make_cache_key(vbar_width), std::move(cached_layout));
        }
    }

    vbar_width = layout_ptr->v_bar_width;
    m_impl->last_vbar_width_pixels = vbar_width;

    frame_context_t ctx{*layout_ptr};
    ctx.v0         = v_min;
    ctx.v1         = v_max;
    ctx.preview_v0 = preview_v_min;
    ctx.preview_v1 = preview_v_max;
    ctx.t0 = snapshot.data_cfg.t_min;
    ctx.t1 = snapshot.data_cfg.t_max;
    ctx.t_available_min = snapshot.data_cfg.t_available_min;
    ctx.t_available_max = snapshot.data_cfg.t_available_max;
    ctx.win_w           = win_w;
    ctx.win_h           = win_h;
    // Pixel-space ortho with origin at top-left. QRhi's correction matrix
    // adapts it to the active backend's clip-space conventions.
    const glm::mat4 pixel_ortho = glm::ortho(
        0.0f,
        static_cast<float>(win_w),
        static_cast<float>(win_h),
        0.0f,
        -1.0f,
        1.0f);
    ctx.pmv                      = rhi_ptr
        ? to_glm_mat4(rhi_ptr->clipSpaceCorrMatrix()) * pixel_ortho
        : pixel_ortho;
    ctx.adjusted_font_px         = snapshot.adjusted_font_px;
    ctx.base_label_height_px     = snapshot.base_label_height_px;
    ctx.adjusted_reserved_height = reserved_h;
    ctx.adjusted_preview_height  = snapshot.adjusted_preview_height;
    ctx.visible_info_flags       = snapshot.visible_info_flags;
    ctx.dark_mode                = config.dark_mode;
    ctx.plot_body_background     = plot_body_background;
    ctx.lcd_subpixel_order       = snapshot.auto_lcd_subpixel_order;
    ctx.config                   = &config;
    ctx.rhi                      = rhi_ptr;
    ctx.cb                       = cb;
    ctx.render_target            = rt;

    auto& feedback = result.feedback;
    feedback.measured_vbar_width = vbar_width;
    feedback.v_min                = ctx.v0;
    feedback.v_max                = ctx.v1;
    feedback.t_min                = ctx.t0;
    feedback.t_max                = ctx.t1;
    feedback.t_available_min      = ctx.t_available_min;
    feedback.t_available_max      = ctx.t_available_max;
    feedback.series_revision      = snapshot.series_revision;

    // Open the resource-update batch BEFORE the render pass. Both series and
    // primitives fill it (series via prepare(), primitives via flush_rects /
    // draw_grid_shader called from chrome); beginPass then submits the
    // now-full batch atomically before any draw in the pass.
    // cb->resourceUpdate from inside an open pass is a hard error on D3D11
    // (recordingPass != NoPass asserts), so the upload work has to be over
    // by the time beginPass runs. record_draws() and series.render()
    // afterwards record draw commands only.
    {
        VNM_PLOT_PROFILE_SCOPE(profiler, "renderer.frame.render_passes");

        QRhiResourceUpdateBatch* rhi_updates =
            rhi_ptr ? rhi_ptr->nextResourceUpdateBatch() : nullptr;
        ctx.rhi_updates = rhi_updates;

        if (m_impl->series_initialized) {
            m_impl->series.prepare(ctx, snapshot.series);
            detail::fill_stack_feedback(m_impl->series, feedback);
        }

        const Text_renderer* prepared_text = nullptr;
#if defined(VNM_PLOT_ENABLE_TEXT)
        if (m_impl->text && config.show_text) {
            const label_pane_opacity_t pane_opacity = label_pane_opacity_for_text(ctx);
            const bool fades_active = m_impl->text->prepare(
                ctx,
                fade_v_labels,
                fade_h_labels,
                pane_opacity.vertical_axis_label_pane_is_opaque,
                pane_opacity.horizontal_axis_label_pane_is_opaque);
            prepared_text = m_impl->text.get();
            if (fades_active) {
                result.needs_update = true;
            }
        }
#endif

        // Chrome runs in two queueing phases so backgrounds and the main grid
        // stay behind the series while the zero line and preview overlay stay
        // in front. Both phases write to ctx.rhi_updates before beginPass so
        // all uploads are submitted atomically; the checkpoint between them
        // lets record_draws() replay the back-layer slice, then the series
        // renders, then record_draws() replays the front-layer slice.
        m_impl->chrome.render_grid_and_backgrounds(
            ctx,
            m_impl->primitives,
            prepared_text);
        const std::size_t back_layer_end = m_impl->primitives.queued_op_count();
        m_impl->chrome.render_zero_line(ctx, m_impl->primitives);
        if (snapshot.adjusted_preview_height > 0.0) {
            m_impl->chrome.render_preview_overlay(ctx, m_impl->primitives);
        }
        const std::size_t front_layer_end = m_impl->primitives.queued_op_count();

        cb->beginPass(rt, clear_color, QRhiDepthStencilClearValue(1.0f, 0), rhi_updates);
        cb->setViewport(QRhiViewport(0, 0, win_w, win_h));
        m_impl->primitives.record_draws(ctx, back_layer_end);
        if (m_impl->series_initialized) {
            m_impl->series.render(ctx, snapshot.series);
        }
        m_impl->primitives.record_draws(ctx, front_layer_end);
#if defined(VNM_PLOT_ENABLE_TEXT)
        if (m_impl->text && config.show_text) {
            m_impl->text->record(ctx);
        }
#endif
        cb->endPass();
        m_impl->primitives.reset_frame();
    }

    return result;
}

} // namespace vnm::plot::detail
