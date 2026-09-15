// vnm_plot Font_renderer draw-range recording tests

#include "test_macros.h"

#include <vnm_plot/rhi/asset_loader.h>
#include <vnm_plot/rhi/font_renderer.h>
#include <vnm_plot/rhi/frame_context.h>
#include <vnm_plot/rhi/primitive_renderer.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSize>
#include <rhi/qrhi.h>

#include <array>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>

namespace plot = vnm::plot;

namespace {

constexpr int k_frame_width  = 400;
constexpr int k_frame_height = 120;
constexpr int k_font_px      = 18;

using frame_step_t = std::function<void(const plot::frame_context_t&)>;

// Renders frames through the QRhi of a Qt Quick render control. The Null
// backend the other QRhi tests use rasterizes nothing, and what these tests
// compare is the pixels the recorded draws paint.
class Offscreen_frames
{
public:
    Offscreen_frames()
    :
        m_window(&m_control)
    {}

    bool initialize(bool& unsupported_configuration)
    {
        m_window.setGeometry(0, 0, k_frame_width, k_frame_height);
        if (!m_control.initialize() || !m_control.rhi()) {
            unsupported_configuration = true;
            return false;
        }

        QRhi* const rhi = m_control.rhi();
        const QSize size(k_frame_width, k_frame_height);
        m_color.reset(rhi->newTexture(
            QRhiTexture::RGBA8,
            size,
            1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_color->create()) {
            return false;
        }

        m_target.reset(rhi->newTextureRenderTarget(
            QRhiTextureRenderTargetDescription(QRhiColorAttachment(m_color.get()))));
        m_render_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_render_pass.get());
        return m_target->create();
    }

    // Runs `prepare` before the pass opens and `record` inside it, then reads
    // the color target back. A null image means the frame could not be read.
    QImage render(const frame_step_t& prepare, const frame_step_t& record)
    {
        QRhi* const rhi = m_control.rhi();
        m_control.beginFrame();

        const glm::mat4 pixel_ortho = glm::ortho(
            0.0f,
            static_cast<float>(k_frame_width),
            static_cast<float>(k_frame_height),
            0.0f,
            -1.0f,
            1.0f);

        plot::frame_context_t ctx{m_layout};
        ctx.win_w         = k_frame_width;
        ctx.win_h         = k_frame_height;
        ctx.pmv           = glm::make_mat4(rhi->clipSpaceCorrMatrix().constData()) * pixel_ortho;
        ctx.rhi           = rhi;
        ctx.cb            = m_control.commandBuffer();
        ctx.render_target = m_target.get();
        ctx.rhi_updates   = rhi->nextResourceUpdateBatch();

        prepare(ctx);
        ctx.cb->beginPass(
            m_target.get(),
            QColor::fromRgbF(0.10f, 0.15f, 0.20f, 1.0f),
            QRhiDepthStencilClearValue(1.0f, 0),
            ctx.rhi_updates);
        ctx.cb->setViewport(QRhiViewport(
            0.0f,
            0.0f,
            static_cast<float>(k_frame_width),
            static_cast<float>(k_frame_height)));
        record(ctx);

        QRhiReadbackResult             readback;
        QRhiResourceUpdateBatch* const readback_updates = rhi->nextResourceUpdateBatch();
        readback_updates->readBackTexture(QRhiReadbackDescription(m_color.get()), &readback);
        ctx.cb->endPass(readback_updates);
        m_control.endFrame();

        if (readback.data.size() != k_frame_width * k_frame_height * 4) {
            return {};
        }
        return QImage(
            reinterpret_cast<const uchar*>(readback.data.constData()),
            k_frame_width,
            k_frame_height,
            QImage::Format_RGBA8888).copy();
    }

private:
    QQuickRenderControl                       m_control;
    QQuickWindow                              m_window;
    plot::frame_layout_result_t               m_layout;

