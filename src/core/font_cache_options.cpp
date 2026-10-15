#include <vnm_plot/rhi/font_renderer.h>

#include <atomic>
#include <mutex>

namespace vnm::plot {

namespace {

std::atomic<bool> s_disk_cache_enabled{true};
std::mutex s_disk_cache_options_mutex;
Font_disk_cache_options s_disk_cache_options;

} // namespace

void set_font_disk_cache_enabled(bool enabled)
{
    s_disk_cache_enabled.store(enabled, std::memory_order_relaxed);
}

bool font_disk_cache_enabled()
{
    return s_disk_cache_enabled.load(std::memory_order_relaxed);
}

void set_font_disk_cache_options(const Font_disk_cache_options& options)
{
    std::lock_guard<std::mutex> lock(s_disk_cache_options_mutex);
    s_disk_cache_options = options;
}

Font_disk_cache_options font_disk_cache_options()
{
    std::lock_guard<std::mutex> lock(s_disk_cache_options_mutex);
    return s_disk_cache_options;
}

} // namespace vnm::plot
