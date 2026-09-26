#pragma once

// VNM Plot Library - RHI Series Renderer
// QRhi series data rendering with LOD support.

#include <vnm_plot/core/series_window.h>
#include <vnm_plot/rhi/frame_context.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiResourceUpdateBatch;
class QRhiShaderResourceBindings;

namespace vnm::plot {

class Asset_loader;
class Profiler;
class Series_renderer;

namespace detail {
struct series_window_planner_state_t;
struct Series_window_snapshot_cache;
struct plot_render_feedback_t;

// Copies the stack validity a completed pass produced into the render feedback
// the plot widget applies. The Qt layer owns plot_render_feedback_t and defines
// this in src/qt/plot_render_feedback.cpp; the declaration lives here only so
// that Series_renderer can befriend the single producer of that feedback.
void fill_stack_feedback(const Series_renderer& series, plot_render_feedback_t& feedback);

// One contiguous run of GPU samples a built-in LINE/AREA primitive draws as a
// single strip. Derived purely from a sample_window_t; computed once during
// prepare and carried on the prepared draw command so the record pass does not
// recompute it.
struct builtin_segment_span_t
{
    std::size_t gpu_first = 0;
    std::size_t gpu_count = 0;
};
} // namespace detail

// -----------------------------------------------------------------------------
// Series Renderer
// -----------------------------------------------------------------------------
// Renders data series using QRhi. Manages buffers and pipelines internally.
class Series_renderer
{
public:
    Series_renderer();
    ~Series_renderer();

    Series_renderer(const Series_renderer&)            = delete;
    Series_renderer& operator=(const Series_renderer&) = delete;

    // Optionally supply assets to custom layers. Built-in shaders are loaded
    // from Qt resources, and prepare() also works without an asset loader.
    void initialize(Asset_loader& asset_loader);

    // Release resources explicitly while the owning QRhi is still alive.
    // This also calls each custom layer's cleanup_qrhi_resources hook.
    void cleanup_resources();

    // Two-phase rendering. Under RHI, the host opens a resource-update batch,
    // calls prepare() to fill it with sample/UBO/per-frame uploads, calls
    // beginPass(rt, clear, depth, batch) to atomically submit those uploads
    // before any draw, and then calls render() inside the open pass to record
    // the draw commands. The split is forced by D3D11: cb->resourceUpdate is
    // only legal outside an open pass, and beginPass consumes its 4th
    // argument before the renderer's own draws would otherwise have written
    // to it.
    void prepare(
        const frame_context_t& ctx,
        const std::map<int, std::shared_ptr<const series_data_t>>&
                               series);

    // Render all series in the frame context. Records draws only when
    // ctx.rhi is non-null; uploads must already have been submitted via
    // prepare() + beginPass(batch).
    void render(
        const frame_context_t& ctx,
        const std::map<int, std::shared_ptr<const series_data_t>>&
                               series);

private:
    friend class Test_series_renderer;
    friend void detail::fill_stack_feedback(
        const Series_renderer&              series,
        detail::plot_render_feedback_t&     feedback);

    const std::map<int, std::vector<detail::Rendered_stack_source>>&
    main_stack_validity() const { return m_main_stack_validity; }

    const std::map<std::pair<int, Series_view_kind>, detail::Rendered_stack_status>&
    stack_view_statuses() const { return m_stack_view_statuses; }

    struct gpu_sample_t
    {
        float t_rel;
        float y;
        float y_min;
        float y_max;
    };

    struct vbo_view_state_t
    {
        struct line_draw_span_t
        {
            std::size_t gpu_first  = 0;
            std::size_t gpu_count  = 0;
            std::size_t line_first = 0;
            std::size_t line_count = 0;
        };

        bool has_uploaded_vbo = false;
        std::unique_ptr<detail::series_window_planner_state_t> planner;

        // Renderer-owned scratch buffer for VBO uploads. Holds the planned
        // visible gpu_sample_t values rebased against the active origin.
        // Reused across uploads to avoid reallocation.
        std::vector<gpu_sample_t>      staging;
#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
        std::size_t                    last_staged_sample_count         = 0;
#endif
        std::size_t                    last_sample_upload_bytes         = 0;
        std::size_t                    last_sample_upload_count         = 0;
        std::size_t                    last_line_window_upload_bytes    = 0;
        std::size_t                    last_line_window_upload_count    = 0;
        std::size_t                    last_uniform_upload_bytes        = 0;
        std::size_t                    last_uniform_upload_count        = 0;
#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
        std::size_t                    last_primitive_prepare_count     = 0;
        std::size_t                    last_line_window_sample_count    = 0;
        std::size_t                    last_recorded_line_span_count    = 0;
        std::size_t                    last_recorded_line_segment_count = 0;
        std::size_t                    last_recorded_area_span_count    = 0;
        std::size_t                    last_recorded_area_segment_count = 0;
        std::size_t                    last_recorded_dot_sample_count   = 0;
        std::int64_t                   last_prepared_t_min_ns           = 0;
        std::int64_t                   last_prepared_t_max_ns           = 0;
        double                         last_prepared_width_px           = 0.0;
        std::size_t                    last_vbo_generation              = 0;
        QRhiBuffer*                    last_sample_buffer               = nullptr;
        detail::access_dispatch_kind_t last_sample_access_dispatch_kind =
            detail::access_dispatch_kind_t::NONE;
#endif
        std::vector<gpu_sample_t>      line_window_staging;
        std::vector<line_draw_span_t>  line_draw_spans;
        bool                           line_window_geometry_dirty = true;
        std::vector<std::uint64_t>     stack_cache_key;
        data_snapshot_t                stack_cache_snapshot;
        bool                           stack_cache_resampled      = false;
        bool                           last_stack_view_suppressed = false;

