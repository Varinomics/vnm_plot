#include <vnm_plot/rhi/font_renderer.h>
#include <vnm_plot/core/lcd.h>
#include <vnm_plot/rhi/asset_loader.h>
#include "atomic_file_write.h"
#include "font_atlas_cache.h"
#include "platform_paths.h"
#include "rhi_helpers.h"

#include <glm/gtc/type_ptr.hpp>
#include <vnm_msdf_text/lcd_contract.h>
#include <vnm_msdf_text/msdf_text.h>

#include <QByteArray>
#include <QByteArrayView>
#include <QFile>
#include <QCryptographicHash>
#include <QImage>
#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <type_traits>
#include <vector>

namespace vnm::plot {

namespace {

constexpr std::uint32_t k_cache_version       = 4;
constexpr double        k_min_atlas_font_size = 48.0;
constexpr float         k_atlas_px_range      = 10.0f;
// 1.0 is a one-output-pixel anti-aliasing ramp now that vnm_msdf_text encodes
// atlas_px_range in true atlas pixels; 2.5 compensated for the shallow
// font-dependent encode the builder produced before that conversion.
constexpr float k_sharpness_bias     = 1.0f;
constexpr int   k_atlas_texture_size = 2048;
// Hashed into the font digest so a change in the builder's bake semantics
// re-keys the cache. The atlas_px_range suffix marks the corrected encode
// (range converted to msdfgen shape units, so SDF slope is font-independent).
constexpr char k_msdf_builder_semantics[] =
    "vnm_msdf_text_scale_independent_font_units_mtsdf_atlas_px_range";

std::atomic<bool> s_disk_cache_enabled{true};

} // anonymous namespace

// -----------------------------------------------------------------------------
// Font Cache Configuration
// -----------------------------------------------------------------------------

void set_font_disk_cache_enabled(bool enabled)
{
    s_disk_cache_enabled.store(enabled, std::memory_order_relaxed);
}

bool font_disk_cache_enabled()
{
    return s_disk_cache_enabled.load(std::memory_order_relaxed);
}

namespace {

using msdf_atlas_t       = vnm::msdf_text::atlas_t;
using msdf_glyph_t       = vnm::msdf_text::glyph_t;
using msdf_kerning_key_t = vnm::msdf_text::kerning_key_t;
using text_vertex_t      = vnm::msdf_text::text_vertex_t;

constexpr std::size_t k_text_vertex_float_count = 10u;

struct rhi_text_vertex_t
{
    float x            = 0.f;
    float y            = 0.f;
    float s_min        = 0.f;
    float t_min        = 0.f;
    float s_max        = 0.f;
    float t_max        = 0.f;
    float frame_x      = 0.f;
    float frame_y      = 0.f;
    float frame_width  = 0.f;
    float frame_height = 0.f;
};

static_assert(
    sizeof(rhi_text_vertex_t) == k_text_vertex_float_count * sizeof(float),
    "MSDF RHI text vertex layout");
static_assert(
    std::is_standard_layout_v<rhi_text_vertex_t>,
    "MSDF RHI text vertex must remain standard layout");

std::atomic<std::uint64_t> s_next_cache_epoch{1};

using detail::cached_font_data_t;
using detail::font_atlas_key_t;

// Every retained entry is one k_atlas_texture_size RGBA bitmap (16 MiB at
// 2048), so this budget keeps the eight most recently used (font, draw height)
// pairs alive: enough for the handful of label sizes a plot cycles through
// while DPI or zoom changes, and far below the ~1 GiB a 64-entry ceiling
// allowed. Evicted entries cost a rebuild, not a wrong result.
constexpr std::size_t k_max_retained_atlas_bytes =
    8u * static_cast<std::size_t>(k_atlas_texture_size) *
    static_cast<std::size_t>(k_atlas_texture_size) * 4u;

detail::Font_atlas_cache& font_atlas_cache()
{
    static detail::Font_atlas_cache cache(k_max_retained_atlas_bytes);
    return cache;
}

vnm::msdf_text::options_t atlas_options()
{
    vnm::msdf_text::options_t options;
    options.atlas_size          = k_atlas_texture_size;
    options.min_atlas_font_size = k_min_atlas_font_size;
    options.atlas_px_range      = k_atlas_px_range;
    options.sharpness_bias      = k_sharpness_bias;
    options.build_kerning_table = true;
    return options;
}

const std::vector<char32_t>& glyph_codepoints()
{
    static const std::vector<char32_t> codepoints = vnm::msdf_text::default_codepoints();
    return codepoints;
}

std::string glyph_seed_string()
{
    return vnm::msdf_text::codepoints_to_utf8(glyph_codepoints());
}

bool uv_in_range(float value)
{
    constexpr float k_uv_slop = 0.001f;
    return std::isfinite(value) && value >= -k_uv_slop && value <= 1.0f + k_uv_slop;
}

bool validate_cached_glyph(const msdf_glyph_t& g)
{
    // Geometry is stored in scale-independent font units (Y-up bounds); plane
    // rectangles are derived per draw size via scaled_glyph(). Invisible
    // advance-only glyphs (e.g. U+0020) carry zero bounds, so equality is valid.
    return (
        std::isfinite(g.advance_units)              &&
        std::isfinite(g.bounds_left_units)          &&
        std::isfinite(g.bounds_bottom_units)        &&
        std::isfinite(g.bounds_right_units)         &&
        std::isfinite(g.bounds_top_units)           &&
        g.bounds_right_units >= g.bounds_left_units &&
        g.bounds_top_units >= g.bounds_bottom_units &&
        uv_in_range(g.uv_left)                      &&
        uv_in_range(g.uv_bottom)                    &&
        uv_in_range(g.uv_right)                     &&
        uv_in_range(g.uv_top)                       &&
        g.uv_right >= g.uv_left                     &&
        g.uv_bottom >= g.uv_top);
}

// draw_scale matching the library's draw_scaling_for(): draw_pixel_height /
// font-unit ascender, computed in double like the library so a font-unit advance
// scaled here matches scaled_glyph().advance_x. Keep in sync with the library if
// it ever changes how advances scale.
double draw_scale_for(const msdf_atlas_t& atlas, int draw_pixel_height)
{
    const double ascender = atlas.font_metrics_units.ascender;
    return (ascender > 0.0)
        ? static_cast<double>(draw_pixel_height) / ascender
        : 0.0;
}

// How far beyond a glyph's outline the fragment shader can still put colour,
// in output pixels. It decodes a signed distance multiplied by sharpness_bias
// and keeps a fragment while that distance is above -0.5, so the coverage ramp
// reaches 0.5 / sharpness_bias pixels outside the outline. With LCD filtering
// a fragment also samples three subpixel steps -- one whole pixel -- to either
// side, and the widest of the three channels decides its alpha. A bias that is
// not positive decodes every texel of the glyph alike, and then nothing
// narrower than the whole quad can be claimed.
double anti_aliasing_reach_px(const msdf_atlas_t& atlas)
{
    return (atlas.sharpness_bias > 0.0f)
        ? 1.0 + 0.5 / double(atlas.sharpness_bias)
        : std::numeric_limits<double>::max();
}

void add_text_to_vectors(
    const char*                    text,
    float                          x,
    float                          y,
    int                            draw_pixel_height,
    const msdf_atlas_t&            atlas,
    std::vector<float>&            vertex_data,
    std::vector<std::uint32_t>&    index_data)
{
    if (!text) {
        return;
    }

    std::vector<text_vertex_t> source_vertices;
    std::vector<std::uint32_t> indices;
    vnm::msdf_text::append_text_quads(
        atlas, draw_pixel_height, text, x, y, source_vertices, &indices);
    if (source_vertices.empty() ||
        indices.empty() ||
        source_vertices.size() % 4u != 0u)
    {
        return;
    }

    std::vector<rhi_text_vertex_t> vertices;
    vertices.reserve(source_vertices.size());
    for (std::size_t i = 0; i < source_vertices.size(); i += 4u) {
        const text_vertex_t& a            = source_vertices[i + 0u];
        const text_vertex_t& b            = source_vertices[i + 1u];
        const text_vertex_t& c            = source_vertices[i + 2u];
        const text_vertex_t& d            = source_vertices[i + 3u];
        const float          frame_left   = std::min(std::min(a.x, b.x), std::min(c.x, d.x));
        const float          frame_top    = std::min(std::min(a.y, b.y), std::min(c.y, d.y));
        const float          frame_right  = std::max(std::max(a.x, b.x), std::max(c.x, d.x));
        const float          frame_bottom = std::max(std::max(a.y, b.y), std::max(c.y, d.y));
        const float          frame_width  = frame_right - frame_left;
        const float          frame_height = frame_bottom - frame_top;

        const auto append_vertex = [&](const text_vertex_t& vertex) {
            vertices.push_back(rhi_text_vertex_t{
                vertex.x,
                vertex.y,
                vertex.s_min,
                vertex.t_min,
                vertex.s_max,
                vertex.t_max,
                frame_left,
                frame_top,
                frame_width,
                frame_height});
        };

        append_vertex(a);
        append_vertex(b);
        append_vertex(c);
        append_vertex(d);
    }

    if (vertex_data.size() % k_text_vertex_float_count != 0u) {
        return;
    }
    quint32 base_vertex = 0;
    if (!detail::to_qrhi_count(
            vertex_data.size() / k_text_vertex_float_count, base_vertex))
    {
        return;
    }

    std::size_t added_float_count  = 0;
    std::size_t new_float_count    = 0;
    std::size_t new_index_count    = 0;
    std::size_t new_vertex_count   = 0;
    quint32     checked_qrhi_value = 0;
    if (!detail::checked_size_product(
            vertices.size(), k_text_vertex_float_count, added_float_count)                     ||
        !detail::checked_size_add(vertex_data.size(), added_float_count, new_float_count)      ||
        !detail::checked_size_add(index_data.size(), indices.size(), new_index_count)          ||
        !detail::checked_size_add(
            vertex_data.size() / k_text_vertex_float_count, vertices.size(), new_vertex_count) ||
        !detail::to_qrhi_count(new_vertex_count, checked_qrhi_value)                           ||
        !detail::qrhi_byte_size(new_float_count, sizeof(float), checked_qrhi_value)            ||
        !detail::qrhi_byte_size( new_index_count, sizeof(std::uint32_t), checked_qrhi_value))
    {
        return;
    }
    vertex_data.reserve(new_float_count);
    index_data.reserve(new_index_count);
    for (std::uint32_t index : indices) {
        index_data.push_back(base_vertex + index);
    }

    for (const rhi_text_vertex_t& vertex : vertices) {
        vertex_data.push_back(vertex.x);
        vertex_data.push_back(vertex.y);
        vertex_data.push_back(vertex.s_min);
        vertex_data.push_back(vertex.t_min);
        vertex_data.push_back(vertex.s_max);
        vertex_data.push_back(vertex.t_max);
        vertex_data.push_back(vertex.frame_x);
        vertex_data.push_back(vertex.frame_y);
        vertex_data.push_back(vertex.frame_width);
        vertex_data.push_back(vertex.frame_height);
    }
}

std::array<std::uint8_t, 32> compute_font_digest(const Byte_buffer& font_bytes)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto add_view = [&hash](const char* data, qsizetype size) {
        hash.addData(QByteArrayView(data, size));
    };
    const auto add_bytes = [&add_view](const auto& value) {
        add_view(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    add_bytes(k_cache_version);
    add_bytes(k_min_atlas_font_size);
    add_bytes(k_atlas_px_range);
    add_bytes(k_sharpness_bias);
    add_bytes(k_atlas_texture_size);
    const std::string_view semantics(k_msdf_builder_semantics);
    add_view(semantics.data(), static_cast<qsizetype>(semantics.size()));
    const std::string glyph_seed = glyph_seed_string();
    add_view(glyph_seed.data(), static_cast<qsizetype>(glyph_seed.size()));
    add_view(font_bytes.data(), static_cast<qsizetype>(font_bytes.size()));

    const QByteArray bytes = hash.result();
    std::array<std::uint8_t, 32> digest{};
    std::copy_n(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        std::min<std::size_t>(digest.size(), static_cast<std::size_t>(bytes.size())),
        digest.begin());
    return digest;
}

std::string digest_to_hex(const std::array<std::uint8_t, 32>& digest)
{
    return
        QByteArray(
            reinterpret_cast<const char*>(digest.data()),
            static_cast<qsizetype>(digest.size())
        ).toHex().toStdString();
}

std::filesystem::path resolve_cache_directory()
{
    // Prefer cache directory for disposable MSDF artifacts
    auto cache_dir = get_cache_directory();
    if (!cache_dir.empty()) {
        return cache_dir;
    }

    // Fallback to data directory
    cache_dir = get_data_directory();
    if (!cache_dir.empty()) {
        return cache_dir;
    }

    // Last resort: current directory
    cache_dir = std::filesystem::current_path() / ".vnm_plot_cache";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    return cache_dir;
}

std::filesystem::path cache_file_path(
    int                                    pixel_height,
    const std::array<std::uint8_t, 32>&    font_digest)
{
    // Resolved exactly once and never reassigned: the magic-static guard makes
    // the resolution itself thread-safe and the value is immutable afterwards,
    // so concurrent render threads never observe a half-assigned path.
    static const std::filesystem::path cache_dir = resolve_cache_directory();

    std::ostringstream oss;
    oss << "msdf_cache_v" << k_cache_version << "_px" << pixel_height << "_font";
    oss << digest_to_hex(font_digest);
    oss << ".bin";
    return cache_dir / oss.str();
}

// Forward declarations for disk cache helpers
std::shared_ptr<cached_font_data_t> load_cached_font_from_disk(
    const std::filesystem::path&           path,
    const std::array<std::uint8_t, 32>&    expected_digest,
    int                                    pixel_height);

[[nodiscard]] bool save_cached_font_to_disk(
    const std::filesystem::path&           path,
    const cached_font_data_t&              font);

std::shared_ptr<cached_font_data_t> load_cached_font_from_disk(
    const std::filesystem::path&           path,
    const std::array<std::uint8_t, 32>&    expected_digest,
    int                                    pixel_height)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return nullptr;
    }

