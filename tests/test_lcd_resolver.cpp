#include "test_macros.h"
#include <vnm_msdf_text/qt/lcd_resolver.h>
#include "plot_renderer.h"
#include <vnm_plot/qt/plot_widget.h>

#include <QGuiApplication>
#include <QQuickWindow>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QImage>
#include <QAbstractEventDispatcher>
#include <QScreen>

#if defined(_WIN32)
#include <qt_windows.h>
#include <QtGui/qscreen_platform.h>
#endif

#include <qpa/qplatformscreen.h>

#include <iostream>

namespace plot = vnm::plot;

namespace {

using request_t  = plot::lcd_request_t;
using resolved_t = plot::lcd_subpixel_order_t;

class Test_lcd_renderer : public plot::Plot_renderer
{
public:
    using plot::Plot_renderer::synchronize;
};

#if defined(VNM_MSDF_TEXT_ENABLE_TEST_HOOKS)
bool test_display_probe_is_cached_across_synchronizations()
{
    QQuickRenderControl control;
    QQuickWindow window(&control);
    QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
    auto target = QQuickRenderTarget::fromPaintDevice(&image);
    target.setDevicePixelRatio(window.devicePixelRatio());
    window.setRenderTarget(target);
    plot::Plot_widget widget;
    widget.setParentItem(window.contentItem());
    Test_lcd_renderer renderer;
    const auto initial_count = vnm::msdf_text::lcd::lcd_platform_probe_count_for_test();
    renderer.synchronize(&widget);
    renderer.synchronize(&widget);
    renderer.synchronize(&widget);
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() == initial_count,
        "render synchronization must reuse the GUI display-context result");

    auto config = widget.config();
    config.dark_mode = !config.dark_mode;
    widget.set_config(config);
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() == initial_count,
        "unrelated configuration changes must not probe the display");
    config.lcd_request = plot::lcd_explicit_request(resolved_t::BGR);
    widget.set_config(config);
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() == initial_count,
        "explicit LCD requests must not probe the display");
    config.lcd_request = plot::lcd_auto_request();
    widget.set_config(config);
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() == initial_count + 1,
        "returning to automatic must resolve the current display");

    const auto before_change = vnm::msdf_text::lcd::lcd_platform_probe_count_for_test();
    target.setDevicePixelRatio(window.devicePixelRatio() * 2.0);
    window.setRenderTarget(target);
    QEvent changed(QEvent::DevicePixelRatioChange);
    QCoreApplication::sendEvent(&window, &changed);
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() > before_change,
        "a display-context notification must refresh automatic detection");
    const auto before_screen = vnm::msdf_text::lcd::lcd_platform_probe_count_for_test();
    window.screen()->geometryChanged(window.screen()->geometry());
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() > before_screen,
        "screen geometry notifications must refresh automatic detection");
    QQuickWindow second_window;
    const auto before_window = vnm::msdf_text::lcd::lcd_platform_probe_count_for_test();
    widget.setParentItem(second_window.contentItem());
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() > before_window,
        "moving to another window must refresh automatic detection");
#if defined(_WIN32)
    MSG message{};
    message.message = WM_SETTINGCHANGE;
    qintptr result = 0;
    const auto before_settings = vnm::msdf_text::lcd::lcd_platform_probe_count_for_test();
    QAbstractEventDispatcher::instance()->filterNativeEvent("windows_generic_MSG", &message, &result);
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() > before_settings,
        "Windows settings notifications must refresh automatic detection");
    config.lcd_request = plot::lcd_explicit_request(resolved_t::VRGB);
    widget.set_config(config);
    const auto explicit_count = vnm::msdf_text::lcd::lcd_platform_probe_count_for_test();
    message.message = WM_DISPLAYCHANGE;
    QAbstractEventDispatcher::instance()->filterNativeEvent("windows_generic_MSG", &message, &result);
    renderer.synchronize(&widget);
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_platform_probe_count_for_test() == explicit_count,
        "display notifications and synchronization must preserve explicit requests without probing");
#endif
    return true;
}

#endif

