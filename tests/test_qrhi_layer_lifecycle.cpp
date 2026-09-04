// vnm_plot QRhi series layer lifecycle tests

#include "test_macros.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/rhi/asset_loader.h>
#include <vnm_plot/core/plot_config.h>
#define private public
#include <vnm_plot/rhi/series_renderer.h>
#undef private
#include <vnm_plot/rhi/qrhi_series_layer.h>
#include <vnm_plot/rhi/series_data.h>

#include <QColor>
#include <QSize>
#include <rhi/qrhi.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plot = vnm::plot;

namespace {

struct test_sample_t
{
    std::int64_t           timestamp_ns;
    float                  value;
    float                  range_min;
    float                  range_max;
};

struct layer_event_t
{
    std::string            layer_id;
    std::string            action;
    bool                   resources_changed    = false;
    bool                   snapshot_valid       = false;
    std::size_t            snapshot_count       = 0;
    plot::Series_view_kind view_kind            = plot::Series_view_kind::MAIN;
    std::size_t            source_first         = 0;
    std::size_t            source_count         = 0;
    std::size_t            synthetic_hold_count = 0;
    std::size_t            gpu_count            = 0;
    std::vector<plot::drawable_sample_span_t>
                           drawable_spans;
    std::uint64_t          sample_sequence      = 0;
    std::uint64_t          snapshot_sequence    = 0;
    std::int64_t           t_min_ns             = 0;
    std::int64_t           t_max_ns             = 0;
    std::int64_t           t_origin_ns          = 0;
    float                  v_min                = 0.0f;
    float                  v_max                = 0.0f;
    plot::Nonfinite_sample_policy nonfinite_policy =
        plot::Nonfinite_sample_policy::BREAK_SEGMENT;
    QRhiBuffer*            sample_buffer                      = nullptr;
    std::size_t            sample_buffer_first_sample         = 0;
    std::size_t            sample_buffer_sample_count         = 0;
    std::size_t            sample_buffer_source_first         = 0;
    std::size_t            sample_buffer_source_count         = 0;
    std::size_t            sample_buffer_synthetic_hold_count = 0;
    std::int64_t           sample_buffer_t_origin_ns          = 0;
    std::int64_t           sample_buffer_t_min_ns             = 0;
    std::int64_t           sample_buffer_t_max_ns             = 0;
    float                  sample_buffer_v_min                = 0.0f;
    float                  sample_buffer_v_max                = 0.0f;
    std::size_t            sample_buffer_stride_bytes         = 0;
    std::size_t            sample_buffer_t_rel_seconds_offset = 0;
    std::size_t            sample_buffer_value_offset         = 0;
    std::size_t            sample_buffer_range_min_offset     = 0;
    std::size_t            sample_buffer_range_max_offset     = 0;
};

class Test_source final : public plot::Data_source
{
public:
    Test_source()
    {
        set_samples({
            { 0LL, 1.0f },
            {1'000'000'000LL, 2.0f},
            {2'000'000'000LL, 3.0f},
            {3'000'000'000LL, 4.0f},
            {4'000'000'000LL, 5.0f},
            {5'000'000'000LL, 6.0f},
            {6'000'000'000LL, 7.0f},
            {7'000'000'000LL, 8.0f}
        });
    }

    plot::snapshot_result_t try_snapshot(std::size_t lod_level) override
    {
        ++m_snapshot_calls;
        if (lod_level != 0) {
            return {plot::data_snapshot_t{}, plot::snapshot_result_t::Snapshot_status::FAILED};
        }
        if (m_busy_snapshots_remaining > 0) {
            --m_busy_snapshots_remaining;
            return {plot::data_snapshot_t{}, plot::snapshot_result_t::Snapshot_status::BUSY};
        }
        if (m_advance_during_next_snapshot) {
            m_advance_during_next_snapshot = false;
            ++m_sequence;
        }
        plot::data_snapshot_t snapshot;
        snapshot.data     = m_samples.data();
        snapshot.count    = m_samples.size();
        snapshot.stride   = sizeof(test_sample_t);
        snapshot.sequence = m_sequence;
        auto hold     = std::make_shared<int>(17);
        m_last_hold   = hold;
        snapshot.hold = hold;
        return {
            snapshot,
            m_samples.empty()
                ? plot::snapshot_result_t::Snapshot_status::EMPTY
                : plot::snapshot_result_t::Snapshot_status::READY
        };
    }

    std::size_t sample_stride() const override { return sizeof(test_sample_t); }
    std::uint64_t current_sequence(std::size_t lod_level = 0) const override
    {
        (void)lod_level;
        return m_sequence;
    }
    const void* identity() const override { return m_identity; }

    void set_identity(const void* identity) { m_identity = identity; }

    void set_samples(std::vector<test_sample_t> samples)
    {
        m_samples = std::move(samples);
        ++m_sequence;
    }

    std::size_t sample_count() const { return m_samples.size(); }

    void notify_changed()
    {
        ++m_sequence;
    }

    int                 snapshot_calls() const { return m_snapshot_calls; }
    std::weak_ptr<void> last_hold()      const { return m_last_hold;      }
    void return_busy_once() { m_busy_snapshots_remaining = 1; }
    void advance_during_next_snapshot() { m_advance_during_next_snapshot = true; }

private:
    std::vector<test_sample_t> m_samples;
    std::weak_ptr<void>        m_last_hold;
    const void*                m_identity                     = this;
    std::uint64_t              m_sequence                     = 0;
    int                        m_snapshot_calls               = 0;
    int                        m_busy_snapshots_remaining     = 0;
    bool                       m_advance_during_next_snapshot = false;
};

plot::Data_access_policy make_access_policy()
{
    plot::Data_access_policy policy;
    policy.get_timestamp = [](const void* sample) {
        return static_cast<const test_sample_t*>(sample)->timestamp_ns;
    };
    policy.get_value = [](const void* sample) {
        return static_cast<const test_sample_t*>(sample)->value;
    };
    policy.get_range = [](const void* sample) {
        const float value = static_cast<const test_sample_t*>(sample)->value;
        return std::make_pair(value, value);
    };
    policy.layout_key = 11;
    return policy;
}

plot::Data_access_policy make_value_only_access_policy()
{
    plot::Data_access_policy policy;
    policy.get_value = [](const void* sample) {
        return static_cast<const test_sample_t*>(sample)->value;
    };
    policy.get_range = [](const void* sample) {
        const float value = static_cast<const test_sample_t*>(sample)->value;
        return std::make_pair(value, value);
    };
    policy.layout_key = 12;
    return policy;
}

// Timestamp + range, no primary value. Valid for auto-range and custom
// range-band layers, but a built-in value style must not draw it.
plot::Data_access_policy make_range_only_access_policy()
{
    plot::Data_access_policy policy;
    policy.get_timestamp = [](const void* sample) {
        return static_cast<const test_sample_t*>(sample)->timestamp_ns;
    };
    policy.get_range = [](const void* sample) {
        const float value = static_cast<const test_sample_t*>(sample)->value;
        return std::make_pair(value - 1.0f, value + 1.0f);
    };
    policy.layout_key = 13;
    return policy;
}

struct access_call_counts_t
{
    int timestamp = 0;
    int value     = 0;
    int range     = 0;
};

plot::Data_access_policy make_direct_member_access_policy()
{
    auto typed = plot::make_access_policy<test_sample_t>(
        &test_sample_t::timestamp_ns,
        &test_sample_t::value,
        &test_sample_t::range_min,
        &test_sample_t::range_max);
    return typed.erase();
}

plot::Data_access_policy make_fallback_access_policy_with_counted_public_accessors(
    access_call_counts_t& calls)
{
    plot::Data_access_policy policy;
    policy.get_timestamp = [&calls](const void* sample) {
        ++calls.timestamp;
        return static_cast<const test_sample_t*>(sample)->timestamp_ns;
    };
    policy.get_value = [&calls](const void* sample) {
        ++calls.value;
        return static_cast<const test_sample_t*>(sample)->value;
    };
    policy.get_range = [&calls](const void* sample) {
        ++calls.range;
        const auto* typed_sample = static_cast<const test_sample_t*>(sample);
        return std::make_pair(typed_sample->range_min, typed_sample->range_max);
    };
    policy.layout_key = 22;
    return policy;
}

class Recording_layer_state final : public plot::Qrhi_series_layer_state
{
public:
    Recording_layer_state(std::string layer_id, std::vector<layer_event_t>& events)
    :
        m_layer_id(std::move(layer_id)),
        m_events(events)
    {}

    void cleanup_qrhi_resources(QRhi* rhi) override
    {
        (void)rhi;
        m_events.push_back({m_layer_id, "cleanup"});
    }

    bool prepare(const plot::qrhi_series_prepare_context_t& ctx) override
    {
        layer_event_t event;
        event.layer_id                   = m_layer_id;
        event.action                     = "prepare";
        event.resources_changed          = ctx.resources_changed;
        event.snapshot_valid             = static_cast<bool>(ctx.window.snapshot);
        event.snapshot_count             = ctx.window.snapshot.count;
        event.snapshot_sequence          = ctx.window.snapshot.sequence;
        event.view_kind                  = ctx.window.view_kind;
        event.source_first               = ctx.window.source_first;
        event.source_count               = ctx.window.source_count;
        event.synthetic_hold_count       = ctx.window.synthetic_hold_count;
        event.gpu_count                  = ctx.window.gpu_count;
        event.drawable_spans             = ctx.window.drawable_spans;
        event.sample_sequence            = ctx.window.sample_sequence;
        event.t_min_ns                   = ctx.window.t_min_ns;
        event.t_max_ns                   = ctx.window.t_max_ns;
        event.t_origin_ns                = ctx.window.t_origin_ns;
        event.v_min                      = ctx.window.v_min;
        event.v_max                      = ctx.window.v_max;
        event.nonfinite_policy           = ctx.window.nonfinite_policy;
        event.sample_buffer              = ctx.sample_buffer.buffer;
        event.sample_buffer_first_sample = ctx.sample_buffer.first_sample;
        event.sample_buffer_sample_count = ctx.sample_buffer.sample_count;
        event.sample_buffer_source_first = ctx.sample_buffer.source_first;
        event.sample_buffer_source_count = ctx.sample_buffer.source_count;
        event.sample_buffer_synthetic_hold_count =
            ctx.sample_buffer.synthetic_hold_count;
        event.sample_buffer_t_origin_ns = ctx.sample_buffer.t_origin_ns;
        event.sample_buffer_t_min_ns    = ctx.sample_buffer.t_min_ns;
        event.sample_buffer_t_max_ns    = ctx.sample_buffer.t_max_ns;
        event.sample_buffer_v_min       = ctx.sample_buffer.v_min;
        event.sample_buffer_v_max       = ctx.sample_buffer.v_max;
        event.sample_buffer_stride_bytes =
            ctx.sample_buffer.layout.stride_bytes;
        event.sample_buffer_t_rel_seconds_offset =
            ctx.sample_buffer.layout.t_rel_seconds_offset;
        event.sample_buffer_value_offset =
            ctx.sample_buffer.layout.value_offset;
        event.sample_buffer_range_min_offset =
            ctx.sample_buffer.layout.range_min_offset;
        event.sample_buffer_range_max_offset =
            ctx.sample_buffer.layout.range_max_offset;
        m_events.push_back(event);
        return true;
    }

    void record(const plot::qrhi_series_record_context_t& ctx) override
    {
        layer_event_t event;
        event.layer_id             = m_layer_id;
        event.action               = "record";
        event.snapshot_valid       = static_cast<bool>(ctx.window.snapshot);
        event.snapshot_count       = ctx.window.snapshot.count;
        event.snapshot_sequence    = ctx.window.snapshot.sequence;
        event.view_kind            = ctx.window.view_kind;
        event.source_first         = ctx.window.source_first;
        event.source_count         = ctx.window.source_count;
        event.synthetic_hold_count = ctx.window.synthetic_hold_count;
        event.gpu_count            = ctx.window.gpu_count;
        event.drawable_spans       = ctx.window.drawable_spans;
        event.sample_sequence      = ctx.window.sample_sequence;
        event.t_min_ns             = ctx.window.t_min_ns;
        event.t_max_ns             = ctx.window.t_max_ns;
        event.t_origin_ns          = ctx.window.t_origin_ns;
        event.nonfinite_policy     = ctx.window.nonfinite_policy;
        m_events.push_back(event);
    }

private:
    std::string m_layer_id;
    std::vector<layer_event_t>& m_events;
};

class Recording_layer final : public plot::Qrhi_series_layer
{
public:
    Recording_layer(
        std::string                    id,
        std::uint64_t                  revision,
        int                            z_order,
        std::vector<layer_event_t>&    events,
        int&                           create_count)
    :
        m_id(std::move(id)),
        m_revision(revision),
        m_z_order(z_order),
        m_events(events),
        m_create_count(create_count)
    {}

    std::string_view id()       const override { return m_id;       }
    std::uint64_t    revision() const override { return m_revision; }
    int              z_order()  const override { return m_z_order;  }

    bool draws_view(plot::Series_view_kind view_kind) const override
    {
        return view_kind == plot::Series_view_kind::MAIN;
    }

    std::unique_ptr<plot::Qrhi_series_layer_state> create_state(QRhi& rhi) const override
    {
        (void)rhi;
        ++m_create_count;
        m_events.push_back({m_id, "create"});
        return std::make_unique<Recording_layer_state>(m_id, m_events);
    }

    void set_id(std::string id) { m_id = std::move(id); }
    void set_revision(std::uint64_t revision) { m_revision = revision; }

private:
    std::string    m_id;
    std::uint64_t  m_revision = 0;
    int            m_z_order  = 0;
    std::vector<layer_event_t>&
                   m_events;
    int&           m_create_count;
};

class Offscreen_rhi_fixture
{
public:
    bool initialize(std::string& error_message)
    {
        QRhiNullInitParams params;
        m_rhi.reset(QRhi::create(QRhi::Null, &params));
        if (!m_rhi) {
            error_message = "failed to create QRhi Null backend";
            return false;
        }

        const QSize size(320, 180);
        m_color_buffer.reset(m_rhi->newRenderBuffer(
            QRhiRenderBuffer::Color,
            size,
            1,
            {},
            QRhiTexture::RGBA8));
        if (!m_color_buffer || !m_color_buffer->create()) {
            error_message = "failed to create offscreen color buffer";
            return false;
        }

        QRhiColorAttachment color_attachment;
        color_attachment.setRenderBuffer(m_color_buffer.get());
        QRhiTextureRenderTargetDescription target_description(color_attachment);
        m_render_target.reset(m_rhi->newTextureRenderTarget(target_description));
        m_render_pass_descriptor.reset(m_render_target->newCompatibleRenderPassDescriptor());
        m_render_target->setRenderPassDescriptor(m_render_pass_descriptor.get());
        if (!m_render_target || !m_render_target->create()) {
            error_message = "failed to create offscreen render target";
            return false;
        }

        return true;
    }

    QRhi*                    rhi()           const { return m_rhi.get();           }
    QRhiTextureRenderTarget* render_target() const { return m_render_target.get(); }

    bool render_layer_frame(
        plot::Series_renderer& renderer,
        plot::frame_context_t& ctx,
        const std::map<int, std::shared_ptr<const plot::series_data_t>>&
                               series_map,
        std::vector<layer_event_t>&
                               events,
        std::string&           error_message)
    {
        QRhiCommandBuffer* command_buffer = nullptr;
        if (m_rhi->beginOffscreenFrame(&command_buffer) != QRhi::FrameOpSuccess || !command_buffer) {
            error_message = "QRhi beginOffscreenFrame failed";
            return false;
        }

        QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
        ctx.rhi           = m_rhi.get();
        ctx.cb            = command_buffer;
        ctx.render_target = m_render_target.get();
        ctx.rhi_updates   = updates;

        renderer.prepare(ctx, series_map);
        events.push_back({"frame", "begin_pass"});
        command_buffer->beginPass(
            m_render_target.get(),
            QColor::fromRgbF(0.0, 0.0, 0.0, 1.0),
            QRhiDepthStencilClearValue(1.0f, 0),
            updates);
        renderer.render(ctx, series_map);
        command_buffer->endPass();
        m_rhi->endOffscreenFrame();
        return true;
    }

private:
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiRenderBuffer> m_color_buffer;
    std::unique_ptr<QRhiTextureRenderTarget> m_render_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_render_pass_descriptor;
};

plot::frame_layout_result_t make_layout()
{
    plot::frame_layout_result_t layout;
    layout.usable_width  = 240.0;
    layout.usable_height = 120.0;
    return layout;
}

plot::frame_context_t make_context(
    const plot::frame_layout_result_t& layout,
    const plot::Plot_config&           config)
{
    plot::frame_context_t ctx{layout};
    ctx.t0              = 0;
    ctx.t1              = 3'000'000'000LL;
    ctx.t_available_min = 0;
    ctx.t_available_max = 7'000'000'000LL;
    ctx.v0              = 0.0f;
    ctx.v1              = 10.0f;
    ctx.win_w           = 320;
    ctx.win_h           = 180;
    ctx.config          = &config;
    return ctx;
}

std::shared_ptr<plot::series_data_t> make_layer_only_series(
    std::shared_ptr<Test_source>                                   source,
    std::vector<std::shared_ptr<const plot::Qrhi_series_layer>>    layers)
{
    auto series         = std::make_shared<plot::rhi_series_data_t>();
    series->style       = plot::Display_style::NONE;
    series->data_source = source;
    series->access      = make_access_policy();
    series->qrhi_layers = std::move(layers);
    return series;
}

std::shared_ptr<plot::series_data_t> make_builtin_plus_layer_series(
    std::shared_ptr<Test_source>                                   source,
    plot::Display_style                                            style,
    std::vector<std::shared_ptr<const plot::Qrhi_series_layer>>    layers)
{
    auto series         = std::make_shared<plot::rhi_series_data_t>();
    series->style       = style;
    series->data_source = source;
    series->access      = make_access_policy();
    series->qrhi_layers = std::move(layers);
    return series;
}

std::shared_ptr<plot::series_data_t> make_line_plus_layer_series(
    std::shared_ptr<Test_source>                                   source,
    std::vector<std::shared_ptr<const plot::Qrhi_series_layer>>    layers)
{
    return make_builtin_plus_layer_series(
        std::move(source),
        plot::Display_style::LINE,
        std::move(layers));
}

const layer_event_t* find_prepare_event(
    const std::vector<layer_event_t>&  events,
    std::string_view                   layer_id)
{
    for (const auto& event : events) {
        if (event.layer_id == layer_id && event.action == "prepare") {
            return &event;
        }
    }
    return nullptr;
}

std::size_t find_event_index(
    const std::vector<layer_event_t>&  events,
    std::string_view                   layer_id,
    std::string_view                   action)
{
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].layer_id == layer_id && events[i].action == action) {
            return i;
        }
    }
    return events.size();
}

bool assert_compact_upload_state(
    const plot::Series_renderer&       renderer,
    int                                series_id,
    const layer_event_t&               prepare,
    std::size_t                        expected_line_window_sample_count,
    std::string_view                   label)
{
    constexpr std::size_t k_gpu_sample_bytes = sizeof(float) * 4u;

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        std::string("expected renderer VBO state for ") + std::string(label));
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_staged_sample_count == prepare.gpu_count,
        std::string(label) + " upload must stage the compact GPU window; got " +
        std::to_string(view_state.last_staged_sample_count) + ", expected " +
        std::to_string(prepare.gpu_count));
    TEST_ASSERT(view_state.last_sample_upload_bytes ==
        prepare.gpu_count * k_gpu_sample_bytes,
        std::string(label) + " upload bytes must match compact GPU sample count");
    TEST_ASSERT(view_state.last_sample_upload_bytes <
        prepare.snapshot_count * k_gpu_sample_bytes,
        std::string(label) + " upload bytes must not scale with full snapshot count");
    TEST_ASSERT(
        view_state.last_line_window_sample_count ==
            expected_line_window_sample_count,
        std::string(label) + " line-window sample count mismatch");
    TEST_ASSERT(view_state.last_sample_buffer,
        std::string(label) + " compact VBO should exist");
    TEST_ASSERT(prepare.sample_buffer == view_state.last_sample_buffer,
        std::string(label) + " prepare context sample buffer must expose the compact VBO");
    TEST_ASSERT(prepare.sample_buffer_first_sample == 0,
        std::string(label) + " sample buffer first sample mismatch");
    TEST_ASSERT(prepare.sample_buffer_sample_count == prepare.gpu_count,
        std::string(label) + " sample buffer count mismatch");
    TEST_ASSERT(prepare.sample_buffer_source_first == prepare.source_first &&
        prepare.sample_buffer_source_count == prepare.source_count &&
        prepare.sample_buffer_synthetic_hold_count ==
            prepare.synthetic_hold_count,
        std::string(label) + " sample buffer window metadata mismatch");
    TEST_ASSERT(prepare.sample_buffer_t_origin_ns == prepare.t_origin_ns &&
        prepare.sample_buffer_t_min_ns == prepare.t_min_ns &&
        prepare.sample_buffer_t_max_ns == prepare.t_max_ns,
        std::string(label) + " sample buffer time metadata mismatch");
    TEST_ASSERT(prepare.sample_buffer_v_min == prepare.v_min &&
        prepare.sample_buffer_v_max == prepare.v_max,
        std::string(label) + " sample buffer value range mismatch");
    TEST_ASSERT(prepare.sample_buffer_stride_bytes == k_gpu_sample_bytes,
        std::string(label) + " sample buffer stride mismatch");
    TEST_ASSERT(prepare.sample_buffer_t_rel_seconds_offset == 0u &&
        prepare.sample_buffer_value_offset == sizeof(float) &&
        prepare.sample_buffer_range_min_offset == sizeof(float) * 2u &&
        prepare.sample_buffer_range_max_offset == sizeof(float) * 3u,
        std::string(label) + " sample buffer lane offsets mismatch");

    return true;
}

bool assert_drawable_span(
    const std::vector<plot::drawable_sample_span_t>&
                           spans,
    std::size_t            index,
    std::size_t            source_first,
    std::size_t            source_count,
    std::size_t            gpu_first,
    std::size_t            gpu_count,
    std::string_view       label)
{
    TEST_ASSERT(index < spans.size(),
        std::string(label) + " expected drawable span index");
    const auto& span = spans[index];
    TEST_ASSERT(span.source_first == source_first,
        std::string(label) + " source_first mismatch");
    TEST_ASSERT(span.source_count == source_count,
        std::string(label) + " source_count mismatch");
    TEST_ASSERT(span.gpu_first == gpu_first,
        std::string(label) + " gpu_first mismatch");
    TEST_ASSERT(span.gpu_count == gpu_count,
        std::string(label) + " gpu_count mismatch");
    return true;
}

bool test_layer_only_zero_style_prepare_record_order()
{
    std::vector<layer_event_t> events;
    int low_create_count  = 0;
    int high_create_count = 0;
    auto low_layer        = std::make_shared<Recording_layer>(
        "low", 1, -5, events, low_create_count);
    auto high_layer = std::make_shared<Recording_layer>(
        "high", 1, 10, events, high_create_count);

    auto source = std::make_shared<Test_source>();
    auto series = make_layer_only_series(source, {high_layer, low_layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    series_map[7] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    TEST_ASSERT(low_create_count == 1,  "low z-order layer state should be created once");
    TEST_ASSERT(high_create_count == 1, "high z-order layer state should be created once");
    const std::size_t low_prepare  = find_event_index(events, "low",   "prepare");
    const std::size_t high_prepare = find_event_index(events, "high",  "prepare");
    const std::size_t begin_pass   = find_event_index(events, "frame", "begin_pass");
    const std::size_t low_record   = find_event_index(events, "low",   "record");
    const std::size_t high_record  = find_event_index(events, "high",  "record");

    TEST_ASSERT(low_prepare  < events.size(), "expected low layer prepare event");
    TEST_ASSERT(high_prepare < events.size(), "expected high layer prepare event");
    TEST_ASSERT(begin_pass   < events.size(), "expected beginPass event");
    TEST_ASSERT(low_record   < events.size(), "expected low layer record event");
    TEST_ASSERT(high_record  < events.size(), "expected high layer record event");
    TEST_ASSERT(low_prepare  < high_prepare, "lower z-order layer must prepare first");
    TEST_ASSERT(high_prepare < begin_pass, "prepare must finish before beginPass");
    TEST_ASSERT(begin_pass   < low_record, "record must happen after beginPass");
    TEST_ASSERT(low_record   < high_record, "lower z-order layer must record first");
    TEST_ASSERT(events[low_prepare].gpu_count > 0 && events[high_prepare].gpu_count > 0,
        "layer-only series with zero Display_style must not be skipped");
    TEST_ASSERT(events[low_prepare].snapshot_valid && events[high_prepare].snapshot_valid,
        "layer-only external prepares must receive a valid snapshot");

    auto state_it = renderer.m_vbo_states.find(7);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for custom-only layer test");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_sample_upload_count == 1,
        "custom-only drawable layers should upload one compact sample window");
    TEST_ASSERT(view_state.last_primitive_prepare_count == 0,
        "custom-only drawable layers must not prepare built-in primitives");
    if (!assert_compact_upload_state(
            renderer, 7, events[low_prepare], 0, "custom-only low layer"))
    {
        return false;
    }
    TEST_ASSERT(events[high_prepare].sample_buffer == events[low_prepare].sample_buffer,
        "custom-only layers in one view should share the compact sample buffer");
    TEST_ASSERT(
        events[high_prepare].sample_buffer_sample_count ==
            events[low_prepare].sample_buffer_sample_count,
        "custom-only layers in one view should share sample buffer metadata");

    return true;
}

bool test_style_none_without_layers_does_not_upload_samples()
{
    std::vector<layer_event_t> events;
    auto source = std::make_shared<Test_source>();
    auto series = std::make_shared<plot::rhi_series_data_t>();
    series->style       = plot::Display_style::NONE;
    series->data_source = source;
    series->access      = make_access_policy();

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 8;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    TEST_ASSERT(renderer.m_vbo_states.find(series_id) == renderer.m_vbo_states.end(),
        "series without built-ins or custom layers must not allocate VBO state");
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders.empty(),
        "series without built-ins or custom layers must not record draws");

    return true;
}

bool test_custom_sample_buffer_not_reused_when_current_access_cannot_stage()
{
    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "external", 1, 0, events, create_count);

    auto source = std::make_shared<Test_source>();
    auto series = make_layer_only_series(source, {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 9;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const layer_event_t* first_prepare = find_prepare_event(events, "external");
    TEST_ASSERT(first_prepare && first_prepare->sample_buffer,
        "initial custom-only prepare should expose a compact sample buffer");

    auto value_only_series    = make_layer_only_series(source, {layer});
    value_only_series->access = make_value_only_access_policy();
    series_map[series_id]     = value_only_series;
    events.clear();

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const layer_event_t* second_prepare = find_prepare_event(events, "external");
    TEST_ASSERT(second_prepare && second_prepare->snapshot_valid,
        "timestamp-less access should still produce a custom-layer window");
    TEST_ASSERT(second_prepare->gpu_count > 0,
        "timestamp-less access should keep the planned drawable count");
    TEST_ASSERT(second_prepare->sample_buffer == nullptr,
        "timestamp-less access must not expose a stale compact sample buffer");
    TEST_ASSERT(second_prepare->sample_buffer_sample_count == 0,
        "empty sample buffer descriptor should report zero samples");

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for stale sample-buffer regression");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(!view_state.has_uploaded_vbo,
        "failed current-window staging should invalidate uploaded VBO state");
    TEST_ASSERT(view_state.last_sample_buffer == nullptr,
        "failed current-window staging should clear sample-buffer instrumentation");
    TEST_ASSERT(view_state.last_sample_upload_count == 0,
        "timestamp-less current window should not upload compact samples");

    return true;
}

bool test_builtin_upload_stages_visible_window_only()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "visible-upload", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    std::vector<test_sample_t> samples;
    samples.reserve(128);
    for (std::size_t i = 0; i < 128; ++i) {
        samples.push_back({
            static_cast<std::int64_t>(i) * k_second_ns,
            static_cast<float>(i)
        });
    }
    source->set_samples(std::move(samples));

    auto series = make_line_plus_layer_series(source, {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 31;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 40LL * k_second_ns;
    ctx.t1                                   = 42LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* prepare = find_prepare_event(events, "visible-upload");
    TEST_ASSERT(prepare, "expected visible-upload layer prepare event");
    TEST_ASSERT(prepare->snapshot_count == source->sample_count(),
        "external layer should still receive the full frame snapshot");
    TEST_ASSERT(prepare->source_count > 0,
        "visible source window should contain samples");
    TEST_ASSERT(prepare->source_count < prepare->snapshot_count,
        "visible source window should be smaller than the full source snapshot");
    TEST_ASSERT(prepare->synthetic_hold_count == 0,
        "ordinary visible upload should not add a synthetic hold sample");
    TEST_ASSERT(prepare->gpu_count == prepare->source_count,
        "linear visible upload should stage one GPU sample per source sample");

    if (!assert_compact_upload_state(
            renderer, series_id, *prepare, prepare->gpu_count, "linear line"))
    {
        return false;
    }

    return true;
}

bool test_builtin_upload_reuses_vbo_capacity_headroom()
{
    constexpr std::int64_t k_second_ns        = 1'000'000'000LL;
    constexpr std::size_t  k_gpu_sample_bytes = sizeof(float) * 4u;

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "capacity-upload", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    std::vector<test_sample_t> samples;
    samples.reserve(128);
    for (std::size_t i = 0; i < 128; ++i) {
        samples.push_back({
            static_cast<std::int64_t>(i) * k_second_ns,
            static_cast<float>(i)
        });
    }
    source->set_samples(std::move(samples));

    auto series = make_line_plus_layer_series(source, {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 38;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 40LL * k_second_ns;
    ctx.t1                                   = 41LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* first_prepare =
        find_prepare_event(events, "capacity-upload");
    TEST_ASSERT(first_prepare && first_prepare->gpu_count == 5,
        "first capacity test frame should stage five GPU samples");

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for capacity test");
    const auto& first_view_state = state_it->second.main_view;
    const std::size_t first_capacity =
        first_view_state.rhi_vbo_capacity_bytes;
    const std::size_t first_generation =
        first_view_state.last_vbo_generation;
    TEST_ASSERT(first_capacity >= first_prepare->gpu_count * k_gpu_sample_bytes,
        "first capacity test frame should allocate enough VBO bytes");

    events.clear();
    ctx.t1              = 42LL * k_second_ns;
    ctx.t_available_max = ctx.t1;
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* second_prepare =
        find_prepare_event(events, "capacity-upload");
    TEST_ASSERT(second_prepare && second_prepare->gpu_count == 6,
        "second capacity test frame should grow to six GPU samples");

    const auto& second_view_state = state_it->second.main_view;
    TEST_ASSERT(
        second_prepare->gpu_count * k_gpu_sample_bytes <= first_capacity,
        "second capacity test frame must stay within first allocation headroom");
    TEST_ASSERT(second_view_state.rhi_vbo_capacity_bytes == first_capacity,
        "compact VBO capacity should stay stable inside existing headroom");
    TEST_ASSERT(second_view_state.last_vbo_generation == first_generation,
        "compact VBO must not be reallocated inside existing headroom");

    return true;
}

bool test_combined_builtin_uploads_samples_once_per_view()
{
    constexpr std::int64_t k_second_ns        = 1'000'000'000LL;
    constexpr std::size_t  k_gpu_sample_bytes = sizeof(float) * 4u;

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "combined-upload", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    std::vector<test_sample_t> samples;
    samples.reserve(128);
    for (std::size_t i = 0; i < 128; ++i) {
        samples.push_back({
            static_cast<std::int64_t>(i) * k_second_ns,
            static_cast<float>(i)
        });
    }
    source->set_samples(std::move(samples));

    auto series = make_builtin_plus_layer_series(
        source,
        plot::Display_style::DOTS_LINE_AREA,
        {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 41;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 40LL * k_second_ns;
    ctx.t1                                   = 42LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* prepare = find_prepare_event(events, "combined-upload");
    TEST_ASSERT(prepare, "expected combined-upload layer prepare event");
    TEST_ASSERT(prepare->gpu_count > 0,
        "combined upload test should plan a drawable GPU window");

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for combined upload test");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_primitive_prepare_count == 3,
        "combined DOTS_LINE_AREA style must prepare three built-in primitives");
    TEST_ASSERT(view_state.last_sample_upload_count == 1,
        "combined DOTS_LINE_AREA style must upload compact samples once");
    TEST_ASSERT(view_state.last_staged_sample_count == prepare->gpu_count,
        "combined style upload must stage the shared compact GPU window");
    TEST_ASSERT(view_state.last_sample_upload_bytes ==
        prepare->gpu_count * k_gpu_sample_bytes,
        "combined style upload bytes must match one compact GPU window");
    TEST_ASSERT(view_state.last_line_window_sample_count == prepare->gpu_count,
        "combined style LINE primitive should still prepare its padded window");
    const auto* const first_line_window_staging_data =
        view_state.line_window_staging.data();
    TEST_ASSERT(first_line_window_staging_data,
        "combined style LINE primitive should allocate reusable line-window scratch");

    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* second_prepare =
        find_prepare_event(events, "combined-upload");
    TEST_ASSERT(second_prepare,
        "expected second combined-upload layer prepare event");
    TEST_ASSERT(view_state.last_primitive_prepare_count == 3,
        "combined primitive prepare count must describe the current frame");
    TEST_ASSERT(view_state.last_sample_upload_count == 1,
        "combined sample upload count must describe the current frame");
    TEST_ASSERT(view_state.last_line_window_upload_count == 0,
        "unchanged custom-layer snapshots must reuse LINE geometry");
    TEST_ASSERT(view_state.last_staged_sample_count == second_prepare->gpu_count,
        "second combined frame must stage the shared compact GPU window");
    TEST_ASSERT(
        view_state.line_window_staging.data() == first_line_window_staging_data,
        "same-size LINE prepare should reuse the same line-window scratch allocation");

    return true;
}

bool test_direct_member_policy_uses_member_dispatch_in_renderer_staging()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    const auto make_source = [=]() {
        auto source = std::make_shared<Test_source>();
        std::vector<test_sample_t> samples;
        samples.reserve(8);
        for (std::size_t i = 0; i < 8; ++i) {
            const float value = 1.0f + static_cast<float>(i);
            samples.push_back({
                static_cast<std::int64_t>(i) * k_second_ns,
                value,
                value - 0.25f,
                value + 0.25f
            });
        }
        source->set_samples(std::move(samples));
        return source;
    };

    auto direct_series         = std::make_shared<plot::series_data_t>();
    direct_series->style       = plot::Display_style::DOTS;
    direct_series->data_source = make_source();
    direct_series->access      = make_direct_member_access_policy();

    access_call_counts_t fallback_calls;
    auto fallback_series         = std::make_shared<plot::series_data_t>();
    fallback_series->style       = plot::Display_style::DOTS;
    fallback_series->data_source = make_source();
    fallback_series->access =
        make_fallback_access_policy_with_counted_public_accessors(fallback_calls);

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int direct_series_id   = 51;
    const int fallback_series_id = 52;
    series_map[direct_series_id] = direct_series;
    series_map[fallback_series_id] = fallback_series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    std::vector<layer_event_t> events;
    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 5LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const auto direct_it = renderer.m_vbo_states.find(direct_series_id);
    TEST_ASSERT(direct_it != renderer.m_vbo_states.end(),
        "expected direct-policy renderer VBO state");
    TEST_ASSERT(direct_it->second.main_view.last_sample_upload_count == 1,
        "direct-policy series should upload one compact sample window");
    TEST_ASSERT(direct_it->second.main_view.last_staged_sample_count > 0,
        "direct-policy upload should stage visible samples");
    TEST_ASSERT(
        direct_it->second.main_view.last_sample_access_dispatch_kind ==
            plot::detail::access_dispatch_kind_t::MEMBER_POINTER,
        "direct member-pointer renderer path should use member-pointer dispatch");

    const auto fallback_it = renderer.m_vbo_states.find(fallback_series_id);
    TEST_ASSERT(fallback_it != renderer.m_vbo_states.end(),
        "expected fallback-policy renderer VBO state");
    TEST_ASSERT(fallback_it->second.main_view.last_sample_upload_count == 1,
        "fallback-policy series should upload one compact sample window");
    TEST_ASSERT(
        fallback_it->second.main_view.last_sample_access_dispatch_kind ==
            plot::detail::access_dispatch_kind_t::STD_FUNCTION,
        "capturing renderer fallback should use std::function dispatch");
    TEST_ASSERT(fallback_calls.timestamp > 0 &&
        fallback_calls.value > 0 &&
        fallback_calls.range > 0,
        "capturing renderer fallback should call public std::function accessors");

    return true;
}

bool test_access_policy_change_reuploads_builtin_samples()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    auto source = std::make_shared<Test_source>();
    std::vector<test_sample_t> samples;
    samples.reserve(8);
    for (std::size_t i = 0; i < 8; ++i) {
        const float value = 1.0f + static_cast<float>(i);
        samples.push_back({
            static_cast<std::int64_t>(i) * k_second_ns,
            value,
            value - 0.25f,
            value + 0.25f
        });
    }
    source->set_samples(std::move(samples));

    auto series         = std::make_shared<plot::rhi_series_data_t>();
    series->style       = plot::Display_style::DOTS;
    series->data_source = source;
    series->access      = make_direct_member_access_policy();

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 53;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    std::vector<layer_event_t> events;
    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 5LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for access-policy reupload test");
    TEST_ASSERT(state_it->second.main_view.last_sample_upload_count == 1,
        "initial access policy should upload one compact sample window");

    access_call_counts_t changed_calls;
    series->access =
        make_fallback_access_policy_with_counted_public_accessors(changed_calls);
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    TEST_ASSERT(state_it->second.main_view.last_sample_upload_count == 1,
        "access policy change must reupload the compact sample window");
    TEST_ASSERT(
        state_it->second.main_view.last_sample_access_dispatch_kind ==
            plot::detail::access_dispatch_kind_t::STD_FUNCTION,
        "changed policy should stage with std::function dispatch");
    TEST_ASSERT(changed_calls.timestamp > 0 &&
        changed_calls.value > 0 &&
        changed_calls.range > 0,
        "changed policy should invoke replacement public accessors");

    return true;
}

bool test_builtin_staging_normalizes_finite_reversed_ranges()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL, 1.0f, 3.0f, 1.0f },
        { 1LL * k_second_ns, 2.0f, 4.0f, 2.0f},
        { 2LL * k_second_ns, 3.0f, 5.0f, 3.0f},
        { 3LL * k_second_ns, 4.0f, 6.0f, 4.0f}
    });

