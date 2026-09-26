// Native frames exercising compact sample reuse with a current custom snapshot.

#include "test_macros.h"
#include "test_series_renderer.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/rhi/qrhi_series_layer.h>
#include <vnm_plot/rhi/series_data.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <QGuiApplication>
#include <QImage>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <rhi/qrhi.h>

#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

namespace plot = vnm::plot;

namespace {

constexpr int k_width = 320;
constexpr int k_height = 128;
constexpr int k_sample_count = 65'536;

struct sample_t { std::int64_t t; float value; };

struct layer_observations_t
{
    int prepares = 0;
    int records = 0;
    bool valid_inputs = true;
    QRhiBuffer* buffer = nullptr;
};

class Snapshot_layer_state final : public plot::Qrhi_series_layer_state
{
public:
    explicit Snapshot_layer_state(layer_observations_t& observations) : m_observations(observations) {}

    bool prepare(const plot::qrhi_series_prepare_context_t& ctx) override
    {
        ++m_observations.prepares;
        m_observations.valid_inputs &= ctx.window.snapshot && ctx.sample_buffer.buffer &&
            ctx.sample_buffer.sample_count == ctx.window.gpu_count && ctx.view_uniform && ctx.view_ubo;
        m_observations.buffer = ctx.sample_buffer.buffer;
        return true;
    }

    void record(const plot::qrhi_series_record_context_t& ctx) override
    {
        ++m_observations.records;
        m_observations.valid_inputs &= static_cast<bool>(ctx.window.snapshot);
    }

private:
    layer_observations_t& m_observations;
};

class Snapshot_layer final : public plot::Qrhi_series_layer
{
public:
    explicit Snapshot_layer(layer_observations_t& observations) : m_observations(observations) {}
    std::string_view id() const override { return "snapshot-reuse"; }
    std::uint64_t revision() const override { return 1; }
    int z_order() const override { return 10; }
    bool draws_view(plot::Series_view_kind kind) const override { return kind == plot::Series_view_kind::MAIN; }
    std::unique_ptr<plot::Qrhi_series_layer_state> create_state(QRhi&) const override
    {
        return std::make_unique<Snapshot_layer_state>(m_observations);
    }

private:
    layer_observations_t& m_observations;
};

class Native_frames
{
public:
    Native_frames() : m_window(&m_control) {}

    bool initialize()
    {
        m_window.setGeometry(0, 0, k_width, k_height);
        if (!m_control.initialize() || !m_control.rhi()) {
            return false;
        }
        auto* rhi = m_control.rhi();
        std::cout << "Sample reuse backend: " << rhi->backendName() << '\n';
        m_color.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(k_width, k_height), 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_color->create()) {
            return false;
        }
        m_target.reset(rhi->newTextureRenderTarget(
            QRhiTextureRenderTargetDescription(QRhiColorAttachment(m_color.get()))));
        m_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_pass.get());
        return m_target->create();
    }

    QImage render(plot::Test_series_renderer& renderer, plot::frame_context_t& ctx,
        const std::map<int, std::shared_ptr<const plot::series_data_t>>& series, bool read_pixels)
    {
        auto* rhi = m_control.rhi();
        m_control.beginFrame();
        ctx.pmv = glm::make_mat4(rhi->clipSpaceCorrMatrix().constData()) *
            glm::ortho(0.0f, float(k_width), float(k_height), 0.0f, -1.0f, 1.0f);
        ctx.rhi = rhi;
        ctx.cb = m_control.commandBuffer();
        ctx.render_target = m_target.get();
        ctx.rhi_updates = rhi->nextResourceUpdateBatch();
        renderer.prepare(ctx, series);
        ctx.cb->beginPass(m_target.get(), Qt::black, {1.0f, 0}, ctx.rhi_updates);
        ctx.cb->setViewport(QRhiViewport(0, 0, k_width, k_height));
        renderer.render(ctx, series);
        QRhiReadbackResult readback;
        QRhiResourceUpdateBatch* updates = nullptr;
        if (read_pixels) {
            updates = rhi->nextResourceUpdateBatch();
            updates->readBackTexture(QRhiReadbackDescription(m_color.get()), &readback);
        }
        ctx.cb->endPass(updates);
        m_control.endFrame();
        if (!read_pixels || readback.data.size() != k_width * k_height * 4) {
            return {};
        }
        return QImage(reinterpret_cast<const uchar*>(readback.data.constData()),
            k_width, k_height, QImage::Format_RGBA8888).copy();
    }

    void finish() { m_control.rhi()->finish(); }