    auto read = [&](auto& val) -> bool {
        in.read(reinterpret_cast<char*>(&val), sizeof(val));
        return bool(in);
    };

    std::uint32_t magic   = 0;
    std::uint32_t version = 0;
    std::uint32_t height  = 0;
    if (!read(magic) || !read(version) || !read(height)) {
        return nullptr;
    }
    constexpr std::uint32_t k_magic = 0x4d534446; // 'MSDF'
    if (magic != k_magic || version != k_cache_version || height != static_cast<std::uint32_t>(pixel_height)) {
        return nullptr;
    }

    std::array<std::uint8_t, 32> digest{};
    in.read(reinterpret_cast<char*>(digest.data()), digest.size());
    if (!in || digest != expected_digest) {
        return nullptr;
    }

    auto font = std::make_shared<cached_font_data_t>();
    font->draw_pixel_height = pixel_height;
    font->font_digest       = digest;

    std::uint32_t atlas_size = 0;
    if (!read(atlas_size))                                              { return nullptr; }
    if (atlas_size != static_cast<std::uint32_t>(k_atlas_texture_size)) { return nullptr; }
    font->atlas.atlas_size = static_cast<int>(atlas_size);

    std::uint32_t baked_pixel_height = 0;
    if (!read(baked_pixel_height) ||
        !read(font->atlas.atlas_px_range) ||
        !read(font->atlas.bitmap_scale) ||
        !read(font->atlas.sharpness_bias) ||
        !read(font->atlas.font_metrics_units.ascender) ||
        !read(font->atlas.font_metrics_units.descender) ||
        !read(font->atlas.font_metrics_units.line_height) ||
        !read(font->atlas.font_metrics_units.em_size) ||
        !read(font->atlas.zero_advance_units))
    {
        return nullptr;
    }
    font->atlas.baked_pixel_height = static_cast<int>(baked_pixel_height);
    // ascender, bitmap_scale, and atlas_px_range are divisors/projections in the
    // scaling helpers, so require them strictly positive; the rest must be finite.
    if (!(font->atlas.atlas_px_range > 0.0) ||
        !(font->atlas.bitmap_scale > 0.0) ||
        !std::isfinite(font->atlas.sharpness_bias) ||
        !std::isfinite(font->atlas.font_metrics_units.ascender) ||
        !(font->atlas.font_metrics_units.ascender > 0.f) ||
        !std::isfinite(font->atlas.font_metrics_units.descender) ||
        !std::isfinite(font->atlas.font_metrics_units.line_height) ||
        !std::isfinite(font->atlas.font_metrics_units.em_size) ||
        !std::isfinite(font->atlas.zero_advance_units))
    {
        return nullptr;
    }
    std::uint8_t zero_available = 0;
    std::uint8_t padding[3]{};
    if (!read(zero_available) || !in.read(reinterpret_cast<char*>(padding), sizeof(padding))) {
        return nullptr;
    }
    font->atlas.zero_advance_available = (zero_available != 0);