    auto series         = std::make_shared<plot::series_data_t>();
    series->style       = plot::Display_style::DOTS;
    series->data_source = source;
    series->access      = make_direct_member_access_policy();

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 54;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    std::vector<layer_event_t> events;
    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 3LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for reversed-range staging test");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_sample_upload_count == 1,
        "reversed-range series should upload one compact sample window");
    TEST_ASSERT(view_state.last_staged_sample_count > 0,
        "reversed-range series should stage visible samples");
    TEST_ASSERT(view_state.last_staged_sample_count == 4,
        "reversed-range series should stage all visible samples");
    for (std::size_t i = 0; i < view_state.last_staged_sample_count; ++i) {
        const float expected_low  = static_cast<float>(i + 1u);
        const float expected_high = static_cast<float>(i + 3u);
        TEST_ASSERT(view_state.staging[i].y_min == expected_low,
            "staged finite reversed range low endpoint should be normalized exactly");
        TEST_ASSERT(view_state.staging[i].y_max == expected_high,
            "staged finite reversed range high endpoint should be normalized exactly");
        TEST_ASSERT(view_state.staging[i].y_min <= view_state.staging[i].y_max,
            "staged finite range endpoints should be ordered for GPU upload");
    }

    return true;
}

bool test_nonfinite_break_and_skip_split_drawable_spans()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    struct policy_case_t
    {
        plot::Nonfinite_sample_policy policy =
            plot::Nonfinite_sample_policy::BREAK_SEGMENT;
        std::string_view   layer_id;
        int                series_id = 0;
    };

    const policy_case_t cases[] = {
        { plot::Nonfinite_sample_policy::BREAK_SEGMENT, "break-gap", 100 },
        {plot::Nonfinite_sample_policy::SKIP, "skip-gap", 101}
    };

    for (const auto& test_case : cases) {
        std::vector<layer_event_t> events;
        int create_count = 0;
        auto layer = std::make_shared<Recording_layer>(
            std::string(test_case.layer_id), 1, 20, events, create_count);
        auto source = std::make_shared<Test_source>();
        source->set_samples({
            { 0LL, 1.0f },
            { 1LL * k_second_ns, 2.0f },
            { 2LL * k_second_ns, nan },
            { 3LL * k_second_ns, 4.0f },
            {4LL * k_second_ns, 5.0f}
        });

        auto series = make_builtin_plus_layer_series(
            source,
            plot::Display_style::DOTS_LINE_AREA,
            {layer});
        series->nonfinite_policy = test_case.policy;

        std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
        series_map[test_case.series_id] = series;

        plot::Asset_loader asset_loader;
        plot::Series_renderer renderer;
        renderer.initialize(asset_loader);

        Offscreen_rhi_fixture rhi_fixture;
        std::string error_message;
        TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

        plot::Plot_config config;
        const plot::frame_layout_result_t layout = make_layout();
        plot::frame_context_t ctx = make_context(layout, config);
        ctx.t0                                   = 0;
        ctx.t1                                   = 4LL * k_second_ns;
        ctx.t_available_min                      = ctx.t0;
        ctx.t_available_max                      = ctx.t1;

        TEST_ASSERT(
            rhi_fixture.render_layer_frame(
                renderer, ctx, series_map, events, error_message),
            error_message);

        const layer_event_t* prepare =
            find_prepare_event(events, test_case.layer_id);
        TEST_ASSERT(prepare, "expected nonfinite gap layer prepare event");
        TEST_ASSERT(prepare->nonfinite_policy == test_case.policy,
            "prepare window should expose the series nonfinite policy");
        TEST_ASSERT(prepare->source_first == 0 && prepare->source_count == 5,
            "gap window should keep the containing source window");
        TEST_ASSERT(prepare->synthetic_hold_count == 0,
            "ordinary nonfinite gap window should not synthesize a hold sample");
        TEST_ASSERT(prepare->gpu_count == 4,
            "gap window should compact only drawable samples");
        TEST_ASSERT(prepare->drawable_spans.size() == 2,
            "gap window should split drawable samples into two spans");
        if (!assert_drawable_span(prepare->drawable_spans, 0, 0, 2, 0, 2, test_case.layer_id) ||
            !assert_drawable_span(prepare->drawable_spans, 1, 3, 2, 2, 2, test_case.layer_id))
        {
            return false;
        }

        auto state_it = renderer.m_vbo_states.find(test_case.series_id);
        TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
            "expected renderer VBO state for nonfinite gap test");
        const auto& view_state = state_it->second.main_view;
        TEST_ASSERT(view_state.last_staged_sample_count == 4,
            "gap staging should upload four compact samples");
        TEST_ASSERT(view_state.staging[0].y == 1.0f &&
            view_state.staging[1].y == 2.0f &&
            view_state.staging[2].y == 4.0f &&
            view_state.staging[3].y == 5.0f,
            "gap staging should omit the nonfinite sample without reordering valid samples");
        const std::size_t expected_builtin_span_count =
            test_case.policy == plot::Nonfinite_sample_policy::SKIP ? 1u : 2u;
        const std::size_t expected_builtin_segment_count =
            test_case.policy == plot::Nonfinite_sample_policy::SKIP ? 3u : 2u;
        TEST_ASSERT(view_state.last_recorded_line_span_count ==
            expected_builtin_span_count &&
                view_state.last_recorded_line_segment_count ==
            expected_builtin_segment_count,
            "LINE rendering should break only for BREAK_SEGMENT and compact SKIP gaps");
        TEST_ASSERT(view_state.last_recorded_area_span_count ==
            expected_builtin_span_count &&
                view_state.last_recorded_area_segment_count ==
            expected_builtin_segment_count,
            "AREA rendering should break only for BREAK_SEGMENT and compact SKIP gaps");
        TEST_ASSERT(view_state.last_recorded_dot_sample_count == 4,
            "DOTS rendering should draw only compact valid samples");
        TEST_ASSERT(view_state.last_line_window_sample_count == 4,
            "LINE padded windows should be built per drawable span");

        if (!assert_compact_upload_state(
                renderer, test_case.series_id, *prepare, 4, test_case.layer_id))
        {
            return false;
        }
    }

    return true;
}

