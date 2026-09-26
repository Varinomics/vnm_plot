// vnm_plot Data_source query API tests

#include "test_macros.h"

#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/types.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace plot = vnm::plot;

namespace {

struct sample_t
{
    std::int64_t   t = 0;
    float          v = 0.0f;
};

constexpr std::uint64_t k_query_semantics_key = 0x5155455259;

class Query_source final : public plot::Data_source
{
public:
    Query_source() = default;

    explicit Query_source(std::vector<sample_t> samples)
    :
        m_samples(std::move(samples))
    {}

    plot::snapshot_result_t try_snapshot(std::size_t /*lod*/) override
    {
        ++m_snapshot_calls;

        plot::data_snapshot_t snapshot;
        snapshot.data     = m_samples.data();
        snapshot.count    = m_samples.size();
        snapshot.stride   = sizeof(sample_t);
        snapshot.sequence = m_sequence;

        if (m_status == plot::snapshot_result_t::Snapshot_status::READY && m_samples.empty()) {
            return {snapshot, plot::snapshot_result_t::Snapshot_status::EMPTY};
        }

        return {snapshot, m_status};
    }

    std::size_t sample_stride() const override { return sizeof(sample_t); }
    std::size_t lod_levels()    const override { return m_scales.size();  }
    std::size_t lod_scale(std::size_t level) const override
    {
        return level < m_scales.size() ? m_scales[level] : 1;
    }
    plot::Time_order time_order(std::size_t /*lod*/) const override { return m_time_order; }

    void set_status(plot::snapshot_result_t::Snapshot_status status) { m_status = status; }
    void set_sequence(std::uint64_t sequence) { m_sequence = sequence; }
    void set_scales(std::vector<std::size_t> scales) { m_scales = std::move(scales); }
    void set_time_order(plot::Time_order order) { m_time_order = order; }
    int snapshot_calls() const { return m_snapshot_calls; }

private:
    std::vector<sample_t>      m_samples;
    std::vector<std::size_t>   m_scales = {1};
    plot::snapshot_result_t::Snapshot_status m_status =
        plot::snapshot_result_t::Snapshot_status::READY;
    plot::Time_order           m_time_order     = plot::Time_order::UNKNOWN;
    std::uint64_t              m_sequence       = 11;
    int                        m_snapshot_calls = 0;
};

plot::Data_access_policy make_value_access(
    int*   timestamp_calls = nullptr,
    int*   value_calls = nullptr)
{
    plot::Data_access_policy access;
    access.get_timestamp = [timestamp_calls](const void* sample) -> std::int64_t {
        if (timestamp_calls) {
            ++*timestamp_calls;
        }
        return static_cast<const sample_t*>(sample)->t;
    };
    access.get_value = [value_calls](const void* sample) {
        if (value_calls) {
            ++*value_calls;
        }
        return static_cast<const sample_t*>(sample)->v;
    };
    access.layout_key = 17;
    return access;
}

plot::Data_access_policy make_timestamp_access(int* timestamp_calls = nullptr)
{
    plot::Data_access_policy access;
    access.get_timestamp = [timestamp_calls](const void* sample) -> std::int64_t {
        if (timestamp_calls) {
            ++*timestamp_calls;
        }
        return static_cast<const sample_t*>(sample)->t;
    };
    return access;
}

plot::data_query_context_t make_query(
    const plot::Data_access_policy&    access,
    std::int64_t                       t_min,
    std::int64_t                       t_max)
{
    plot::data_query_context_t query;
    query.access                     = &access;
    query.semantics_key.value        = k_query_semantics_key;
    query.semantics_key.revision     = 1;
    query.semantics_key.conservative = false;
    query.time_window                = {t_min, t_max};
    return query;
}

plot::data_query_context_t make_hold_query(
    const plot::Data_access_policy&    access,
    std::int64_t                       t_min,
    std::int64_t                       t_max)
{
    plot::data_query_context_t query = make_query(access, t_min, t_max);
    query.interpolation         = plot::Series_interpolation::STEP_AFTER;
    query.empty_window_behavior = plot::Empty_window_behavior::HOLD_LAST_FORWARD;
    return query;
}

plot::data_query_context_t make_draw_query(
    const plot::Data_access_policy&    access,
    std::int64_t                       t_min,
    std::int64_t                       t_max)
{
    plot::data_query_context_t query = make_query(access, t_min, t_max);
    query.empty_window_behavior      = plot::Empty_window_behavior::DRAW_NOTHING;
    return query;
}

bool test_query_v_range_without_access_is_unsupported()
{
    Query_source source({{0, 1.0f}});

    plot::data_query_context_t query;
    query.time_window = {0, 10};
    const auto result = source.query_v_range(0, query);
    TEST_ASSERT(result.status == plot::Data_query_status::UNSUPPORTED,
        "query_v_range without access policy should report UNSUPPORTED");

    return true;
}

bool test_ready_value_range_scan_populates_sequence()
{
    Query_source source(
        {
            { 0, 3.0f  },
            { 1, -2.0f },
            { 2, 5.0f  },
            });
    source.set_sequence(123);

    const plot::Data_access_policy access = make_value_access();
    const auto                     query  = make_query(access, 0, 2);
    const auto                     result = source.query_v_range(0, query);
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "finite value-range query should be READY");
    TEST_ASSERT(result.sequence == 123,
        "READY value-range query should carry snapshot sequence");
    TEST_ASSERT(result.value.min == -2.0f && result.value.max == 5.0f,
        "value-range query should scan finite matching samples");