private:
    QQuickRenderControl m_control;
    QQuickWindow m_window;
    std::unique_ptr<QRhiTexture> m_color;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
};

bool test_reuse(Native_frames& frames, plot::Display_style style)
{
    std::vector<sample_t> samples;
    for (int i = 0; i < k_sample_count; ++i) {
        samples.push_back({std::int64_t(i) * 10'000, 64.0f});
    }
    auto source = std::make_shared<plot::Vector_data_source<sample_t>>(samples);
    layer_observations_t observations;
    auto series = std::make_shared<plot::rhi_series_data_t>();
    series->data_source = source;
    series->access = plot::make_access_policy<sample_t>(&sample_t::t, &sample_t::value).erase();
    series->style = style;
    series->color = glm::vec4(1.0f);
    series->qrhi_layers.push_back(std::make_shared<Snapshot_layer>(observations));
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map{{1, series}};
    plot::Test_series_renderer renderer;
    plot::frame_layout_result_t layout;
    layout.usable_width = k_width;
    layout.usable_height = k_height;
    plot::Plot_config config;
    plot::frame_context_t ctx{layout};
    ctx.t0 = ctx.t_available_min = 0;
    ctx.t1 = ctx.t_available_max = samples.back().t;
    ctx.v0 = 0;
    ctx.v1 = k_height;
    ctx.win_w = k_width;
    ctx.win_h = k_height;
    ctx.config = &config;

    const QImage initial = frames.render(renderer, ctx, series_map, true);
    TEST_ASSERT(!initial.isNull(), "cold native frame must be read back");
    if (style == plot::Display_style::AREA) {
        TEST_ASSERT(qRed(initial.pixel(k_width / 2, 3 * k_height / 4)) > 0 ||
            qRed(initial.pixel(k_width / 2, k_height / 4)) > 0,
            "mixed view must contain the visible built-in area");
    }
    const auto& view = renderer.m_vbo_states.at(1).main_view;
    TEST_ASSERT(view.last_sample_upload_count == 1, "cold frame must upload compact samples");
    QRhiBuffer* const initial_buffer = observations.buffer;
    for (int i = 0; i < 8; ++i) {
        frames.render(renderer, ctx, series_map, false);
    }
    frames.finish();
    constexpr int measured_frames = 60;
    std::size_t upload_count = 0;
    std::size_t upload_bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < measured_frames; ++i) {
        frames.render(renderer, ctx, series_map, false);
        upload_count += view.last_sample_upload_count;
        upload_bytes += view.last_sample_upload_count ? view.last_sample_upload_bytes : 0;
    }
    frames.finish();
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << (style == plot::Display_style::NONE ? "custom" : "mixed")
              << ": frames=" << measured_frames << " mean_ms=" << milliseconds / measured_frames
              << " uploads=" << upload_count << " bytes=" << upload_bytes << '\n';
    const QImage reused = frames.render(renderer, ctx, series_map, true);
    TEST_ASSERT(reused == initial, "unchanged native frame must retain exactly the same pixels");
    TEST_ASSERT(observations.valid_inputs && observations.prepares == observations.records &&
        observations.prepares == measured_frames + 10, "every frame must prepare and record with current inputs");
    TEST_ASSERT(observations.buffer == initial_buffer, "unchanged frame must retain its sample buffer");
    TEST_ASSERT(upload_count == 0 && upload_bytes == 0,
        "unchanged custom snapshots must reuse the uploaded compact samples");

    source->set_data(samples);
    const QImage reuploaded = frames.render(renderer, ctx, series_map, true);
    TEST_ASSERT(view.last_sample_upload_count == 1, "changed source sequence must force an upload");
    TEST_ASSERT(reuploaded == initial, "forced upload of equal data must agree with reused pixels");
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    Native_frames frames;
    if (!frames.initialize()) {
        return 1;
    }
    int passed = 0;
    int failed = 0;
    const auto custom = [&] { return test_reuse(frames, plot::Display_style::NONE); };
    const auto mixed = [&] { return test_reuse(frames, plot::Display_style::AREA); };
    RUN_TEST(custom);
    RUN_TEST(mixed);
    return failed ? 1 : 0;
}
