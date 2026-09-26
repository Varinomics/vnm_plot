#include <vnm_plot/rhi/primitive_renderer.h>
#include <vnm_plot/core/constants.h>
#include <vnm_plot/core/plot_config.h>
#include "rhi_helpers.h"

#include <glm/gtc/type_ptr.hpp>

#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vnm::plot {
using detail::k_rect_initial_quads;

namespace {

using detail::load_qsb;
using detail::to_int_rounded;
using detail::to_positive_int;

// -----------------------------------------------------------------------------
// std140 UBO mirrors for the QSB chrome shaders.
// -----------------------------------------------------------------------------
//
// generic_rect.vert exposes:
//
//     layout(std140, binding = 0) uniform Block { mat4 pmv; } u;
//
// std140 puts mat4 columns at vec4 stride (16 bytes), giving a 64-byte block.
// The host struct mirrors the column-major buffer glm::value_ptr produces.
struct Rect_block_std140
{
    float pmv[16];  // offset 0
};
static_assert(sizeof(Rect_block_std140) == 64,
    "Rect UBO mirror must be 64 bytes (single mat4)");

// grid_quad.frag exposes:
//
//     layout(std140, binding = 0) uniform Block {
//         vec2  plot_size_px;          // offset 0
//         vec2  region_origin_px;      // offset 8
//         vec4  grid_color;            // offset 16
//         int   v_count;               // offset 32
//         int   t_count;               // offset 36
//         int   framebuffer_y_up;      // offset 40
//         float win_h;                 // offset 44
//         vec4  v_levels[GRID_LEVEL_MAX]; // offset 48
//         vec4  t_levels[GRID_LEVEL_MAX]; // offset 560
//         float lcd_subpixel_order;        // offset 1072
//         vec4  background_color;          // offset 1088
//     } u;
//
// Verified against `qsb --dump shaders/qsb/grid_quad.frag.qsb`.
struct Grid_block_std140
{
    float      plot_size_px[2];     // offset   0
    float      region_origin_px[2]; // offset   8
    float      grid_color[4];       // offset  16
    int32_t    v_count;             // offset  32
    int32_t    t_count;             // offset  36
    int32_t    framebuffer_y_up;    // offset  40
    float      win_h;               // offset  44
    float      v_levels[32][4];     // offset  48 (32 * 16 = 512 bytes)
    float      t_levels[32][4];     // offset 560 (32 * 16 = 512 bytes)
    float      lcd_subpixel_order;  // offset 1072
    float      padding[3];          // offset 1076
    float      background_color[4]; // offset 1088
};
static_assert(offsetof(Grid_block_std140, plot_size_px)     ==    0, "plot_size_px offset");
static_assert(offsetof(Grid_block_std140, region_origin_px) ==    8, "region_origin_px offset");
static_assert(offsetof(Grid_block_std140, grid_color)       ==   16, "grid_color offset");
static_assert(offsetof(Grid_block_std140, v_count)          ==   32, "v_count offset");
static_assert(offsetof(Grid_block_std140, t_count)          ==   36, "t_count offset");
static_assert(offsetof(Grid_block_std140, framebuffer_y_up) ==   40, "framebuffer_y_up offset");
static_assert(offsetof(Grid_block_std140, win_h)            ==   44, "win_h offset");
static_assert(offsetof(Grid_block_std140, v_levels)         ==   48, "v_levels offset");
static_assert(offsetof(Grid_block_std140, t_levels)         ==  560, "t_levels offset");
static_assert(offsetof(Grid_block_std140, lcd_subpixel_order) == 1072, "lcd_subpixel_order offset");
static_assert(offsetof(Grid_block_std140, background_color) == 1088, "background_color offset");
static_assert(sizeof(Grid_block_std140)                     == 1104, "Grid UBO mirror size");

constexpr std::uint32_t k_rect_ubo_bytes = sizeof(Rect_block_std140);
constexpr std::uint32_t k_grid_ubo_bytes = sizeof(Grid_block_std140);

} // namespace