    std::uint32_t glyph_count = 0;
    if (!read(glyph_count)) {
        return nullptr;
    }
    const std::size_t glyph_count_limit =
        std::max<std::size_t>(glyph_codepoints().size() + 16u, 256u);
    if (glyph_count > glyph_count_limit) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < glyph_count; ++i) {
        std::uint32_t code    = 0;
        std::uint8_t  visible = 0;
        msdf_glyph_t g{};
        if (!read(code) ||
            !read(g.advance_units) ||
            !read(g.bounds_left_units) ||
            !read(g.bounds_bottom_units) ||
            !read(g.bounds_right_units) ||
            !read(g.bounds_top_units) ||
            !read(g.uv_left) ||
            !read(g.uv_bottom) ||
            !read(g.uv_right) ||
            !read(g.uv_top) ||
            !read(visible))
        {
            return nullptr;
        }
        g.visible = (visible != 0);
        if (!validate_cached_glyph(g)) {
            return nullptr;
        }
        font->atlas.glyphs.emplace(static_cast<char32_t>(code), g);
    }

    std::uint32_t kerning_count = 0;
    if (!read(kerning_count)) {
        return nullptr;
    }
    const std::size_t kerning_limit =
        static_cast<std::size_t>(glyph_count) * static_cast<std::size_t>(glyph_count);
    if (kerning_count > kerning_limit) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < kerning_count; ++i) {
        msdf_kerning_key_t key{};
        float value = 0.f;
        if (!read(key) || !read(value)) {
            return nullptr;
        }
        if (!std::isfinite(value)) {
            return nullptr;
        }
        font->atlas.kerning_units.emplace(key, value);
    }

    std::uint32_t atlas_bytes = 0;
    if (!read(atlas_bytes)) {
        return nullptr;
    }
    const std::uint32_t expected_atlas_bytes =
        static_cast<std::uint32_t>(k_atlas_texture_size) *
        static_cast<std::uint32_t>(k_atlas_texture_size) *
        4u;
    if (atlas_bytes != expected_atlas_bytes) {
        return nullptr;
    }
    font->atlas.rgba.resize(atlas_bytes);
    if (!font->atlas.rgba.empty()) {
        in.read(reinterpret_cast<char*>(font->atlas.rgba.data()), atlas_bytes);
        if (!in) {
            return nullptr;
        }
    }

    font->cache_epoch = s_next_cache_epoch.fetch_add(1, std::memory_order_relaxed);
    return font;
}

bool serialize_cached_font(std::ostream& out, const cached_font_data_t& font)
{
    auto write = [&](auto val) {
        out.write(reinterpret_cast<const char*>(&val), sizeof(val));
    };

    constexpr std::uint32_t k_magic = 0x4d534446; // 'MSDF'
    write(k_magic);
    write(k_cache_version);
    write(static_cast<std::uint32_t>(font.draw_pixel_height));
    out.write(reinterpret_cast<const char*>(font.font_digest.data()), font.font_digest.size());
    write(static_cast<std::uint32_t>(font.atlas.atlas_size));
    write(static_cast<std::uint32_t>(font.atlas.baked_pixel_height));
    write(font.atlas.atlas_px_range);
    write(font.atlas.bitmap_scale);
    write(font.atlas.sharpness_bias);
    write(font.atlas.font_metrics_units.ascender);
    write(font.atlas.font_metrics_units.descender);
    write(font.atlas.font_metrics_units.line_height);
    write(font.atlas.font_metrics_units.em_size);
    write(font.atlas.zero_advance_units);
    std::uint8_t zero_available = font.atlas.zero_advance_available ? 1u : 0u;
    out.write(reinterpret_cast<const char*>(&zero_available), sizeof(zero_available));
    std::uint8_t padding[3]{0, 0, 0};
    out.write(reinterpret_cast<const char*>(padding), sizeof(padding));

    write(static_cast<std::uint32_t>(font.atlas.glyphs.size()));
    for (const auto& [code, g] : font.atlas.glyphs) {
        write(static_cast<std::uint32_t>(code));
        write(g.advance_units);
        write(g.bounds_left_units);
        write(g.bounds_bottom_units);
        write(g.bounds_right_units);
        write(g.bounds_top_units);
        write(g.uv_left);
        write(g.uv_bottom);
        write(g.uv_right);
        write(g.uv_top);
        write(static_cast<std::uint8_t>(g.visible ? 1u : 0u));
    }

    write(static_cast<std::uint32_t>(font.atlas.kerning_units.size()));
    for (const auto& [key, value] : font.atlas.kerning_units) {
        write(key);
        write(value);
    }

    write(static_cast<std::uint32_t>(font.atlas.rgba.size()));
    if (!font.atlas.rgba.empty()) {
        out.write(
            reinterpret_cast<const char*>(font.atlas.rgba.data()),
            static_cast<std::streamsize>(font.atlas.rgba.size()));
    }

    return static_cast<bool>(out);
}

