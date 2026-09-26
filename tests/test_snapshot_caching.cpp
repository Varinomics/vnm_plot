// vnm_plot core snapshot cache tests

#include "test_macros.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/core/algo.h>
#include <vnm_plot/rhi/asset_loader.h>
#include "../src/core/series_window_planner.h"
#include "test_series_renderer.h"
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/time_units.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plot = vnm::plot;
using plot::Asset_loader;
using plot::Data_access_policy;
using plot::Data_source;
using plot::Display_style;
using plot::Empty_window_behavior;
using plot::Plot_config;
using Series_renderer = plot::Test_series_renderer;
using plot::data_snapshot_t;
using plot::frame_context_t;
using plot::frame_layout_result_t;
using plot::preview_config_t;
using plot::series_data_t;
using plot::snapshot_result_t;

namespace {

const plot::detail::series_window_planner_state_t& planner_state(
    const Series_renderer::vbo_view_state_t& view_state)
{
    assert(view_state.planner);
    return *view_state.planner;
}

struct Test_sample
{
    // Timestamps are int64 nanoseconds (API convention).
    std::int64_t   t;
    float          v;
};

class Single_level_source final : public Data_source
{
public:
    std::vector<Test_sample>   samples;
    int                        snapshot_calls   = 0;
    mutable int                lod_scales_calls = 0;
    mutable int                lod_scale_calls  = 0;
    uint64_t                   sequence         = 1;
    std::weak_ptr<void>        last_hold;
    std::vector<std::size_t>   scale_values     = {1};

    snapshot_result_t try_snapshot(size_t lod_level) override
    {
        if (lod_level != 0) {
            return {data_snapshot_t{}, snapshot_result_t::Snapshot_status::FAILED};
        }
        ++snapshot_calls;
        auto hold = std::make_shared<int>(7);
        last_hold = hold;
        data_snapshot_t snapshot{
            samples.data(),
            samples.size(),
            sizeof(Test_sample),
            sequence,
            nullptr,
            0,
            hold
        };
        if (samples.empty()) {
            return {data_snapshot_t{}, snapshot_result_t::Snapshot_status::EMPTY};
        }
        return {snapshot, snapshot_result_t::Snapshot_status::READY};
    }

    size_t lod_levels() const override { return scale_values.size(); }
    size_t lod_scale(size_t level) const override
    {
        ++lod_scale_calls;
        return level < scale_values.size() ? scale_values[level] : 1;
    }
    std::vector<std::size_t> lod_scales() const override
    {
        ++lod_scales_calls;
        return scale_values;
    }
    size_t sample_stride() const override { return sizeof(Test_sample); }
    uint64_t current_sequence(size_t /*lod_level*/) const override { return sequence; }
};

class Two_level_source final : public Data_source
{
public:
    std::vector<Test_sample>   lod0;
    std::vector<Test_sample>   lod1;
    std::array<int, 2>         snapshot_calls{{0, 0}};
    std::array<uint64_t, 2>    sequences{{1, 1}};

    snapshot_result_t try_snapshot(size_t lod_level) override
    {
        if (lod_level >= 2) {
            return {data_snapshot_t{}, snapshot_result_t::Snapshot_status::FAILED};
        }
        ++snapshot_calls[lod_level];
        const auto& data = (lod_level == 0) ? lod0 : lod1;
        auto hold = std::make_shared<int>(21);
        data_snapshot_t snapshot{
            data.data(),
            data.size(),
            sizeof(Test_sample),
            sequences[lod_level],
            nullptr,
            0,
            hold
        };
        if (data.empty()) {
            return {data_snapshot_t{}, snapshot_result_t::Snapshot_status::EMPTY};
        }
        return {snapshot, snapshot_result_t::Snapshot_status::READY};
    }

    size_t lod_levels() const override { return 2; }
    size_t lod_scale(size_t level) const override { return level == 0 ? 1 : 4; }
    size_t sample_stride() const override { return sizeof(Test_sample); }
    uint64_t current_sequence(size_t /*lod_level*/) const override { return 0; }
};

Data_access_policy make_policy()
{
    Data_access_policy policy;
    policy.get_timestamp = [](const void* sample) -> std::int64_t {
        return static_cast<const Test_sample*>(sample)->t;
    };
    policy.get_value = [](const void* sample) {
        return static_cast<const Test_sample*>(sample)->v;
    };
    policy.get_range = [](const void* sample) {
        const float value = static_cast<const Test_sample*>(sample)->v;
        return std::make_pair(value, value);
    };
    return policy;
}

struct Access_call_counts
{
    int timestamp = 0;
    int value     = 0;
};

class Observation_profiler final : public plot::Profiler
{
public:
    void begin_scope(const char*) override {}
    void end_scope() override {}
    void record_observation(const char* name, double value) override
    {
        observations[name ? name : ""] += value;
    }