// -----------------------------------------------------------------------------
// rhi_state_t: Cached pipelines, per-frame buffers, and the deferred draw plan.
// -----------------------------------------------------------------------------
//
// The rect pipeline is one cached QRhiGraphicsPipeline shared across every
// rect draw in the frame; pipeline state (vertex layout, blend, sample count)
// only depends on the binding LAYOUT, not on the concrete buffer handles.
// Each flush_rects call allocates a fresh per-call vertex buffer + UBO + SRB
// triple via the renderer's per-frame ring; this keeps lifetimes simple
// because every draw owns its own buffers and the ring is reset each frame.
//
// The grid pipeline mirrors the same shape: one cached QRhiGraphicsPipeline,
// per-call UBO + SRB, full-screen unit quad shared across all draws.

struct Primitive_renderer::rhi_state_t
{
    // Per-call buffers used by exactly one draw op. Owned by rhi_state_t and
    // recycled across frames: the pool grows as the frame's draw count
    // demands and m_used resets to 0 at the top of each prepare phase. The
    // SRB references the per-call UBO directly; rebuilding it whenever the
    // UBO pointer changes (e.g. capacity grew the QRhiBuffer) is the only
    // way to avoid use-after-free, since QRhiShaderResourceBindings captures
    // raw handles at create() time.
    struct rect_call_t
    {
        std::unique_ptr<QRhiBuffer>                 vbo;
        std::size_t                                 vbo_capacity_bytes = 0;
        std::unique_ptr<QRhiBuffer>                 ubo;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        QRhiBuffer*                                 srb_last_ubo = nullptr;
    };

    struct grid_call_t
    {
        std::unique_ptr<QRhiBuffer>                    ubo;
        std::unique_ptr<QRhiShaderResourceBindings>    srb;
        QRhiBuffer*                                    srb_last_ubo = nullptr;
    };

    enum class op_kind_t :                 uint8_t{ RECT, GRID };

    struct draw_op_t
    {
        op_kind_t      kind;
        // Index into rect_calls / grid_calls depending on kind. Stored as
        // size_t because each op references exactly one preallocated call
        // resource; the indices are stable for the duration of the frame.
        std::size_t    resource_index;
        // For RECT: number of instances (== quads) in vbo.
        // For GRID: scissor rectangle in QRhi's bottom-left coordinates.
        union
        {
            struct
            {
                quint32 instance_count;
            } rect;
            struct
            {
                int x, y, w, h;
            } grid;
        };
    };

    std::vector<rect_call_t>               rect_calls;
    std::vector<grid_call_t>               grid_calls;
    std::vector<draw_op_t>                 ops;
    std::size_t                            rect_used = 0;
    std::size_t                            grid_used = 0;
    // Position in `ops` where the next record_draws() call should start
    // playback. Lets the host interleave multiple record_draws() invocations
    // around a series.render() call so chrome paints both behind and in
    // front of the data series in a single frame.
    std::size_t                            record_cursor = 0;

    // Cached pipelines keyed only by primitive kind: the descriptor depends
    // on shader stages, vertex layout, blend, and sample count. Per-call
    // buffer handles ride the SRB on each draw.
    std::unique_ptr<QRhiGraphicsPipeline>  rect_pipeline;
    std::unique_ptr<QRhiGraphicsPipeline>  grid_pipeline;
    QRhiRenderPassDescriptor*              rect_pipeline_rpd     = nullptr;
    int                                    rect_pipeline_samples = 0;
    QRhiRenderPassDescriptor*              grid_pipeline_rpd     = nullptr;
    int                                    grid_pipeline_samples = 0;

    // Static unit-quad VBO consumed by the grid pipeline. Allocated once on
    // first prepare and reused forever; the same four vertices feed every
    // grid draw (the fragment shader resolves region clipping itself).
    std::unique_ptr<QRhiBuffer>            grid_quad_vbo;

