// vnm_plot public QRhi layer API tests

#include "test_macros.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/core/series_builder.h>
#include <vnm_plot/core/series_window.h>
#include <vnm_plot/core/types.h>
#include <vnm_plot/rhi/font_renderer.h>
#include <vnm_plot/rhi/qrhi_series_layer.h>
#include <vnm_plot/rhi/series_builder.h>
#include <vnm_plot/rhi/series_data.h>

#include <glm/mat4x4.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace plot = vnm::plot;

namespace {

bool test_font_cache_configuration_is_available_without_text()
{
    const auto previous = plot::font_disk_cache_options();
    plot::font_disk_cache_options_t requested;
    requested.directory = std::filesystem::temp_directory_path() / "plot-profile" / "font-cache";
    requested.max_bytes = 32u * 1024u * 1024u;
    plot::set_font_disk_cache_options(requested);
    const auto observed = plot::font_disk_cache_options();
    plot::set_font_disk_cache_options(previous);
    TEST_ASSERT(observed.directory == requested.directory,
        "font cache configuration must remain callable with text enabled or disabled");
    TEST_ASSERT(observed.max_bytes == requested.max_bytes,
        "font cache configuration must preserve the host's byte budget");
    return true;
}

template<typename T, typename = void>
struct has_bind_internal_access : std::false_type {};

template<typename T>
struct has_bind_internal_access<
    T,
    std::void_t<decltype(&T::bind_internal_access)>> : std::true_type {};

using erased_timestamp_accessor_t =
    decltype(std::declval<plot::Data_access_policy&>().get_timestamp);
using erased_value_accessor_t     =
    decltype(std::declval<plot::Data_access_policy&>().get_value);
using erased_range_accessor_t     =
    decltype(std::declval<plot::Data_access_policy&>().get_range);

static_assert(std::is_assignable_v<
    erased_timestamp_accessor_t&,
    std::function<std::int64_t(const void*)>>);
static_assert(std::is_assignable_v<
    erased_value_accessor_t&,
    std::function<float(const void*)>>);
static_assert(std::is_assignable_v<
    erased_range_accessor_t&,
    std::function<std::pair<float, float>(const void*)>>);
static_assert(std::is_same_v<
    decltype(std::declval<const erased_timestamp_accessor_t&>()(
        static_cast<const void*>(nullptr))),
    std::int64_t>);
static_assert(std::is_same_v<
    decltype(std::declval<const erased_value_accessor_t&>()(
        static_cast<const void*>(nullptr))),
    float>);
static_assert(std::is_same_v<
    decltype(std::declval<const erased_range_accessor_t&>()(
        static_cast<const void*>(nullptr))),
    std::pair<float, float>>);
static_assert(!has_bind_internal_access<erased_timestamp_accessor_t>::value);
static_assert(!has_bind_internal_access<erased_value_accessor_t>::value);
static_assert(!has_bind_internal_access<erased_range_accessor_t>::value);
static_assert(std::is_same_v<decltype(plot::Data_access_policy::layout_key), std::uint64_t>);

static_assert(std::is_same_v<
    decltype(std::declval<plot::Series_builder&>().enabled(true)),
    plot::Series_builder&>);
static_assert(std::is_same_v<
    decltype(std::declval<plot::Series_builder&>().build_value()),
    plot::series_data_t>);
static_assert(std::is_same_v<
    decltype(std::declval<plot::Rhi_series_builder&>().enabled(true)),
    plot::Rhi_series_builder&>);
static_assert(std::is_same_v<
    decltype(std::declval<plot::Rhi_series_builder&>().nonfinite_policy(
        plot::Nonfinite_sample_policy::SKIP)),
    plot::Rhi_series_builder&>);
static_assert(std::is_same_v<
    decltype(std::declval<plot::Rhi_series_builder&>().build_value()),
    plot::rhi_series_data_t>);

static_assert(static_cast<int>(plot::Display_style::NONE)           == 0x0);
static_assert(static_cast<int>(plot::Display_style::DOTS)           == 0x1);
static_assert(static_cast<int>(plot::Display_style::LINE)           == 0x2);
static_assert(static_cast<int>(plot::Display_style::AREA)           == 0x4);
static_assert(static_cast<int>(plot::Display_style::DOTS_LINE_AREA) == 0x7);

static_assert(std::is_default_constructible_v<plot::qrhi_series_sample_buffer_layout_t>);
static_assert(std::is_default_constructible_v<plot::qrhi_series_sample_buffer_t>);
static_assert(std::is_default_constructible_v<plot::sample_window_t>);
static_assert(std::is_default_constructible_v<plot::value_range_plan_t>);
static_assert(std::is_default_constructible_v<plot::Planned_snapshot>);
static_assert(std::is_default_constructible_v<plot::drawable_sample_span_t>);
static_assert(std::is_default_constructible_v<plot::Series_view_plan>);
static_assert(std::is_default_constructible_v<plot::Frame_range_plan>);
static_assert(std::is_same_v<
    decltype(std::declval<plot::Series_view_plan&>().drawable_spans),
    std::vector<plot::drawable_sample_span_t>>);

class Test_layer_state final : public plot::Qrhi_series_layer_state
{
public:
    bool prepare(const plot::qrhi_series_prepare_context_t& ctx) override
    {
        last_prepare_context = ctx;
        return true;
    }