    std::map<std::string, double> observations;
};

Data_access_policy make_direct_member_policy()
{
    auto typed = plot::make_access_policy<Test_sample>(
        &Test_sample::t,
        &Test_sample::v);
    return typed.erase();
}

Data_access_policy make_fallback_policy_with_counted_public_accessors(
    Access_call_counts& calls)
{
    Data_access_policy policy;
    policy.get_timestamp = [&calls](const void* sample) -> std::int64_t {
        ++calls.timestamp;
        return static_cast<const Test_sample*>(sample)->t;
    };
    policy.get_value = [&calls](const void* sample) {
        ++calls.value;
        return static_cast<const Test_sample*>(sample)->v;
    };
    return policy;
}

frame_context_t make_context(const frame_layout_result_t& layout, Plot_config& config)
{
    frame_context_t ctx{layout};
    // Timestamps are int64 ns. Tests use small ordinal indices for clarity.
    ctx.t0              = 0;
    ctx.t1              = 10;
    ctx.t_available_min = 0;
    ctx.t_available_max = 10;
    ctx.win_w           = 200;
    ctx.win_h           = 120;
    ctx.dark_mode       = config.dark_mode;
    ctx.config          = &config;
    return ctx;
}

void fill_lod_samples(Two_level_source& source)
{
    source.lod0.resize(100);
    for (size_t i = 0; i < source.lod0.size(); ++i) {
        source.lod0[i].t = static_cast<std::int64_t>(i);
        source.lod0[i].v = 1.0f + static_cast<float>(i);
    }
    source.lod1.resize(25);
    for (size_t i = 0; i < source.lod1.size(); ++i) {
        const size_t src = i * 4;
        source.lod1[i].t = static_cast<std::int64_t>(src);
        source.lod1[i].v = 1.0f + static_cast<float>(src);
    }
}

plot::Series_view_plan plan_two_level_lod_width(
    Two_level_source&                              source,
    const Data_access_policy&                      access,
    const std::vector<std::size_t>&                scales,
    plot::detail::series_window_planner_state_t&   state,
    plot::detail::Series_window_snapshot_cache&    cache,
    std::uint64_t                                  frame_id,
    double                                         width_px)
{
    plot::detail::series_window_plan_request_t request;
    request.planner_state  = &state;
    request.snapshot_cache = &cache;
    request.frame_id       = frame_id;
    request.data_source    = &source;
    request.access         = &access;
    request.scales         = &scales;
    request.t_min_ns       = 0;
    request.t_max_ns       = 99;
    request.t_origin_ns    = 0;
    request.width_px       = width_px;
    request.style          = Display_style::LINE;
    return plot::detail::plan_series_window(request);
}

std::shared_ptr<Single_level_source> make_single_level_source(
    std::vector<std::int64_t> timestamps)
{
    auto source   = std::make_shared<Single_level_source>();
    source->samples.resize(timestamps.size());
    for (std::size_t i = 0; i < timestamps.size(); ++i) {
        source->samples[i].t = timestamps[i];
        source->samples[i].v = 1.0f + static_cast<float>(i);
    }
    return source;
}

const plot::detail::series_window_planner_state_t* render_source_and_get_main_state(
    Series_renderer&               renderer,
    const frame_context_t&         ctx,
    std::shared_ptr<series_data_t> series,
    int                            series_id)
{
    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = std::move(series);
    renderer.render(ctx, series_map);

    const auto state_it = renderer.m_vbo_states.find(series_id);
    if (state_it == renderer.m_vbo_states.end()) {
        return nullptr;
    }
    return state_it->second.main_view.planner.get();
}

bool run_defensive_time_order_scan_case(
    std::vector<std::int64_t>              timestamps,
    plot::detail::Timestamp_window_search  expected_search,
    bool                                   expected_monotonic,
    const std::string&                     label)
{
    auto data_source = make_single_level_source(timestamps);

    auto series         = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    frame_context_t ctx = make_context(layout, config);

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    const auto* state = render_source_and_get_main_state(renderer, ctx, series, 65);

    TEST_ASSERT(state, label + " source should produce planner state");
    TEST_ASSERT(state->last_timestamp_order_scan_performed,
        label + " source should run the defensive monotonicity scan");
    TEST_ASSERT(state->last_timestamp_order_scan_samples > 0,
        label + " defensive scan should inspect timestamp samples");
    TEST_ASSERT(state->last_timestamps_monotonic == expected_monotonic,
        label + " defensive scan should record the observed timestamp order");
    TEST_ASSERT(state->last_timestamp_window_search == expected_search,
        label + " source should use the expected timestamp window search");

    return true;
}

bool test_timestamp_order_scan_selects_binary_or_linear_search()
{
    const bool ascending_ok = run_defensive_time_order_scan_case(
        { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
        plot::detail::Timestamp_window_search::BINARY,
        true,
        "ascending timestamps");
    if (!ascending_ok) {
        return false;
    }

    return run_defensive_time_order_scan_case(
        { 0, 5, 2, 7, 3, 8, 4, 9 },
        plot::detail::Timestamp_window_search::LINEAR,
        false,
        "unordered timestamps");
}

bool test_descending_timestamps_use_linear_window_search()
{
    auto data_source = make_single_level_source(
        { 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 });

    auto series         = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    frame_context_t ctx = make_context(layout, config);

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    const auto* state = render_source_and_get_main_state(renderer, ctx, series, 66);

    TEST_ASSERT(state, "expected planner state for DESCENDING time-order test");
    TEST_ASSERT(
        state->last_timestamp_window_search ==
            plot::detail::Timestamp_window_search::LINEAR,
        "DESCENDING source should use linear timestamp window search");
    TEST_ASSERT(state->last_count > 0,
        "DESCENDING linear timestamp window search should find visible samples");

    return true;
}

bool test_descending_timestamps_do_not_hold_oldest_sample()
{
    auto data_source = make_single_level_source(
        { 9, 7, 5, 3, 1 });

    auto series = std::make_shared<series_data_t>();
    series->style                 = Display_style::LINE;
    series->data_source           = data_source;
    series->access                = make_policy();
    series->empty_window_behavior = Empty_window_behavior::HOLD_LAST_FORWARD;

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    frame_context_t ctx = make_context(layout, config);
    ctx.t0              = 10;
    ctx.t1              = 12;
    ctx.t_available_min = ctx.t0;
    ctx.t_available_max = ctx.t1;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    const auto* state = render_source_and_get_main_state(renderer, ctx, series, 68);

    TEST_ASSERT(state, "expected planner state for DESCENDING hold-forward test");
    TEST_ASSERT(!state->last_hold_last_forward,
        "DESCENDING source should not synthesize hold-forward from the oldest physical sample");
    TEST_ASSERT(state->last_count == 0,
        "DESCENDING empty window should not draw the oldest physical sample as held data");

    return true;
}

bool test_renderer_uses_lod_scales_metadata()
{
    auto data_source = make_single_level_source(
        { 0, 1, 2, 3, 4, 5, 6, 7 });
    data_source->scale_values = {0};

    auto series         = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    frame_context_t ctx = make_context(layout, config);

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    const auto* state = render_source_and_get_main_state(renderer, ctx, series, 67);

    TEST_ASSERT(state, "expected planner state for LOD scale metadata test");
    TEST_ASSERT(data_source->lod_scales_calls > 0,
        "renderer should request source-provided LOD scale metadata");
    TEST_ASSERT(data_source->lod_scale_calls == 0,
        "renderer should not recompute per-level scales when metadata has the advertised LOD count");
    TEST_ASSERT(state->last_count > 0,
        "clamped source-provided LOD scale metadata should still produce a visible window");
    TEST_ASSERT(state->last_applied_pps > 0.0,
        "clamped source-provided LOD scale metadata should produce positive pixels-per-sample");

    return true;
}

bool test_direct_member_policy_uses_member_dispatch_in_planner()
{
    const auto make_source = []() {
        auto source = std::make_shared<Single_level_source>();
        source->samples.resize(16);
        for (size_t i = 0; i < source->samples.size(); ++i) {
            source->samples[i].t = static_cast<std::int64_t>(i);
            source->samples[i].v = 1.0f + static_cast<float>(i);
        }
        return source;
    };

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    frame_context_t ctx = make_context(layout, config);

    auto direct_series         = std::make_shared<series_data_t>();
    direct_series->style       = Display_style::LINE;
    direct_series->data_source = make_source();
    direct_series->access      = make_direct_member_policy();

    Series_renderer direct_renderer;
    Asset_loader direct_asset_loader;
    direct_renderer.initialize(direct_asset_loader);
    std::map<int, std::shared_ptr<const series_data_t>> direct_map;
    direct_map[61] = direct_series;
    direct_renderer.render(ctx, direct_map);

    const auto direct_state_it = direct_renderer.m_vbo_states.find(61);
    TEST_ASSERT(direct_state_it != direct_renderer.m_vbo_states.end(),
        "expected direct-policy planner state");
    TEST_ASSERT(
        planner_state(direct_state_it->second.main_view).last_access_dispatch_kind ==
            plot::detail::access_dispatch_kind_t::MEMBER_POINTER,
        "direct member-pointer planner path should use member-pointer dispatch");

    Access_call_counts fallback_calls;
    auto fallback_series         = std::make_shared<series_data_t>();
    fallback_series->style       = Display_style::LINE;
    fallback_series->data_source = make_source();
    fallback_series->access =
        make_fallback_policy_with_counted_public_accessors(fallback_calls);

    Series_renderer fallback_renderer;
    Asset_loader fallback_asset_loader;
    fallback_renderer.initialize(fallback_asset_loader);
    std::map<int, std::shared_ptr<const series_data_t>> fallback_map;
    fallback_map[62] = fallback_series;
    fallback_renderer.render(ctx, fallback_map);

    const auto fallback_state_it = fallback_renderer.m_vbo_states.find(62);
    TEST_ASSERT(fallback_state_it != fallback_renderer.m_vbo_states.end(),
        "expected fallback-policy planner state");
    TEST_ASSERT(
        planner_state(fallback_state_it->second.main_view).last_access_dispatch_kind ==
            plot::detail::access_dispatch_kind_t::STD_FUNCTION,
        "capturing planner fallback should use std::function dispatch");
    TEST_ASSERT(fallback_calls.timestamp > 0,
        "capturing planner fallback should call the public timestamp std::function");

    return true;
}

bool test_access_policy_change_invalidates_planner_fast_path_cache()
{
    auto data_source = std::make_shared<Single_level_source>();
    data_source->samples.resize(16);
    for (size_t i = 0; i < data_source->samples.size(); ++i) {
        data_source->samples[i].t = static_cast<std::int64_t>(i);
        data_source->samples[i].v = 1.0f + static_cast<float>(i);
    }

    const int series_id = 63;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_direct_member_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    frame_context_t ctx = make_context(layout, config);

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);
    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);
    TEST_ASSERT(data_source->snapshot_calls == 1,
        "expected initial planner render to take one snapshot");

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected planner state for access-policy cache test");
    state_it->second.main_view.has_uploaded_vbo = true;

    renderer.render(ctx, series_map);
    TEST_ASSERT(data_source->snapshot_calls == 1,
        "unchanged access policy should keep the planner fast path");

    Access_call_counts changed_calls;
    series->access =
        make_fallback_policy_with_counted_public_accessors(changed_calls);
    renderer.render(ctx, series_map);

    TEST_ASSERT(data_source->snapshot_calls == 2,
        "access policy change must invalidate the planner fast path");
    TEST_ASSERT(
        planner_state(state_it->second.main_view).last_access_dispatch_kind ==
            plot::detail::access_dispatch_kind_t::STD_FUNCTION,
        "changed policy should replan with std::function dispatch");
    TEST_ASSERT(changed_calls.timestamp > 0,
        "changed policy should invoke the replacement timestamp accessor");

    return true;
}

bool test_frame_scoped_cache_reuse()
{
    auto data_source = std::make_shared<Single_level_source>();
    data_source->samples.resize(16);
    for (size_t i = 0; i < data_source->samples.size(); ++i) {
        data_source->samples[i].t = static_cast<std::int64_t>(i);
        data_source->samples[i].v = 1.0f + static_cast<float>(i);
    }

    const int series_id = 7;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    config.preview_visibility = 1.0;

    frame_context_t ctx         = make_context(layout, config);
    ctx.adjusted_preview_height = 20.0;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);

    TEST_ASSERT(data_source->snapshot_calls == 1,
        std::string("expected shared snapshot between main and preview views, got ") +
            std::to_string(data_source->snapshot_calls));
    TEST_ASSERT(data_source->last_hold.expired(),
        "expected frame-scoped snapshot hold to release after render");

    return true;
}

bool test_preview_uses_distinct_source_snapshot()
{
    auto main_source    = std::make_shared<Single_level_source>();
    auto preview_source = std::make_shared<Single_level_source>();
    main_source->samples.resize(8, Test_sample{});
    preview_source->samples.resize(8, Test_sample{});
    for (size_t i = 0; i < main_source->samples.size(); ++i) {
        main_source->samples[i].t    = static_cast<std::int64_t>(i);
        main_source->samples[i].v    = 1.0f + static_cast<float>(i);
        preview_source->samples[i].t = static_cast<std::int64_t>(i);
        preview_source->samples[i].v = 2.0f + static_cast<float>(i);
    }

    const int series_id = 14;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = main_source;
    series->access      = make_policy();

    preview_config_t preview_cfg;
    preview_cfg.data_source = preview_source;
    preview_cfg.access      = make_policy();
    series->preview_config  = preview_cfg;

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    config.preview_visibility = 1.0;

    frame_context_t ctx         = make_context(layout, config);
    ctx.adjusted_preview_height = 20.0;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);

    TEST_ASSERT(main_source->snapshot_calls == 1,
        "expected main source snapshot call");
    TEST_ASSERT(preview_source->snapshot_calls == 1,
        "expected preview source snapshot call");

    return true;
}

bool test_preview_disabled_skips_preview_snapshot()
{
    auto main_source    = std::make_shared<Single_level_source>();
    auto preview_source = std::make_shared<Single_level_source>();
    main_source->samples.resize(8);
    preview_source->samples.resize(8);

    const int series_id = 15;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = main_source;
    series->access      = make_policy();

    preview_config_t preview_cfg;
    preview_cfg.data_source = preview_source;
    preview_cfg.access      = make_policy();
    series->preview_config  = preview_cfg;

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    config.preview_visibility = 1.0;

    frame_context_t ctx         = make_context(layout, config);
    ctx.adjusted_preview_height = 0.0;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);

    TEST_ASSERT(main_source->snapshot_calls == 1,
        "expected main source snapshot call");
    TEST_ASSERT(preview_source->snapshot_calls == 0,
        "expected preview source to be skipped when preview disabled");

    return true;
}