    QShader                                rect_vert;
    QShader                                rect_frag;
    QShader                                grid_vert;
    QShader                                grid_frag;
    bool                                   shaders_loaded = false;

    // Last QRhi seen on the prepare path. A backend swap (rare; tests / host
    // teardown) invalidates every cached buffer and pipeline because they
    // belong to the previous QRhi instance.
    QRhi*                                  last_rhi = nullptr;
};

Primitive_renderer::Primitive_renderer()
    : m_rhi_state(std::make_unique<rhi_state_t>())
{}

Primitive_renderer::~Primitive_renderer() = default;

void Primitive_renderer::set_log_callback(std::function<void(const std::string&)> callback)
{
    m_log_error = std::move(callback);
}

void Primitive_renderer::batch_rect(const glm::vec4& color, const glm::vec4& rect_coords)
{
    if (m_cpu_buffer.size() == m_cpu_buffer.capacity()) {
        m_cpu_buffer.reserve(m_cpu_buffer.size() + k_rect_initial_quads);
    }
    m_cpu_buffer.push_back({color, rect_coords});
}

bool Primitive_renderer::rhi_ensure_rect_pipeline(
    rhi_state_t&       rhi_state,
    QRhi*              rhi,
    QRhiRenderTarget*  rt)
{
    QRhiRenderPassDescriptor* rpd = rt->renderPassDescriptor();
    const int samples = rt->sampleCount();

    if (rhi_state.rect_pipeline && (rhi_state.rect_pipeline_rpd != rpd || rhi_state.rect_pipeline_samples != samples))
    {
        rhi_state.rect_pipeline.reset();
    }
    if (rhi_state.rect_pipeline) {
        return true;
    }

    QRhiVertexInputLayout vlayout;
    QRhiVertexInputBinding vb(
        static_cast<quint32>(sizeof(rect_vertex_t)),
        QRhiVertexInputBinding::PerInstance,
        1);
    vlayout.setBindings({vb});
    QRhiVertexInputAttribute a_color(
        0, 0, QRhiVertexInputAttribute::Float4,
        static_cast<quint32>(offsetof(rect_vertex_t, color)));
    QRhiVertexInputAttribute a_rect(
        0, 1, QRhiVertexInputAttribute::Float4,
        static_cast<quint32>(offsetof(rect_vertex_t, rect_coords)));
    vlayout.setAttributes({a_color, a_rect});

    detail::alpha_blended_pipeline_desc_t desc;
    desc.vert               = rhi_state.rect_vert;
    desc.frag               = rhi_state.rect_frag;
    desc.vlayout            = vlayout;
    desc.ubo_bytes          = k_rect_ubo_bytes;
    desc.ubo_stages         = QRhiShaderResourceBinding::VertexStage;
    rhi_state.rect_pipeline = detail::build_alpha_blended_pipeline(rhi, rt, desc);
    if (!rhi_state.rect_pipeline) {
        return false;
    }
    rhi_state.rect_pipeline_rpd     = rpd;
    rhi_state.rect_pipeline_samples = samples;
    return true;
}

