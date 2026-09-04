#include <vnm_plot/rhi/series_renderer.h>
#include <vnm_plot/core/algo.h>
#include <vnm_plot/rhi/asset_loader.h>
#include <vnm_plot/core/constants.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/series_window.h>
#include <vnm_plot/core/time_units.h>
#include <vnm_plot/rhi/qrhi_series_layer.h>
#include <vnm_plot/rhi/series_data.h>
#include "rhi_helpers.h"
#include "series_window_planner.h"

#include <glm/gtc/type_ptr.hpp>
#include <rhi/qrhi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace vnm::plot {
using detail::choose_origin_ns;
using detail::compute_lod_scales;
using detail::k_scissor_pad_px;
using detail::positive_span_ns_for_signed_api;

namespace {

using detail::load_qsb;

constexpr glm::vec4 k_default_series_color(
    0.16f,
    0.45f,
    0.64f,
    1.0f);

constexpr glm::vec4 k_default_series_color_dark(
    0.30f,
    0.63f,
    0.88f,
    1.0f);

constexpr float         k_default_color_epsilon     = 0.01f;
constexpr int           k_stack_sum_z_order         = 20;
constexpr std::uint64_t k_stack_grid_policy_version = 1;

const char* stack_rejection_reason_text(Stack_rejection_reason reason)
{
    switch (reason) {
        case Stack_rejection_reason::NONE:                    return "none";
        case Stack_rejection_reason::MIXED_INTERPOLATION:     return "mixed interpolation";
        case Stack_rejection_reason::NO_DRAWABLE_DATA:        return "no drawable data";
        case Stack_rejection_reason::INCOMPATIBLE_DATA:       return "incompatible data";
        case Stack_rejection_reason::NONMONOTONIC_TIMESTAMPS: return "nonmonotonic timestamps";
        case Stack_rejection_reason::NO_COMMON_DOMAIN:        return "no common time domain";
        case Stack_rejection_reason::CUMULATIVE_OVERFLOW:     return "cumulative overflow";
        case Stack_rejection_reason::OUTPUT_LIMIT:            return "output limit";
    }
    return "unknown";
}

std::vector<std::size_t> source_lod_scales(const Data_source& source)
{
    const std::size_t        level_count = source.lod_levels();
    std::vector<std::size_t> scales      = source.lod_scales();
    if (scales.size() != level_count) {
        scales = compute_lod_scales(source);
    }
    for (std::size_t& scale : scales) {
        scale = std::max<std::size_t>(1, scale);
    }
    return scales;
}

bool is_default_series_color(const glm::vec4& color)
{
    return glm::all(glm::lessThan(
        glm::abs(color - k_default_series_color),
        glm::vec4(k_default_color_epsilon)));
}

glm::vec4 builtin_draw_color(
    const frame_context_t& ctx,
    const glm::vec4&       series_color,
    Display_style          primitive_style,
    const sample_window_t& window,
    float                  area_fill_alpha)
{
    glm::vec4 color = series_color;
    if (primitive_style == Display_style::AREA) {
        const bool use_dark_default_color =
            ctx.dark_mode && is_default_series_color(color);
        color.w *= area_fill_alpha;
        if (use_dark_default_color) {
            color = k_default_series_color_dark;
            color.w *= area_fill_alpha;
        }
    }
    color.w *= window.window_alpha;
    return color;
}

bool line_window_sample_count(
    std::size_t            source_count,
    Series_interpolation   interpolation,
    std::size_t&           out_count)
{
    out_count = 0;
    if (source_count == 0) {
        return true;
    }
    if (interpolation == Series_interpolation::STEP_AFTER) {
        std::size_t doubled = 0;
        if (!detail::checked_size_product(source_count, 2u, doubled)) {
            return false;
        }
        out_count = doubled - 1u;
        return true;
    }
    out_count = source_count;
    return true;
}

int builtin_primitive_z_order(Display_style primitive_style)
{
    switch (primitive_style) {
        case Display_style::AREA: return -10;
        case Display_style::DOTS: return  10;
        case Display_style::LINE:
        default:                  return   0;
    }
}

int view_band_order(Series_view_kind view_kind)
{
    switch (view_kind) {
        case Series_view_kind::MAIN:    return 0;
        case Series_view_kind::PREVIEW: return 1;
    }
    return 1;
}

void hash_combine(std::size_t& seed, std::size_t value) noexcept
{
    seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
}

std::size_t hash_drawable_spans(
    const std::vector<drawable_sample_span_t>& spans) noexcept
{
    std::size_t seed = spans.size();
    for (const drawable_sample_span_t& span : spans) {
        hash_combine(seed, span.source_first);
        hash_combine(seed, span.source_count);
        hash_combine(seed, span.gpu_first);
        hash_combine(seed, span.gpu_count);
    }
    return seed;
}

// Defined in the public header so prepared_draw_command_t can carry it across
// the prepare/record boundary; aliased here for the existing local references.
using detail::builtin_segment_span_t;

std::vector<builtin_segment_span_t> builtin_segment_spans(
    const sample_window_t& window)
{
    std::vector<builtin_segment_span_t> spans;
    if (window.gpu_count < 2) {
        return spans;
    }

    if (window.nonfinite_policy == Nonfinite_sample_policy::SKIP) {
        spans.push_back({0, window.gpu_count});
        return spans;
    }

    spans.reserve(window.drawable_spans.size());
    for (const drawable_sample_span_t& span : window.drawable_spans) {
        if (span.gpu_count >= 2) {
            spans.push_back({span.gpu_first, span.gpu_count});
        }
    }
    return spans;
}

bool has_builtin_segment_span(const sample_window_t& window)
{
    if (window.gpu_count        <  2)                             { return false; }
    if (window.nonfinite_policy == Nonfinite_sample_policy::SKIP) { return true;  }
    return std::any_of(
        window.drawable_spans.begin(),
        window.drawable_spans.end(),
        [](const drawable_sample_span_t& span) {
            return span.gpu_count >= 2;
        });
}

bool is_builtin_primitive_drawable(
    Display_style          primitive_style,
    const sample_window_t& window)
{
    if (window.gpu_count == 0)                   { return false; }
    if (primitive_style  == Display_style::DOTS) { return true;  }
    return has_builtin_segment_span(window);
}

std::size_t hash_access_policy_cache_key(
    const detail::access_policy_cache_key_t& key) noexcept
{
    std::size_t h = std::hash<const Data_access_policy*>{}(key.identity);
    hash_combine(h, std::hash<std::uint64_t>{}(key.layout_key));
    hash_combine(h, std::hash<std::uint64_t>{}(key.revision));
    hash_combine(h, std::hash<int>{}(static_cast<int>(key.dispatch_kind)));
    hash_combine(h, std::hash<bool>{}(key.has_timestamp));
    hash_combine(h, std::hash<bool>{}(key.has_value));
    hash_combine(h, std::hash<bool>{}(key.has_range));
    return h;
}

} // anonymous namespace

static_assert(offsetof(series_view_uniform_std140_t, pmv)      ==   0, "Series_view_t std140 pmv offset");
static_assert(offsetof(series_view_uniform_std140_t, color)    ==  64, "Series_view_t std140 color offset");
static_assert(offsetof(series_view_uniform_std140_t, t_min)    ==  80, "Series_view_t std140 t_min offset");
static_assert(offsetof(series_view_uniform_std140_t, t_max)    ==  84, "Series_view_t std140 t_max offset");
static_assert(offsetof(series_view_uniform_std140_t, v_min)    ==  88, "Series_view_t std140 v_min offset");
static_assert(offsetof(series_view_uniform_std140_t, v_max)    ==  92, "Series_view_t std140 v_max offset");
static_assert(offsetof(series_view_uniform_std140_t, width)    ==  96, "Series_view_t std140 width offset");
static_assert(offsetof(series_view_uniform_std140_t, height)   == 100, "Series_view_t std140 height offset");
static_assert(offsetof(series_view_uniform_std140_t, y_offset) == 104, "Series_view_t std140 y_offset offset");
static_assert(offsetof(series_view_uniform_std140_t, win_h)    == 108, "Series_view_t std140 win_h offset");
static_assert(offsetof(series_view_uniform_std140_t, framebuffer_y_up) == 112,
    "Series_view_t framebuffer_y_up offset");
static_assert(sizeof(series_view_uniform_std140_t) == 128, "Series_view_t std140 size");

series_view_uniform_std140_t make_series_view_uniform(
    const frame_context_t& frame,
    const series_data_t&   series,
    const sample_window_t& window)
{
    series_view_uniform_std140_t uniform{};
    std::memcpy(uniform.pmv, glm::value_ptr(frame.pmv), sizeof(float) * 16);

    glm::vec4 draw_color = series.color;
    draw_color.w     *= window.window_alpha;
    uniform.color[0]  = draw_color.r;
    uniform.color[1]  = draw_color.g;
    uniform.color[2]  = draw_color.b;
    uniform.color[3]  = draw_color.a;

    uniform.t_min    = detail::to_view_seconds(window.t_min_ns, window.t_origin_ns);
    uniform.t_max    = detail::to_view_seconds(window.t_max_ns, window.t_origin_ns);
    uniform.v_min    = window.v_min;
    uniform.v_max    = window.v_max;
    uniform.width    = window.width_px;
    uniform.height   = window.height_px;
    uniform.y_offset = window.y_offset_px;
    uniform.win_h    = static_cast<float>(frame.win_h);
    uniform.framebuffer_y_up =
        (frame.rhi && frame.rhi->isYUpInFramebuffer()) ? 1 : 0;
    return uniform;
}

// Per-view RHI resources held off-line so the public header forward-declares
// the type. The unique_ptr<rhi_buffers_t> in vbo_view_state_t pulls in QRhiBuffer
// transitively through this struct's members.
//
// The SRB is per-view because it captures concrete buffer handles. The
// pipeline state object stays cached in rhi_state_t (its descriptor only
// depends on layout, not on which buffer the SRB references). When a
// buffer is reallocated to grow capacity, the SRB's last_* pointers no
// longer match and the SRB is rebuilt before the next draw.
struct Series_renderer::vbo_view_state_t::rhi_buffers_t
{
    std::unique_ptr<QRhiBuffer>    vbo;
    std::unique_ptr<QRhiBuffer>    ubo;
    // LINE-specific per-frame buffer that holds the active sample window
    // padded with leading and trailing duplicates. Bound four times as a
    // vertex buffer at consecutive gpu_sample_t offsets with per-instance
    // stepping, so the vertex shader receives prev/p0/p1/next as plain
    // attributes. This side-steps the SM 5.0 UAV restriction that blocks
    // SSBOs in the D3D11 vertex stage.
    std::unique_ptr<QRhiBuffer>    line_window_vbo;

    // Per-(view, primitive_style) UBO + SRB cache. Each drawable primitive
    // needs an independent UBO because every resource update is submitted
    // before the render pass starts; combined Display_style values would
    // otherwise overwrite the previously prepared uniform bytes before the
    // matching draw records.
    struct srb_entry_t
    {
        std::unique_ptr<QRhiBuffer>
                       ubo;
        std::unique_ptr<QRhiShaderResourceBindings>
                       srb;
        QRhiBuffer*    last_ubo           = nullptr;
        std::size_t    ubo_capacity_bytes = 0;
    };
    srb_entry_t                    dots_srb;
    srb_entry_t                    line_srb;
    srb_entry_t                    stack_sum_line_srb;
    srb_entry_t                    area_fill_srb;

};

Series_renderer::vbo_view_state_t::vbo_view_state_t()
:
    planner(std::make_unique<detail::series_window_planner_state_t>())
{}
Series_renderer::vbo_view_state_t::~vbo_view_state_t()                           = default;
Series_renderer::vbo_view_state_t::vbo_view_state_t(vbo_view_state_t&&) noexcept = default;
Series_renderer::vbo_view_state_t&
Series_renderer::vbo_view_state_t::operator=(vbo_view_state_t&&) noexcept = default;

void Series_renderer::vbo_view_state_t::reset()
{
    *this = vbo_view_state_t{};
}

Series_renderer::vbo_state_t::vbo_state_t()
:
    snapshot_cache(std::make_unique<detail::Series_window_snapshot_cache>())
{}
Series_renderer::vbo_state_t::~vbo_state_t()                      = default;
Series_renderer::vbo_state_t::vbo_state_t(vbo_state_t&&) noexcept = default;
Series_renderer::vbo_state_t&
Series_renderer::vbo_state_t::operator=(vbo_state_t&&) noexcept = default;

// -----------------------------------------------------------------------------
// RHI state: pipelines, shaders, and the global UBO mirror.
// -----------------------------------------------------------------------------
//
// Each Display_style maps to one cached QRhiGraphicsPipeline. The pipeline
// descriptor only depends on the binding LAYOUT (which stages reference each
// slot, and the vertex-input layout), not on which buffers fill those slots,
// so one pipeline is shared across every series. The QRhiShaderResourceBindings
// that actually reference per-series buffers live on the per-view rhi_buffers_t
// so each series gets its own SRB.

struct Series_renderer::rhi_state_t
{
    enum class pipeline_kind_t : uint32_t
    {
        DOTS = 0,
        LINE = 1,
        AREA = 2
    };

    struct pipeline_key_t
    {
        pipeline_kind_t kind;

        bool operator==(const pipeline_key_t& o) const noexcept
        {
            return kind == o.kind;
        }
    };

    struct pipeline_key_hash_t
    {
        std::size_t operator()(const pipeline_key_t& k) const noexcept
        {
            return static_cast<std::size_t>(k.kind);
        }
    };

    struct rhi_pipeline_t
    {
        std::unique_ptr<QRhiGraphicsPipeline>  pipeline;
        QShader                                vert;
        QShader                                frag;
        // Render-pass descriptor captured at pipeline creation. If the host's
        // current render target carries a different descriptor (e.g. resize
        // recreated the FBO with a different color format or sample count),
        // the cached pipeline is no longer compatible and is rebuilt.
        QRhiRenderPassDescriptor*              last_rpd          = nullptr;
        int                                    last_sample_count = 1;
    };