bool test_frame_change_invalidates_snapshot_cache()
{
    auto data_source = std::make_shared<Single_level_source>();
    data_source->samples.resize(12);
    for (size_t i = 0; i < data_source->samples.size(); ++i) {
        data_source->samples[i].t = static_cast<std::int64_t>(i);
        data_source->samples[i].v = 0.5f + static_cast<float>(i);
    }

    const int series_id = 8;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 140.0;
    layout.usable_height = 80.0;

    Plot_config config;

    frame_context_t ctx = make_context(layout, config);

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);
    renderer.render(ctx, series_map);

    TEST_ASSERT(data_source->snapshot_calls == 2,
        "expected snapshot refresh on frame change");

    return true;
}

bool test_empty_window_behavior_invalidates_fast_path_cache()
{
    auto data_source = std::make_shared<Single_level_source>();
    data_source->samples.resize(32);
    for (size_t i = 0; i < data_source->samples.size(); ++i) {
        data_source->samples[i].t = static_cast<std::int64_t>(i);
        data_source->samples[i].v = 10.0f + static_cast<float>(i);
    }

    const int series_id = 18;
    auto      series    = std::make_shared<series_data_t>();
    series->style                 = Display_style::LINE;
    series->data_source           = data_source;
    series->access                = make_policy();
    series->empty_window_behavior = Empty_window_behavior::DRAW_NOTHING;

    frame_layout_result_t layout;
    layout.usable_width  = 180.0;
    layout.usable_height = 80.0;

    Plot_config config;

    frame_context_t ctx = make_context(layout, config);

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);
    TEST_ASSERT(data_source->snapshot_calls == 1,
        "expected first render to take one snapshot");
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected vbo state for test series");
    // QRhi-less tests seed upload state to exercise the CPU fast-path conditions in plan_view().
    state_it->second.main_view.has_uploaded_vbo = true;

    renderer.render(ctx, series_map);
    TEST_ASSERT(data_source->snapshot_calls == 1,
        "expected fast-path cache hit to skip snapshot");

    series->empty_window_behavior = Empty_window_behavior::HOLD_LAST_FORWARD;
    renderer.render(ctx, series_map);
    TEST_ASSERT(data_source->snapshot_calls == 2,
        "expected empty_window_behavior change to invalidate fast-path cache");

    return true;
}

bool test_preview_honors_hold_last_forward()
{
    auto data_source = std::make_shared<Single_level_source>();
    data_source->samples.resize(16);
    for (size_t i = 0; i < data_source->samples.size(); ++i) {
        data_source->samples[i].t = static_cast<std::int64_t>(i);
        data_source->samples[i].v = 20.0f + static_cast<float>(i);
    }

    const int series_id = 19;
    auto      series    = std::make_shared<series_data_t>();
    series->style                 = Display_style::LINE;
    series->data_source           = data_source;
    series->access                = make_policy();
    series->empty_window_behavior = Empty_window_behavior::HOLD_LAST_FORWARD;

    frame_layout_result_t layout;
    layout.usable_width  = 160.0;
    layout.usable_height = 80.0;

    Plot_config config;
    config.preview_visibility = 1.0;

    frame_context_t ctx         = make_context(layout, config);
    ctx.adjusted_preview_height = 24.0;
    ctx.t_available_max         = 40.0;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected vbo state for preview hold-forward test");
    TEST_ASSERT(state_it->second.preview_view.planner,
        "expected preview planner state for preview hold-forward test");
    TEST_ASSERT(planner_state(state_it->second.preview_view).last_hold_last_forward,
        "preview should honor HOLD_LAST_FORWARD behavior");

    return true;
}

bool test_lod_level_separation()
{
    auto data_source = std::make_shared<Two_level_source>();
    fill_lod_samples(*data_source);

    const int series_id = 9;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    Plot_config config;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    frame_layout_result_t layout_wide;
    layout_wide.usable_width  = 100.0;
    layout_wide.usable_height = 80.0;
    frame_context_t ctx_wide = make_context(layout_wide, config);
    ctx_wide.t0    = 0.0;
    ctx_wide.t1    = 99.0;
    ctx_wide.win_w = 100;

    renderer.render(ctx_wide, series_map);

    TEST_ASSERT(data_source->snapshot_calls[0] >= 1,
        "expected LOD0 snapshot at wide width");
    TEST_ASSERT(data_source->snapshot_calls[1] == 0,
        "did not expect LOD1 snapshot at wide width");

    frame_layout_result_t layout_narrow;
    layout_narrow.usable_width  = 20.0;
    layout_narrow.usable_height = 80.0;
    frame_context_t ctx_narrow = make_context(layout_narrow, config);
    ctx_narrow.t0    = 0.0;
    ctx_narrow.t1    = 99.0;
    ctx_narrow.win_w = 20;

    renderer.render(ctx_narrow, series_map);

    TEST_ASSERT(data_source->snapshot_calls[1] >= 1,
        "expected LOD1 snapshot at narrow width");

    return true;
}

// Repo policy discourages hysteresis-based behavior. The planner must select the
// LOD level deterministically from pixels-per-sample (choose_lod_level), with no
// temporal stickiness toward the previously applied level. This guards against
// reintroducing a sticky switch band.
bool test_lod_selection_has_no_hysteresis()
{
    Two_level_source source;
    fill_lod_samples(source);

    const Data_access_policy       access = make_policy();
    const std::vector<std::size_t> scales = {1, 4};
    plot::detail::series_window_planner_state_t state;
    plot::detail::Series_window_snapshot_cache cache;
    std::uint64_t frame_id = 1;

    // Wide view: full-resolution level is closest to one pixel per sample.
    const auto wide_plan = plan_two_level_lod_width(
        source, access, scales, state, cache, frame_id++, 50.0);
    TEST_ASSERT(wide_plan.lod_level == 0,
        "wide LOD plan should choose the full-resolution level");

    // Narrowing past the raw midpoint must switch coarser immediately, with no
    // band that holds the previous (finer) level.
    TEST_ASSERT(plot::detail::choose_lod_level(scales, 0.39) == 1,
        "direct LOD chooser switches coarser at the raw midpoint");
    const auto narrow_plan = plan_two_level_lod_width(
        source, access, scales, state, cache, frame_id++, 39.0);
    TEST_ASSERT(narrow_plan.lod_level == 1,
        "planner must switch coarser at the raw midpoint (no hysteresis)");
    TEST_ASSERT(state.last_lod_level == 1,
        "state should record the switched level, not hold the previous one");

    // Widening back past the raw midpoint must switch finer immediately too.
    TEST_ASSERT(plot::detail::choose_lod_level(scales, 0.50) == 0,
        "direct LOD chooser switches finer at the raw midpoint");
    const auto rewide_plan = plan_two_level_lod_width(
        source, access, scales, state, cache, frame_id++, 50.0);
    TEST_ASSERT(rewide_plan.lod_level == 0,
        "planner must switch finer at the raw midpoint (no hysteresis)");

    return true;
}

bool test_stacking_composes_different_timestamps_from_independent_lods()
{
    Two_level_source lower_source;
    Two_level_source upper_source;
    fill_lod_samples(lower_source);
    fill_lod_samples(upper_source);
    for (auto& sample : upper_source.lod0) {
        sample.t += 2;
        sample.v  = 10.0f;
    }
    for (auto& sample : upper_source.lod1) {
        sample.t += 2;
        sample.v  = 10.0f;
    }

    const Data_access_policy       lower_access = make_policy();
    const Data_access_policy       upper_access = make_policy();
    const std::vector<std::size_t> scales       = {1, 4};
    plot::detail::series_window_planner_state_t lower_state;
    plot::detail::series_window_planner_state_t upper_state;
    plot::detail::Series_window_snapshot_cache lower_cache;
    plot::detail::Series_window_snapshot_cache upper_cache;

    auto lower_plan = plan_two_level_lod_width(
        lower_source, lower_access, scales, lower_state, lower_cache, 1, 50.0);
    auto upper_plan = plan_two_level_lod_width(
        upper_source, upper_access, scales, upper_state, upper_cache, 1, 39.0);
    TEST_ASSERT(lower_plan.lod_level == 0 && upper_plan.lod_level == 1,
        "stack inputs must retain independent LOD choices");

    plot::detail::stack_composition_stats_t exact_stats;
    std::vector<std::vector<plot::detail::stacked_sample_t>> layers;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&lower_plan, &upper_plan}, layers, 101u, &exact_stats) ==
            plot::Stack_rejection_reason::NONE,
        "different timestamp grids should compose");
    TEST_ASSERT(!exact_stats.resampled,
        "a timestamp union within the view budget should remain exact");
    TEST_ASSERT(layers.size() == 2 && layers[0].size() == layers[1].size(),
        "stack layers should share the timestamp union");
    TEST_ASSERT(layers[0].front().timestamp_ns == 2 &&
        layers[0].back().timestamp_ns == 98,
        "stack output must stay inside the common selected-sample domain");

    const auto sample = std::find_if(
        layers[1].begin(), layers[1].end(),
        [](const auto& value) { return value.timestamp_ns == 3; });
    TEST_ASSERT(sample != layers[1].end(),
        "full-resolution lower timestamps should remain in the union");
    TEST_ASSERT(std::abs(sample->base - 4.0f) < 1e-5f &&
        std::abs(sample->value - 14.0f) < 1e-5f,
        "upper layer should use the lower value and interpolate its own selected LOD");

    auto lower_series = std::make_shared<series_data_t>();
    lower_series->data_source.set_ref(lower_source);
    lower_series->access      = lower_access;
    lower_series->stack_group = 1;
    auto upper_series = std::make_shared<series_data_t>();
    upper_series->data_source.set_ref(upper_source);
    upper_series->access      = upper_access;
    upper_series->stack_group = 1;
    std::map<int, std::shared_ptr<const series_data_t>> series{
        {1, lower_series}, {2, upper_series}};
    frame_layout_result_t layout;
    layout.usable_width  = 50.0;
    layout.usable_height = 50.0;
    Plot_config config;
    auto profiler = std::make_shared<Observation_profiler>();
    config.profiler = profiler;
    auto ctx = make_context(layout, config);
    ctx.t1   = 99;
    Series_renderer renderer;
    Asset_loader assets;
    renderer.initialize(assets);
    renderer.render(ctx, series);
    TEST_ASSERT(profiler->observations["renderer.stacking.output_sample_count"] > 0.0,
        "stack composition should report its concrete output sample count");
    renderer.render(ctx, series);
    TEST_ASSERT(profiler->observations["renderer.stacking.cache_hit_count"] == 1.0,
        "unchanged stack inputs should reuse their composed timestamp union");

    series.erase(2);
    renderer.render(ctx, series);
    TEST_ASSERT(!renderer.m_vbo_states.at(1).main_view.stack_cache_snapshot,
        "dropping below two stack members must clear the surviving view cache");
    series[2] = upper_series;
    renderer.render(ctx, series);
    TEST_ASSERT(profiler->observations["renderer.stacking.cache_hit_count"] == 1.0,
        "re-enabling a stack must recompose instead of accepting a stale cache hit");

    upper_series->interpolation = plot::Series_interpolation::STEP_AFTER;
    renderer.render(ctx, series);
    TEST_ASSERT(
        renderer.m_vbo_states.at(1).main_view.last_stack_view_suppressed &&
        renderer.m_vbo_states.at(2).main_view.last_stack_view_suppressed,
        "a rejected stack must suppress its views instead of rendering ordinary series");
    return true;
}