bool Primitive_renderer::rhi_ensure_grid_pipeline(
    rhi_state_t&       rhi_state,
    QRhi*              rhi,
    QRhiRenderTarget*  rt)
{
    QRhiRenderPassDescriptor* rpd = rt->renderPassDescriptor();
    const int samples = rt->sampleCount();

    if (rhi_state.grid_pipeline && (rhi_state.grid_pipeline_rpd != rpd || rhi_state.grid_pipeline_samples != samples))
    {
        rhi_state.grid_pipeline.reset();
    }
    if (rhi_state.grid_pipeline) {
        return true;
    }

    QRhiVertexInputLayout vlayout;
    QRhiVertexInputBinding vb(
        2 * sizeof(float), QRhiVertexInputBinding::PerVertex, 1);
    vlayout.setBindings({vb});
    QRhiVertexInputAttribute a_pos(
        0, 0, QRhiVertexInputAttribute::Float2, 0);
    vlayout.setAttributes({a_pos});

    // grid_quad.frag is the only stage that reads the UBO; the .vert just
    // forwards the unit-quad vertex through gl_Position.
    detail::alpha_blended_pipeline_desc_t desc;
    desc.vert               = rhi_state.grid_vert;
    desc.frag               = rhi_state.grid_frag;
    desc.vlayout            = vlayout;
    desc.ubo_bytes          = k_grid_ubo_bytes;
    desc.ubo_stages         = QRhiShaderResourceBinding::FragmentStage;
    desc.flags              = QRhiGraphicsPipeline::UsesScissor;
    rhi_state.grid_pipeline = detail::build_alpha_blended_pipeline(rhi, rt, desc);
    if (!rhi_state.grid_pipeline) {
        return false;
    }
    rhi_state.grid_pipeline_rpd     = rpd;
    rhi_state.grid_pipeline_samples = samples;
    return true;
}

bool Primitive_renderer::rhi_ensure_grid_quad_vbo(
    rhi_state_t&               rhi_state,
    QRhi*                      rhi,
    QRhiResourceUpdateBatch*   updates)
{
    if (rhi_state.grid_quad_vbo) {
        return true;
    }
    static constexpr float k_quad[] = {
        -1.f, -1.f,
         1.f, -1.f,
        -1.f,  1.f,
         1.f,  1.f
    };
    rhi_state.grid_quad_vbo.reset(rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(k_quad)));
    if (!rhi_state.grid_quad_vbo || !rhi_state.grid_quad_vbo->create()) {
        rhi_state.grid_quad_vbo.reset();
        return false;
    }
    if (updates) {
        updates->uploadStaticBuffer(
            rhi_state.grid_quad_vbo.get(), 0, sizeof(k_quad), k_quad);
    }
    return true;
}

// Reset the per-frame plan if the caller hasn't already. A fresh prepare
// phase always starts with no ops; record_draws() leaves the plan empty
// after consuming it. The frame_id-tracked scheme used by Series_renderer is
// overkill here because the plan is bounded and each batched op owns its
// resources outright.
void Primitive_renderer::rhi_reset_frame_plan(rhi_state_t& rhi_state)
{
    rhi_state.ops.clear();
    rhi_state.rect_used     = 0;
    rhi_state.grid_used     = 0;
    rhi_state.record_cursor = 0;
}

void Primitive_renderer::rhi_on_backend_change(rhi_state_t& rhi_state, QRhi* rhi)
{
    if (rhi_state.last_rhi == rhi) {
        return;
    }
    // Backend swap: every cached resource belongs to the previous QRhi and
    // is no longer valid. Drop the lot and rebuild lazily on the next call.
    rhi_state.rect_calls.clear();
    rhi_state.grid_calls.clear();
    rhi_state.ops.clear();
    rhi_state.rect_used = 0;
    rhi_state.grid_used = 0;
    rhi_state.rect_pipeline.reset();
    rhi_state.grid_pipeline.reset();
    rhi_state.rect_pipeline_rpd = nullptr;
    rhi_state.grid_pipeline_rpd = nullptr;
    rhi_state.grid_quad_vbo.reset();
    rhi_state.last_rhi = rhi;
}

