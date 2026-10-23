// vnm_plot QQuickRhiItem renderer ownership tests

#include "test_macros.h"
#include "plot_render_feedback.h"
#include "plot_renderer.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/qt/plot_widget.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickRhiItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSize>
#include <QThread>
#include <rhi/qrhi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <type_traits>
#include <vector>

namespace plot = vnm::plot;

namespace {

static_assert(std::is_default_constructible_v<plot::Plot_renderer>);
static_assert(!std::is_constructible_v<plot::Plot_renderer, const plot::Plot_widget*>);

constexpr int k_scene_graph_timeout_ms = 10000;

// Every field is written and read from the one thread that drives the frames,
// so none of it needs to be atomic.
struct destruction_state_t
{
    bool feedback_visible_at_destruction = false;
    bool renderer_only_frame_completed   = false;
    int  render_callbacks                = 0;
};

class test_renderer_t final : public plot::Plot_renderer
{
public:
    using plot::Plot_renderer::synchronize;
};

class test_widget_t : public plot::Plot_widget
{
public:
    test_widget_t() = default;

    explicit test_widget_t(std::shared_ptr<destruction_state_t> destruction_state)
        : m_destruction_state(std::move(destruction_state))
    {}

    ~test_widget_t() override
    {
        if (!m_destruction_state) {
            return;
        }

        float  v_min = 0.0f;
        float  v_max = 0.0f;
        qint64 t_min = 0;
        qint64 t_max = 0;
        m_destruction_state->feedback_visible_at_destruction =
            rendered_v_range(v_min, v_max) || rendered_t_range(t_min, t_max);
    }

    using plot::Plot_widget::createRenderer;

    bool rendered_feedback(
        float&  v_min,
        float&  v_max,
        qint64& t_min,
        qint64& t_max) const
    {
        return
            rendered_v_range(v_min, v_max) &&
            rendered_t_range(t_min, t_max);
    }

private:
    std::shared_ptr<destruction_state_t> m_destruction_state;
};

// Records each renderer pass and, from the first one, asks for one more. That
// request is what makes the follow-up pass a renderer-only frame: update()
// re-arms the scene-graph node without the item contributing a synchronize().
class recording_renderer_t final : public plot::Plot_renderer
{
public:
    explicit recording_renderer_t(std::shared_ptr<destruction_state_t> state)
        : m_state(std::move(state))
    {}

protected:
    void render(QRhiCommandBuffer* cb) override
    {
        const int callback = ++m_state->render_callbacks;

        plot::Plot_renderer::render(cb);

        if (callback == 1) {
            update();
        }
        else if (callback == 2) {
            m_state->renderer_only_frame_completed = true;
        }
    }

private:
    std::shared_ptr<destruction_state_t> m_state;
};

class recording_widget_t final : public test_widget_t
{
public:
    explicit recording_widget_t(std::shared_ptr<destruction_state_t> state)
        :
        test_widget_t(state),
        m_state(std::move(state))
    {}

