// vnm_plot production auto-range query tests

#include "test_macros.h"

#include "../src/core/auto_range_resolver.h"
#include "../src/core/frame_range_planner.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/types.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plot = vnm::plot;
using plot::Auto_v_range_mode;
using plot::Data_access_policy;
using plot::Data_query_status;
using plot::Data_source;
using plot::Display_style;
using plot::Empty_window_behavior;
using plot::Plot_config;
using plot::Series_interpolation;
using plot::data_config_t;
using plot::data_query_context_t;
using plot::data_query_result_t;
using plot::data_snapshot_t;
using plot::series_data_t;
using plot::snapshot_result_t;
using plot::value_range_t;

namespace {

struct Test_sample
{
    std::int64_t   t;
    float          v;
};

constexpr std::uint64_t k_stable_policy_semantics = 0x535441424C45;

class Query_range_source final : public Data_source
{
public:
    std::vector<Test_sample>   samples;
    Data_query_status          query_status           = Data_query_status::UNSUPPORTED;
    value_range_t              query_range{0.0f, 0.0f};
    std::uint64_t              query_sequence         = 1;
    std::uint64_t              current_sequence_value = 1;
    std::uint64_t              snapshot_sequence      = 1;
    std::size_t                levels                 = 1;
    int                        query_calls            = 0;
    int                        snapshot_calls         = 0;
    std::size_t                last_query_lod         = 0;
    data_query_context_t       last_query;

    snapshot_result_t try_snapshot(std::size_t lod_level) override
    {
        ++snapshot_calls;
        if (lod_level >= levels) {
            return {data_snapshot_t{}, snapshot_result_t::Snapshot_status::FAILED};
        }
        data_snapshot_t snapshot{
            samples.data(),
            samples.size(),
            sizeof(Test_sample),
            snapshot_sequence,
            nullptr,
            0,
            std::make_shared<int>(7)
        };
        if (samples.empty()) {
            return {data_snapshot_t{}, snapshot_result_t::Snapshot_status::EMPTY};
        }
        return {snapshot, snapshot_result_t::Snapshot_status::READY};
    }

    data_query_result_t<value_range_t> query_v_range(
        std::size_t                    lod,
        const data_query_context_t&    query) override
    {
        ++query_calls;
        last_query_lod = lod;
        last_query = query;
        data_query_result_t<value_range_t> result;
        result.status   = query_status;
        result.sequence = query_sequence;
        result.value    = query_range;
        return result;
    }

    std::size_t lod_levels() const override { return levels; }
    std::size_t lod_scale(std::size_t level) const override { return level == 0 ? 1 : 4; }
    std::size_t sample_stride() const override { return sizeof(Test_sample); }
    std::uint64_t current_sequence(std::size_t /*lod_level*/) const override
    {
        return current_sequence_value;
    }

};

class Snapshot_range_source final : public Data_source
{
public:
    std::vector<Test_sample>   samples;
    std::uint64_t              snapshot_sequence      = 1;
    std::uint64_t              current_sequence_value = 1;
    int                        snapshot_calls         = 0;

    snapshot_result_t try_snapshot(std::size_t /*lod_level*/) override
    {
        ++snapshot_calls;
        data_snapshot_t snapshot{
            samples.data(),
            samples.size(),
            sizeof(Test_sample),
            snapshot_sequence,
            nullptr,
            0,
            std::make_shared<int>(11)
        };
        if (samples.empty()) {
            return {data_snapshot_t{}, snapshot_result_t::Snapshot_status::EMPTY};
        }
        return {snapshot, snapshot_result_t::Snapshot_status::READY};
    }

    std::size_t sample_stride() const override { return sizeof(Test_sample); }
    std::uint64_t current_sequence(std::size_t /*lod_level*/) const override
    {
        return current_sequence_value;
    }
};

class Counting_profiler final : public plot::Profiler
{
public:
    void begin_scope(const char* /*name*/) override {}
    void end_scope() override {}
    void record_observation(const char* name, double value) override
    {
        observations[name ? name : ""] += value;
    }

    double total(const std::string& name) const
    {
        const auto found = observations.find(name);
        return found == observations.end() ? 0.0 : found->second;
    }

