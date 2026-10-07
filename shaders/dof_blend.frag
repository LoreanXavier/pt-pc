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
    vec2 uv = pass.f2.zw * (gl_FragCoord.xy - 0.00390625);
    vec4 sharp = Img(pass.ids.x, SMP_LINEAR_CLAMP, uv);
    vec4 near = Img(pass.ids.y, SMP_LINEAR_CLAMP, uv);
    vec4 far = Img(pass.ids.z, SMP_LINEAR_CLAMP, uv);
    vec3 c = mix(sharp.rgb, far.rgb, pass.f0.z * Saturate(pass.f0.x * Saturate(2.0 * sharp.w - 1.0)));
    c = mix(c, near.rgb, pass.f0.w * Saturate(near.w * pass.f0.y));
    out_color = vec4(c, 1.0);
}
