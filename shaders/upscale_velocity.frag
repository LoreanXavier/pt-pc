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

layout(location = 0) out vec4 out_motion;

void main() {
    Material m = materials[draw.ids.y];
    float bias = frame.views[draw.ids.x].jitter.z;
    float alpha = (m.flags & MAT_CONSTANT_COLOR) != 0u ? 1.0 : texture(textures[nonuniformEXT(m.albedo)], in_uv0, bias).a;
    vec4 dither = MeshDither(gl_FragCoord.xy, frame.views[draw.ids.x].temporal);
    if (draw.tint.a - dither.x < 0.0 || alpha - m.params.x * AlphaReference(m.flags, dither) < 0.0) {
        discard;
    }
    vec2 motion = (in_previous.xy / in_previous.w - in_current.xy / in_current.w) * 0.5;
    out_motion = vec4(motion, 0.0, 0.0);
}