    struct view_ubo_key_t
    {
        int                series_id = 0;
        Series_view_kind   view_kind = Series_view_kind::MAIN;

        bool operator==(const view_ubo_key_t& o) const noexcept
        {
            return series_id == o.series_id && view_kind == o.view_kind;
        }
    };

    struct view_ubo_key_hash_t
    {
        std::size_t operator()(const view_ubo_key_t& k) const noexcept
        {
            return (static_cast<std::size_t>(k.series_id) << 1)
                ^ static_cast<std::size_t>(k.view_kind);
        }
    };

    struct view_ubo_state_t
    {
        std::unique_ptr<QRhiBuffer> buffer;
        std::uint64_t last_frame_used = 0;
    };

    struct qrhi_layer_program_key_t
    {
        int                series_id      = 0;
        Series_view_kind   view_kind      = Series_view_kind::MAIN;
        std::string        layer_id;
        std::uint64_t      layer_revision = 0;
        const void*        data_identity  = nullptr;
        std::uint64_t      layout_key     = 0;
        detail::access_policy_cache_key_t
                           access_key;
        QRhi*              rhi            = nullptr;

        bool operator==(const qrhi_layer_program_key_t& o) const noexcept
        {
            return
                series_id      == o.series_id      &&
                view_kind      == o.view_kind      &&
                layer_id       == o.layer_id       &&
                layer_revision == o.layer_revision &&
                data_identity  == o.data_identity  &&
                layout_key     == o.layout_key     &&
                access_key     == o.access_key     &&
                rhi            == o.rhi;
        }
    };

    struct qrhi_layer_program_key_hash_t
    {
        std::size_t operator()(const qrhi_layer_program_key_t& k) const noexcept
        {
            std::size_t h = std::hash<int>{}(k.series_id);
            hash_combine(h, std::hash<int>{}(static_cast<int>(k.view_kind)));
            hash_combine(h, std::hash<std::string>{}(k.layer_id));
            hash_combine(h, std::hash<std::uint64_t>{}(k.layer_revision));
            hash_combine(h, std::hash<const void*>{}(k.data_identity));
            hash_combine(h, std::hash<std::uint64_t>{}(k.layout_key));
            hash_combine(h, hash_access_policy_cache_key(k.access_key));
            hash_combine(h, std::hash<QRhi*>{}(k.rhi));
            return h;
        }
    };

    struct qrhi_layer_data_key_t
    {
        std::size_t                lod_level            = 0;
        std::uint64_t              sample_sequence      = 0;
        std::int64_t               t_origin_ns          = 0;
        std::size_t                source_first         = 0;
        std::size_t                source_count         = 0;
        std::size_t                synthetic_hold_count = 0;
        std::size_t                gpu_count            = 0;
        bool                       hold_last_forward    = false;
        std::int64_t               hold_timestamp_ns    = 0;
        Series_interpolation       interpolation        = Series_interpolation::LINEAR;
        Nonfinite_sample_policy nonfinite_policy =
            Nonfinite_sample_policy::BREAK_SEGMENT;
        std::size_t                drawable_span_count = 0;
        std::size_t                drawable_spans_hash = 0;
        detail::access_policy_cache_key_t
                                   access_key;

        bool operator==(const qrhi_layer_data_key_t& o) const noexcept
        {
            const bool frame_fields_match =
                lod_level            == o.lod_level            &&
                sample_sequence      == o.sample_sequence      &&
                t_origin_ns          == o.t_origin_ns          &&
                source_first         == o.source_first         &&
                source_count         == o.source_count         &&
                synthetic_hold_count == o.synthetic_hold_count &&
                gpu_count            == o.gpu_count            &&
                hold_last_forward    == o.hold_last_forward    &&
                hold_timestamp_ns    == o.hold_timestamp_ns    &&
                interpolation        == o.interpolation;

            return
                frame_fields_match                           &&
                nonfinite_policy    == o.nonfinite_policy    &&
                drawable_span_count == o.drawable_span_count &&
                drawable_spans_hash == o.drawable_spans_hash &&
                access_key          == o.access_key;
        }
    };

    struct qrhi_layer_cache_entry_t
    {
        std::unique_ptr<Qrhi_series_layer_state>
                                   state;
        qrhi_layer_data_key_t      data_key;
        bool                       has_data_key    = false;
        std::uint64_t              last_frame_used = 0;
    };

    struct prepared_draw_command_t
    {
        enum class kind_t
        {
            BUILTIN,
            CUSTOM
        };

        kind_t                     kind              = kind_t::CUSTOM;
        int                        z_order           = 0;
        int                        series_id         = 0;
        Series_view_kind           view_kind         = Series_view_kind::MAIN;
        std::size_t                series_order      = 0;
        std::size_t                insertion_order   = 0;
        Qrhi_series_layer_state*   state             = nullptr;
        const series_data_t*       series            = nullptr;
        sample_window_t            window;
        QRhiBuffer*                view_ubo          = nullptr;
        Display_style              primitive_style   = Display_style::LINE;
        vbo_view_state_t*          view_state        = nullptr;
        bool                       stack_sum_overlay = false;
        glm::vec4                  draw_color{};
        float                      line_width_px     = 0.0f;
        // Built-in LINE/AREA strips, computed once in prepare and reused by the
        // record pass instead of recomputing from window. Empty for DOTS.
        std::vector<detail::builtin_segment_span_t>
                                   builtin_segment_spans;
    };

    std::unordered_map<pipeline_key_t, rhi_pipeline_t, pipeline_key_hash_t>    pipelines;
    std::unordered_map<view_ubo_key_t, view_ubo_state_t, view_ubo_key_hash_t>  view_ubos;
    std::unordered_map<
        qrhi_layer_program_key_t,
        qrhi_layer_cache_entry_t,
        qrhi_layer_program_key_hash_t> qrhi_layer_cache;
    std::vector<prepared_draw_command_t>                                       prepared_draws;
    QShader                                                                    cached_dot_vert;
    QShader                                                                    cached_dot_frag;
    QShader                                                                    cached_line_vert;
    QShader                                                                    cached_line_frag;
    QShader                                                                    cached_area_vert;
    QShader                                                                    cached_area_frag;
    bool                                                                       shaders_loaded = false;

    QRhi*                                                                      last_rhi = nullptr;
    QRhiResourceUpdateBatch*                                                   pending_updates = nullptr;

    // Per-frame draw plan computed in prepare() and replayed in render().
    // The vector lives on the renderer rather than in a stack
    // frame because the prepare() / render() split happens across two host
    // calls, with cb->beginPass(batch) sandwiched in between. The fp32
    // origins computed in prepare() ride inside the per-view UBO/staging
    // bytes already submitted to the resource-update batch, so they do
    // not need to be cached here for render() to read back.
    std::vector<series_draw_state_t>                                           frame_draw_states;
    bool                                                                       frame_preview_visible = false;
    // True if prepare() filled this plan. Reset after render() consumes it
    // so a stray render() without a matching prepare() is a no-op.
    bool                                                                       frame_plan_ready = false;
};

// -----------------------------------------------------------------------------
// std140 UBO mirror for the QSB shader uniform_blocks.glsl::Series_view_t,
// plus the per-shader trailing values consumed by plot_dot_quad, plot_line,
// and plot_area.
// -----------------------------------------------------------------------------
//
// The shaders declare:
//
//     layout(std140, binding = 0) uniform Block {
//         Series_view_t view;             // 128 bytes
//         float         line_px;          // for LINE
//         int           snap;             // for LINE
//         float         dot_px;           // for DOTS
//         int           interpolation;    // for AREA
//     } u;
//
// std140 requires:
//   - mat4 aligns to a vec4 (16 bytes)
//   - vec4 aligns to 16 bytes
//   - scalar floats and ints align to 4 bytes
//   - the structure as a whole aligns to a vec4
//
// Series_view_t layout (each row is 16 bytes):
//   offset 0   pmv      (mat4 = 4 vec4 = 64 bytes)
//   offset 64  color    (vec4 = 16)
//   offset 80  t_min    (float)
//   offset 84  t_max    (float)
//   offset 88  v_min    (float)
//   offset 92  v_max    (float)
//   offset 96  width    (float)
//   offset 100 height   (float)
//   offset 104 y_offset (float)
//   offset 108 win_h    (float)
//   offset 112 framebuffer_y_up (int)
//   total: 128 bytes
//
// Per-shader trailing fields sit just past the view, padded out to vec4 to
// satisfy std140's structure-trailing rule. The renderer allocates enough UBO
// bytes for the largest block and uploads the concrete block size needed by
// each primitive.
// Whole-block layout: Series_view + LINE trailing (line_px, snap_to_pixels).
// Padded out to a 16-byte multiple so the host-side struct mirrors the
// vec4-aligned trailing element rule std140 enforces on the GLSL block.
struct Line_block_std140
{
    series_view_uniform_std140_t
           view;           // offset 0
    float  line_px;        // offset 128
    int    snap_to_pixels; // offset 132
    float  pad0;           // offset 136
    float  pad1;           // offset 140
};
static_assert(sizeof(Line_block_std140) == 144, "Line_block_std140 must be a multiple of 16");
static_assert(offsetof(Line_block_std140, line_px)        == 128, "Line_block line_px offset");
static_assert(offsetof(Line_block_std140, snap_to_pixels) == 132, "Line_block snap_to_pixels offset");

// Whole-block layout: Series_view + DOTS trailing (point_diameter_px).
// Padded out to 144 bytes for the same reason as Line_block_std140.
struct Dot_block_std140
{
    series_view_uniform_std140_t
           view;              // offset 0
    float  point_diameter_px; // offset 128
    float  pad0;              // offset 132
    float  pad1;              // offset 136
    float  pad2;              // offset 140
};
static_assert(sizeof(Dot_block_std140) == 144, "Dot_block_std140 must be a multiple of 16");
static_assert(offsetof(Dot_block_std140, point_diameter_px) == 128, "Dot_block point_diameter_px offset");

// Whole-block layout: Series_view + AREA trailing values. Padded out to a
// 16-byte multiple.
struct Area_block_std140
{
    series_view_uniform_std140_t
           view;          // offset 0
    int    interpolation; // offset 128
    int    stacked;       // offset 132
    int    pad1;          // offset 136
    int    pad2;          // offset 140
};
static_assert(sizeof(Area_block_std140) == 144, "Area_block_std140 must be a multiple of 16");
static_assert(offsetof(Area_block_std140, interpolation) == 128, "Area_block interpolation offset");
static_assert(offsetof(Area_block_std140, stacked)       == 132, "Area_block stacked offset");

constexpr std::uint32_t k_series_ubo_bytes = 144;
static_assert(sizeof(Line_block_std140) <= k_series_ubo_bytes, "ubo bytes fit LINE block");
static_assert(sizeof(Dot_block_std140)  <= k_series_ubo_bytes, "ubo bytes fit DOTS block");
static_assert(sizeof(Area_block_std140) == k_series_ubo_bytes, "ubo bytes match AREA block");

Series_renderer::Series_renderer()
:
    m_rhi_state(std::make_unique<rhi_state_t>())
{}

Series_renderer::~Series_renderer() = default;

void Series_renderer::initialize(Asset_loader& asset_loader)
{
    m_asset_loader = &asset_loader;
}

void Series_renderer::cleanup_resources()
{
    clear_frame_snapshot_caches();
    for (auto& [_, state] : m_vbo_states) {
        state.main_view.reset();
        state.preview_view.reset();
    }
    m_vbo_states.clear();
    m_logged_errors.clear();

    m_rhi_state->pipelines.clear();
    for (auto& [key, entry] : m_rhi_state->qrhi_layer_cache) {
        if (entry.state) {
            entry.state->cleanup_qrhi_resources(key.rhi);
        }
    }
    m_rhi_state->qrhi_layer_cache.clear();
    m_rhi_state->view_ubos.clear();
    m_rhi_state->prepared_draws.clear();
    m_rhi_state->shaders_loaded   = false;
    m_rhi_state->cached_dot_vert  = {};
    m_rhi_state->cached_dot_frag  = {};
    m_rhi_state->cached_line_vert = {};
    m_rhi_state->cached_line_frag = {};
    m_rhi_state->cached_area_vert = {};
    m_rhi_state->cached_area_frag = {};
    m_rhi_state->last_rhi         = nullptr;
    m_rhi_state->pending_updates  = nullptr;
    m_rhi_state->frame_draw_states.clear();
    m_rhi_state->prepared_draws.clear();
    m_rhi_state->frame_plan_ready = false;
    m_last_recorded_draw_z_orders.clear();
    m_last_recorded_draw_styles.clear();
    m_last_recorded_draw_series_ids.clear();
    m_last_recorded_draw_view_kinds.clear();
    m_last_recorded_stack_sum_overlays.clear();
    m_last_recorded_draw_colors.clear();
    m_last_recorded_line_widths.clear();
    m_last_qrhi_layer_cache_size = 0;
}

void Series_renderer::clear_frame_snapshot_caches()
{
    for (auto& [_, state] : m_vbo_states) {
        if (state.snapshot_cache) {
            *state.snapshot_cache = detail::Series_window_snapshot_cache{};
        }
    }
}