    return true;
}

bool test_ascending_value_range_scans_only_selected_time_window()
{
    std::vector<sample_t> samples;
    samples.reserve(1024);
    for (std::int64_t i = 0; i < 1024; ++i) {
        samples.push_back({i, static_cast<float>(i)});
    }

    Query_source source(std::move(samples));
    source.set_time_order(plot::Time_order::ASCENDING);

    int timestamp_calls = 0;
    int value_calls     = 0;

    const plot::Data_access_policy access =
        make_value_access(&timestamp_calls, &value_calls);
    const auto result = source.query_v_range(0, make_query(access, 500, 501));

    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "ascending visible value-range query should be READY");
    TEST_ASSERT(result.value.min == 500.0f && result.value.max == 501.0f,
        "ascending visible value-range query should scan the requested values");
    TEST_ASSERT(value_calls < 16,
        "ascending visible value-range query should not scan every sample value");
    TEST_ASSERT(timestamp_calls < 128,
        "ascending visible value-range query should use bounded timestamp lookup");

    return true;
}

bool test_ascending_skip_hold_value_range_scans_bounded_prefix()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<sample_t> samples;
    samples.reserve(1024);
    for (std::int64_t i = 0; i < 1024; ++i) {
        samples.push_back({i, static_cast<float>(i)});
    }
    samples[499].v = nan;

    Query_source source(std::move(samples));
    source.set_time_order(plot::Time_order::ASCENDING);

    int timestamp_calls = 0;
    int value_calls     = 0;

    const plot::Data_access_policy access =
        make_value_access(&timestamp_calls, &value_calls);
    auto query = make_hold_query(access, 500, 501);
    query.nonfinite_policy = plot::Nonfinite_sample_policy::SKIP;
    const auto result = source.query_v_range(0, query);

    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "ascending SKIP hold value-range query should be READY");
    TEST_ASSERT(result.value.min == 498.0f && result.value.max == 501.0f,
        "ascending SKIP hold value-range query should use the latest drawable held sample");
    TEST_ASSERT(value_calls < 32,
        "ascending SKIP hold value-range query should not scan the full prefix");
    TEST_ASSERT(timestamp_calls < 128,
        "ascending SKIP hold value-range query should use bounded timestamp lookup");

    return true;
}

bool test_unordered_value_range_aggregates_discontiguous_matches()
{
    Query_source source(
        {
            { 0,   1.0f   },
            { 100, 100.0f },
            { 5,   2.0f   },
            });
    source.set_time_order(plot::Time_order::UNORDERED);

    const plot::Data_access_policy access = make_value_access();
    const auto result = source.query_v_range(0, make_query(access, 0, 10));

    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "unordered value-range query should aggregate discontiguous matches");
    TEST_ASSERT(result.value.min == 1.0f && result.value.max == 2.0f,
        "unordered value-range query should exclude out-of-window gap samples");

    return true;
}

