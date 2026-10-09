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
layout(location = 0) out vec4 out_blur;
layout(location = 1) out vec4 out_sum;

// One bloom iteration's second Kawase pass and its add to the sum in one draw (scene_post.cpp): out_blur is kawase.frag with
// the clamp, out_sum (additive) is what the add pass, kawase.frag with offset 0 and pass.f1.x as its weight, read back from the
// stored blur: the four taps of one texel of the 8-bit sRGB target, so the value goes through that storage first.
void main() {
    uint source = pass.ids.x;
    vec2 texel = 1.0 / ImgSize(source);
    vec2 o = texel * pass.f0.x;
    vec4 c = Img(source, SMP_LINEAR_CLAMP, in_uv + vec2(-o.x, -o.y));
    c += Img(source, SMP_LINEAR_CLAMP, in_uv + vec2(o.x, -o.y));
    c += Img(source, SMP_LINEAR_CLAMP, in_uv + vec2(-o.x, o.y));
    c += Img(source, SMP_LINEAR_CLAMP, in_uv + vec2(o.x, o.y));
    c *= 0.25 * pass.f0.y;
    out_blur = clamp(c, 0.0, 1.0);
    vec4 stored = vec4(SrgbDecode(round(SrgbEncode(out_blur.rgb) * 255.0) / 255.0), round(out_blur.a * 255.0) / 255.0);
    vec4 sum = stored;
    sum += stored;
    sum += stored;
    sum += stored;
    out_sum = sum * (0.25 * pass.f1.x);
}