    std::map<std::string, double> observations;
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
    policy.layout_key = 0x54455354;
    return policy;
}

Data_access_policy make_stable_policy()
{
    Data_access_policy policy = make_policy();
    policy.set_semantics_key(k_stable_policy_semantics, 1);
    return policy;
}

Data_access_policy make_member_pointer_policy()
{
    return plot::make_access_policy<Test_sample>(
        &Test_sample::t,
        &Test_sample::v).erase();
}

Data_access_policy make_value_only_policy()
{
    Data_access_policy policy;
    policy.get_value = [](const void* sample) {
        return static_cast<const Test_sample*>(sample)->v;
    };
    policy.layout_key = 0x56414C55;
    return policy;
}

std::shared_ptr<series_data_t> make_series(const std::shared_ptr<Query_range_source>& source)
{
    auto series         = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = source;
    series->access      = make_policy();
    return series;
}

std::shared_ptr<series_data_t> make_series(const std::shared_ptr<Snapshot_range_source>& source)
{
    auto series         = std::make_shared<series_data_t>();
    series->style       = Display_style::LINE;
    series->data_source = source;
    series->access      = make_policy();
    return series;
}

std::shared_ptr<series_data_t> make_stable_series(
    const std::shared_ptr<Query_range_source>& source)
{
    auto series    = make_series(source);
    series->access = make_stable_policy();
    return series;
}

std::map<int, std::shared_ptr<const series_data_t>> make_series_map(
    const std::shared_ptr<series_data_t>& series)
{
    std::map<int, std::shared_ptr<const series_data_t>> series_map;
    series_map[7] = series;
    return series_map;
}

data_config_t make_data_config()
{
    data_config_t cfg;
    cfg.t_min           = 10;
    cfg.t_max           = 20;
    cfg.t_available_min = 0;
    cfg.t_available_max = 100;
    cfg.v_min           = -100.0f;
    cfg.v_max           = 100.0f;
    cfg.v_manual_min    = -10.0f;
    cfg.v_manual_max    = 10.0f;
    return cfg;
}

bool test_visible_auto_range_uses_source_query_without_snapshot_fallback()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::READY;
    source->query_range  = {2.0f, 5.0f};

    auto series = make_series(source);
    series->interpolation         = Series_interpolation::STEP_AFTER;
    series->empty_window_behavior = Empty_window_behavior::HOLD_LAST_FORWARD;

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == 2.0f && range.second == 5.0f,
        "visible auto-range should use the source query result");
    TEST_ASSERT(source->query_calls == 1,
        "visible auto-range should call query_v_range once");
    TEST_ASSERT(source->snapshot_calls == 0,
        "READY query result should avoid snapshot fallback");
    TEST_ASSERT(source->last_query_lod == 0,
        "VISIBLE mode should query LOD 0");
    TEST_ASSERT(source->last_query.time_window.min_ns == 10 &&
        source->last_query.time_window.max_ns == 20,
                "visible query should use the configured time window");
    TEST_ASSERT(source->last_query.interpolation == Series_interpolation::STEP_AFTER,
        "query should carry series interpolation");
    TEST_ASSERT(source->last_query.empty_window_behavior ==
        Empty_window_behavior::HOLD_LAST_FORWARD,
                "query should carry series empty-window behavior");
    TEST_ASSERT(source->last_query.semantics_key.value == 0,
        "default access semantics key should not reuse layout identity");
    TEST_ASSERT(source->last_query.semantics_key.conservative,
        "default access semantics key should be conservative");
    TEST_ASSERT(source->last_query.semantics_key.revision != 0,
        "conservative access semantics should carry accessor revision");

    return true;
}

bool test_member_pointer_query_uses_stable_semantics_key()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::READY;
    source->query_range  = {2.0f, 5.0f};

    auto series    = make_series(source);
    series->access = make_member_pointer_policy();

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == 2.0f && range.second == 5.0f,
        "member-pointer query should use the source query result");
    TEST_ASSERT(!source->last_query.semantics_key.conservative,
        "member-pointer query should expose stable semantics");
    TEST_ASSERT(source->last_query.semantics_key.value != 0,
        "member-pointer query semantics key should be non-zero");
    TEST_ASSERT(source->last_query.semantics_key.value != series->access.layout_key,
        "query semantics key should not reuse layout identity");
    TEST_ASSERT(source->last_query.semantics_key.revision == 0,
        "member-pointer query semantics should not consume accessor revision");

    return true;
}

