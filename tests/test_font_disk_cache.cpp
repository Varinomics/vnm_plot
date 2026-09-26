// vnm_plot font disk cache validation tests

#include "test_macros.h"

#include <vnm_plot/rhi/font_renderer.h>
#include <vnm_plot/rhi/asset_loader.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>

namespace plot = vnm::plot;

namespace {

constexpr std::uint32_t k_magic                  = 0x4d534446; // 'MSDF'
constexpr std::uint32_t k_cache_version          = 4;
constexpr std::uint32_t k_previous_cache_version = 3;
constexpr std::uint32_t k_pixel_height           = 18;
constexpr std::uint32_t k_atlas_texture_size     = 2048;
constexpr std::uint32_t k_expected_atlas_bytes   =
    k_atlas_texture_size * k_atlas_texture_size * 4u;

using digest_t = plot::detail::font_disk_cache_digest_t;

struct Scoped_temp_dir
{
    std::filesystem::path path;

    Scoped_temp_dir()
    {
        path = std::filesystem::temp_directory_path() /
               ("vnm_plot_font_cache_test_" +
                std::to_string(std::hash<const void*>{}(this)));
        std::filesystem::create_directories(path);
    }

    ~Scoped_temp_dir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    Scoped_temp_dir(const Scoped_temp_dir&)            = delete;
    Scoped_temp_dir& operator=(const Scoped_temp_dir&) = delete;
};

struct Scoped_cache_options
{
    plot::font_disk_cache_options_t previous_options = plot::font_disk_cache_options();
    bool previous_enabled = plot::font_disk_cache_enabled();

