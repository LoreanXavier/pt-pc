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
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 sharp = ImgFetch(pass.ids.x, pixel);
    vec3 color = PostLight(sharp.rgb);
    float coc = Saturate(2.0 * sharp.a - 1.0);
    vec2 half_size = vec2(ImgSize(pass.ids.y));
    vec2 p = in_uv * half_size - 0.5;
    ivec2 base = ivec2(floor(p));
    vec2 f = p - vec2(base);
    ivec2 last = ivec2(half_size) - 1;
    vec3 far = vec3(0.0);
    float total = 0.0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            vec4 t = ImgFetch(pass.ids.y, clamp(base + ivec2(i, j), ivec2(0), last));
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y) * (t.a + 1.0e-3);
            far += t.rgb * w;
            total += w;
        }
    }
    far /= max(total, 1.0e-6);
    color = mix(color, far, pass.f0.y * Saturate(pass.f0.x * coc));
    vec4 near = Img(pass.ids.z, SMP_LINEAR_CLAMP, in_uv);
    color = mix(color, near.rgb, pass.f0.z * near.a);
    out_color = vec4(PostEncode(color), 1.0);
}
