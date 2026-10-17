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
// revision does: current_sequence() and the snapshot sequence are both 0. It can
// also report transient unavailability, report an empty snapshot as READY, and
// be cleared at the moment the planner acquires its frame snapshot.
class Test_source final : public plot::Vector_data_source<sample_t>
{
public:
    using plot::Vector_data_source<sample_t>::Vector_data_source;

    plot::snapshot_result_t try_snapshot(std::size_t lod = 0) override
    {
        if (clear_on_next_snapshot) {
            clear_on_next_snapshot = false;
            set_data({});
        }
        if (busy) {
            return {{}, plot::snapshot_result_t::Snapshot_status::BUSY};
        }
        auto result = plot::Vector_data_source<sample_t>::try_snapshot(lod);
        if (!revision_available) {
            result.snapshot.sequence = 0;
        }
        if (ready_when_empty && result.snapshot.count == 0) {
            result.status = plot::snapshot_result_t::Snapshot_status::READY;
        }
        return result;
    }

    std::uint64_t current_sequence(std::size_t lod = 0) const override
    {
        return revision_available
            ? plot::Vector_data_source<sample_t>::current_sequence(lod)
            : 0;
    }

    bool busy                   = false;
    bool revision_available     = true;
    bool ready_when_empty       = false;
    bool clear_on_next_snapshot = false;
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

bool test_empty_snapshot_invalidates_uploaded_plan()
{
    for (const bool ready_when_empty : {false, true}) {
        for (const auto requirement : {detail::Snapshot_requirement::Optional,
                                      detail::Snapshot_requirement::Frame_snapshot_required}) {
            Test_source source(std::vector<sample_t>{{0, 100.0f}, {10, 200.0f}});
            source.ready_when_empty = ready_when_empty;
            Planner_fixture fixture(source);
            fixture.request.snapshot_requirement = requirement;
            const auto initial = fixture.next_frame();
            TEST_ASSERT(initial.gpu_count == 2,
                "the initial frame should contain both bracketing samples");
            fixture.request.has_uploaded_vbo = true;

            source.set_data({});
            for (int frame = 0; frame < 2; ++frame) {
                const auto empty = fixture.next_frame();
                TEST_ASSERT(empty.gpu_count == 0 && empty.source_count == 0 &&
                    !fixture.state.last_plan_reused_upload,
                    "a cleared source must not keep drawing the pre-clear upload");
            }

            source.busy = true;
            const auto busy_after_empty = fixture.next_frame();
            TEST_ASSERT(busy_after_empty.gpu_count == 0 &&
                !fixture.state.last_plan_reused_upload,
                "a later BUSY result must not resurrect the pre-clear upload");

            source.busy = false;
            source.set_data({{0, 300.0f}, {10, 400.0f}});
            const auto restored = fixture.next_frame();
            TEST_ASSERT(restored.gpu_count == 2 && !fixture.state.last_plan_reused_upload &&
                static_cast<const sample_t*>(restored.snapshot.snapshot.at(0))->value == 300.0f,
                "new data after a clear must produce a fresh plan");
        }
    }
    return true;
}

bool test_clear_between_sequence_check_and_snapshot_is_honored()
{
    for (const bool ready_when_empty : {false, true}) {
        Test_source source(std::vector<sample_t>{{0, 100.0f}, {10, 200.0f}});
        source.ready_when_empty = ready_when_empty;
        Planner_fixture fixture(source);
        fixture.request.snapshot_requirement =
            detail::Snapshot_requirement::Frame_snapshot_required;
        TEST_ASSERT(fixture.next_frame().gpu_count == 2, "initial plan must be drawable");
        fixture.request.has_uploaded_vbo = true;

        // current_sequence() still observes the uploaded revision; the clear
        // lands when the planner acquires its required frame snapshot.
        source.clear_on_next_snapshot = true;
        const auto cleared = fixture.next_frame();
        TEST_ASSERT(cleared.gpu_count == 0 && !fixture.state.last_plan_reused_upload,
            "a clear observed in the frame snapshot must not keep the pre-clear upload");
    }
    return true;
}

bool test_busy_snapshot_keeps_compatible_uploaded_plan()
{
    for (const auto requirement : {detail::Snapshot_requirement::Optional,
                                  detail::Snapshot_requirement::Frame_snapshot_required}) {
        Test_source source(std::vector<sample_t>{{0, 100.0f}, {10, 200.0f}});
        Planner_fixture fixture(source);
        fixture.request.snapshot_requirement = requirement;
        TEST_ASSERT(fixture.next_frame().gpu_count == 2, "initial plan must be drawable");
        fixture.request.has_uploaded_vbo = true;
        source.busy = true;
        const auto busy = fixture.next_frame();
        TEST_ASSERT(busy.gpu_count == 2 && fixture.state.last_plan_reused_upload,
            "temporary snapshot unavailability should retain the compatible upload");
    }
    return true;
}

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
    RUN_TEST(test_empty_snapshot_invalidates_uploaded_plan);
    RUN_TEST(test_clear_between_sequence_check_and_snapshot_is_honored);
    RUN_TEST(test_busy_snapshot_keeps_compatible_uploaded_plan);
    RUN_TEST(test_unknown_revision_rescans_timestamp_order);
    RUN_TEST(test_known_revision_reuses_timestamp_order);
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
