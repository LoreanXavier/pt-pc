#version 460
// The SH resolve (rendering.md 5): SSLighting2_SH_SkyLight_Encode (ps 43c43aed9d80b9f5) and BilateralUpscale2x2
// (ps d767b1928bf49d8b). probe_acc holds the MultiBlend sum (rgb) and the remaining transmittance T (a).
#include "common.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec4 out_specular;

// SkyLight_Encode: where the probes cover at least 0.1 (T <= 0.89990234) the sum over its coverage 1 - T, else the sphere
// map atlas's cell 0 at the normal, which P.T. leaves black (f010_dumps: every texel of the cell is 0)
vec3 Encode(vec4 acc) {
    float t = clamp(acc.a, 0.0, 1.0);
    float coverage = 1.0 - (t <= 0.89990234 ? t : 1.0);
    return coverage > 0.0 ? acc.rgb / coverage : vec3(0.0);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    out_specular = vec4(0.0);
    if (pass.ids.x == 2u) {
        // PT_SH_RESOLVE=legacy: the weighted sum without the normalization
        out_diffuse = vec4(ImgFetch(IMG_PROBE_ACC, p).rgb, 1.0);
        return;
    }
    if (pass.ids.x == 0u) {
        out_diffuse = vec4(Encode(ImgFetch(IMG_PROBE_ACC, p)), 1.0);
        return;
    }
    // BilateralUpscale2x2: the half resolution texels i = floor(p / 2) and i + 1 in each axis, texel i standing for the
    // pixel 2i; bilinear weights for the pixel's place between them (1 and 0 on an even pixel, 0.5 and 0.5 on an odd one)
    // divided by the depth difference to the pixel plus 1e-6
    ivec2 half_size = ivec2(pass.ids.yz);
    ivec2 full_size = ivec2(ImgSize(IMG_DEPTH));
    ivec2 i = p >> 1;
    vec2 f = vec2(p & 1) * 0.5;
    float centre = ImgFetch(IMG_DEPTH, p).x;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 t = min(i + o, half_size - 1);
        float table = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float d = ImgFetch(IMG_DEPTH, min(t * 2, full_size - 1)).x;
        float w = table / (abs(d - centre) + 1e-6);
        sum += w * Encode(ImgFetch(IMG_PROBE_ACC, t));
        total += w;
    }
    out_diffuse = vec4(total > 0.0 ? sum / total : vec3(0.0), 1.0);
}
