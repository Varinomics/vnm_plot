#include <vnm_plot/rhi/font_renderer.h>
#include <vnm_plot/core/lcd.h>
#include <vnm_plot/rhi/asset_loader.h>
#include "atomic_file_write.h"
#include "font_atlas_cache.h"

#include <glm/gtc/type_ptr.hpp>
#include <vnm_msdf_text/lcd_contract.h>
#include <vnm_msdf_text/msdf_text.h>
#include <vnm_msdf_text/rhi/text_renderer.h>
#include <QDebug>

#include <QByteArray>
#include <QByteArrayView>
#include <QFile>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>
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
#include <mutex>
#include <sstream>
#include <type_traits>
#include <vector>

namespace vnm::plot {

namespace text_rhi = vnm::msdf_text::rhi;

namespace {

constexpr std::uint32_t k_cache_version       = 5;
constexpr double        k_min_atlas_font_size = 48.0;
constexpr float         k_atlas_px_range      = 10.0f;
// 1.0 is a one-output-pixel anti-aliasing ramp now that vnm_msdf_text encodes
// atlas_px_range in true atlas pixels; 2.5 compensated for the shallow
// font-dependent encode the builder produced before that conversion.
constexpr float k_sharpness_bias     = 1.0f;
constexpr int   k_atlas_texture_size = 2048;

std::atomic<bool> s_disk_cache_enabled{true};
std::mutex s_disk_cache_options_mutex;
font_disk_cache_options_t s_disk_cache_options;

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

void set_font_disk_cache_options(const font_disk_cache_options_t& options)
{
    std::lock_guard<std::mutex> lock(s_disk_cache_options_mutex);
    s_disk_cache_options = options;
}

font_disk_cache_options_t font_disk_cache_options()
{
    std::lock_guard<std::mutex> lock(s_disk_cache_options_mutex);
    return s_disk_cache_options;
}

namespace {

using msdf_atlas_t       = vnm::msdf_text::atlas_t;
using msdf_glyph_t       = vnm::msdf_text::glyph_t;
using msdf_kerning_key_t = vnm::msdf_text::kerning_key_t;
std::atomic<std::uint64_t> s_next_cache_epoch{1};

using detail::cached_font_data_t;
using detail::font_atlas_key_t;

// Every retained entry is one k_atlas_texture_size RGBA bitmap (16 MiB at
// 2048), so this budget keeps the eight most recently used (font, bake height)
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
    add_bytes(vnm::msdf_text::k_font_bake_compatibility_version);
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

std::filesystem::path resolve_cache_directory(const font_disk_cache_options_t& options)
{
    auto cache_dir = options.directory;
    if (cache_dir.empty()) {
        const auto base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (base.isEmpty()) {
            return {};
        }
        cache_dir = std::filesystem::path(base.toStdU16String()) / "vnm_plot";
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    return ec ? std::filesystem::path{} : cache_dir;
}

void prune_disk_cache(const std::filesystem::path& path, std::uint64_t max_bytes)
{
    // Restrict cleanup to our complete file-name grammar, so a host may share
    // its cache directory with other artifacts. Never follow symbolic links.
    static const QRegularExpression cache_name(
        QStringLiteral("^msdf_cache_v[0-9]+_px[0-9]+_font[0-9a-f]{64}\\.bin(\\.tmp-[0-9a-f]+-[0-9a-f]+)?$"));
    const QDir directory(path);
    const auto files = directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Time | QDir::Reversed);
    const auto stale_before = QDateTime::currentDateTimeUtc().addDays(-1);
    std::vector<QFileInfo> completed;
    std::uint64_t total_bytes = 0;
    for (const auto& file : files) {
        const auto match = cache_name.match(file.fileName());
        if (!match.hasMatch()) {
            continue;
        }
        if (!match.captured(1).isEmpty()) {
            if (file.lastModified() < stale_before) {
                QFile::remove(file.absoluteFilePath());
            }
            continue;
        }
        total_bytes += static_cast<std::uint64_t>(file.size());
        completed.push_back(file);
    }
    for (const auto& file : completed) {
        if (total_bytes <= max_bytes) {
            break;
        }
        if (QFile::remove(file.absoluteFilePath())) {
            total_bytes -= static_cast<std::uint64_t>(file.size());
        }
    }
}

std::filesystem::path cache_file_path(
    const std::filesystem::path&           cache_dir,
    int                                    pixel_height,
    const std::array<std::uint8_t, 32>&    font_digest)
{
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

    vnm::msdf_text::build_result_t build;
    auto font = std::make_shared<cached_font_data_t>();
    font->font_digest       = digest;

    std::uint32_t atlas_size = 0;
    if (!read(atlas_size))                                              { return nullptr; }
    if (atlas_size != static_cast<std::uint32_t>(k_atlas_texture_size)) { return nullptr; }
    build.atlas.atlas_size = static_cast<int>(atlas_size);

    std::uint32_t baked_pixel_height = 0;
    if (!read(baked_pixel_height) ||
        !read(build.atlas.atlas_px_range) ||
        !read(build.atlas.bitmap_scale) ||
        !read(build.atlas.sharpness_bias) ||
        !read(build.atlas.font_metrics_units.ascender) ||
        !read(build.atlas.font_metrics_units.descender) ||
        !read(build.atlas.font_metrics_units.line_height) ||
        !read(build.atlas.font_metrics_units.em_size) ||
        !read(build.atlas.zero_advance_units))
    {
        return nullptr;
    }
    if (baked_pixel_height != static_cast<std::uint32_t>(pixel_height)) {
        return nullptr;
    }
    build.atlas.baked_pixel_height = pixel_height;
    // ascender, bitmap_scale, and atlas_px_range are divisors/projections in the
    // scaling helpers, so require them strictly positive; the rest must be finite.
    if (!(build.atlas.atlas_px_range > 0.0) ||
        !(build.atlas.bitmap_scale > 0.0) ||
        !std::isfinite(build.atlas.sharpness_bias) ||
        !std::isfinite(build.atlas.font_metrics_units.ascender) ||
        !(build.atlas.font_metrics_units.ascender > 0.f) ||
        !std::isfinite(build.atlas.font_metrics_units.descender) ||
        !std::isfinite(build.atlas.font_metrics_units.line_height) ||
        !std::isfinite(build.atlas.font_metrics_units.em_size) ||
        !std::isfinite(build.atlas.zero_advance_units))
    {
        return nullptr;
    }
    std::uint8_t zero_available = 0;
    std::uint8_t padding[3]{};
    if (!read(zero_available) || !in.read(reinterpret_cast<char*>(padding), sizeof(padding))) {
        return nullptr;
    }
    build.atlas.zero_advance_available = (zero_available != 0);

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
        build.atlas.glyphs.emplace(static_cast<char32_t>(code), g);
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
        build.atlas.kerning_units.emplace(key, value);
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
    build.atlas.rgba.resize(atlas_bytes);
    if (!build.atlas.rgba.empty()) {
        in.read(reinterpret_cast<char*>(build.atlas.rgba.data()), atlas_bytes);
        if (!in) {
            return nullptr;
        }
    }

    std::uint32_t status = 0;
    std::uint32_t message_size = 0;
    if (!read(status) || status > static_cast<std::uint32_t>(vnm::msdf_text::Build_status::SUCCESS) ||
        !read(message_size) || message_size > 1024u * 1024u)
    {
        return nullptr;
    }
    build.status = static_cast<vnm::msdf_text::Build_status>(status);
    build.message.resize(message_size);
    if (!in.read(build.message.data(), message_size)) {
        return nullptr;
    }
    const auto read_codes = [&](std::vector<char32_t>& codes) {
        std::uint32_t count = 0;
        if (!read(count) || count > glyph_codepoints().size()) {
            return false;
        }
        codes.resize(count);
        for (auto& code : codes) {
            std::uint32_t value = 0;
            if (!read(value)) {
                return false;
            }
            code = static_cast<char32_t>(value);
        }
        return true;
    };
    std::uint8_t atlas_full = 0;
    if (!read_codes(build.invalid_codepoints) || !read_codes(build.missing_codepoints) ||
        !read_codes(build.failed_codepoints) || !read_codes(build.skipped_too_large) ||
        !read_codes(build.skipped_no_space) || !read(atlas_full) || atlas_full > 1)
    {
        return nullptr;
    }
    build.atlas_full = atlas_full != 0;
    const auto adopted = text_rhi::adopt_baked_font(std::move(build));
    if (adopted.result.status != text_rhi::Text_status::OK) {
        return nullptr;
    }
    font->font = adopted.font;
    font->cache_epoch = s_next_cache_epoch.fetch_add(1, std::memory_order_relaxed);
    return font;
}

bool serialize_cached_font(std::ostream& out, const cached_font_data_t& font)
{
    const auto& build = font.font->build_result();
    const auto& atlas = build.atlas;
    auto write = [&](auto val) {
        out.write(reinterpret_cast<const char*>(&val), sizeof(val));
    };

    constexpr std::uint32_t k_magic = 0x4d534446; // 'MSDF'
    write(k_magic);
    write(k_cache_version);
    write(static_cast<std::uint32_t>(atlas.baked_pixel_height));
    out.write(reinterpret_cast<const char*>(font.font_digest.data()), font.font_digest.size());
    write(static_cast<std::uint32_t>(atlas.atlas_size));
    write(static_cast<std::uint32_t>(atlas.baked_pixel_height));
    write(atlas.atlas_px_range);
    write(atlas.bitmap_scale);
    write(atlas.sharpness_bias);
    write(atlas.font_metrics_units.ascender);
    write(atlas.font_metrics_units.descender);
    write(atlas.font_metrics_units.line_height);
    write(atlas.font_metrics_units.em_size);
    write(atlas.zero_advance_units);
    std::uint8_t zero_available = atlas.zero_advance_available ? 1u : 0u;
    out.write(reinterpret_cast<const char*>(&zero_available), sizeof(zero_available));
    std::uint8_t padding[3]{0, 0, 0};
    out.write(reinterpret_cast<const char*>(padding), sizeof(padding));

    write(static_cast<std::uint32_t>(atlas.glyphs.size()));
    for (const auto& [code, g] : atlas.glyphs) {
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

    write(static_cast<std::uint32_t>(atlas.kerning_units.size()));
    for (const auto& [key, value] : atlas.kerning_units) {
        write(key);
        write(value);
    }

    write(static_cast<std::uint32_t>(atlas.rgba.size()));
    if (!atlas.rgba.empty()) {
        out.write(
            reinterpret_cast<const char*>(atlas.rgba.data()),
            static_cast<std::streamsize>(atlas.rgba.size()));
    }

    write(static_cast<std::uint32_t>(build.status));
    write(static_cast<std::uint32_t>(build.message.size()));
    out.write(build.message.data(), static_cast<std::streamsize>(build.message.size()));
    const auto write_codes = [&](const std::vector<char32_t>& codes) {
        write(static_cast<std::uint32_t>(codes.size()));
        for (char32_t code : codes) {
            write(static_cast<std::uint32_t>(code));
        }
    };
    write_codes(build.invalid_codepoints);
    write_codes(build.missing_codepoints);
    write_codes(build.failed_codepoints);
    write_codes(build.skipped_too_large);
    write_codes(build.skipped_no_space);
    write(static_cast<std::uint8_t>(build.atlas_full));

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

    const auto adopted = text_rhi::adopt_baked_font(std::move(result));
    if (adopted.result.status != text_rhi::Text_status::OK) {
        if (log_error) {
            log_error(adopted.result.diagnostic.data());
        }
        return nullptr;
    }
    font->font = adopted.font;
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
    // Normalize only valid draw sizes; otherwise the bake floor would turn an
    // invalid height into a successful 48px atlas request.
    if (pixel_height <= 0) {
        if (log_error) {
            log_error("Font pixel height must be positive");
        }
        return nullptr;
    }
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

    const int bake_height = vnm::msdf_text::msdf_bake_pixel_height(pixel_height, atlas_options());
    const font_atlas_key_t key{compute_font_digest(*font_bytes), bake_height};
    const auto options = font_disk_cache_options();
    const bool disk_cache = font_disk_cache_enabled() && options.max_bytes > 0;

    return font_atlas_cache().get_or_build(
        key,
        force_rebuild,
        [&]() -> std::shared_ptr<cached_font_data_t> {
            const auto directory = disk_cache ? resolve_cache_directory(options) : std::filesystem::path{};
            const auto path = directory.empty()
                ? std::filesystem::path{}
                : cache_file_path(directory, bake_height, key.font_digest);
            if (disk_cache && path.empty() && log_error) {
                log_error("Failed to create MSDF font cache directory");
            }
            // A forced rebuild has to regenerate the atlas, so it bypasses the
            // disk cache as well as the memo; reading the file back would hand
            // out the same bytes through another door.
            if (!path.empty() && !force_rebuild) {
                auto from_disk = load_cached_font_from_disk(
                    path,
                    key.font_digest,
                    bake_height);
                if (from_disk) {
                    std::error_code ec;
                    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
                    prune_disk_cache(directory, options.max_bytes);
                    return from_disk;
                }
            }

            auto built = build_font_cache(
                *font_bytes,
                bake_height,
                key.font_digest,
                log_error,
                log_debug_info);
            if (built && !path.empty()) {
                if (!save_cached_font_to_disk(path, *built) && log_error) {
                    log_error("Failed to publish MSDF font cache " + path.string());
                }
                prune_disk_cache(directory, options.max_bytes);
            }
            return built;
        });
}

} // anonymous namespace

#if defined(VNM_PLOT_ENABLE_TEST_HOOKS)
namespace detail {

void prune_font_disk_cache_directory(const std::filesystem::path& path, std::uint64_t max_bytes)
{
    prune_disk_cache(path, max_bytes);
}

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
    std::shared_ptr<const text_rhi::Font_snapshot> m_snapshot;
    text_rhi::Text_batch                        m_batch;
    text_rhi::Text_renderer                     m_renderer;

