// QRhi-independent regressions for visible-window planning.

#include "test_macros.h"
#include "../src/core/series_window_planner.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace plot = vnm::plot;
namespace detail = plot::detail;

namespace {

struct sample_t
{
    std::int64_t t;
    float value;
};

// A source whose revision can be withheld, as a source with no content
// revision does: current_sequence() and the snapshot sequence are both 0.
class Test_source final : public plot::Vector_data_source<sample_t>
{
public:
    using plot::Vector_data_source<sample_t>::Vector_data_source;

    plot::snapshot_result_t try_snapshot(std::size_t lod = 0) override
    {
        auto result = plot::Vector_data_source<sample_t>::try_snapshot(lod);
        if (!revision_available) {
            result.snapshot.sequence = 0;
        }
        return result;
    }

    std::uint64_t current_sequence(std::size_t lod = 0) const override
    {
        return revision_available
            ? plot::Vector_data_source<sample_t>::current_sequence(lod)
            : 0;
    }

    bool revision_available = true;
};

struct Planner_fixture
{
    Planner_fixture(plot::Data_source& source)
    {
        access.get_timestamp = [](const void* sample) {
            return static_cast<const sample_t*>(sample)->t;
        };
        access.get_value = [](const void* sample) {
            return static_cast<const sample_t*>(sample)->value;
        };
        request.planner_state = &state;
        request.snapshot_cache = &cache;
        request.data_source = &source;
        request.access = &access;
        request.scales = &scales;
        request.t_min_ns = 4;
        request.t_max_ns = 6;
        request.width_px = 600;
        request.style = plot::Display_style::LINE;
    }

    plot::Series_view_plan next_frame()
    {
        ++request.frame_id;
        return detail::plan_series_window(request);
    }

    plot::Data_access_policy access;
    std::vector<std::size_t> scales{1};
    detail::series_window_planner_state_t state;
    detail::Series_window_snapshot_cache cache;
    detail::series_window_plan_request_t request;
};

bool test_unknown_revision_rescans_timestamp_order()
{
    Test_source source(std::vector<sample_t>{{0, 1}, {10, 2}, {20, 3}, {30, 4}, {40, 5}});
    source.revision_available = false;
    Planner_fixture fixture(source);
    fixture.request.t_min_ns = 25;
    fixture.request.t_max_ns = 35;
    fixture.next_frame();
    TEST_ASSERT(fixture.state.last_timestamp_order_scan_performed &&
        fixture.state.last_timestamps_monotonic,
        "the initial ascending snapshot should be scanned");

    source.set_data({{0, 1}, {100, 2}, {200, 3}, {300, 4}, {20, 5}, {30, 6}, {40, 7}});
    const auto changed = fixture.next_frame();
    TEST_ASSERT(fixture.state.last_timestamp_order_scan_performed &&
        !fixture.state.last_timestamps_monotonic &&
        fixture.state.last_timestamp_window_search == detail::Timestamp_window_search::LINEAR,
        "an unversioned reordered snapshot requires a fresh order scan and linear selection");
    bool includes_visible_sample = false;
    for (std::size_t index = changed.source_first;
         index < changed.source_first + changed.source_count; ++index) {
        const auto* sample = static_cast<const sample_t*>(changed.snapshot.snapshot.at(index));
        includes_visible_sample = includes_visible_sample || (sample && sample->t == 30);
    }
    TEST_ASSERT(includes_visible_sample,
        "reordering unversioned data must not hide the sample inside the requested window");

    source.set_data({{0, 1}, {10, 2}, {20, 3}, {30, 4}, {40, 5}});
    fixture.next_frame();
    TEST_ASSERT(fixture.state.last_timestamp_order_scan_performed &&
        fixture.state.last_timestamps_monotonic,
        "restoring ascending order without a revision must also be detected");
    return true;
}

bool test_known_revision_reuses_timestamp_order()
{
    Test_source source(std::vector<sample_t>{{0, 1}, {10, 2}, {20, 3}, {30, 4}, {40, 5}});
    Planner_fixture fixture(source);
    fixture.next_frame();
    TEST_ASSERT(fixture.state.last_timestamp_order_scan_performed,
        "the first versioned snapshot needs an order scan");
    fixture.next_frame();
    TEST_ASSERT(!fixture.state.last_timestamp_order_scan_performed &&
        fixture.state.last_timestamps_monotonic,
        "an unchanged nonzero revision should retain the order-scan optimization");
    return true;
}

} // namespace

int main()
{
    int passed = 0;
    int failed = 0;
    RUN_TEST(test_unknown_revision_rescans_timestamp_order);
    RUN_TEST(test_known_revision_reuses_timestamp_order);
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
