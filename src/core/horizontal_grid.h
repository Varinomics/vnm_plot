#pragma once

#include <vnm_plot/core/types.h>

#include <glm/common.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace vnm::plot::detail {

struct horizontal_grid_line_t
{
    float position_px;
    float alpha;
    float thickness_px;
};

struct horizontal_grid_strip_t
{
    int                 left_px;
    int                 width_px;
    grid_layer_params_t levels;
};

// Matches grid_quad.frag, including its float modulo and smoothstep inputs.
inline float explicit_grid_line_distance(
    float                         coordinate,
    const horizontal_grid_line_t& line,
    float                         left_px,
    float                         spacing)
{
    const float start   = line.position_px - left_px;
    const float wrapped = glm::mod(coordinate - start, spacing);
    return std::min(wrapped, spacing - wrapped);
}

inline float explicit_grid_line_mask(
    float                         coordinate,
    const horizontal_grid_line_t& line,
    float                         left_px,
    float                         spacing)
{
    const float distance = explicit_grid_line_distance(coordinate, line, left_px, spacing);
    const float half_thickness = line.thickness_px * 0.1f;
    return (1.0f - glm::smoothstep(half_thickness, half_thickness + 1.0f, distance)) * line.alpha;
}

// Lines are ordered by position. Disjoint pixel strips preserve the shader's
// max composition with value lines instead of blending crossings repeatedly.
inline std::vector<horizontal_grid_strip_t> build_horizontal_grid_strips(
    const std::vector<horizontal_grid_line_t>& lines,
    float                                      width_px,
    bool                                       lcd)
{
    std::vector<horizontal_grid_strip_t> strips;
    const double rounded_width = std::round((double)width_px);
    if (!std::isfinite(rounded_width) || rounded_width < 1.0 ||
        rounded_width > std::numeric_limits<int>::max())
    {
        return strips;
    }
    const int extent = (int)rounded_width;
    float support_radius = 2.0f;
    for (const auto& line : lines) {
        support_radius = std::max(support_radius, line.thickness_px * 0.1f + 2.0f);
    }
    const float spacing = std::max(1.0e6f, (float)extent * 2.0f + support_radius * 4.0f);
    // The shader subtracts a wrapped float near spacing for negative offsets.
    // Keep that rounding inside the halo as well as the LCD sample footprint.
    support_radius += 2.0f * (std::nextafter(spacing, std::numeric_limits<float>::infinity()) - spacing);
    const auto end = std::upper_bound(
        lines.begin(), lines.end(), (float)extent + support_radius,
        [](float position, const auto& line) { return position < line.position_px; });
    auto begin = lines.begin();
    int left = 0;
    while (left < extent) {
        while (begin != end && begin->position_px + support_radius < (float)left) {
            ++begin;
        }
        const auto available = (std::size_t)(end - begin);
        int right = extent;
        if (available > grid_layer_params_t::k_max_levels) {
            const float next_support =
                begin[grid_layer_params_t::k_max_levels].position_px - support_radius;
            right = (int)std::clamp(std::floor((double)next_support), (double)left, (double)extent);
        }

        horizontal_grid_strip_t strip{left, 0, {}};
        const auto append = [&](const horizontal_grid_line_t& line) {
            const int index = strip.levels.count++;
            strip.levels.spacing_px[index]   = spacing;
            strip.levels.start_px[index]     = line.position_px - (float)left;
            strip.levels.alpha[index]        = line.alpha;
            strip.levels.thickness_px[index] = line.thickness_px;
        };
        if (right > left) {
            const auto count = std::min<std::size_t>(available, grid_layer_params_t::k_max_levels);
            for (std::size_t i = 0; i < count; ++i) {
                append(begin[i]);
            }
        }
        else {
            // A zoom can compress many outgoing labels into one column. Only
            // the original maximizer at each shader sample affects that pixel;
            // their union fits seven entries even with arbitrarily many fades.
            constexpr std::array<float, 7> k_offsets = {
                0.0f, -1.0f, -2.0f / 3.0f, -1.0f / 3.0f,
                1.0f / 3.0f, 2.0f / 3.0f, 1.0f
            };
            std::array<const horizontal_grid_line_t*, 7> winners{};
            std::array<float, 7> best{};
            const std::size_t sample_count = lcd ? k_offsets.size() : 1;
            for (auto it = begin;
                it != end && it->position_px <= (float)left + 1.0f + support_radius;
                ++it)
            {
                if (lcd && explicit_grid_line_distance(0.5f, *it, (float)left, spacing) >=
                    it->thickness_px * 0.1f + 2.0f)
                {
                    continue;
                }
                for (std::size_t sample = 0; sample < sample_count; ++sample) {
                    const float mask = explicit_grid_line_mask(
                        0.5f + k_offsets[sample], *it, (float)left, spacing);
                    if (mask > best[sample]) {
                        best[sample]    = mask;
                        winners[sample] = &*it;
                    }
                }
            }
            for (std::size_t i = 0; i < sample_count; ++i) {
                if (winners[i] && std::find(winners.begin(), winners.begin() + i, winners[i]) ==
                    winners.begin() + i)
                {
                    append(*winners[i]);
                }
            }
            right = left + 1;
        }
        strip.width_px = right - left;
        strips.push_back(strip);
        left = right;
    }
    return strips;
}

} // namespace vnm::plot::detail