bool save_cached_font_to_disk(
    const std::filesystem::path&   path,
    const cached_font_data_t&      font)
{
    // Published through a temporary of its own, so a reader never sees a
    // partially written cache and two writers of the same file cannot
    // interleave their records into a structurally valid mixture.
    return detail::write_file_atomically(
        path,
        [&font](std::ostream& out) {
            return serialize_cached_font(out, font);
        });
}

std::shared_ptr<cached_font_data_t> build_font_cache(
    const Byte_buffer&                             font_bytes,
    int                                            pixel_height,
    const std::array<std::uint8_t, 32>&            font_digest,
    const std::function<void(const std::string&)>& log_error,
    const std::function<void(const std::string&)>& log_debug_info)
{
    auto font = std::make_shared<cached_font_data_t>();
    font->font_digest       = font_digest;
    font->draw_pixel_height = pixel_height;

    auto result = vnm::msdf_text::build_font_atlas(
        reinterpret_cast<const std::uint8_t*>(font_bytes.data()),
        font_bytes.size(),
        pixel_height,
        glyph_codepoints(),
        atlas_options(),
        log_debug_info);
    if (result.status == vnm::msdf_text::Build_status::FAILURE) {
        if (log_error) {
            log_error(result.message);
        }
        return nullptr;
    }

    font->atlas       = std::move(result.atlas);
    font->cache_epoch = s_next_cache_epoch.fetch_add(1, std::memory_order_relaxed);

    return font;
}

std::shared_ptr<cached_font_data_t> load_or_build_font_cache(
    Asset_loader&                                  asset_loader,
    int                                            pixel_height,
    bool                                           force_rebuild,
    const std::function<void(const std::string&)>& log_error,
    const std::function<void(const std::string&)>& log_debug_info)
{
    // The font belongs to the asset loader that was passed in: a process can
    // hold several loaders that register different fonts under this name, so
    // the bytes are read per request and their digest keys the cache.
    const auto font_bytes = asset_loader.load("fonts/monospace.ttf");
    if (!font_bytes || font_bytes->empty()) {
        if (log_error) {
            log_error("Failed to load MSDF font asset fonts/monospace.ttf");
        }
        return nullptr;
    }

    const font_atlas_key_t key{compute_font_digest(*font_bytes), pixel_height};
    const bool             disk_cache = s_disk_cache_enabled.load(std::memory_order_relaxed);

    return font_atlas_cache().get_or_build(
        key,
        force_rebuild,
        [&]() -> std::shared_ptr<cached_font_data_t> {
            // A forced rebuild has to regenerate the atlas, so it bypasses the
            // disk cache as well as the memo; reading the file back would hand
            // out the same bytes through another door.
            if (disk_cache && !force_rebuild) {
                auto from_disk = load_cached_font_from_disk(
                    cache_file_path(pixel_height, key.font_digest),
                    key.font_digest,
                    pixel_height);
                if (from_disk) {
                    return from_disk;
                }
            }

            auto built = build_font_cache(
                *font_bytes,
                pixel_height,
                key.font_digest,
                log_error,
                log_debug_info);
            if (built && disk_cache) {
                const auto cache_path = cache_file_path(pixel_height, key.font_digest);
                if (!save_cached_font_to_disk(cache_path, *built) && log_error) {
                    log_error("Failed to publish MSDF font cache " + cache_path.string());
                }
            }
            return built;
        });
}

} // anonymous namespace

#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
namespace detail {

bool validate_font_disk_cache_file(
    const std::filesystem::path&       path,
    const font_disk_cache_digest_t&    expected_digest,
    int                                pixel_height)
{
    return static_cast<bool>(
        load_cached_font_from_disk(path, expected_digest, pixel_height));
}

} // namespace detail
#endif

namespace {

using detail::load_qsb;

struct Text_block_std140
{
    float          pmv[16]             = {};
    float          color[4]            = {};
    float          shadow_color[4]     = {};
    float          px_range            = 0.f;
    float          target_width        = 0.f;
    float          target_height       = 0.f;
    float          shadow_radius       = 0.f;
    float          lcd_subpixel_order  = 0.f;
    std::int32_t   framebuffer_y_up    = 0;
    float          padding[2]          = {};
    float          background_color[4] = {};
};

static_assert(offsetof(Text_block_std140, pmv)              ==  0,    "Text UBO pmv offset");
static_assert(offsetof(Text_block_std140, color)            == 64,    "Text UBO color offset");
static_assert(offsetof(Text_block_std140, shadow_color)     == 80,    "Text UBO shadow color offset");
static_assert(offsetof(Text_block_std140, px_range)         == 96,    "Text UBO px_range offset");
static_assert(offsetof(Text_block_std140, target_width)     == 100,   "Text UBO target width offset");
static_assert(offsetof(Text_block_std140, target_height)    == 104,   "Text UBO target height offset");
static_assert(offsetof(Text_block_std140, shadow_radius)    == 108,   "Text UBO shadow radius offset");
static_assert(offsetof(Text_block_std140, lcd_subpixel_order) == 112, "Text UBO LCD order offset");
static_assert(offsetof(Text_block_std140, framebuffer_y_up) == 116,   "Text UBO framebuffer y-up offset");
static_assert(offsetof(Text_block_std140, background_color) == 128,   "Text UBO background color offset");
static_assert(sizeof(Text_block_std140)                     == 144,   "Text UBO std140 size");

constexpr std::uint32_t k_text_ubo_bytes = sizeof(Text_block_std140);

struct rhi_text_call_t
{
    std::unique_ptr<QRhiBuffer>
                   ubo;
    std::unique_ptr<QRhiShaderResourceBindings>
                   srb;
    QRhiBuffer*    srb_last_ubo       = nullptr;
    QRhiTexture*   srb_last_texture   = nullptr;
    QRhiSampler*   srb_last_sampler   = nullptr;
};

enum class rhi_text_pass_t : std::uint8_t
{
    SHADOW,
    FOREGROUND,
};

struct rhi_text_draw_op_t
{
    std::size_t                        call_index  = 0;
    quint32                            index_start = 0;
    quint32                            index_count = 0;
    text_scissor_t                     scissor;
    rhi_text_pass_t                    pass        = rhi_text_pass_t::FOREGROUND;
};

struct rhi_text_state_t
{
    QRhi*                              last_rhi    = nullptr;

    std::unique_ptr<QRhiTexture>       atlas_texture;
    std::unique_ptr<QRhiSampler>       sampler;
    int                                atlas_size = 0;
    std::uint64_t                      uploaded_cache_epoch = 0;

    std::unique_ptr<QRhiBuffer>        vbo;
    std::unique_ptr<QRhiBuffer>        ibo;
    std::size_t                        vbo_capacity_bytes = 0;
    std::size_t                        ibo_capacity_bytes = 0;

    std::vector<rhi_text_call_t>       calls;
    std::vector<rhi_text_draw_op_t>    ops;
    std::size_t                        call_used = 0;
    // First op the next rhi_record_draws() call records. Every path that
    // clears `ops` returns it to zero.
    std::size_t                        record_cursor = 0;

    std::unique_ptr<QRhiGraphicsPipeline>
                                       pipeline;
    QRhiRenderPassDescriptor*          pipeline_rpd = nullptr;
    int                                pipeline_samples = 0;

    QShader                            vert;
    QShader                            frag;
    bool                               shaders_loaded = false;
};

} // anonymous namespace

// --- PIMPL Definition ---
struct Font_renderer::impl_t
{
    explicit impl_t(Asset_loader& asset_loader)
    :
        m_asset_loader(asset_loader)
    {}