    void record(const plot::qrhi_series_record_context_t& ctx) override
    {
        last_record_context = ctx;
    }

    plot::qrhi_series_prepare_context_t    last_prepare_context;
    plot::qrhi_series_record_context_t     last_record_context;
};

class Test_layer final : public plot::Qrhi_series_layer
{
public:
    Test_layer(std::string id, std::uint64_t revision, int z_order)
    :
        m_id(std::move(id)),
        m_revision(revision),
        m_z_order(z_order)
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
        return std::make_unique<Test_layer_state>();
    }

private:
    std::string    m_id;
    std::uint64_t  m_revision = 0;
    int            m_z_order  = 0;
};

bool nearly_equal(float a, float b)
{
    return std::fabs(a - b) < 1.0e-6f;
}

bool test_series_builder_qrhi_layers_append_replace_clear()
{
    auto layer_a = std::make_shared<Test_layer>("main-a", 7, -3);
    auto layer_b = std::make_shared<Test_layer>("main-b", 8,  4);
    auto layer_c = std::make_shared<Test_layer>("main-c", 9, 11);

    plot::Rhi_series_builder builder;
    auto series = builder
        .enabled(false)
        .style(plot::Display_style::DOTS_LINE_AREA)
        .series_label("rhi-builder")
        .nonfinite_policy(plot::Nonfinite_sample_policy::SKIP)
        .qrhi_layer(layer_a)
        .qrhi_layer(layer_b)
        .build_value();

    TEST_ASSERT(!series.enabled, "Rhi_series_builder enabled mismatch");
    TEST_ASSERT(series.style == plot::Display_style::DOTS_LINE_AREA,
        "Rhi_series_builder style mismatch");
    TEST_ASSERT(series.series_label == "rhi-builder",
        "Rhi_series_builder series_label mismatch");
    TEST_ASSERT(series.nonfinite_policy == plot::Nonfinite_sample_policy::SKIP,
        "Rhi_series_builder nonfinite_policy mismatch");
    TEST_ASSERT(series.qrhi_layers.size() == 2,   "qrhi_layer() must append");
    TEST_ASSERT(series.qrhi_layers[0] == layer_a, "first appended layer pointer mismatch");
    TEST_ASSERT(series.qrhi_layers[1] == layer_b, "second appended layer pointer mismatch");

    series = builder
        .qrhi_layers({layer_c})
        .build_value();

    TEST_ASSERT(series.qrhi_layers.size() == 1,   "qrhi_layers() must replace existing layers");
    TEST_ASSERT(series.qrhi_layers[0] == layer_c, "replacement layer pointer mismatch");

    series = builder
        .clear_qrhi_layers()
        .build_value();

    TEST_ASSERT(series.qrhi_layers.empty(), "clear_qrhi_layers() must remove all layers");
    return true;
}