bool test_windows_display_identity_and_rotation()
{
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_windows_registry_key(QStringLiteral("\\\\.\\DISPLAY1")) ==
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Avalon.Graphics\\DISPLAY1"),
        "monitor one must use its own PixelStructure registry key");
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_windows_registry_key(QStringLiteral("\\\\.\\DISPLAY12")) ==
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Avalon.Graphics\\DISPLAY12"),
        "monitor twelve must not inherit monitor one's PixelStructure");
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_windows_registry_key(QStringLiteral("unknown")).isEmpty(),
        "unknown display identifiers must fail closed");
    // Microsoft DEVMODE defines counter-clockwise rotation; the shared LCD
    // contract defines vertical orders from top to bottom.
    const resolved_t rgb[] = {resolved_t::RGB, resolved_t::VBGR, resolved_t::BGR, resolved_t::VRGB};
    const resolved_t bgr[] = {resolved_t::BGR, resolved_t::VRGB, resolved_t::RGB, resolved_t::VBGR};
    for (unsigned int rotation = 0; rotation != 4; ++rotation) {
        TEST_ASSERT(vnm::msdf_text::lcd::lcd_from_windows_display_settings(1U, rotation, resolved_t::NONE) == rgb[rotation],
            "physical RGB stripes must follow counter-clockwise display rotation");
        TEST_ASSERT(vnm::msdf_text::lcd::lcd_from_windows_display_settings(2U, rotation, resolved_t::NONE) == bgr[rotation],
            "physical BGR stripes must follow counter-clockwise display rotation");
        TEST_ASSERT(vnm::msdf_text::lcd::lcd_from_windows_display_settings(0U, rotation, resolved_t::RGB) == resolved_t::NONE,
            "Flat is an explicit grayscale setting, not missing detection");
        TEST_ASSERT(vnm::msdf_text::lcd::lcd_from_windows_display_settings(std::nullopt, rotation, resolved_t::RGB) == rgb[rotation],
            "missing PixelStructure may use the system fallback with current rotation");
    }
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_from_windows_display_settings(42U, 0U, resolved_t::RGB) == resolved_t::NONE,
        "unknown PixelStructure must fail closed");
    TEST_ASSERT(vnm::msdf_text::lcd::lcd_from_windows_display_settings(1U, 42U, resolved_t::RGB) == resolved_t::NONE,
        "unknown rotation must fail closed");
    return true;
}

bool test_native_screen_device_identity()
{
#if defined(_WIN32)
    for (auto* screen : QGuiApplication::screens()) {
        const auto* native_screen = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (!native_screen) {
            continue;
        }
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        TEST_ASSERT(GetMonitorInfoW(native_screen->handle(), &info), "native screen must identify its display device");
        const auto expected = QString::fromWCharArray(info.szDevice);
        const auto actual = vnm::msdf_text::lcd::lcd_windows_device_name(screen);
        std::cout << "screen='" << screen->name().toStdString() << "' device='"
                  << actual.toStdString() << "' native='" << expected.toStdString() << "'\n";
        TEST_ASSERT(actual == expected, "resolver must use the native GDI device identity, not the friendly monitor label");
        TEST_ASSERT(!vnm::msdf_text::lcd::lcd_windows_registry_key(actual).isEmpty(), "native display identity must select a registry key");
    }
#endif
    return true;
}

struct explicit_request_case_t
{
    request_t  request;
    resolved_t expected;
};

constexpr unsigned int k_win_font_smoothing_standard          = 0x0001U;
constexpr unsigned int k_win_font_smoothing_cleartype         = 0x0002U;
constexpr unsigned int k_win_font_smoothing_orientation_bgr   = 0x0000U;
constexpr unsigned int k_win_font_smoothing_orientation_rgb   = 0x0001U;
constexpr unsigned int k_win_font_smoothing_orientation_other = 0x002AU;

bool test_default_null_probes_return_none()
{
    const vnm::msdf_text::lcd::lcd_resolver_probes_t probes;
    TEST_ASSERT(
        vnm::msdf_text::lcd::resolve_lcd_subpixel_order_from_probes(
            plot::lcd_auto_request(),
            probes) == resolved_t::NONE,
        "AUTO with default probes should fail closed to NONE");
    return true;
}

bool test_auto_prefers_qt_probe_and_skips_windows()
{
    bool windows_called = false;

    vnm::msdf_text::lcd::lcd_resolver_probes_t probes;
    probes.qt_probe = [] {
        return resolved_t::RGB;
    };
    probes.windows_probe = [&windows_called] {
        windows_called = true;
        return resolved_t::BGR;
    };

    TEST_ASSERT(
        vnm::msdf_text::lcd::resolve_lcd_subpixel_order_from_probes(
            plot::lcd_auto_request(),
            probes) == resolved_t::RGB,
        "Qt display-specific probe should win AUTO resolution");
    TEST_ASSERT(!windows_called, "Windows fallback should not run after Qt RGB");
    return true;
}