bool test_global_lod_auto_range_uses_query_when_no_legacy_range_exists()
{
    auto source = std::make_shared<Query_range_source>();
    source->levels       = 2;
    source->query_status = Data_query_status::READY;
    source->query_range  = {-4.0f, 9.0f};

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL_LOD;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == -4.0f && range.second == 9.0f,
        "GLOBAL_LOD auto-range should use query_v_range when no legacy O(1) range exists");
    TEST_ASSERT(source->last_query_lod == 1,
        "GLOBAL_LOD should query the last LOD level");
    TEST_ASSERT(source->snapshot_calls == 0,
        "READY global query result should avoid snapshot fallback");

    return true;
}

bool test_unsupported_query_falls_back_to_snapshot_scan()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::UNSUPPORTED;
    source->samples      = {
        { 0,  6.0f  },
        { 5,  8.0f  },
        { 10, 12.0f },
    };

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == 6.0f && range.second == 12.0f,
        "unsupported query should fall back to snapshot scan");
    TEST_ASSERT(source->query_calls == 1,
        "resolver should try query_v_range before fallback");
    TEST_ASSERT(source->snapshot_calls == 1,
        "unsupported query should take one fallback snapshot");

    return true;
}

bool test_ready_query_profiler_counts_query_without_scan()
{
    auto profiler = std::make_shared<Counting_profiler>();
    auto source   = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::READY;
    source->query_range  = {2.0f, 5.0f};

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL;
    config.profiler          = profiler;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == 2.0f && range.second == 5.0f,
        "READY query should resolve the auto-range");
    TEST_ASSERT(profiler->total("renderer.auto_range.query_count") == 1.0,
        "READY query should increment the auto-range query counter");
    TEST_ASSERT(profiler->total("renderer.auto_range.range_scan_count") == 0.0,
        "READY query should not be counted as a range scan");
    TEST_ASSERT(source->snapshot_calls == 0,
        "READY query should avoid snapshot work");

    return true;
}

bool test_default_query_profiler_counts_snapshot_scan()
{
    auto profiler = std::make_shared<Counting_profiler>();
    auto source   = std::make_shared<Snapshot_range_source>();
    source->samples = {
        { 0,  6.0f  },
        { 5,  8.0f  },
        { 10, 12.0f },
    };

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL;
    config.profiler          = profiler;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == 6.0f && range.second == 12.0f,
        "default query should resolve the auto-range through its snapshot scan");
    TEST_ASSERT(profiler->total("renderer.auto_range.query_count") == 1.0,
        "default query should increment the auto-range query counter");
    TEST_ASSERT(profiler->total("renderer.auto_range.range_scan_count") == 1.0,
        "default query snapshot scan should increment the range scan counter");
    TEST_ASSERT(source->snapshot_calls == 1,
        "default query should take one snapshot");

    return true;
}

bool test_positive_auto_range_excludes_zero_by_default()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::READY;
    source->query_range  = {2.0f, 8.0f};

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == 2.0f && range.second == 8.0f,
        "positive-only auto-range should preserve the positive data range by default");
    TEST_ASSERT(range.first > 0.0f && range.second > 0.0f,
        "positive-only auto-range should not force zero into the default range");

    return true;
}

bool test_negative_auto_range_excludes_zero_by_default()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::READY;
    source->query_range  = {-8.0f, -2.0f};

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == -8.0f && range.second == -2.0f,
        "negative-only auto-range should preserve the negative data range by default");
    TEST_ASSERT(range.first < 0.0f && range.second < 0.0f,
        "negative-only auto-range should not force zero into the default range");

    return true;
}

bool test_nonnegative_auto_range_floor_policy_includes_zero()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::READY;
    source->query_range  = {1.0f, 3.0f};

    Plot_config config;
    config.auto_v_range_mode                      = Auto_v_range_mode::GLOBAL;
    config.auto_v_range_extra_scale               = 2.0;
    config.floor_nonnegative_auto_v_range_at_zero = true;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == 0.0f && range.second == 5.0f,
        "nonnegative auto-range floor policy should clamp padded lower bound to zero");

    return true;
}

bool test_visible_step_after_draw_nothing_contributes_held_sample()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::UNSUPPORTED;
    source->samples = {{0, 5.0f}, {10, 1.0f}};
    auto series = make_series(source);
    series->interpolation = Series_interpolation::STEP_AFTER;
    series->empty_window_behavior = Empty_window_behavior::DRAW_NOTHING;
    auto data = make_data_config();
    data.t_min = 5;
    data.t_max = 15;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;
    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(series), data, config, true);
    TEST_ASSERT(range.first == 1.0f && range.second == 5.0f,
        "visible auto-range must include the held STEP_AFTER value drawn in the window");
    return true;
}