void Primitive_renderer::flush_rects(const frame_context_t& ctx, const glm::mat4& pmv)
{
    VNM_PLOT_PROFILE_SCOPE(m_profiler, "renderer.frame.prims.flush_rects");

    if (m_cpu_buffer.empty()) {
        return;
    }

    if (ctx.rhi && ctx.rhi_updates && ctx.render_target) {
        QRhi* rhi = ctx.rhi;
        QRhiResourceUpdateBatch* updates = ctx.rhi_updates;

        rhi_on_backend_change(*m_rhi_state, rhi);

        if (!m_rhi_state->shaders_loaded) {
            m_rhi_state->rect_vert      = load_qsb("generic_rect.vert.qsb");
            m_rhi_state->rect_frag      = load_qsb("generic_rect.frag.qsb");
            m_rhi_state->grid_vert      = load_qsb("grid_quad.vert.qsb");
            m_rhi_state->grid_frag      = load_qsb("grid_quad.frag.qsb");
            m_rhi_state->shaders_loaded = true;
        }

        if (!rhi_ensure_rect_pipeline(*m_rhi_state, rhi, ctx.render_target)) {
            m_cpu_buffer.clear();
            return;
        }

        std::size_t bytes_needed   = 0;
        quint32     upload_bytes   = 0;
        quint32     instance_count = 0;
        if (!detail::qrhi_byte_size(
                m_cpu_buffer.size(), sizeof(rect_vertex_t), bytes_needed, upload_bytes) ||
            !detail::to_qrhi_count(m_cpu_buffer.size(), instance_count))
        {
            m_cpu_buffer.clear();
            return;
        }

        // Acquire the next per-call resource slot. The pool grows on demand
        // and is recycled across frames; rect_used is bumped here and reset
        // by record_draws().
        if (m_rhi_state->rect_used == m_rhi_state->rect_calls.size()) {
            m_rhi_state->rect_calls.emplace_back();
        }
        const std::size_t slot = m_rhi_state->rect_used++;
        auto& call = m_rhi_state->rect_calls[slot];

        if (!call.vbo || call.vbo_capacity_bytes < bytes_needed) {
            // Headroom of 25% on grow keeps reallocations rare across frames
            // where the rect count drifts slightly. The grow reseats call.vbo,
            // which invalidates the SRB's last_ubo handle (UBO is unchanged),
            // and forces ensure-* to upload fresh contents.
            std::size_t alloc      = 0;
            quint32     qrhi_alloc = 0;
            if (!detail::qrhi_grown_capacity_bytes(
                    bytes_needed, alloc, qrhi_alloc))
            {
                m_cpu_buffer.clear();
                m_rhi_state->rect_used--;
                return;
            }
            call.vbo.reset(rhi->newBuffer(
                QRhiBuffer::Static, QRhiBuffer::VertexBuffer,
                qrhi_alloc));
            if (!call.vbo || !call.vbo->create()) {
                call.vbo.reset();
                m_cpu_buffer.clear();
                m_rhi_state->rect_used--;
                return;
            }
            call.vbo_capacity_bytes = alloc;
        }
        if (!call.ubo) {
            call.ubo.reset(rhi->newBuffer(
                QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, k_rect_ubo_bytes));
            if (!call.ubo || !call.ubo->create()) {
                call.ubo.reset();
                m_cpu_buffer.clear();
                m_rhi_state->rect_used--;
                return;
            }
        }
        if (!call.srb || call.srb_last_ubo != call.ubo.get()) {
            if (!detail::rebuild_single_ubo_srb(
                    rhi, call.srb, call.ubo.get(), k_rect_ubo_bytes, QRhiShaderResourceBinding::VertexStage))
            {
                m_cpu_buffer.clear();
                m_rhi_state->rect_used--;
                return;
            }
            call.srb_last_ubo = call.ubo.get();
        }

        updates->uploadStaticBuffer(
            call.vbo.get(), 0,
            upload_bytes,
            m_cpu_buffer.data());

        Rect_block_std140 block{};
        std::memcpy(block.pmv, glm::value_ptr(pmv), sizeof(block.pmv));
        updates->updateDynamicBuffer(call.ubo.get(), 0, sizeof(block), &block);

        rhi_state_t::draw_op_t op{};
        op.kind                = rhi_state_t::op_kind_t::RECT;
        op.resource_index      = slot;
        op.rect.instance_count = instance_count;
        m_rhi_state->ops.push_back(op);

        m_cpu_buffer.clear();
        return;
    }

}