    bool check(const text_rhi::text_result_t& result)
    {
        if (result.status == text_rhi::Text_status::OK) {
            return true;
        }
        if (m_log_error) {
            m_log_error(result.diagnostic.data());
        }
        else {
            qWarning("vnm_plot text: %s", result.diagnostic.data());
        }
        return false;
    }

    void clear_batch()
    {
        m_batch.clear();
        check(m_batch.enable_glyph_frames());
    }

    // m_font_cache is a shared_ptr to an immutable atlas, so the accessors below
    // read it without a copy and two renderers on one thread never alias each
    // other's font: each holds its own reference to the entry its own asset
    // loader and draw height selected.
    const msdf_atlas_t* current_atlas() const
    {
        return m_font_cache ? &m_font_cache->font->atlas() : nullptr;
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

    const auto snapshot = text_rhi::make_font_snapshot(cached->font, pixel_height);
    if (!m_impl->check(snapshot.result)) {
        return;
    }
    m_impl->m_snapshot = snapshot.snapshot;
    m_impl->m_renderer.set_font(snapshot.snapshot);
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
    const auto epoch = m_impl->current_cache_epoch();
    // The GPU texture follows the atlas epoch; measurement caches also need
    // the draw size even when the same immutable atlas serves both sizes.
    return epoch == 0 ? 0 :
        (epoch * 0x9e3779b97f4a7c15ULL) ^ static_cast<std::uint64_t>(m_impl->current_draw_pixel_height());
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

namespace {

text_rhi::frame_t text_frame(const frame_context_t& ctx)
{
    return {ctx.rhi, ctx.cb, ctx.render_target, ctx.rhi_updates};
}

} // namespace

void Font_renderer::batch_text(float x, float y, const char* text)
{
    if (!m_impl->m_rhi_batch_active || !m_impl->m_snapshot || !text) {
        return;
    }
    m_impl->check(m_impl->m_batch.append_run(*m_impl->m_snapshot, text, x, y));
}

void Font_renderer::rhi_begin_frame()
{
    m_impl->m_rhi_batch_active = true;
    m_impl->m_renderer.begin_frame();
    m_impl->clear_batch();
}

void Font_renderer::rhi_queue_draw(
    const frame_context_t& ctx,
    const glm::vec4&       color,
    const text_scissor_t&  scissor,
    const text_shadow_t&   shadow)
{
    rhi_queue_draw(ctx, color, scissor, shadow, {});
}

void Font_renderer::rhi_queue_draw(
    const frame_context_t& ctx,
    const glm::vec4&       color,
    const text_scissor_t&  scissor,
    const text_shadow_t&   shadow,
    const text_lcd_t&      lcd)
{
    text_rhi::draw_state_t state;
    state.transform = text_rhi::pixel_ortho_transform(text_frame(ctx));
    std::copy_n(glm::value_ptr(color), 4, state.color.begin());
    state.clip = {scissor.enabled, scissor.x, scissor.y, scissor.width, scissor.height};
    state.sdf_mask = text_rhi::sdf_mask_t{};
    if (shadow.radius_px > 0.0f && shadow.color.a > 0.0f) {
        state.glow = text_rhi::glow_style_t{};
        std::copy_n(glm::value_ptr(shadow.color), 4, state.glow->color.begin());
        state.glow->radius_px = shadow.radius_px;
    }
    if (lcd.subpixel_order != lcd_subpixel_order_t::NONE) {
        state.lcd = text_rhi::lcd_style_t{};
        state.lcd->order = lcd.subpixel_order;
        std::copy_n(glm::value_ptr(lcd.background_color), 4, state.lcd->background_color.begin());
    }
    m_impl->check(m_impl->m_renderer.queue(m_impl->m_batch, state));
    m_impl->clear_batch();
}

void Font_renderer::rhi_finalize_frame(const frame_context_t& ctx)
{
    m_impl->check(m_impl->m_renderer.prepare(text_frame(ctx)));
}

std::size_t Font_renderer::queued_draw_count() const
{
    return m_impl->m_renderer.queued_draw_count();
}

void Font_renderer::rhi_record_draws(const frame_context_t& ctx, std::size_t end)
{
    m_impl->check(m_impl->m_renderer.record_draws(
        text_frame(ctx), end, text_rhi::grouped_shadows_t{}));
}

void Font_renderer::rhi_record_frame(const frame_context_t& ctx)
{
    rhi_record_draws(ctx, queued_draw_count());
    rhi_reset_frame();
}

void Font_renderer::rhi_reset_frame()
{
    m_impl->m_rhi_batch_active = false;
    m_impl->m_batch.clear();
    m_impl->m_renderer.reset_frame();
}

} // namespace vnm::plot