bool test_nonfinite_replace_with_zero_keeps_contiguous_span()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "replace-zero", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL, 1.0f },
        { 1LL * k_second_ns, 2.0f },
        { 2LL * k_second_ns, nan },
        { 3LL * k_second_ns, 4.0f },
        {4LL * k_second_ns, 5.0f}
    });

    auto series = make_builtin_plus_layer_series(
        source,
        plot::Display_style::DOTS_LINE_AREA,
        {layer});
    series->nonfinite_policy =
        plot::Nonfinite_sample_policy::REPLACE_WITH_ZERO;

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 102;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 4LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(
            renderer, ctx, series_map, events, error_message),
        error_message);

    const layer_event_t* prepare = find_prepare_event(events, "replace-zero");
    TEST_ASSERT(prepare, "expected replace-zero layer prepare event");
    TEST_ASSERT(prepare->source_count == 5 && prepare->gpu_count == 5,
        "REPLACE_WITH_ZERO should keep all source samples drawable");
    TEST_ASSERT(prepare->drawable_spans.size() == 1,
        "REPLACE_WITH_ZERO should keep a contiguous drawable span");
    if (!assert_drawable_span(
            prepare->drawable_spans, 0, 0, 5, 0, 5, "replace-zero"))
    {
        return false;
    }

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for replace-zero test");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_staged_sample_count == 5,
        "REPLACE_WITH_ZERO should stage all five samples");
    TEST_ASSERT(view_state.staging[2].y == 0.0f &&
        view_state.staging[2].y_min == 0.0f &&
        view_state.staging[2].y_max == 0.0f,
        "REPLACE_WITH_ZERO should stage zero for nonfinite value/range lanes");
    TEST_ASSERT(view_state.last_recorded_line_span_count == 1 &&
        view_state.last_recorded_line_segment_count == 4,
        "REPLACE_WITH_ZERO line rendering should remain contiguous");
    TEST_ASSERT(view_state.last_recorded_area_span_count == 1 &&
        view_state.last_recorded_area_segment_count == 4,
        "REPLACE_WITH_ZERO area rendering should remain contiguous");
    TEST_ASSERT(view_state.last_recorded_dot_sample_count == 5,
        "REPLACE_WITH_ZERO dots should draw every compact sample");

    return true;
}