void Primitive_renderer::draw_grid_shader(
    const frame_context_t&     ctx,
    const glm::vec2&           origin,
    const glm::vec2&           size,
    const glm::vec4&           color,
    const grid_layer_params_t& vertical_levels,
    const grid_layer_params_t& horizontal_levels,
    lcd_subpixel_order_t        vertical_subpixel_order,
    const glm::vec4&            background_color)
{
    VNM_PLOT_PROFILE_SCOPE(m_profiler, "renderer.frame.prims.draw_grid");

    if (vertical_levels.count <= 0 && horizontal_levels.count <= 0) {
        return;
    }

    if (ctx.rhi && ctx.rhi_updates && ctx.render_target) {
        QRhi* rhi = ctx.rhi;
        QRhiResourceUpdateBatch* updates = ctx.rhi_updates;

        rhi_on_backend_change(*m_rhi_state, rhi);

        if (!m_rhi_state->shaders_loaded) {
            m_rhi_state->rect_vert      = load_qsb("generic_rect.vert.qsb");
            m_rhi_state->rect_frag      = load_qsb("generic_rect.frag.qsb");
            m_rhi_state->grid_vert      = load_qsb("grid_quad.vert.qsb");
            m_rhi_state->grid_frag      = load_qsb("grid_quad.frag.qsb");
            m_rhi_state->shaders_loaded = true;
        }

        if (!rhi_ensure_grid_pipeline(*m_rhi_state, rhi, ctx.render_target)) {
            return;
        }
        if (!rhi_ensure_grid_quad_vbo(*m_rhi_state, rhi, updates)) {
            return;
        }

        // Chrome passes region coordinates in framebuffer bottom-left origin.
        // QRhiScissor also wants bottom-left, while the fragment shader works
        // in the plot's top-left pixel convention.
        int sx = 0;
        int sy = 0;
        int sw = 0;
        int sh = 0;
        {
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;
            if (!to_int_rounded(origin.x, x)  ||
                !to_int_rounded(origin.y, y)  ||
                !to_positive_int(size.x,   w) ||
                !to_positive_int(size.y,   h))
            {
                return;
            }
            sx = x;
            sy = y;
            sw = w;
            sh = h;
        }

        if (m_rhi_state->grid_used == m_rhi_state->grid_calls.size()) {
            m_rhi_state->grid_calls.emplace_back();
        }
        const std::size_t slot = m_rhi_state->grid_used++;
        auto& call = m_rhi_state->grid_calls[slot];

        if (!call.ubo) {
            call.ubo.reset(rhi->newBuffer(
                QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, k_grid_ubo_bytes));
            if (!call.ubo || !call.ubo->create()) {
                call.ubo.reset();
                m_rhi_state->grid_used--;
                return;
            }
        }
        if (!call.srb || call.srb_last_ubo != call.ubo.get()) {
            if (!detail::rebuild_single_ubo_srb(
                    rhi, call.srb, call.ubo.get(), k_grid_ubo_bytes, QRhiShaderResourceBinding::FragmentStage))
            {
                m_rhi_state->grid_used--;
                return;
            }
            call.srb_last_ubo = call.ubo.get();
        }

        Grid_block_std140 block{};
        block.plot_size_px[0]     = size.x;
        block.plot_size_px[1]     = size.y;
        block.region_origin_px[0] = origin.x;
        block.region_origin_px[1] =
            static_cast<float>(ctx.win_h) - (origin.y + size.y);
        block.grid_color[0]    = color.r;
        block.grid_color[1]    = color.g;
        block.grid_color[2]    = color.b;
        block.grid_color[3]    = color.a;
        block.v_count          = std::min(vertical_levels.count,   grid_layer_params_t::k_max_levels);
        block.t_count          = std::min(horizontal_levels.count, grid_layer_params_t::k_max_levels);
        block.framebuffer_y_up = rhi->isYUpInFramebuffer() ? 1 : 0;
        block.win_h            = static_cast<float>(ctx.win_h);
        for (int i = 0; i < block.v_count; ++i) {
            block.v_levels[i][0] = vertical_levels.spacing_px[i];
            block.v_levels[i][1] = size.y - vertical_levels.start_px[i];
            block.v_levels[i][2] = vertical_levels.alpha[i];
            block.v_levels[i][3] = vertical_levels.thickness_px[i];
        }
        for (int i = 0; i < block.t_count; ++i) {
            block.t_levels[i][0] = horizontal_levels.spacing_px[i];
            block.t_levels[i][1] = horizontal_levels.start_px[i];
            block.t_levels[i][2] = horizontal_levels.alpha[i];
            block.t_levels[i][3] = horizontal_levels.thickness_px[i];
        }
        block.lcd_subpixel_order =
            vnm::msdf_text::lcd::shader_uniform_value(vertical_subpixel_order);
        block.background_color[0] = background_color.r;
        block.background_color[1] = background_color.g;
        block.background_color[2] = background_color.b;
        block.background_color[3] = background_color.a;

        updates->updateDynamicBuffer(call.ubo.get(), 0, sizeof(block), &block);

        rhi_state_t::draw_op_t op{};
        op.kind           = rhi_state_t::op_kind_t::GRID;
        op.resource_index = slot;
        op.grid.x         = sx;
        op.grid.y         = sy;
        op.grid.w         = sw;
        op.grid.h         = sh;
        m_rhi_state->ops.push_back(op);
        return;
    }

}

