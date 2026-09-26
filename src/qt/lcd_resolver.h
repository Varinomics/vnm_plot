#pragma once

#include <vnm_plot/core/lcd.h>

#include <functional>
#include <cstddef>
#include <memory>
#include <optional>
#include <QString>

class QQuickWindow;
class QObject;
class QScreen;

namespace vnm::plot {

std::unique_ptr<QObject> observe_lcd_settings(std::function<void()> changed);

QString lcd_windows_registry_key(const QString& device_name);
QString lcd_windows_device_name(QScreen* screen);
lcd_subpixel_order_t lcd_from_windows_display_settings(
    std::optional<unsigned int> pixel_structure,
    unsigned int rotation,
    lcd_subpixel_order_t fallback);

struct lcd_resolver_probes_t
{
    using probe_t = std::function<lcd_subpixel_order_t()>;

    probe_t qt_probe      = nullptr;
    probe_t windows_probe = nullptr;
};

lcd_subpixel_order_t resolve_lcd_subpixel_order_from_probes(
    lcd_request_t                requested,
    const lcd_resolver_probes_t& probes);

lcd_subpixel_order_t resolve_lcd_subpixel_order_for_window(
    lcd_request_t  requested,
    QQuickWindow*  window);

lcd_subpixel_order_t lcd_from_qt_subpixel_hint(int qt_subpixel_hint);

lcd_subpixel_order_t lcd_from_windows_font_smoothing_settings(
    bool         enabled,
    unsigned int smoothing_type,
    unsigned int smoothing_orientation);

#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
std::size_t lcd_platform_probe_count_for_test();
#endif

} // namespace vnm::plot