void Series_renderer::prepare(
    const frame_context_t& ctx,
    const std::map<int, std::shared_ptr<const series_data_t>>&
                           series)
{
    m_main_stack_validity.clear();
    m_stack_view_statuses.clear();
    m_rhi_state->frame_draw_states.clear();
    m_rhi_state->prepared_draws.clear();
    m_rhi_state->frame_plan_ready = false;
    m_last_recorded_draw_z_orders.clear();
    m_last_recorded_draw_styles.clear();
    m_last_recorded_draw_series_ids.clear();
    m_last_recorded_draw_view_kinds.clear();
    m_last_recorded_stack_sum_overlays.clear();
    m_last_recorded_draw_colors.clear();
    m_last_recorded_line_widths.clear();
    m_last_qrhi_layer_cache_size = m_rhi_state->qrhi_layer_cache.size();

    const auto clear_retired_series_resources = [&]() {
        clear_frame_snapshot_caches();
        m_vbo_states.clear();
        for (auto& [key, entry] : m_rhi_state->qrhi_layer_cache) {
            if (entry.state) {
                entry.state->cleanup_qrhi_resources(key.rhi);
            }
        }
        m_rhi_state->qrhi_layer_cache.clear();
        m_rhi_state->view_ubos.clear();
        m_rhi_state->prepared_draws.clear();
        m_last_qrhi_layer_cache_size = 0;
    };

    if (series.empty()) {
        clear_retired_series_resources();
        return;
    }
    if (!m_asset_loader) {
        return;
    }

    const auto& layout = ctx.layout;
    if (layout.usable_width <= 0.0 || layout.usable_height <= 0.0) {
        return;
    }

    ++m_frame_id;

    vnm::plot::Profiler* profiler = ctx.config ? ctx.config->profiler.get() : nullptr;
    VNM_PLOT_PROFILE_SCOPE(profiler,
        "renderer.frame.execute_passes.render_data_series.prepare");

    QRhi* rhi = ctx.rhi;
    QRhiResourceUpdateBatch* rhi_updates = ctx.rhi_updates;
    if (m_rhi_state->last_rhi != rhi) {
        for (auto& [key, entry] : m_rhi_state->qrhi_layer_cache) {
            if (entry.state) {
                entry.state->cleanup_qrhi_resources(key.rhi);
            }
        }
        m_rhi_state->qrhi_layer_cache.clear();
        m_rhi_state->view_ubos.clear();
        m_rhi_state->pipelines.clear();
        m_rhi_state->prepared_draws.clear();
        for (auto& [_, state] : m_vbo_states) {
            state.main_view.reset();
            state.preview_view.reset();
            if (state.snapshot_cache) {
                *state.snapshot_cache = detail::Series_window_snapshot_cache{};
            }
        }
        m_last_qrhi_layer_cache_size = 0;
    }
    m_rhi_state->last_rhi        = rhi;
    m_rhi_state->pending_updates = rhi_updates;

    for (auto it = m_vbo_states.begin(); it != m_vbo_states.end(); ) {
        if (series.find(it->first) == series.end()) {
            it = m_vbo_states.erase(it);
        }
        else {
            ++it;
        }
    }

    auto& draw_states = m_rhi_state->frame_draw_states;
    draw_states.reserve(series.size());

    const double preview_visibility = ctx.config ? ctx.config->preview_visibility : 1.0;
    const bool   preview_visible    = ctx.adjusted_preview_height > 0.0 && preview_visibility > 0.0;
    m_rhi_state->frame_preview_visible = preview_visible;

    const std::int64_t main_span_ns = positive_span_ns_for_signed_api(ctx.t0, ctx.t1);
    const std::int64_t preview_span_ns = positive_span_ns_for_signed_api(
        ctx.t_available_min,
        ctx.t_available_max);
    const std::int64_t main_origin_ns = choose_origin_ns(ctx.t0, main_span_ns);
    const std::int64_t preview_origin_ns = preview_visible
        ? choose_origin_ns(ctx.t_available_min, preview_span_ns)
        : main_origin_ns;

    enum class Error_cat : uint32_t {
        PREVIEW_MISSING_SOURCE,
        MISSING_SHADER,
        STACK_COMPOSITION
    };

    const auto log_error_once = [&](Error_cat cat, int series_id,
        const std::string& message) {
        if (!ctx.config || !ctx.config->log_error) {
            return;
        }
        const uint64_t key = (static_cast<uint64_t>(cat) << 32)
            | static_cast<uint64_t>(static_cast<uint32_t>(series_id));
        if (m_logged_errors.insert(key).second) {
            ctx.config->log_error(message);
        }
    };

    std::size_t next_series_order = 0;
    for (const auto& [id, s] : series) {
        VNM_PLOT_PROFILE_SCOPE(profiler,
            "renderer.frame.execute_passes.render_data_series.series");
        if (!s || !s->enabled) {
            continue;
        }

        Data_source* main_source = s->main_source();
        if (!main_source) {
            continue;
        }

        const Data_access_policy& main_access = s->main_access();

        Display_style        main_style         = s->style;
        Series_interpolation main_interpolation = s->interpolation;
        const auto&          qrhi_layers        = qrhi_layers_for(*s);
        const auto has_layer_for_view = [&](Series_view_kind view_kind) {
            return std::any_of(
                qrhi_layers.begin(),
                qrhi_layers.end(),
                [view_kind](const auto& layer) {
                    return layer && layer->draws_view(view_kind);
                });
        };
        const bool has_main_layer = has_layer_for_view(Series_view_kind::MAIN);
        if (!main_style && !has_main_layer) {
            continue;
        }

        const bool                has_preview_config    = s->has_preview_config();
        Data_source*              preview_source        = nullptr;
        const Data_access_policy* preview_access        = nullptr;
        Display_style             preview_style         = Display_style::NONE;
        Series_interpolation      preview_interpolation = Series_interpolation::LINEAR;
        bool                      preview_matches_main  = false;

        bool preview_valid     = false;
        bool has_preview_layer = false;

        if (preview_visible) {
            preview_source        = s->preview_source();
            preview_access        = &s->preview_access();
            preview_style         = s->effective_preview_style();
            preview_interpolation = s->effective_preview_interpolation();
            preview_matches_main  = s->preview_matches_main();
            has_preview_layer     = has_layer_for_view(Series_view_kind::PREVIEW);

            if (has_preview_config && !preview_source) {
                log_error_once(Error_cat::PREVIEW_MISSING_SOURCE, id,
                    "Preview config set but preview data_source is null (series "
                        + std::to_string(id) + ")");
                preview_style = Display_style::NONE;
            }

            if (preview_source &&
                preview_access &&
                (!!preview_style || has_preview_layer))
            {
                preview_valid = true;
            }
            else {
                preview_source = nullptr;
            }
        }

        auto& vbo_state = m_vbo_states[id];
        if (vbo_state.stack_group != s->stack_group) {
            vbo_state = vbo_state_t{};
            vbo_state.stack_group = s->stack_group;
        }

        std::vector<std::size_t> main_scales = source_lod_scales(*main_source);
        std::vector<std::size_t> preview_scales;
        if (preview_valid) {
            if (preview_matches_main) {
                preview_scales = main_scales;
            }
            else {
                preview_scales = source_lod_scales(*preview_source);
            }
        }

        const auto plan_series_view =
            [&](Series_view_kind view_kind,
                vbo_view_state_t& view_state,
                Data_source& data_source,
                const Data_access_policy& access,
                const std::vector<std::size_t>& scales,
                std::int64_t t_min_ns,
                std::int64_t t_max_ns,
                std::int64_t t_origin_ns,
                double width_px,
                Display_style style,
                Series_interpolation interpolation,
                detail::Snapshot_requirement snapshot_requirement)
        {
            detail::series_window_plan_request_t request;
            request.series_id             = id;
            request.view_kind             = view_kind;
            request.planner_state         = view_state.planner.get();
            request.snapshot_cache        = vbo_state.snapshot_cache.get();
            request.frame_id              = m_frame_id;
            request.data_source           = &data_source;
            request.access                = &access;
            request.scales                = &scales;
            request.t_min_ns              = t_min_ns;
            request.t_max_ns              = t_max_ns;
            request.t_origin_ns           = t_origin_ns;
            request.width_px              = width_px;
            request.empty_window_behavior = s->empty_window_behavior;
            request.nonfinite_policy      = s->nonfinite_policy;
            request.style                 = style;
            request.interpolation         = interpolation;
            request.snapshot_requirement  = snapshot_requirement;
            request.has_uploaded_vbo      = view_state.has_uploaded_vbo;
            request.profiler              = profiler;
            return detail::plan_series_window(request);
        };

        const std::size_t prev_lod_level = vbo_state.main_view.planner
            ? vbo_state.main_view.planner->last_lod_level
            : 0;
        auto main_plan = plan_series_view(
            Series_view_kind::MAIN,
            vbo_state.main_view,
            *main_source,
            main_access,
            main_scales,
            ctx.t0,
            ctx.t1,
            main_origin_ns,
            layout.usable_width,
            main_style,
            main_interpolation,
            (has_main_layer || s->stack_group != 0)
                ? detail::Snapshot_requirement::Frame_snapshot_required
                : detail::Snapshot_requirement::Optional);
        const bool main_reuses_uploaded_geometry =
            vbo_state.main_view.planner->last_plan_reused_upload;
        main_plan.v_min        = ctx.v0;
        main_plan.v_max        = ctx.v1;
        main_plan.height_px    = static_cast<float>(layout.usable_height);
        main_plan.y_offset_px  = 0.0f;
        main_plan.window_alpha = 1.0f;
        if (ctx.config && ctx.config->log_debug_info &&
            main_plan.gpu_count >  0 &&
            main_plan.lod_level != prev_lod_level)
        {
            std::string message = "LOD selection: series=" + std::to_string(id) +
                " level=" + std::to_string(main_plan.lod_level) +
                " pps=" + std::to_string(main_plan.pixels_per_sample);
            ctx.config->log_debug_info(message);
        }

        Series_view_plan preview_plan;
        bool             preview_reuses_uploaded_geometry = false;
        preview_plan.series_id             = id;
        preview_plan.view_kind             = Series_view_kind::PREVIEW;
        preview_plan.source                = preview_source;
        preview_plan.access                = preview_access;
        preview_plan.empty_window_behavior = s->empty_window_behavior;
        preview_plan.nonfinite_policy      = s->nonfinite_policy;
        preview_plan.style                 = preview_style;
        preview_plan.interpolation         = preview_interpolation;
        if (preview_visible && preview_valid) {
            preview_plan = plan_series_view(
                Series_view_kind::PREVIEW,
                vbo_state.preview_view,
                *preview_source,
                *preview_access,
                preview_scales,
                ctx.t_available_min,
                ctx.t_available_max,
                preview_origin_ns,
                ctx.win_w,
                preview_style,
                preview_interpolation,
                (has_preview_layer || s->stack_group != 0)
                    ? detail::Snapshot_requirement::Frame_snapshot_required
                    : detail::Snapshot_requirement::Optional);
            preview_reuses_uploaded_geometry =
                vbo_state.preview_view.planner->last_plan_reused_upload;
            const double preview_top =
                double(ctx.win_h) - ctx.adjusted_preview_height;
            preview_plan.v_min        = ctx.preview_v0;
            preview_plan.v_max        = ctx.preview_v1;
            preview_plan.height_px    = static_cast<float>(ctx.adjusted_preview_height);
            preview_plan.y_offset_px  = static_cast<float>(preview_top);
            preview_plan.window_alpha = static_cast<float>(preview_visibility);
        }

        series_draw_state_t draw_state;
        draw_state.id           = id;
        draw_state.series_order = next_series_order++;
        draw_state.series       = s;
        draw_state.vbo_state    = &vbo_state;
        draw_state.main_plan    = std::move(main_plan);
        draw_state.preview_plan = std::move(preview_plan);
        draw_state.has_preview  = preview_visible && preview_valid;
        draw_state.main_reuses_uploaded_geometry =
            main_reuses_uploaded_geometry;
        draw_state.preview_reuses_uploaded_geometry =
            preview_reuses_uploaded_geometry;
        draw_states.push_back(std::move(draw_state));
    }

    const auto compose_stacks = [&](Series_view_kind view_kind) {
        VNM_PLOT_PROFILE_SCOPE(profiler, "renderer.frame.compose_stacks");
        const auto view_state_for = [view_kind](series_draw_state_t& member)
            -> vbo_view_state_t& {
            return view_kind == Series_view_kind::MAIN
                ? member.vbo_state->main_view
                : member.vbo_state->preview_view;
        };
        const auto clear_stack_cache = [&](series_draw_state_t& member) {
            auto& state = view_state_for(member);
            state.stack_cache_key.clear();
            state.stack_cache_snapshot  = {};
            state.stack_cache_resampled = false;
        };
        const auto set_reuses_uploaded_geometry =
            [view_kind](series_draw_state_t& member, bool value) {
                if (view_kind == Series_view_kind::MAIN) {
                    member.main_reuses_uploaded_geometry = value;
                }
                else {
                    member.preview_reuses_uploaded_geometry = value;
                }
            };
        for (auto& draw_state : draw_states) {
            if (draw_state.vbo_state) {
                view_state_for(draw_state).last_stack_view_suppressed = false;
            }
        }
        std::map<int, std::vector<series_draw_state_t*>> groups;
        for (auto& draw_state : draw_states) {
            if (!draw_state.series || draw_state.series->stack_group == 0) {
                continue;
            }
            if (view_kind == Series_view_kind::PREVIEW && !draw_state.has_preview) {
                clear_stack_cache(draw_state);
                continue;
            }
            groups[draw_state.series->stack_group].push_back(&draw_state);
        }

        for (auto& [group, members] : groups) {
            auto& published = m_stack_view_statuses[{group, view_kind}];
            published.status.group     = group;
            published.status.view_kind = view_kind;
            published.status.state     = Stack_view_state::ACTIVE;
            published.status.reason    = Stack_rejection_reason::NONE;
            published.status.affected_series_ids.reserve(members.size());
            published.sources.reserve(members.size());
            std::vector<const Series_view_plan*> plans;
            plans.reserve(members.size());
            std::vector<Time_order> selected_time_orders;
            selected_time_orders.reserve(members.size());
            std::size_t input_samples = 0;
            std::vector<std::uint64_t> cache_key;
            cache_key.reserve(members.size() * 22u + 2u);
            bool cacheable = true;
            for (auto* member : members) {
                const Series_view_plan* plan = view_kind == Series_view_kind::MAIN
                    ? &member->main_plan
                    : &member->preview_plan;
                plans.push_back(plan);
                const auto& view_state = view_state_for(*member);
                selected_time_orders.push_back(view_state.planner
                    ? view_state.planner->last_selected_time_order
                    : Time_order::UNKNOWN);
                published.status.affected_series_ids.push_back(member->id);
                const std::uint64_t attempt_sequence = plan->snapshot.sequence != 0
                    ? plan->snapshot.sequence
                    : (plan->source
                        ? plan->source->current_sequence(plan->lod_level)
                        : 0);
                published.sources.push_back({
                    member->id,
                    plan->source,
                    plan->lod_level,
                    attempt_sequence,
                    plan->interpolation,
                    {}});
                input_samples += plan->gpu_count;
                cacheable = cacheable && plan->snapshot.sequence != 0;
                const auto access_view = plan->access
                    ? detail::make_erased_access_policy_view(*plan->access)
                    : detail::erased_access_policy_t{};
                const auto access_key = detail::make_access_policy_cache_key(
                    plan->access, access_view);
                const auto semantics_key = detail::make_sample_semantics_key(
                    plan->access);
                cache_key.insert(cache_key.end(), {
                    static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                        plan->source ? plan->source->identity() : nullptr)),
                    plan->snapshot.sequence,
                    static_cast<std::uint64_t>(plan->lod_level),
                    static_cast<std::uint64_t>(plan->source_first),
                    static_cast<std::uint64_t>(plan->source_count),
                    static_cast<std::uint64_t>(plan->synthetic_hold_count),
                    static_cast<std::uint64_t>(plan->hold_timestamp_ns),
                    static_cast<std::uint64_t>(plan->t_min_ns),
                    static_cast<std::uint64_t>(plan->t_max_ns),
                    static_cast<std::uint64_t>(plan->t_origin_ns),
                    static_cast<std::uint64_t>(plan->interpolation),
                    static_cast<std::uint64_t>(plan->nonfinite_policy),
                    static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                        access_key.identity)),
                    access_key.layout_key,
                    access_key.revision,
                    static_cast<std::uint64_t>(access_key.dispatch_kind),
                    static_cast<std::uint64_t>(access_key.has_timestamp),
                    static_cast<std::uint64_t>(access_key.has_value),
                    static_cast<std::uint64_t>(access_key.has_range),
                    semantics_key.value,
                    semantics_key.revision,
                    static_cast<std::uint64_t>(semantics_key.conservative)
                });
            }

            if (members.size() < 2) {
                clear_stack_cache(*members.front());
                continue;
            }

            const std::size_t timestamp_budget = detail::stack_timestamp_budget(
                plans.front()->width_px,
                plans.size());
            cache_key.push_back(k_stack_grid_policy_version);
            cache_key.push_back(static_cast<std::uint64_t>(timestamp_budget));

            const bool cache_hit = cacheable && std::all_of(
                members.begin(),
                members.end(),
                [&](auto* member) {
                    const auto& state = view_state_for(*member);
                    return
                        state.stack_cache_key == cache_key &&
                        state.stack_cache_snapshot;
                });
            std::vector<std::vector<detail::stacked_sample_t>> layers;
            detail::stack_composition_stats_t composition_stats;
            const Stack_rejection_reason rejection = cache_hit
                ? Stack_rejection_reason::NONE
                : detail::compose_stacked_series(
                    plans,
                    selected_time_orders,
                    layers,
                    timestamp_budget,
                    &composition_stats);
            if (cache_hit) {
                const auto& cached = view_state_for(*members.front());
                composition_stats.timestamp_count = cached.stack_cache_snapshot.count;
                composition_stats.resampled       = cached.stack_cache_resampled;
            }
            if (rejection != Stack_rejection_reason::NONE) {
                published.status.state  = Stack_view_state::SUPPRESSED;
                published.status.reason = rejection;
                for (auto* member : members) {
                    clear_stack_cache(*member);
                    Series_view_plan& plan = view_kind == Series_view_kind::MAIN
                        ? member->main_plan
                        : member->preview_plan;
                    plan.snapshot             = {};
                    plan.source_first         = 0;
                    plan.source_count         = 0;
                    plan.synthetic_hold_count = 0;
                    plan.gpu_count            = 0;
                    plan.drawable_spans.clear();
                    plan.hold_last_forward    = false;
                    view_state_for(*member).last_stack_view_suppressed = true;
                }
                log_error_once(
                    Error_cat::STACK_COMPOSITION,
                    members.front()->id,
                    "Stack composition suppressed: " +
                    std::string(stack_rejection_reason_text(rejection)) +
                    " (group " + std::to_string(group) + ")");
                if (profiler && rejection == Stack_rejection_reason::OUTPUT_LIMIT) {
                    profiler->record_counter("renderer.stacking.output_limit_count");
                }
                continue;
            }
            std::size_t output_samples = 0;
            std::vector<stack_source_revision_t>* main_validity = nullptr;
            if (view_kind == Series_view_kind::MAIN) {
                auto& validity = m_main_stack_validity[group];
                validity.reserve(plans.size());
                for (std::size_t i = 0; i < plans.size(); ++i) {
                    const Series_view_plan* plan = plans[i];
                    validity.push_back({
                        members[i]->id,
                        plan->source,
                        plan->lod_level,
                        plan->snapshot.sequence,
                        plan->interpolation,
                        {}});
                }
                main_validity = &validity;
            }
            for (std::size_t i = 0; i < members.size(); ++i) {
                Series_view_plan& plan = view_kind == Series_view_kind::MAIN
                    ? members[i]->main_plan
                    : members[i]->preview_plan;
                auto& view_state = view_state_for(*members[i]);
                if (!cache_hit) {
                    auto samples = std::make_shared<std::vector<detail::stacked_sample_t>>(
                        std::move(layers[i]));
                    view_state.stack_cache_snapshot  = {
                        samples->data(), samples->size(),
                        sizeof(detail::stacked_sample_t),
                        m_frame_id,
                        nullptr, 0, samples};
                    view_state.stack_cache_key       = cache_key;
                    view_state.stack_cache_resampled = composition_stats.resampled;
                }
                set_reuses_uploaded_geometry(
                    *members[i],
                    cache_hit && view_state.has_uploaded_vbo);
                const data_snapshot_t snapshot = view_state.stack_cache_snapshot;
                if (main_validity) {
                    (*main_validity)[i].cumulative = snapshot;
                }
                const std::size_t     count = snapshot.count;
                output_samples            += count;
                plan.access                = &detail::stacked_sample_access();
                plan.snapshot.snapshot     = snapshot;
                plan.snapshot.sequence     = snapshot.sequence;
                plan.source_first          = 0;
                plan.source_count          = count;
                plan.synthetic_hold_count  = 0;
                plan.gpu_count             = count;
                plan.drawable_spans        = {{0, count, 0, count}};
                plan.hold_last_forward     = false;
                plan.stacked               = true;

                const bool has_custom_layer = std::any_of(
                    qrhi_layers_for(*members[i]->series).begin(),
                    qrhi_layers_for(*members[i]->series).end(),
                    [view_kind](const auto& layer) {
                        return layer && layer->draws_view(view_kind);
                    });
                if (cache_hit && view_state.has_uploaded_vbo && !has_custom_layer) {
                    plan.snapshot.snapshot = {};
                }
            }
            if (view_kind == Series_view_kind::MAIN) {
                members.back()->main_stack_sum_overlay = true;
            }
            else {
                members.back()->preview_stack_sum_overlay = true;
            }
            if (profiler) {
                profiler->record_observation(
                    "renderer.stacking.input_sample_count",
                    static_cast<double>(input_samples));
                profiler->record_observation(
                    "renderer.stacking.output_sample_count",
                    static_cast<double>(output_samples));
                profiler->record_observation(
                    "renderer.stacking.grid_timestamp_count",
                    static_cast<double>(composition_stats.timestamp_count));
                profiler->record_observation(
                    "renderer.stacking.source_read_count",
                    static_cast<double>(composition_stats.source_read_count));
                profiler->record_counter(composition_stats.resampled
                    ? "renderer.stacking.resampled_group_count"
                    : "renderer.stacking.exact_group_count");
                if (cache_hit) {
                    profiler->record_counter("renderer.stacking.cache_hit_count");
                }
            }
        }
    };

    compose_stacks(Series_view_kind::MAIN);
    if (preview_visible) {
        compose_stacks(Series_view_kind::PREVIEW);
    }

    const auto invalidate_view_upload_state = [](vbo_view_state_t& view_state) {
        view_state.has_uploaded_vbo              = false;
        view_state.last_sample_buffer            = nullptr;
        view_state.last_staged_sample_count      = 0;
        view_state.last_sample_upload_bytes      = 0;
        view_state.last_line_window_upload_bytes = 0;
        view_state.last_line_window_upload_count = 0;
        view_state.last_uniform_upload_bytes     = 0;
        view_state.last_uniform_upload_count     = 0;
        view_state.line_window_geometry_dirty    = true;
        view_state.line_draw_spans.clear();
    };

    if (!rhi || !rhi_updates || !ctx.render_target) {
        for (auto& draw_state : draw_states) {
            if (!draw_state.vbo_state) {
                continue;
            }
            invalidate_view_upload_state(draw_state.vbo_state->main_view);
            if (draw_state.has_preview) {
                invalidate_view_upload_state(
                    draw_state.vbo_state->preview_view);
            }
        }
        m_rhi_state->frame_plan_ready = false;
        return;
    }

    const auto make_window = [](const Series_view_plan& plan) {
        sample_window_t window;
        window.view_kind            = plan.view_kind;
        window.snapshot             = plan.snapshot.snapshot;
        window.access               = plan.access;
        window.source_first         = plan.source_first;
        window.source_count         = plan.source_count;
        window.synthetic_hold_count = plan.synthetic_hold_count;
        window.gpu_count            = plan.gpu_count;
        window.drawable_spans       = plan.drawable_spans;
        window.lod_level            = plan.lod_level;
        window.pixels_per_sample    = plan.pixels_per_sample;
        window.sample_sequence      = plan.snapshot.sequence;
        window.interpolation        = plan.interpolation;
        window.stacked              = plan.stacked;
        window.nonfinite_policy     = plan.nonfinite_policy;
        window.t_min_ns             = plan.t_min_ns;
        window.t_max_ns             = plan.t_max_ns;
        window.t_origin_ns          = plan.t_origin_ns;
        window.hold_last_forward    = plan.hold_last_forward;
        window.hold_timestamp_ns    = plan.hold_timestamp_ns;
        window.v_min                = plan.v_min;
        window.v_max                = plan.v_max;
        window.width_px             = plan.width_px;
        window.height_px            = plan.height_px;
        window.y_offset_px          = plan.y_offset_px;
        window.window_alpha         = plan.window_alpha;
        return window;
    };

    const auto make_sample_buffer =
        [](const vbo_view_state_t& view_state,
            const sample_window_t& window) -> qrhi_series_sample_buffer_t
    {
        qrhi_series_sample_buffer_t sample_buffer;
        if (!view_state.has_uploaded_vbo ||
            !view_state.rhi ||
            !view_state.rhi->vbo ||
            window.gpu_count == 0)
        {
            return sample_buffer;
        }

        sample_buffer.buffer               = view_state.rhi->vbo.get();
        sample_buffer.first_sample         = 0;
        sample_buffer.sample_count         = window.gpu_count;
        sample_buffer.source_first         = window.source_first;
        sample_buffer.source_count         = window.source_count;
        sample_buffer.synthetic_hold_count = window.synthetic_hold_count;
        sample_buffer.t_origin_ns          = window.t_origin_ns;
        sample_buffer.t_min_ns             = window.t_min_ns;
        sample_buffer.t_max_ns             = window.t_max_ns;
        sample_buffer.v_min                = window.v_min;
        sample_buffer.v_max                = window.v_max;
        sample_buffer.layout.stride_bytes  = sizeof(gpu_sample_t);
        sample_buffer.layout.t_rel_seconds_offset =
            offsetof(gpu_sample_t, t_rel);
        sample_buffer.layout.value_offset     = offsetof(gpu_sample_t, y);
        sample_buffer.layout.range_min_offset = offsetof(gpu_sample_t, y_min);
        sample_buffer.layout.range_max_offset = offsetof(gpu_sample_t, y_max);
        return sample_buffer;
    };

    std::size_t frame_sample_upload_bytes      = 0;
    std::size_t frame_sample_upload_count      = 0;
    std::size_t frame_line_window_upload_bytes = 0;
    std::size_t frame_line_window_upload_count = 0;
    std::size_t frame_uniform_upload_bytes     = 0;
    std::size_t frame_uniform_upload_count     = 0;
    std::size_t next_draw_insertion_order      = 0;

    const auto ensure_view_ubo =
        [&](int series_id,
            Series_view_kind view_kind,
            const series_data_t& series_data,
            const sample_window_t& window) -> QRhiBuffer*
    {
        rhi_state_t::view_ubo_key_t key{series_id, view_kind};
        auto& state = m_rhi_state->view_ubos[key];
        if (!state.buffer) {
            state.buffer.reset(rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                static_cast<quint32>(sizeof(series_view_uniform_std140_t))));
            if (!state.buffer || !state.buffer->create()) {
                state.buffer.reset();
                return nullptr;
            }
            if (profiler) {
                profiler->record_observation(
                    "renderer.frame.gpu_buffer_allocation_bytes",
                    static_cast<double>(sizeof(series_view_uniform_std140_t)));
                profiler->record_counter("renderer.frame.gpu_buffer_allocation_count");
            }
        }

        const series_view_uniform_std140_t uniform =
            make_series_view_uniform(ctx, series_data, window);
        rhi_updates->updateDynamicBuffer(
            state.buffer.get(),
            0,
            sizeof(uniform),
            &uniform);
        frame_uniform_upload_bytes += sizeof(uniform);
        ++frame_uniform_upload_count;
        state.last_frame_used = m_frame_id;
        return state.buffer.get();
    };

    const auto prepare_view_layers =
        [&](series_draw_state_t& draw_state,
            const Series_view_plan& plan,
            vbo_view_state_t& view_state)
    {
        view_state.last_sample_upload_count         = 0;
        view_state.last_line_window_upload_bytes    = 0;
        view_state.last_line_window_upload_count    = 0;
        view_state.last_uniform_upload_bytes        = 0;
        view_state.last_uniform_upload_count        = 0;
        view_state.last_primitive_prepare_count     = 0;
        view_state.last_line_window_sample_count    = 0;
        view_state.last_recorded_line_span_count    = 0;
        view_state.last_recorded_line_segment_count = 0;
        view_state.last_recorded_area_span_count    = 0;
        view_state.last_recorded_area_segment_count = 0;
        view_state.last_recorded_dot_sample_count   = 0;
        view_state.last_sample_access_dispatch_kind =
            detail::access_dispatch_kind_t::NONE;
        view_state.last_sample_buffer = nullptr;

        if (!draw_state.series) {
            return;
        }
        if (plan.gpu_count == 0) {
            invalidate_view_upload_state(view_state);
            return;
        }

        struct planned_draw_t
        {
            std::shared_ptr<const Qrhi_series_layer>
                           layer;
            bool           is_builtin        = false;
            Display_style  primitive_style   = Display_style::NONE;
            int            z_order           = 0;
            bool           stack_sum_overlay = false;
        };

        // Built-in LINE/DOTS/AREA all read the primary value lane. A range-only
        // access policy (timestamp + range, no value) is valid for auto-range
        // and custom range-band layers, but feeding it to a value style would
        // silently render flat at y = 0. Require a value accessor for built-ins;
        // custom layers below still receive the range lane.
        const bool wants_builtin_value_style =
            !!(plan.style & Display_style::AREA) ||
            !!(plan.style & Display_style::LINE) ||
            !!(plan.style & Display_style::DOTS);
        const bool access_has_value = plan.access && plan.access->get_value;
        if (wants_builtin_value_style && !access_has_value) {
            static std::atomic<bool> warned_missing_value{false};
            if (!warned_missing_value.exchange(true)) {
                qWarning("vnm_plot: built-in LINE/DOTS/AREA styles require a "
                    "primary value accessor; a range-only access policy renders "
                    "nothing for these styles. Use a custom range-band layer "
                    "instead. Skipping built-in primitives for such series.");
            }
        }

        std::vector<planned_draw_t> planned_draws;
        if (access_has_value && !!(plan.style & Display_style::AREA)) {
            planned_draws.push_back({
                nullptr,
                true,
                Display_style::AREA,
                builtin_primitive_z_order(Display_style::AREA)});
        }
        if (access_has_value && !!(plan.style & Display_style::LINE)) {
            planned_draws.push_back({
                nullptr,
                true,
                Display_style::LINE,
                builtin_primitive_z_order(Display_style::LINE)});
        }
        if (access_has_value && !!(plan.style & Display_style::DOTS)) {
            planned_draws.push_back({
                nullptr,
                true,
                Display_style::DOTS,
                builtin_primitive_z_order(Display_style::DOTS)});
        }
        const bool stack_sum_overlay = plan.view_kind == Series_view_kind::MAIN
            ? draw_state.main_stack_sum_overlay
            : draw_state.preview_stack_sum_overlay;
        if (access_has_value && stack_sum_overlay) {
            planned_draws.push_back({
                nullptr,
                true,
                Display_style::LINE,
                k_stack_sum_z_order,
                true});
        }
        for (const auto& layer : qrhi_layers_for(*draw_state.series)) {
            if (layer && layer->draws_view(plan.view_kind)) {
                planned_draws.push_back({
                    layer,
                    false,
                    Display_style::NONE,
                    layer->z_order()});
            }
        }
        // Sort by z_order before preparing. The global stable_sort of
        // prepared_draws below independently fixes the final record order, but
        // this per-view sort is still load-bearing: it determines the order in
        // which layers are *prepared*, which is a tested contract (a lower
        // z-order layer must prepare before a higher one). Keep it.
        std::stable_sort(
            planned_draws.begin(),
            planned_draws.end(),
            [](const auto& a, const auto& b) {
                return a.z_order < b.z_order;
            });
        if (planned_draws.empty()) {
            invalidate_view_upload_state(view_state);
            return;
        }

        sample_window_t window = make_window(plan);
        const bool has_drawable_builtin_layer = std::any_of(
            planned_draws.begin(),
            planned_draws.end(),
            [&](const planned_draw_t& draw) {
                return
                    draw.is_builtin &&
                    is_builtin_primitive_drawable(draw.primitive_style, window);
            });
        const bool has_custom_layer = std::any_of(
            planned_draws.begin(),
            planned_draws.end(),
            [](const planned_draw_t& draw) {
                return !draw.is_builtin;
            });
        const bool needs_sample_buffer =
            has_drawable_builtin_layer || has_custom_layer;
        if (!needs_sample_buffer) {
            invalidate_view_upload_state(view_state);
            return;
        }
        bool samples_ready = true;
        samples_ready = rhi_prepare_series_view_samples(
            ctx,
            view_state,
            window);
        const bool reuses_uploaded_geometry =
            plan.view_kind == Series_view_kind::MAIN
                ? draw_state.main_reuses_uploaded_geometry
                : draw_state.preview_reuses_uploaded_geometry;
        if (samples_ready                              &&
            view_state.last_sample_upload_count > 0   &&
            !reuses_uploaded_geometry)
        {
            view_state.line_window_geometry_dirty = true;
        }
        const qrhi_series_sample_buffer_t sample_buffer =
            samples_ready
                ? make_sample_buffer(view_state, window)
                : qrhi_series_sample_buffer_t{};
        const bool needs_view_ubo = has_custom_layer;
        series_view_uniform_std140_t uniform{};
        const series_view_uniform_std140_t* view_uniform = nullptr;
        QRhiBuffer* view_ubo = nullptr;
        if (needs_view_ubo) {
            uniform      = make_series_view_uniform(ctx, *draw_state.series, window);
            view_uniform = &uniform;
            view_ubo     = ensure_view_ubo(
                draw_state.id,
                plan.view_kind,
                *draw_state.series,
                window);
        }

        const float line_width_px = ctx.config
            ? static_cast<float>(ctx.config->line_width_px) : 1.0f;
        const float point_diameter_px = ctx.config
            ? static_cast<float>(ctx.config->point_diameter_px) : 1.0f;
        const float area_fill_alpha = ctx.config
            ? static_cast<float>(ctx.config->area_fill_alpha) : 0.3f;
        const float stack_sum_width_extra_px = ctx.config
            ? static_cast<float>(ctx.config->stack_sum_line_width_extra_px) : 2.0f;
        const glm::vec4 stack_sum_color =
            resolved_color_palette(ctx.config, ctx.dark_mode).stack_sum_line;

        for (const auto& planned_draw : planned_draws) {
            if (planned_draw.is_builtin) {
                if (!samples_ready ||
                    !is_builtin_primitive_drawable( planned_draw.primitive_style, window))
                {
                    continue;
                }

                std::vector<builtin_segment_span_t> prepared_segment_spans;
                const glm::vec4 series_color = planned_draw.stack_sum_overlay
                    ? stack_sum_color
                    : draw_state.series->color;
                const glm::vec4 draw_color = builtin_draw_color(
                    ctx,
                    series_color,
                    planned_draw.primitive_style,
                    window,
                    area_fill_alpha);
                const float draw_line_width_px = planned_draw.stack_sum_overlay
                    ? line_width_px + stack_sum_width_extra_px
                    : line_width_px;
                if (rhi_prepare_series_primitive(
                        ctx, planned_draw.primitive_style, view_state, window, draw_color, draw_line_width_px,
                        point_diameter_px, planned_draw.stack_sum_overlay, &prepared_segment_spans))
                {
                    rhi_state_t::prepared_draw_command_t command;
                    command.kind =
                        rhi_state_t::prepared_draw_command_t::kind_t::BUILTIN;
                    command.z_order           = planned_draw.z_order;
                    command.series_id         = draw_state.id;
                    command.view_kind         = plan.view_kind;
                    command.series_order      = draw_state.series_order;
                    command.insertion_order   = next_draw_insertion_order++;
                    command.series            = draw_state.series.get();
                    command.window            = window;
                    command.primitive_style   = planned_draw.primitive_style;
                    command.view_state        = &view_state;
                    command.stack_sum_overlay = planned_draw.stack_sum_overlay;
                    command.draw_color        = draw_color;
                    command.line_width_px     = draw_line_width_px;
                    command.builtin_segment_spans =
                        std::move(prepared_segment_spans);
                    m_rhi_state->prepared_draws.push_back(std::move(command));
                }
                continue;
            }

            const auto& layer = planned_draw.layer;
            if (!layer) {
                continue;
            }

            const detail::erased_access_policy_t layer_access_view =
                plan.access
                    ? detail::make_erased_access_policy_view(*plan.access)
                    : detail::erased_access_policy_t{};
            const detail::access_policy_cache_key_t layer_access_key = detail::make_access_policy_cache_key(
                plan.access,
                layer_access_view);

            rhi_state_t::qrhi_layer_program_key_t program_key;
            program_key.series_id      = draw_state.id;
            program_key.view_kind      = plan.view_kind;
            program_key.layer_id       = std::string(layer->id());
            program_key.layer_revision = layer->revision();
            program_key.data_identity  = plan.source ? plan.source->identity() : nullptr;
            program_key.layout_key     = plan.access ? plan.access->layout_key : 0;
            program_key.access_key     = layer_access_key;
            program_key.rhi            = rhi;

            if (!window.snapshot) {
                for (auto& [cached_key, cache_entry] : m_rhi_state->qrhi_layer_cache) {
                    if (cached_key.series_id      == program_key.series_id      &&
                        cached_key.view_kind      == program_key.view_kind      &&
                        cached_key.layer_id       == program_key.layer_id       &&
                        cached_key.layer_revision == program_key.layer_revision &&
                        cached_key.layout_key     == program_key.layout_key     &&
                        cached_key.access_key     == program_key.access_key     &&
                        cached_key.rhi            == program_key.rhi)
                    {
                        cache_entry.last_frame_used = m_frame_id;
                    }
                }
                continue;
            }
            if (!view_ubo) {
                continue;
            }

            rhi_state_t::qrhi_layer_data_key_t data_key;
            data_key.lod_level            = window.lod_level;
            data_key.sample_sequence      = window.sample_sequence;
            data_key.t_origin_ns          = window.t_origin_ns;
            data_key.source_first         = window.source_first;
            data_key.source_count         = window.source_count;
            data_key.synthetic_hold_count = window.synthetic_hold_count;
            data_key.gpu_count            = window.gpu_count;
            data_key.hold_last_forward    = window.hold_last_forward;
            data_key.hold_timestamp_ns    = window.hold_timestamp_ns;
            data_key.interpolation        = window.interpolation;
            data_key.nonfinite_policy     = window.nonfinite_policy;
            data_key.drawable_span_count  = window.drawable_spans.size();
            data_key.drawable_spans_hash =
                hash_drawable_spans(window.drawable_spans);
            data_key.access_key = layer_access_key;

            auto& cache_entry       = m_rhi_state->qrhi_layer_cache[program_key];
            bool  resources_changed = false;
            if (!cache_entry.state) {
                cache_entry.state = layer->create_state(*rhi);
                resources_changed = true;
            }
            if (!cache_entry.has_data_key || !(cache_entry.data_key == data_key)) {
                resources_changed = true;
            }
            cache_entry.data_key        = data_key;
            cache_entry.has_data_key    = true;
            cache_entry.last_frame_used = m_frame_id;

            if (!cache_entry.state) {
                continue;
            }

            qrhi_series_prepare_context_t prepare_ctx;
            prepare_ctx.rhi               = rhi;
            prepare_ctx.render_target     = ctx.render_target;
            prepare_ctx.updates           = rhi_updates;
            prepare_ctx.asset_loader      = m_asset_loader;
            prepare_ctx.frame             = &ctx;
            prepare_ctx.series            = draw_state.series.get();
            prepare_ctx.window            = window;
            prepare_ctx.view_uniform      = view_uniform;
            prepare_ctx.view_ubo          = view_ubo;
            prepare_ctx.sample_buffer     = sample_buffer;
            prepare_ctx.resources_changed = resources_changed;

            if (cache_entry.state->prepare(prepare_ctx)) {
                rhi_state_t::prepared_draw_command_t command;
                command.kind =
                    rhi_state_t::prepared_draw_command_t::kind_t::CUSTOM;
                command.z_order         = planned_draw.z_order;
                command.series_id       = draw_state.id;
                command.view_kind       = plan.view_kind;
                command.series_order    = draw_state.series_order;
                command.insertion_order = next_draw_insertion_order++;
                command.state           = cache_entry.state.get();
                command.series          = draw_state.series.get();
                command.window          = window;
                command.view_ubo        = view_ubo;
                m_rhi_state->prepared_draws.push_back(std::move(command));
            }
        }

        if (view_state.last_sample_upload_count > 0) {
            frame_sample_upload_bytes += view_state.last_sample_upload_bytes;
            frame_sample_upload_count += view_state.last_sample_upload_count;
            if (profiler) {
                profiler->record_observation(
                    "renderer.series_view.sample_upload_count",
                    static_cast<double>(view_state.last_sample_upload_count));
                profiler->record_observation(
                    plan.view_kind == Series_view_kind::PREVIEW
                        ? "renderer.series_view.preview.sample_upload_count"
                        : "renderer.series_view.main.sample_upload_count",
                    static_cast<double>(view_state.last_sample_upload_count));
            }
        }
        if (view_state.last_line_window_upload_count > 0) {
            frame_line_window_upload_bytes += view_state.last_line_window_upload_bytes;
            frame_line_window_upload_count += view_state.last_line_window_upload_count;
        }
        frame_uniform_upload_bytes += view_state.last_uniform_upload_bytes;
        frame_uniform_upload_count += view_state.last_uniform_upload_count;
    };

    for (auto& draw_state : draw_states) {
        if (!draw_state.vbo_state) {
            continue;
        }
        prepare_view_layers(
            draw_state,
            draw_state.main_plan,
            draw_state.vbo_state->main_view);
    }

    if (preview_visible) {
        for (auto& draw_state : draw_states) {
            if (!draw_state.vbo_state || !draw_state.has_preview) {
                continue;
            }
            prepare_view_layers(
                draw_state,
                draw_state.preview_plan,
                draw_state.vbo_state->preview_view);
        }
    }

    std::stable_sort(
        m_rhi_state->prepared_draws.begin(),
        m_rhi_state->prepared_draws.end(),
        [](const auto& a, const auto& b) {
            const int a_view = view_band_order(a.view_kind);
            const int b_view = view_band_order(b.view_kind);
            if (a_view != b_view)                 { return a_view < b_view;                 }
            if (a.z_order != b.z_order)           { return a.z_order < b.z_order;           }
            if (a.series_order != b.series_order) { return a.series_order < b.series_order; }
            return a.insertion_order < b.insertion_order;
        });

    if (profiler) {
        profiler->record_observation(
            "renderer.frame.uploaded_sample_bytes",
            static_cast<double>(frame_sample_upload_bytes));
        profiler->record_observation(
            "renderer.frame.sample_upload_count",
            static_cast<double>(frame_sample_upload_count));
        profiler->record_observation(
            "renderer.frame.upload.primary_bytes",
            static_cast<double>(frame_sample_upload_bytes));
        profiler->record_observation(
            "renderer.frame.upload.primary_count",
            static_cast<double>(frame_sample_upload_count));
        profiler->record_observation(
            "renderer.frame.upload.line_window_bytes",
            static_cast<double>(frame_line_window_upload_bytes));
        profiler->record_observation(
            "renderer.frame.upload.line_window_count",
            static_cast<double>(frame_line_window_upload_count));
        profiler->record_observation(
            "renderer.frame.upload.uniform_bytes",
            static_cast<double>(frame_uniform_upload_bytes));
        profiler->record_observation(
            "renderer.frame.upload.uniform_count",
            static_cast<double>(frame_uniform_upload_count));
        profiler->record_observation("renderer.frame.upload.known_custom_bytes", 0.0);
        profiler->record_observation("renderer.frame.upload.known_custom_count", 0.0);
        profiler->record_observation(
            "renderer.frame.upload.total_bytes",
            static_cast<double>(
                frame_sample_upload_bytes +
                frame_line_window_upload_bytes +
                frame_uniform_upload_bytes));
        profiler->record_observation(
            "renderer.frame.upload.total_count",
            static_cast<double>(
                frame_sample_upload_count +
                frame_line_window_upload_count +
                frame_uniform_upload_count));
    }

    const auto qrhi_layer_still_configured =
        [&](const rhi_state_t::qrhi_layer_program_key_t& key) {
            const auto series_it = series.find(key.series_id);
            if (series_it == series.end() || !series_it->second || !series_it->second->enabled) {
                return false;
            }

            const series_data_t&      series_data = *series_it->second;
            const Data_source*        source      = nullptr;
            const Data_access_policy* access      = nullptr;
            if (key.view_kind == Series_view_kind::MAIN) {
                source = series_data.main_source();
                access = &series_data.main_access();
            }
            else {
                if (!preview_visible) {
                    return false;
                }
                source = series_data.preview_source();
                access = &series_data.preview_access();
            }
            if (!source || !access ||
                key.data_identity != source->identity() ||
                key.layout_key    != access->layout_key ||
                key.rhi           != rhi)
            {
                return false;
            }
            const detail::erased_access_policy_t access_view =
                detail::make_erased_access_policy_view(*access);
            const detail::access_policy_cache_key_t access_key = detail::make_access_policy_cache_key(
                access,
                access_view);
            if (key.access_key != access_key) {
                return false;
            }

            const auto& qrhi_layers = qrhi_layers_for(series_data);
            return std::any_of(
                qrhi_layers.begin(),
                qrhi_layers.end(),
                [&](const auto& layer) {
                    return
                        layer                            &&
                        layer->draws_view(key.view_kind) &&
                        layer->id() == key.layer_id      &&
                        layer->revision() == key.layer_revision;
                });
        };

    for (auto it = m_rhi_state->qrhi_layer_cache.begin();
        it != m_rhi_state->qrhi_layer_cache.end();)
    {
        if (it->second.last_frame_used == m_frame_id ||
            qrhi_layer_still_configured(it->first))
        {
            ++it;
            continue;
        }
        if (it->second.state) {
            it->second.state->cleanup_qrhi_resources(it->first.rhi);
        }
        it = m_rhi_state->qrhi_layer_cache.erase(it);
    }
    m_last_qrhi_layer_cache_size = m_rhi_state->qrhi_layer_cache.size();

    for (auto it = m_rhi_state->view_ubos.begin();
        it != m_rhi_state->view_ubos.end();)
    {
        if (it->second.last_frame_used == m_frame_id) {
            ++it;
            continue;
        }
        it = m_rhi_state->view_ubos.erase(it);
    }

    m_rhi_state->frame_plan_ready = true;
}