bool test_visible_step_after_hold_forward_contributes_held_sample()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::UNSUPPORTED;
    source->samples      = {
        { 5,  -4.0f  },
        { 15, 6.0f   },
        { 25, 100.0f },
    };

    auto series = make_series(source);
    series->interpolation         = Series_interpolation::STEP_AFTER;
    series->empty_window_behavior = Empty_window_behavior::HOLD_LAST_FORWARD;

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == -4.0f && range.second == 6.0f,
        "visible STEP_AFTER auto-range should include the held sample entering the window");
    TEST_ASSERT(source->query_calls == 1,
        "visible STEP_AFTER auto-range should try query_v_range before fallback");
    TEST_ASSERT(source->snapshot_calls == 1,
        "unsupported STEP_AFTER override should use one snapshot through the base query");

    return true;
}

bool test_visible_step_after_skip_fallback_keeps_earlier_drawable_held_sample()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();

    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::UNSUPPORTED;
    source->samples      = {
        { 5,  -4.0f },
        { 9,  nan   },
        { 15, 6.0f  },
    };

    auto series = make_series(source);
    series->interpolation         = Series_interpolation::STEP_AFTER;
    series->empty_window_behavior = Empty_window_behavior::HOLD_LAST_FORWARD;
    series->nonfinite_policy      = plot::Nonfinite_sample_policy::SKIP;

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == -4.0f && range.second == 6.0f,
        "SKIP fallback auto-range should keep the earlier drawable held sample");
    TEST_ASSERT(source->query_calls == 1,
        "SKIP fallback auto-range should try query_v_range before fallback");
    TEST_ASSERT(source->snapshot_calls == 1,
        "SKIP fallback auto-range should take one snapshot scan");

    return true;
}

bool test_global_value_only_access_falls_back_to_snapshot_scan()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::UNSUPPORTED;
    source->samples      = {
        { 0, -3.0f },
        { 0, 8.0f  },
        { 0, 2.0f  },
    };

    auto series    = make_series(source);
    series->access = make_value_only_policy();

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true);

    TEST_ASSERT(range.first == -3.0f && range.second == 8.0f,
        "global value-only access should use snapshot scan fallback");
    TEST_ASSERT(source->query_calls == 1,
        "value-only fallback should still try the source query first");
    TEST_ASSERT(source->snapshot_calls == 1,
        "value-only fallback should take one snapshot");

    return true;
}

bool test_failed_query_does_not_fall_back_to_stale_scan()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::FAILED;
    source->samples      = {
        { 0, 1.0f },
        { 5, 2.0f },
    };

    const data_config_t cfg = make_data_config();
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        cfg,
        config,
        true);

    TEST_ASSERT(range.first == cfg.v_min && range.second == cfg.v_max,
        "FAILED query should be authoritative and leave configured fallback range");
    TEST_ASSERT(source->query_calls == 1,
        "FAILED source should still be queried once");
    TEST_ASSERT(source->snapshot_calls == 0,
        "FAILED query must not be silently downgraded to snapshot fallback");

    return true;
}

bool test_ready_query_result_is_cached_by_current_sequence()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status           = Data_query_status::READY;
    source->query_range            = {1.0f, 4.0f};
    source->query_sequence         = 5;
    source->current_sequence_value = 5;

    auto series     = make_stable_series(source);
    auto series_map = make_series_map(series);
    plot::detail::auto_range_cache_t cache;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    const auto first = plot::detail::resolve_main_v_range(
        series_map,
        make_data_config(),
        config,
        true,
        &cache);
    const auto second = plot::detail::resolve_main_v_range(
        series_map,
        make_data_config(),
        config,
        true,
        &cache);

    TEST_ASSERT(first.first == 1.0f && first.second == 4.0f,
        "first range should come from query");
    TEST_ASSERT(second.first == 1.0f && second.second == 4.0f,
        "second range should come from cache");
    TEST_ASSERT(source->query_calls == 1,
        "matching sequence and query shape should reuse cached range");

    return true;
}