bool test_empty_status_for_empty_snapshot_and_no_matches()
{
    const plot::Data_access_policy access = make_value_access();

    Query_source empty_source;
    empty_source.set_sequence(201);
    const auto empty_result = empty_source.query_v_range(0, make_query(access, 0, 10));
    TEST_ASSERT(empty_result.status == plot::Data_query_status::EMPTY,
        "empty snapshot should map to EMPTY query status");
    TEST_ASSERT(empty_result.sequence == 201,
        "EMPTY query result from snapshot should carry snapshot sequence");

    Query_source no_match_source({{20, 1.0f}});
    no_match_source.set_sequence(202);
    const auto no_match_result = no_match_source.query_v_range(0, make_query(access, 0, 10));
    TEST_ASSERT(no_match_result.status == plot::Data_query_status::EMPTY,
        "query with no matching samples should report EMPTY");
    TEST_ASSERT(no_match_result.sequence == 202,
        "EMPTY no-match query should carry snapshot sequence");

    return true;
}

bool test_busy_and_failed_snapshot_status_map_through_queries()
{
    const plot::Data_access_policy access = make_value_access();
    const auto query = make_query(access, 0, 10);

    Query_source busy_source({{0, 1.0f}});
    busy_source.set_status(plot::snapshot_result_t::Snapshot_status::BUSY);
    const auto busy_result = busy_source.query_v_range(0, query);
    TEST_ASSERT(busy_result.status == plot::Data_query_status::BUSY,
        "BUSY snapshot should map to BUSY query status");

    Query_source failed_source({{0, 1.0f}});
    failed_source.set_status(plot::snapshot_result_t::Snapshot_status::FAILED);
    const auto failed_result = failed_source.query_v_range(0, query);
    TEST_ASSERT(failed_result.status == plot::Data_query_status::FAILED,
        "FAILED snapshot should map to FAILED query status");

    Query_source metadata_source({{0, 1.0f}});
    metadata_source.set_status(plot::snapshot_result_t::Snapshot_status::FAILED);
    const auto unsupported_time_range = metadata_source.time_range(0);
    TEST_ASSERT(unsupported_time_range.status == plot::Data_query_status::UNSUPPORTED,
        "default time_range should report unsupported without snapshot-backed probing");
    TEST_ASSERT(metadata_source.snapshot_calls() == 0,
        "default time_range should not call try_snapshot");

    return true;
}

bool test_nonfinite_values_are_skipped_or_zeroed_by_policy()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    const plot::Data_access_policy access = make_value_access();

    Query_source mixed_source(
        {
            { 0, 5.0f  },
            { 1, nan   },
            { 2, -2.0f },
            { 3, inf   },
            });
    const auto default_query = make_query(access, 0, 3);
    TEST_ASSERT(default_query.nonfinite_policy == plot::Nonfinite_sample_policy::BREAK_SEGMENT,
        "query default nonfinite policy should be BREAK_SEGMENT");
    TEST_ASSERT(default_query.empty_window_behavior == plot::Empty_window_behavior::HOLD_LAST_FORWARD,
        "query default empty-window behavior should be HOLD_LAST_FORWARD");
    const auto default_result = mixed_source.query_v_range(0, default_query);
    TEST_ASSERT(default_result.status == plot::Data_query_status::READY,
        "default BREAK_SEGMENT policy should exclude nonfinite values from aggregate ranges");
    TEST_ASSERT(default_result.value.min == -2.0f && default_result.value.max == 5.0f,
        "nonfinite values should not contribute to the default aggregate range");

    Query_source nonfinite_source(
        {
            { 0, nan },
            { 1, inf },
            });
    auto replace_query = make_query(access, 0, 1);
    replace_query.nonfinite_policy = plot::Nonfinite_sample_policy::REPLACE_WITH_ZERO;
    const auto replace_result = nonfinite_source.query_v_range(0, replace_query);
    TEST_ASSERT(replace_result.status == plot::Data_query_status::READY,
        "REPLACE_WITH_ZERO should turn nonfinite-only windows into a zero range");
    TEST_ASSERT(replace_result.value.min == 0.0f && replace_result.value.max == 0.0f,
        "REPLACE_WITH_ZERO should contribute zero for nonfinite values");

    auto reject_query = make_query(access, 0, 1);
    reject_query.nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    const auto reject_result = nonfinite_source.query_v_range(0, reject_query);
    TEST_ASSERT(reject_result.status == plot::Data_query_status::FAILED,
        "REJECT_WINDOW should fail when a matching sample has a nonfinite value");

    return true;
}