void Series_renderer::render(
    const frame_context_t& ctx,
    const std::map<int, std::shared_ptr<const series_data_t>>&
                           series)
{
    if (!ctx.rhi) {
        prepare(ctx, series);
        m_rhi_state->pending_updates = nullptr;
        m_rhi_state->frame_draw_states.clear();
        m_rhi_state->prepared_draws.clear();
        m_rhi_state->frame_plan_ready = false;
        clear_frame_snapshot_caches();
        return;
    }

    if (!ctx.rhi || !ctx.cb || !m_rhi_state->frame_plan_ready) {
        return;
    }

    const auto apply_band_scissor = [&](const sample_window_t& window) {
        const auto to_scissor_y = [&](double top, double height) -> int {
            return static_cast<int>(std::lround(double(ctx.win_h) - (top + height)));
        };
        if (window.view_kind == Series_view_kind::PREVIEW) {
            ctx.cb->setScissor(QRhiScissor(
                0,
                to_scissor_y(window.y_offset_px, window.height_px),
                ctx.win_w,
                static_cast<int>(std::max(1.0f, window.height_px))));
            return;
        }
        ctx.cb->setScissor(QRhiScissor(
            0,
            to_scissor_y(0.0, ctx.layout.usable_height),
            static_cast<int>(std::max(1.0, ctx.layout.usable_width)),
            static_cast<int>(std::max(1.0, ctx.layout.usable_height))));
    };

    for (auto& command : m_rhi_state->prepared_draws) {
        apply_band_scissor(command.window);
        m_last_recorded_draw_z_orders.push_back(command.z_order);
        m_last_recorded_draw_styles.push_back(
            command.kind ==
                    rhi_state_t::prepared_draw_command_t::kind_t::BUILTIN
                        ? command.primitive_style
                        : Display_style::NONE);
        m_last_recorded_draw_series_ids.push_back(command.series_id);
        m_last_recorded_draw_view_kinds.push_back(command.view_kind);
        m_last_recorded_stack_sum_overlays.push_back(command.stack_sum_overlay);
        m_last_recorded_draw_colors.push_back(command.draw_color);
        m_last_recorded_line_widths.push_back(command.line_width_px);

        if (command.kind ==
            rhi_state_t::prepared_draw_command_t::kind_t::BUILTIN)
        {
            if (command.view_state) {
                rhi_record_series_primitive(
                    ctx,
                    command.primitive_style,
                    *command.view_state,
                    command.window,
                    command.stack_sum_overlay,
                    command.builtin_segment_spans);
            }
            continue;
        }

        if (!command.state) {
            continue;
        }
        qrhi_series_record_context_t record_ctx;
        record_ctx.cb            = ctx.cb;
        record_ctx.render_target = ctx.render_target;
        record_ctx.frame         = &ctx;
        record_ctx.series        = command.series;
        record_ctx.window        = command.window;
        record_ctx.view_ubo      = command.view_ubo;
        command.state->record(record_ctx);
    }

    m_rhi_state->pending_updates = nullptr;
    m_rhi_state->frame_draw_states.clear();
    m_rhi_state->prepared_draws.clear();
    m_rhi_state->frame_plan_ready = false;
    clear_frame_snapshot_caches();
}

