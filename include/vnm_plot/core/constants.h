#pragma once

// VNM Plot Library - Constants
// Rendering and layout constants used by the core library.

namespace vnm::plot {

// Public constants - safe for external use
constexpr int    k_msaa_samples                   = 8;  ///< Recommended MSAA sample count

namespace detail {
// Internal implementation details - do not use directly

// Layout metrics
constexpr float  k_v_label_horizontal_padding_px  = 6.0f;

// Grid & preview metrics
constexpr float k_cell_span_min_factor = 0.1f;
constexpr float k_cell_span_max_factor = 6.0f;

// Label nudge factors
constexpr float k_v_label_vertical_nudge_px = 0.1f;
constexpr float k_h_label_vertical_nudge_px = 1.05f;

// Grid appearance
constexpr float  k_grid_line_alpha_base           = 0.75f;

// Value formatting
constexpr int    k_value_decimals                 = 3;

// Text margins
constexpr float k_text_margin_px  = 3.0f;
constexpr float k_overlay_left_px = 10.0f;
constexpr float k_line_spacing    = 1.5f;

// Scissor padding
constexpr float  k_scissor_pad_px                 = 1.0f;

// Preview bar
constexpr double k_preview_band_max_px   = 5.0;
constexpr int    k_preview_min_window_px = 60;

// Internal constants
constexpr double k_vbar_width_change_threshold_d = 0.5;
constexpr double k_vbar_min_width_px_d           = 1.0;
constexpr int    k_rect_initial_quads            = 256;

// Precision constants
constexpr float  k_pixel_snap = 0.5f;
constexpr double k_eps        = 1e-6;

} // namespace detail
} // namespace vnm::plot
