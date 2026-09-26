#include "lcd_resolver.h"
#include "../core/lcd_policy.h"

#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QObject>
#include <qpa/qplatformscreen.h>

#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
#include <atomic>
#endif

#if defined(_WIN32)
#include <qt_windows.h>
#include <QtGui/qscreen_platform.h>
#endif

#include <utility>

namespace vnm::plot {

namespace {

class Lcd_settings_observer : public QObject, public QAbstractNativeEventFilter
{
public:
    explicit Lcd_settings_observer(std::function<void()> changed)
    :
        m_changed(std::move(changed))
    {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }

    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr*) override
    {
#if defined(_WIN32)
        if (event_type == "windows_generic_MSG" || event_type == "windows_dispatcher_MSG") {
            const auto* native_message = static_cast<const MSG*>(message);
            if (native_message->message == WM_SETTINGCHANGE || native_message->message == WM_DISPLAYCHANGE) {
                m_changed();
            }
        }
#else
        Q_UNUSED(event_type);
        Q_UNUSED(message);
#endif
        return false;
    }

private:
    std::function<void()> m_changed;
};

#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
std::atomic<std::size_t> g_platform_probe_count{0};
#endif

#if defined(_WIN32)
constexpr unsigned int k_win_spi_get_font_smoothing             = 0x004AU;
constexpr unsigned int k_win_spi_get_font_smoothing_type        = 0x200AU;
constexpr unsigned int k_win_spi_get_font_smoothing_orientation = 0x2012U;
#endif

constexpr unsigned int k_win_font_smoothing_cleartype       = 0x0002U;
constexpr unsigned int k_win_font_smoothing_orientation_bgr = 0x0000U;
constexpr unsigned int k_win_font_smoothing_orientation_rgb = 0x0001U;

lcd_subpixel_order_t lcd_subpixel_order_from_qt(QQuickWindow* window)
{
    if (window == nullptr) {
        return lcd_subpixel_order_t::NONE;
    }

    QPlatformScreen* const platform_screen =
        QPlatformScreen::platformScreenForWindow(window);
    if (platform_screen == nullptr) {
        return lcd_subpixel_order_t::NONE;
    }

    // The Windows override reads DISPLAY1 regardless of the actual screen.
    // Its base implementation preserves Qt's explicit environment hint.
#if defined(_WIN32)
    const auto hint = platform_screen->QPlatformScreen::subpixelAntialiasingTypeHint();
#else
    const auto hint = platform_screen->subpixelAntialiasingTypeHint();
#endif
    return lcd_from_qt_subpixel_hint(static_cast<int>(hint));
}

lcd_subpixel_order_t lcd_subpixel_order_from_windows()
{
#if defined(_WIN32)
    int font_smoothing_enabled = 0;
    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing,
            0U,
            &font_smoothing_enabled,
            0U)                == 0 ||
        font_smoothing_enabled == 0)
    {
        return lcd_subpixel_order_t::NONE;
    }

    unsigned int font_smoothing_type = 0U;
    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing_type,
            0U,
            &font_smoothing_type,
            0U) == 0)
    {
        return lcd_subpixel_order_t::NONE;
    }

    unsigned int font_smoothing_orientation = 0U;
    if (SystemParametersInfoW(
            k_win_spi_get_font_smoothing_orientation,
            0U,
            &font_smoothing_orientation,
            0U) == 0)
    {
        return lcd_subpixel_order_t::NONE;
    }

    return lcd_from_windows_font_smoothing_settings(
        true,
        font_smoothing_type,
        font_smoothing_orientation);
#else
    return lcd_subpixel_order_t::NONE;
#endif
}

} // anonymous namespace

std::unique_ptr<QObject> observe_lcd_settings(std::function<void()> changed)
{
    return std::make_unique<Lcd_settings_observer>(std::move(changed));
}

QString lcd_windows_registry_key(const QString& device_name)
{
    const auto display = device_name.startsWith(QStringLiteral("\\\\.\\"))
        ? device_name.mid(4) : device_name;
    if (!display.startsWith(QStringLiteral("DISPLAY")) || display.size() == 7) {
        return {};
    }
    for (const auto character : display.mid(7)) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) {
            return {};
        }
    }
    return QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Avalon.Graphics\\") + display;
}

QString lcd_windows_device_name(QScreen* screen)
{
#if defined(_WIN32)
    // QScreen::name() may be an EDID label. The native monitor owns the GDI
    // identity shared by Avalon.Graphics and EnumDisplaySettingsW.
    const auto* native_screen = screen
        ? screen->nativeInterface<QNativeInterface::QWindowsScreen>() : nullptr;
    if (native_screen) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(native_screen->handle(), &info)) {
            return QString::fromWCharArray(info.szDevice);
        }
    }
#else
    Q_UNUSED(screen);
#endif
    return {};
}