    // The renderer's font source for its whole life, so the atlas this instance
    // holds can only go stale in its draw size, or through an in-place change
    // to this loader's font bytes, which is what force_rebuild is for.
    Asset_loader&                              m_asset_loader;
    std::shared_ptr<cached_font_data_t>        m_font_cache;
    int                                        m_metric_pixel_height = 0;
    std::function<void(const std::string&)>    m_log_error;
    std::function<void(const std::string&)>    m_log_debug_info;
    bool                                       m_rhi_batch_active    = false;
    std::vector<float>                         m_rhi_vertex_data;
    std::vector<std::uint32_t>                 m_rhi_index_data;
    std::vector<float>                         m_rhi_frame_vertex_data;
    std::vector<std::uint32_t>                 m_rhi_frame_index_data;

    rhi_text_state_t m_rhi;

    // m_font_cache is a shared_ptr to an immutable atlas, so the accessors below
    // read it without a copy and two renderers on one thread never alias each
    // other's font: each holds its own reference to the entry its own asset
    // loader and draw height selected.
    const msdf_atlas_t* current_atlas() const
    {
        return m_font_cache ? &m_font_cache->atlas : nullptr;
    }

    std::uint64_t current_cache_epoch() const
    {
        return m_font_cache ? m_font_cache->cache_epoch : std::uint64_t{0};
    }

    // The draw pixel height matching current_atlas(): the scale-independent atlas
    // needs it to derive draw-size geometry, advances, metrics, and px_range.
    int current_draw_pixel_height() const
    {
        return m_metric_pixel_height;
    }
};

// --- Public API Implementation ---

Font_renderer::Font_renderer(Asset_loader& asset_loader)
    : m_impl(std::make_unique<impl_t>(asset_loader))
{
}

Font_renderer::~Font_renderer() = default;

void Font_renderer::set_log_callbacks(
    std::function<void(const std::string&)>    log_error,
    std::function<void(const std::string&)>    log_debug_info)
{
    m_impl->m_log_error = log_error;
    m_impl->m_log_debug_info = log_debug_info;
}

void Font_renderer::initialize_metrics(int pixel_height, bool force_rebuild)
{
    // Resolving the font below reads the asset and digests it, which this call
    // site repeats once per frame. The atlas already held was produced from the
    // same bound loader, so at an unchanged draw size it is still the right one.
    if (!force_rebuild &&
        m_impl->m_font_cache &&
        m_impl->m_metric_pixel_height == pixel_height)
    {
        return;
    }

    auto cached = load_or_build_font_cache(
        m_impl->m_asset_loader,
        pixel_height,
        force_rebuild,
        m_impl->m_log_error,
        m_impl->m_log_debug_info);
    if (!cached) {
        return;
    }

    m_impl->m_font_cache          = std::move(cached);
    m_impl->m_metric_pixel_height = pixel_height;
}

float Font_renderer::measure_text_px(const char* text) const
{
    const msdf_atlas_t* atlas = m_impl->current_atlas();
    if (!text || !atlas) {
        return 0.0f;
    }
    return vnm::msdf_text::measure_text_advance_px(
        *atlas, m_impl->current_draw_pixel_height(), text);
}

bool Font_renderer::text_visual_bounds_px(
    const char*    text,
    float          x,
    float          y,
    glm::vec4&     bounds) const
{
    const msdf_atlas_t* atlas = m_impl->current_atlas();
    if (!text || !atlas) {
        return false;
    }

    const vnm::msdf_text::text_bounds_t measured = vnm::msdf_text::measure_text_bounds_px(
        *atlas,
        m_impl->current_draw_pixel_height(),
        text);
    if (!measured.has_visible_glyphs) {
        return false;
    }

    bounds = glm::vec4(
        x + measured.left,
        y + measured.top,
        x + measured.right,
        y + measured.bottom);
    return
        std::isfinite(bounds.x) &&
        std::isfinite(bounds.y) &&
        std::isfinite(bounds.z) &&
        std::isfinite(bounds.w) &&
        bounds.z > bounds.x     &&
        bounds.w > bounds.y;
}

bool Font_renderer::text_ink_bounds_px(
    const char*    text,
    float          x,
    float          y,
    glm::vec4&     bounds) const
{
    const msdf_atlas_t* atlas = m_impl->current_atlas();
    if (!text || !atlas) {
        return false;
    }

    const int    draw_pixel_height = m_impl->current_draw_pixel_height();
    const double draw_scale        = draw_scale_for(*atlas, draw_pixel_height);
    const double reach             = anti_aliasing_reach_px(*atlas);
    bool         has_visible_glyph = false;
    double       left              = 0.0;
    double       top               = 0.0;
    double       right             = 0.0;
    double       bottom            = 0.0;
    static_cast<void>(vnm::msdf_text::for_each_positioned_glyph(
        *atlas,
        draw_pixel_height,
        text,
        0.0f,
        [&](const vnm::msdf_text::positioned_glyph_t& positioned) {
            const vnm::msdf_text::scaled_glyph_t& quad = positioned.glyph;
            const auto outline = atlas->glyphs.find(positioned.codepoint);
            if (quad.plane_left == quad.plane_right ||
                quad.plane_bottom == quad.plane_top ||
                outline == atlas->glyphs.end())
            {
                return;
            }

            // The quad's Y axis points down, so its plane_bottom is the lower
            // of its two Y values and the font's top bound is the lower of the
            // outline's. Widening the outline and clipping it to the quad
            // needs both in one orientation.
            const double quad_top    = std::min(double(quad.plane_bottom), double(quad.plane_top));
            const double quad_bottom = std::max(double(quad.plane_bottom), double(quad.plane_top));
            const double glyph_left   = std::max(
                double(quad.plane_left),
                double(outline->second.bounds_left_units)    * draw_scale - reach);
            const double glyph_right  = std::min(
                double(quad.plane_right),
                double(outline->second.bounds_right_units)   * draw_scale + reach);
            const double glyph_top    = std::max(
                quad_top,
                double(-outline->second.bounds_top_units)    * draw_scale - reach);
            const double glyph_bottom = std::min(
                quad_bottom,
                double(-outline->second.bounds_bottom_units) * draw_scale + reach);

            if (!has_visible_glyph) {
                has_visible_glyph = true;
                left   = double(positioned.pen_x) + glyph_left;
                right  = double(positioned.pen_x) + glyph_right;
                top    = glyph_top;
                bottom = glyph_bottom;
                return;
            }

            left   = std::min(left,   double(positioned.pen_x) + glyph_left);
            right  = std::max(right,  double(positioned.pen_x) + glyph_right);
            top    = std::min(top,    glyph_top);
            bottom = std::max(bottom, glyph_bottom);
        }));
    if (!has_visible_glyph) {
        return false;
    }

    bounds = glm::vec4(
        float(double(x) + left),
        float(double(y) + top),
        float(double(x) + right),
        float(double(y) + bottom));
    return
        std::isfinite(bounds.x) &&
        std::isfinite(bounds.y) &&
        std::isfinite(bounds.z) &&
        std::isfinite(bounds.w) &&
        bounds.z > bounds.x     &&
        bounds.w > bounds.y;
}

std::uint64_t Font_renderer::text_measure_cache_key() const
{
    return m_impl->current_cache_epoch();
}