    // Released before the render control's QRhi, which owns them.
    std::unique_ptr<QRhiTexture>              m_color;
    std::unique_ptr<QRhiTextureRenderTarget>  m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_render_pass;
};

struct text_draw_t
{
    float                 x;
    float                 y;
    const char*           text;
    glm::vec4             color;
    plot::text_scissor_t  scissor;
    plot::text_shadow_t   shadow;
};

void queue_text(
    plot::Font_renderer&            fonts,
    const plot::frame_context_t&    ctx,
    const text_draw_t&              draw)
{
    fonts.batch_text(draw.x, draw.y, draw.text);
    fonts.rhi_queue_draw(ctx, ctx.pmv, draw.color, draw.scissor, draw.shadow);
}

void prepare_whole_frame(
    plot::Font_renderer&                fonts,
    const plot::frame_context_t&        ctx,
    std::initializer_list<text_draw_t>  draws)
{
    fonts.rhi_begin_frame();
    for (const text_draw_t& draw : draws) {
        queue_text(fonts, ctx, draw);
    }
    fonts.rhi_finalize_frame(ctx);
}

bool column_differs(const QImage& image, const QImage& blank, int left, int right)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = left; x < right; ++x) {
            if (image.pixel(x, y) != blank.pixel(x, y)) {
                return true;
            }
        }
    }
    return false;
}

// Three text slices in columns of their own and a rect in a fourth column,
// queued between the first two slices. No draw overlaps a draw of another
// slice, so a recording that draws every slice exactly once, each under its
// own pipeline state, paints the frame the whole-frame recorder paints.
class Column_scene
{
public:
    Column_scene()
    :
        m_fonts(m_loader)
    {
        plot::init_embedded_assets(m_loader);
        m_fonts.initialize_metrics(k_font_px);
    }

    plot::Font_renderer& fonts()                            { return m_fonts;             }
    std::size_t          slice_end(std::size_t slice) const { return m_slice_ends[slice]; }

    void prepare(const plot::frame_context_t& ctx)
    {
        const plot::text_shadow_t  shadow{glm::vec4(0.0f, 0.0f, 0.0f, 0.8f), 3.0f};
        const plot::text_scissor_t clip{true, 250, 0, 24, k_frame_height};
        const glm::vec4            opaque(0.95f, 0.90f, 0.80f, 1.0f);
        const glm::vec4            translucent(0.40f, 0.90f, 0.50f, 0.6f);

        m_fonts.rhi_begin_frame();

        // Overlapping shadowed draws, so the slice's shadow-then-foreground
        // order shows in the pixels.
        queue_text(m_fonts, ctx, { 10.0f, 40.0f, "Range", opaque,      {},   shadow});
        queue_text(m_fonts, ctx, { 14.0f, 46.0f, "Range", translucent, {},   shadow});
        m_slice_ends[0] = m_fonts.queued_draw_count();

        m_primitives.batch_rect(
            glm::vec4(0.80f, 0.30f, 0.20f, 1.0f),
            glm::vec4(364.0f, 10.0f, 396.0f, 110.0f));
        m_primitives.flush_rects(ctx, ctx.pmv);

        queue_text(m_fonts, ctx, {130.0f, 40.0f, "Slice", translucent, {},   {}});
        queue_text(m_fonts, ctx, {130.0f, 70.0f, "0123",  opaque,      {},   {}});
        m_slice_ends[1] = m_fonts.queued_draw_count();

        queue_text(m_fonts, ctx, {250.0f, 40.0f, "Clip",  opaque,      clip, {}});
        queue_text(m_fonts, ctx, {250.0f, 80.0f, "Draw",  translucent, {},   shadow});
        m_slice_ends[2] = m_fonts.queued_draw_count();

        m_fonts.rhi_finalize_frame(ctx);
    }

    void record_rect(const plot::frame_context_t& ctx)
    {
        m_primitives.record_draws(ctx, m_primitives.queued_op_count());
    }

    void reset_frame()
    {
        m_fonts.rhi_reset_frame();
        m_primitives.reset_frame();
    }

    // The existing whole-frame recording that every sliced recording of this
    // scene must reproduce, checked to paint all four columns.
    bool render_whole(Offscreen_frames& frames, QImage& whole)
    {
        const QImage blank = frames.render(
            [](const plot::frame_context_t&) {},
            [](const plot::frame_context_t&) {});
        whole = frames.render(
            [this](const plot::frame_context_t& ctx) { prepare(ctx); },
            [this](const plot::frame_context_t& ctx) {
                m_fonts.rhi_record_frame(ctx);
                record_rect(ctx);
                reset_frame();
            });

        TEST_ASSERT(!blank.isNull() && !whole.isNull(),
            "the column scene frames must be read back");
        TEST_ASSERT(column_differs(whole, blank,   0, 120), "the first text slice must paint");
        TEST_ASSERT(column_differs(whole, blank, 120, 240), "the second text slice must paint");
        TEST_ASSERT(column_differs(whole, blank, 240, 360), "the third text slice must paint");
        TEST_ASSERT(column_differs(whole, blank, 360, 400), "the rect must paint");
        return true;
    }

private:
    plot::Asset_loader         m_loader;
    plot::Font_renderer        m_fonts;
    plot::Primitive_renderer   m_primitives;
    std::array<std::size_t, 3> m_slice_ends{};
};

