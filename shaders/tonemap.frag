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

vec3 Lut16(uint lut, vec3 c) {
    float slice = floor(15.0 * c.b);
    float u = c.r * (15.0 / 256.0) + 1.0 / 512.0 + slice / 16.0;
    float v = c.g * (15.0 / 16.0) + 1.0 / 32.0;
    vec3 a = Img(lut, SMP_LINEAR_CLAMP, vec2(u, v)).rgb;
    vec3 b = Img(lut, SMP_LINEAR_CLAMP, vec2(u + 1.0 / 16.0, v)).rgb;
    return mix(a, b, 15.0 * c.b - slice);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 c;
    if (pass.ids.y == 1u) {
        c = ImgFetch(pass.ids.x, pixel).rgb;
    } else {
        vec3 hdr = ImgFetch(IMG_HDR, pixel).rgb * pass.f0.z;
        vec3 x = min(SrgbEncode(clamp(hdr, 0.0, 1.0)) + max(ImgFetch(IMG_FLARE, pixel).rgb, vec3(0.0)), vec3(1.0));
        vec2 bloom_uv = in_uv + 0.5 / ImgSize(IMG_BLOOM_SUM);
        vec3 b = pass.f0.x > 0.0 ? min(vec3(0.5), Img(IMG_BLOOM_SUM, SMP_LINEAR_CLAMP, bloom_uv).rgb) : vec3(0.0);
        vec3 x2 = x * x;
        vec3 r = clamp(x2 + b - x2 * b, 0.0, 1.0);
        c = clamp(r * inversesqrt(max(vec3(1.0 / 512.0), r)), 0.0, 1.0);
    }
    vec3 graded = c;
    if (pass.f0.w > 0.0) {
        graded = Lut16(RES_COLOR_LUT, c);
        if (pass.f0.y < 1.0) {
            graded = mix(Lut16(RES_COLOR_LUT_PREV, c), graded, pass.f0.y);
        }
    }
    out_color = vec4(graded, Luma601(graded));
}