float Font_renderer::monospace_advance_px() const
{
    const msdf_atlas_t* atlas = m_impl->current_atlas();
    if (!atlas) {
        return 0.f;
    }
    return static_cast<float>(
        atlas->zero_advance_units *
        draw_scale_for(*atlas, m_impl->current_draw_pixel_height()));
}

bool Font_renderer::monospace_advance_is_reliable() const
{
    const msdf_atlas_t* atlas = m_impl->current_atlas();
    return atlas ? atlas->zero_advance_available : false;
}

float Font_renderer::compute_numeric_bottom() const
{
    const msdf_atlas_t* atlas = m_impl->current_atlas();
    if (!atlas) {
        return 0.0f;
    }
    static const char* k_sample   = "0123456789-+.,";
    const int          draw_px    = m_impl->current_draw_pixel_height();
    float              max_bottom = -std::numeric_limits<float>::infinity();
    for (const char* p = k_sample; *p; ++p) {
        const auto it = atlas->glyphs.find(static_cast<unsigned char>(*p));
        if (it != atlas->glyphs.end()) {
            const float plane_bottom =
                vnm::msdf_text::scaled_glyph(*atlas, it->second, draw_px).plane_bottom;
            const float neg_bottom = -plane_bottom;
            if (neg_bottom > max_bottom) {
                max_bottom = neg_bottom;
            }
        }
    }
    return std::isfinite(max_bottom) ? max_bottom : 0.0f;
}

float Font_renderer::baseline_offset_px() const
{
    const msdf_atlas_t* atlas = m_impl->current_atlas();
    if (!atlas) {
        return 0.f;
    }
    return -vnm::msdf_text::scaled_font_metrics(
        *atlas, m_impl->current_draw_pixel_height()).descender;
}

void Font_renderer::batch_text(float x, float y, const char* text)
{
    const auto* cached = m_impl->m_font_cache.get();
    if (!m_impl->m_rhi_batch_active || !cached) {
        return;
    }
    add_text_to_vectors(
        text,
        x,
        y,
        cached->draw_pixel_height,
        cached->atlas,
        m_impl->m_rhi_vertex_data,
        m_impl->m_rhi_index_data);
}

void Font_renderer::rhi_begin_frame()
{
    m_impl->m_rhi_batch_active = true;
    m_impl->m_rhi_vertex_data.clear();
    m_impl->m_rhi_index_data.clear();
    m_impl->m_rhi_frame_vertex_data.clear();
    m_impl->m_rhi_frame_index_data.clear();
    m_impl->m_rhi.ops.clear();
    m_impl->m_rhi.call_used     = 0;
    m_impl->m_rhi.record_cursor = 0;
}

void Font_renderer::rhi_queue_draw(
    const frame_context_t& ctx,
    const glm::mat4&       pmv,
    const glm::vec4&       color,
    const text_scissor_t&  scissor,
    const text_shadow_t&   shadow)
{
    rhi_queue_draw(ctx, pmv, color, scissor, shadow, {});
}