bool test_stacking_interpolates_sub_256ns_epoch_intervals()
{
    constexpr std::int64_t k_epoch = 1'750'000'000'000'000'000LL;
    std::vector<Test_sample> lower{
        { k_epoch, 0.0f },
        { k_epoch + 254, 10.0f },
    };
    std::vector<Test_sample> upper{
        { k_epoch,       1.0f },
        { k_epoch + 127, 1.0f },
        { k_epoch + 254, 1.0f },
    };
    const Data_access_policy access = make_policy();
    const auto make_plan = [&access](std::vector<Test_sample>& samples) {
        plot::Series_view_plan plan;
        plan.access            = &access;
        plan.snapshot.snapshot = {
            samples.data(), samples.size(), sizeof(Test_sample), 1};
        plan.snapshot.sequence = 1;
        plan.source_count      = samples.size();
        plan.gpu_count         = samples.size();
        plan.drawable_spans    = {{0, samples.size(), 0, samples.size()}};
        plan.interpolation     = plot::Series_interpolation::LINEAR;
        return plan;
    };
    const auto lower_plan = make_plan(lower);
    const auto upper_plan = make_plan(upper);

    std::vector<std::vector<plot::detail::stacked_sample_t>> layers;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&lower_plan, &upper_plan}, layers, 16u) == plot::Stack_rejection_reason::NONE,
        "sub-256ns current-epoch grids should compose");
    TEST_ASSERT(layers.size() == 2 && layers[0].size() == 3,
        "epoch stack should retain the exact timestamp union");
    TEST_ASSERT(layers[0][1].timestamp_ns == k_epoch + 127 &&
        std::abs(layers[0][1].value - 5.0f) < 1e-6f &&
        std::abs(layers[1][1].value - 6.0f) < 1e-6f,
        "epoch stack should linearly interpolate before accumulating");

    lower.insert(lower.begin() + 1, {k_epoch + 127, 3.0f});
    lower.insert(lower.begin() + 2, {k_epoch + 127, 5.0f});
    const auto duplicate_plan = make_plan(lower);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&duplicate_plan, &upper_plan}, layers, 16u) == plot::Stack_rejection_reason::NONE,
        "duplicate epoch timestamps should compose");
    TEST_ASSERT(std::abs(layers[0][1].value - 5.0f) < 1e-6f,
        "stack composition should retain its last duplicate value policy");

    return true;
}

bool test_stacking_cursor_matches_equivalent_source_representations()
{
    const Data_access_policy callback_access = make_policy();
    const Data_access_policy member_access = plot::make_access_policy<Test_sample>(
        &Test_sample::t,
        &Test_sample::v
    ).erase();
    const auto make_plan = [](std::vector<Test_sample>& samples,
        const Data_access_policy& access, bool hold = false,
        std::int64_t hold_timestamp = 0) {
        plot::Series_view_plan plan;
        plan.access            = &access;
        plan.snapshot.snapshot = {
            samples.data(), samples.size(), sizeof(Test_sample), 1};
        plan.snapshot.sequence = 1;
        plan.source_count      = samples.size();
        plan.gpu_count         = samples.size() + (hold ? 1u : 0u);
        plan.drawable_spans    = {{0, samples.size(), 0, plan.gpu_count}};
        plan.interpolation     = plot::Series_interpolation::LINEAR;
        plan.hold_timestamp_ns = hold_timestamp;
        return plan;
    };
    const auto same_layers = [](
        const std::vector<std::vector<plot::detail::stacked_sample_t>>& lhs,
        const std::vector<std::vector<plot::detail::stacked_sample_t>>& rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t layer = 0; layer < lhs.size(); ++layer) {
            if (lhs[layer].size() != rhs[layer].size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs[layer].size(); ++i) {
                if (lhs[layer][i].timestamp_ns != rhs[layer][i].timestamp_ns ||
                    lhs[layer][i].value        != rhs[layer][i].value        ||
                    lhs[layer][i].base         != rhs[layer][i].base)
                {
                    return false;
                }
            }
        }
        return true;
    };

    std::vector<Test_sample> canonical{{-2, 0.0f}, {0, 0.0f}, {5, 5.0f}, {10, 11.0f}};
    std::vector<Test_sample> descending{{10, 10.0f}, {10, 11.0f}, {5, 5.0f}, {0, 0.0f}};
    std::vector<Test_sample> upper{{-2, 1.0f}, {4, 2.0f}, {10, 3.0f}};
    auto canonical_plan  = make_plan(canonical, callback_access);
    auto descending_plan = make_plan(descending, member_access, true, -2);
    auto upper_plan      = make_plan(upper, callback_access);
    std::vector<std::vector<plot::detail::stacked_sample_t>> expected;
    std::vector<std::vector<plot::detail::stacked_sample_t>> actual;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&canonical_plan, &upper_plan}, expected, 32u) ==
            plot::Stack_rejection_reason::NONE,
        "canonical ascending stack should compose");
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&descending_plan, &upper_plan}, actual, 32u) ==
            plot::Stack_rejection_reason::NONE && same_layers(actual, expected),
        "descending member access, last duplicate, and synthetic hold should match explicit ascending samples");

    std::vector<Test_sample> canonical_no_hold{{0, 0.0f}, {5, 5.0f}, {10, 11.0f}};
    canonical_plan  = make_plan(canonical_no_hold, callback_access);
    descending_plan = make_plan(descending, member_access, true, 0);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&canonical_plan, &upper_plan}, expected, 32u) ==
            plot::Stack_rejection_reason::NONE &&
        plot::detail::compose_stacked_series(
        {&descending_plan, &upper_plan}, actual, 32u) ==
            plot::Stack_rejection_reason::NONE && same_layers(actual, expected),
        "a hold at the last physical timestamp should remain elided after reversal");

    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();
    std::vector<Test_sample> full_ascending{{k_min, 0.0f}, {-1, 2.0f}, {k_max, 4.0f}};
    std::vector<Test_sample> full_descending(full_ascending.rbegin(), full_ascending.rend());
    std::vector<Test_sample> full_upper{{k_min, 1.0f}, {1, 3.0f}, {k_max, 5.0f}};
    canonical_plan  = make_plan(full_ascending,  callback_access);
    descending_plan = make_plan(full_descending, member_access);
    upper_plan      = make_plan(full_upper,      callback_access);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&canonical_plan, &upper_plan}, expected, 3u) ==
            plot::Stack_rejection_reason::NONE &&
        plot::detail::compose_stacked_series(
        {&descending_plan, &upper_plan}, actual, 3u) ==
            plot::Stack_rejection_reason::NONE && same_layers(actual, expected),
        "descending cursor evaluation should match ascending input across the full int64 range");

    std::vector<Test_sample> invalid_ascending{
        {0, std::numeric_limits<float>::quiet_NaN()}, {1, 1.0f}};
    std::vector<Test_sample> invalid_descending(
        invalid_ascending.rbegin(), invalid_ascending.rend());
    canonical_plan                   = make_plan(invalid_ascending,  callback_access);
    descending_plan                  = make_plan(invalid_descending, member_access);
    canonical_plan.nonfinite_policy  = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    descending_plan.nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&canonical_plan, &upper_plan}, expected, 16u) ==
            plot::Stack_rejection_reason::INCOMPATIBLE_DATA &&
        plot::detail::compose_stacked_series(
        {&descending_plan, &upper_plan}, actual, 16u) ==
            plot::Stack_rejection_reason::INCOMPATIBLE_DATA,
        "nonfinite rejection status should be independent of traversal and access dispatch");

    plot::Series_view_plan missing_plan;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&canonical_plan, &missing_plan}, actual, 16u) ==
            plot::Stack_rejection_reason::INCOMPATIBLE_DATA,
        "an earlier invalid source should retain precedence over a later missing source");
    std::vector<Test_sample> unordered{{0, 1.0f}, {2, 2.0f}, {1, 3.0f}};
    canonical_plan = make_plan(unordered, callback_access);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&canonical_plan, &missing_plan}, actual, 16u) ==
            plot::Stack_rejection_reason::NONMONOTONIC_TIMESTAMPS,
        "an earlier nonmonotonic source should retain precedence over a later missing source");

    std::vector<Test_sample> invalid_tail{
        { 0,  1.0f },
        { 10, 2.0f },
        { 15, 3.0f },
        { 20, 4.0f },
        {30, std::numeric_limits<float>::quiet_NaN()}};
    std::vector<Test_sample> shorter_domain{{0, 3.0f}, {10, 4.0f}};
    canonical_plan                  = make_plan(invalid_tail,   callback_access);
    upper_plan                      = make_plan(shorter_domain, callback_access);
    canonical_plan.nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&canonical_plan, &upper_plan}, actual, 16u) ==
            plot::Stack_rejection_reason::INCOMPATIBLE_DATA,
        "a nonfinite sample outside the common overlap should still reject composition");

    canonical_plan = make_plan(canonical, callback_access);
    canonical_plan.drawable_spans.front().source_count = canonical.size() + 1u;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&canonical_plan, &upper_plan}, actual, 16u) ==
            plot::Stack_rejection_reason::INCOMPATIBLE_DATA,
        "a drawable span outside its snapshot should be rejected without materialization");
    return true;
}

