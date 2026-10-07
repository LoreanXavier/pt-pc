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
    vec2 block = ImgSize(IMG_HDR) / ImgSize(IMG_BLOOM_A);
    vec4 t = ImgFetch(IMG_HDR, ivec2(floor(floor(gl_FragCoord.xy) * block + 0.5 * block)));
    vec3 x = clamp(t.rgb * pass.f1.x, 0.0, 1.0);
    float l1 = Luma709(x);
    float l2 = l1 * l1;
    float l3 = l2 * l1;
    float w = pass.f0.x * min(l1, 32.0) + pass.f0.y * min(l2, 32.0) + pass.f0.z * min(l3, 32.0) + pass.f0.w * min(l3 * l1, 32.0);
    out_color = vec4(clamp(x * w, 0.0, 1.0), 1.0);
}
