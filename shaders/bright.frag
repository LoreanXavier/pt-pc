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

void main() {
    // TonemapBrightpass reads the quarter size copy of the scene: CopyRenderBuffer takes one pixel of each 4x4 block, (4x + 2,
    // 4y + 2) at 1080p, from the scene's UNORM view into a UNORM target, and the bright pass reads that through the bloom
    // chain's sRGB texture, so it sees the pixel's linear colour clamped to 1 (0xDD0D40, liquid_trace_f010 ops 924 and 925;
    // bloom_trace_f010: the copy equals the scene's pixel (4x + 2, 4y + 2) byte for byte, the bright pass's T# is SRGB). Its
    // max(32 a^2, luma) term sees a = 0: Fxaa, which writes the scene with alpha 0 (b9d819a5fe5c845d, no blending), runs six
    // ops before the copy in every traced frame, so the scene alpha is 0 in every pixel of bloom_trace_f010's copy
    vec2 block = ImgSize(IMG_HDR) / ImgSize(IMG_BLOOM_A);
    vec4 t = ImgFetch(IMG_HDR, ivec2(floor(floor(gl_FragCoord.xy) * block + 0.5 * block)));
    vec3 x = clamp(t.rgb * pass.f1.x, 0.0, 1.0);
    float l1 = Luma709(x);
    float l2 = l1 * l1;
    float l3 = l2 * l1;
    float w = pass.f0.x * min(l1, 32.0) + pass.f0.y * min(l2, 32.0) + pass.f0.z * min(l3, 32.0) + pass.f0.w * min(l3 * l1, 32.0);
    out_color = vec4(clamp(x * w, 0.0, 1.0), 1.0);
}
