#include "plot_renderer.h"
#include "plot_frame_renderer.h"
#include "lcd_resolver.h"
#include "plot_render_feedback.h"
#include <vnm_plot/qt/plot_widget.h>

#include <QColor>
#include <QQuickWindow>
#include <glm/glm.hpp>
#include <rhi/qrhi.h>

#include <chrono>
#include <memory>
#include <shared_mutex>
#include <utility>

namespace vnm::plot {

namespace {

glm::vec4 qcolor_to_vec4(const QColor& color)
{
    return
        glm::vec4(
            static_cast<float>(color.redF()),
            static_cast<float>(color.greenF()),
            static_cast<float>(color.blueF()),
            static_cast<float>(color.alphaF()));
}

} // namespace

struct Plot_renderer::impl_t
{
    detail::Plot_render_snapshot snapshot;
    detail::Plot_frame_renderer  frame_renderer;
    std::shared_ptr<detail::plot_render_feedback_channel_t> feedback_channel =
        std::make_shared<detail::plot_render_feedback_channel_t>();
    std::chrono::steady_clock::time_point last_render_callback;
};

Plot_renderer::Plot_renderer()

:
    m_impl(std::make_unique<impl_t>())
{}

Plot_renderer::~Plot_renderer() = default;

void Plot_renderer::initialize(QRhiCommandBuffer* /*cb*/)
{
    m_impl->frame_renderer.initialize();
}

// The item is a live Plot_widget, so the shared_locks below are taken on live
// mutexes. Qt reaches here only through QQuickRhiItemNode::sync(), whose sole
// caller is QQuickRhiItem::updatePaintNode(), which
// QQuickWindowPrivate::updateDirtyNode() invokes during the scene-graph sync
// phase for items on the window's dirty list. A destroyed item can no longer be
// dispatched from there: ~QQuickItem calls derefWindow(), whose
// removeFromDirtyList() takes the item off that list before ~QObject runs.
// Destruction and the sync phase also never interleave - the threaded loop
// parks the GUI thread in polishAndSync() for the duration of
// syncSceneGraph(), while the basic loop and QQuickRenderControl run sync on
// the same thread that would be running the destructor. Qt itself relies on
// the same fact: sync() dereferences the item - d_func(), width(), height(),
// effectiveColorBufferSizeChanged() - before handing it over. The item is
// never null either: the node keeps the pointer it was constructed with in
// updatePaintNode (new QQuickRhiItemNode(this)) and never reassigns it, and
// the only other entry is a direct call from a test, which passes a live
// widget.
//
// This reasoning covers synchronize() only. The node itself outlives the item:
// derefWindow() hands the item's node subtree to
// QQuickWindowPrivate::cleanup(), which destroys it only at the next sync, in
// the cleanupNodes() that heads updateDirtyNodes(). Until then render() can
// still run for a destroyed widget, which is why it reads no widget state and
// publishes through the shared feedback channel instead.
void Plot_renderer::synchronize(QQuickRhiItem* item)
{
    auto* widget = static_cast<Plot_widget*>(item);

    widget->arm_render_feedback_delivery(m_impl->feedback_channel);

    {
        std::shared_lock lock(widget->m_config_mutex);
        m_impl->snapshot.config = widget->m_config;
    }
    {
        std::shared_lock lock(widget->m_data_cfg_mutex);
        m_impl->snapshot.data_cfg = widget->m_data_cfg;
    }
    {
        std::shared_lock lock(widget->m_series_mutex);
        m_impl->snapshot.series          = widget->m_series;
        m_impl->snapshot.series_revision = widget->series_revision();
    }
    m_impl->snapshot.v_auto = widget->m_v_auto.load(std::memory_order_acquire);
    m_impl->snapshot.visible_info_flags =
        widget->m_visible_info_flags.load(std::memory_order_acquire);
    m_impl->snapshot.adjusted_font_px        = widget->m_adjusted_font_size;
    m_impl->snapshot.base_label_height_px    = widget->m_base_label_height;
    m_impl->snapshot.adjusted_preview_height = widget->m_preview_height * widget->m_scaling_factor;
    m_impl->snapshot.vbar_width_pixels       = widget->vbar_width_pixels();
    if (QQuickWindow* window = widget->window()) {
        m_impl->snapshot.window_background = qcolor_to_vec4(window->color());
    }
    // Only AUTO needs platform probing here. The core renderers combine this
    // with the request again so direct-RHI explicit requests need no
    // prefilled frame order.
    m_impl->snapshot.auto_lcd_subpixel_order =
        m_impl->snapshot.config.lcd_request.automatic
            ? resolve_lcd_subpixel_order_for_window(
                m_impl->snapshot.config.lcd_request,
                widget->window())
            : lcd_subpixel_order_t::NONE;
    m_impl->snapshot.config_revision = widget->m_config_revision.load(std::memory_order_acquire);
}

void Plot_renderer::render(QRhiCommandBuffer* cb)
{
    QRhiRenderTarget* const rt = renderTarget();
    if (!cb || !rt) {
        return;
    }
    QRhi* const rhi_ptr = rhi();

    Profiler* profiler = m_impl->snapshot.config.profiler.get();
    const auto           callback_now = std::chrono::steady_clock::now();
    if (profiler && m_impl->last_render_callback.time_since_epoch().count() != 0) {
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            callback_now - m_impl->last_render_callback
        ).count();
        profiler->record_observation("qrhi.renderer.callback_interval", elapsed_ms);
    }
    m_impl->last_render_callback = callback_now;

    auto result = m_impl->frame_renderer.render(m_impl->snapshot, rhi_ptr, rt, cb);
    if (result.needs_update) {
        update();
    }
    m_impl->feedback_channel->publish(std::move(result.feedback));
}

} // namespace vnm::plot