bool Series_renderer::rhi_prepare_series_view_samples(
    const frame_context_t& ctx,
    vbo_view_state_t&      view_state,
    const sample_window_t& window)
{
    QRhi* rhi = ctx.rhi;
    QRhiResourceUpdateBatch* updates = ctx.rhi_updates;

    const auto invalidate_uploaded_vbo = [&]() {
        view_state.has_uploaded_vbo = false;
        if (view_state.planner) {
            view_state.planner->uploaded_t_origin_ns =
                detail::series_window_planner_state_t::k_no_timestamp;
        }
        view_state.staging.clear();
        view_state.last_staged_sample_count      = 0;
        view_state.last_sample_upload_bytes      = 0;
        view_state.last_line_window_upload_bytes = 0;
        view_state.last_line_window_upload_count = 0;
        view_state.last_uniform_upload_bytes     = 0;
        view_state.last_uniform_upload_count     = 0;
        view_state.last_line_window_sample_count = 0;
        view_state.last_prepared_t_min_ns        = 0;
        view_state.last_prepared_t_max_ns        = 0;
        view_state.last_prepared_width_px        = 0.0;
        view_state.last_sample_buffer            = nullptr;
        view_state.line_window_geometry_dirty    = true;
        view_state.line_draw_spans.clear();
    };

    if (window.gpu_count == 0) {
        invalidate_uploaded_vbo();
        return false;
    }

    if (!rhi) {
        return false;
    }

    if (!view_state.rhi) {
        view_state.rhi = std::make_unique<vbo_view_state_t::rhi_buffers_t>();
    }

    const data_snapshot_t& snapshot = window.snapshot;
    const detail::erased_access_policy_t access_view = window.access
        ? detail::make_erased_access_policy_view(*window.access)
        : detail::erased_access_policy_t{};
    view_state.last_sample_access_dispatch_kind = access_view.dispatch_kind;
    if (snapshot) {
        if (!access_view.has_timestamp() || !updates) {
            invalidate_uploaded_vbo();
            return false;
        }

        if (window.synthetic_hold_count > 1              ||
            window.drawable_spans.empty()                ||
            window.source_first         > snapshot.count ||
            window.source_count         > snapshot.count - window.source_first)
        {
            invalidate_uploaded_vbo();
            return false;
        }

        std::size_t expected_gpu_count  = 0;
        bool        synthetic_hold_seen = false;
        for (std::size_t span_index = 0;
            span_index < window.drawable_spans.size();
            ++span_index)
        {
            const drawable_sample_span_t& span =
                window.drawable_spans[span_index];
            const bool final_span =
                span_index + 1u == window.drawable_spans.size();
            if (span.source_count == 0               || span.gpu_count < span.source_count                     ||
                span.gpu_first != expected_gpu_count || span.source_first < window.source_first                ||
                span.source_first >  snapshot.count  || span.source_count > snapshot.count - span.source_first ||
                span.source_first + span.source_count >  window.source_first + window.source_count)
            {
                invalidate_uploaded_vbo();
                return false;
            }

            const bool has_synthetic_hold =
                final_span                               &&
                window.synthetic_hold_count == 1         &&
                span.gpu_count == span.source_count + 1u;
            if (has_synthetic_hold) {
                synthetic_hold_seen = true;
            }
            else
            if (span.gpu_count != span.source_count) {
                invalidate_uploaded_vbo();
                return false;
            }

            if (!detail::checked_size_add(
                    expected_gpu_count, span.gpu_count, expected_gpu_count))
            {
                invalidate_uploaded_vbo();
                return false;
            }
        }
        if (expected_gpu_count  != window.gpu_count ||
            synthetic_hold_seen != (window.synthetic_hold_count == 1))
        {
            invalidate_uploaded_vbo();
            return false;
        }

        std::size_t needed_elements = 0;
        std::size_t needed_bytes    = 0;
        quint32     upload_bytes    = 0;
        if (!detail::checked_size_add(window.gpu_count, 0u, needed_elements) ||
            !detail::qrhi_byte_size(
                needed_elements, sizeof(gpu_sample_t), needed_bytes, upload_bytes))
        {
            invalidate_uploaded_vbo();
            return false;
        }

        auto& staging = view_state.staging;
        staging.resize(needed_elements);

        const auto stage_one_sample =
            [&](gpu_sample_t& dst, const void* src, std::int64_t ts_ns) {
                detail::sample_draw_value_t draw_value;
                const detail::sample_draw_status_t status = detail::read_sample_draw_value(
                    access_view,
                    src,
                    window.nonfinite_policy,
                    draw_value);
                if (status != detail::sample_draw_status_t::DRAWABLE) {
                    return false;
                }
                dst.t_rel = detail::to_view_seconds(ts_ns, window.t_origin_ns);
                dst.y     = draw_value.y;
                dst.y_min = draw_value.y_min;
                dst.y_max = draw_value.y_max;
                return true;
            };

        for (const drawable_sample_span_t& span : window.drawable_spans) {
            for (std::size_t i = 0; i < span.source_count; ++i) {
                const std::size_t source_index = span.source_first + i;
                const void*       src          = snapshot.at(source_index);
                if (!src ||
                    !stage_one_sample(
                        staging[span.gpu_first + i], src, access_view.timestamp(src)))
                {
                    invalidate_uploaded_vbo();
                    return false;
                }
            }
            if (span.gpu_count == span.source_count + 1u) {
                const std::size_t source_index =
                    span.source_first + span.source_count - 1u;
                const void* source_sample = snapshot.at(source_index);
                if (!source_sample ||
                    !stage_one_sample(
                        staging[span.gpu_first + span.gpu_count - 1u],
                        source_sample, window.hold_timestamp_ns))
                {
                    invalidate_uploaded_vbo();
                    return false;
                }
            }
        }

        std::size_t alloc_bytes      = 0;
        quint32     qrhi_alloc_bytes = 0;
        if (!detail::qrhi_grown_capacity_bytes(
                needed_bytes, alloc_bytes, qrhi_alloc_bytes))
        {
            invalidate_uploaded_vbo();
            return false;
        }
        if (!view_state.rhi->vbo ||
            view_state.rhi_vbo_capacity_bytes < needed_bytes)
        {
            view_state.rhi->vbo.reset(rhi->newBuffer(
                QRhiBuffer::Static,
                QRhiBuffer::VertexBuffer,
                qrhi_alloc_bytes));
            if (view_state.rhi->vbo && view_state.rhi->vbo->create()) {
                view_state.rhi_vbo_capacity_bytes = alloc_bytes;
                ++view_state.last_vbo_generation;
                if (ctx.config && ctx.config->profiler) {
                    ctx.config->profiler->record_observation(
                        "renderer.frame.gpu_buffer_allocation_bytes",
                        static_cast<double>(alloc_bytes));
                    ctx.config->profiler->record_counter(
                        "renderer.frame.gpu_buffer_allocation_count");
                }
            }
            else {
                view_state.rhi->vbo.reset();
                view_state.rhi_vbo_capacity_bytes = 0;
                invalidate_uploaded_vbo();
                return false;
            }
        }
        updates->uploadStaticBuffer(
            view_state.rhi->vbo.get(),
            0,
            upload_bytes,
            staging.data());
        view_state.last_staged_sample_count = needed_elements;
        view_state.last_sample_upload_bytes = upload_bytes;
        ++view_state.last_sample_upload_count;
        view_state.has_uploaded_vbo = true;
    }

    if (!view_state.rhi->vbo) {
        return false;
    }
    view_state.last_sample_buffer     = view_state.rhi->vbo.get();
    view_state.last_prepared_t_min_ns = window.t_min_ns;
    view_state.last_prepared_t_max_ns = window.t_max_ns;
    view_state.last_prepared_width_px = window.width_px;
    return true;
}