bool test_query_time_window_returns_simple_ascending_window()
{
    std::vector<sample_t> samples;
    for (std::int64_t i = 0; i < 64; ++i) {
        samples.push_back({i, static_cast<float>(i)});
    }

    Query_source source(std::move(samples));
    source.set_sequence(321);
    source.set_time_order(plot::Time_order::ASCENDING);

    int                            timestamp_calls = 0;
    const plot::Data_access_policy access          = make_timestamp_access(&timestamp_calls);
    const auto                     result          = source.query_time_window(0, make_draw_query(access, 20, 30));
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "query_time_window should be READY for matching ascending samples");
    TEST_ASSERT(result.sequence == 321,
        "query_time_window should carry snapshot sequence");
    TEST_ASSERT(result.value.first == 20 && result.value.count == 11,
        "ascending query_time_window should use inclusive bounds");
    TEST_ASSERT(timestamp_calls < 64,
        "ordered ascending query_time_window should avoid a full timestamp scan");

    return true;
}

bool test_query_time_window_handles_descending_inclusive_bounds()
{
    Query_source source(
        {
            { 9, 9.0f },
            { 7, 7.0f },
            { 5, 5.0f },
            { 3, 3.0f },
            { 1, 1.0f },
            });
    source.set_time_order(plot::Time_order::DESCENDING);

    const plot::Data_access_policy access = make_timestamp_access();
    const auto result = source.query_time_window(0, make_draw_query(access, 3, 7));
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "descending query_time_window should be READY for matching samples");
    TEST_ASSERT(result.value.first == 1 && result.value.count == 3,
        "descending query_time_window should use inclusive bounds");

    return true;
}