bool test_conservative_query_result_is_not_cached()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status           = Data_query_status::READY;
    source->query_range            = {1.0f, 4.0f};
    source->query_sequence         = 5;
    source->current_sequence_value = 5;

    auto series     = make_series(source);
    auto series_map = make_series_map(series);
    plot::detail::auto_range_cache_t cache;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    (void)plot::detail::resolve_main_v_range(
        series_map,
        make_data_config(),
        config,
        true,
        &cache);
    (void)plot::detail::resolve_main_v_range(
        series_map,
        make_data_config(),
        config,
        true,
        &cache);

    TEST_ASSERT(source->query_calls == 2,
        "conservative callable semantics should not reuse cached query results");
    TEST_ASSERT(cache.main_entries.empty(),
        "conservative callable semantics should not populate resolver cache");

    return true;
}

bool test_empty_query_result_is_cached_by_current_sequence()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status           = Data_query_status::EMPTY;
    source->query_sequence         = 5;
    source->current_sequence_value = 5;

    auto series     = make_stable_series(source);
    auto series_map = make_series_map(series);
    plot::detail::auto_range_cache_t cache;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;
    const data_config_t cfg = make_data_config();

    const auto first = plot::detail::resolve_main_v_range(
        series_map,
        cfg,
        config,
        true,
        &cache);
    const auto second = plot::detail::resolve_main_v_range(
        series_map,
        cfg,
        config,
        true,
        &cache);

    TEST_ASSERT(first.first == cfg.v_min && first.second == cfg.v_max,
        "empty query should use configured fallback range");
    TEST_ASSERT(second.first == cfg.v_min && second.second == cfg.v_max,
        "cached empty query should keep configured fallback range");
    TEST_ASSERT(source->query_calls == 1,
        "matching empty query should be cached by sequence");

    return true;
}

bool test_sequence_change_invalidates_auto_range_cache()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status           = Data_query_status::READY;
    source->query_range            = {1.0f, 4.0f};
    source->query_sequence         = 5;
    source->current_sequence_value = 5;

    auto series     = make_stable_series(source);
    auto series_map = make_series_map(series);
    plot::detail::auto_range_cache_t cache;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    (void)plot::detail::resolve_main_v_range(
        series_map,
        make_data_config(),
        config,
        true,
        &cache);

    source->query_range            = {-3.0f, 9.0f};
    source->query_sequence         = 6;
    source->current_sequence_value = 6;
    const auto range = plot::detail::resolve_main_v_range(
        series_map,
        make_data_config(),
        config,
        true,
        &cache);

    TEST_ASSERT(range.first == -3.0f && range.second == 9.0f,
        "sequence change should force a fresh query result");
    TEST_ASSERT(source->query_calls == 2,
        "sequence change should invalidate the cached range");

    return true;
}

bool test_visible_window_change_invalidates_auto_range_cache()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status           = Data_query_status::READY;
    source->query_range            = {1.0f, 4.0f};
    source->query_sequence         = 5;
    source->current_sequence_value = 5;

    auto series     = make_stable_series(source);
    auto series_map = make_series_map(series);
    plot::detail::auto_range_cache_t cache;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;
    data_config_t cfg = make_data_config();

    (void)plot::detail::resolve_main_v_range(
        series_map,
        cfg,
        config,
        true,
        &cache);

    cfg.t_min           = 30;
    cfg.t_max           = 40;
    source->query_range = {-6.0f, -2.0f};
    const auto range = plot::detail::resolve_main_v_range(
        series_map,
        cfg,
        config,
        true,
        &cache);

    TEST_ASSERT(range.first == -6.0f && range.second == -2.0f,
        "visible window change should force a fresh query result");
    TEST_ASSERT(source->query_calls == 2,
        "visible window should be part of the cache key");

    return true;
}

bool test_access_policy_change_invalidates_auto_range_cache()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status           = Data_query_status::READY;
    source->query_range            = {1.0f, 4.0f};
    source->query_sequence         = 5;
    source->current_sequence_value = 5;

    auto series = make_stable_series(source);
    plot::detail::auto_range_cache_t cache;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    (void)plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true,
        &cache);

    auto changed_series = make_stable_series(source);
    changed_series->access.layout_key = series->access.layout_key;
    source->query_range = {10.0f, 12.0f};
    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(changed_series),
        make_data_config(),
        config,
        true,
        &cache);

    TEST_ASSERT(range.first == 10.0f && range.second == 12.0f,
        "access-policy identity change should not reuse an old range");
    TEST_ASSERT(source->query_calls == 2,
        "cache key should include access-policy identity");

    return true;
}