        // Per-view RHI resources. Defined out-of-line in series_renderer.cpp
        // where QRhiBuffer is complete; the public header only sees the
        // forward declaration. unique_ptr-of-incomplete-type forces every
        // special member to be defined out-of-line, hence the explicit
        // declarations below.
        struct rhi_buffers_t;
        std::unique_ptr<rhi_buffers_t> rhi;

        std::size_t    rhi_vbo_capacity_bytes             = 0;
        std::size_t    rhi_line_window_vbo_capacity_bytes = 0;

        vbo_view_state_t();
        ~vbo_view_state_t();
        vbo_view_state_t(const vbo_view_state_t&)            = delete;
        vbo_view_state_t& operator=(const vbo_view_state_t&) = delete;
        vbo_view_state_t(vbo_view_state_t&&) noexcept;
        vbo_view_state_t& operator=(vbo_view_state_t&&) noexcept;

        void reset();
    };

    struct vbo_state_t
    {
        int                stack_group = 0;
        vbo_view_state_t   main_view;
        vbo_view_state_t   preview_view;
        std::unique_ptr<detail::Series_window_snapshot_cache>
                           snapshot_cache;

        vbo_state_t();
        ~vbo_state_t();
        vbo_state_t(const vbo_state_t&)            = delete;
        vbo_state_t& operator=(const vbo_state_t&) = delete;
        vbo_state_t(vbo_state_t&&) noexcept;
        vbo_state_t& operator=(vbo_state_t&&) noexcept;
    };

    // Per-series planning scratch used within prepare(). Prepared draw commands
    // retain their series and snapshot owners through the matching render().
    struct series_draw_state_t
    {
        int                id                               = 0;
        std::size_t        series_order                     = 0;
        std::shared_ptr<const series_data_t>
                           series;
        vbo_state_t*       vbo_state                        = nullptr;
        Series_view_plan   main_plan;
        Series_view_plan   preview_plan;
        bool               has_preview                      = false;
        bool               main_reuses_uploaded_geometry    = false;
        bool               preview_reuses_uploaded_geometry = false;
        bool               main_stack_sum_overlay           = false;
        bool               preview_stack_sum_overlay        = false;
    };

    Asset_loader*                                                      m_asset_loader = nullptr;
    std::unordered_map<int, vbo_state_t>                               m_vbo_states;
    // Consolidated once-per-series error log deduplication.
    // Key encodes (series_id, error_category) as uint64_t.
    std::unordered_set<uint64_t>                                       m_logged_errors;
    std::map<int, std::vector<detail::Rendered_stack_source>>
                                                                      m_main_stack_validity;
    std::map<std::pair<int, Series_view_kind>, detail::Rendered_stack_status>
                                                                      m_stack_view_statuses;
    // Private test instrumentation for the QRhi prepare/render split.
#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
    std::vector<int>                                                   m_last_recorded_draw_z_orders;
    std::vector<Display_style>                                         m_last_recorded_draw_styles;
    std::vector<int>                                                   m_last_recorded_draw_series_ids;
    std::vector<Series_view_kind>                                      m_last_recorded_draw_view_kinds;
    std::vector<bool>                                                  m_last_recorded_stack_sum_overlays;
    std::vector<glm::vec4>                                             m_last_recorded_draw_colors;
    std::vector<float>                                                 m_last_recorded_line_widths;
    std::size_t                                                        m_last_qrhi_layer_cache_size = 0;
#endif

    // The full implementation sits in series_renderer.cpp where the QRhi
    // types are complete.
    struct rhi_state_t;
    std::unique_ptr<rhi_state_t>                                       m_rhi_state;

    uint64_t m_frame_id = 0; // Monotonic frame counter for snapshot caching

    void clear_frame_snapshot_caches();

    // rhi_prepare_series_view_samples: writes the compact sample VBO for one
    //   planned series/view window. Built-in AREA/LINE/DOTS primitives share
    //   this upload.
    //
    // rhi_prepare_series_primitive: writes to ctx.rhi_updates only. Builds the
    //   per-primitive UBO(s) and (LINE-only) the per-frame line_window_vbo, and
    //   ensures the cached pipeline / SRB are valid. No cb->* draw calls. Must
    //   run before the host calls beginPass(batch) so the upload is submitted
    //   alongside the rest of ctx.rhi_updates.
    //
    // rhi_record_series_primitive: emits cb->setGraphicsPipeline /
    //   setShaderResources / setVertexInput / draw only. Scissor is owned by
    //   the outer layer replay loop. No buffer writes; safe to call inside the
    //   open render pass.
    bool rhi_prepare_series_view_samples(
        const frame_context_t& ctx,
        vbo_view_state_t&      view_state,
        const sample_window_t& window);

    bool rhi_prepare_series_primitive(
        const frame_context_t& ctx,
        Display_style          primitive_style,
        vbo_view_state_t&      view_state,
        const sample_window_t& window,
        const glm::vec4&       draw_color,
        float                  line_width_px,
        float                  point_diameter_px,
        bool                   stack_sum_overlay,
        std::vector<detail::builtin_segment_span_t>*
                               out_segment_spans = nullptr);

    void rhi_record_series_primitive(
        const frame_context_t& ctx,
        Display_style          primitive_style,
        vbo_view_state_t&      view_state,
        const sample_window_t& window,
        bool                   stack_sum_overlay,
        const std::vector<detail::builtin_segment_span_t>&
                               segment_spans);
};

} // namespace vnm::plot
