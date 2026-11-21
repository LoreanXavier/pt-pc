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
    float limit = pass.f0.z > 0.0 ? min(pass.f0.z, 1.0) : 1.0;
    float len = min(Saturate(sqrt(d2)), limit);
    vec2 velocity = len == 0.0 ? vec2(0.0) : d * (len * inversesqrt(d2));
    if (pass.f0.w > 0.5) {
        velocity = vec2(0.0);
    }
    vec4 object = ImgFetch(pass.ids.x, ivec2(gl_FragCoord.xy));
    if (pass.f1.y > 1.0) {
        vec2 moved = MbLoad(object.zw) * pass.f1.y;
        float moved2 = dot(moved, moved);
        if (moved2 > limit * limit) {
            moved *= limit * inversesqrt(moved2);
        }
        object.zw = MbStore(moved);
    }
    if (HdrOutput()) {
        out_color = vec4(0.0, 0.0, mix(velocity, object.zw, pass.f1.x > 0.5 ? 0.0 : object.x));
        return;
    }
    out_color = vec4(0.0, 0.0, object.x * (object.zw - (0.5 * velocity + 0.5)) + 0.5 * velocity + 0.5);
}
