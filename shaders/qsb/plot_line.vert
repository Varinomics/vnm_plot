#version 440
#extension GL_GOOGLE_include_directive : require

// Per-instance segment expansion of the line strip. The four triangle-strip
// corners (gl_VertexIndex 0..3) form a thickened screen-space quad spanning
// samples p0 -> p1, with prev and next passed flat to the fragment shader
// for rounded segment joins.
//
// The shader receives the four neighbouring samples (prev, p0, p1, next)
// for each segment as four per-instance vertex attributes. The host fills a
// dedicated per-frame buffer that already contains the leading and trailing
// boundary duplicates ([s[first], s[first], s[first+1], ..., s[last], s[last]])
// and binds it four times at consecutive gpu_sample_t offsets. Reading neighbour
// samples through vertex attributes avoids the SSBO -> UAV mapping SPIRV-Cross
// emits for std430 buffers; D3D11 SM 5.0 vertex shaders accept no UAVs at all,
// so any storage-buffer access in the vertex stage fails to compile.

#include "uniform_blocks.glsl"

layout(location = 0) in vec2 in_prev;
layout(location = 1) in vec2 in_p0;
layout(location = 2) in vec2 in_p1;
layout(location = 3) in vec2 in_next;

layout(std140, binding = 0) uniform Block
{
    Series_view_t view;
    float line_px;
    int   snap_to_pixels;
} u;

layout(location = 0) flat out vec4 fs_segment;
layout(location = 1) flat out vec4 fs_prev_segment;
layout(location = 2) flat out vec4 fs_next_segment;

vec2 sample_to_pos(vec2 sample_xy)
{
    float rt = max(u.view.t_max - u.view.t_min, 1e-30);
    float rv = max(u.view.v_max - u.view.v_min, 1e-30);

    float x = u.view.width  *       (sample_xy.x - u.view.t_min) / rt;
    float y = u.view.height * (1.0 - (sample_xy.y - u.view.v_min) / rv) + u.view.y_offset;

    if (u.snap_to_pixels != 0) {
        x = floor(x) + 0.5;
        y = floor(y) + 0.5;
    }
    return vec2(x, y);
}

bool clip_segment(inout vec2 a, inout vec2 b, vec2 lower, vec2 upper)
{
    for (int axis = 0; axis < 2; ++axis) {
        if (max(a[axis], b[axis]) < lower[axis] || min(a[axis], b[axis]) > upper[axis]) {
            return false;
        }
        if (a[axis] < lower[axis] || a[axis] > upper[axis]) {
            float edge = clamp(a[axis], lower[axis], upper[axis]);
            float t = (edge - a[axis]) / (b[axis] - a[axis]);
            a = mix(a, b, t);
            // Write the clipped coordinate directly. Reconstructing it with
            // a + t * (b - a) loses whole pixels for sparse, distant samples.
            a[axis] = edge;
        }
        if (b[axis] < lower[axis] || b[axis] > upper[axis]) {
            float edge = clamp(b[axis], lower[axis], upper[axis]);
            float t = (edge - a[axis]) / (b[axis] - a[axis]);
            b = mix(a, b, t);
            b[axis] = edge;
        }
    }
    return true;
}

void main()
{
    vec2 p_prev = sample_to_pos(in_prev);
    vec2 p0     = sample_to_pos(in_p0);
    vec2 p1     = sample_to_pos(in_p1);
    vec2 p_next = sample_to_pos(in_next);

    float half_px = max(u.line_px * 0.5, 0.5);
    // Keep synthetic clipped caps beyond the stroke and antialiasing support.
    // Each neighbor is clipped independently: a shared original endpoint may
    // be replaced by different intersections on its two incident segments.
    float margin = half_px + 2.0;
    vec2 lower = vec2(-margin, u.view.y_offset - margin);
    vec2 upper = vec2(u.view.width + margin, u.view.y_offset + u.view.height + margin);
    vec2 prev_end   = p0;
    vec2 next_begin = p1;
    if (!clip_segment(p0, p1, lower, upper)) {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    if (!clip_segment(p_prev, prev_end, lower, upper)) {
        p_prev = p0;
        prev_end = p0;
    }
    if (!clip_segment(next_begin, p_next, lower, upper)) {
        next_begin = p1;
        p_next = p1;
    }

    vec2  seg_v   = p1 - p0;
    float seg_len = length(seg_v);

    vec2 pos;
    if (seg_len <= 1e-6) {
        // Degenerate segment collapses to a point; the rasterizer culls it.
        pos = p0;
    }
    else {
        vec2 dir = seg_v / seg_len;
        vec2 n   = vec2(-dir.y, dir.x);

        vec2 p0_ext = p0 - dir * half_px;
        vec2 p1_ext = p1 + dir * half_px;

        // Triangle-strip vertex order (gl_VertexIndex 0..3): p0+n, p0-n, p1+n, p1-n.
        vec2  base   = (gl_VertexIndex < 2) ? p0_ext : p1_ext;
        float n_sign = (gl_VertexIndex & 1) == 0 ? 1.0 : -1.0;
        pos = base + n * (half_px * n_sign);
    }

    fs_segment      = vec4(p0, p1);
    fs_prev_segment = vec4(p_prev, prev_end);
    fs_next_segment = vec4(next_begin, p_next);
    gl_Position = u.view.pmv * vec4(pos, 0.0, 1.0);
}
