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
    vec4 light_position[3];
    vec4 light_color[3];
    vec4 light_factors;
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
layout(location = 12) flat out vec4 out_light_position0;
layout(location = 13) flat out vec4 out_light_position1;
layout(location = 14) flat out vec4 out_light_position2;
layout(location = 15) flat out vec4 out_light_color0;
layout(location = 16) flat out vec4 out_light_color1;
layout(location = 17) flat out vec4 out_light_color2;
layout(location = 18) flat out vec4 out_light_factors;

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
    out_light_position0 = q.light_position[0];
    out_light_position1 = q.light_position[1];
    out_light_position2 = q.light_position[2];
    out_light_color0 = q.light_color[0];
    out_light_color1 = q.light_color[1];
    out_light_color2 = q.light_color[2];
    out_light_factors = q.light_factors;
    vec4 p = q.corner[corner];
    out_world = p.xyz;
    out_tangent = vec3(1.0, 0.0, 0.0);
    out_bitangent = vec3(0.0, 1.0, 0.0);
    out_normal = vec3(0.0, 0.0, 1.0);
    if ((q.info.y & kLiquid) != 0u) {
        // The liquid frame is the quad's own, whichever side the camera is on: texel x along the edge from the first corner to the
        // +u one, y along the upper edge's normal away from the +v corner, z = x cross y. Primitive_Liquid2Final turns the texel
        // by the conjugate of the particle quaternion, and in liquid_trace_f010 frame 1490 that frame is (v1 - v0, v0 - v3,
        // cross) for all 345 quads within 0.999, the plane's local x, y and z whatever the UV flips, with z facing away from the
        // camera on 341 of them (the window glass and its rain, most sconce panes, the blood water, whose z points down);
        // Primitive_LiqSprt2Final passes (-m_view[0], m_view[1], -m_view[2]), the same frame for a sprite (screen right, up and
        // toward the camera, since the view x axis points to the left of the screen)
        vec3 t = q.corner[1].xyz - q.corner[0].xyz;
        vec3 b = q.corner[0].xyz - q.corner[2].xyz;
        vec3 n = cross(t, b);
        out_tangent = normalize(t + vec3(1e-7, 0.0, 0.0));
        out_bitangent = normalize(b + vec3(0.0, 1e-7, 0.0));
        out_normal = normalize(n + vec3(0.0, 0.0, 1e-7));
    }
    // mode 0: world draw on the scene target, 2: world draw into the offscreen particle buffer, 1: flare and screen layers
    if (push.ids.y != 1u) {
        gl_Position = push.view_projection * vec4(p.xyz, 1.0);
        out_view_depth = gl_Position.w;
    } else {
        gl_Position = vec4(p.xy, 0.0, 1.0);
        out_view_depth = 0.0;
    }
}
