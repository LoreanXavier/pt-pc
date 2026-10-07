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
layout(location = 0) out vec4 out_motion;

void main() {
    View v = frame.views[pass.ids.w];
    float depth = ImgFetch(IMG_DEPTH, ivec2(gl_FragCoord.xy)).x;
    vec2 ndc = PixelNdc(v, gl_FragCoord.xy);
    vec4 previous;
    if (depth > 0.0) {
        float z = ViewZ(v, depth);
        vec3 world = (v.inv_view * vec4(ndc * v.projection_param.xy * z, z, 1.0)).xyz;
        previous = pass.m * vec4(world, 1.0);
    } else {
        vec3 direction = mat3(v.inv_view) * vec3(ndc * v.projection_param.xy, 1.0);
        previous = pass.m * vec4(direction, 0.0);
    }
    vec2 motion = vec2(0.0);
    if (previous.w > 1.0e-6) {
        motion = (previous.xy / previous.w - ndc) * 0.5;
    }
    out_motion = vec4(motion, 0.0, 0.0);
}