bool test_semantics_revision_change_invalidates_auto_range_cache()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status           = Data_query_status::READY;
    source->query_range            = {1.0f, 4.0f};
    source->query_sequence         = 5;
    source->current_sequence_value = 5;

    auto series = make_stable_series(source);
    plot::detail::auto_range_cache_t cache;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    (void)plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true,
        &cache);

    series->access.set_semantics_key(k_stable_policy_semantics, 2);
    source->query_range = {8.0f, 10.0f};
    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true,
        &cache);

    TEST_ASSERT(range.first == 8.0f && range.second == 10.0f,
        "semantics revision change should force a fresh query result");
    TEST_ASSERT(source->query_calls == 2,
        "cache key should include explicit semantics revision");

    return true;
}

bool test_removed_series_prunes_auto_range_cache()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status           = Data_query_status::READY;
    source->query_range            = {1.0f, 4.0f};
    source->query_sequence         = 5;
    source->current_sequence_value = 5;

    auto series = make_stable_series(source);
    plot::detail::auto_range_cache_t cache;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;

    (void)plot::detail::resolve_main_v_range(
        make_series_map(series),
        make_data_config(),
        config,
        true,
        &cache);
    TEST_ASSERT(cache.main_entries.size() == 1,
        "first query should populate one cache entry");

    std::map<int, std::shared_ptr<const series_data_t>> empty_series;
    (void)plot::detail::resolve_main_v_range(
        empty_series,
        make_data_config(),
        config,
        true,
        &cache);
    TEST_ASSERT(cache.main_entries.empty(),
        "removed series should be pruned from auto-range cache");

    return true;
}

bool test_preview_auto_range_uses_preview_query_source()
{
    auto main_source    = std::make_shared<Query_range_source>();
    auto preview_source = std::make_shared<Query_range_source>();
    preview_source->query_status = Data_query_status::READY;
    preview_source->query_range  = {-2.0f, 11.0f};

    auto series = make_series(main_source);
    plot::preview_config_t preview;
    preview.data_source    = preview_source;
    preview.access         = make_policy();
    preview.style          = Display_style::AREA;
    series->preview_config = preview;

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL;

    const auto range = plot::detail::resolve_preview_v_range(
        make_series_map(series),
        make_data_config(),
        config);

    TEST_ASSERT(range.first == -2.0f && range.second == 11.0f,
        "preview auto-range should use the preview source query");
    TEST_ASSERT(main_source->query_calls == 0,
        "preview resolver should not query the main source when a preview source is set");
    TEST_ASSERT(preview_source->query_calls == 1,
        "preview resolver should query preview source");
    TEST_ASSERT(preview_source->snapshot_calls == 0,
        "READY preview query should avoid snapshot fallback");

    return true;
}

bool test_frame_range_planner_populates_ranges_and_reuses_cache()
{
    auto main_source = std::make_shared<Query_range_source>();
    main_source->query_status           = Data_query_status::READY;
    main_source->query_range            = {2.0f, 5.0f};
    main_source->query_sequence         = 8;
    main_source->current_sequence_value = 8;

    auto preview_source = std::make_shared<Query_range_source>();
    preview_source->query_status           = Data_query_status::READY;
    preview_source->query_range            = {-2.0f, 11.0f};
    preview_source->query_sequence         = 9;
    preview_source->current_sequence_value = 9;

    auto series = make_stable_series(main_source);
    plot::preview_config_t preview;
    preview.data_source    = preview_source;
    preview.access         = make_stable_policy();
    preview.style          = Display_style::AREA;
    series->preview_config = preview;

    auto series_map = make_series_map(series);
    plot::detail::Frame_range_planner planner;
    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::GLOBAL;

    const auto first = planner.plan(
        series_map,
        make_data_config(),
        config,
        true,
        true);
    const auto second = planner.plan(
        series_map,
        make_data_config(),
        config,
        true,
        true);

    TEST_ASSERT(first.main_v_range.valid && first.main_v_range.min == 2.0f &&
        first.main_v_range.max == 5.0f,
                "frame planner should populate the main range plan");
    TEST_ASSERT(first.preview_v_range.valid && first.preview_v_range.min == -2.0f &&
        first.preview_v_range.max == 11.0f,
                "frame planner should populate the preview range plan");
    TEST_ASSERT(second.main_v_range.min == 2.0f && second.main_v_range.max == 5.0f,
        "frame planner should reuse cached main range values");
    TEST_ASSERT(second.preview_v_range.min == -2.0f && second.preview_v_range.max == 11.0f,
        "frame planner should reuse cached preview range values");
    TEST_ASSERT(main_source->query_calls == 1,
        "frame planner should preserve main range cache reuse");
    TEST_ASSERT(preview_source->query_calls == 1,
        "frame planner should preserve preview range cache reuse");

    main_source->query_range            = {-4.0f, 12.0f};
    main_source->query_sequence         = 10;
    main_source->current_sequence_value = 10;
    const auto after_sequence_change = planner.plan(
        series_map,
        make_data_config(),
        config,
        true,
        true);

    TEST_ASSERT(after_sequence_change.main_v_range.min == -4.0f &&
        after_sequence_change.main_v_range.max == 12.0f,
                "frame planner should preserve sequence-based main cache invalidation");
    TEST_ASSERT(main_source->query_calls == 2,
        "main sequence change should force a fresh planner query");
    TEST_ASSERT(preview_source->query_calls == 1,
        "unchanged preview range should remain cached");

    return true;
}