std::size_t Primitive_renderer::queued_op_count() const
{
    return m_rhi_state->ops.size();
}

void Primitive_renderer::record_draws(
    [[maybe_unused]] const frame_context_t&    ctx,
    [[maybe_unused]] std::size_t               end)
{
    if (!ctx.rhi || !ctx.cb) {
        return;
    }
    QRhiCommandBuffer* cb = ctx.cb;

    // Slice play: replay ops [cursor, end). The host advances `end` across
    // multiple calls so it can interleave chrome draws (back layer / front
    // layer) around the series pass and keep depth order stable. A clamped
    // `end` past the queue size protects against a
    // stale checkpoint after a chrome op was rejected mid-frame.
    const std::size_t cursor = m_rhi_state->record_cursor;
    if (end > m_rhi_state->ops.size()) {
        end = m_rhi_state->ops.size();
    }
    for (std::size_t i = cursor; i < end; ++i) {
        const auto& op = m_rhi_state->ops[i];
        switch (op.kind) {
            case rhi_state_t::op_kind_t::RECT: {
                auto& call = m_rhi_state->rect_calls[op.resource_index];
                if (!call.vbo || !call.srb) {
                    continue;
                }
                cb->setGraphicsPipeline(m_rhi_state->rect_pipeline.get());
                cb->setShaderResources(call.srb.get());
                QRhiCommandBuffer::VertexInput vi{call.vbo.get(), 0u};
                cb->setVertexInput(0, 1, &vi);
                cb->draw(4, op.rect.instance_count);
                break;
            }
            case rhi_state_t::op_kind_t::GRID: {
                auto& call = m_rhi_state->grid_calls[op.resource_index];
                if (!call.srb || !m_rhi_state->grid_quad_vbo) {
                    continue;
                }
                cb->setGraphicsPipeline(m_rhi_state->grid_pipeline.get());
                cb->setShaderResources(call.srb.get());
                QRhiCommandBuffer::VertexInput vi{m_rhi_state->grid_quad_vbo.get(), 0u};
                cb->setVertexInput(0, 1, &vi);
                cb->setScissor(QRhiScissor(op.grid.x, op.grid.y, op.grid.w, op.grid.h));
                cb->draw(4);
                break;
            }
        }
    }
    m_rhi_state->record_cursor = end;
}

void Primitive_renderer::reset_frame()
{
    rhi_reset_frame_plan(*m_rhi_state);
}

} // namespace vnm::plot