bool Series_renderer::rhi_prepare_series_primitive(
    const frame_context_t&                         ctx,
    Display_style                                  primitive_style,
    vbo_view_state_t&                              view_state,
    const sample_window_t&                         window,
    const glm::vec4&                               draw_color,
    float                                          line_width_px,
    float                                          point_diameter_px,
    bool                                           stack_sum_overlay,
    std::vector<detail::builtin_segment_span_t>*   out_segment_spans)
{
    QRhi* rhi = ctx.rhi;
    QRhiResourceUpdateBatch* updates = ctx.rhi_updates;

    const bool        is_dots = (primitive_style == Display_style::DOTS);
    const bool        is_area = (primitive_style == Display_style::AREA);
    const std::size_t count   = window.gpu_count;
    if (count == 0) {
        return false;
    }
    const std::vector<builtin_segment_span_t> segment_spans =
        is_dots
            ? std::vector<builtin_segment_span_t>{}
            : builtin_segment_spans(window);
    const bool has_segment_span = !segment_spans.empty();
    if (!is_dots && !has_segment_span) {
        return false;
    }
    // Hand the computed spans to the caller so the record pass can reuse them
    // (empty for DOTS, which draw per-sample instances).
    if (out_segment_spans) {
        *out_segment_spans = segment_spans;
    }

    QRhiRenderTarget* rt = ctx.render_target;

    // Lazy QShader load.
    if (!m_rhi_state->shaders_loaded) {
        m_rhi_state->cached_dot_vert  = load_qsb("plot_dot_quad.vert.qsb");
        m_rhi_state->cached_dot_frag  = load_qsb("plot_dot_quad.frag.qsb");
        m_rhi_state->cached_line_vert = load_qsb("plot_line.vert.qsb");
        m_rhi_state->cached_line_frag = load_qsb("plot_line.frag.qsb");
        m_rhi_state->cached_area_vert = load_qsb("plot_area.vert.qsb");
        m_rhi_state->cached_area_frag = load_qsb("plot_area.frag.qsb");
        m_rhi_state->shaders_loaded   = true;
    }

    const QShader& vert = is_dots
        ? m_rhi_state->cached_dot_vert
        : (is_area ? m_rhi_state->cached_area_vert : m_rhi_state->cached_line_vert);
    const QShader& frag = is_dots
        ? m_rhi_state->cached_dot_frag
        : (is_area ? m_rhi_state->cached_area_frag : m_rhi_state->cached_line_frag);
    if (!vert.isValid() || !frag.isValid()) {
        return false;
    }

    if (!view_state.rhi) {
        view_state.rhi = std::make_unique<vbo_view_state_t::rhi_buffers_t>();
    }

    auto ensure_ubo = [&](vbo_view_state_t::rhi_buffers_t::srb_entry_t& entry) -> bool {
        const bool already_sized =
            entry.ubo && entry.ubo_capacity_bytes >= k_series_ubo_bytes;
        if (!detail::ensure_dynamic_ubo(
                rhi, entry.ubo, entry.ubo_capacity_bytes, k_series_ubo_bytes))
        {
            return false;
        }
        if (!already_sized) {
            // ensure_dynamic_ubo replaced the QRhiBuffer; the SRB still holds
            // the old handle. Invalidate so the per-view SRB rebuild below
            // captures the new pointer.
            entry.srb.reset();
            entry.last_ubo = nullptr;
            if (ctx.config && ctx.config->profiler) {
                ctx.config->profiler->record_observation(
                    "renderer.frame.gpu_buffer_allocation_bytes",
                    static_cast<double>(k_series_ubo_bytes));
                ctx.config->profiler->record_counter(
                    "renderer.frame.gpu_buffer_allocation_count");
            }
        }
        return true;
    };

    auto& primary_srb_entry = is_dots
        ? view_state.rhi->dots_srb
        : (is_area
            ? view_state.rhi->area_fill_srb
            : (stack_sum_overlay
                ? view_state.rhi->stack_sum_line_srb
                : view_state.rhi->line_srb));
    if (!ensure_ubo(primary_srb_entry)) {
        return false;
    }

    if (!view_state.rhi->vbo) {
        return false;
    }

    // LINE under RHI feeds the vertex shader through four per-instance
    // vertex attributes (prev, p0, p1, next) sourced from a dedicated
    // per-frame buffer with the active sample window padded by leading and
    // trailing duplicates: [s[first], s[first], s[first+1], ..., s[last],
    // s[last]]. The buffer is bound four times at element offsets 0, 1, 2, 3,
    // so instance i sees (padded[i], padded[i+1],
    // padded[i+2], padded[i+3]) which collapses to the desired clamping at
    // the window edges. SSBOs are unusable here because SPIRV-Cross emits
    // them as RWByteAddressBuffer UAVs and D3D11 SM 5.0 vertex shaders
    // accept zero UAVs; QRhi requires HLSL 5.0 bytecode for D3D11, so any
    // storage-buffer access in the vertex stage fails to compile.
    const bool line_geometry_current =
        !view_state.line_window_geometry_dirty &&
        view_state.rhi->line_window_vbo        &&
        !view_state.line_draw_spans.empty();
    if (!is_dots && !is_area && !line_geometry_current) {
        view_state.line_window_geometry_dirty = true;
        view_state.line_draw_spans.clear();
        std::size_t total_window_count = 0;
        std::size_t total_padded_count = 0;
        for (const builtin_segment_span_t& span : segment_spans) {
            std::size_t span_window_count = 0;
            if (!line_window_sample_count(
                    span.gpu_count, window.interpolation, span_window_count))
            {
                return false;
            }
            std::size_t span_padded_count = 0;
            if (!detail::checked_size_add( span_window_count, 2u, span_padded_count) ||
                !detail::checked_size_add(
                    total_window_count, span_window_count, total_window_count)       ||
                !detail::checked_size_add(
                    total_padded_count, span_padded_count, total_padded_count))
            {
                return false;
            }
        }
        if (total_window_count < 2 || total_padded_count == 0) {
            return false;
        }

        std::size_t needed_bytes = 0;
        quint32     upload_bytes = 0;
        if (!detail::qrhi_byte_size(
                total_padded_count, sizeof(gpu_sample_t), needed_bytes, upload_bytes))
        {
            return false;
        }
        std::size_t alloc_bytes      = 0;
        quint32     qrhi_alloc_bytes = 0;
        if (!detail::qrhi_grown_capacity_bytes(
                needed_bytes, alloc_bytes, qrhi_alloc_bytes))
        {
            return false;
        }
        if (!view_state.rhi->line_window_vbo ||
            view_state.rhi_line_window_vbo_capacity_bytes < needed_bytes)
        {
            view_state.rhi->line_window_vbo.reset(rhi->newBuffer(
                QRhiBuffer::Static, QRhiBuffer::VertexBuffer,
                qrhi_alloc_bytes));
            if (view_state.rhi->line_window_vbo && view_state.rhi->line_window_vbo->create())
            {
                view_state.rhi_line_window_vbo_capacity_bytes = alloc_bytes;
                if (ctx.config && ctx.config->profiler) {
                    ctx.config->profiler->record_observation(
                        "renderer.frame.gpu_buffer_allocation_bytes",
                        static_cast<double>(alloc_bytes));
                    ctx.config->profiler->record_counter(
                        "renderer.frame.gpu_buffer_allocation_count");
                }
            }
            else {
                view_state.rhi->line_window_vbo.reset();
                view_state.rhi_line_window_vbo_capacity_bytes = 0;
                view_state.line_window_geometry_dirty         = true;
                return false;
            }
        }
        if (updates) {
            if (view_state.staging.size() < count) {
                return false;
            }
            auto& padded = view_state.line_window_staging;
            padded.resize(total_padded_count);
            std::size_t write_idx = 0;
            for (const builtin_segment_span_t& span : segment_spans) {
                if (span.gpu_first > view_state.staging.size() ||
                    span.gpu_count >
                        view_state.staging.size() - span.gpu_first)
                {
                    return false;
                }

                std::size_t span_window_count = 0;
                if (!line_window_sample_count(
                        span.gpu_count, window.interpolation, span_window_count))
                {
                    return false;
                }
                std::size_t span_padded_count = 0;
                if (!detail::checked_size_add( span_window_count, 2u, span_padded_count) ||
                    span_padded_count > total_padded_count - write_idx)
                {
                    return false;
                }

                vbo_view_state_t::line_draw_span_t line_span;
                line_span.gpu_first  = span.gpu_first;
                line_span.gpu_count  = span.gpu_count;
                line_span.line_first = write_idx;
                line_span.line_count = span_window_count;
                view_state.line_draw_spans.push_back(line_span);

                const std::size_t span_base = write_idx;
                const std::size_t span_last =
                    span.gpu_first + span.gpu_count - 1u;
                std::size_t span_write = span_base + 1u;
                padded[span_base] = view_state.staging[span.gpu_first];
                if (window.interpolation == Series_interpolation::STEP_AFTER) {
                    padded[span_write++] =
                        view_state.staging[span.gpu_first];
                    for (std::size_t i = 1; i < span.gpu_count; ++i) {
                        const gpu_sample_t previous =
                            view_state.staging[span.gpu_first + i - 1u];
                        const gpu_sample_t current =
                            view_state.staging[span.gpu_first + i];
                        gpu_sample_t held    = current;
                        held.y               = previous.y;
                        held.y_min           = previous.y_min;
                        held.y_max           = previous.y_max;
                        padded[span_write++] = held;
                        padded[span_write++] = current;
                    }
                }
                else {
                    for (std::size_t i = 0; i < span.gpu_count; ++i) {
                        padded[span_write++] =
                            view_state.staging[span.gpu_first + i];
                    }
                }
                padded[span_base + span_padded_count - 1u] =
                    view_state.staging[span_last];
                write_idx += span_padded_count;
            }
            updates->uploadStaticBuffer(
                view_state.rhi->line_window_vbo.get(),
                0,
                upload_bytes,
                padded.data());
            view_state.last_line_window_sample_count = total_window_count;
            view_state.last_line_window_upload_bytes = upload_bytes;
            ++view_state.last_line_window_upload_count;
            view_state.line_window_geometry_dirty = false;
        }
    }

    // Pipeline cache key: only kind. The pipeline descriptor depends on the
    // shader-resource-binding LAYOUT (which slots, which stages), not on the
    // concrete buffer handles. Per-series binding handles ride the SRB,
    // which is rebuilt per view below.
    rhi_state_t::pipeline_key_t key{
        is_dots
            ? rhi_state_t::pipeline_kind_t::DOTS
            : (is_area
                ? rhi_state_t::pipeline_kind_t::AREA
                : rhi_state_t::pipeline_kind_t::LINE)
    };
    auto& cached = m_rhi_state->pipelines[key];

    // The pipeline state object captures the render-pass descriptor and
    // sample count of the target it was built for. A resize that recreates
    // the FBO can swap the descriptor out from under us, leaving the cached
    // pipeline incompatible with the new pass; rebuild when the descriptor
    // or sample count moves.
    QRhiRenderPassDescriptor* current_rpd     = rt->renderPassDescriptor();
    const int                 current_samples = rt->sampleCount();
    if (cached.pipeline && (cached.last_rpd != current_rpd || cached.last_sample_count != current_samples))
    {
        cached.pipeline.reset();
    }

    if (!cached.pipeline) {
        cached.vert = vert;
        cached.frag = frag;

        QRhiVertexInputLayout vlayout;
        if (is_dots) {
            QRhiVertexInputBinding ib0(
                static_cast<quint32>(sizeof(gpu_sample_t)),
                QRhiVertexInputBinding::PerInstance,
                1);
            vlayout.setBindings({ib0});
            QRhiVertexInputAttribute a0(
                0, 0, QRhiVertexInputAttribute::Float,
                static_cast<quint32>(offsetof(gpu_sample_t, t_rel)));
            QRhiVertexInputAttribute a1(
                0, 1, QRhiVertexInputAttribute::Float,
                static_cast<quint32>(offsetof(gpu_sample_t, y)));
            vlayout.setAttributes({a0, a1});
        }
        else
        if (is_area) {
            const quint32 stride = static_cast<quint32>(sizeof(gpu_sample_t));
            QRhiVertexInputBinding ib_p0(stride, QRhiVertexInputBinding::PerInstance, 1);
            QRhiVertexInputBinding ib_p1(stride, QRhiVertexInputBinding::PerInstance, 1);
            vlayout.setBindings({ib_p0, ib_p1});

            const quint32 t_offset =
                static_cast<quint32>(offsetof(gpu_sample_t, t_rel));
            const quint32 y_offset =
                static_cast<quint32>(offsetof(gpu_sample_t, y));
            QRhiVertexInputAttribute a_x0(0, 0, QRhiVertexInputAttribute::Float, t_offset);
            QRhiVertexInputAttribute a_y0(0, 1, QRhiVertexInputAttribute::Float, y_offset);
            QRhiVertexInputAttribute a_min0(
                0, 2, QRhiVertexInputAttribute::Float,
                static_cast<quint32>(offsetof(gpu_sample_t, y_min)));
            QRhiVertexInputAttribute a_max0(
                0, 3, QRhiVertexInputAttribute::Float,
                static_cast<quint32>(offsetof(gpu_sample_t, y_max)));
            QRhiVertexInputAttribute a_x1(1, 4, QRhiVertexInputAttribute::Float, t_offset);
            QRhiVertexInputAttribute a_y1(1, 5, QRhiVertexInputAttribute::Float, y_offset);
            QRhiVertexInputAttribute a_min1(
                1, 6, QRhiVertexInputAttribute::Float,
                static_cast<quint32>(offsetof(gpu_sample_t, y_min)));
            QRhiVertexInputAttribute a_max1(
                1, 7, QRhiVertexInputAttribute::Float,
                static_cast<quint32>(offsetof(gpu_sample_t, y_max)));
            vlayout.setAttributes({a_x0, a_y0, a_min0, a_max0,
                a_x1, a_y1, a_min1, a_max1});
        }
        else {
            // LINE binds the line_window_vbo four times at element offsets 0,
            // 1, 2, 3. Each binding has stride sizeof(gpu_sample_t)
            // and steps once per instance, so the bindings form a
            // sliding (prev, p0, p1, next) window over the padded
            // sample array. Only the (t_rel, y) pair is consumed by
            // the LINE vertex shader; the range lanes are left unbound.
            const quint32 line_stride =
                static_cast<quint32>(sizeof(gpu_sample_t));
            QRhiVertexInputBinding ib_prev(
                line_stride, QRhiVertexInputBinding::PerInstance, 1);
            QRhiVertexInputBinding ib_p0(
                line_stride, QRhiVertexInputBinding::PerInstance, 1);
            QRhiVertexInputBinding ib_p1(
                line_stride, QRhiVertexInputBinding::PerInstance, 1);
            QRhiVertexInputBinding ib_next(
                line_stride, QRhiVertexInputBinding::PerInstance, 1);
            vlayout.setBindings({ib_prev, ib_p0, ib_p1, ib_next});

            const quint32 ty_offset =
                static_cast<quint32>(offsetof(gpu_sample_t, t_rel));
            QRhiVertexInputAttribute a_prev(
                0, 0, QRhiVertexInputAttribute::Float2, ty_offset);
            QRhiVertexInputAttribute a_p0(
                1, 1, QRhiVertexInputAttribute::Float2, ty_offset);
            QRhiVertexInputAttribute a_p1(
                2, 2, QRhiVertexInputAttribute::Float2, ty_offset);
            QRhiVertexInputAttribute a_next(
                3, 3, QRhiVertexInputAttribute::Float2, ty_offset);
            vlayout.setAttributes({a_prev, a_p0, a_p1, a_next});
        }

        // RHI series primitives bind only a UBO through the SRB. Sample data
        // rides vertex attributes instead of SSBOs so the D3D11 backend's
        // SM 5.0 vertex shader has zero UAVs.
        detail::alpha_blended_pipeline_desc_t desc;
        desc.vert       = cached.vert;
        desc.frag       = cached.frag;
        desc.vlayout    = vlayout;
        desc.ubo_bytes  = k_series_ubo_bytes;
        desc.ubo_stages = QRhiShaderResourceBinding::VertexStage
                        | QRhiShaderResourceBinding::FragmentStage;
        desc.flags      = QRhiGraphicsPipeline::UsesScissor;
        cached.pipeline = detail::build_alpha_blended_pipeline(rhi, rt, desc);
        if (!cached.pipeline) {
            return false;
        }
        cached.last_rpd          = current_rpd;
        cached.last_sample_count = current_samples;
    }

    // Per-view SRBs. Rebuild whenever the bound UBO pointer has changed since
    // last frame (capacity growth replaces QRhiBuffer outright). The SRB
    // captures concrete handles, so reusing one whose buffers were freed is a
    // use-after-free.
    auto ensure_srb = [&](vbo_view_state_t::rhi_buffers_t::srb_entry_t& entry) -> bool {
        QRhiBuffer* current_ubo = entry.ubo.get();
        const bool srb_handles_match = entry.srb &&
            entry.last_ubo == current_ubo;
        if (srb_handles_match) {
            return true;
        }

        if (!detail::rebuild_single_ubo_srb(
                rhi, entry.srb, current_ubo, k_series_ubo_bytes,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage))
        {
            // A failed create() leaves a non-null but unusable SRB; clear it so
            // the null guard in rhi_record_series_primitive skips this draw and
            // a later frame retries the rebuild instead of binding a bad object.
            entry.srb.reset();
            entry.last_ubo = nullptr;
            return false;
        }
        entry.last_ubo = current_ubo;
        return true;
    };

    if (!ensure_srb(primary_srb_entry)) {
        return false;
    }

    series_view_uniform_std140_t view_block{};
    std::memcpy(view_block.pmv, glm::value_ptr(ctx.pmv), sizeof(float) * 16);

    view_block.color[0] = draw_color.r;
    view_block.color[1] = draw_color.g;
    view_block.color[2] = draw_color.b;
    view_block.color[3] = draw_color.a;

    view_block.t_min    = detail::to_view_seconds(
        window.t_min_ns, window.t_origin_ns);
    view_block.t_max    = detail::to_view_seconds(
        window.t_max_ns, window.t_origin_ns);
    view_block.v_min    = window.v_min;
    view_block.v_max    = window.v_max;
    view_block.y_offset = window.y_offset_px;
    view_block.width    = window.width_px;
    view_block.height   = window.height_px;
    view_block.win_h    = static_cast<float>(ctx.win_h);
    view_block.framebuffer_y_up =
        (ctx.rhi && ctx.rhi->isYUpInFramebuffer()) ? 1 : 0;

    if (is_dots) {
        Dot_block_std140 block{};
        block.view              = view_block;
        block.point_diameter_px = point_diameter_px;
        if (updates) {
            updates->updateDynamicBuffer(
                primary_srb_entry.ubo.get(), 0, sizeof(block), &block);
            view_state.last_uniform_upload_bytes += sizeof(block);
            ++view_state.last_uniform_upload_count;
        }
    }
    else
    if (is_area) {
        Area_block_std140 block{};
        block.view = view_block;
        block.interpolation =
            window.interpolation == Series_interpolation::STEP_AFTER ? 1 : 0;
        block.stacked = window.stacked ? 1 : 0;

        if (updates) {
            updates->updateDynamicBuffer(
                view_state.rhi->area_fill_srb.ubo.get(), 0,
                sizeof(block), &block);
            view_state.last_uniform_upload_bytes += sizeof(block);
            ++view_state.last_uniform_upload_count;
        }
    }
    else {
        Line_block_std140 block{};
        block.view           = view_block;
        block.line_px        = line_width_px;
        block.snap_to_pixels = (ctx.config && ctx.config->snap_lines_to_pixels)
            ? 1 : 0;
        if (updates) {
            updates->updateDynamicBuffer(
                primary_srb_entry.ubo.get(), 0, sizeof(block), &block);
            view_state.last_uniform_upload_bytes += sizeof(block);
            ++view_state.last_uniform_upload_count;
        }
    }
    ++view_state.last_primitive_prepare_count;
    return true;
}

