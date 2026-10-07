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
    uint source = pass.ids.x;
    vec2 texel = 1.0 / ImgSize(source);
    vec2 o = texel * pass.f0.x;
    vec4 c = Img(source, SMP_LINEAR_CLAMP, in_uv + vec2(-o.x, -o.y));
    c += Img(source, SMP_LINEAR_CLAMP, in_uv + vec2(o.x, -o.y));
    c += Img(source, SMP_LINEAR_CLAMP, in_uv + vec2(-o.x, o.y));
    c += Img(source, SMP_LINEAR_CLAMP, in_uv + vec2(o.x, o.y));
    c *= 0.25 * pass.f0.y;
    out_color = pass.f0.z > 0.0 ? clamp(c, 0.0, 1.0) : c;
}
