#version 460
#include "common.glsl"

layout(location = 0) in vec2 in_uv0;

layout(push_constant) uniform DrawPush {
    mat4 model;
    uvec4 ids;
    vec4 tint;
} draw;

void main() {
    Material m = materials[draw.ids.y];
    if ((m.flags & MAT_ALPHA_TEST) != 0u) {
        float a = texture(textures[nonuniformEXT(m.albedo)], in_uv0).a;
        if (a < ShadowAlphaCutoff(m.flags)) {
            discard;
        }
    }
}