bool same_stacked_layers(
    const std::vector<std::vector<plot::detail::stacked_sample_t>>&    lhs,
    const std::vector<std::vector<plot::detail::stacked_sample_t>>&    rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t layer = 0; layer < lhs.size(); ++layer) {
        if (lhs[layer].size() != rhs[layer].size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs[layer].size(); ++i) {
            if (lhs[layer][i].timestamp_ns != rhs[layer][i].timestamp_ns ||
                lhs[layer][i].value        != rhs[layer][i].value        ||
                lhs[layer][i].base         != rhs[layer][i].base)
            {
                return false;
            }
        }
    }
    return true;
}

bool test_stacking_binary_lookup_bounds_source_reads()
{
    constexpr std::size_t    k_timestamp_budget = 129;
    constexpr std::int64_t   k_epoch            = 1'750'000'000'000'000'000LL;
    const Data_access_policy access             = make_policy();

    const auto ceil_log2 = [](std::size_t value) {
        std::size_t result = 0;
        if (value > 0) {
            --value;
        }
        while (value > 0) {
            ++result;
            value >>= 1u;
        }
        return result;
    };
    const auto compose = [&](std::size_t sample_count) -> std::size_t {
        std::array<std::vector<Test_sample>, 2> sources;
        for (std::size_t layer = 0; layer < sources.size(); ++layer) {
            auto& samples = sources[layer];
            samples.reserve(sample_count);
            for (std::size_t i = 0; i < sample_count; ++i) {
                samples.push_back({
                    k_epoch + static_cast<std::int64_t>(i * 4u + layer),
                    static_cast<float>((i % 31u) + layer)});
            }
        }
        std::array<plot::Series_view_plan, 2> plans;
        std::vector<const plot::Series_view_plan*> plan_ptrs;
        for (std::size_t layer = 0; layer < plans.size(); ++layer) {
            auto& plan = plans[layer];
            plan.access            = &access;
            plan.snapshot.snapshot = {
                sources[layer].data(), sources[layer].size(),
                sizeof(Test_sample), 1};
            plan.snapshot.sequence = 1;
            plan.source_count      = sources[layer].size();
            plan.gpu_count         = sources[layer].size();
            plan.drawable_spans    = {{
                0, sources[layer].size(), 0, sources[layer].size()}};
            plan.interpolation     = plot::Series_interpolation::LINEAR;
            plan_ptrs.push_back(&plan);
        }

        plot::detail::stack_composition_stats_t stats;
        std::vector<std::vector<plot::detail::stacked_sample_t>> layers;
        const auto reason = plot::detail::compose_stacked_series(
            plan_ptrs,
            std::vector<plot::detail::Time_order>(2, plot::detail::Time_order::ASCENDING),
            layers,
            k_timestamp_budget,
            &stats);
        const std::size_t logarithm = ceil_log2(sample_count);
        const std::size_t read_bound =
            2u * k_timestamp_budget * (2u * logarithm + 4u) +
            2u * k_timestamp_budget + 32u;
        TEST_ASSERT(reason == plot::Stack_rejection_reason::NONE &&
            stats.resampled &&
            stats.timestamp_count == k_timestamp_budget &&
            stats.source_read_count <= read_bound,
            "certified resampling should keep physical source reads within its grid-derived binary-search bound");
        return stats.source_read_count;
    };

    const std::size_t reads_1x   = compose(10'000);
    const std::size_t reads_10x  = compose(100'000);
    const std::size_t reads_100x = compose(1'000'000);
    TEST_ASSERT(reads_1x < reads_10x && reads_10x < reads_100x &&
        reads_100x < 20'000,
        "1x/10x/100x history should add only logarithmic binary-search reads");
    return true;
}

bool test_stacking_binary_lookup_matches_streaming_fallback()
{
    const Data_access_policy callback_access = make_policy();
    const Data_access_policy member_access = plot::make_access_policy<Test_sample>(
        &Test_sample::t,
        &Test_sample::v
    ).erase();
    const auto make_plan = [](std::vector<Test_sample>& samples,
        const Data_access_policy& access,
        plot::Series_interpolation interpolation,
        bool hold = false,
        std::int64_t hold_timestamp = 0) {
        plot::Series_view_plan plan;
        plan.access            = &access;
        plan.snapshot.snapshot = {
            samples.data(), samples.size(), sizeof(Test_sample), 1};
        plan.snapshot.sequence = 1;
        plan.source_count      = samples.size();
        plan.gpu_count         = samples.size() + (hold ? 1u : 0u);
        plan.drawable_spans    = {{0, samples.size(), 0, plan.gpu_count}};
        plan.interpolation     = interpolation;
        plan.hold_timestamp_ns = hold_timestamp;
        return plan;
    };
    const auto compare = [](
        const std::vector<const plot::Series_view_plan*>& plans,
        const std::vector<plot::detail::Time_order>& orders,
        std::size_t budget) {
        plot::detail::stack_composition_stats_t streaming_stats;
        plot::detail::stack_composition_stats_t binary_stats;
        std::vector<std::vector<plot::detail::stacked_sample_t>> streaming;
        std::vector<std::vector<plot::detail::stacked_sample_t>> binary;
        return
            plot::detail::compose_stacked_series(
                plans, streaming, budget, &streaming_stats) ==
                plot::Stack_rejection_reason::NONE &&
            plot::detail::compose_stacked_series(
                plans, orders, binary, budget, &binary_stats) ==
                plot::Stack_rejection_reason::NONE &&
            streaming_stats.resampled && binary_stats.resampled &&
            binary_stats.source_read_count < streaming_stats.source_read_count &&
            same_stacked_layers(binary, streaming);
    };

    constexpr std::size_t  k_count = 2'048;
    constexpr std::int64_t k_epoch = 1'750'000'000'000'000'000LL;
    std::vector<Test_sample> ascending_a;
    std::vector<Test_sample> ascending_b;
    ascending_a.reserve(k_count + 1u);
    ascending_b.reserve(k_count);
    for (std::size_t i = 0; i < k_count; ++i) {
        ascending_a.push_back({
            k_epoch + static_cast<std::int64_t>(i * 4u),
            static_cast<float>(i % 29u)});
        ascending_b.push_back({
            k_epoch + static_cast<std::int64_t>(i * 4u + 1u),
            static_cast<float>(10u + i % 17u)});
    }
    ascending_a.insert(ascending_a.begin() + 1'001, {
        ascending_a[1'000].t, 123.0f});
    auto ascending_plan = make_plan(
        ascending_a, callback_access, plot::Series_interpolation::LINEAR);
    auto upper_plan = make_plan(
        ascending_b, member_access, plot::Series_interpolation::LINEAR);
    TEST_ASSERT(compare(
        { &ascending_plan, &upper_plan },
        { plot::detail::Time_order::ASCENDING, plot::detail::Time_order::ASCENDING },
        31u),
        "binary LINEAR lookup should match streaming for epoch "
        "timestamps, callable/member access, and last physical duplicates");

    std::vector<Test_sample> descending(ascending_a.rbegin(), ascending_a.rend());
    const auto duplicate = std::find_if(
        descending.begin(), descending.end(),
        [](const auto& sample) { return sample.v == 123.0f; });
    TEST_ASSERT(duplicate != descending.end(),
        "expected descending duplicate fixture");
    descending.insert(duplicate + 1, {duplicate->t, 321.0f});
    auto descending_plan = make_plan(
        descending, member_access, plot::Series_interpolation::LINEAR,
        true, k_epoch - 4);
    ascending_b.insert(ascending_b.begin(), {k_epoch - 4, 9.0f});
    upper_plan = make_plan(
        ascending_b, callback_access, plot::Series_interpolation::LINEAR);
    TEST_ASSERT(compare(
        { &descending_plan, &upper_plan },
        { plot::detail::Time_order::DESCENDING, plot::detail::Time_order::ASCENDING },
        31u),
        "binary descending lookup should match streaming for physical-last duplicates and synthetic holds");

    ascending_plan.interpolation = plot::Series_interpolation::STEP_AFTER;
    upper_plan.interpolation     = plot::Series_interpolation::STEP_AFTER;
    TEST_ASSERT(compare(
        { &ascending_plan, &upper_plan },
        { plot::detail::Time_order::ASCENDING, plot::detail::Time_order::ASCENDING },
        31u),
        "binary STEP_AFTER lookup should match streaming values");

    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();
    std::vector<Test_sample> full_a{{k_min, 0.0f}};
    std::vector<Test_sample> full_b{{k_min, 1.0f}};
    for (std::int64_t timestamp = -128; timestamp <= 128; ++timestamp) {
        full_a.push_back({timestamp, static_cast<float>(timestamp)});
        full_b.push_back({timestamp, static_cast<float>(timestamp + 1)});
    }
    full_a.push_back({k_max, 2.0f});
    full_b.push_back({k_max, 3.0f});
    auto full_a_plan = make_plan(
        full_a, callback_access, plot::Series_interpolation::LINEAR);
    auto full_b_plan = make_plan(
        full_b, member_access, plot::Series_interpolation::LINEAR);
    TEST_ASSERT(compare(
        { &full_a_plan, &full_b_plan },
        { plot::detail::Time_order::ASCENDING, plot::detail::Time_order::ASCENDING },
        3u),
        "binary lookup should match streaming across the full int64 timestamp range");
    return true;
}

bool test_stacking_bounds_independent_timestamp_grids()
{
    constexpr std::size_t k_layer_count       = 32;
    constexpr std::size_t k_samples_per_layer = 10'000;
    constexpr std::size_t k_timestamp_budget  = 2'281;

    std::vector<std::vector<Test_sample>> sources(k_layer_count);
    std::vector<plot::Series_view_plan> plans(k_layer_count);
    std::vector<const plot::Series_view_plan*> plan_ptrs;
    plan_ptrs.reserve(k_layer_count);
    const Data_access_policy access = make_policy();
    for (std::size_t layer = 0; layer < k_layer_count; ++layer) {
        auto& samples = sources[layer];
        samples.reserve(k_samples_per_layer);
        for (std::size_t i = 0; i < k_samples_per_layer; ++i) {
            samples.push_back({
                static_cast<std::int64_t>(i * k_layer_count + layer),
                static_cast<float>(layer + 1u)});
        }
        auto& plan = plans[layer];
        plan.access            = &access;
        plan.snapshot.snapshot = {
            samples.data(), samples.size(), sizeof(Test_sample), 1};
        plan.snapshot.sequence = 1;
        plan.source_count      = samples.size();
        plan.gpu_count         = samples.size();
        plan.drawable_spans    = {{0, samples.size(), 0, samples.size()}};
        plan.interpolation     = plot::Series_interpolation::LINEAR;
        plan_ptrs.push_back(&plan);
    }

    plot::detail::stack_composition_stats_t stats;
    std::vector<std::vector<plot::detail::stacked_sample_t>> layers;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        plan_ptrs, layers, k_timestamp_budget, &stats) ==
            plot::Stack_rejection_reason::NONE,
        "32 independent timestamp grids should compose within the bounded grid");
    TEST_ASSERT(stats.resampled && stats.timestamp_count == k_timestamp_budget,
        "an oversized timestamp union should use the deterministic view grid");
    TEST_ASSERT(layers.size() == k_layer_count &&
        std::all_of(layers.begin(), layers.end(), [](const auto& layer) {
            return layer.size() == k_timestamp_budget;
        }),
        "every cumulative layer should share the bounded timestamp count");
    TEST_ASSERT(k_layer_count * k_timestamp_budget <=
        plot::detail::k_stack_max_output_samples_per_view,
        "bounded composition must stay below the per-view output ceiling");
    TEST_ASSERT(layers.front().front().timestamp_ns == 31 &&
        layers.front().back().timestamp_ns == 319'968,
        "the bounded grid should retain both common-domain endpoints");
    TEST_ASSERT(std::abs(layers.back().back().value - 528.0f) < 1e-5f,
        "bounded interpolation should preserve the cumulative value at emitted timestamps");

    return true;
}

bool test_stacking_bounded_grid_handles_interpolation_and_integer_extremes()
{
    const Data_access_policy access = make_policy();
    const auto make_plan = [&access](std::vector<Test_sample>& samples,
        plot::Series_interpolation interpolation) {
        plot::Series_view_plan plan;
        plan.access            = &access;
        plan.snapshot.snapshot = {
            samples.data(), samples.size(), sizeof(Test_sample), 1};
        plan.snapshot.sequence = 1;
        plan.source_count      = samples.size();
        plan.gpu_count         = samples.size();
        plan.drawable_spans    = {{0, samples.size(), 0, samples.size()}};
        plan.interpolation     = interpolation;
        return plan;
    };
    std::vector<std::vector<plot::detail::stacked_sample_t>> layers;
    plot::detail::stack_composition_stats_t stats;

    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();
    std::vector<Test_sample> full_a{{k_min, 0.0f}, {-1, 10.0f}, {k_max, 20.0f}};
    std::vector<Test_sample> full_b{{k_min, 1.0f}, {1, 3.0f}, {k_max, 5.0f}};
    auto a_plan = make_plan(full_a, plot::Series_interpolation::LINEAR);
    auto b_plan = make_plan(full_b, plot::Series_interpolation::LINEAR);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 3u, &stats) == plot::Stack_rejection_reason::NONE,
        "the bounded grid should span the full int64 timestamp range");
    TEST_ASSERT(stats.resampled && layers.front().size() == 3 &&
        layers.front()[0].timestamp_ns == k_min &&
        layers.front()[1].timestamp_ns == -1 &&
        layers.front()[2].timestamp_ns == k_max,
        "full-range grid timestamps should be distinct and retain both endpoints");

    std::vector<Test_sample> step_a{{0, 1.0f}, {2, 2.0f}, {4, 3.0f}};
    std::vector<Test_sample> step_b{{0, 10.0f}, {1, 20.0f}, {3, 30.0f}, {4, 40.0f}};
    a_plan = make_plan(step_a, plot::Series_interpolation::STEP_AFTER);
    b_plan = make_plan(step_b, plot::Series_interpolation::STEP_AFTER);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 3u, &stats) == plot::Stack_rejection_reason::NONE,
        "STEP_AFTER sources should compose on the bounded grid");
    TEST_ASSERT(stats.resampled && layers.back()[0].value == 11.0f &&
        layers.back()[1].value == 22.0f && layers.back()[2].value == 43.0f,
        "bounded STEP_AFTER values should use the last source value at each grid timestamp");

    std::vector<Test_sample> left{{0, 1.0f}, {10, 2.0f}};
    std::vector<Test_sample> right{{10, 3.0f}, {20, 4.0f}};
    a_plan = make_plan(left,  plot::Series_interpolation::LINEAR);
    b_plan = make_plan(right, plot::Series_interpolation::LINEAR);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 1u, &stats) == plot::Stack_rejection_reason::NONE &&
        !stats.resampled && stats.timestamp_count == 1 &&
        layers.back().front().timestamp_ns == 10,
        "a single-timestamp common domain should remain exact under a one-point budget");

    return true;
}