bool test_nonfinite_reject_window_suppresses_drawable_upload()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "reject-window", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL, 1.0f },
        { 1LL * k_second_ns, 2.0f },
        { 2LL * k_second_ns, nan },
        {3LL * k_second_ns, 4.0f}
    });

    auto series = make_builtin_plus_layer_series(
        source,
        plot::Display_style::DOTS_LINE_AREA,
        {layer});
    series->nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 103;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 3LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(
            renderer, ctx, series_map, events, error_message),
        error_message);

    TEST_ASSERT(!find_prepare_event(events, "reject-window"),
        "REJECT_WINDOW should not prepare custom layers for a failed drawable window");
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders.empty(),
        "REJECT_WINDOW should not record built-in draw commands for a failed window");
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for reject-window test");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_sample_upload_count == 0 &&
        view_state.last_staged_sample_count == 0,
        "REJECT_WINDOW should not upload compact sample data");

    return true;
}

bool test_nonfinite_reject_window_invalidates_prior_upload_before_busy()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f}
    });

    auto series = std::make_shared<plot::series_data_t>();
    series->style            = plot::Display_style::DOTS;
    series->nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    series->data_source      = source;
    series->access           = make_access_policy();

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 104;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    std::vector<layer_event_t> events;
    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 2LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for reject stale fallback test");
    TEST_ASSERT(state_it->second.main_view.has_uploaded_vbo,
        "initial finite REJECT_WINDOW frame should upload samples");
    TEST_ASSERT(!renderer.m_last_recorded_draw_z_orders.empty(),
        "initial finite REJECT_WINDOW frame should draw");

    source->set_samples({
        { 0LL, 1.0f },
        { 1LL * k_second_ns, nan },
        {2LL * k_second_ns, 3.0f}
    });
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(!state_it->second.main_view.has_uploaded_vbo,
        "REJECT_WINDOW failure must invalidate the previous uploaded VBO");
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders.empty(),
        "REJECT_WINDOW failure should not draw stale finite samples");

    source->return_busy_once();
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(!state_it->second.main_view.has_uploaded_vbo,
        "BUSY after REJECT_WINDOW failure must not resurrect stale VBO fallback");
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders.empty(),
        "BUSY after REJECT_WINDOW failure should not draw stale samples");

    return true;
}

bool test_custom_layer_zero_gpu_window_invalidates_prior_upload_before_busy()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f}
    });

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "reject-layer",
        1,
        0,
        events,
        create_count);

    auto series = make_builtin_plus_layer_series(
        source,
        plot::Display_style::DOTS,
        {layer});
    series->nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 106;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 2LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for custom-layer zero-gpu stale test");
    TEST_ASSERT(state_it->second.main_view.has_uploaded_vbo,
        "initial custom-layer frame should upload finite built-in samples");
    TEST_ASSERT(find_prepare_event(events, "reject-layer"),
        "initial custom-layer frame should prepare the layer");
    TEST_ASSERT(!renderer.m_last_recorded_draw_z_orders.empty(),
        "initial custom-layer frame should draw built-ins");

    source->set_samples({
        { 0LL, 1.0f },
        { 1LL * k_second_ns, nan },
        {2LL * k_second_ns, 3.0f}
    });
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(!state_it->second.main_view.has_uploaded_vbo,
        "custom-layer zero-gpu REJECT_WINDOW frame must invalidate the prior VBO");
    TEST_ASSERT(!find_prepare_event(events, "reject-layer"),
        "custom-layer zero-gpu REJECT_WINDOW frame should not prepare the layer");
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders.empty(),
        "custom-layer zero-gpu REJECT_WINDOW frame should not draw stale built-ins");

    source->return_busy_once();
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(!state_it->second.main_view.has_uploaded_vbo,
        "BUSY after custom-layer zero-gpu failure must not resurrect stale VBO fallback");
    TEST_ASSERT(!find_prepare_event(events, "reject-layer"),
        "BUSY after custom-layer zero-gpu failure should not prepare from stale data");
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders.empty(),
        "BUSY after custom-layer zero-gpu failure should not draw stale built-ins");

    return true;
}

bool test_non_drawable_window_invalidates_prior_upload_before_fast_path()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,                1.0f },
        { 1LL * k_second_ns,  2.0f },
        { 10LL * k_second_ns, 10.0f}
    });

    auto series         = std::make_shared<plot::series_data_t>();
    series->style       = plot::Display_style::DOTS;
    series->data_source = source;
    series->access      = make_access_policy();

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 105;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    std::vector<layer_event_t> events;
    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 1LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for non-drawable fast-path test");
    TEST_ASSERT(state_it->second.main_view.has_uploaded_vbo &&
        state_it->second.main_view.last_staged_sample_count > 1,
        "initial DOTS frame should upload multiple samples");

    source->set_samples({
        {10LL * k_second_ns, 10.0f}
    });
    series->style       = plot::Display_style::LINE;
    ctx.t0              = 10LL * k_second_ns;
    ctx.t1              = 11LL * k_second_ns;
    ctx.t_available_min = ctx.t0;
    ctx.t_available_max = ctx.t1;
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(!state_it->second.main_view.has_uploaded_vbo,
        "non-drawable singleton LINE frame must invalidate the previous upload");
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders.empty(),
        "non-drawable singleton LINE frame should not record draw commands");

    series->style = plot::Display_style::DOTS;
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(state_it->second.main_view.has_uploaded_vbo,
        "DOTS frame after non-drawable singleton should upload its own VBO");
    TEST_ASSERT(state_it->second.main_view.last_sample_upload_count == 1 &&
        state_it->second.main_view.last_staged_sample_count == 1,
        "DOTS frame after non-drawable singleton must not reuse the old two-sample upload");
    TEST_ASSERT(state_it->second.main_view.staging[0].y == 10.0f,
        "DOTS frame after non-drawable singleton should stage the singleton sample");

    return true;
}

bool test_non_rhi_prepare_invalidates_prior_upload_before_fast_path()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,                1.0f },
        { 1LL * k_second_ns,  2.0f },
        { 10LL * k_second_ns, 10.0f}
    });

    auto series         = std::make_shared<plot::series_data_t>();
    series->style       = plot::Display_style::LINE;
    series->data_source = source;
    series->access      = make_access_policy();

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 110;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    std::vector<layer_event_t> events;
    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 1LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for non-RHI prepare test");
    TEST_ASSERT(state_it->second.main_view.has_uploaded_vbo &&
        state_it->second.main_view.last_staged_sample_count > 1,
        "initial QRhi frame should upload multiple samples");
    TEST_ASSERT(!state_it->second.main_view.line_window_geometry_dirty,
        "initial QRhi LINE frame should cache its geometry");

    plot::frame_context_t non_rhi_ctx = make_context(layout, config);
    non_rhi_ctx.t0              = 10LL * k_second_ns;
    non_rhi_ctx.t1              = 11LL * k_second_ns;
    non_rhi_ctx.t_available_min = non_rhi_ctx.t0;
    non_rhi_ctx.t_available_max = non_rhi_ctx.t1;
    renderer.prepare(non_rhi_ctx, series_map);
    TEST_ASSERT(!state_it->second.main_view.has_uploaded_vbo,
        "non-RHI prepare must invalidate the previous upload for its planned window");
    TEST_ASSERT(state_it->second.main_view.line_window_geometry_dirty,
        "QRhi transitions must invalidate cached LINE geometry");

    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(
            renderer, non_rhi_ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(state_it->second.main_view.has_uploaded_vbo,
        "QRhi frame after non-RHI prepare should upload its own VBO");
    TEST_ASSERT(state_it->second.main_view.last_sample_upload_count == 1,
        "QRhi frame after non-RHI prepare must not reuse the old upload");
    TEST_ASSERT(state_it->second.main_view.last_line_window_upload_count == 1,
        "QRhi frame after non-RHI prepare must rebuild LINE geometry");
    TEST_ASSERT(state_it->second.main_view.last_staged_sample_count == 2,
        "QRhi frame after non-RHI prepare should stage the new expanded window");
    TEST_ASSERT(state_it->second.main_view.staging[0].y == 2.0f &&
        state_it->second.main_view.staging[1].y == 10.0f,
        "QRhi frame after non-RHI prepare should stage samples from the new window");

    return true;
}