bool test_query_time_window_hold_forward_includes_held_sample()
{
    Query_source source(
        {
            { 0,  0.0f  },
            { 10, 10.0f },
            { 20, 20.0f },
            { 30, 30.0f },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_timestamp_access();
    const auto result = source.query_time_window(0, make_hold_query(access, 15, 25));
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "hold-forward time query should be READY when it includes a held sample");
    TEST_ASSERT(result.value.first == 1 && result.value.count == 2,
        "hold-forward time query should include the last pre-window sample");

    return true;
}

bool test_query_time_window_keeps_break_segment_gaps_in_window()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Query_source source(
        {
            { 0, 1.0f },
            { 1, nan  },
            { 2, 2.0f },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    const auto result = source.query_time_window(0, make_draw_query(access, 0, 2));
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "BREAK_SEGMENT time-window query should keep the containing source window");
    TEST_ASSERT(result.value.first == 0 && result.value.count == 3,
        "BREAK_SEGMENT time-window query should leave gap splitting to drawable spans");

    return true;
}

bool test_query_time_window_reject_window_fails_on_nonfinite_in_window_sample()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Query_source source(
        {
            { 0, 1.0f },
            { 1, nan  },
            { 2, 2.0f },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    auto query = make_draw_query(access, 0, 2);
    query.nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    const auto result = source.query_time_window(0, query);
    TEST_ASSERT(result.status == plot::Data_query_status::FAILED,
        "REJECT_WINDOW time-window query should fail on a nonfinite in-window sample");

    return true;
}

bool test_query_time_window_does_not_hold_nonfinite_break_segment_sample()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Query_source source(
        {
            { 0, 7.0f },
            { 2, nan },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    const auto result = source.query_time_window(0, make_hold_query(access, 3, 4));
    TEST_ASSERT(result.status == plot::Data_query_status::EMPTY,
        "time-window query should not hold across a nonfinite BREAK_SEGMENT sample");

    return true;
}

bool test_query_time_window_skip_holds_latest_drawable_sample()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Query_source source(
        {
            { 0, 7.0f },
            { 2, nan },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    auto query = make_hold_query(access, 3, 4);
    query.nonfinite_policy = plot::Nonfinite_sample_policy::SKIP;
    const auto result = source.query_time_window(0, query);
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "SKIP time-window query should hold the latest drawable pre-window sample");
    TEST_ASSERT(result.value.first == 0 && result.value.count == 1,
        "SKIP time-window query should omit skipped held candidates");

    return true;
}

bool test_query_time_window_reject_window_fails_on_nonfinite_held_sample()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Query_source source(
        {
            { 0, 7.0f },
            { 2, nan },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    auto query = make_hold_query(access, 3, 4);
    query.nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    const auto result = source.query_time_window(0, query);
    TEST_ASSERT(result.status == plot::Data_query_status::FAILED,
        "time-window REJECT_WINDOW should fail on a nonfinite held candidate");

    return true;
}

bool test_step_after_draw_nothing_includes_visible_held_sample()
{
    const auto access = make_value_access();
    for (const auto order : {plot::Time_order::UNKNOWN,
                            plot::Time_order::ASCENDING,
                            plot::Time_order::DESCENDING})
    {
        std::vector<sample_t> samples = {{0, 5.0f}, {10, 1.0f}};
        if (order == plot::Time_order::DESCENDING) {
            std::reverse(samples.begin(), samples.end());
        }
        Query_source source(std::move(samples));
        source.set_time_order(order);
        auto query = make_draw_query(access, 5, 15);
        query.interpolation = plot::Series_interpolation::STEP_AFTER;
        const auto range = source.query_v_range(0, query);
        TEST_ASSERT(range.status == plot::Data_query_status::READY &&
            range.value.min == 1.0f && range.value.max == 5.0f,
            "STEP_AFTER range must include the step drawn from the left window edge");

        query.time_window = {5, 9};
        const auto between = source.query_v_range(0, query);
        TEST_ASSERT(between.status == plot::Data_query_status::READY &&
            between.value.min == 5.0f && between.value.max == 5.0f,
            "STEP_AFTER range must include a held step whose endpoint is beyond the window");

        query.time_window = {11, 15};
        const auto after = source.query_v_range(0, query);
        TEST_ASSERT(after.status == plot::Data_query_status::EMPTY,
            "DRAW_NOTHING must not extend the last step beyond the source");
    }
    return true;
}

bool test_hold_forward_value_range_includes_pre_window_sample()
{
    Query_source source(
        {
            { 0, 10.0f },
            { 5, 1.0f  },
            { 6, 2.0f  },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    const auto result = source.query_v_range(0, make_hold_query(access, 5, 6));
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "hold-forward value range should include matching samples and held sample");
    TEST_ASSERT(result.value.min == 1.0f && result.value.max == 10.0f,
        "held pre-window value should contribute to the aggregate range");

    return true;
}

bool test_hold_forward_value_range_ready_from_held_sample_only()
{
    Query_source source(
        {
            { 0, 7.0f },
            { 2, 9.0f },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    const auto result = source.query_v_range(0, make_hold_query(access, 3, 4));
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "hold-forward value range should be READY with only a held sample");
    TEST_ASSERT(result.value.min == 9.0f && result.value.max == 9.0f,
        "latest valid pre-window sample should provide the held value range");

    return true;
}

bool test_hold_forward_does_not_use_nonfinite_break_segment_sample()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Query_source source(
        {
            { 0, 7.0f },
            { 2, nan },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    const auto result = source.query_v_range(0, make_hold_query(access, 3, 4));
    TEST_ASSERT(result.status == plot::Data_query_status::EMPTY,
        "default BREAK_SEGMENT policy should not hold across a nonfinite pre-window sample");

    return true;
}

bool test_hold_forward_skip_uses_latest_drawable_pre_window_sample()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Query_source source(
        {
            { 0, 7.0f },
            { 2, nan },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    auto query = make_hold_query(access, 3, 4);
    query.nonfinite_policy = plot::Nonfinite_sample_policy::SKIP;
    const auto result = source.query_v_range(0, query);
    TEST_ASSERT(result.status == plot::Data_query_status::READY,
        "SKIP value-range query should hold the latest drawable pre-window sample");
    TEST_ASSERT(result.value.min == 7.0f && result.value.max == 7.0f,
        "SKIP held value range should come from the latest drawable sample");

    return true;
}

bool test_hold_forward_reject_window_fails_on_nonfinite_held_candidate()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Query_source source(
        {
            { 0, 7.0f },
            { 2, nan },
            });
    source.set_time_order(plot::Time_order::ASCENDING);

    const plot::Data_access_policy access = make_value_access();
    auto query = make_hold_query(access, 3, 4);
    query.nonfinite_policy = plot::Nonfinite_sample_policy::REJECT_WINDOW;
    const auto result = source.query_v_range(0, query);
    TEST_ASSERT(result.status == plot::Data_query_status::FAILED,
        "REJECT_WINDOW should fail when the held pre-window candidate is nonfinite");

    return true;
}

bool test_lod_scales_match_compute_lod_scales_and_clamp_minimum()
{
    Query_source source;
    source.set_scales({0, 1, 8, 0});

    const std::vector<std::size_t> scales = source.lod_scales();
    TEST_ASSERT(scales.size() == 4,
        "lod_scales should return one entry per LOD level");
    TEST_ASSERT(scales[0] == 1 && scales[1] == 1 && scales[2] == 8 && scales[3] == 1,
        "lod_scales should clamp zero scales to one");

    const std::vector<std::size_t> computed = plot::detail::compute_lod_scales(source);
    TEST_ASSERT(scales == computed,
        "lod_scales should preserve compute_lod_scales behavior");

    return true;
}

} // namespace

int main()
{
    std::cout << "Data_source query API tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_query_v_range_without_access_is_unsupported);
    RUN_TEST(test_ready_value_range_scan_populates_sequence);
    RUN_TEST(test_ascending_value_range_scans_only_selected_time_window);
    RUN_TEST(test_ascending_skip_hold_value_range_scans_bounded_prefix);
    RUN_TEST(test_unordered_value_range_aggregates_discontiguous_matches);
    RUN_TEST(test_empty_status_for_empty_snapshot_and_no_matches);
    RUN_TEST(test_busy_and_failed_snapshot_status_map_through_queries);
    RUN_TEST(test_nonfinite_values_are_skipped_or_zeroed_by_policy);
    RUN_TEST(test_query_time_window_returns_simple_ascending_window);
    RUN_TEST(test_query_time_window_handles_descending_inclusive_bounds);
    RUN_TEST(test_query_time_window_hold_forward_includes_held_sample);
    RUN_TEST(test_query_time_window_keeps_break_segment_gaps_in_window);
    RUN_TEST(test_query_time_window_reject_window_fails_on_nonfinite_in_window_sample);
    RUN_TEST(test_query_time_window_does_not_hold_nonfinite_break_segment_sample);
    RUN_TEST(test_query_time_window_skip_holds_latest_drawable_sample);
    RUN_TEST(test_query_time_window_reject_window_fails_on_nonfinite_held_sample);
    RUN_TEST(test_step_after_draw_nothing_includes_visible_held_sample);
    RUN_TEST(test_hold_forward_value_range_includes_pre_window_sample);
    RUN_TEST(test_hold_forward_value_range_ready_from_held_sample_only);
    RUN_TEST(test_hold_forward_does_not_use_nonfinite_break_segment_sample);
    RUN_TEST(test_hold_forward_skip_uses_latest_drawable_pre_window_sample);
    RUN_TEST(test_hold_forward_reject_window_fails_on_nonfinite_held_candidate);
    RUN_TEST(test_lod_scales_match_compute_lod_scales_and_clamp_minimum);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