void Series_renderer::rhi_record_series_primitive(
    const frame_context_t& ctx,
    Display_style          primitive_style,
    vbo_view_state_t&      view_state,
    const sample_window_t& window,
    bool                   stack_sum_overlay,
    const std::vector<detail::builtin_segment_span_t>&
                           segment_spans)
{
    QRhiCommandBuffer* cb = ctx.cb;
    if (!cb) {
        return;
    }

    const bool        is_dots = (primitive_style == Display_style::DOTS);
    const bool        is_area = (primitive_style == Display_style::AREA);
    const std::size_t count   = window.gpu_count;
    if (count == 0) {
        return;
    }
    // segment_spans were computed in rhi_prepare_series_primitive and carried on
    // the prepared draw command; DOTS pass an empty span list (drawn per-sample).
    const bool has_segment_span = !segment_spans.empty();
    if (!is_dots && !has_segment_span) {
        return;
    }
    if (!view_state.rhi) {
        return;
    }

    rhi_state_t::pipeline_key_t key{
        is_dots
            ? rhi_state_t::pipeline_kind_t::DOTS
            : (is_area
                ? rhi_state_t::pipeline_kind_t::AREA
                : rhi_state_t::pipeline_kind_t::LINE)
    };
    auto pipe_it = m_rhi_state->pipelines.find(key);
    if (pipe_it == m_rhi_state->pipelines.end() || !pipe_it->second.pipeline) {
        return;
    }
    auto& cached = pipe_it->second;

    auto& srb_entry = is_dots
        ? view_state.rhi->dots_srb
        : (is_area
            ? view_state.rhi->area_fill_srb
            : (stack_sum_overlay
                ? view_state.rhi->stack_sum_line_srb
                : view_state.rhi->line_srb));
    if (!srb_entry.srb) {
        return;
    }
    cb->setGraphicsPipeline(cached.pipeline.get());

    if (is_dots) {
        cb->setShaderResources(srb_entry.srb.get());
        if (view_state.rhi->vbo) {
            QRhiCommandBuffer::VertexInput input{
                view_state.rhi->vbo.get(), 0u};
            cb->setVertexInput(0, 1, &input);
        }
        quint32 instance_count = 0;
        if (!detail::to_qrhi_count(count, instance_count)) {
            return;
        }
        cb->draw(4, instance_count);
        view_state.last_recorded_dot_sample_count += count;
    }
    else
    if (is_area) {
        if (!view_state.rhi->vbo) {
            return;
        }
        QRhiBuffer* const vbo = view_state.rhi->vbo.get();
        for (const builtin_segment_span_t& span : segment_spans) {
            if (span.gpu_first > count ||
                span.gpu_count > count - span.gpu_first)
            {
                return;
            }
            quint32 instance_count = 0;
            quint32 first_offset   = 0;
            quint32 next_offset    = 0;
            if (!detail::to_qrhi_count(span.gpu_count - 1u, instance_count)                      ||
                !detail::qrhi_buffer_offset( span.gpu_first, sizeof(gpu_sample_t), first_offset) ||
                !detail::qrhi_buffer_offset(
                    span.gpu_first + 1u, sizeof(gpu_sample_t), next_offset))
            {
                return;
            }
            const QRhiCommandBuffer::VertexInput inputs[2] = {
                { vbo, first_offset },
                { vbo, next_offset }
            };
            cb->setVertexInput(0, 2, inputs);
            cb->setShaderResources(srb_entry.srb.get());
            cb->draw(6, instance_count);
            ++view_state.last_recorded_area_span_count;
            view_state.last_recorded_area_segment_count += span.gpu_count - 1u;
        }
    }
    else {
        cb->setShaderResources(srb_entry.srb.get());
        // LINE: triangle-strip quads per segment, one instance per segment.
        // The four vertex inputs all reference the line_window_vbo at
        // increasing element offsets so each instance reads a sliding
        // (prev, p0, p1, next) window across the padded sample array.
        if (!view_state.rhi->line_window_vbo) {
            return;
        }
        QRhiBuffer* const win = view_state.rhi->line_window_vbo.get();
        for (const auto& line_span : view_state.line_draw_spans) {
            if (line_span.line_count < 2) {
                continue;
            }
            const std::size_t offset0        = line_span.line_first;
            std::size_t       offset1        = 0;
            std::size_t       offset2        = 0;
            std::size_t       offset3        = 0;
            quint32           qrhi_offset0   = 0;
            quint32           qrhi_offset1   = 0;
            quint32           qrhi_offset2   = 0;
            quint32           qrhi_offset3   = 0;
            quint32           instance_count = 0;
            if (!detail::checked_size_add( line_span.line_first, 1u, offset1)             ||
                !detail::checked_size_add( line_span.line_first, 2u, offset2)             ||
                !detail::checked_size_add( line_span.line_first, 3u, offset3)             ||
                !detail::qrhi_buffer_offset( offset0, sizeof(gpu_sample_t), qrhi_offset0) ||
                !detail::qrhi_buffer_offset( offset1, sizeof(gpu_sample_t), qrhi_offset1) ||
                !detail::qrhi_buffer_offset( offset2, sizeof(gpu_sample_t), qrhi_offset2) ||
                !detail::qrhi_buffer_offset( offset3, sizeof(gpu_sample_t), qrhi_offset3) ||
                !detail::to_qrhi_count( line_span.line_count - 1u, instance_count))
            {
                return;
            }
            const QRhiCommandBuffer::VertexInput inputs[4] = {
                { win, qrhi_offset0 },
                { win, qrhi_offset1 },
                { win, qrhi_offset2 },
                { win, qrhi_offset3 }
            };
            cb->setVertexInput(0, 4, inputs);
            cb->draw(4, instance_count);
            ++view_state.last_recorded_line_span_count;
            view_state.last_recorded_line_segment_count +=
                line_span.line_count - 1u;
        }
    }
}

} // namespace vnm::plot
