#pragma once

#include "checked_size.h"

#include <vnm_plot/core/time_units.h>

#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace vnm::plot::detail {

inline QShader load_qsb(const char* alias)
{
    QFile file(QStringLiteral(":/vnm_plot/shaders/qsb/") + QString::fromLatin1(alias));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

inline bool to_int_rounded(double value, int& out)
{
    if (!std::isfinite(value)) {
        return false;
    }

    const double rounded = std::round(value);
    if (rounded < static_cast<double>(std::numeric_limits<int>::min()) ||
        rounded > static_cast<double>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    out = static_cast<int>(rounded);
    return true;
}

inline bool to_positive_int(double value, int& out)
{
    if (!std::isfinite(value)) {
        return false;
    }

    const double rounded = std::round(value);
    if (rounded <= 0.0 ||
        rounded >  static_cast<double>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    out = static_cast<int>(rounded);
    return true;
}

// fp32 GPU time rebasing: timestamps live as int64 nanoseconds but are
// uploaded as fp32 seconds relative to a per-view origin. choose_origin_ns
// keeps the rebased values inside fp32's usable mantissa range; this helper
// just names the cast so the math doesn't get rewritten differently each
// time it appears in the renderer.
inline float to_view_seconds(std::int64_t ts_ns, std::int64_t origin_ns)
{
    return static_cast<float>(
        span_ns_as_long_double(origin_ns, ts_ns) * 1.0e-9L);
}

inline bool to_qrhi_u32(std::size_t value, quint32& out)
{
    if (value > static_cast<std::size_t>(std::numeric_limits<quint32>::max())) {
        return false;
    }
    out = static_cast<quint32>(value);
    return true;
}

inline bool to_qrhi_count(std::size_t count, quint32& out)
{
    return to_qrhi_u32(count, out);
}

inline bool to_qrhi_byte_count(std::size_t bytes, quint32& out)
{
    return to_qrhi_u32(bytes, out);
}

inline bool qrhi_byte_size(
    std::size_t    element_count,
    std::size_t    element_bytes,
    std::size_t&   out_bytes,
    quint32&       out_qrhi_bytes)
{
    std::size_t bytes = 0;
    if (!checked_size_product(element_count, element_bytes, bytes) ||
        !to_qrhi_byte_count(bytes, out_qrhi_bytes))
    {
        return false;
    }
    out_bytes = bytes;
    return true;
}

inline bool qrhi_byte_size(
    std::size_t    element_count,
    std::size_t    element_bytes,
    quint32&       out_qrhi_bytes)
{
    std::size_t bytes = 0;
    return qrhi_byte_size(element_count, element_bytes, bytes, out_qrhi_bytes);
}

inline bool qrhi_buffer_offset(
    std::size_t    element_index,
    std::size_t    element_bytes,
    quint32&       out_offset)
{
    return qrhi_byte_size(element_index, element_bytes, out_offset);
}

inline bool qrhi_grown_capacity_bytes(
    std::size_t    bytes_needed,
    std::size_t&   out_capacity_bytes,
    quint32&       out_qrhi_capacity_bytes)
{
    if (!to_qrhi_byte_count(bytes_needed, out_qrhi_capacity_bytes)) {
        return false;
    }

    const std::size_t max_qrhi_bytes =
        static_cast<std::size_t>(std::numeric_limits<quint32>::max());
    const std::size_t headroom       = bytes_needed / 4u;
    std::size_t       capacity_bytes = 0;
    if (!checked_size_add(bytes_needed, headroom, capacity_bytes) ||
        capacity_bytes > max_qrhi_bytes)
    {
        capacity_bytes = max_qrhi_bytes;
    }

    if (!to_qrhi_byte_count(capacity_bytes, out_qrhi_capacity_bytes)) {
        return false;
    }
    out_capacity_bytes = capacity_bytes;
    return true;
}

// Rebuilds an SRB that contains a single uniform buffer binding at slot 0.
// Replaces the ~6-line "newShaderResourceBindings + setBindings + create"
// dance that recurred across primitive/grid/series renderers.
inline bool rebuild_single_ubo_srb(
    QRhi*                                          rhi,
    std::unique_ptr<QRhiShaderResourceBindings>&   srb,
    QRhiBuffer*                                    ubo,
    quint32                                        ubo_bytes,
    QRhiShaderResourceBinding::StageFlags          stages)
{
    srb.reset(rhi->newShaderResourceBindings());
    if (!srb) {
        return false;
    }
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo, 0, ubo_bytes)
    });
    return srb->create();
}