bool test_nonfinite_hold_forward_policy_controls_held_sample()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    struct hold_case_t
    {
        plot::Nonfinite_sample_policy policy =
            plot::Nonfinite_sample_policy::BREAK_SEGMENT;
        std::string_view   layer_id;
        int                series_id             = 0;
        bool               expect_prepare        = false;
        std::size_t        expected_source_first = 0;
        float              expected_value        = 0.0f;
    };

    const hold_case_t cases[] = {
        { plot::Nonfinite_sample_policy::BREAK_SEGMENT,     "hold-break",  106, false, 0, 0.0f},
        { plot::Nonfinite_sample_policy::SKIP,              "hold-skip",   107, true,  0, 1.0f},
        { plot::Nonfinite_sample_policy::REJECT_WINDOW,     "hold-reject", 108, false, 0, 0.0f},
        { plot::Nonfinite_sample_policy::REPLACE_WITH_ZERO, "hold-zero",   109, true,  1, 0.0f}
    };

    for (const auto& test_case : cases) {
        std::vector<layer_event_t> events;
        int create_count = 0;
        auto layer = std::make_shared<Recording_layer>(
            std::string(test_case.layer_id), 1, 20, events, create_count);
        auto source = std::make_shared<Test_source>();
        source->set_samples({
            { 0LL, 1.0f },
            {2LL * k_second_ns, nan}
        });

        auto series = make_builtin_plus_layer_series(
            source,
            plot::Display_style::LINE,
            {layer});
        series->interpolation = plot::Series_interpolation::STEP_AFTER;
        series->empty_window_behavior =
            plot::Empty_window_behavior::HOLD_LAST_FORWARD;
        series->nonfinite_policy = test_case.policy;

        std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
        series_map[test_case.series_id] = series;

        plot::Asset_loader asset_loader;
        plot::Series_renderer renderer;
        renderer.initialize(asset_loader);

        Offscreen_rhi_fixture rhi_fixture;
        std::string error_message;
        TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

        plot::Plot_config config;
        const plot::frame_layout_result_t layout = make_layout();
        plot::frame_context_t ctx = make_context(layout, config);
        ctx.t0                                   = 3LL * k_second_ns;
        ctx.t1                                   = 4LL * k_second_ns;
        ctx.t_available_min                      = ctx.t0;
        ctx.t_available_max                      = ctx.t1;

        TEST_ASSERT(
            rhi_fixture.render_layer_frame(
                renderer, ctx, series_map, events, error_message),
            error_message);

        const layer_event_t* prepare =
            find_prepare_event(events, test_case.layer_id);
        if (!test_case.expect_prepare) {
            TEST_ASSERT(!prepare,
                "non-drawable held sample should suppress layer prepare");
            TEST_ASSERT(renderer.m_last_recorded_draw_z_orders.empty(),
                "non-drawable held sample should suppress built-in draws");
            continue;
        }

        TEST_ASSERT(prepare, "REPLACE_WITH_ZERO held sample should prepare");
        TEST_ASSERT(prepare->source_first == test_case.expected_source_first &&
            prepare->source_count == 1,
            "drawable held sample should use the expected source sample");
        TEST_ASSERT(prepare->synthetic_hold_count == 1 &&
            prepare->gpu_count == 2,
            "drawable hold should stage source plus synthetic hold samples");
        TEST_ASSERT(prepare->drawable_spans.size() == 1,
            "drawable held sample should have one drawable span");
        if (!assert_drawable_span(
                prepare->drawable_spans, 0, test_case.expected_source_first, 1, 0, 2, test_case.layer_id))
        {
            return false;
        }

        auto state_it = renderer.m_vbo_states.find(test_case.series_id);
        TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
            "expected renderer VBO state for replace-zero held sample");
        const auto& view_state = state_it->second.main_view;
        TEST_ASSERT(view_state.last_staged_sample_count == 2,
            "drawable held sample should stage two GPU samples");
        TEST_ASSERT(view_state.staging[0].y == test_case.expected_value &&
            view_state.staging[1].y == test_case.expected_value,
            "held source and synthetic samples should stage the expected value");
    }

    return true;
}

bool test_nonfinite_skip_hold_forward_preserves_earlier_held_sample_with_visible_data()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "hold-skip-visible", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL, 7.0f },
        { 2LL * k_second_ns, nan },
        {5LL * k_second_ns, 9.0f}
    });

    auto series = make_builtin_plus_layer_series(
        source,
        plot::Display_style::DOTS_LINE_AREA,
        {layer});
    series->interpolation = plot::Series_interpolation::STEP_AFTER;
    series->empty_window_behavior =
        plot::Empty_window_behavior::HOLD_LAST_FORWARD;
    series->nonfinite_policy = plot::Nonfinite_sample_policy::SKIP;

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 111;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 3LL * k_second_ns;
    ctx.t1                                   = 6LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(
            renderer, ctx, series_map, events, error_message),
        error_message);

    const layer_event_t* prepare =
        find_prepare_event(events, "hold-skip-visible");
    TEST_ASSERT(prepare,
        "SKIP held sample with later visible data should prepare a window");
    TEST_ASSERT(prepare->source_first == 0 && prepare->source_count == 3,
        "SKIP held sample should widen the containing source window backward");
    TEST_ASSERT(prepare->synthetic_hold_count == 1 &&
        prepare->gpu_count == 3,
        "SKIP held window should stage held, visible, and synthetic hold samples");
    TEST_ASSERT(prepare->drawable_spans.size() == 2,
        "SKIP held window should keep the skipped sample as a drawable gap");
    if (!assert_drawable_span(prepare->drawable_spans, 0, 0, 1, 0, 1, "hold-skip-visible") ||
        !assert_drawable_span(prepare->drawable_spans, 1, 2, 1, 1, 2, "hold-skip-visible"))
    {
        return false;
    }

    const auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for SKIP held visible test");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_staged_sample_count == 3,
        "SKIP held visible window should stage three compact GPU samples");
    TEST_ASSERT(view_state.staging[0].y == 7.0f &&
        view_state.staging[1].y == 9.0f &&
        view_state.staging[2].y == 9.0f,
        "SKIP held visible staging should preserve held, visible, and hold values");
    TEST_ASSERT(view_state.last_recorded_line_span_count == 1 &&
        view_state.last_recorded_line_segment_count == 4,
        "SKIP held visible LINE rendering should connect the compact held and visible samples");
    TEST_ASSERT(view_state.last_recorded_area_span_count == 1 &&
        view_state.last_recorded_area_segment_count == 2,
        "SKIP held visible AREA rendering should connect the compact held and visible samples");
    TEST_ASSERT(view_state.last_recorded_dot_sample_count == 3,
        "DOTS should draw the compact held and visible samples");

    return true;
}

bool test_nonfinite_skip_hold_forward_ignores_future_padding_without_visible_data()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "hold-skip-padding", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL, 7.0f },
        { 2LL * k_second_ns, nan },
        {5LL * k_second_ns, 9.0f}
    });

    auto series = make_builtin_plus_layer_series(
        source,
        plot::Display_style::DOTS_LINE_AREA,
        {layer});
    series->interpolation = plot::Series_interpolation::STEP_AFTER;
    series->empty_window_behavior =
        plot::Empty_window_behavior::HOLD_LAST_FORWARD;
    series->nonfinite_policy = plot::Nonfinite_sample_policy::SKIP;

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 112;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 3LL * k_second_ns;
    ctx.t1                                   = 4LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(
            renderer, ctx, series_map, events, error_message),
        error_message);

    const layer_event_t* prepare =
        find_prepare_event(events, "hold-skip-padding");
    TEST_ASSERT(prepare,
        "SKIP held sample should prepare even when only future padding exists");
    TEST_ASSERT(prepare->source_first == 0 && prepare->source_count == 1,
        "SKIP held-only window should collapse to the earlier drawable held sample");
    TEST_ASSERT(prepare->synthetic_hold_count == 1 &&
        prepare->gpu_count == 2,
        "SKIP held-only window should stage source plus synthetic hold samples");
    TEST_ASSERT(prepare->drawable_spans.size() == 1,
        "SKIP held-only window should have one drawable span");
    if (!assert_drawable_span(
            prepare->drawable_spans, 0, 0, 1, 0, 2, "hold-skip-padding"))
    {
        return false;
    }

    const auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for SKIP held padding test");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_staged_sample_count == 2,
        "SKIP held-only window should stage two compact GPU samples");
    TEST_ASSERT(view_state.staging[0].y == 7.0f &&
        view_state.staging[1].y == 7.0f,
        "SKIP held-only staging should ignore skipped pre-window and future padding samples");

    return true;
}

bool test_global_draw_order_sorts_builtins_across_series_and_custom_layers()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto middle_layer = std::make_shared<Recording_layer>(
        "global-middle", 1, 5, events, create_count);

    const auto make_source = []() {
        constexpr std::int64_t k_second_ns = 1'000'000'000LL;
        auto source = std::make_shared<Test_source>();
        std::vector<test_sample_t> samples;
        samples.reserve(32);
        for (std::size_t i = 0; i < 32; ++i) {
            samples.push_back({
                static_cast<std::int64_t>(i) * k_second_ns,
                static_cast<float>(i)
            });
        }
        source->set_samples(std::move(samples));
        return source;
    };

    auto first_series = make_builtin_plus_layer_series(
        make_source(),
        plot::Display_style::DOTS_LINE_AREA,
        {middle_layer});
    auto second_series = make_builtin_plus_layer_series(
        make_source(),
        plot::Display_style::DOTS_LINE_AREA,
        {});

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int first_series_id  = 70;
    const int second_series_id = 80;
    series_map[first_series_id] = first_series;
    series_map[second_series_id] = second_series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 10LL * k_second_ns;
    ctx.t1                                   = 12LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const std::vector<int> expected_z_orders = {-10, -10, 0, 0, 5, 10, 10};
    const std::vector<plot::Display_style> expected_styles = {
        plot::Display_style::AREA,
        plot::Display_style::AREA,
        plot::Display_style::LINE,
        plot::Display_style::LINE,
        plot::Display_style::NONE,
        plot::Display_style::DOTS,
        plot::Display_style::DOTS
    };
    const std::vector<int> expected_series_ids = {
        first_series_id,
        second_series_id,
        first_series_id,
        second_series_id,
        first_series_id,
        first_series_id,
        second_series_id
    };
    const std::vector<plot::Series_view_kind> expected_view_kinds(
        expected_z_orders.size(),
        plot::Series_view_kind::MAIN);

    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders == expected_z_orders,
        "global draw sort must group all AREA, LINE, custom, then DOTS z-order slots");
    TEST_ASSERT(renderer.m_last_recorded_draw_styles == expected_styles,
        "global draw sort must retain built-in/custom command identities");
    TEST_ASSERT(renderer.m_last_recorded_draw_series_ids == expected_series_ids,
        "global draw sort must keep stable ties in series order");
    TEST_ASSERT(renderer.m_last_recorded_draw_view_kinds == expected_view_kinds,
        "global draw sort should preserve main-band commands before preview commands");
    TEST_ASSERT(create_count == 1,
        "global draw sort custom layer should create one state");
    TEST_ASSERT(find_event_index(events, "global-middle", "record") < events.size(),
        "global draw sort custom layer should be recorded");

    return true;
}

bool test_builtin_draw_commands_sort_relative_to_custom_layers()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    std::vector<layer_event_t> events;
    int under_create_count  = 0;
    int middle_create_count = 0;
    int top_create_count    = 0;
    auto under_layer        = std::make_shared<Recording_layer>(
        "under", 1, -20, events, under_create_count);
    auto middle_layer = std::make_shared<Recording_layer>(
        "middle", 1, 5, events, middle_create_count);
    auto top_layer = std::make_shared<Recording_layer>(
        "top", 1, 20, events, top_create_count);

    auto source = std::make_shared<Test_source>();
    std::vector<test_sample_t> samples;
    samples.reserve(32);
    for (std::size_t i = 0; i < 32; ++i) {
        samples.push_back({
            static_cast<std::int64_t>(i) * k_second_ns,
            static_cast<float>(i)
        });
    }
    source->set_samples(std::move(samples));

    auto series = make_builtin_plus_layer_series(
        source,
        plot::Display_style::DOTS_LINE_AREA,
        {top_layer, middle_layer, under_layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 42;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 10LL * k_second_ns;
    ctx.t1                                   = 12LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const std::vector<int> expected_z_orders = {-20, -10, 0, 5, 10, 20};
    const std::vector<plot::Display_style> expected_styles = {
        plot::Display_style::NONE,
        plot::Display_style::AREA,
        plot::Display_style::LINE,
        plot::Display_style::NONE,
        plot::Display_style::DOTS,
        plot::Display_style::NONE
    };
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders == expected_z_orders,
        "built-ins and custom layers must record in merged z-order");
    TEST_ASSERT(renderer.m_last_recorded_draw_styles == expected_styles,
        "built-in command identities must match their static z-order slots");
    TEST_ASSERT(renderer.m_last_qrhi_layer_cache_size == 3,
        "custom layer cache should contain only the three custom states");
    TEST_ASSERT(under_create_count == 1 &&
        middle_create_count == 1 &&
        top_create_count == 1,
        "custom layer states should each be created once");

    const std::size_t under_record =
        find_event_index(events, "under", "record");
    const std::size_t middle_record =
        find_event_index(events, "middle", "record");
    const std::size_t top_record =
        find_event_index(events, "top", "record");
    TEST_ASSERT(under_record < middle_record && middle_record < top_record,
        "custom layer record events should preserve their relative z-order");

    return true;
}

bool test_builtins_do_not_use_qrhi_layer_cache()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    std::vector<layer_event_t> events;
    auto source = std::make_shared<Test_source>();
    std::vector<test_sample_t> samples;
    samples.reserve(32);
    for (std::size_t i = 0; i < 32; ++i) {
        samples.push_back({
            static_cast<std::int64_t>(i) * k_second_ns,
            static_cast<float>(i)
        });
    }
    source->set_samples(std::move(samples));

    auto series = make_builtin_plus_layer_series(
        source,
        plot::Display_style::DOTS_LINE_AREA,
        {});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 43;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 10LL * k_second_ns;
    ctx.t1                                   = 12LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const std::vector<int> expected_z_orders = {-10, 0, 10};
    const std::vector<plot::Display_style> expected_styles = {
        plot::Display_style::AREA,
        plot::Display_style::LINE,
        plot::Display_style::DOTS
    };
    TEST_ASSERT(renderer.m_last_recorded_draw_z_orders == expected_z_orders,
        "built-in-only series should record static AREA/LINE/DOTS z-order");
    TEST_ASSERT(renderer.m_last_recorded_draw_styles == expected_styles,
        "built-in-only series should expose only built-in draw commands");
    TEST_ASSERT(renderer.m_last_qrhi_layer_cache_size == 0,
        "built-in draw commands must not allocate custom layer cache entries");

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for built-in cache test");
    const auto& view_state = state_it->second.main_view;
    TEST_ASSERT(view_state.last_primitive_prepare_count == 3,
        "built-in-only DOTS_LINE_AREA style must prepare three primitives");
    TEST_ASSERT(view_state.last_sample_upload_count == 1,
        "built-in-only DOTS_LINE_AREA style must share one sample upload");

    return true;
}