    QQuickRhiItemRenderer* createRenderer() override
    {
        return new recording_renderer_t(m_state);
    }

private:
    std::shared_ptr<destruction_state_t> m_state;
};

bool wait_until(const std::function<bool()>& predicate, int timeout_ms)
{
    QElapsedTimer elapsed;
    elapsed.start();

    while (!predicate() && elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

struct rhi_probe_t
{
    std::atomic<bool> answered{false};
    std::atomic<bool> has_rhi{false};
};

// Records whether this window's scene graph came up with a QRhi. It is the same
// interrogation QQuickRhiItemNode::sync() performs before it declares that the
// item "will not be functional", so it names the exact capability the
// renderer-backed cases depend on - never the platform they happen to run on.
// The query runs on the scene-graph thread, where the resource pointer is the
// one the renderer will use, and the state is shared so the connection cannot
// outlive what it writes into.
std::shared_ptr<rhi_probe_t> probe_window_rhi(QQuickWindow& window)
{
    auto probe = std::make_shared<rhi_probe_t>();

    QObject::connect(
        &window,
        &QQuickWindow::sceneGraphInitialized,
        &window,
        [probe, &window] {
            QSGRendererInterface* renderer_interface = window.rendererInterface();
            probe->has_rhi.store(
                renderer_interface != nullptr &&
                    renderer_interface->getResource(
                        &window,
                        QSGRendererInterface::RhiResource) != nullptr,
                std::memory_order_release);
            probe->answered.store(true, std::memory_order_release);
        },
        Qt::DirectConnection);

    return probe;
}

// A scene graph that never answers, and one that answers without a QRhi, are
// the same fact for a QQuickRhiItem: no renderer callback will ever run, so its
// behavior is unobservable here rather than wrong.
bool window_provides_rhi(const std::shared_ptr<rhi_probe_t>& probe)
{
    const bool answered = wait_until(
        [&] { return probe->answered.load(std::memory_order_acquire); },
        k_scene_graph_timeout_ms);

    return answered && probe->has_rhi.load(std::memory_order_acquire);
}

void configure_static_widget(test_widget_t& widget, QQuickWindow& window)
{
    widget.setParentItem(window.contentItem());
    widget.setWidth(window.width());
    widget.setHeight(window.height());
    widget.set_v_auto(false);
    widget.set_v_range(-4.0f, 9.0f);
    widget.set_available_t_range(1000, 9000);
    widget.set_t_range(2000, 7000);
}

// A QQuickRenderControl window has no swapchain, so the caller owns the color
// buffer the scene graph draws into. These objects belong to the render
// control's QRhi and must therefore be released before it is.
class offscreen_render_target_t
{
public:
    bool create(QRhi& rhi, const QSize& size)
    {
        m_color.reset(rhi.newTexture(
            QRhiTexture::RGBA8,
            size,
            1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_color->create()) {
            return false;
        }

        m_depth_stencil.reset(rhi.newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        if (!m_depth_stencil->create()) {
            return false;
        }

        QRhiTextureRenderTargetDescription description(QRhiColorAttachment(m_color.get()));
        description.setDepthStencilBuffer(m_depth_stencil.get());

        m_target.reset(rhi.newTextureRenderTarget(description));
        m_render_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_render_pass.get());
        return m_target->create();
    }

    QQuickRenderTarget quick_render_target() const
    {
        return QQuickRenderTarget::fromRhiRenderTarget(m_target.get());
    }

private:
    std::unique_ptr<QRhiTexture>              m_color;
    std::unique_ptr<QRhiRenderBuffer>         m_depth_stencil;
    std::unique_ptr<QRhiTextureRenderTarget>  m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_render_pass;
};

bool test_feedback_channel_respects_completed_frame_boundary()
{
    auto channel = std::make_shared<plot::detail::plot_render_feedback_channel_t>();

    plot::detail::plot_render_feedback_t first;
    first.v_min = 11.0f;
    channel->publish(std::move(first));
    channel->mark_latest_completed();

    plot::detail::plot_render_feedback_t second;
    second.v_min = 22.0f;
    channel->publish(std::move(second));

    auto completed = plot::detail::take_completed_feedback(channel, channel, 0);
    TEST_ASSERT(completed && completed->generation == 1 && completed->v_min == 11.0f,
        "frame N delivery must consume frame N's completed generation");

    completed = plot::detail::take_completed_feedback(channel, channel, 1);
    TEST_ASSERT(!completed,
        "published frame N+1 feedback must wait for its own completion marker");

    channel->mark_latest_completed();
    completed = plot::detail::take_completed_feedback(channel, channel, 1);
    TEST_ASSERT(completed && completed->generation == 2 && completed->v_min == 22.0f,
        "frame N+1 feedback should become consumable after its frame boundary");
    return true;
}

bool test_replaced_feedback_channel_ignores_old_callback()
{
    auto old_channel =
        std::make_shared<plot::detail::plot_render_feedback_channel_t>();
    auto current_channel =
        std::make_shared<plot::detail::plot_render_feedback_channel_t>();

    plot::detail::plot_render_feedback_t old_feedback;
    old_feedback.v_min = 17.0f;
    old_channel->publish(std::move(old_feedback));
    old_channel->mark_latest_completed();

    auto completed = plot::detail::take_completed_feedback(
        current_channel,
        old_channel,
        0);
    TEST_ASSERT(!completed,
        "an old renderer callback must not consume a replacement channel");

    completed = plot::detail::take_completed_feedback(old_channel, old_channel, 0);
    TEST_ASSERT(completed && completed->generation == 1 && completed->v_min == 17.0f,
        "rejecting an old callback must leave its channel generation untouched");
    return true;
}

bool test_renderer_outlives_synchronized_widget()
{
    auto renderer = std::make_unique<test_renderer_t>();

    {
        auto widget = std::make_unique<plot::Plot_widget>();
        renderer->synchronize(widget.get());
    }

    renderer.reset();
    return true;
}

bool test_created_renderer_has_independent_lifetime()
{
    std::unique_ptr<QQuickRhiItemRenderer> renderer;

    {
        test_widget_t widget;
        renderer.reset(widget.createRenderer());
        TEST_ASSERT(renderer, "Plot_widget should create a renderer");
    }

    renderer.reset();
    return true;
}

bool test_static_first_frame_delivers_feedback(bool& unsupported_configuration)
{
    QQuickWindow window;
    window.resize(480, 320);
    auto probe = probe_window_rhi(window);

    auto widget = std::make_unique<test_widget_t>();
    configure_static_widget(*widget, window);
    window.show();

    if (!window_provides_rhi(probe)) {
        unsupported_configuration = true;
        window.close();
        return false;
    }

    float  v_min = 0.0f;
    float  v_max = 0.0f;
    qint64 t_min = 0;
    qint64 t_max = 0;
    const bool delivered = wait_until(
        [&] {
            return widget->rendered_feedback(v_min, v_max, t_min, t_max);
        },
        k_scene_graph_timeout_ms);

    TEST_ASSERT(delivered,
        "a static item's first frame should deliver render feedback");
    TEST_ASSERT(v_min == -4.0f && v_max == 9.0f,
        "first-frame feedback should preserve the rendered manual value range");
    TEST_ASSERT(t_min == 2000 && t_max == 7000,
        "first-frame feedback should preserve the rendered time range");

    widget.reset();
    window.close();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return true;
}

// A render loop owns the order of synchronize() and render(), and the two run
// on different threads, so a case that needs the item destroyed between them
// could only pin that order by parking one thread until the other arrives.
// polishAndSync already parks the GUI thread on the render thread, so such a
// rendezvous can close a cycle that nothing but a timeout breaks. A
// QQuickRenderControl issues polishItems / sync / render from the calling
// thread, which turns the order this case needs into a plain statement
// sequence with nothing to wait for.
bool test_destroy_before_renderer_only_frame_cancels_feedback_delivery(
    bool& unsupported_configuration)
{
    const QSize size(480, 320);

    QQuickRenderControl       control;
    QQuickWindow              window(&control);
    offscreen_render_target_t render_target;

    window.setGeometry(0, 0, size.width(), size.height());
    window.contentItem()->setSize(QSizeF(size));

    if (!control.initialize()) {
        unsupported_configuration = true;
        return false;
    }

    // initialize() also succeeds on a backend that has no QRhi at all, and that
    // is the same fact for this case as it is for the window-backed ones: no
    // renderer callback will ever run, so the behavior is unobservable here
    // rather than wrong.
    QRhi* const rhi = control.rhi();
    if (!rhi) {
        unsupported_configuration = true;
        return false;
    }

    // A QRhi exists from here on, so anything that fails below is a failure of
    // this suite's subject, not of the environment.
    TEST_ASSERT(render_target.create(*rhi, size),
        "the render control's QRhi should provide an offscreen color target");
    window.setRenderTarget(render_target.quick_render_target());

    auto state  = std::make_shared<destruction_state_t>();
    auto widget = std::make_unique<recording_widget_t>(state);
    configure_static_widget(*widget, window);

    // A complete frame: synchronize() arms the item's queued feedback
    // delivery, render() publishes the frame's feedback, and afterRendering
    // both marks that generation deliverable and posts the delivery.
    control.polishItems();
    control.beginFrame();
    TEST_ASSERT(control.sync(),
        "the render control should synchronize the first frame");
    control.render();
    control.endFrame();

    TEST_ASSERT(state->render_callbacks == 1,
        "the first frame should reach the renderer exactly once");

    // The item dies with that delivery still queued, so its destructor is the
    // only thing that can cancel it.
    widget.reset();
    TEST_ASSERT(!state->feedback_visible_at_destruction,
        "feedback must not run before the queued owner-thread delivery point");

    // No sync, so the scene graph still carries the node the destroyed item
    // left behind and this pass is the renderer-only frame.
    control.beginFrame();
    control.render();
    control.endFrame();

    TEST_ASSERT(state->render_callbacks == 2,
        "lifecycle regression must execute render after item destruction");
    TEST_ASSERT(state->renderer_only_frame_completed,
        "renderer-only frame should finish after item destruction");

    // Draining the queue is where a delivery that outlived its receiver would
    // reach freed memory.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return true;
}

bool test_area_hover_rejects_delayed_feedback_after_exit_and_view_changes(
    bool& unsupported_configuration)
{
    const QSize size(480, 320);
    QQuickRenderControl control;
    QQuickWindow window(&control);
    offscreen_render_target_t render_target;
    window.setGeometry(0, 0, size.width(), size.height());
    window.contentItem()->setSize(QSizeF(size));
    if (!control.initialize() || !control.rhi()) {
        unsupported_configuration = true;
        return false;
    }
    TEST_ASSERT(render_target.create(*control.rhi(), size),
        "hover lifecycle fixture needs an offscreen color target");
    window.setRenderTarget(render_target.quick_render_target());

    auto widget = std::make_unique<test_widget_t>();
    configure_static_widget(*widget, window);
    auto config = widget->config();
    config.show_text         = false;
    config.preview_height_px = 20.0;
    widget->set_config(config);
    widget->set_v_range(0.0f, 10.0f);
    widget->set_available_t_range(0, 3'000'000'000LL);
    widget->set_t_range(0, 3'000'000'000LL);

    struct sample_t
    {
        std::int64_t timestamp;
        float        value;
    };
    plot::Data_access_policy_typed<sample_t> access;
    access.get_timestamp = [](const sample_t& sample) { return sample.timestamp; };
    access.get_value     = [](const sample_t& sample) { return sample.value; };
    auto series = std::make_shared<plot::series_data_t>();
    series->style       = plot::Display_style::AREA;
    series->access      = access.erase();
    series->data_source = std::make_shared<plot::Vector_data_source<sample_t>>(
        std::vector<sample_t>{{0, 6.0f}, {3'000'000'000LL, 6.0f}});
    widget->add_series(7, series);

    const auto render_frame = [&] {
        widget->update();
        control.polishItems();
        control.beginFrame();
        const bool synchronized = control.sync();
        control.render();
        control.endFrame();
        return synchronized;
    };
    const auto deliver_feedback = [&] {
        QCoreApplication::sendPostedEvents(widget.get(), QEvent::MetaCall);
    };
    TEST_ASSERT(render_frame(), "initial plot frame must synchronize");
    deliver_feedback();
    TEST_ASSERT(render_frame(), "settled plot layout must synchronize");
    deliver_feedback();
    TEST_ASSERT(widget->hovered_series_id().isNull(),
        "rendering an area without opting into hover must not select a series");

    widget->set_area_hover_position(60.0, 200.0);
    TEST_ASSERT(render_frame(), "hovered plot frame must synchronize");
    widget->clear_area_hover();
    deliver_feedback();
    TEST_ASSERT(widget->hovered_series_id().isNull(),
        "a frame queued before pointer exit must not restore its old hover");

    widget->set_area_hover_position(60.0, 200.0);
    TEST_ASSERT(render_frame(), "pointer reentry must synchronize");
    deliver_feedback();
    TEST_ASSERT(widget->hovered_series_id() == QVariant(7),
        "a completed frame must publish the area containing the current pointer");

    TEST_ASSERT(render_frame(), "pre-replacement frame must synchronize");
    widget->clear();
    widget->add_series(7, series);
    TEST_ASSERT(widget->hovered_series_id().isNull(),
        "replacing the series set must immediately clear even a reused series ID");
    deliver_feedback();
    TEST_ASSERT(widget->hovered_series_id().isNull(),
        "old completed feedback must not select a series from the replacement set");
    TEST_ASSERT(render_frame(), "replacement series must synchronize");
    deliver_feedback();
    TEST_ASSERT(widget->hovered_series_id() == QVariant(7),
        "a new frame must recompute the stationary pointer against replacement data");

    TEST_ASSERT(render_frame(), "pre-view-change frame must synchronize");
    widget->set_v_range(20.0f, 30.0f);
    TEST_ASSERT(widget->hovered_series_id().isNull(), "changing the view must clear its hover immediately");
    deliver_feedback();
    TEST_ASSERT(widget->hovered_series_id().isNull(),
        "feedback from the preceding view must not restore an invisible area");
    TEST_ASSERT(render_frame(), "changed view must synchronize");
    deliver_feedback();
    TEST_ASSERT(widget->hovered_series_id().isNull(),
        "the pointer must remain unselected when the new view contains no filled area");
    return true;
}

using rhi_dependent_test_fn_t = bool (*)(bool& unsupported_configuration);

// An RHI-dependent case reports through its out-parameter that the window it
// opened never received a QRhi. That is a statement about the environment, not
// about the renderer, so it is tallied and printed apart from a real failure:
// it must never read as a pass, and it must still fail the run, because a job
// that is supposed to have a graphics backend and silently stops exercising it
// is exactly the degradation this suite exists to catch.
void run_rhi_dependent_test(
    const char*             name,
    rhi_dependent_test_fn_t test_fn,
    int&                    passed,
    int&                    failed,
    int&                    unsupported)
{
    std::cout << "Running " << name << "... ";

    bool       unsupported_configuration = false;
    const bool assertions_held           = test_fn(unsupported_configuration);

    if (unsupported_configuration) {
        std::cout << "UNSUPPORTED CONFIGURATION" << std::endl;
        std::cerr
            << "UNSUPPORTED CONFIGURATION: " << name
            << " needs a QRhi, and this window's scene graph never provided one."
            << std::endl;
        ++unsupported;
    }
    else if (assertions_held) {
        std::cout << "OK" << std::endl;
        ++passed;
    }
    else {
        std::cout << "FAIL" << std::endl;
        ++failed;
    }
}

#define RUN_RHI_DEPENDENT_TEST(test_fn)                                                \
    run_rhi_dependent_test(#test_fn, test_fn, passed, failed, unsupported)

} // namespace

int main(int argc, char** argv)
{
    qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("threaded"));
    QGuiApplication app(argc, argv);

    std::cout << "Plot renderer lifecycle tests" << std::endl;

    int passed      = 0;
    int failed      = 0;
    int unsupported = 0;

    RUN_TEST(test_feedback_channel_respects_completed_frame_boundary);
    RUN_TEST(test_replaced_feedback_channel_ignores_old_callback);
    RUN_TEST(test_renderer_outlives_synchronized_widget);
    RUN_TEST(test_created_renderer_has_independent_lifetime);
    RUN_RHI_DEPENDENT_TEST(test_static_first_frame_delivers_feedback);
    RUN_RHI_DEPENDENT_TEST(test_destroy_before_renderer_only_frame_cancels_feedback_delivery);
    RUN_RHI_DEPENDENT_TEST(test_area_hover_rejects_delayed_feedback_after_exit_and_view_changes);

    std::cout
        << "Results: "
        << passed      << " passed, "
        << failed      << " failed, "
        << unsupported << " unsupported"
        << std::endl;

    if (unsupported > 0) {
        std::cerr
            << "UNSUPPORTED CONFIGURATION: no QRhi was available on this run, so "
            << unsupported
            << " renderer case(s) never executed. A job that is expected to have a "
               "graphics backend must be repaired, not skipped."
            << std::endl;
    }

    return failed > 0 || unsupported > 0 ? 1 : 0;
}
