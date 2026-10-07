#version 460
#include "common.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

const float kSteps[13] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 16.0);

vec4 Sample(vec2 uv) {
    return ImgLod(pass.ids.x, SMP_LINEAR_CLAMP, uv, 0.0);
}

float LumaAt(vec2 uv, vec2 offset, vec2 rcp_frame) {
    return Sample(uv + offset * rcp_frame).y;
}

vec4 ResolveColor(vec4 sample_color, float luma) {
    if (pass.ids.y == 2u) {
        return vec4(SrgbDecode(sample_color.rgb), sample_color.a);
    }
    return vec4(sample_color.rgb, luma);
}

void main() {
    if (pass.ids.y == 1u) {
        vec4 source = ImgFetch(pass.ids.x, ivec2(gl_FragCoord.xy));
        out_color = vec4(SrgbEncode(max(source.rgb, vec3(0.0))), source.a);
        return;
    }
    vec2 rcp_frame = 1.0 / ImgSize(pass.ids.x);
    vec2 pos_m = gl_FragCoord.xy * rcp_frame;
    const float subpix = 1.0;
    const float edge_threshold = 0.063;
    const float edge_threshold_min = 0.0312;
    vec4 rgby_m = Sample(pos_m);
    float luma_m = rgby_m.y;
    float luma_s = LumaAt(pos_m, vec2(0.0, 1.0), rcp_frame);
    float luma_e = LumaAt(pos_m, vec2(1.0, 0.0), rcp_frame);
    float luma_n = LumaAt(pos_m, vec2(0.0, -1.0), rcp_frame);
    float luma_w = LumaAt(pos_m, vec2(-1.0, 0.0), rcp_frame);
    float range_max = max(max(luma_n, luma_w), max(luma_e, max(luma_s, luma_m)));
    float range_min = min(min(luma_n, luma_w), min(luma_e, min(luma_s, luma_m)));
    float range = range_max - range_min;
    if (range < max(edge_threshold_min, range_max * edge_threshold)) {
        out_color = ResolveColor(rgby_m, rgby_m.a);
        return;
    }
    float luma_nw = LumaAt(pos_m, vec2(-1.0, -1.0), rcp_frame);
    float luma_se = LumaAt(pos_m, vec2(1.0, 1.0), rcp_frame);
    float luma_ne = LumaAt(pos_m, vec2(1.0, -1.0), rcp_frame);
    float luma_sw = LumaAt(pos_m, vec2(-1.0, 1.0), rcp_frame);
    float luma_ns = luma_n + luma_s;
    float luma_we = luma_w + luma_e;
    float subpix_rcp_range = 1.0 / range;
    float subpix_nswe = luma_ns + luma_we;
    float edge_horz1 = -2.0 * luma_m + luma_ns;
    float edge_vert1 = -2.0 * luma_m + luma_we;
    float luma_nese = luma_ne + luma_se;
    float luma_nwne = luma_nw + luma_ne;
    float edge_horz2 = -2.0 * luma_e + luma_nese;
    float edge_vert2 = -2.0 * luma_n + luma_nwne;
    float luma_nwsw = luma_nw + luma_sw;
    float luma_swse = luma_sw + luma_se;
    float edge_horz4 = abs(edge_horz1) * 2.0 + abs(edge_horz2);
    float edge_vert4 = abs(edge_vert1) * 2.0 + abs(edge_vert2);
    float edge_horz3 = -2.0 * luma_w + luma_nwsw;
    float edge_vert3 = -2.0 * luma_s + luma_swse;
    float edge_horz = abs(edge_horz3) + edge_horz4;
    float edge_vert = abs(edge_vert3) + edge_vert4;
    float subpix_nwswnese = luma_nwsw + luma_nese;
    float length_sign = rcp_frame.x;
    bool horz_span = edge_horz >= edge_vert;
    float subpix_a = subpix_nswe * 2.0 + subpix_nwswnese;
    if (!horz_span) {
        luma_n = luma_w;
        luma_s = luma_e;
    } else {
        length_sign = rcp_frame.y;
    }
    float subpix_b = subpix_a * (1.0 / 12.0) - luma_m;
    float gradient_n = luma_n - luma_m;
    float gradient_s = luma_s - luma_m;
    float luma_nn = luma_n + luma_m;
    float luma_ss = luma_s + luma_m;
    bool pair_n = abs(gradient_n) >= abs(gradient_s);
    float gradient = max(abs(gradient_n), abs(gradient_s));
    if (pair_n) {
        length_sign = -length_sign;
    }
    float subpix_c = clamp(abs(subpix_b) * subpix_rcp_range, 0.0, 1.0);
    vec2 pos_b = pos_m;
    vec2 off_np = vec2(horz_span ? rcp_frame.x : 0.0, horz_span ? 0.0 : rcp_frame.y);
    if (!horz_span) {
        pos_b.x += length_sign * 0.5;
    } else {
        pos_b.y += length_sign * 0.5;
    }
    vec2 pos_n = pos_b - off_np * kSteps[0];
    vec2 pos_p = pos_b + off_np * kSteps[0];
    float subpix_d = -2.0 * subpix_c + 3.0;
    float luma_end_n = Sample(pos_n).y;
    float subpix_e = subpix_c * subpix_c;
    float luma_end_p = Sample(pos_p).y;
    if (!pair_n) {
        luma_nn = luma_ss;
    }
    float gradient_scaled = gradient * 0.25;
    float luma_mm = luma_m - luma_nn * 0.5;
    float subpix_f = subpix_d * subpix_e;
    bool luma_m_lt_zero = luma_mm < 0.0;
    luma_end_n -= luma_nn * 0.5;
    luma_end_p -= luma_nn * 0.5;
    bool done_n = abs(luma_end_n) >= gradient_scaled;
    bool done_p = abs(luma_end_p) >= gradient_scaled;
    if (!done_n) {
        pos_n -= off_np * kSteps[1];
    }
    if (!done_p) {
        pos_p += off_np * kSteps[1];
    }
    for (int i = 2; i < 13 && (!done_n || !done_p); ++i) {
        if (!done_n) {
            luma_end_n = Sample(pos_n).y - luma_nn * 0.5;
        }
        if (!done_p) {
            luma_end_p = Sample(pos_p).y - luma_nn * 0.5;
        }
        done_n = abs(luma_end_n) >= gradient_scaled;
        done_p = abs(luma_end_p) >= gradient_scaled;
        if (!done_n) {
            pos_n -= off_np * kSteps[i];
        }
        if (!done_p) {
            pos_p += off_np * kSteps[i];
        }
    }
    float dst_n = horz_span ? pos_m.x - pos_n.x : pos_m.y - pos_n.y;
    float dst_p = horz_span ? pos_p.x - pos_m.x : pos_p.y - pos_m.y;
    bool good_span_n = (luma_end_n < 0.0) != luma_m_lt_zero;
    float span_length = dst_p + dst_n;
    bool good_span_p = (luma_end_p < 0.0) != luma_m_lt_zero;
    float span_length_rcp = 1.0 / span_length;
    bool direction_n = dst_n < dst_p;
    float dst = min(dst_n, dst_p);
    bool good_span = direction_n ? good_span_n : good_span_p;
    float subpix_g = subpix_f * subpix_f;
    float pixel_offset = dst * -span_length_rcp + 0.5;
    float subpix_h = subpix_g * subpix;
    float pixel_offset_good = good_span ? pixel_offset : 0.0;
    float pixel_offset_subpix = max(pixel_offset_good, subpix_h);
    if (!horz_span) {
        pos_m.x += pixel_offset_subpix * length_sign;
    } else {
        pos_m.y += pixel_offset_subpix * length_sign;
    }
    out_color = ResolveColor(Sample(pos_m), luma_m);
}
