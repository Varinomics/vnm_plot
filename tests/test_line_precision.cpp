// Raster regressions for sparse LINE data at deep time zoom.

#include "test_macros.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/rhi/series_renderer.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace plot = vnm::plot;

namespace {

constexpr int k_width  = 1000;
constexpr int k_height = 128;
constexpr std::int64_t k_span_ns = 100'000'000;

struct sample_t
{
    std::int64_t t;
    float        v;
};

class Line_frames
{
public:
    Line_frames() : m_window(&m_control) {}

    bool initialize()
    {
        m_window.setGeometry(0, 0, k_width, k_height);
        if (!m_control.initialize() || !m_control.rhi()) {
            return false;
        }
        auto* rhi = m_control.rhi();
        std::cout << "Raster backend: " << rhi->backendName() << '\n';
        m_color.reset(rhi->newTexture(
            QRhiTexture::RGBA8, QSize(k_width, k_height), 1,
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

    QImage render(
        std::vector<sample_t> samples,
        double width = 1.0,
        bool snap = false,
        plot::Series_interpolation interpolation = plot::Series_interpolation::LINEAR)
    {
        plot::frame_layout_result_t layout;
        layout.usable_width  = k_width;
        layout.usable_height = k_height;
        plot::Plot_config config;
        config.line_width_px        = width;
        config.snap_lines_to_pixels = snap;

        auto series          = std::make_shared<plot::series_data_t>();
        series->style        = plot::Display_style::LINE;
        series->color        = glm::vec4(1.0f);
        series->interpolation = interpolation;
        series->data_source = std::make_shared<plot::Vector_data_source<sample_t>>(std::move(samples));
        series->access = plot::make_access_policy<sample_t>(&sample_t::t, &sample_t::v).erase();
        std::map<int, std::shared_ptr<const plot::series_data_t>> series_map{{1, series}};
        plot::Series_renderer renderer;

        auto* rhi = m_control.rhi();
        m_control.beginFrame();
        plot::frame_context_t ctx{layout};
        ctx.t0              = 0;
        ctx.t1              = k_span_ns;
        ctx.t_available_min = 0;
        ctx.t_available_max = k_span_ns;
        ctx.v0              = 0.0f;
        ctx.v1              = k_height;
        ctx.win_w           = k_width;
        ctx.win_h           = k_height;
        ctx.config          = &config;
        ctx.pmv = glm::make_mat4(rhi->clipSpaceCorrMatrix().constData()) *
            glm::ortho(0.0f, float(k_width), float(k_height), 0.0f, -1.0f, 1.0f);
        ctx.rhi           = rhi;
        ctx.cb            = m_control.commandBuffer();
        ctx.render_target = m_target.get();
        ctx.rhi_updates   = rhi->nextResourceUpdateBatch();
        renderer.prepare(ctx, series_map);
        ctx.cb->beginPass(m_target.get(), Qt::black, {1.0f, 0}, ctx.rhi_updates);
        ctx.cb->setViewport(QRhiViewport(0.0f, 0.0f, float(k_width), float(k_height)));
        renderer.render(ctx, series_map);

        QRhiReadbackResult readback;
        auto* updates = rhi->nextResourceUpdateBatch();
        updates->readBackTexture(QRhiReadbackDescription(m_color.get()), &readback);
        ctx.cb->endPass(updates);
        m_control.endFrame();
        if (readback.data.size() != k_width * k_height * 4) {
            return {};
        }
        QImage result(
            reinterpret_cast<const uchar*>(readback.data.constData()),
            k_width, k_height, QImage::Format_RGBA8888);
        // QRhi texture rows follow the backend framebuffer orientation.
        return rhi->isYUpInFramebuffer() ? result.mirrored(false, true) : result.copy();
    }

private:
    QQuickRenderControl                       m_control;
    QQuickWindow                              m_window;
    std::unique_ptr<QRhiTexture>              m_color;
    std::unique_ptr<QRhiTextureRenderTarget>  m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
};

bool compare_coverage(const QImage& image, const QImage& reference)
{
    TEST_ASSERT(!image.isNull() && !reference.isNull(), "LINE frames must be read back");
    int max_difference = 0;
    int changed_pixels = 0;
    for (int y = 0; y < k_height; ++y) {
        for (int x = 0; x < k_width; ++x) {
            const int difference = std::abs(qRed(image.pixel(x, y)) - qRed(reference.pixel(x, y)));
            max_difference = std::max(max_difference, difference);
            changed_pixels += difference > 2;
        }
    }
    std::cout << "max coverage difference=" << max_difference
              << ", changed pixels=" << changed_pixels << '\n';
    TEST_ASSERT(max_difference <= 2,
        "equivalent visible segments must agree within two 8-bit coverage levels");
    return true;
}

bool test_sparse_horizontal_segments(Line_frames& frames)
{
    for (const double width : {1.0, 1.35, 6.0}) {
        for (const bool snap : {false, true}) {
            const QImage reference = frames.render({{-k_span_ns, 64.0f}, {2 * k_span_ns, 64.0f}}, width, snap);
            TEST_ASSERT(!reference.isNull(), "normal-zoom horizontal frame must be read back");
            // At the middle of a horizontal segment every column has the
            // same distance to the line. This expectation is analytic, not a
            // golden image approved from the candidate renderer.
            const auto center_coverage = [&](int x) {
                return std::max(qRed(reference.pixel(x, 63)), qRed(reference.pixel(x, 64)));
            };
            const int coverage = center_coverage(k_width / 2);
            TEST_ASSERT(coverage >= 120, "the center row must have at least half coverage");
            for (int x = 0; x < k_width; ++x) {
                TEST_ASSERT(center_coverage(x) == coverage,
                    "normal horizontal coverage must be constant across the viewport");
            }
            for (const std::int64_t gap_ns : {3'600'000'000'000LL, 86'400'000'000'000LL}) {
                for (const auto interpolation : {
                    plot::Series_interpolation::LINEAR,
                    plot::Series_interpolation::STEP_AFTER})
                {
                    const QImage image = frames.render(
                        {{-gap_ns / 2, 64.0f}, {gap_ns / 2, 64.0f}}, width, snap, interpolation);
                    TEST_ASSERT(compare_coverage(image, reference),
                        "deep time zoom must retain horizontal LINE coverage");
                }
            }
        }
    }
    return true;
}

bool test_visible_caps_and_step_corner(Line_frames& frames)
{
    const QImage capped = frames.render({{25'000'000, 64.0f}, {75'000'000, 64.0f}}, 8.0);
    TEST_ASSERT(!capped.isNull(), "cap frame must be read back");
    TEST_ASSERT(qRed(capped.pixel(250, 64)) > 240, "a real endpoint must keep its round cap");
    TEST_ASSERT(qRed(capped.pixel(247, 64)) > 200, "the cap extends behind its endpoint");
    TEST_ASSERT(qRed(capped.pixel(245, 64)) == 0, "the round cap has finite extent");
    TEST_ASSERT(qRed(capped.pixel(246, 67)) < 40, "the cap corner is outside the round stroke");

    const QImage step = frames.render(
        {{-1'800'000'000'000, 96.0f}, {50'000'000, 32.0f}, {1'800'000'000'000, 32.0f}},
        6.0, false, plot::Series_interpolation::STEP_AFTER);
    TEST_ASSERT(!step.isNull(), "step frame must be read back");
    TEST_ASSERT(qRed(step.pixel(200, 32)) > 240, "the first held value must reach the step");
    TEST_ASSERT(qRed(step.pixel(500, 64)) > 240, "the step's vertical segment must remain visible");
    TEST_ASSERT(qRed(step.pixel(800, 96)) > 240, "the second held value must leave the step");
    TEST_ASSERT(qRed(step.pixel(200, 96)) == 0, "clipping must not join unrelated held levels");
    return true;
}

bool test_sloped_vertical_and_invisible_segments(Line_frames& frames)
{
    const QImage sloped = frames.render({{-10'000'000, 96.0f}, {110'000'000, 32.0f}}, 6.0);
    TEST_ASSERT(!sloped.isNull(), "sloped frame must be read back");
    TEST_ASSERT(qRed(sloped.pixel(500, 64)) > 240, "ordinary diagonal center must remain covered");
    TEST_ASSERT(qRed(sloped.pixel(500, 70)) == 0, "ordinary diagonal coverage must remain bounded");

    // At this time scale the y change over the visible width is less than
    // 0.002 pixels, so its coverage rounds to the horizontal reference.
    const QImage shallow = frames.render({{-1'800'000'000'000, 96.0f}, {1'800'000'000'000, 32.0f}}, 6.0);
    const QImage horizontal = frames.render({{-k_span_ns, 64.0f}, {2 * k_span_ns, 64.0f}}, 6.0);
    TEST_ASSERT(compare_coverage(shallow, horizontal), "sparse diagonal must retain its visible coverage");

    const QImage vertical = frames.render({{50'000'000, -18'000'000.0f}, {50'000'000, 18'000'000.0f}}, 6.0);
    const QImage vertical_reference = frames.render({{50'000'000, -128.0f}, {50'000'000, 256.0f}}, 6.0);
    TEST_ASSERT(compare_coverage(vertical, vertical_reference), "deep value zoom must retain vertical coverage");

    const QImage invisible = frames.render({{-1'800'000'000'000, 512.0f}, {1'800'000'000'000, 512.0f}}, 6.0);
    TEST_ASSERT(!invisible.isNull(), "invisible segment frame must be read back");
    for (int y = 0; y < k_height; ++y) {
        for (int x = 0; x < k_width; ++x) {
            TEST_ASSERT(qRed(invisible.pixel(x, y)) == 0, "a rejected segment must paint no pixels");
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    Line_frames frames;
    if (!frames.initialize()) {
        std::cerr << "FAIL: LINE precision tests require a rasterizing QRhi backend\n";
        return 1;
    }
    int passed = 0;
    int failed = 0;
    const auto sparse_horizontal_segments = [&] { return test_sparse_horizontal_segments(frames); };
    const auto visible_caps_and_step_corner = [&] { return test_visible_caps_and_step_corner(frames); };
    const auto sloped_vertical_and_invisible_segments = [&] {
        return test_sloped_vertical_and_invisible_segments(frames);
    };
    RUN_TEST(sparse_horizontal_segments);
    RUN_TEST(visible_caps_and_step_corner);
    RUN_TEST(sloped_vertical_and_invisible_segments);
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