bool test_auto_falls_back_to_windows()
{
    vnm::msdf_text::lcd::lcd_resolver_probes_t probes;
    probes.qt_probe = [] {
        return resolved_t::NONE;
    };
    probes.windows_probe = [] {
        return resolved_t::BGR;
    };

    TEST_ASSERT(
        vnm::msdf_text::lcd::resolve_lcd_subpixel_order_from_probes(
            plot::lcd_auto_request(),
            probes) == resolved_t::BGR,
        "Windows display-specific probe should be used when Qt reports NONE");

    probes.windows_probe = [] {
        return resolved_t::NONE;
    };
    TEST_ASSERT(
        vnm::msdf_text::lcd::resolve_lcd_subpixel_order_from_probes(
            plot::lcd_auto_request(),
            probes) == resolved_t::NONE,
        "AUTO should fail closed when both probes report NONE");
    return true;
}

bool test_auto_invalid_qt_result_falls_back_to_windows()
{
    vnm::msdf_text::lcd::lcd_resolver_probes_t probes;
    probes.qt_probe = [] {
        return static_cast<resolved_t>(255);
    };
    probes.windows_probe = [] {
        return resolved_t::BGR;
    };

    TEST_ASSERT(
        vnm::msdf_text::lcd::resolve_lcd_subpixel_order_from_probes(
            plot::lcd_auto_request(),
            probes) == resolved_t::BGR,
        "invalid Qt probe result should fall back to valid Windows result");
    return true;
}

bool test_explicit_orders_skip_probes()
{
    int probe_calls = 0;
    vnm::msdf_text::lcd::lcd_resolver_probes_t probes;
    probes.qt_probe = [&probe_calls] {
        ++probe_calls;
        return resolved_t::RGB;
    };
    probes.windows_probe = [&probe_calls] {
        ++probe_calls;
        return resolved_t::BGR;
    };

    const explicit_request_case_t explicit_orders[] = {
        { plot::lcd_none_request(), resolved_t::NONE },
        { plot::lcd_explicit_request(resolved_t::RGB), resolved_t::RGB },
        { plot::lcd_explicit_request(resolved_t::BGR), resolved_t::BGR },
        { plot::lcd_explicit_request(resolved_t::VRGB), resolved_t::VRGB },
        { plot::lcd_explicit_request(resolved_t::VBGR), resolved_t::VBGR },
    };

    for (const explicit_request_case_t& item : explicit_orders) {
        TEST_ASSERT(
            vnm::msdf_text::lcd::resolve_lcd_subpixel_order_from_probes(item.request, probes) ==
                item.expected,
            "explicit LCD order should resolve without platform probes");
    }

    TEST_ASSERT(probe_calls == 0, "explicit LCD requests should not call any probe");
    return true;
}

bool test_explicit_orders_for_null_window_are_deterministic()
{
    const explicit_request_case_t explicit_orders[] = {
        { plot::lcd_none_request(), resolved_t::NONE },
        { plot::lcd_explicit_request(resolved_t::RGB), resolved_t::RGB },
        { plot::lcd_explicit_request(resolved_t::BGR), resolved_t::BGR },
        { plot::lcd_explicit_request(resolved_t::VRGB), resolved_t::VRGB },
        { plot::lcd_explicit_request(resolved_t::VBGR), resolved_t::VBGR },
    };

    for (const explicit_request_case_t& item : explicit_orders) {
        TEST_ASSERT(
            vnm::msdf_text::lcd::resolve_lcd_subpixel_order_for_window(item.request, nullptr) ==
                item.expected,
            "explicit LCD order should resolve for nullptr window without platform settings");
    }

    return true;
}