// Grows / lazily creates a Dynamic UBO so it can hold at least `bytes_needed`
// bytes. Returns true if the buffer is ready; on failure the unique_ptr is
// reset to null so callers can early-out cleanly.
inline bool ensure_dynamic_ubo(
    QRhi*                          rhi,
    std::unique_ptr<QRhiBuffer>&   ubo,
    std::size_t&                   capacity_bytes,
    quint32                        bytes_needed)
{
    if (ubo && capacity_bytes >= bytes_needed) {
        return true;
    }
    ubo.reset(rhi->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, bytes_needed));
    if (!ubo || !ubo->create()) {
        ubo.reset();
        return false;
    }
    capacity_bytes = bytes_needed;
    return true;
}

// Description of a graphics pipeline that uses the canonical vnm_plot blend
// (alpha-over) and Triangle-strip topology. The vertex input layout is
// passed in pre-built because rect/grid/series/dots/line/area each need a
// distinct binding+attribute set.
struct alpha_blended_pipeline_desc_t
{
    QShader                vert;
    QShader                frag;
    QRhiVertexInputLayout  vlayout;
    quint32                ubo_bytes = 0;
    QRhiShaderResourceBinding::StageFlags ubo_stages =
        QRhiShaderResourceBinding::VertexStage;
    QRhiGraphicsPipeline::Flags    flags = {};
};

// Builds a graphics pipeline with the standard vnm_plot alpha blend and a
// layout-only SRB containing a single UBO at slot 0. The SRB and the
// throwaway UBO it references live only long enough for the pipeline's
// create() call; Qt validates layout, not handles, at create-time, so the
// caller is free to bind a per-draw SRB with a different UBO handle
// (matching layout) at draw time.
inline std::unique_ptr<QRhiGraphicsPipeline> build_alpha_blended_pipeline(
    QRhi*                                  rhi,
    QRhiRenderTarget*                      rt,
    const alpha_blended_pipeline_desc_t&   desc)
{
    if (!rhi || !rt || !desc.vert.isValid() || !desc.frag.isValid()) {
        return nullptr;
    }

    auto layout_ubo = std::unique_ptr<QRhiBuffer>(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, desc.ubo_bytes));
    if (!layout_ubo || !layout_ubo->create()) {
        return nullptr;
    }

    std::unique_ptr<QRhiShaderResourceBindings> layout_srb;
    if (!rebuild_single_ubo_srb(
            rhi, layout_srb, layout_ubo.get(), desc.ubo_bytes, desc.ubo_stages))
    {
        return nullptr;
    }

    auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(
        rhi->newGraphicsPipeline());
    if (!pipeline) {
        return nullptr;
    }
    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, desc.vert },
        { QRhiShaderStage::Fragment, desc.frag }
    });
    pipeline->setVertexInputLayout(desc.vlayout);
    pipeline->setShaderResourceBindings(layout_srb.get());
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);

    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable   = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    pipeline->setTargetBlends({blend});
    if (desc.flags) {
        pipeline->setFlags(desc.flags);
    }
    pipeline->setRenderPassDescriptor(rt->renderPassDescriptor());
    pipeline->setSampleCount(rt->sampleCount());

    if (!pipeline->create()) {
        return nullptr;
    }
    return pipeline;
}

} // namespace vnm::plot::detail