bool test_stacking_uses_separate_view_budgets_and_invalidates_resized_cache()
{
    auto lower_source = std::make_shared<Single_level_source>();
    auto upper_source = std::make_shared<Single_level_source>();
    for (std::int64_t timestamp = 0; timestamp <= 198; timestamp += 2) {
        lower_source->samples.push_back({timestamp, 1.0f});
    }
    upper_source->samples.push_back({0, 2.0f});
    for (std::int64_t timestamp = 1; timestamp < 198; timestamp += 2) {
        upper_source->samples.push_back({timestamp, 2.0f});
    }
    upper_source->samples.push_back({198, 2.0f});

    const Data_access_policy access = make_policy();
    auto lower = std::make_shared<series_data_t>();
    lower->data_source = lower_source;
    lower->access      = access;
    lower->stack_group = 1;
    auto upper = std::make_shared<series_data_t>();
    upper->data_source = upper_source;
    upper->access      = access;
    upper->stack_group = 1;
    std::map<int, std::shared_ptr<const series_data_t>> series{{1, lower}, {2, upper}};

    frame_layout_result_t layout;
    layout.usable_width  = 10.0;
    layout.usable_height = 50.0;
    Plot_config config;
    auto profiler    = std::make_shared<Observation_profiler>();
    config.profiler  = profiler;
    auto ctx = make_context(layout, config);
    ctx.t1                      = 198;
    ctx.t_available_max         = 198;
    ctx.adjusted_preview_height = 20.0;
    Series_renderer renderer;
    Asset_loader assets;
    renderer.initialize(assets);
    renderer.render(ctx, series);

    const auto& initial_main    = renderer.m_vbo_states.at(1).main_view.stack_cache_snapshot;
    const auto& initial_preview = renderer.m_vbo_states.at(1).preview_view.stack_cache_snapshot;
    TEST_ASSERT(initial_main.count == 21 && initial_preview.count == 199,
        "main and preview should use their own pixel-derived timestamp budgets");
    TEST_ASSERT(profiler->observations["renderer.stacking.resampled_group_count"] == 1.0 &&
        profiler->observations["renderer.stacking.exact_group_count"] == 1.0,
        "the renderer should report one bounded main grid and one exact preview union");
    TEST_ASSERT(
        planner_state(renderer.m_vbo_states.at(1).main_view)
                .last_selected_time_order == plot::detail::Time_order::ASCENDING &&
        planner_state(renderer.m_vbo_states.at(1).preview_view)
                .last_selected_time_order == plot::detail::Time_order::ASCENDING,
        "main and preview plans should retain the planner-established selected order");
    const void* initial_main_hold    = initial_main.hold.get();
    const void* initial_preview_hold = initial_preview.hold.get();

    layout.usable_width = 20.0;
    renderer.render(ctx, series);
    const auto& resized_main    = renderer.m_vbo_states.at(1).main_view.stack_cache_snapshot;
    const auto& resized_preview = renderer.m_vbo_states.at(1).preview_view.stack_cache_snapshot;
    TEST_ASSERT(resized_main.count == 41 && resized_main.hold.get() != initial_main_hold,
        "resizing should invalidate and rebuild a resampled main grid");
    TEST_ASSERT(resized_preview.count == 199 &&
        resized_preview.hold.get() == initial_preview_hold,
        "an unchanged preview budget should retain its exact cached union");
    TEST_ASSERT(profiler->observations["renderer.stacking.cache_hit_count"] == 1.0,
        "only the unchanged preview composition should hit its cache after resizing");
    TEST_ASSERT(renderer.stack_view_statuses().at({1, plot::Series_view_kind::MAIN})
        .status.state == plot::Stack_view_state::ACTIVE,
        "bounded resampling should remain an active stack result");

    return true;
}