bool test_probe_auto_or_invalid_results_fail_closed()
{
    vnm::msdf_text::lcd::lcd_resolver_probes_t probes;
    probes.qt_probe = [] {
        return resolved_t::NONE;
    };
    probes.windows_probe = [] {
        return static_cast<resolved_t>(255);
    };
    TEST_ASSERT(
        vnm::msdf_text::lcd::resolve_lcd_subpixel_order_from_probes(
            plot::lcd_auto_request(),
            probes) == resolved_t::NONE,
        "Windows invalid probe result should fail closed");

    const request_t invalid_request{false, static_cast<resolved_t>(255)};
    int invalid_request_probe_calls = 0;
    probes.qt_probe = [&invalid_request_probe_calls] {
        ++invalid_request_probe_calls;
        return resolved_t::RGB;
    };
    probes.windows_probe = [&invalid_request_probe_calls] {
        ++invalid_request_probe_calls;
        return resolved_t::BGR;
    };
    TEST_ASSERT(
        vnm::msdf_text::lcd::resolve_lcd_subpixel_order_from_probes(
            invalid_request,
            probes) == resolved_t::NONE,
        "invalid requested order should fail closed without probing");
    TEST_ASSERT(
        invalid_request_probe_calls == 0,
        "invalid requested order should not call platform probes");
    return true;
}

bool test_qt_subpixel_hint_mapping()
{
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_qt_subpixel_hint(
            static_cast<int>(QPlatformScreen::Subpixel_RGB)) ==
            resolved_t::RGB,
        "Qt RGB hint should map to RGB");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_qt_subpixel_hint(
            static_cast<int>(QPlatformScreen::Subpixel_BGR)) ==
            resolved_t::BGR,
        "Qt BGR hint should map to BGR");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_qt_subpixel_hint(
            static_cast<int>(QPlatformScreen::Subpixel_VRGB)) ==
            resolved_t::VRGB,
        "Qt VRGB hint should map to VRGB");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_qt_subpixel_hint(
            static_cast<int>(QPlatformScreen::Subpixel_VBGR)) ==
            resolved_t::VBGR,
        "Qt VBGR hint should map to VBGR");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_qt_subpixel_hint(
            static_cast<int>(QPlatformScreen::Subpixel_None)) ==
            resolved_t::NONE,
        "Qt None hint should map to NONE");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_qt_subpixel_hint(255) ==
            resolved_t::NONE,
        "unknown Qt hint should fail closed");
    return true;
}

bool test_windows_font_smoothing_mapping()
{
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_windows_font_smoothing_settings(
            false,
            k_win_font_smoothing_cleartype,
            k_win_font_smoothing_orientation_rgb) ==
            resolved_t::NONE,
        "disabled Windows font smoothing should map to NONE");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_windows_font_smoothing_settings(
            true,
            k_win_font_smoothing_standard,
            k_win_font_smoothing_orientation_rgb) ==
            resolved_t::NONE,
        "non-ClearType smoothing should map to NONE");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_windows_font_smoothing_settings(
            true,
            k_win_font_smoothing_cleartype,
            k_win_font_smoothing_orientation_rgb) ==
            resolved_t::RGB,
        "ClearType RGB orientation should map to RGB");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_windows_font_smoothing_settings(
            true,
            k_win_font_smoothing_cleartype,
            k_win_font_smoothing_orientation_bgr) ==
            resolved_t::BGR,
        "ClearType BGR orientation should map to BGR");
    TEST_ASSERT(
        vnm::msdf_text::lcd::lcd_from_windows_font_smoothing_settings(
            true,
            k_win_font_smoothing_cleartype,
            k_win_font_smoothing_orientation_other) ==
            resolved_t::NONE,
        "unknown ClearType orientation should fail closed");
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    std::cout << "LCD resolver tests" << std::endl;

    int passed = 0;
    int failed = 0;

#if defined(VNM_MSDF_TEXT_ENABLE_TEST_HOOKS)
    RUN_TEST(test_display_probe_is_cached_across_synchronizations);
#endif
    RUN_TEST(test_windows_display_identity_and_rotation);
    RUN_TEST(test_native_screen_device_identity);
    RUN_TEST(test_default_null_probes_return_none);
    RUN_TEST(test_auto_prefers_qt_probe_and_skips_windows);
    RUN_TEST(test_auto_falls_back_to_windows);
    RUN_TEST(test_auto_invalid_qt_result_falls_back_to_windows);
    RUN_TEST(test_explicit_orders_skip_probes);
    RUN_TEST(test_explicit_orders_for_null_window_are_deterministic);
    RUN_TEST(test_probe_auto_or_invalid_results_fail_closed);
    RUN_TEST(test_qt_subpixel_hint_mapping);
    RUN_TEST(test_windows_font_smoothing_mapping);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
