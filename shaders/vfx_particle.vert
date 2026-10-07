#version 460

struct Quad {
    vec4 corner[4];
    vec4 uv;
    vec4 uv_next;
    vec4 color;
    vec4 params;
    vec4 luminance;
    vec4 extra;
    uvec4 info;
    vec4 rain_rotation;
};

layout(std430, set = 1, binding = 0) readonly buffer Quads {
    Quad quads[];
};

layout(push_constant) uniform VfxPush {
    mat4 view_projection;
    vec4 eye;
    vec4 frame;
    uvec4 ids;
} push;

layout(location = 0) out vec4 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) out vec4 out_params;
layout(location = 3) out vec4 out_luminance;
layout(location = 4) flat out uvec4 out_info;
layout(location = 5) out float out_view_depth;
layout(location = 6) flat out vec4 out_extra;
layout(location = 7) out vec3 out_world;
layout(location = 8) flat out vec3 out_tangent;
layout(location = 9) flat out vec3 out_bitangent;
layout(location = 10) flat out vec3 out_normal;
layout(location = 11) flat out vec4 out_rain_rotation;

const uint kCorners[6] = uint[6](0u, 2u, 1u, 1u, 2u, 3u);
const uint kLiquid = 8u;
const uint kTriangle = 512u;

void main() {
    const uint index = push.ids.x + uint(gl_VertexIndex) / 6u;
    const uint corner = kCorners[uint(gl_VertexIndex) % 6u];
    Quad q = quads[index];
    vec2 u = vec2((corner & 1u) != 0u ? 1.0 : 0.0, (corner & 2u) != 0u ? 1.0 : 0.0);
    out_uv = vec4(mix(q.uv.xy, q.uv.zw, u), mix(q.uv_next.xy, q.uv_next.zw, u));
    if ((q.info.y & kTriangle) != 0u) {
        const vec2 t = corner == 0u ? q.uv.xy : corner == 1u ? q.uv.zw : q.uv_next.xy;
        out_uv = vec4(t, t);
    }
    out_color = q.color;
    out_params = q.params;
    out_luminance = q.luminance;
    out_info = q.info;
    out_extra = q.extra;
    out_rain_rotation = q.rain_rotation;
    vec4 p = q.corner[corner];
    out_world = p.xyz;
    out_tangent = vec3(1.0, 0.0, 0.0);
    out_bitangent = vec3(0.0, 1.0, 0.0);
    out_normal = vec3(0.0, 0.0, 1.0);
    if ((q.info.y & kLiquid) != 0u) {
        vec3 t = q.corner[1].xyz - q.corner[0].xyz;
        vec3 b = q.corner[0].xyz - q.corner[2].xyz;
        vec3 n = cross(t, b);
        out_tangent = normalize(t + vec3(1e-7, 0.0, 0.0));
        out_bitangent = normalize(b + vec3(0.0, 1e-7, 0.0));
        out_normal = normalize(n + vec3(0.0, 0.0, 1e-7));
    }
    if (push.ids.y != 1u) {
        gl_Position = push.view_projection * vec4(p.xyz, 1.0);
        out_view_depth = gl_Position.w;
    } else {
        gl_Position = vec4(p.xy, 0.0, 1.0);
        out_view_depth = 0.0;
    }
}