bool test_stacking_reports_specific_rejection_reasons()
{
    const Data_access_policy access = make_policy();
    const auto make_plan = [&access](std::vector<Test_sample>& samples) {
        plot::Series_view_plan plan;
        plan.access            = &access;
        plan.snapshot.snapshot = {
            samples.data(), samples.size(), sizeof(Test_sample), 1};
        plan.snapshot.sequence = 1;
        plan.source_count      = samples.size();
        plan.gpu_count         = samples.size();
        plan.drawable_spans    = {{0, samples.size(), 0, samples.size()}};
        return plan;
    };
    std::vector<std::vector<plot::detail::stacked_sample_t>> layers;

    std::vector<Test_sample> a{{0, 1.0f}, {1, 2.0f}};
    std::vector<Test_sample> b{{0, 3.0f}, {1, 4.0f}};
    auto a_plan = make_plan(a);
    auto b_plan = make_plan(b);

    TEST_ASSERT(plot::detail::compose_stacked_series(
        {nullptr, &b_plan}, layers, 16u) == plot::Stack_rejection_reason::NO_DRAWABLE_DATA,
        "missing plans should have a queryable rejection reason");

    b_plan.interpolation = plot::Series_interpolation::STEP_AFTER;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 16u) == plot::Stack_rejection_reason::MIXED_INTERPOLATION,
        "mixed interpolation should have a queryable rejection reason");
    b_plan.interpolation = plot::Series_interpolation::LINEAR;

    a_plan.drawable_spans.push_back({1, 1, 1, 1});
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 16u) == plot::Stack_rejection_reason::INCOMPATIBLE_DATA,
        "multiple drawable spans should report discontinuous data");
    a_plan.drawable_spans.resize(1);

    a[0].v = std::numeric_limits<float>::quiet_NaN();
    a_plan.nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 16u) == plot::Stack_rejection_reason::INCOMPATIBLE_DATA,
        "nonfinite input should have a queryable rejection reason");
    a = {{0, 1.0f}, {1, 2.0f}};
    a_plan = make_plan(a);

    a = {{0, 1.0f}, {2, 2.0f}, {1, 3.0f}};
    a_plan = make_plan(a);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 16u) ==
            plot::Stack_rejection_reason::NONMONOTONIC_TIMESTAMPS,
        "unordered timestamps should have a queryable rejection reason");

    a      = {{0, 1.0f}, {1, 2.0f}};
    b      = {{2, 3.0f}, {3, 4.0f}};
    a_plan = make_plan(a);
    b_plan = make_plan(b);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 16u) == plot::Stack_rejection_reason::NO_COMMON_DOMAIN,
        "disjoint inputs should report no common domain");

    b = {{0, 3.0f}, {1, 4.0f}};
    b_plan = make_plan(b);
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 1u) == plot::Stack_rejection_reason::OUTPUT_LIMIT,
        "a grid budget that cannot retain both endpoints should report its output limit");

    const float maximum = std::numeric_limits<float>::max();
    a      = {{0, maximum}, {1, maximum}};
    b      = {{0, maximum}, {1, maximum}};
    a_plan = make_plan(a);
    b_plan = make_plan(b);
    plot::detail::stack_composition_stats_t overflow_stats;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&a_plan, &b_plan}, layers, 16u, &overflow_stats) ==
            plot::Stack_rejection_reason::CUMULATIVE_OVERFLOW &&
        layers.empty() && overflow_stats.timestamp_count == 0 && !overflow_stats.resampled,
        "nonfinite cumulative output should report overflow and clear partial results");

    return true;
}

bool test_stacked_area_preserves_signed_component_bands()
{
    const Data_access_policy access = make_policy();
    const auto make_plan = [&access](std::vector<Test_sample>& samples) {
        plot::Series_view_plan plan;
        plan.access            = &access;
        plan.snapshot.snapshot = {
            samples.data(), samples.size(), sizeof(Test_sample), 1};
        plan.snapshot.sequence = 1;
        plan.source_count      = samples.size();
        plan.gpu_count         = samples.size();
        plan.drawable_spans    = {{0, samples.size(), 0, samples.size()}};
        return plan;
    };

    std::vector<Test_sample> lower{{0, -100.0f}, {1, 0.0f}, {2, 100.0f}};
    std::vector<Test_sample> upper{{0,   25.0f}, {1, 0.0f}, {2, -150.0f}};
    auto lower_plan = make_plan(lower);
    auto upper_plan = make_plan(upper);

    std::vector<std::vector<plot::detail::stacked_sample_t>> layers;
    TEST_ASSERT(plot::detail::compose_stacked_series(
        {&lower_plan, &upper_plan}, layers, 16u) == plot::Stack_rejection_reason::NONE,
        "signed and zero account-equity components should compose");
    TEST_ASSERT(layers.size() == 2 && layers[0].size() == 3 && layers[1].size() == 3,
        "signed stack should preserve every input timestamp");
    TEST_ASSERT(
        layers[0][0].base == 0.0f  && layers[0][0].value == -100.0f &&
        layers[0][1].base == 0.0f  && layers[0][1].value == 0.0f    &&
        layers[0][2].base == 0.0f  && layers[0][2].value == 100.0f,
        "bottom area layer should span zero to its signed component value");
    TEST_ASSERT(
        layers[1][0].base == -100.0f  && layers[1][0].value == -75.0f &&
        layers[1][1].base == 0.0f     && layers[1][1].value == 0.0f   &&
        layers[1][2].base == 100.0f   && layers[1][2].value == -50.0f,
        "upper area layer should span the previous cumulative value to the signed total");

    const Data_access_policy& stacked_access = plot::detail::stacked_sample_access();
    const auto descending_band = stacked_access.get_range(&layers[1][2]);
    TEST_ASSERT(descending_band.first == 100.0f && descending_band.second == -50.0f,
        "area access should retain a descending signed band without clamping or reordering");
    return true;
}

bool test_upload_origin_records_per_view_origin()
{
    // The renderer's per-view upload-invalidation key must include the view
    // origin. After a render, view_state.uploaded_t_origin_ns must equal
    // the renderer's signed-span origin for that view, otherwise the next
    // frame's origin-change branch in plan_view will not fire when it should.
    // This is the visible state-trace of the upload-invalidation contract; the
    // inline predicate inside plan_view is hard to test directly without
    // refactoring the renderer.

    auto data_source = std::make_shared<Single_level_source>();
    data_source->samples.resize(8);
    for (size_t i = 0; i < data_source->samples.size(); ++i) {
        data_source->samples[i].t = static_cast<std::int64_t>(i) * 1'000'000LL;
        data_source->samples[i].v = 1.0f + static_cast<float>(i);
    }

    const int series_id = 41;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;

    frame_context_t ctx = make_context(layout, config);
    // Span = 1 ms + 1 ns is strictly greater than k_ns_per_ms, so
    // choose_snap_ns falls into the next bucket and returns 1 us. t0 is
    // an exact multiple of 1 us, so the snap-aligned origin equals t0.
    ctx.t0              = 5'000'000LL;
    ctx.t1              = 5'000'001LL + 1'000'000LL; // 1 ms + 1 ns
    ctx.t_available_min = ctx.t0;
    ctx.t_available_max = ctx.t1;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected vbo state for upload-origin test");

    const std::int64_t expected_span_ns =
        plot::detail::positive_span_ns_for_signed_api(ctx.t0, ctx.t1);
    const std::int64_t expected_origin_ns =
        plot::detail::choose_origin_ns(ctx.t0, expected_span_ns);
    TEST_ASSERT(state_it->second.main_view.planner,
        "expected main planner state for upload-origin test");
    TEST_ASSERT(
        planner_state(state_it->second.main_view).uploaded_t_origin_ns == expected_origin_ns,
        std::string("uploaded_t_origin_ns must match choose_origin_ns; got ") +
        std::to_string(planner_state(state_it->second.main_view).uploaded_t_origin_ns) +
        ", expected " + std::to_string(expected_origin_ns));

    return true;
}

