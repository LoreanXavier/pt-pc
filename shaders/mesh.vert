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
layout(location = 8) in vec2 in_uv2;

layout(push_constant) uniform DrawPush {
    mat4 model;
    uvec4 ids;
    vec4 tint;
} draw;

layout(location = 0) out vec2 out_uv0;
layout(location = 1) out vec2 out_uv1;
layout(location = 2) out vec4 out_color;
layout(location = 3) out vec3 out_tangent;
layout(location = 4) out vec3 out_bitangent;
layout(location = 5) out vec3 out_normal;
layout(location = 6) out vec3 out_view_position;
layout(location = 7) out vec3 out_world;
layout(location = 8) out vec3 out_view_dir;
layout(location = 9) out vec2 out_uv2;

out gl_PerVertex {
    invariant vec4 gl_Position;
    float gl_ClipDistance[1];
};

void main() {
    View v = frame.views[draw.ids.x];
    vec3 position = in_position;
    vec3 normal = in_normal;
    vec3 tangent = in_tangent.xyz;
    float weight_sum = dot(in_weights, vec4(1.0));
    if (draw.ids.w != 0xFFFFFFFFu && weight_sum > 0.0) {
        uint base = draw.ids.w;
        mat4 s = skin[base + in_joints.x] * in_weights.x + skin[base + in_joints.y] * in_weights.y + skin[base + in_joints.z] * in_weights.z +
                 skin[base + in_joints.w] * in_weights.w;
        s /= weight_sum;
        position = (s * vec4(position, 1.0)).xyz;
        normal = mat3(s) * normal;
        tangent = mat3(s) * tangent;
    }
    vec4 world = draw.model * vec4(position, 1.0);
    world.xyz += WaveOffset(draw.ids.y, in_uv0);
    mat3 model3 = mat3(draw.model);
    mat3 view3 = mat3(v.view);
    vec3 nw = model3 * normal;
    vec3 tw = model3 * tangent;
    vec3 n = normalize(view3 * nw);
    vec3 t = view3 * tw;
    t = dot(t, t) > 1.0e-12 ? normalize(t) : vec3(1.0, 0.0, 0.0);
    out_normal = n;
    out_tangent = t;
    out_bitangent = in_tangent.w * normalize(view3 * cross(nw, tw) + vec3(0.0, 0.0, 1.0e-9));
    out_uv0 = in_uv0;
    out_uv1 = in_uv1;
    out_uv2 = in_uv2;
    out_color = in_color;
    out_view_position = (v.view * world).xyz;
    out_view_dir = dot(out_view_position, out_view_position) > 1.0e-12 ? -normalize(out_view_position) : vec3(0.0, 0.0, 1.0);
    out_world = world.xyz;
    gl_ClipDistance[0] = dot(v.clip_plane, vec4(world.xyz, 1.0)) + (v.clip_plane == vec4(0.0) ? 1.0 : 0.0);
    gl_Position = v.view_projection * world;
}