bool test_series_data_copy_preserves_layer_shared_pointers()
{
    auto layer_a = std::make_shared<Test_layer>("copy-a", 1, 0);
    auto layer_b = std::make_shared<Test_layer>("copy-b", 2, 1);

    plot::rhi_series_data_t original;
    original.qrhi_layers.push_back(layer_a);
    original.qrhi_layers.push_back(layer_b);

    const plot::rhi_series_data_t copied = original;
    TEST_ASSERT(copied.qrhi_layers.size() == 2, "series_data_t copy must preserve layer count");
    TEST_ASSERT(copied.qrhi_layers[0] == original.qrhi_layers[0],
        "series_data_t copy must preserve first shared layer pointer");
    TEST_ASSERT(copied.qrhi_layers[1] == original.qrhi_layers[1],
        "series_data_t copy must preserve second shared layer pointer");
    TEST_ASSERT(layer_a.use_count() == 3, "copied series must retain first layer");
    TEST_ASSERT(layer_b.use_count() == 3, "copied series must retain second layer");

    const auto shared_series = plot::Rhi_series_builder()
        .qrhi_layers({layer_a, layer_b})
        .build_shared();

    TEST_ASSERT(shared_series->qrhi_layers.size() == 2,
        "build_shared() must preserve QRhi layer vector");
    TEST_ASSERT(shared_series->qrhi_layers[0] == layer_a,
        "build_shared() first layer pointer mismatch");
    TEST_ASSERT(shared_series->qrhi_layers[1] == layer_b,
        "build_shared() second layer pointer mismatch");

    return true;
}

bool test_qrhi_layer_api_surface_can_be_implemented()
{
    plot::qrhi_series_prepare_context_t prepare_context;
    plot::qrhi_series_record_context_t record_context;
    TEST_ASSERT(prepare_context.rhi == nullptr, "prepare context rhi default mismatch");
    TEST_ASSERT(prepare_context.render_target == nullptr,
        "prepare context render target default mismatch");
    TEST_ASSERT(prepare_context.updates == nullptr, "prepare context update batch default mismatch");
    TEST_ASSERT(prepare_context.asset_loader == nullptr,
        "prepare context asset loader default mismatch");
    TEST_ASSERT(prepare_context.frame == nullptr,  "prepare context frame default mismatch");
    TEST_ASSERT(prepare_context.series == nullptr, "prepare context series default mismatch");
    TEST_ASSERT(prepare_context.view_uniform == nullptr,
        "prepare context view uniform default mismatch");
    TEST_ASSERT(prepare_context.view_ubo == nullptr, "prepare context UBO default mismatch");
    TEST_ASSERT(prepare_context.sample_buffer.buffer == nullptr,
        "prepare context sample buffer default mismatch");
    TEST_ASSERT(prepare_context.sample_buffer.sample_count == 0,
        "prepare context sample count default mismatch");
    TEST_ASSERT(prepare_context.sample_buffer.layout.stride_bytes == 0,
        "prepare context sample layout default mismatch");
    TEST_ASSERT(!prepare_context.resources_changed,
        "prepare context resources_changed default mismatch");

    TEST_ASSERT(record_context.cb == nullptr, "record context command buffer default mismatch");
    TEST_ASSERT(record_context.render_target == nullptr,
        "record context render target default mismatch");
    TEST_ASSERT(record_context.frame == nullptr,    "record context frame default mismatch");
    TEST_ASSERT(record_context.series == nullptr,   "record context series default mismatch");
    TEST_ASSERT(record_context.view_ubo == nullptr, "record context UBO default mismatch");

    return true;
}

bool test_core_plan_types_are_usable()
{
    plot::value_range_plan_t range;
    TEST_ASSERT(!range.valid, "value_range_plan_t should default to invalid");
    range.min   = -4.0f;
    range.max   = 11.0f;
    range.valid = true;

    plot::Planned_snapshot planned_snapshot;
    planned_snapshot.sequence = 17;
    planned_snapshot.snapshot.sequence = planned_snapshot.sequence;

    plot::drawable_sample_span_t span;
    span.source_first = 2;
    span.source_count = 5;
    span.gpu_first    = 0;
    span.gpu_count    = span.source_count;

    plot::Series_view_plan view_plan;
    view_plan.series_id            = 9;
    view_plan.view_kind            = plot::Series_view_kind::PREVIEW;
    view_plan.lod_level            = 2;
    view_plan.lod_scale            = 4;
    view_plan.snapshot             = planned_snapshot;
    view_plan.source_first         = span.source_first;
    view_plan.source_count         = span.source_count;
    view_plan.synthetic_hold_count = 1;
    view_plan.gpu_count =
        view_plan.source_count + view_plan.synthetic_hold_count;
    view_plan.drawable_spans.push_back(span);
    view_plan.t_min_ns              = 1'000;
    view_plan.t_max_ns              = 5'000;
    view_plan.t_origin_ns           = 1'000;
    view_plan.hold_last_forward     = true;
    view_plan.hold_timestamp_ns     = view_plan.t_max_ns;
    view_plan.interpolation         = plot::Series_interpolation::STEP_AFTER;
    view_plan.empty_window_behavior = plot::Empty_window_behavior::HOLD_LAST_FORWARD;
    view_plan.style                 = plot::Display_style::LINE;
    view_plan.v_min                 = range.min;
    view_plan.v_max                 = range.max;
    view_plan.width_px              = 320.0f;
    view_plan.height_px             = 64.0f;
    view_plan.y_offset_px           = 12.0f;
    view_plan.window_alpha          = 0.75f;
    view_plan.pixels_per_sample     = 2.5;

    plot::Frame_range_plan frame_plan;
    frame_plan.main_v_range    = range;
    frame_plan.preview_v_range = range;

    plot::sample_window_t window;
    TEST_ASSERT(window.view_kind == plot::Series_view_kind::MAIN,
        "sample_window_t availability/default mismatch");
    TEST_ASSERT(window.source_first == 0 && window.source_count == 0,
        "sample_window_t source metadata defaults mismatch");
    TEST_ASSERT(window.synthetic_hold_count == 0 && window.gpu_count == 0,
        "sample_window_t GPU metadata defaults mismatch");

    return true;
}