bool test_sliced_recording_matches_whole_recording(Offscreen_frames& frames)
{
    Column_scene scene;
    QImage       whole;
    if (!scene.render_whole(frames, whole)) {
        return false;
    }

    // The rect records between the first and second slice, so the second
    // slice starts with another renderer's pipeline and bindings in place.
    const QImage sliced = frames.render(
        [&](const plot::frame_context_t& ctx) { scene.prepare(ctx); },
        [&](const plot::frame_context_t& ctx) {
            scene.fonts().rhi_record_draws(ctx, scene.slice_end(0));
            scene.record_rect(ctx);
            scene.fonts().rhi_record_draws(ctx, scene.slice_end(1));
            scene.fonts().rhi_record_draws(ctx, scene.slice_end(2));
            scene.reset_frame();
        });

    TEST_ASSERT(!sliced.isNull(), "the sliced frame must be read back");
    TEST_ASSERT(sliced == whole,
        "recording in slices around another renderer's draw must paint the whole-frame result");
    return true;
}

bool test_record_draws_cursor_only_advances(Offscreen_frames& frames)
{
    Column_scene scene;
    QImage       whole;
    if (!scene.render_whole(frames, whole)) {
        return false;
    }

    // After two slices, an end before the cursor, one at it, and one past the
    // queue, which must leave exactly the third slice to record.
    const QImage revisited = frames.render(
        [&](const plot::frame_context_t& ctx) { scene.prepare(ctx); },
        [&](const plot::frame_context_t& ctx) {
            scene.fonts().rhi_record_draws(ctx, scene.slice_end(1));
            scene.fonts().rhi_record_draws(ctx, scene.slice_end(0));
            scene.fonts().rhi_record_draws(ctx, scene.slice_end(1));
            scene.fonts().rhi_record_draws(ctx, scene.slice_end(2) + 1);
            scene.record_rect(ctx);
            scene.reset_frame();
        });

    TEST_ASSERT(!revisited.isNull(), "the revisited frame must be read back");
    TEST_ASSERT(revisited == whole,
        "an end at or before the cursor must record nothing, and one past the queue only the rest");
    return true;
}