void Font_renderer::rhi_queue_draw(
    const frame_context_t& ctx,
    const glm::mat4&       pmv,
    const glm::vec4&       color,
    const text_scissor_t&  scissor,
    const text_shadow_t&   shadow,
    const text_lcd_t&      lcd)
{
    if (!ctx.rhi || !ctx.rhi_updates || !ctx.render_target || !m_impl->m_font_cache) {
        m_impl->m_rhi_vertex_data.clear();
        m_impl->m_rhi_index_data.clear();
        return;
    }
    if (m_impl->m_rhi_index_data.empty() || m_impl->m_rhi_vertex_data.empty()) {
        return;
    }

    const std::size_t vertex_start_float_count =
        m_impl->m_rhi_frame_vertex_data.size();
    if (vertex_start_float_count % k_text_vertex_float_count         != 0u ||
        m_impl->m_rhi_vertex_data.size() % k_text_vertex_float_count != 0u)
    {
        m_impl->m_rhi_vertex_data.clear();
        m_impl->m_rhi_index_data.clear();
        return;
    }

    quint32 index_start = 0;
    quint32 base_vertex = 0;
    quint32 index_count = 0;
    if (!detail::to_qrhi_count( m_impl->m_rhi_frame_index_data.size(), index_start) ||
        !detail::to_qrhi_count(
            vertex_start_float_count / k_text_vertex_float_count, base_vertex)      ||
        !detail::to_qrhi_count(m_impl->m_rhi_index_data.size(), index_count))
    {
        m_impl->m_rhi_vertex_data.clear();
        m_impl->m_rhi_index_data.clear();
        return;
    }

    std::size_t new_vertex_float_count = 0;
    std::size_t new_index_count        = 0;
    std::size_t queued_vertex_count    = 0;
    quint32     checked_qrhi_bytes     = 0;
    if (!detail::checked_size_add(
            m_impl->m_rhi_frame_vertex_data.size(),
            m_impl->m_rhi_vertex_data.size(), new_vertex_float_count)                          ||
        !detail::checked_size_add(
            m_impl->m_rhi_frame_index_data.size(),
            m_impl->m_rhi_index_data.size(), new_index_count)                                  ||
        !detail::checked_size_add(
            vertex_start_float_count / k_text_vertex_float_count,
            m_impl->m_rhi_vertex_data.size() / k_text_vertex_float_count, queued_vertex_count) ||
        !detail::to_qrhi_count(queued_vertex_count, checked_qrhi_bytes)                        ||
        !detail::qrhi_byte_size( new_vertex_float_count, sizeof(float), checked_qrhi_bytes)    ||
        !detail::qrhi_byte_size( new_index_count, sizeof(std::uint32_t), checked_qrhi_bytes))
    {
        m_impl->m_rhi_vertex_data.clear();
        m_impl->m_rhi_index_data.clear();
        return;
    }

    m_impl->m_rhi_frame_vertex_data.insert(
        m_impl->m_rhi_frame_vertex_data.end(),
        m_impl->m_rhi_vertex_data.begin(),
        m_impl->m_rhi_vertex_data.end());
    m_impl->m_rhi_frame_index_data.reserve(new_index_count);
    for (std::uint32_t index : m_impl->m_rhi_index_data) {
        m_impl->m_rhi_frame_index_data.push_back(index + base_vertex);
    }

    auto& rhi_state = m_impl->m_rhi;
    QRhi* rhi       = ctx.rhi;

    QRhiResourceUpdateBatch* updates = ctx.rhi_updates;

    if (rhi_state.last_rhi != rhi) {
        rhi_state = rhi_text_state_t{};
        rhi_state.last_rhi = rhi;
    }

    if (!rhi_state.shaders_loaded) {
        rhi_state.vert           = load_qsb("msdf_text.vert.qsb");
        rhi_state.frag           = load_qsb("msdf_text.frag.qsb");
        rhi_state.shaders_loaded = true;
    }

    const auto& cached = *m_impl->m_font_cache;
    if (!rhi_state.atlas_texture ||
        rhi_state.atlas_size           != cached.atlas.atlas_size ||
        rhi_state.uploaded_cache_epoch != cached.cache_epoch)
    {
        rhi_state.atlas_texture.reset(rhi->newTexture(
            QRhiTexture::RGBA8,
            QSize(cached.atlas.atlas_size, cached.atlas.atlas_size)));
        if (!rhi_state.atlas_texture || !rhi_state.atlas_texture->create()) {
            rhi_state.atlas_texture.reset();
            m_impl->m_rhi_vertex_data.clear();
            m_impl->m_rhi_index_data.clear();
            return;
        }
        QImage image(
            cached.atlas.rgba.data(),
            cached.atlas.atlas_size,
            cached.atlas.atlas_size,
            cached.atlas.atlas_size * 4,
            QImage::Format_RGBA8888);
        updates->uploadTexture(rhi_state.atlas_texture.get(), image);
        rhi_state.atlas_size           = cached.atlas.atlas_size;
        rhi_state.uploaded_cache_epoch = cached.cache_epoch;
        for (auto& call : rhi_state.calls) {
            call.srb.reset();
            call.srb_last_texture = nullptr;
        }
    }

    if (!rhi_state.sampler) {
        rhi_state.sampler.reset(rhi->newSampler(
            QRhiSampler::Linear,
            QRhiSampler::Linear,
            QRhiSampler::None,
            QRhiSampler::ClampToEdge,
            QRhiSampler::ClampToEdge));
        if (!rhi_state.sampler || !rhi_state.sampler->create()) {
            rhi_state.sampler.reset();
            m_impl->m_rhi_vertex_data.clear();
            m_impl->m_rhi_index_data.clear();
            return;
        }
    }

    const auto acquire_call = [&]() -> std::size_t {
        if (rhi_state.call_used == rhi_state.calls.size()) {
            rhi_state.calls.emplace_back();
        }
        const std::size_t call_index = rhi_state.call_used++;
        auto&             call       = rhi_state.calls[call_index];

        if (!call.ubo) {
            call.ubo.reset(rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                k_text_ubo_bytes));
            if (!call.ubo || !call.ubo->create()) {
                call.ubo.reset();
                --rhi_state.call_used;
                return std::numeric_limits<std::size_t>::max();
            }
        }

        if (!call.srb ||
            call.srb_last_ubo     != call.ubo.get()                ||
            call.srb_last_texture != rhi_state.atlas_texture.get() ||
            call.srb_last_sampler != rhi_state.sampler.get())
        {
            call.srb.reset(rhi->newShaderResourceBindings());
            call.srb->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0,
                    QRhiShaderResourceBinding::VertexStage
                        | QRhiShaderResourceBinding::FragmentStage,
                    call.ubo.get(),
                    0,
                    k_text_ubo_bytes),
                QRhiShaderResourceBinding::sampledTexture(
                    1,
                    QRhiShaderResourceBinding::FragmentStage,
                    rhi_state.atlas_texture.get(),
                    rhi_state.sampler.get())
            });
            if (!call.srb->create()) {
                call.srb.reset();
                --rhi_state.call_used;
                return std::numeric_limits<std::size_t>::max();
            }
            call.srb_last_ubo     = call.ubo.get();
            call.srb_last_texture = rhi_state.atlas_texture.get();
            call.srb_last_sampler = rhi_state.sampler.get();
        }

        return call_index;
    };

    const bool has_shadow    = shadow.radius_px > 0.0f && shadow.color.a > 0.0f;
    text_lcd_t effective_lcd = lcd;
    if (has_shadow) {
        effective_lcd.subpixel_order = lcd_subpixel_order_t::NONE;
    }

    const std::size_t first_call_index = acquire_call();
    if (first_call_index == std::numeric_limits<std::size_t>::max()) {
        m_impl->m_rhi_vertex_data.clear();
        m_impl->m_rhi_index_data.clear();
        return;
    }

    std::size_t shadow_call_index     = std::numeric_limits<std::size_t>::max();
    std::size_t foreground_call_index = first_call_index;
    if (has_shadow) {
        shadow_call_index = first_call_index;
        foreground_call_index = acquire_call();
        if (foreground_call_index == std::numeric_limits<std::size_t>::max()) {
            m_impl->m_rhi_vertex_data.clear();
            m_impl->m_rhi_index_data.clear();
            return;
        }
    }

    auto& first_call = rhi_state.calls[first_call_index];

    QRhiRenderPassDescriptor* current_rpd     = ctx.render_target->renderPassDescriptor();
    const int                 current_samples = ctx.render_target->sampleCount();
    if (rhi_state.pipeline &&
        (rhi_state.pipeline_rpd != current_rpd ||
         rhi_state.pipeline_samples != current_samples))
    {
        rhi_state.pipeline.reset();
    }

    if (!rhi_state.pipeline) {
        std::unique_ptr<QRhiShaderResourceBindings> layout_srb(
            rhi->newShaderResourceBindings());
        layout_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage
                    | QRhiShaderResourceBinding::FragmentStage,
                first_call.ubo.get(),
                0,
                k_text_ubo_bytes),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                rhi_state.atlas_texture.get(),
                rhi_state.sampler.get())
        });
        if (!layout_srb->create()) {
            m_impl->m_rhi_vertex_data.clear();
            m_impl->m_rhi_index_data.clear();
            return;
        }

        QRhiVertexInputLayout vlayout;
        QRhiVertexInputBinding binding(
            static_cast<quint32>(sizeof(rhi_text_vertex_t)));
        QRhiVertexInputAttribute position(
            0, 0, QRhiVertexInputAttribute::Float2,
            static_cast<quint32>(offsetof(rhi_text_vertex_t, x)));
        QRhiVertexInputAttribute tex_bounds(
            0, 1, QRhiVertexInputAttribute::Float4,
            static_cast<quint32>(offsetof(rhi_text_vertex_t, s_min)));
        QRhiVertexInputAttribute frame_rect(
            0, 2, QRhiVertexInputAttribute::Float4,
            static_cast<quint32>(offsetof(rhi_text_vertex_t, frame_x)));
        vlayout.setBindings({binding});
        vlayout.setAttributes({position, tex_bounds, frame_rect});

        rhi_state.pipeline.reset(rhi->newGraphicsPipeline());
        rhi_state.pipeline->setShaderStages({
            { QRhiShaderStage::Vertex, rhi_state.vert },
            { QRhiShaderStage::Fragment, rhi_state.frag }
        });
        rhi_state.pipeline->setVertexInputLayout(vlayout);
        rhi_state.pipeline->setShaderResourceBindings(layout_srb.get());
        rhi_state.pipeline->setTopology(QRhiGraphicsPipeline::Triangles);

        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable   = true;
        blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        rhi_state.pipeline->setTargetBlends({blend});
        rhi_state.pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
        rhi_state.pipeline->setRenderPassDescriptor(current_rpd);
        rhi_state.pipeline->setSampleCount(current_samples);

        if (!rhi_state.pipeline->create()) {
            rhi_state.pipeline.reset();
            m_impl->m_rhi_vertex_data.clear();
            m_impl->m_rhi_index_data.clear();
            return;
        }
        rhi_state.pipeline_rpd     = current_rpd;
        rhi_state.pipeline_samples = current_samples;
    }

    const auto queue_text_pass = [&](std::size_t call_index,
        const glm::vec4& draw_color,
        const text_shadow_t& draw_shadow,
        rhi_text_pass_t pass) {
        auto& call = rhi_state.calls[call_index];

        Text_block_std140 block{};
        std::memcpy(block.pmv, glm::value_ptr(pmv), sizeof(block.pmv));
        block.color[0]        = draw_color.r;
        block.color[1]        = draw_color.g;
        block.color[2]        = draw_color.b;
        block.color[3]        = draw_color.a;
        block.shadow_color[0] = draw_shadow.color.r;
        block.shadow_color[1] = draw_shadow.color.g;
        block.shadow_color[2] = draw_shadow.color.b;
        block.shadow_color[3] = draw_shadow.color.a;
        block.px_range        = vnm::msdf_text::px_range_for_pixel_height(
            cached.atlas, cached.draw_pixel_height);
        block.target_width    = static_cast<float>(std::max(1, ctx.win_w));
        block.target_height   = static_cast<float>(std::max(1, ctx.win_h));
        block.shadow_radius   = draw_shadow.radius_px;
        block.lcd_subpixel_order =
            vnm::msdf_text::lcd::shader_uniform_value(effective_lcd.subpixel_order);
        block.framebuffer_y_up =
            (ctx.rhi && ctx.rhi->isYUpInFramebuffer()) ? 1 : 0;
        block.background_color[0] = effective_lcd.background_color.r;
        block.background_color[1] = effective_lcd.background_color.g;
        block.background_color[2] = effective_lcd.background_color.b;
        block.background_color[3] = effective_lcd.background_color.a;
        updates->updateDynamicBuffer(call.ubo.get(), 0, sizeof(block), &block);

        rhi_text_draw_op_t op{};
        op.call_index  = call_index;
        op.index_start = index_start;
        op.index_count = index_count;
        op.scissor     = scissor;
        op.pass        = pass;
        rhi_state.ops.push_back(op);
    };

    if (has_shadow) {
        glm::vec4 transparent_text = color;
        transparent_text.a         = 0.0f;
        queue_text_pass(
            shadow_call_index,
            transparent_text,
            shadow,
            rhi_text_pass_t::SHADOW);
    }

    queue_text_pass(
        foreground_call_index,
        color,
        text_shadow_t{},
        rhi_text_pass_t::FOREGROUND);

    m_impl->m_rhi_vertex_data.clear();
    m_impl->m_rhi_index_data.clear();
}

