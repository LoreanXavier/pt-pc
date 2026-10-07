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
    View v = frame.views[pass.ids.w];
    float z = ViewZ(v, ImgFetch(IMG_DEPTH, ivec2(gl_FragCoord.xy)).x);
    float r = (z - pass.f0.x) / z;
    out_color = vec4(1.0, 1.0, 1.0, 0.5 * max(-1.0, min(1.0, pass.f0.w * (0.0 > r ? pass.f0.z * r : r))) + 0.5);
}