bool test_make_series_view_uniform_values()
{
    plot::frame_layout_result_t layout;
    plot::frame_context_t frame{layout};
    frame.win_h = 300;

    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            frame.pmv[column][row] = static_cast<float>(column * 4 + row + 1);
        }
    }

    plot::series_data_t series;
    series.color = glm::vec4(0.2f, 0.4f, 0.6f, 0.8f);

    plot::sample_window_t window;
    window.t_origin_ns  = 1'000'000'000LL;
    window.t_min_ns     = 2'500'000'000LL;
    window.t_max_ns     = 5'000'000'000LL;
    window.v_min        = -3.0f;
    window.v_max        = 9.0f;
    window.width_px     = 640.0f;
    window.height_px    = 120.0f;
    window.y_offset_px  = 24.0f;
    window.window_alpha = 0.5f;

    const plot::series_view_uniform_std140_t uniform =
        plot::make_series_view_uniform(frame, series, window);

    TEST_ASSERT(nearly_equal(uniform.pmv[0],  1.0f),  "uniform pmv[0] mismatch");
    TEST_ASSERT(nearly_equal(uniform.pmv[5],  6.0f),  "uniform pmv[5] mismatch");
    TEST_ASSERT(nearly_equal(uniform.pmv[15], 16.0f), "uniform pmv[15] mismatch");

    TEST_ASSERT(nearly_equal(uniform.color[0], 0.2f), "uniform color red mismatch");
    TEST_ASSERT(nearly_equal(uniform.color[1], 0.4f), "uniform color green mismatch");
    TEST_ASSERT(nearly_equal(uniform.color[2], 0.6f), "uniform color blue mismatch");
    TEST_ASSERT(nearly_equal(uniform.color[3], 0.4f),
        "uniform color alpha must include window alpha");

    TEST_ASSERT(nearly_equal(uniform.t_min, 1.5f),     "uniform t_min relative seconds mismatch");
    TEST_ASSERT(nearly_equal(uniform.t_max, 4.0f),     "uniform t_max relative seconds mismatch");
    TEST_ASSERT(nearly_equal(uniform.v_min, -3.0f),    "uniform v_min mismatch");
    TEST_ASSERT(nearly_equal(uniform.v_max,  9.0f),    "uniform v_max mismatch");
    TEST_ASSERT(nearly_equal(uniform.width, 640.0f),   "uniform width mismatch");
    TEST_ASSERT(nearly_equal(uniform.height, 120.0f),  "uniform height mismatch");
    TEST_ASSERT(nearly_equal(uniform.y_offset, 24.0f), "uniform y_offset mismatch");
    TEST_ASSERT(nearly_equal(uniform.win_h, 300.0f),   "uniform win_h mismatch");
    TEST_ASSERT(uniform.framebuffer_y_up == 0,
        "null frame.rhi must produce framebuffer_y_up == 0");

    return true;
}

} // namespace

int main()
{
    std::cout << "QRhi public API tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_series_builder_qrhi_layers_append_replace_clear);
    RUN_TEST(test_series_data_copy_preserves_layer_shared_pointers);
    RUN_TEST(test_qrhi_layer_api_surface_can_be_implemented);
    RUN_TEST(test_core_plan_types_are_usable);
    RUN_TEST(test_make_series_view_uniform_values);
    RUN_TEST(test_font_cache_configuration_is_available_without_text);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
