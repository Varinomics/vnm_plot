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

    void set_status(plot::snapshot_result_t::Snapshot_status status) { m_status = status; }
    void set_sequence(std::uint64_t sequence) { m_sequence = sequence; }
    void set_scales(std::vector<std::size_t> scales) { m_scales = std::move(scales); }

private:
    std::vector<sample_t>      m_samples;
    std::vector<std::size_t>   m_scales = {1};
    plot::snapshot_result_t::Snapshot_status m_status =
        plot::snapshot_result_t::Snapshot_status::READY;
    std::uint64_t              m_sequence       = 11;
};

plot::Data_access_policy make_value_access()
{
    plot::Data_access_policy access;
    access.get_timestamp = [](const void* sample) -> std::int64_t {
        return static_cast<const sample_t*>(sample)->t;
    };
    access.get_value = [](const void* sample) {
        return static_cast<const sample_t*>(sample)->v;
    };
    access.layout_key = 17;
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

bool test_unordered_value_range_aggregates_discontiguous_matches()
{
    Query_source source(
        {
            { 0,   1.0f   },
            { 100, 100.0f },
            { 5,   2.0f   },
            });

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

bool test_step_after_draw_nothing_includes_visible_held_sample()
{
    const auto access = make_value_access();
    for (const bool descending : {false, true})
    {
        std::vector<sample_t> samples = {{0, 5.0f}, {10, 1.0f}};
        if (descending) {
            std::reverse(samples.begin(), samples.end());
        }
        Query_source source(std::move(samples));
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
    RUN_TEST(test_unordered_value_range_aggregates_discontiguous_matches);
    RUN_TEST(test_empty_status_for_empty_snapshot_and_no_matches);
    RUN_TEST(test_busy_and_failed_snapshot_status_map_through_queries);
    RUN_TEST(test_nonfinite_values_are_skipped_or_zeroed_by_policy);
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
