#include "src/preview_controller.h"
#include "src/top_controller.h"

#include "../../tests/test_macros.h"

#include <vnm_plot/qt/plot_time_axis.h>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QVariant>

#include <cmath>
#include <memory>

namespace plot = vnm::plot;

namespace {

constexpr qint64 k_second_ns = 1'000'000'000;
constexpr qint64 k_t_min_ns = -20 * k_second_ns;
constexpr qint64 k_t_max_ns = 20 * k_second_ns;

bool has_example_time_range(const plot::Plot_widget& widget)
{
    return widget.t_min() == k_t_min_ns && widget.t_max() == k_t_max_ns &&
        widget.t_available_min() == k_t_min_ns && widget.t_available_max() == k_t_max_ns;
}

bool test_controller_lifecycle(bool preview)
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        preview ? "import Example 1.0\nPreviewController {}" :
                  "import Example 1.0\nTopController {}",
        QUrl("qrc:/controller_fixture.qml"));

    plot::Plot_time_axis first_axis;
    plot::Plot_time_axis replacement_axis;
    plot::Plot_time_axis detached_axis;
    plot::Plot_widget first;
    plot::Plot_widget replacement;
    std::unique_ptr<QObject> object(component.create());
    if (component.isError()) {
        std::cerr << component.errorString().toStdString();
    }
    auto* controller = qobject_cast<Example_plot_controller*>(object.get());
    TEST_ASSERT(controller, "the registered example QML name should create its controller");

    int notifications = 0;
    QObject::connect(controller, &Example_plot_controller::plot_widget_changed,
        controller, [&notifications]() { ++notifications; });
    TEST_ASSERT(object->setProperty("plot_widget", QVariant::fromValue(&first)),
        "the QML controller should expose its writable plot_widget property");
    TEST_ASSERT(controller->plot_widget() == &first && notifications == 1,
        "attachment should update the property and notify once");

    const int series_id = preview ? 1 : 2;
    const auto series_map = first.get_series_snapshot();
    TEST_ASSERT(series_map.size() == 1 && series_map.count(series_id) == 1,
        "each example should attach its distinct series id");
    const auto series = series_map.at(series_id);
    TEST_ASSERT(series->enabled && series->style ==
        (preview ? plot::Display_style::LINE : plot::Display_style::AREA),
        "the preview and top plots should retain their distinct display styles");
    TEST_ASSERT(series->color == (preview ? plot::rgba_u8(64, 217, 140) :
        plot::rgba_u8(230, 89, 51, 230)), "each example should retain its series color");

    const auto samples = series->data_source.get()->try_snapshot(0);
    TEST_ASSERT(samples.status == plot::snapshot_result_t::Snapshot_status::READY &&
        samples.snapshot.count == (preview ? 4000u : 1800u),
        "the two examples should retain their full-resolution sample counts");
    TEST_ASSERT(series->access.get_timestamp(samples.snapshot.at(0)) == k_t_min_ns &&
        series->access.get_timestamp(samples.snapshot.at(samples.snapshot.count - 1)) == k_t_max_ns,
        "generated sample endpoints should cover the example time range");
    const double expected_first = preview ?
        -std::sin(20.0) + 0.35 * std::cos(7.0) :
        250.0 * (2.2 * std::cos(8.0) + 0.6 * std::sin(32.0));
    TEST_ASSERT(std::abs(series->access.get_value(samples.snapshot.at(0)) - expected_first) < 0.0001,
        "the examples should retain their different signal formulas");

    TEST_ASSERT(series->preview_config.has_value() == preview,
        "only the preview example should use an alternate preview source");
    if (preview) {
        const auto& preview_config = *series->preview_config;
        const auto preview_samples = preview_config.data_source.get()->try_snapshot(0);
        TEST_ASSERT(preview_config.data_source.get() != series->data_source.get() &&
            preview_config.style == plot::Display_style::AREA &&
            preview_samples.status == plot::snapshot_result_t::Snapshot_status::READY &&
            preview_samples.snapshot.count == 320,
            "the preview should retain its separate 320-sample AREA source");
        TEST_ASSERT(std::abs(preview_config.access.get_value(preview_samples.snapshot.at(0)) -
            expected_first) < 0.0001, "the preview source should sample the same signal");
    }

    const auto config = first.config();
    TEST_ASSERT(config.dark_mode == preview && config.clear_to_transparent && config.show_text,
        "example theme and transparency settings should be preserved");
    TEST_ASSERT(config.font_size_px == (preview ? 11.0 : 10.0) &&
        config.line_width_px == (preview ? 1.35 : 1.6) &&
        config.preview_height_px == (preview ? 120.0 : 0.0) &&
        config.preview_visibility == (preview ? 0.6 : 0.0),
        "the example configurations should retain their distinct sizing and visibility");
    TEST_ASSERT(config.auto_v_range_extra_scale ==
        (preview ? 0.2 : plot::Plot_config{}.auto_v_range_extra_scale),
        "only the preview example should override automatic value-range padding");
    TEST_ASSERT(has_example_time_range(first) && first.v_auto() == preview,
        "attachment should seed the time domain and each example's value-range mode");
    if (!preview) {
        TEST_ASSERT(first.v_min() == -800.0f && first.v_max() == 800.0f,
            "the top plot should retain its manual value range");
        first.setHeight(600.0);
        TEST_ASSERT(first.preview_height_target() == 0.0,
            "the top plot should keep the automatic preview height disabled after resize");
    }

    object->setProperty("plot_widget", QVariant::fromValue(&first));
    TEST_ASSERT(notifications == 1 && first.get_series_snapshot().size() == 1,
        "assigning the same widget should leave attachment and notifications unchanged");

    first.set_time_axis(&first_axis);
    first.set_v_auto(false);
    first.set_v_range(-25.0f, 75.0f);
    replacement_axis.set_t_range(-2 * k_second_ns, 2 * k_second_ns);
    replacement_axis.set_available_t_range(-2 * k_second_ns, 2 * k_second_ns);
    first.set_time_axis(&replacement_axis);
    TEST_ASSERT(has_example_time_range(first) && !first.v_auto() &&
        first.v_min() == -25.0f && first.v_max() == 75.0f,
        "changing time axis should restore only the time domain, preserving value-range state");

    object->setProperty("plot_widget", QVariant::fromValue(&replacement));
    const auto replacement_series = replacement.get_series_snapshot();
    TEST_ASSERT(notifications == 2 && first.get_series_snapshot().empty() &&
        replacement_series.count(series_id) == 1 &&
        replacement_series.at(series_id)->main_source() == series->main_source(),
        "reattachment should remove the old series and retain its data source on the replacement");
    replacement.set_t_range(-5 * k_second_ns, 5 * k_second_ns);
    first.set_time_axis(&detached_axis);
    TEST_ASSERT(replacement.t_min() == -5 * k_second_ns &&
        replacement.t_max() == 5 * k_second_ns,
        "time-axis changes on a detached widget should not affect the replacement");

    first_axis.set_t_range(-2 * k_second_ns, 2 * k_second_ns);
    first_axis.set_available_t_range(-2 * k_second_ns, 2 * k_second_ns);
    replacement.set_time_axis(&first_axis);
    TEST_ASSERT(has_example_time_range(replacement),
        "the replacement widget should receive the active time-axis connection");

    object->setProperty("plot_widget", QVariant::fromValue<plot::Plot_widget*>(nullptr));
    TEST_ASSERT(controller->plot_widget() == nullptr && notifications == 3 &&
        replacement.get_series_snapshot().empty(),
        "detachment should clear the property, remove the series, and notify once");
    return true;
}

bool test_preview_controller()
{
    return test_controller_lifecycle(true);
}

bool test_top_controller()
{
    return test_controller_lifecycle(false);
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    plot::register_qml_types();
    qmlRegisterType<Preview_controller>("Example", 1, 0, "PreviewController");
    qmlRegisterType<Top_controller>("Example", 1, 0, "TopController");

    int passed = 0;
    int failed = 0;
    RUN_TEST(test_preview_controller);
    RUN_TEST(test_top_controller);
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