bool test_frame_range_planner_skips_preview_when_disabled()
{
    auto main_source = std::make_shared<Query_range_source>();
    main_source->query_status = Data_query_status::READY;
    main_source->query_range  = {1.0f, 4.0f};

    auto preview_source = std::make_shared<Query_range_source>();
    preview_source->query_status = Data_query_status::READY;
    preview_source->query_range  = {-2.0f, 11.0f};

    auto series = make_series(main_source);
    plot::preview_config_t preview;
    preview.data_source    = preview_source;
    preview.access         = make_policy();
    series->preview_config = preview;

    plot::detail::Frame_range_planner planner;
    Plot_config config;
    const auto plan = planner.plan(
        make_series_map(series),
        make_data_config(),
        config,
        true,
        false);

    TEST_ASSERT(plan.main_v_range.min == 1.0f && plan.main_v_range.max == 4.0f,
        "disabled-preview planner should still compute the main range");
    TEST_ASSERT(plan.preview_v_range.min == 1.0f && plan.preview_v_range.max == 4.0f,
        "disabled-preview planner should mirror the main range");
    TEST_ASSERT(main_source->query_calls == 1,
        "disabled-preview planner should query the main source");
    TEST_ASSERT(preview_source->query_calls == 0,
        "disabled-preview planner should not query preview sources");

    return true;
}

bool test_frame_range_planner_applies_manual_range_to_preview()
{
    auto main_source = std::make_shared<Query_range_source>();
    main_source->query_status = Data_query_status::READY;
    main_source->query_range  = {1.0f, 4.0f};

    auto preview_source = std::make_shared<Query_range_source>();
    preview_source->query_status = Data_query_status::READY;
    preview_source->query_range  = {-2.0f, 11.0f};

    auto series = make_series(main_source);
    plot::preview_config_t preview;
    preview.data_source    = preview_source;
    preview.access         = make_policy();
    series->preview_config = preview;

    plot::detail::Frame_range_planner planner;
    const auto data_cfg = make_data_config();
    const auto plan = planner.plan(
        make_series_map(series),
        data_cfg,
        Plot_config{},
        false,
        true);

    TEST_ASSERT(plan.main_v_range.min == data_cfg.v_manual_min &&
        plan.main_v_range.max == data_cfg.v_manual_max,
                "manual main range should use the configured limits");
    TEST_ASSERT(plan.preview_v_range.min == data_cfg.v_manual_min &&
        plan.preview_v_range.max == data_cfg.v_manual_max,
                "manual main range should also apply to the preview");
    TEST_ASSERT(main_source->query_calls == 0 && preview_source->query_calls == 0,
        "manual range should not query main or preview sources");

    return true;
}

bool test_frame_range_planner_preserves_step_after_visible_scan()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::UNSUPPORTED;
    source->samples      = {
        { 5,  -3.0f  },
        { 15, 8.0f   },
        { 25, 100.0f },
    };

    auto series = make_series(source);
    series->interpolation         = Series_interpolation::STEP_AFTER;
    series->empty_window_behavior = Empty_window_behavior::HOLD_LAST_FORWARD;

    Plot_config config;
    config.auto_v_range_mode = Auto_v_range_mode::VISIBLE;
    plot::detail::Frame_range_planner planner;

    const auto plan = planner.plan(
        make_series_map(series),
        make_data_config(),
        config,
        true,
        false);

    TEST_ASSERT(plan.main_v_range.min == -3.0f && plan.main_v_range.max == 8.0f,
        "frame planner should preserve visible STEP_AFTER held-sample scan behavior");
    TEST_ASSERT(source->query_calls == 1,
        "frame planner should try query_v_range before step-after scan fallback");
    TEST_ASSERT(source->snapshot_calls == 1,
        "unsupported step-after query should fall back to one snapshot scan");

    return true;
}

