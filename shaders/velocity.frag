#version 460
#include "common.glsl"

layout(location = 0) in vec2 in_uv0;
layout(location = 1) in vec4 in_current;
layout(location = 2) in vec4 in_previous;

layout(push_constant) uniform DrawPush {
    mat4 model;
    uvec4 ids;
    vec4 tint;
    vec4 aux[2];
} draw;

layout(location = 0) out vec4 out_color;

void main() {
    Material m = materials[draw.ids.y];
    float alpha = (m.flags & MAT_CONSTANT_COLOR) != 0u ? 1.0 : texture(textures[nonuniformEXT(m.albedo)], in_uv0).a;
    vec4 dither = MeshDither(gl_FragCoord.xy, frame.views[draw.ids.x].temporal);
    if (draw.tint.a - dither.x < 0.0 || alpha - m.params.x * AlphaReference(m.flags, dither) < 0.0) {
        discard;
    }
    vec2 d = (in_current.xy / in_current.w - in_previous.xy / in_previous.w) * 0.5 * vec2(1920.0 / 128.0, 1080.0 / 128.0);
    float len = sqrt(dot(d, d));
    vec2 v = len > 0.0 ? d * (Saturate(len) / len) : vec2(0.0);
    out_color = vec4(1.0, 0.0, 0.5 * v + 0.5);
}
