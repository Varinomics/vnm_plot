// VNM Plot Library - Core Font Renderer Stub
// No-op implementation for builds without MSDF text rendering.
// Provides stable symbols for core library when VNM_PLOT_ENABLE_TEXT is OFF.

#include <vnm_plot/rhi/font_renderer.h>

namespace vnm::plot {

#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
std::uint64_t Font_renderer::atlas_upload_enqueues_for_test() const { return 0; }
#endif

// Completes the private type owned by the renderer in text-disabled builds.
struct Font_renderer::impl_t {};

Font_renderer::Font_renderer(Asset_loader&)
{
    // No atlas is ever built, so the loader is not retained.
}

Font_renderer::~Font_renderer() = default;

void Font_renderer::initialize_metrics(int, bool) {}

void Font_renderer::set_log_callbacks(
    std::function<void(const std::string&)>,
    std::function<void(const std::string&)>) {}

float Font_renderer::measure_text_px(const char*) const { return 0.0f; }

bool Font_renderer::text_visual_bounds_px(const char*, float, float, glm::vec4&) const { return false; }

bool Font_renderer::text_ink_bounds_px(const char*, float, float, glm::vec4&) const { return false; }

std::uint64_t Font_renderer::text_measure_cache_key() const { return 0; }

float Font_renderer::monospace_advance_px() const { return 0.0f; }

bool Font_renderer::monospace_advance_is_reliable() const { return false; }

float Font_renderer::compute_numeric_bottom() const { return 0.0f; }

float Font_renderer::baseline_offset_px() const { return 0.0f; }

void Font_renderer::batch_text(float, float, const char*) {}

void Font_renderer::rhi_begin_frame() {}

void Font_renderer::rhi_queue_draw(
    const frame_context_t&,
    const glm::vec4&,
    const text_scissor_t&,
    const text_shadow_t&) {}

void Font_renderer::rhi_queue_draw(
    const frame_context_t&,
    const glm::vec4&,
    const text_scissor_t&,
    const text_shadow_t&,
    const text_lcd_t&) {}

bool Font_renderer::rhi_finalize_frame(const frame_context_t&) { return true; }

std::size_t Font_renderer::queued_draw_count() const { return 0; }

void Font_renderer::rhi_record_draws(const frame_context_t&, std::size_t) {}

void Font_renderer::rhi_record_frame(const frame_context_t&) {}

void Font_renderer::rhi_reset_frame() {}

} // namespace vnm::plot