bool test_manual_range_skips_queries()
{
    auto source = std::make_shared<Query_range_source>();
    source->query_status = Data_query_status::READY;
    source->query_range  = {-2.0f, 11.0f};

    const data_config_t cfg = make_data_config();
    Plot_config config;

    const auto range = plot::detail::resolve_main_v_range(
        make_series_map(make_series(source)),
        cfg,
        config,
        false);

    TEST_ASSERT(range.first == cfg.v_manual_min && range.second == cfg.v_manual_max,
        "manual v-range should return manual config values");
    TEST_ASSERT(source->query_calls == 0,
        "manual v-range should not query sources");
    TEST_ASSERT(source->snapshot_calls == 0,
        "manual v-range should not snapshot sources");

    return true;
}

bool test_stacked_auto_range_includes_cumulative_envelope()
{
    auto lower = std::make_shared<Query_range_source>();
    auto upper = std::make_shared<Query_range_source>();
    lower->query_status = upper->query_status = Data_query_status::READY;
    lower->query_range  = {1.0f, 2.0f};
    upper->query_range  = {10.0f, 20.0f};
    auto lower_series = make_series(lower);
    auto upper_series = make_series(upper);
    lower_series->stack_group = upper_series->stack_group = 1;
    std::map<int, std::shared_ptr<const series_data_t>> series{
        {1, lower_series}, {2, upper_series}};

    const auto range = plot::detail::resolve_main_v_range(
        series, make_data_config(), Plot_config{}, true);
    TEST_ASSERT(range.first == 0.0f && range.second == 22.0f,
        "stacked auto-range should include the baseline and cumulative top");
    return true;
}

}  // namespace

int main()
{
    std::cout << "Production auto-range query tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_visible_auto_range_uses_source_query_without_snapshot_fallback);
    RUN_TEST(test_member_pointer_query_uses_stable_semantics_key);
    RUN_TEST(test_global_lod_auto_range_uses_query_when_no_legacy_range_exists);
    RUN_TEST(test_unsupported_query_falls_back_to_snapshot_scan);
    RUN_TEST(test_ready_query_profiler_counts_query_without_scan);
    RUN_TEST(test_default_query_profiler_counts_snapshot_scan);
    RUN_TEST(test_positive_auto_range_excludes_zero_by_default);
    RUN_TEST(test_negative_auto_range_excludes_zero_by_default);
    RUN_TEST(test_nonnegative_auto_range_floor_policy_includes_zero);
    RUN_TEST(test_visible_step_after_draw_nothing_contributes_held_sample);
    RUN_TEST(test_visible_step_after_hold_forward_contributes_held_sample);
    RUN_TEST(test_visible_step_after_skip_fallback_keeps_earlier_drawable_held_sample);
    RUN_TEST(test_global_value_only_access_falls_back_to_snapshot_scan);
    RUN_TEST(test_failed_query_does_not_fall_back_to_stale_scan);
    RUN_TEST(test_ready_query_result_is_cached_by_current_sequence);
    RUN_TEST(test_conservative_query_result_is_not_cached);
    RUN_TEST(test_empty_query_result_is_cached_by_current_sequence);
    RUN_TEST(test_sequence_change_invalidates_auto_range_cache);
    RUN_TEST(test_visible_window_change_invalidates_auto_range_cache);
    RUN_TEST(test_access_policy_change_invalidates_auto_range_cache);
    RUN_TEST(test_semantics_revision_change_invalidates_auto_range_cache);
    RUN_TEST(test_removed_series_prunes_auto_range_cache);
    RUN_TEST(test_preview_auto_range_uses_preview_query_source);
    RUN_TEST(test_frame_range_planner_populates_ranges_and_reuses_cache);
    RUN_TEST(test_frame_range_planner_skips_preview_when_disabled);
    RUN_TEST(test_frame_range_planner_applies_manual_range_to_preview);
    RUN_TEST(test_frame_range_planner_preserves_step_after_visible_scan);
    RUN_TEST(test_manual_range_skips_queries);
    RUN_TEST(test_stacked_auto_range_includes_cumulative_envelope);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;

    return failed > 0 ? 1 : 0;
}