bool test_builtin_upload_stages_visible_windows_for_dots_and_area()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    struct upload_case_t
    {
        plot::Display_style    style     = plot::Display_style::DOTS;
        std::string_view       layer_id;
        int                    series_id = 0;
    };

    const upload_case_t cases[] = {
        { plot::Display_style::DOTS, "visible-dots-upload", 33 },
        {plot::Display_style::AREA, "visible-area-upload", 34}
    };

    for (const auto& test_case : cases) {
        std::vector<layer_event_t> events;
        int create_count = 0;
        auto layer = std::make_shared<Recording_layer>(
            std::string(test_case.layer_id), 1, 20, events, create_count);
        auto source = std::make_shared<Test_source>();
        std::vector<test_sample_t> samples;
        samples.reserve(128);
        for (std::size_t i = 0; i < 128; ++i) {
            samples.push_back({
                static_cast<std::int64_t>(i) * k_second_ns,
                static_cast<float>(i)
            });
        }
        source->set_samples(std::move(samples));

        auto series = make_builtin_plus_layer_series(
            source,
            test_case.style,
            {layer});
        std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
        series_map[test_case.series_id] = series;

        plot::Asset_loader asset_loader;
        plot::Series_renderer renderer;
        renderer.initialize(asset_loader);

        Offscreen_rhi_fixture rhi_fixture;
        std::string error_message;
        TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

        plot::Plot_config config;
        const plot::frame_layout_result_t layout = make_layout();
        plot::frame_context_t ctx = make_context(layout, config);
        ctx.t0                                   = 40LL * k_second_ns;
        ctx.t1                                   = 42LL * k_second_ns;
        ctx.t_available_min                      = ctx.t0;
        ctx.t_available_max                      = ctx.t1;

        TEST_ASSERT(
            rhi_fixture.render_layer_frame(
                renderer, ctx, series_map, events, error_message),
            error_message);
        const layer_event_t* prepare =
            find_prepare_event(events, test_case.layer_id);
        TEST_ASSERT(prepare, "expected visible dots/area layer prepare event");
        TEST_ASSERT(prepare->snapshot_count == source->sample_count(),
            "external layer should still receive the full frame snapshot");
        TEST_ASSERT(prepare->source_count > 0,
            "visible source window should contain samples");
        TEST_ASSERT(prepare->source_count < prepare->snapshot_count,
            "visible source window should be smaller than the full source snapshot");
        TEST_ASSERT(prepare->synthetic_hold_count == 0,
            "ordinary visible dots/area upload should not add a synthetic hold sample");
        TEST_ASSERT(prepare->gpu_count == prepare->source_count,
            "visible dots/area upload should stage one GPU sample per source sample");

        if (!assert_compact_upload_state(
                renderer, test_case.series_id, *prepare, 0, test_case.layer_id))
        {
            return false;
        }
    }

    return true;
}

bool test_builtin_upload_stages_single_synthetic_hold_sample()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "hold-upload", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f}
    });

    auto series = std::make_shared<plot::rhi_series_data_t>();
    series->style                 = plot::Display_style::LINE;
    series->interpolation         = plot::Series_interpolation::STEP_AFTER;
    series->empty_window_behavior = plot::Empty_window_behavior::HOLD_LAST_FORWARD;
    series->data_source           = source;
    series->access                = make_access_policy();
    series->qrhi_layers           = {layer};

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 32;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 10LL * k_second_ns;
    ctx.t1                                   = 12LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* prepare = find_prepare_event(events, "hold-upload");
    TEST_ASSERT(prepare, "expected hold-upload layer prepare event");
    TEST_ASSERT(prepare->source_first == 2,
        "hold-forward source window should start at the last real source sample");
    TEST_ASSERT(prepare->source_count == 1,
        "hold-forward source window should keep exactly one real source sample");
    TEST_ASSERT(prepare->synthetic_hold_count == 1,
        "hold-forward upload should add exactly one synthetic GPU sample");
    TEST_ASSERT(prepare->gpu_count == 2,
        "hold-forward upload should stage one real and one synthetic GPU sample");

    if (!assert_compact_upload_state(
            renderer, series_id, *prepare, 3, "hold-forward line"))
    {
        return false;
    }

    return true;
}

bool test_builtin_upload_stages_hold_windows_for_dots_and_area()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    struct upload_case_t
    {
        plot::Display_style    style     = plot::Display_style::DOTS;
        std::string_view       layer_id;
        int                    series_id = 0;
    };

    const upload_case_t cases[] = {
        { plot::Display_style::DOTS, "hold-dots-upload", 35 },
        {plot::Display_style::AREA, "hold-area-upload", 36}
    };

    for (const auto& test_case : cases) {
        std::vector<layer_event_t> events;
        int create_count = 0;
        auto layer = std::make_shared<Recording_layer>(
            std::string(test_case.layer_id), 1, 20, events, create_count);
        auto source = std::make_shared<Test_source>();
        source->set_samples({
            { 0LL,               1.0f},
            { 1LL * k_second_ns, 2.0f},
            { 2LL * k_second_ns, 3.0f}
        });

        auto series = std::make_shared<plot::rhi_series_data_t>();
        series->style         = test_case.style;
        series->interpolation = plot::Series_interpolation::STEP_AFTER;
        series->empty_window_behavior =
            plot::Empty_window_behavior::HOLD_LAST_FORWARD;
        series->data_source = source;
        series->access      = make_access_policy();
        series->qrhi_layers = {layer};

        std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
        series_map[test_case.series_id] = series;

        plot::Asset_loader asset_loader;
        plot::Series_renderer renderer;
        renderer.initialize(asset_loader);

        Offscreen_rhi_fixture rhi_fixture;
        std::string error_message;
        TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

        plot::Plot_config config;
        const plot::frame_layout_result_t layout = make_layout();
        plot::frame_context_t ctx = make_context(layout, config);
        ctx.t0                                   = 10LL * k_second_ns;
        ctx.t1                                   = 12LL * k_second_ns;
        ctx.t_available_min                      = ctx.t0;
        ctx.t_available_max                      = ctx.t1;

        TEST_ASSERT(
            rhi_fixture.render_layer_frame(
                renderer, ctx, series_map, events, error_message),
            error_message);
        const layer_event_t* prepare =
            find_prepare_event(events, test_case.layer_id);
        TEST_ASSERT(prepare, "expected hold dots/area layer prepare event");
        TEST_ASSERT(prepare->source_first == 2,
            "hold-forward dots/area source window should start at last source sample");
        TEST_ASSERT(prepare->source_count == 1,
            "hold-forward dots/area source window should keep one real source sample");
        TEST_ASSERT(prepare->synthetic_hold_count == 1,
            "hold-forward dots/area upload should add one synthetic GPU sample");
        TEST_ASSERT(prepare->gpu_count == 2,
            "hold-forward dots/area upload should stage real and synthetic samples");

        if (!assert_compact_upload_state(
                renderer, test_case.series_id, *prepare, 0, test_case.layer_id))
        {
            return false;
        }
    }

    return true;
}

bool test_resources_changed_tracks_data_and_window_changes()
{
    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "tracked", 1, 0, events, create_count);
    auto source = std::make_shared<Test_source>();
    auto series = make_layer_only_series(source, {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    series_map[9] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* initial_prepare = find_prepare_event(events, "tracked");
    TEST_ASSERT(initial_prepare && initial_prepare->resources_changed,
        "first prepare must report resources_changed");

    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* stable_prepare = find_prepare_event(events, "tracked");
    TEST_ASSERT(stable_prepare && !stable_prepare->resources_changed,
        "unchanged data and window must not report resources_changed");

    access_call_counts_t changed_access_calls;
    series->access = make_fallback_access_policy_with_counted_public_accessors(
        changed_access_calls);
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* access_prepare = find_prepare_event(events, "tracked");
    TEST_ASSERT(access_prepare && access_prepare->resources_changed,
        "access policy change must report resources_changed");
    TEST_ASSERT(changed_access_calls.timestamp > 0,
        "access policy change should replan with the replacement timestamp accessor");

    source->notify_changed();
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* data_prepare = find_prepare_event(events, "tracked");
    TEST_ASSERT(data_prepare && data_prepare->resources_changed,
        "sample sequence change must report resources_changed");

    events.clear();
    ctx.t0 = 4'000'000'000LL;
    ctx.t1 = 7'000'000'000LL;
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* window_prepare = find_prepare_event(events, "tracked");
    TEST_ASSERT(window_prepare && window_prepare->resources_changed,
        "visible window change must report resources_changed");
    TEST_ASSERT(window_prepare->t_min_ns == ctx.t0 && window_prepare->t_max_ns == ctx.t1,
        "prepare context must carry the changed visible window");

    return true;
}

bool test_resources_changed_tracks_hold_timestamp_changes()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "hold-key", 1, 20, events, create_count);
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f}
    });

    auto series = std::make_shared<plot::rhi_series_data_t>();
    series->style         = plot::Display_style::LINE;
    series->interpolation = plot::Series_interpolation::STEP_AFTER;
    series->empty_window_behavior =
        plot::Empty_window_behavior::HOLD_LAST_FORWARD;
    series->data_source = source;
    series->access      = make_access_policy();
    series->qrhi_layers = {layer};

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    series_map[37] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 10LL * k_second_ns;
    ctx.t1                                   = 12LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* first_prepare = find_prepare_event(events, "hold-key");
    TEST_ASSERT(first_prepare && first_prepare->resources_changed,
        "first hold-forward prepare must report resources_changed");
    TEST_ASSERT(first_prepare->source_first == 2 &&
        first_prepare->source_count == 1 &&
        first_prepare->synthetic_hold_count == 1 &&
        first_prepare->gpu_count == 2,
        "first hold-forward prepare should draw one real and one synthetic sample");

    const layer_event_t first        = *first_prepare;
    const std::int64_t  first_origin = first.t_origin_ns;

    events.clear();
    ctx.t1              = 13LL * k_second_ns;
    ctx.t_available_max = ctx.t1;
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* second_prepare = find_prepare_event(events, "hold-key");
    TEST_ASSERT(second_prepare,
        "second hold-forward prepare must run");
    TEST_ASSERT(second_prepare->t_origin_ns == first_origin,
        "hold timestamp test requires both frames to stay in the same origin bucket");
    TEST_ASSERT(second_prepare->source_first == first.source_first &&
        second_prepare->source_count == first.source_count &&
        second_prepare->synthetic_hold_count ==
            first.synthetic_hold_count &&
        second_prepare->gpu_count == first.gpu_count,
        "hold timestamp test requires unchanged source/gpu window metadata");
    TEST_ASSERT(second_prepare->resources_changed,
        "changed hold-forward timestamp must invalidate external layer data key");

    return true;
}

bool test_busy_stale_fallback_rejects_changed_request_shape()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    struct stale_case_t
    {
        std::string    label;
        plot::Empty_window_behavior empty_behavior =
            plot::Empty_window_behavior::DRAW_NOTHING;
        plot::Series_interpolation interpolation =
            plot::Series_interpolation::LINEAR;
        std::int64_t   initial_t0    = 0;
        std::int64_t   initial_t1    = 0;
        std::int64_t   busy_t0       = 0;
        std::int64_t   busy_t1       = 0;
        double         initial_width = 240.0;
        double         busy_width    = 240.0;
    };

    const std::vector<stale_case_t> cases = {
        {
            "changed t_min",
            plot::Empty_window_behavior::DRAW_NOTHING,
            plot::Series_interpolation::LINEAR,
            0,
            3LL * k_second_ns,
            1LL * k_second_ns,
            3LL * k_second_ns,
            240.0,
            240.0
        },
        {
            "changed t_max",
            plot::Empty_window_behavior::DRAW_NOTHING,
            plot::Series_interpolation::LINEAR,
            0,
            3LL * k_second_ns,
            0,
            4LL * k_second_ns,
            240.0,
            240.0
        },
        {
            "changed width",
            plot::Empty_window_behavior::DRAW_NOTHING,
            plot::Series_interpolation::LINEAR,
            0,
            3LL * k_second_ns,
            0,
            3LL * k_second_ns,
            240.0,
            180.0
        },
        {
            "hold-forward changed t_min with same t_max",
            plot::Empty_window_behavior::HOLD_LAST_FORWARD,
            plot::Series_interpolation::STEP_AFTER,
            10LL * k_second_ns,
            12LL * k_second_ns,
            11LL * k_second_ns,
            12LL * k_second_ns,
            240.0,
            240.0
        }
    };

    int series_id = 90;
    for (const auto& test_case : cases) {
        std::vector<layer_event_t> events;
        auto source = std::make_shared<Test_source>();
        source->set_samples({
            { 0LL,               1.0f},
            { 1LL * k_second_ns, 2.0f},
            { 2LL * k_second_ns, 3.0f},
            { 3LL * k_second_ns, 4.0f},
            { 4LL * k_second_ns, 5.0f},
            { 5LL * k_second_ns, 6.0f},
            { 6LL * k_second_ns, 7.0f},
            { 7LL * k_second_ns, 8.0f}
        });

        auto series = std::make_shared<plot::series_data_t>();
        series->style                 = plot::Display_style::LINE;
        series->interpolation         = test_case.interpolation;
        series->empty_window_behavior = test_case.empty_behavior;
        series->data_source           = source;
        series->access                = make_access_policy();

        std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
        series_map[series_id] = series;

        plot::Asset_loader asset_loader;
        plot::Series_renderer renderer;
        renderer.initialize(asset_loader);

        Offscreen_rhi_fixture rhi_fixture;
        std::string error_message;
        TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

        plot::Plot_config config;
        plot::frame_layout_result_t layout = make_layout();
        layout.usable_width = test_case.initial_width;
        plot::frame_context_t ctx = make_context(layout, config);
        ctx.t0              = test_case.initial_t0;
        ctx.t1              = test_case.initial_t1;
        ctx.t_available_min = ctx.t0;
        ctx.t_available_max = ctx.t1;

        TEST_ASSERT(
            rhi_fixture.render_layer_frame(
                renderer, ctx, series_map, events, error_message),
            error_message);
        auto state_it = renderer.m_vbo_states.find(series_id);
        TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
            test_case.label + ": expected renderer VBO state");
        const auto& initial_view_state = state_it->second.main_view;
        TEST_ASSERT(initial_view_state.last_staged_sample_count > 0,
            test_case.label + ": initial frame should stage samples");

        const std::int64_t prepared_t_min =
            initial_view_state.last_prepared_t_min_ns;
        const std::int64_t prepared_t_max =
            initial_view_state.last_prepared_t_max_ns;
        const double prepared_width =
            initial_view_state.last_prepared_width_px;
        const std::size_t vbo_generation =
            initial_view_state.last_vbo_generation;

        source->return_busy_once();
        events.clear();
        plot::frame_layout_result_t busy_layout = make_layout();
        busy_layout.usable_width = test_case.busy_width;
        plot::frame_context_t busy_ctx = make_context(busy_layout, config);
        busy_ctx.t0              = test_case.busy_t0;
        busy_ctx.t1              = test_case.busy_t1;
        busy_ctx.t_available_min = busy_ctx.t0;
        busy_ctx.t_available_max = busy_ctx.t1;
        TEST_ASSERT(
            rhi_fixture.render_layer_frame(
                renderer, busy_ctx, series_map, events, error_message),
            error_message);

        const auto& busy_view_state = state_it->second.main_view;
        TEST_ASSERT(busy_view_state.last_prepared_t_min_ns == prepared_t_min,
            test_case.label + ": BUSY frame must not reuse stale VBO with changed t_min");
        TEST_ASSERT(busy_view_state.last_prepared_t_max_ns == prepared_t_max,
            test_case.label + ": BUSY frame must not reuse stale VBO with changed t_max");
        TEST_ASSERT(busy_view_state.last_prepared_width_px == prepared_width,
            test_case.label + ": BUSY frame must not reuse stale VBO with changed width");
        TEST_ASSERT(busy_view_state.last_vbo_generation == vbo_generation,
            test_case.label + ": BUSY frame must not reallocate or reupload stale VBO data");

        ++series_id;
    }

    return true;
}