bool test_slices_record_in_submission_order(Offscreen_frames& frames)
{
    plot::Asset_loader loader;
    plot::init_embedded_assets(loader);

    plot::Font_renderer subject(loader);
    plot::Font_renderer first_layer(loader);
    plot::Font_renderer second_layer(loader);
    plot::Font_renderer third_layer(loader);
    for (plot::Font_renderer* fonts : {&subject, &first_layer, &second_layer, &third_layer}) {
        fonts->initialize_metrics(k_font_px);
    }

    // Three shadowed draws of one word, each shifted by a couple of pixels, so
    // every shadow lands on the glyphs of the draws before it.
    const plot::text_shadow_t shadow{glm::vec4(0.05f, 0.05f, 0.10f, 0.9f), 4.0f};
    const text_draw_t first {20.0f, 50.0f, "Order", glm::vec4(0.95f, 0.90f, 0.80f, 1.0f), {}, shadow};
    const text_draw_t second{23.0f, 53.0f, "Order", glm::vec4(0.30f, 0.60f, 1.00f, 1.0f), {}, shadow};
    const text_draw_t third {26.0f, 56.0f, "Order", glm::vec4(0.90f, 0.30f, 0.40f, 1.0f), {}, shadow};

    // A foreground with zero alpha leaves the target untouched, so a draw
    // stripped of it paints only its shadow, and one stripped of its shadow
    // paints only its foreground.
    text_draw_t first_shadow  = first;
    text_draw_t second_shadow = second;
    first_shadow.color.a      = 0.0f;
    second_shadow.color.a     = 0.0f;
    text_draw_t first_glyphs  = first;
    text_draw_t second_glyphs = second;
    first_glyphs.shadow       = {};
    second_glyphs.shadow      = {};

    std::size_t first_slice_end = 0;
    const QImage sliced = frames.render(
        [&](const plot::frame_context_t& ctx) {
            subject.rhi_begin_frame();
            queue_text(subject, ctx, first);
            queue_text(subject, ctx, second);
            first_slice_end = subject.queued_draw_count();
            queue_text(subject, ctx, third);
            subject.rhi_finalize_frame(ctx);
        },
        [&](const plot::frame_context_t& ctx) {
            subject.rhi_record_draws(ctx, first_slice_end);
            subject.rhi_record_draws(ctx, subject.queued_draw_count());
            subject.rhi_reset_frame();
        });

    // The first slice's shadows, then its foregrounds, then the second slice,
    // each layer recorded whole by a renderer of its own.
    const QImage expected = frames.render(
        [&](const plot::frame_context_t& ctx) {
            prepare_whole_frame(first_layer,  ctx, {first_shadow, second_shadow});
            prepare_whole_frame(second_layer, ctx, {first_glyphs, second_glyphs});
            prepare_whole_frame(third_layer,  ctx, {third});
        },
        [&](const plot::frame_context_t& ctx) {
            first_layer.rhi_record_frame(ctx);
            second_layer.rhi_record_frame(ctx);
            third_layer.rhi_record_frame(ctx);
        });

    // The two orders the expected frame must be distinguishable from, or the
    // comparison below could not tell a correct recording from a wrong one.
    const QImage shadows_of_all_draws_first = frames.render(
        [&](const plot::frame_context_t& ctx) {
            prepare_whole_frame(subject, ctx, {first, second, third});
        },
        [&](const plot::frame_context_t& ctx) { subject.rhi_record_frame(ctx); });
    const QImage each_draw_whole_in_turn = frames.render(
        [&](const plot::frame_context_t& ctx) {
            prepare_whole_frame(first_layer,  ctx, {first});
            prepare_whole_frame(second_layer, ctx, {second});
            prepare_whole_frame(third_layer,  ctx, {third});
        },
        [&](const plot::frame_context_t& ctx) {
            first_layer.rhi_record_frame(ctx);
            second_layer.rhi_record_frame(ctx);
            third_layer.rhi_record_frame(ctx);
        });

    TEST_ASSERT(
        !sliced.isNull()                     &&
        !expected.isNull()                   &&
        !shadows_of_all_draws_first.isNull() &&
        !each_draw_whole_in_turn.isNull(),
        "the submission order frames must be read back");
    TEST_ASSERT(expected != shadows_of_all_draws_first,
        "a later slice's shadow must visibly cover an earlier slice's glyphs");
    TEST_ASSERT(expected != each_draw_whole_in_turn,
        "a slice's second shadow must visibly cover its first draw's glyphs");
    TEST_ASSERT(sliced == expected,
        "each slice must record its shadows, then its foregrounds, after all earlier slices");
    return true;
}

using frame_test_fn_t = bool (*)(Offscreen_frames& frames);

void run_frame_test(
    const char*         name,
    frame_test_fn_t     test_fn,
    Offscreen_frames&   frames,
    int&                passed,
    int&                failed)
{
    std::cout << "Running " << name << "... ";
    if (test_fn(frames)) {
        std::cout << "OK" << std::endl;
        ++passed;
    }
    else {
        std::cout << "FAIL" << std::endl;
        ++failed;
    }
}

#define RUN_FRAME_TEST(test_fn) run_frame_test(#test_fn, test_fn, frames, passed, failed)

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    std::cout << "Font renderer draw range tests" << std::endl;

    // Every atlas here is built from the embedded font; none needs to persist.
    plot::set_font_disk_cache_enabled(false);

    Offscreen_frames frames;
    bool unsupported_configuration = false;
    if (!frames.initialize(unsupported_configuration)) {
        if (unsupported_configuration) {
            std::cerr
                << "UNSUPPORTED CONFIGURATION: the Qt Quick render control provided no "
                   "QRhi, so no draw-range case ran. A job that is expected to have a "
                   "graphics backend must be repaired, not skipped."
                << std::endl;
        }
        else {
            std::cerr
                << "FAIL: the render control's QRhi could not create a color target"
                << std::endl;
        }
        return 1;
    }

    int passed = 0;
    int failed = 0;

    RUN_FRAME_TEST(test_sliced_recording_matches_whole_recording);
    RUN_FRAME_TEST(test_record_draws_cursor_only_advances);
    RUN_FRAME_TEST(test_slices_record_in_submission_order);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