    ~Scoped_cache_options()
    {
        plot::set_font_disk_cache_options(previous_options);
        plot::set_font_disk_cache_enabled(previous_enabled);
    }
};

std::string owned_cache_name(int version, int height)
{
    return "msdf_cache_v" + std::to_string(version) + "_px" + std::to_string(height) +
           "_font" + std::string(64, 'a') + ".bin";
}

bool test_cache_prunes_owned_files_and_stale_temporaries()
{
    Scoped_temp_dir tmp;
    const auto oldest = tmp.path / owned_cache_name(3, 16);
    const auto newest = tmp.path / owned_cache_name(4, 18);
    const auto stale_temporary = tmp.path / (owned_cache_name(4, 20) + ".tmp-a-1");
    const auto fresh_temporary = tmp.path / (owned_cache_name(4, 22) + ".tmp-b-2");
    const auto unrelated = tmp.path / "msdf_cache_notes.bin";
    for (const auto& path : {oldest, newest, stale_temporary, fresh_temporary, unrelated}) {
        std::ofstream out(path, std::ios::binary);
        out << std::string(100, 'x');
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(oldest, now - std::chrono::hours(2));
    std::filesystem::last_write_time(stale_temporary, now - std::chrono::hours(48));

    plot::detail::prune_font_disk_cache_directory(tmp.path, 100);
    TEST_ASSERT(!std::filesystem::exists(oldest), "the least recently used owned cache must be evicted");
    TEST_ASSERT(std::filesystem::exists(newest), "the most recently used cache must fit within the budget");
    TEST_ASSERT(!std::filesystem::exists(stale_temporary), "abandoned atomic-write files must be removed");
    TEST_ASSERT(std::filesystem::exists(fresh_temporary), "a concurrent writer's temporary must survive");
    TEST_ASSERT(std::filesystem::exists(unrelated), "host-owned files must never be pruned");

    plot::detail::prune_font_disk_cache_directory(tmp.path, 0);
    TEST_ASSERT(!std::filesystem::exists(newest), "a zero budget must remove every completed owned file");
    return true;
}

bool test_renderer_publishes_within_host_directory_and_budget()
{
    Scoped_temp_dir tmp;
    Scoped_cache_options restore;
    plot::font_disk_cache_options_t options;
    options.directory = tmp.path / "profile" / "font-cache";
    options.max_bytes = 20u * 1024u * 1024u;
    plot::set_font_disk_cache_options(options);
    plot::set_font_disk_cache_enabled(true);
    std::filesystem::create_directories(options.directory);
    const auto superseded = options.directory / owned_cache_name(3, 18);
    {
        std::ofstream out(superseded, std::ios::binary);
        out << 'x';
    }
    std::filesystem::resize_file(superseded, options.max_bytes);
    std::filesystem::last_write_time(
        superseded, std::filesystem::file_time_type::clock::now() - std::chrono::hours(2));

    plot::Asset_loader loader;
    plot::init_embedded_assets(loader);
    plot::Font_renderer renderer(loader);
    renderer.initialize_metrics(k_pixel_height, true);
    TEST_ASSERT(renderer.measure_text_px("Profile cache") > 0.f, "the renderer must build a usable atlas");
    TEST_ASSERT(!std::filesystem::exists(superseded), "publishing must evict superseded cache files");
    std::uint64_t bytes = 0;
    std::size_t files = 0;
    for (const auto& file : std::filesystem::directory_iterator(options.directory)) {
        bytes += file.file_size();
        ++files;
    }
    TEST_ASSERT(files == 1, "the configured profile directory must receive the atlas");
    TEST_ASSERT(bytes > k_expected_atlas_bytes && bytes <= options.max_bytes,
        "the atlas and its metrics must fit within the configured byte budget");
    return true;
}

struct cache_file_options_t
{
    digest_t       digest{};
    std::uint32_t  cache_version       = k_cache_version;
    std::uint32_t  pixel_height        = k_pixel_height;
    std::uint32_t  atlas_size          = k_atlas_texture_size;
    std::uint32_t  glyph_count         = 0;
    std::uint32_t  kerning_count       = 0;
    std::uint32_t  atlas_bytes         = k_expected_atlas_bytes;
    bool           write_atlas_payload = false;
};

digest_t make_digest(std::uint8_t seed)
{
    digest_t digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        digest[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
    }
    return digest;
}

template<typename T>
void write_value(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_zero_bytes(std::ofstream& out, std::uint32_t byte_count)
{
    std::array<char, 4096> zeros{};
    std::uint32_t remaining = byte_count;
    while (remaining > 0u) {
        const auto chunk = std::min<std::uint32_t>(
            remaining,
            static_cast<std::uint32_t>(zeros.size()));
        out.write(zeros.data(), static_cast<std::streamsize>(chunk));
        remaining -= chunk;
    }
}

void write_valid_glyph(std::ofstream& out)
{
    // Scale-independent geometry in font units: bounds_right >= bounds_left and
    // bounds_top >= bounds_bottom, with the visibility flag last (matches the
    // renderer's on-disk glyph layout).
    const std::uint32_t codepoint           = static_cast<std::uint32_t>('A');
    const float         advance_units       = 10.0f;
    const float         bounds_left_units   = 0.0f;
    const float         bounds_bottom_units = 0.0f;
    const float         bounds_right_units  = 1.0f;
    const float         bounds_top_units    = 1.0f;
    const float         uv_left             = 0.0f;
    const float         uv_bottom           = 1.0f;
    const float         uv_right            = 1.0f;
    const float         uv_top              = 0.0f;
    const std::uint8_t  visible             = 1u;

    write_value(out, codepoint);
    write_value(out, advance_units);
    write_value(out, bounds_left_units);
    write_value(out, bounds_bottom_units);
    write_value(out, bounds_right_units);
    write_value(out, bounds_top_units);
    write_value(out, uv_left);
    write_value(out, uv_bottom);
    write_value(out, uv_right);
    write_value(out, uv_top);
    write_value(out, visible);
}

bool write_cache_file(
    const std::filesystem::path&   path,
    const cache_file_options_t&    options)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    write_value(out, k_magic);
    write_value(out, options.cache_version);
    write_value(out, options.pixel_height);
    out.write(
        reinterpret_cast<const char*>(options.digest.data()),
        static_cast<std::streamsize>(options.digest.size()));
    write_value(out, options.atlas_size);

    // Scale-independent atlas header: font-unit metrics plus the bake-time
    // projection parameters. atlas_px_range, bitmap_scale, and ascender must be
    // strictly positive for the loader to accept the file.
    const std::uint32_t baked_pixel_height = 48u;
    const double        atlas_px_range     = 10.0;
    const double        bitmap_scale       = 1.0;
    const float         sharpness_bias     = 2.5f;
    const float         ascender           = 14.0f;
    const float         descender          = -4.0f;
    const float         line_height        = 20.0f;

    const float em_size            = 18.0f;
    const float zero_advance_units = 9.0f;
    write_value(out, baked_pixel_height);
    write_value(out, atlas_px_range);
    write_value(out, bitmap_scale);
    write_value(out, sharpness_bias);
    write_value(out, ascender);
    write_value(out, descender);
    write_value(out, line_height);
    write_value(out, em_size);
    write_value(out, zero_advance_units);

    const std::uint8_t zero_available = 1u;
    const std::uint8_t padding[3]{0u, 0u, 0u};
    out.write(reinterpret_cast<const char*>(&zero_available), sizeof(zero_available));
    out.write(reinterpret_cast<const char*>(padding),         sizeof(padding));

    write_value(out, options.glyph_count);
    if (options.glyph_count == 1u) {
        write_valid_glyph(out);
    }

    write_value(out, options.kerning_count);
    write_value(out, options.atlas_bytes);
    if (options.write_atlas_payload) {
        write_zero_bytes(out, options.atlas_bytes);
    }

    return bool(out);
}

bool cache_file_is_valid(
    const std::filesystem::path&   path,
    const digest_t&                expected_digest,
    int                            pixel_height = static_cast<int>(k_pixel_height))
{
    return plot::detail::validate_font_disk_cache_file(path, expected_digest, pixel_height);
}

bool test_corrupt_glyph_count_is_rejected()
{
    Scoped_temp_dir tmp;
    const auto digest = make_digest(0x20u);
    cache_file_options_t options;
    options.digest      = digest;
    options.glyph_count = std::numeric_limits<std::uint32_t>::max();

    const auto path = tmp.path / "corrupt_glyph_count.bin";
    TEST_ASSERT(write_cache_file(path, options), "corrupt glyph-count cache should be writable");
    TEST_ASSERT(!cache_file_is_valid(path, digest),
        "glyph count above the validation limit must be rejected");
    return true;
}

bool test_corrupt_kerning_count_is_rejected()
{
    Scoped_temp_dir tmp;
    const auto digest = make_digest(0x30u);
    cache_file_options_t options;
    options.digest        = digest;
    options.glyph_count   = 1u;
    options.kerning_count = 2u;

    const auto path = tmp.path / "corrupt_kerning_count.bin";
    TEST_ASSERT(write_cache_file(path, options), "corrupt kerning-count cache should be writable");
    TEST_ASSERT(!cache_file_is_valid(path, digest),
        "kerning count above glyph-count squared must be rejected");
    return true;
}

bool test_corrupt_atlas_size_and_bytes_are_rejected()
{
    Scoped_temp_dir tmp;
    const auto digest = make_digest(0x40u);

    cache_file_options_t bad_size;
    bad_size.digest     = digest;
    bad_size.atlas_size = k_atlas_texture_size / 2u;
    const auto bad_size_path = tmp.path / "corrupt_atlas_size.bin";
    TEST_ASSERT(write_cache_file(bad_size_path, bad_size),
        "corrupt atlas-size cache should be writable");
    TEST_ASSERT(!cache_file_is_valid(bad_size_path, digest),
        "atlas size different from the renderer texture size must be rejected");

    cache_file_options_t bad_bytes;
    bad_bytes.digest      = digest;
    bad_bytes.atlas_bytes = k_expected_atlas_bytes - 4u;
    const auto bad_bytes_path = tmp.path / "corrupt_atlas_bytes.bin";
    TEST_ASSERT(write_cache_file(bad_bytes_path, bad_bytes),
        "corrupt atlas-byte-count cache should be writable");
    TEST_ASSERT(!cache_file_is_valid(bad_bytes_path, digest),
        "atlas byte count different from width * height * rgba must be rejected");

    return true;
}

bool test_same_height_changed_digest_does_not_reuse_old_cache()
{
    Scoped_temp_dir tmp;
    const auto old_digest     = make_digest(0x50u);
    const auto changed_digest = make_digest(0x90u);

    cache_file_options_t options;
    options.digest              = old_digest;
    options.write_atlas_payload = true;

    const auto path = tmp.path / "old_digest_same_height.bin";
    TEST_ASSERT(write_cache_file(path, options), "valid old-digest cache should be writable");
    TEST_ASSERT(cache_file_is_valid(path, old_digest),
        "test fixture cache should load with the digest it was written with");
    TEST_ASSERT(!cache_file_is_valid(path, changed_digest),
        "same-height cache with an old digest must not satisfy a changed font digest");
    return true;
}

bool test_previous_cache_version_is_rejected()
{
    Scoped_temp_dir tmp;
    const auto digest = make_digest(0x60u);

    cache_file_options_t options;
    options.digest              = digest;
    options.cache_version       = k_previous_cache_version;
    options.write_atlas_payload = true;

    const auto path = tmp.path / "previous_cache_version.bin";
    TEST_ASSERT(write_cache_file(path, options), "previous-version cache should be writable");
    TEST_ASSERT(!cache_file_is_valid(path, digest),
        "cache generated by previous MSDF builder semantics must be rejected");
    return true;
}

} // namespace

int main()
{
    std::cout << "Font disk cache tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_corrupt_glyph_count_is_rejected);
    RUN_TEST(test_corrupt_kerning_count_is_rejected);
    RUN_TEST(test_corrupt_atlas_size_and_bytes_are_rejected);
    RUN_TEST(test_same_height_changed_digest_does_not_reuse_old_cache);
    RUN_TEST(test_previous_cache_version_is_rejected);
    RUN_TEST(test_cache_prunes_owned_files_and_stale_temporaries);
    RUN_TEST(test_renderer_publishes_within_host_directory_and_budget);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