lcd_subpixel_order_t lcd_from_windows_display_settings(
    std::optional<unsigned int> pixel_structure,
    unsigned int rotation,
    lcd_subpixel_order_t fallback)
{
    using Order = lcd_subpixel_order_t;
    auto order = fallback;
    if (pixel_structure) {
        switch (*pixel_structure) {
            case 1: order = Order::RGB; break;
            case 2: order = Order::BGR; break;
            default: return Order::NONE;
        }
    }
    if (order != Order::RGB && order != Order::BGR) {
        return Order::NONE;
    }
    // DEVMODE specifies counter-clockwise physical display rotation.
    // A left-to-right RGB stripe then reads BGR from top to bottom at 90 degrees.
    const bool rgb = order == Order::RGB;
    switch (rotation) {
        case 0: return order;
        case 1: return rgb ? Order::VBGR : Order::VRGB;
        case 2: return rgb ? Order::BGR : Order::RGB;
        case 3: return rgb ? Order::VRGB : Order::VBGR;
        default: return Order::NONE;
    }
}

namespace {

lcd_subpixel_order_t lcd_subpixel_order_from_windows_display(QQuickWindow* window)
{
#if defined(_WIN32)
    if (!window || !window->screen()) {
        return lcd_subpixel_order_t::NONE;
    }
    const auto device = lcd_windows_device_name(window->screen());
    const auto key = lcd_windows_registry_key(device);
    if (key.isEmpty()) {
        return lcd_subpixel_order_t::NONE;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const auto device_name = device.toStdWString();
    if (!EnumDisplaySettingsW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        return lcd_subpixel_order_t::NONE;
    }
    QSettings settings(key, QSettings::NativeFormat);
    const auto value = settings.value(QStringLiteral("PixelStructure"));
    std::optional<unsigned int> pixel_structure;
    if (value.isValid()) {
        bool converted = false;
        pixel_structure = value.toUInt(&converted);
        if (!converted) {
            return lcd_subpixel_order_t::NONE;
        }
    }
    const auto fallback = pixel_structure ? lcd_subpixel_order_t::NONE : lcd_subpixel_order_from_windows();
    return lcd_from_windows_display_settings(pixel_structure, mode.dmDisplayOrientation, fallback);
#else
    Q_UNUSED(window);
    return lcd_subpixel_order_t::NONE;
#endif
}

} // anonymous namespace

lcd_subpixel_order_t resolve_lcd_subpixel_order_from_probes(
    lcd_request_t                requested,
    const lcd_resolver_probes_t& probes)
{
    if (!requested.automatic) {
        return detail::lcd_effective_order(
            requested,
            lcd_subpixel_order_t::NONE);
    }

    const lcd_subpixel_order_t qt_order = probes.qt_probe
        ? detail::lcd_sanitize_resolved_order(probes.qt_probe())
        : lcd_subpixel_order_t::NONE;
    if (vnm::msdf_text::lcd::is_display_specific(qt_order)) {
        return qt_order;
    }

    const lcd_subpixel_order_t windows_order = probes.windows_probe
        ? detail::lcd_sanitize_resolved_order(probes.windows_probe())
        : lcd_subpixel_order_t::NONE;
    return detail::lcd_auto_order_from_detections(qt_order, windows_order);
}

lcd_subpixel_order_t resolve_lcd_subpixel_order_for_window(
    lcd_request_t requested,
    QQuickWindow* window)
{
#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
    if (requested.automatic) {
        g_platform_probe_count.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    lcd_resolver_probes_t probes;
    probes.qt_probe = [window] {
        return lcd_subpixel_order_from_qt(window);
    };
    probes.windows_probe = [window] {
        return lcd_subpixel_order_from_windows_display(window);
    };
    return resolve_lcd_subpixel_order_from_probes(requested, probes);
}

lcd_subpixel_order_t lcd_from_qt_subpixel_hint(int qt_subpixel_hint)
{
    switch (qt_subpixel_hint) {
        case static_cast<int>(QPlatformScreen::Subpixel_RGB):  return lcd_subpixel_order_t::RGB;
        case static_cast<int>(QPlatformScreen::Subpixel_BGR):  return lcd_subpixel_order_t::BGR;
        case static_cast<int>(QPlatformScreen::Subpixel_VRGB): return lcd_subpixel_order_t::VRGB;
        case static_cast<int>(QPlatformScreen::Subpixel_VBGR): return lcd_subpixel_order_t::VBGR;
        case static_cast<int>(QPlatformScreen::Subpixel_None):
        default:                                               return lcd_subpixel_order_t::NONE;
    }
}

lcd_subpixel_order_t lcd_from_windows_font_smoothing_settings(
    bool           enabled,
    unsigned int   smoothing_type,
    unsigned int   smoothing_orientation)
{
    if (!enabled) {
        return lcd_subpixel_order_t::NONE;
    }

    if (smoothing_type != k_win_font_smoothing_cleartype) {
        return lcd_subpixel_order_t::NONE;
    }

    switch (smoothing_orientation) {
        case k_win_font_smoothing_orientation_rgb: return lcd_subpixel_order_t::RGB;
        case k_win_font_smoothing_orientation_bgr: return lcd_subpixel_order_t::BGR;
        default:                                   return lcd_subpixel_order_t::NONE;
    }
}

#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
std::size_t lcd_platform_probe_count_for_test()
{
    return g_platform_probe_count.load(std::memory_order_relaxed);
}
#endif

} // namespace vnm::plot
