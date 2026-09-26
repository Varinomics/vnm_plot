// vnm_plot QRhi helper boundary tests

#include "test_macros.h"

#include "../src/core/rhi_helpers.h"

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace plot = vnm::plot;

namespace {

bool test_to_qrhi_count_accepts_limit_and_rejects_above_limit()
{
    quint32 out = 0;
    const std::size_t max_qrhi =
        static_cast<std::size_t>(std::numeric_limits<quint32>::max());

    TEST_ASSERT(plot::detail::to_qrhi_count(max_qrhi, out),
        "QRhi count helper must accept quint32 max");
    TEST_ASSERT(out == std::numeric_limits<quint32>::max(),
        "QRhi count helper must preserve quint32 max exactly");

    if (std::numeric_limits<std::size_t>::max() > max_qrhi) {
        TEST_ASSERT(!plot::detail::to_qrhi_count(max_qrhi + 1u, out),
            "QRhi count helper must reject values above quint32 max");
    }
    return true;
}

bool test_qrhi_byte_size_rejects_product_above_qrhi_limit()
{
    quint32 out = 0;
    const std::size_t max_qrhi =
        static_cast<std::size_t>(std::numeric_limits<quint32>::max());

    TEST_ASSERT(plot::detail::qrhi_byte_size(max_qrhi / 4u, 4u, out),
        "QRhi byte-size helper must accept a product inside quint32 range");
    TEST_ASSERT(out == (max_qrhi / 4u) * 4u,
        "QRhi byte-size helper must preserve in-range products exactly");

    TEST_ASSERT(!plot::detail::qrhi_byte_size(max_qrhi / 4u + 1u, 4u, out),
        "QRhi byte-size helper must reject products above quint32 max");
    return true;
}

bool test_qrhi_byte_size_rejects_size_t_product_overflow()
{
    quint32 out = 0;
    const std::size_t overflowing_count =
        std::numeric_limits<std::size_t>::max() / 16u + 1u;

    TEST_ASSERT(!plot::detail::qrhi_byte_size(overflowing_count, 16u, out),
        "QRhi byte-size helper must reject size_t multiplication overflow");
    return true;
}

bool test_qrhi_grown_capacity_bytes_checks_headroom_overflow()
{
    std::size_t capacity      = 0;
    quint32     qrhi_capacity = 0;
    const std::size_t max_qrhi =
        static_cast<std::size_t>(std::numeric_limits<quint32>::max());

    TEST_ASSERT(plot::detail::qrhi_grown_capacity_bytes(
        80u, capacity, qrhi_capacity),
        "QRhi grown-capacity helper must accept ordinary byte counts");
    TEST_ASSERT(capacity == 100u && qrhi_capacity == 100u,
        "QRhi grown-capacity helper must add 25 percent headroom");

    TEST_ASSERT(plot::detail::qrhi_grown_capacity_bytes(
        max_qrhi, capacity, qrhi_capacity),
        "QRhi grown-capacity helper must accept a representable full-size buffer");
    TEST_ASSERT(capacity == max_qrhi &&
        qrhi_capacity == std::numeric_limits<quint32>::max(),
        "QRhi grown-capacity helper must cap headroom at quint32 max");

    if (std::numeric_limits<std::size_t>::max() > max_qrhi) {
        TEST_ASSERT(!plot::detail::qrhi_grown_capacity_bytes(
            max_qrhi + 1u, capacity, qrhi_capacity),
            "QRhi grown-capacity helper must reject byte counts above quint32 max");
    }
    return true;
}

bool test_qrhi_buffer_offset_checks_scaled_offsets()
{
    quint32 offset = 0;
    const std::size_t max_qrhi =
        static_cast<std::size_t>(std::numeric_limits<quint32>::max());

    TEST_ASSERT(plot::detail::qrhi_buffer_offset(3u, 16u, offset),
        "QRhi buffer-offset helper must accept ordinary scaled offsets");
    TEST_ASSERT(offset == 48u,
        "QRhi buffer-offset helper must preserve ordinary scaled offsets");

    TEST_ASSERT(!plot::detail::qrhi_buffer_offset(max_qrhi / 16u + 1u, 16u, offset),
        "QRhi buffer-offset helper must reject scaled offsets above quint32 max");
    return true;
}

bool test_view_seconds_subtracts_before_floating_conversion()
{
    constexpr std::int64_t k_epoch = 1'750'000'000'000'000'000LL;
    TEST_ASSERT(std::abs(plot::detail::to_view_seconds(k_epoch + 1, k_epoch) - 1.0e-9f) < 1.0e-15f,
        "GPU-relative time should preserve an adjacent current-epoch nanosecond");
    TEST_ASSERT(std::abs(plot::detail::to_view_seconds(k_epoch - 1, k_epoch) + 1.0e-9f) < 1.0e-15f,
        "GPU-relative time should preserve a negative adjacent epoch offset");
    TEST_ASSERT(plot::detail::to_view_seconds(
        std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::min()) > 0.0f,
        "GPU-relative time should avoid overflow across the full int64 range");
    return true;
}

bool test_embedded_shaders_retain_required_glsl_profiles()
{
    std::vector<const char*> shaders = {
        "generic_rect.vert.qsb",
        "generic_rect.frag.qsb",
        "grid_quad.vert.qsb",
        "grid_quad.frag.qsb",
        "plot_line.vert.qsb",
        "plot_line.frag.qsb",
        "plot_dot_quad.vert.qsb",
        "plot_dot_quad.frag.qsb",
        "plot_area.vert.qsb",
        "plot_area.frag.qsb",
    };

    for (const char* path : shaders) {
        const QShader shader = plot::detail::load_qsb(path);
        TEST_ASSERT(shader.isValid(), "embedded QSB shader must deserialize");
        bool has_glsl_es_300 = false;
        bool has_glsl_330 = false;
        bool has_glsl_410 = false;
        for (const QShaderKey& key : shader.availableShaders()) {
            if (key.source() != QShader::GlslShader) {
                continue;
            }
            const auto version = key.sourceVersion();
            has_glsl_es_300 = has_glsl_es_300 ||
                              (version.version() == 300 &&
                               version.flags().testFlag(QShaderVersion::GlslEs));
            has_glsl_330 = has_glsl_330 || version.version() == 330;
            has_glsl_410 = has_glsl_410 || version.version() == 410;
        }
        TEST_ASSERT(has_glsl_es_300, "embedded QSB shader must retain GLSL ES 300");
        TEST_ASSERT(has_glsl_330, "embedded QSB shader must retain desktop GLSL 330");
        TEST_ASSERT(has_glsl_410, "embedded QSB shader must retain desktop GLSL 410");
    }
    return true;
}

} // namespace

int main()
{
    int passed = 0;
    int failed = 0;

    std::cout << "QRhi helper tests" << std::endl;

    RUN_TEST(test_to_qrhi_count_accepts_limit_and_rejects_above_limit);
    RUN_TEST(test_qrhi_byte_size_rejects_product_above_qrhi_limit);
    RUN_TEST(test_qrhi_byte_size_rejects_size_t_product_overflow);
    RUN_TEST(test_qrhi_grown_capacity_bytes_checks_headroom_overflow);
    RUN_TEST(test_qrhi_buffer_offset_checks_scaled_offsets);
    RUN_TEST(test_view_seconds_subtracts_before_floating_conversion);
    RUN_TEST(test_embedded_shaders_retain_required_glsl_profiles);

    std::cout << "Passed: " << passed << ", Failed: " << failed << std::endl;
    return failed == 0 ? 0 : 1;
}