bool test_upload_invalidates_when_origin_changes_across_snap_bucket()
{
    // The cache-fast-path predicate inside plan_view requires
    // view_state.uploaded_t_origin_ns == t_origin_ns. With sequence,
    // identity, and width all unchanged, an origin change (achieved by
    // moving t_min/t_max across a snap-step boundary) must still force the
    // fast-path miss and a fresh snapshot. Two different view ranges in
    // the same snap bucket would skip the snapshot; two ranges in
    // different buckets must not.

    auto data_source = std::make_shared<Single_level_source>();
    data_source->samples.resize(64);
    for (size_t i = 0; i < data_source->samples.size(); ++i) {
        // 100-second timeline at 1.5-second cadence.
        data_source->samples[i].t = static_cast<std::int64_t>(i) * 1'500'000'000LL;
        data_source->samples[i].v = 1.0f + static_cast<float>(i);
    }

    const int series_id = 42;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    // Span = 1 hour -> snap = 1 second (per choose_snap_ns).
    // Render frames whose t_min spans different 1-second buckets. Sequence,
    // identity, width all stay the same; only origin (and t_min/t_max)
    // moves. The renderer should call try_snapshot on every frame because
    // origin change is part of the upload-invalidation key.
    auto run_frame = [&](std::int64_t t_min_ns, std::int64_t span_ns) {
        frame_context_t ctx = make_context(layout, config);
        ctx.t0              = t_min_ns;
        ctx.t1              = t_min_ns + span_ns;
        ctx.t_available_min = ctx.t0;
        ctx.t_available_max = ctx.t1;
        renderer.render(ctx, series_map);
        // QRhi-less tests seed upload state to keep the cache-hit predicate's other terms truthy.
        auto it = renderer.m_vbo_states.find(series_id);
        if (it != renderer.m_vbo_states.end()) {
            it->second.main_view.has_uploaded_vbo = true;
        }
    };

    constexpr std::int64_t k_one_hour_ns   = 3'600LL * 1'000'000'000LL;
    constexpr std::int64_t k_one_second_ns = 1'000'000'000LL;

    // Frame 1: bucket-aligned origin = 0.
    run_frame(0LL, k_one_hour_ns);
    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected vbo state to exist after first render");
    TEST_ASSERT(state_it->second.main_view.planner,
        "expected main planner state for origin invalidation test");
    const std::int64_t origin_after_frame1 =
        planner_state(state_it->second.main_view).uploaded_t_origin_ns;
    TEST_ASSERT(origin_after_frame1 == 0LL,
        std::string("expected origin 0 after first render, got ") +
        std::to_string(origin_after_frame1));
    const int snapshots_after_frame1 = data_source->snapshot_calls;
    TEST_ASSERT(snapshots_after_frame1 >= 1,
        "expected at least one snapshot on first render");

    // Frame 2: same span, t_min still in same 1 s bucket -> origin
    // unchanged. The cache fast path should fire (no extra snapshot).
    run_frame(k_one_second_ns / 2, k_one_hour_ns);
    const std::int64_t origin_after_frame2 =
        planner_state(state_it->second.main_view).uploaded_t_origin_ns;
    TEST_ASSERT(origin_after_frame2 == 0LL,
        "expected origin to remain 0 when t_min moves within the same snap bucket");

    // Frame 3: t_min moves into the next 1 s bucket -> origin must change.
    run_frame(k_one_second_ns + k_one_second_ns / 4, k_one_hour_ns);
    const std::int64_t origin_after_frame3 =
        planner_state(state_it->second.main_view).uploaded_t_origin_ns;
    TEST_ASSERT(origin_after_frame3 == k_one_second_ns,
        std::string("expected origin to advance to 1 s bucket after t_min "
            "crosses snap boundary, got ") +
        std::to_string(origin_after_frame3));
    TEST_ASSERT(origin_after_frame3 != origin_after_frame1,
        "origin must change when t_min crosses a snap-step boundary");

    return true;
}

bool test_renderer_assigns_distinct_origins_to_main_and_preview()
{
    // The renderer chooses per-view origins from each view's own visible
    // window through the same signed-span adapter used by production. When the
    // main span lands in a finer snap bucket (e.g. a 1-hour span -> 1 s snap)
    // and the preview span in a coarser one (e.g. a 10-year span -> 1 day
    // snap), the two origins must end up at different floored boundaries. A
    // regression that fed main_origin_ns into the preview's plan_view call
    // would leave both views with the same uploaded_t_origin_ns and break fp32
    // precision in preview.
    auto data_source = std::make_shared<Single_level_source>();
    // Sparse 10-year coverage: one sample per day is enough for the
    // renderer to find data within both windows without ballooning memory.
    constexpr std::int64_t k_ns_per_second = 1'000'000'000LL;
    constexpr std::int64_t k_ns_per_day    = k_ns_per_second * 86'400LL;
    constexpr std::int64_t k_ns_per_hour   = k_ns_per_second * 3'600LL;
    constexpr int          k_num_samples   = 365 * 10;
    data_source->samples.resize(k_num_samples);
    for (int i = 0; i < k_num_samples; ++i) {
        data_source->samples[i].t = static_cast<std::int64_t>(i) * k_ns_per_day;
        data_source->samples[i].v = 1.0f + static_cast<float>(i);
    }

    const int series_id = 43;
    auto      series    = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = data_source;
    series->access      = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 200.0;
    layout.usable_height = 80.0;

    Plot_config config;
    config.preview_visibility = 1.0;

    frame_context_t ctx = make_context(layout, config);
    // Main view: 1-hour window at an offset that is NOT day-aligned, so
    // the per-view floored origin differs depending on the snap step.
    // 100 days + 30 minutes pushes the main range mid-day.
    const std::int64_t main_t0 = 100LL * k_ns_per_day + 30LL * 60LL * k_ns_per_second;
    ctx.t0 = main_t0;
    ctx.t1 = main_t0 + k_ns_per_hour;
    // Preview view: full 10-year range starting at 0.
    ctx.t_available_min         = 0LL;
    ctx.t_available_max         = 10LL * 365LL * k_ns_per_day;
    ctx.adjusted_preview_height = 20.0;

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[series_id] = series;

    renderer.render(ctx, series_map);

    auto state_it = renderer.m_vbo_states.find(series_id);
    TEST_ASSERT(state_it != renderer.m_vbo_states.end(),
        "expected vbo state for distinct-origin renderer test");
    TEST_ASSERT(state_it->second.main_view.planner,
        "expected main planner state for distinct-origin renderer test");
    TEST_ASSERT(state_it->second.preview_view.planner,
        "expected preview planner state for distinct-origin renderer test");

    const std::int64_t main_origin =
        planner_state(state_it->second.main_view).uploaded_t_origin_ns;
    const std::int64_t preview_origin =
        planner_state(state_it->second.preview_view).uploaded_t_origin_ns;

    const std::int64_t expected_main_span =
        plot::detail::positive_span_ns_for_signed_api(ctx.t0, ctx.t1);
    const std::int64_t expected_preview_span = plot::detail::positive_span_ns_for_signed_api(
        ctx.t_available_min,
        ctx.t_available_max);
    const std::int64_t expected_main_origin =
        plot::detail::choose_origin_ns(ctx.t0, expected_main_span);
    const std::int64_t expected_preview_origin = plot::detail::choose_origin_ns(
        ctx.t_available_min,
        expected_preview_span);

    TEST_ASSERT(main_origin == expected_main_origin,
        std::string("main view must record its own per-view origin; got ") +
        std::to_string(main_origin) + ", expected " +
        std::to_string(expected_main_origin));
    TEST_ASSERT(preview_origin == expected_preview_origin,
        std::string("preview view must record its own per-view origin; got ") +
        std::to_string(preview_origin) + ", expected " +
        std::to_string(expected_preview_origin));
    TEST_ASSERT(main_origin != preview_origin,
        std::string("main and preview must record different origins when "
            "their spans land in different snap buckets; got main=") +
        std::to_string(main_origin) + ", preview=" +
        std::to_string(preview_origin));

    return true;
}

bool test_render_skips_invalid_series()
{
    auto data_source = std::make_shared<Single_level_source>();
    data_source->samples.resize(4, Test_sample{});

    const int disabled_id     = 12;
    auto      disabled_series = std::make_shared<series_data_t>();
    disabled_series->enabled     = false;
    disabled_series->style       = Display_style::LINE;
    disabled_series->data_source = data_source;
    disabled_series->access      = make_policy();

    const int null_source_id     = 13;
    auto      null_source_series = std::make_shared<series_data_t>();
    null_source_series->enabled  = true;
    null_source_series->style    = Display_style::LINE;
    null_source_series->data_source.reset();
    null_source_series->access = make_policy();

    frame_layout_result_t layout;
    layout.usable_width  = 140.0;
    layout.usable_height = 80.0;

    Plot_config config;

    frame_context_t ctx = make_context(layout, config);

    Series_renderer renderer;
    Asset_loader asset_loader;
    renderer.initialize(asset_loader);

    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[disabled_id]    = disabled_series;
    series_map[null_source_id] = null_source_series;
    series_map[99]             = nullptr;

    renderer.render(ctx, series_map);

    TEST_ASSERT(data_source->snapshot_calls == 0,
        "expected disabled series to skip snapshots");

    return true;
}

}  // namespace

int main()
{
    std::cout << "Core snapshot cache tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_timestamp_order_scan_selects_binary_or_linear_search);
    RUN_TEST(test_descending_timestamps_use_linear_window_search);
    RUN_TEST(test_descending_timestamps_do_not_hold_oldest_sample);
    RUN_TEST(test_renderer_uses_lod_scales_metadata);
    RUN_TEST(test_direct_member_policy_uses_member_dispatch_in_planner);
    RUN_TEST(test_access_policy_change_invalidates_planner_fast_path_cache);
    RUN_TEST(test_frame_scoped_cache_reuse);
    RUN_TEST(test_preview_uses_distinct_source_snapshot);
    RUN_TEST(test_preview_disabled_skips_preview_snapshot);
    RUN_TEST(test_frame_change_invalidates_snapshot_cache);
    RUN_TEST(test_empty_window_behavior_invalidates_fast_path_cache);
    RUN_TEST(test_preview_honors_hold_last_forward);
    RUN_TEST(test_lod_level_separation);
    RUN_TEST(test_lod_selection_has_no_hysteresis);
    RUN_TEST(test_stacking_composes_different_timestamps_from_independent_lods);
    RUN_TEST(test_stacking_interpolates_sub_256ns_epoch_intervals);
    RUN_TEST(test_stacking_cursor_matches_equivalent_source_representations);
    RUN_TEST(test_stacking_binary_lookup_bounds_source_reads);
    RUN_TEST(test_stacking_binary_lookup_matches_streaming_fallback);
    RUN_TEST(test_stacking_bounds_independent_timestamp_grids);
    RUN_TEST(test_stacking_bounded_grid_handles_interpolation_and_integer_extremes);
    RUN_TEST(test_stacking_uses_separate_view_budgets_and_invalidates_resized_cache);
    RUN_TEST(test_stacking_reports_specific_rejection_reasons);
    RUN_TEST(test_stacked_area_preserves_signed_component_bands);
    RUN_TEST(test_upload_origin_records_per_view_origin);
    RUN_TEST(test_upload_invalidates_when_origin_changes_across_snap_bucket);
    RUN_TEST(test_renderer_assigns_distinct_origins_to_main_and_preview);
    RUN_TEST(test_render_skips_invalid_series);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;

    return failed > 0 ? 1 : 0;
}
