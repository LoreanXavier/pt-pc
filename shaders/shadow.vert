#version 460
#include "common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv0;
layout(location = 4) in vec2 in_uv1;
layout(location = 5) in vec4 in_color;
layout(location = 6) in uvec4 in_joints;
layout(location = 7) in vec4 in_weights;

layout(push_constant) uniform DrawPush {
    mat4 model;
    uvec4 ids;
    vec4 tint;
} draw;

layout(location = 0) out vec2 out_uv0;

out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};

void main() {
    View v = frame.views[draw.ids.x];
    vec3 position = in_position;
    float weight_sum = dot(in_weights, vec4(1.0));
    if (draw.ids.w != 0xFFFFFFFFu && weight_sum > 0.0) {
        uint base = draw.ids.w;
        mat4 s = skin[base + in_joints.x] * in_weights.x + skin[base + in_joints.y] * in_weights.y + skin[base + in_joints.z] * in_weights.z +
                 skin[base + in_joints.w] * in_weights.w;
        position = (s * vec4(position, 1.0)).xyz / weight_sum;
    }
    vec4 world = draw.model * vec4(position, 1.0);
    out_uv0 = in_uv0;
    if (v.projection_param.w == 2.0) {
        vec3 d = (v.view * world).xyz;
        float len = max(length(d), 1.0e-5);
        float side = v.eye.w;
        vec2 p = vec2(-d.x, d.y) / (len + abs(d.z));
        gl_ClipDistance[0] = side * d.z + 1.0e-4 * len;
        gl_Position = vec4(p.x, -p.y, 1.0 - len * v.shadow.x, 1.0);
    } else {
        gl_ClipDistance[0] = 1.0;
        gl_Position = v.view_projection * world;
    }
}
