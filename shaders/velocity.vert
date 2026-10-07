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
    vec4 aux[2];
} draw;

layout(location = 0) out vec2 out_uv0;
layout(location = 1) out vec4 out_current;
layout(location = 2) out vec4 out_previous;

out gl_PerVertex {
    invariant vec4 gl_Position;
};

vec3 Skinned(uint base, float weight_sum) {
    mat4 s = skin[base + in_joints.x] * in_weights.x + skin[base + in_joints.y] * in_weights.y + skin[base + in_joints.z] * in_weights.z +
             skin[base + in_joints.w] * in_weights.w;
    s /= weight_sum;
    return (s * vec4(in_position, 1.0)).xyz;
}

void main() {
    View v = frame.views[draw.ids.x];
    vec3 position = in_position;
    vec3 previous = in_position;
    float weight_sum = dot(in_weights, vec4(1.0));
    uint previous_skin = floatBitsToUint(draw.aux[0].y);
    if (draw.ids.w != 0xFFFFFFFFu && weight_sum > 0.0) {
        position = Skinned(draw.ids.w, weight_sum);
        previous = previous_skin != 0xFFFFFFFFu ? Skinned(previous_skin, weight_sum) : position;
    }
    vec4 world = draw.model * vec4(position, 1.0);
    world.xyz += WaveOffset(draw.ids.y, in_uv0);
    gl_Position = v.view_projection * world;
    out_uv0 = in_uv0;
    out_current = gl_Position;
    out_previous = frame.views[draw.ids.z].view_projection * (skin[floatBitsToUint(draw.aux[0].x)] * vec4(previous, 1.0) + vec4(WaveOffset(draw.ids.y, in_uv0), 0.0));
}
