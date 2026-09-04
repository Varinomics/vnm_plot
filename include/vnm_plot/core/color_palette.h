#pragma once

// VNM Plot Library - Core Color Palette
// Theme colors for light and dark modes.

#include <glm/vec4.hpp>

#include <cstdint>

namespace vnm::plot {

// -----------------------------------------------------------------------------
// Color Utilities
// -----------------------------------------------------------------------------

constexpr int hex_char_to_int(char c)
{
    if (c >= '0' && c <= '9') { return c - '0';      }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return 0;
}

constexpr float hex2f(const char* str)
{
    return static_cast<float>(hex_char_to_int(str[0]) * 16 + hex_char_to_int(str[1])) / 255.0f;
}

// Converts "aarrggbb" hex string to glm::vec4(r, g, b, a)
constexpr glm::vec4 hex_to_vec4(const char* str)
{
    return glm::vec4(hex2f(str + 2), hex2f(str + 4), hex2f(str + 6), hex2f(str));
}

inline glm::vec4 rgba_u8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
{
    constexpr float k_inv_255 = 1.0f / 255.0f;
    return
        glm::vec4(
            static_cast<float>(r) * k_inv_255,
            static_cast<float>(g) * k_inv_255,
            static_cast<float>(b) * k_inv_255,
            static_cast<float>(a) * k_inv_255);
}

// -----------------------------------------------------------------------------
// Color Palette
// -----------------------------------------------------------------------------
// All colors used by the plot renderer, organized by theme.
struct Color_palette
{
    glm::vec4 background              = hex_to_vec4("ff1b1c20");
    glm::vec4 h_label_background      = hex_to_vec4("ff343940");
    glm::vec4 v_label_background      = hex_to_vec4("ff292c31");
    glm::vec4 preview_background      = hex_to_vec4("ff1e1f22");
    glm::vec4 separator               = hex_to_vec4("ff8f9399");
    glm::vec4 grid_line               = hex_to_vec4("ffd5d7da");
    glm::vec4 preview_cover           = hex_to_vec4("26555555");
    glm::vec4 preview_cover_secondary = hex_to_vec4("10505050");
    // Overlay tracing a stacked group's running total, drawn over the
    // components, so it carries theme ink rather than any series color. Soften
    // it by moving it toward the background, not by lowering alpha: segment
    // quads overlap by half a line width and a translucent stroke beads at
    // every join.
    glm::vec4 stack_sum_line          = hex_to_vec4("ffe6dfcc");

    // --- Factory methods ---

    static Color_palette dark()
    {
        return Color_palette();
    }

    static Color_palette light()
    {
        Color_palette p;
        p.background              = hex_to_vec4("ffCDD2D5");
        p.h_label_background      = hex_to_vec4("ffbadaef");
        p.v_label_background      = hex_to_vec4("ffc1c6c9");
        p.preview_background      = hex_to_vec4("ffeeeeee");
        p.separator               = hex_to_vec4("ff999999");
        p.grid_line               = hex_to_vec4("ff000000");
        p.preview_cover           = hex_to_vec4("a8949ea4");
        p.preview_cover_secondary = hex_to_vec4("94949ea4");
        p.stack_sum_line          = hex_to_vec4("ff192033");
        return p;
    }


    // Get palette based on dark mode flag
    static Color_palette for_theme(bool dark_mode)
    {
        return dark_mode ? dark() : light();
    }
};

} // namespace vnm::plot
