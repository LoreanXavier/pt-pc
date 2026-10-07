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
    vec2 uv = gl_FragCoord.xy * v.viewport.zw;
    float depth = ImgFetch(IMG_DEPTH, ivec2(gl_FragCoord.xy)).x;
    float z = ViewZ(v, depth);
    vec3 world = (v.inv_view * vec4(PixelNdc(v, gl_FragCoord.xy) * v.projection_param.xy * z, z, 1.0)).xyz;
    vec4 previous = pass.m * vec4(world, 1.0);
    vec2 d = (uv - (previous.xy / previous.w * 0.5 + 0.5)) * pass.f0.xy;
    float d2 = dot(d, d);
    float len = Saturate(sqrt(d2));
    vec2 velocity = len == 0.0 ? vec2(0.0) : d * (len * inversesqrt(d2));
    vec4 object = ImgFetch(pass.ids.x, ivec2(gl_FragCoord.xy));
    out_color = vec4(0.0, 0.0, object.x * (object.zw - (0.5 * velocity + 0.5)) + 0.5 * velocity + 0.5);
}