void Font_renderer::rhi_finalize_frame(const frame_context_t& ctx)
{
    auto& rhi_state = m_impl->m_rhi;
    if (!ctx.rhi                                || !ctx.rhi_updates ||
        m_impl->m_rhi_frame_vertex_data.empty() || m_impl->m_rhi_frame_index_data.empty())
    {
        return;
    }

    QRhi* rhi = ctx.rhi;
    QRhiResourceUpdateBatch* updates = ctx.rhi_updates;

    std::size_t vertex_bytes      = 0;
    std::size_t index_bytes       = 0;
    quint32     qrhi_vertex_bytes = 0;
    quint32     qrhi_index_bytes  = 0;
    if (!detail::qrhi_byte_size(
            m_impl->m_rhi_frame_vertex_data.size(),
            sizeof(float), vertex_bytes, qrhi_vertex_bytes) ||
        !detail::qrhi_byte_size(
            m_impl->m_rhi_frame_index_data.size(),
            sizeof(std::uint32_t), index_bytes, qrhi_index_bytes))
    {
        rhi_reset_frame();
        return;
    }

    if (!rhi_state.vbo || rhi_state.vbo_capacity_bytes < vertex_bytes) {
        std::size_t alloc      = 0;
        quint32     qrhi_alloc = 0;
        if (!detail::qrhi_grown_capacity_bytes(vertex_bytes, alloc, qrhi_alloc)) {
            rhi_reset_frame();
            return;
        }
        rhi_state.vbo.reset(rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::VertexBuffer,
            qrhi_alloc));
        if (!rhi_state.vbo || !rhi_state.vbo->create()) {
            rhi_state.vbo.reset();
            rhi_reset_frame();
            return;
        }
        rhi_state.vbo_capacity_bytes = alloc;
    }

    if (!rhi_state.ibo || rhi_state.ibo_capacity_bytes < index_bytes) {
        std::size_t alloc      = 0;
        quint32     qrhi_alloc = 0;
        if (!detail::qrhi_grown_capacity_bytes(index_bytes, alloc, qrhi_alloc)) {
            rhi_reset_frame();
            return;
        }
        rhi_state.ibo.reset(rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::IndexBuffer,
            qrhi_alloc));
        if (!rhi_state.ibo || !rhi_state.ibo->create()) {
            rhi_state.ibo.reset();
            rhi_reset_frame();
            return;
        }
        rhi_state.ibo_capacity_bytes = alloc;
    }

    updates->updateDynamicBuffer(
        rhi_state.vbo.get(),
        0,
        qrhi_vertex_bytes,
        m_impl->m_rhi_frame_vertex_data.data());
    updates->updateDynamicBuffer(
        rhi_state.ibo.get(),
        0,
        qrhi_index_bytes,
        m_impl->m_rhi_frame_index_data.data());
}

std::size_t Font_renderer::queued_draw_count() const
{
    return m_impl->m_rhi.ops.size();
}

void Font_renderer::rhi_record_draws(const frame_context_t& ctx, std::size_t end)
{
    auto& rhi_state = m_impl->m_rhi;

    // A frame reset after `end` was captured, which rhi_finalize_frame()
    // performs when its upload fails, leaves fewer ops queued than `end` names.
    const std::size_t slice_end = std::min(end, rhi_state.ops.size());
    if (slice_end <= rhi_state.record_cursor ||
        !ctx.cb || !rhi_state.pipeline || !rhi_state.vbo || !rhi_state.ibo)
    {
        return;
    }

    // Other renderers may have bound their own pipeline since the previous slice.
    QRhiCommandBuffer* cb = ctx.cb;
    cb->setGraphicsPipeline(rhi_state.pipeline.get());

    QRhiCommandBuffer::VertexInput vertex_input{rhi_state.vbo.get(), 0u};
    const auto record_pass = [&](rhi_text_pass_t pass) {
        for (std::size_t op_index = rhi_state.record_cursor; op_index < slice_end; ++op_index) {
            const auto& op = rhi_state.ops[op_index];
            if (op.pass        != pass                   ||
                op.call_index  >= rhi_state.calls.size() ||
                op.index_count == 0)
            {
                continue;
            }
            const auto& call = rhi_state.calls[op.call_index];
            if (!call.srb) {
                continue;
            }

            cb->setShaderResources(call.srb.get());
            cb->setVertexInput(
                0,
                1,
                &vertex_input,
                rhi_state.ibo.get(),
                0,
                QRhiCommandBuffer::IndexUInt32);
            if (op.scissor.enabled) {
                cb->setScissor(QRhiScissor(
                    op.scissor.x,
                    op.scissor.y,
                    op.scissor.width,
                    op.scissor.height));
            }
            else {
                cb->setScissor(QRhiScissor(0, 0, ctx.win_w, ctx.win_h));
            }
            cb->drawIndexed(op.index_count, 1, op.index_start, 0, 0);
        }
    };

    record_pass(rhi_text_pass_t::SHADOW);
    record_pass(rhi_text_pass_t::FOREGROUND);
    rhi_state.record_cursor = slice_end;
}

void Font_renderer::rhi_record_frame(const frame_context_t& ctx)
{
    rhi_record_draws(ctx, queued_draw_count());
    rhi_reset_frame();
}

void Font_renderer::rhi_reset_frame()
{
    m_impl->m_rhi_batch_active = false;
    m_impl->m_rhi_vertex_data.clear();
    m_impl->m_rhi_index_data.clear();
    m_impl->m_rhi_frame_vertex_data.clear();
    m_impl->m_rhi_frame_index_data.clear();
    m_impl->m_rhi.ops.clear();
    m_impl->m_rhi.call_used     = 0;
    m_impl->m_rhi.record_cursor = 0;
}

} // namespace vnm::plot