bool test_busy_hold_forward_does_not_prepare_stale_tmax()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    std::vector<layer_event_t> events;
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f}
    });

    auto series = std::make_shared<plot::series_data_t>();
    series->style         = plot::Display_style::LINE;
    series->interpolation = plot::Series_interpolation::STEP_AFTER;
    series->empty_window_behavior =
        plot::Empty_window_behavior::HOLD_LAST_FORWARD;
    series->data_source = source;
    series->access      = make_access_policy();

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 39;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 10LL * k_second_ns;
    ctx.t1                                   = 12LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for busy hold-forward test");
    const auto& initial_view_state = state_it->second.main_view;
    TEST_ASSERT(initial_view_state.last_staged_sample_count == 2,
        "initial hold-forward frame should stage real and synthetic samples");
    TEST_ASSERT(initial_view_state.last_prepared_t_max_ns == ctx.t1,
        "initial hold-forward frame should prepare at its visible t_max");

    const std::int64_t prepared_t_max = initial_view_state.last_prepared_t_max_ns;
    const std::size_t  vbo_generation = initial_view_state.last_vbo_generation;
    source->return_busy_once();
    ctx.t1              = 13LL * k_second_ns;
    ctx.t_available_max = ctx.t1;
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const auto& busy_view_state = state_it->second.main_view;
    TEST_ASSERT(busy_view_state.last_prepared_t_max_ns == prepared_t_max,
        "busy snapshot with changed hold timestamp must not prepare stale VBO data");
    TEST_ASSERT(busy_view_state.last_vbo_generation == vbo_generation,
        "busy snapshot with changed hold timestamp must not reupload stale VBO data");

    return true;
}

bool test_busy_hold_forward_does_not_reuse_non_hold_window()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    std::vector<layer_event_t> events;
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f}
    });

    auto series = std::make_shared<plot::series_data_t>();
    series->style         = plot::Display_style::LINE;
    series->interpolation = plot::Series_interpolation::STEP_AFTER;
    series->empty_window_behavior =
        plot::Empty_window_behavior::HOLD_LAST_FORWARD;
    series->data_source = source;
    series->access      = make_access_policy();

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 40;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 2LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for busy non-hold-to-hold test");
    const auto& initial_view_state = state_it->second.main_view;
    TEST_ASSERT(initial_view_state.last_staged_sample_count >= 2,
        "initial non-hold frame should stage drawable source samples");
    TEST_ASSERT(initial_view_state.last_prepared_t_max_ns == ctx.t1,
        "initial non-hold frame should prepare at its visible t_max");

    const std::int64_t prepared_t_max =
        initial_view_state.last_prepared_t_max_ns;
    const std::size_t vbo_generation =
        initial_view_state.last_vbo_generation;
    source->return_busy_once();
    ctx.t1              = 3LL * k_second_ns;
    ctx.t_available_max = ctx.t1;
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const auto& busy_view_state = state_it->second.main_view;
    TEST_ASSERT(busy_view_state.last_prepared_t_max_ns == prepared_t_max,
        "busy non-hold-to-hold transition must not prepare stale VBO data");
    TEST_ASSERT(busy_view_state.last_vbo_generation == vbo_generation,
        "busy non-hold-to-hold transition must not reupload stale VBO data");

    return true;
}

bool test_external_layer_gets_snapshot_on_builtin_cache_hit()
{
    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "cached", 1, 0, events, create_count);
    auto source = std::make_shared<Test_source>();
    auto series = make_line_plus_layer_series(source, {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 21;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* initial_prepare = find_prepare_event(events, "cached");
    TEST_ASSERT(initial_prepare && initial_prepare->snapshot_valid,
        "initial prepare must receive a valid snapshot");
    TEST_ASSERT(initial_prepare->snapshot_count > 0,
        "initial prepare snapshot must contain samples");

    const int snapshots_after_initial_frame = source->snapshot_calls();

    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* stable_prepare = find_prepare_event(events, "cached");
    TEST_ASSERT(stable_prepare, "expected external prepare on stable built-in cache-hit frame");
    TEST_ASSERT(!stable_prepare->resources_changed,
        "unchanged data and window must keep resources_changed false");
    TEST_ASSERT(stable_prepare->snapshot_valid,
        "stable built-in cache-hit frame must still provide the external layer snapshot");
    TEST_ASSERT(stable_prepare->snapshot_count > 0,
        "stable built-in cache-hit snapshot must contain samples");
    TEST_ASSERT(source->snapshot_calls() > snapshots_after_initial_frame,
        "external layer view must acquire a frame snapshot instead of taking the no-snapshot fast path");

    std::weak_ptr<void> hold = source->last_hold();
    TEST_ASSERT(hold.expired(),
        "external layer cache-hit snapshot hold must release after render");

    return true;
}

bool test_external_layer_skips_busy_stale_fallback_and_recovers()
{
    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "busy", 1, 0, events, create_count);
    auto source = std::make_shared<Test_source>();
    auto series = make_line_plus_layer_series(source, {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 22;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(find_prepare_event(events, "busy"),
        "expected initial external prepare before BUSY simulation");
    TEST_ASSERT(create_count == 1,
        "expected one external layer state after initial prepare");

    source->return_busy_once();
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(!find_prepare_event(events, "busy"),
        "external layer must not prepare from stale fallback without a valid snapshot");
    TEST_ASSERT(create_count == 1,
        "transient BUSY snapshot must not recreate external layer state");

    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* recovered_prepare = find_prepare_event(events, "busy");
    TEST_ASSERT(recovered_prepare && recovered_prepare->snapshot_valid,
        "external layer must prepare with a valid snapshot after BUSY recovery");
    TEST_ASSERT(recovered_prepare->snapshot_count > 0,
        "recovered external prepare snapshot must contain samples");
    TEST_ASSERT(create_count == 1,
        "external layer state must survive transient BUSY snapshot recovery");

    return true;
}

bool test_external_layer_replans_when_snapshot_advances_after_sequence_probe()
{
    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "advance", 1, 0, events, create_count);
    auto source = std::make_shared<Test_source>();
    auto series = make_line_plus_layer_series(source, {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int series_id = 23;
    series_map[series_id] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* initial_prepare = find_prepare_event(events, "advance");
    TEST_ASSERT(initial_prepare && initial_prepare->snapshot_valid,
        "expected initial external prepare with a snapshot");

    source->advance_during_next_snapshot();
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    const layer_event_t* advanced_prepare = find_prepare_event(events, "advance");
    TEST_ASSERT(advanced_prepare && advanced_prepare->snapshot_valid,
        "external prepare must still receive a valid snapshot after source advancement");
    TEST_ASSERT(advanced_prepare->sample_sequence == advanced_prepare->snapshot_sequence,
        "cache-hit metadata must be replanned when the acquired snapshot sequence advances");
    TEST_ASSERT(advanced_prepare->resources_changed,
        "advanced snapshot sequence must invalidate the external layer data key");

    return true;
}

bool test_layer_state_recreated_for_program_identity_changes()
{
    std::vector<layer_event_t> events;
    int create_count = 0;
    auto layer = std::make_shared<Recording_layer>(
        "program", 1, 0, events, create_count);
    auto source = std::make_shared<Test_source>();
    auto series = make_layer_only_series(source, {layer});
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    series_map[11] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);

    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(create_count == 1, "initial frame must create one layer state");

    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(create_count == 1, "unchanged program key must reuse the layer state");

    layer->set_revision(2);
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(create_count == 2, "revision change must create a new layer state");

    layer->set_id("program-renamed");
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(create_count == 3, "layer id change must create a new layer state");

    int identity_token = 0;
    source->set_identity(&identity_token);
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(create_count == 4, "data identity change must create a new layer state");

    return true;
}

// Item #1 regression: a range-only access policy (timestamp + range, no
// primary value) is valid for auto-range and custom range-band layers, but the
// built-in LINE/DOTS/AREA styles read only the value lane. Feeding such a
// policy to a value style previously rendered a flat line at y = 0; the
// dispatch gate must instead skip every built-in primitive. A value policy over
// an identical window must still draw, proving the window is renderable and
// only the missing value accessor suppresses the built-ins.
//
// (The companion item #2 fix -- treating a failed SRB rebuild as a skipped
// draw rather than binding an unusable object -- has no regression test here:
// the Null QRhi backend used by Offscreen_rhi_fixture never fails create(), so
// the failure branch is not inducible without a mock RHI seam. It stays covered
// implicitly by the existing prepare-path tests exercising the success branch.)
bool test_range_only_access_skips_builtin_value_styles()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    auto range_source = std::make_shared<Test_source>();
    range_source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f},
        { 3LL * k_second_ns, 4.0f}
    });
    auto value_source = std::make_shared<Test_source>();
    value_source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f},
        { 3LL * k_second_ns, 4.0f}
    });

    auto range_series = make_builtin_plus_layer_series(
        range_source, plot::Display_style::DOTS_LINE_AREA, {});
    range_series->access = make_range_only_access_policy();

    auto value_series = make_builtin_plus_layer_series(
        value_source, plot::Display_style::DOTS_LINE_AREA, {});

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    const int range_series_id = 130;
    const int value_series_id = 131;
    series_map[range_series_id] = range_series;
    series_map[value_series_id] = value_series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.t0                                   = 0;
    ctx.t1                                   = 3LL * k_second_ns;
    ctx.t_available_min                      = ctx.t0;
    ctx.t_available_max                      = ctx.t1;

    std::vector<layer_event_t> events;
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(
            renderer, ctx, series_map, events, error_message),
        error_message);

    // The value-policy series proves the shared window is renderable: it draws
    // built-in primitives over the same four samples.
    auto value_it = renderer.m_vbo_states.find(value_series_id);
    TEST_ASSERT(value_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for the value-policy contrast series");
    const auto& value_view = value_it->second.main_view;
    TEST_ASSERT(value_view.last_primitive_prepare_count > 0,
        "value-policy series should prepare built-in primitives");
    TEST_ASSERT(value_view.last_recorded_line_segment_count > 0,
        "value-policy series should record line segments");
    TEST_ASSERT(value_view.last_recorded_area_segment_count > 0,
        "value-policy series should record area segments");
    TEST_ASSERT(value_view.last_recorded_dot_sample_count > 0,
        "value-policy series should record dot samples");

    // The range-only series shares an identical window but lacks a value
    // accessor: the dispatch gate must skip every built-in primitive rather
    // than silently record a flat y = 0 line.
    auto range_it = renderer.m_vbo_states.find(range_series_id);
    TEST_ASSERT(range_it != renderer.m_vbo_states.end(),
        "expected renderer VBO state for the range-only series");
    const auto& range_view = range_it->second.main_view;
    TEST_ASSERT(range_view.last_primitive_prepare_count == 0,
        "range-only access must not prepare any built-in value primitive");
    TEST_ASSERT(range_view.last_recorded_line_segment_count == 0 &&
        range_view.last_recorded_line_span_count == 0,
        "range-only access must record no line geometry");
    TEST_ASSERT(range_view.last_recorded_area_segment_count == 0 &&
        range_view.last_recorded_area_span_count == 0,
        "range-only access must record no area geometry");
    TEST_ASSERT(range_view.last_recorded_dot_sample_count == 0,
        "range-only access must record no dots");

    return true;
}

