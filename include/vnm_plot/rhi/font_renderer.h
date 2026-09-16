#pragma once

// VNM Plot Library - RHI Font Renderer
// MSDF text rendering with font loading, measurement, and QRhi rendering.

#include <vnm_plot/core/lcd.h>
#include <vnm_plot/rhi/frame_context.h>

#include <glm/glm.hpp>

#if defined(VNM_PLOT_ENABLE_TEXT) && defined(VNM_PLOT_ENABLE_TEST_HOOKS)
#include <array>
#include <filesystem>
#endif

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vnm::plot {

class Asset_loader;

struct text_scissor_t
{
    bool       enabled = false;
    int        x       = 0;
    int        y       = 0;
    int        width   = 0;
    int        height  = 0;
};

struct text_shadow_t
{
    glm::vec4  color     = glm::vec4(0.f);
    float      radius_px = 0.0f;
};

struct text_lcd_t
{
    lcd_subpixel_order_t subpixel_order = lcd_subpixel_order_t::NONE;
    glm::vec4  background_color = glm::vec4(0.f);
};

// -----------------------------------------------------------------------------
// Font Cache Configuration
// -----------------------------------------------------------------------------
// Controls whether MSDF font data is persisted to disk for faster startup.

// Enable or disable disk caching of MSDF font data (default: enabled).
// Call before Font_renderer::initialize_metrics() to take effect.
void set_font_disk_cache_enabled(bool enabled);

// Returns true if disk caching is enabled.
[[nodiscard]] bool font_disk_cache_enabled();

#if defined(VNM_PLOT_ENABLE_TEXT) && defined(VNM_PLOT_ENABLE_TEST_HOOKS)
namespace detail {

using font_disk_cache_digest_t = std::array<std::uint8_t, 32>;

[[nodiscard]] bool validate_font_disk_cache_file(
    const std::filesystem::path&       path,
    const font_disk_cache_digest_t&    expected_digest,
    int                                pixel_height);

} // namespace detail
#endif

// -----------------------------------------------------------------------------
// Font Renderer
// -----------------------------------------------------------------------------
// Renders text using multi-channel signed distance field (MSDF) technique.
// The atlas is memoized process-wide and handed to each instance as an
// immutable shared snapshot, so instances on one thread stay independent.
class Font_renderer
{
public:
    // Binds the font source for the renderer's whole life: the "fonts/monospace.ttf"
    // asset of this loader, which must outlive the renderer. A consumer that wants
    // a second typeface builds a second renderer on the loader that registers it.
    //
    // Resolving the font costs a read and a digest of the font bytes, which is
    // too much to repeat on every frame, so initialize_metrics() below keeps the
    // atlas it already holds. Binding the loader here is what makes that sound:
    // one renderer can never be asked for two fonts.
    explicit Font_renderer(Asset_loader& asset_loader);
    ~Font_renderer();

    // Non-copyable and non-movable due to renderer resource ownership.
    Font_renderer(const Font_renderer&)            = delete;
    Font_renderer& operator=(const Font_renderer&) = delete;
    Font_renderer(Font_renderer&&)                 = delete;
    Font_renderer& operator=(Font_renderer&&)      = delete;

    // Initializes CPU font metrics/cache for layout calculation before the render pass.
    // The atlas is built from the bound loader's font asset, so renderers whose
    // loaders register different font bytes get different atlases, and each
    // atlas is built once per (font, pixel height) across all threads.
    //
    // The font asset is read at the first call for a pixel height. force_rebuild
    // re-reads it and rebuilds the atlas, which is also how a caller that
    // changed the loader's font bytes in place picks them up.
    void initialize_metrics(int pixel_height, bool force_rebuild = false);

    void set_log_callbacks(
        std::function<void(const std::string&)>    log_error,
        std::function<void(const std::string&)>    log_debug_info);

    // --- Text Measurement ---

    // Measures the horizontal pixel width of a given string.
    float measure_text_px(const char* text) const;

    // Returns the visual glyph quad bounds for text at the given baseline.
    bool text_visual_bounds_px(const char* text, float x, float y, glm::vec4& bounds) const;

    // Returns the pixels the text can put colour in, at the same baseline. A
    // glyph's quad carries the distance field's padding all round its outline,
    // and the shader leaves that padding clear except for the anti-aliasing at
    // the outline itself, so this bound is every glyph's outline widened by
    // that anti-aliasing and clipped to its own quad. It is the bound for a
    // scissor, a culling extent or a conflict rectangle, where the quad
    // text_visual_bounds_px() reports is far wider than the drawn glyph; the
    // quad remains the bound for what the frame uploads.
    bool text_ink_bounds_px(const char* text, float x, float y, glm::vec4& bounds) const;

    // Returns a key that changes when font metrics change (for caching).
    std::uint64_t text_measure_cache_key() const;

    // Returns the advance width of '0' in pixels (for monospace layout).
    float monospace_advance_px() const;

    // Returns true if the monospace advance is reliably measured.
    bool monospace_advance_is_reliable() const;

    // Computes a font-specific metric for aligning numeric labels vertically.
    float compute_numeric_bottom() const;

    // Returns the baseline offset in pixels.
    float baseline_offset_px() const;

    // --- Text Rendering ---

    // Appends a string to the QRhi CPU batch to be drawn in this frame.
    // There is exactly one such batch, and it only exists between
    // rhi_begin_frame() and the frame's reset by rhi_record_frame() or
    // rhi_reset_frame(): outside that window, or before initialize_metrics()
    // has produced an atlas, the call is a no-op.
    void batch_text(float x, float y, const char* text);

    // Starts a QRhi text frame. Subsequent batch_text() calls append to the
    // QRhi CPU batch until rhi_record_frame() or rhi_reset_frame() resets the
    // frame state.
    void rhi_begin_frame();

    // Uploads the current QRhi CPU batch into this frame's draw plan and clears it.
    void rhi_queue_draw(
        const frame_context_t& ctx,
        const glm::mat4&       pmv,
        const glm::vec4&       color,
        const text_scissor_t&  scissor = {},
        const text_shadow_t&   shadow = {});

    void rhi_queue_draw(
        const frame_context_t& ctx,
        const glm::mat4&       pmv,
        const glm::vec4&       color,
        const text_scissor_t&  scissor,
        const text_shadow_t&   shadow,
        const text_lcd_t&      lcd);

    // Uploads the accumulated QRhi text geometry after all draw batches are queued.
    void rhi_finalize_frame(const frame_context_t& ctx);

    // Number of draw ops queued so far this frame. A shadowed rhi_queue_draw()
    // queues two, so the value counts neither draws nor strings: its only use
    // is as an `end` boundary for rhi_record_draws(), captured between queues.
    std::size_t queued_draw_count() const;

    // Records the queued draws from where the previous call stopped up to, but
    // not including, `end`, clamped to queued_draw_count(). Within those draws
    // every shadow is recorded before every foreground. The text pipeline is
    // bound on every call, so other renderers may record between calls. The
    // cursor only advances: an `end` at or before it records nothing. The call
    // never resets the frame; rhi_begin_frame() and rhi_reset_frame() return
    // the cursor to the first draw. Must be called inside the render pass.
    void rhi_record_draws(const frame_context_t& ctx, std::size_t end);

    // Records every queued draw that rhi_record_draws() has not recorded yet,
    // then resets the frame. Must be called inside the render pass.
    void rhi_record_frame(const frame_context_t& ctx);

    // Clears QRhi per-frame CPU/draw state without touching persistent resources.
    void rhi_reset_frame();

private:
    struct impl_t;
    std::unique_ptr<impl_t> m_impl;
};

} // namespace vnm::plot