bool test_stacked_sum_overlay_uses_top_geometry_and_theme()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    const auto make_source = [&](float offset) {
        auto source = std::make_shared<Test_source>();
        source->set_samples({
            { 0LL, offset + 1.0f },
            { 1LL * k_second_ns, offset + 2.0f },
            { 2LL * k_second_ns, offset + 3.0f },
            {3LL * k_second_ns,   offset + 4.0f}
        });
        return source;
    };
    const auto make_series = [&](int group, plot::Display_style style,
        float offset, const glm::vec4& color) {
        auto series         = std::make_shared<plot::series_data_t>();
        series->data_source = make_source(offset);
        series->access      = make_access_policy();
        series->stack_group = group;
        series->style       = style;
        series->color       = color;
        return series;
    };

    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    series_map[10] = make_series(
        1, plot::Display_style::AREA, 0.0f, glm::vec4(0.8f, 0.1f, 0.2f, 1.0f));
    series_map[20] = make_series(
        1, plot::Display_style::AREA, 10.0f, glm::vec4(0.1f, 0.8f, 0.2f, 1.0f));
    series_map[30] = make_series(
        2, plot::Display_style::DOTS, 20.0f, glm::vec4(0.1f, 0.2f, 0.8f, 1.0f));
    series_map[40] = make_series(
        2, plot::Display_style::DOTS, 30.0f, glm::vec4(0.8f, 0.2f, 0.7f, 1.0f));
    series_map[45] = make_series(
        3, plot::Display_style::LINE, 40.0f, glm::vec4(0.2f, 0.7f, 0.8f, 1.0f));
    series_map[50] = make_series(
        3, plot::Display_style::LINE, 45.0f, glm::vec4(0.7f, 0.8f, 0.2f, 1.0f));
    series_map[55] = make_series(
        5, plot::Display_style::LINE, 48.0f, glm::vec4(0.3f, 0.6f, 0.9f, 1.0f));
    auto rejected_lower = make_series(
        4, plot::Display_style::LINE, 50.0f, glm::vec4(0.4f, 0.5f, 0.6f, 1.0f));
    auto rejected_upper = make_series(
        4, plot::Display_style::LINE, 60.0f, glm::vec4(0.6f, 0.5f, 0.4f, 1.0f));
    rejected_upper->interpolation = plot::Series_interpolation::STEP_AFTER;
    series_map[60]                = rejected_lower;
    series_map[70]                = rejected_upper;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);

    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    config.line_width_px = 1.5;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    ctx.adjusted_preview_height              = 40.0;
    ctx.preview_v0                           = 0.0f;
    ctx.preview_v1                           = 80.0f;
    ctx.dark_mode                            = true;
    std::vector<layer_event_t> events;
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);

    const auto close_color = [](const glm::vec4& actual, const glm::vec4& expected) {
        return glm::all(glm::lessThan(
            glm::abs(actual - expected), glm::vec4(0.0001f)));
    };
    const glm::vec4 dark_sum_color(
        230.0f / 255.0f, 223.0f / 255.0f, 204.0f / 255.0f, 1.0f);
    std::size_t overlay_count = 0;
    for (std::size_t i = 0; i < renderer.m_last_recorded_stack_sum_overlays.size(); ++i) {
        if (!renderer.m_last_recorded_stack_sum_overlays[i]) {
            continue;
        }
        ++overlay_count;
        TEST_ASSERT(renderer.m_last_recorded_draw_styles[i] == plot::Display_style::LINE,
            "stack sum overlay must use the built-in LINE path");
        TEST_ASSERT(renderer.m_last_recorded_draw_z_orders[i] == 20,
            "stack sum overlay must record above component primitives");
        TEST_ASSERT(renderer.m_last_recorded_draw_series_ids[i] == 20 ||
            renderer.m_last_recorded_draw_series_ids[i] == 40 ||
            renderer.m_last_recorded_draw_series_ids[i] == 50,
            "only the final member of each successful stack may own an overlay");
        TEST_ASSERT(close_color(renderer.m_last_recorded_draw_colors[i], dark_sum_color),
            "dark stack sum overlay color must be #E6DFCC");
        TEST_ASSERT(renderer.m_last_recorded_line_widths[i] == 3.5f,
            "stack sum overlay must be two pixels thicker than configured lines");
        for (std::size_t j = i + 1; j < renderer.m_last_recorded_draw_view_kinds.size(); ++j) {
            TEST_ASSERT(renderer.m_last_recorded_draw_view_kinds[j] !=
                renderer.m_last_recorded_draw_view_kinds[i] ||
                renderer.m_last_recorded_draw_z_orders[j] >= 20,
                "stack sum overlay must follow every component primitive in its view");
        }
    }
    TEST_ASSERT(overlay_count == 6,
        "three successful groups must produce one main and one preview overlay each");

    const auto top_state = renderer.m_vbo_states.find(20);
    TEST_ASSERT(top_state != renderer.m_vbo_states.end(),
        "top stack member must retain renderer state");
    TEST_ASSERT(top_state->second.main_view.staging.size() == 4 &&
        top_state->second.main_view.staging.front().y == 12.0f &&
        top_state->second.main_view.staging.back().y == 18.0f,
        "stack sum overlay must reuse the final cumulative layer geometry");
    TEST_ASSERT(top_state->second.main_view.last_sample_upload_count == 1 &&
        top_state->second.main_view.last_line_window_upload_count == 1,
        "AREA-only stack top and sum overlay must share one sample and LINE upload");
    const auto line_top_state = renderer.m_vbo_states.find(50);
    TEST_ASSERT(line_top_state != renderer.m_vbo_states.end() &&
        line_top_state->second.main_view.last_line_window_upload_count == 1,
        "LINE stack top and sum overlay must not duplicate the LINE-window upload");

    bool lower_component_color_preserved = false;
    for (std::size_t i = 0; i < renderer.m_last_recorded_draw_series_ids.size(); ++i) {
        if (renderer.m_last_recorded_draw_series_ids[i] == 10 &&
            !renderer.m_last_recorded_stack_sum_overlays[i])
        {
            lower_component_color_preserved = close_color(
                renderer.m_last_recorded_draw_colors[i],
                glm::vec4(0.8f, 0.1f, 0.2f, 0.3f));
        }
    }
    TEST_ASSERT(lower_component_color_preserved,
        "sum overlay must not recolor component primitives");

    ctx.dark_mode = false;
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(top_state->second.main_view.last_line_window_upload_count == 0 &&
        top_state->second.preview_view.last_line_window_upload_count == 0 &&
        top_state->second.main_view.last_recorded_line_segment_count > 0 &&
        top_state->second.preview_view.last_recorded_line_segment_count > 0,
        "unchanged AREA stack overlays must draw in main and preview without LINE uploads");
    TEST_ASSERT(line_top_state->second.main_view.last_line_window_upload_count == 0 &&
        line_top_state->second.preview_view.last_line_window_upload_count == 0,
        "unchanged ordinary LINE and sum overlay must share retained geometry");
    const glm::vec4 light_sum_color(
        25.0f / 255.0f, 32.0f / 255.0f, 51.0f / 255.0f, 1.0f);
    for (std::size_t i = 0; i < renderer.m_last_recorded_stack_sum_overlays.size(); ++i) {
        if (renderer.m_last_recorded_stack_sum_overlays[i]) {
            TEST_ASSERT(close_color(
                renderer.m_last_recorded_draw_colors[i], light_sum_color),
                "light stack sum overlay color must be #192033");
        }
    }

    auto* changed_source = dynamic_cast<Test_source*>(series_map.at(10)->main_source());
    TEST_ASSERT(changed_source, "stack fixture should expose its mutable source");
    changed_source->set_samples({
        { 0LL,               101.0f},
        { 1LL * k_second_ns, 102.0f},
        { 2LL * k_second_ns, 103.0f},
        { 3LL * k_second_ns, 104.0f}
    });
    events.clear();
    TEST_ASSERT(
        rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
        error_message);
    TEST_ASSERT(top_state->second.main_view.last_line_window_upload_count == 1 &&
        top_state->second.preview_view.last_line_window_upload_count == 1 &&
        top_state->second.main_view.line_window_staging.front().y == 112.0f,
        "changed stack geometry must rebuild each AREA sum overlay once");

    // The two theme defaults above are defaults, not fixed ink: a host that
    // paints its own plot background has to be able to restate the overlay's
    // color and its extra width per theme. Both dark and light are checked so
    // an override cannot land on one palette and leave the other hardcoded.
    const glm::vec4 override_dark_color(0.20f, 0.40f, 0.60f, 1.0f);
    const glm::vec4 override_light_color(0.60f, 0.40f, 0.20f, 1.0f);
    config.dark_color_palette.stack_sum_line  = override_dark_color;
    config.light_color_palette.stack_sum_line = override_light_color;
    config.stack_sum_line_width_extra_px      = 0.5;
    for (const bool dark_mode : {true, false}) {
        ctx.dark_mode = dark_mode;
        events.clear();
        TEST_ASSERT(
            rhi_fixture.render_layer_frame(renderer, ctx, series_map, events, error_message),
            error_message);
        std::size_t overridden_overlay_count = 0;
        for (std::size_t i = 0; i < renderer.m_last_recorded_stack_sum_overlays.size(); ++i) {
            if (!renderer.m_last_recorded_stack_sum_overlays[i]) {
                continue;
            }
            ++overridden_overlay_count;
            TEST_ASSERT(close_color(
                renderer.m_last_recorded_draw_colors[i],
                dark_mode ? override_dark_color : override_light_color),
                "stack sum overlay must take its color from the configured palette");
            TEST_ASSERT(renderer.m_last_recorded_line_widths[i] == 2.0f,
                "stack sum overlay must add the configured extra width to line_width_px");
        }
        TEST_ASSERT(overridden_overlay_count == 6,
            "overriding the overlay theme must not change which draws are overlays");
    }

    // Component primitives keep their own series color under an override.
    bool overridden_component_color_preserved = false;
    for (std::size_t i = 0; i < renderer.m_last_recorded_draw_series_ids.size(); ++i) {
        if (renderer.m_last_recorded_draw_series_ids[i] == 10 &&
            !renderer.m_last_recorded_stack_sum_overlays[i])
        {
            overridden_component_color_preserved = close_color(
                renderer.m_last_recorded_draw_colors[i],
                glm::vec4(0.8f, 0.1f, 0.2f, 0.3f));
        }
    }
    TEST_ASSERT(overridden_component_color_preserved,
        "configured overlay theme must not recolor component primitives");

    return true;
}

bool test_line_geometry_reuses_and_rebuilds_after_area_only_change()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    auto source = std::make_shared<Test_source>();
    source->set_samples({
        { 0LL,               1.0f},
        { 1LL * k_second_ns, 2.0f},
        { 2LL * k_second_ns, 3.0f},
        { 3LL * k_second_ns, 4.0f}
    });

    auto series         = std::make_shared<plot::series_data_t>();
    series->style       = plot::Display_style::LINE;
    series->data_source = source;
    series->access      = make_access_policy();
    std::map<int, std::shared_ptr<const plot::series_data_t>> series_map;
    series_map[1] = series;

    plot::Asset_loader asset_loader;
    plot::Series_renderer renderer;
    renderer.initialize(asset_loader);
    Offscreen_rhi_fixture rhi_fixture;
    std::string error_message;
    TEST_ASSERT(rhi_fixture.initialize(error_message), error_message);

    plot::Plot_config config;
    const plot::frame_layout_result_t layout = make_layout();
    plot::frame_context_t ctx = make_context(layout, config);
    std::vector<layer_event_t> events;

    TEST_ASSERT(rhi_fixture.render_layer_frame(
        renderer, ctx, series_map, events, error_message), error_message);
    auto state = renderer.m_vbo_states.find(1);
    TEST_ASSERT(state != renderer.m_vbo_states.end() &&
        state->second.main_view.last_line_window_upload_count == 1,
        "cold LINE frame should upload its geometry once");

    TEST_ASSERT(rhi_fixture.render_layer_frame(
        renderer, ctx, series_map, events, error_message), error_message);
    TEST_ASSERT(state->second.main_view.last_line_window_upload_count == 0 &&
        state->second.main_view.last_recorded_line_segment_count > 0,
        "unchanged LINE frame should draw without a geometry upload");

    series->style = plot::Display_style::AREA;
    source->set_samples({
        { 0LL,               10.0f},
        { 1LL * k_second_ns, 20.0f},
        { 2LL * k_second_ns, 30.0f},
        { 3LL * k_second_ns, 40.0f}
    });
    TEST_ASSERT(rhi_fixture.render_layer_frame(
        renderer, ctx, series_map, events, error_message), error_message);
    TEST_ASSERT(state->second.main_view.last_line_window_upload_count == 0,
        "AREA-only changed frame should not upload unused LINE geometry");

    series->style = plot::Display_style::LINE;
    TEST_ASSERT(rhi_fixture.render_layer_frame(
        renderer, ctx, series_map, events, error_message), error_message);
    TEST_ASSERT(state->second.main_view.last_sample_upload_count == 0 &&
        state->second.main_view.last_line_window_upload_count == 1 &&
        state->second.main_view.line_window_staging.front().y == 10.0f,
        "re-enabled LINE must rebuild from geometry changed during AREA-only rendering");

    return true;
}

} // namespace

int main()
{
    std::cout << "QRhi layer lifecycle tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_layer_only_zero_style_prepare_record_order);
    RUN_TEST(test_style_none_without_layers_does_not_upload_samples);
    RUN_TEST(test_custom_sample_buffer_not_reused_when_current_access_cannot_stage);
    RUN_TEST(test_builtin_upload_stages_visible_window_only);
    RUN_TEST(test_builtin_upload_reuses_vbo_capacity_headroom);
    RUN_TEST(test_combined_builtin_uploads_samples_once_per_view);
    RUN_TEST(test_direct_member_policy_uses_member_dispatch_in_renderer_staging);
    RUN_TEST(test_access_policy_change_reuploads_builtin_samples);
    RUN_TEST(test_builtin_staging_normalizes_finite_reversed_ranges);
    RUN_TEST(test_nonfinite_break_and_skip_split_drawable_spans);
    RUN_TEST(test_nonfinite_replace_with_zero_keeps_contiguous_span);
    RUN_TEST(test_nonfinite_reject_window_suppresses_drawable_upload);
    RUN_TEST(test_nonfinite_reject_window_invalidates_prior_upload_before_busy);
    RUN_TEST(test_custom_layer_zero_gpu_window_invalidates_prior_upload_before_busy);
    RUN_TEST(test_non_drawable_window_invalidates_prior_upload_before_fast_path);
    RUN_TEST(test_non_rhi_prepare_invalidates_prior_upload_before_fast_path);
    RUN_TEST(test_nonfinite_hold_forward_policy_controls_held_sample);
    RUN_TEST(test_nonfinite_skip_hold_forward_preserves_earlier_held_sample_with_visible_data);
    RUN_TEST(test_nonfinite_skip_hold_forward_ignores_future_padding_without_visible_data);
    RUN_TEST(test_global_draw_order_sorts_builtins_across_series_and_custom_layers);
    RUN_TEST(test_builtin_draw_commands_sort_relative_to_custom_layers);
    RUN_TEST(test_builtins_do_not_use_qrhi_layer_cache);
    RUN_TEST(test_builtin_upload_stages_visible_windows_for_dots_and_area);
    RUN_TEST(test_builtin_upload_stages_single_synthetic_hold_sample);
    RUN_TEST(test_builtin_upload_stages_hold_windows_for_dots_and_area);
    RUN_TEST(test_resources_changed_tracks_data_and_window_changes);
    RUN_TEST(test_resources_changed_tracks_hold_timestamp_changes);
    RUN_TEST(test_busy_stale_fallback_rejects_changed_request_shape);
    RUN_TEST(test_busy_hold_forward_does_not_prepare_stale_tmax);
    RUN_TEST(test_busy_hold_forward_does_not_reuse_non_hold_window);
    RUN_TEST(test_external_layer_gets_snapshot_on_builtin_cache_hit);
    RUN_TEST(test_external_layer_skips_busy_stale_fallback_and_recovers);
    RUN_TEST(test_external_layer_replans_when_snapshot_advances_after_sequence_probe);
    RUN_TEST(test_layer_state_recreated_for_program_identity_changes);
    RUN_TEST(test_range_only_access_skips_builtin_value_styles);
    RUN_TEST(test_stacked_sum_overlay_uses_top_geometry_and_theme);
    RUN_TEST(test_line_geometry_reuses_and_rebuilds_after_area_only_change);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
