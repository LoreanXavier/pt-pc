#version 460
#include "common.glsl"
#include "lighting.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_factor;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 hdr = ImgFetch(IMG_HDR, pixel);
    float factor = 1.0;
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    if (depth > 0.0 && hdr.a < 1.0 && pass.ids.y != 0xFFFFFFFFu) {
        View v = frame.views[pass.ids.x];
        Light l = frame.lights[pass.ids.y];
        Surface s;
        s.P = ViewPosition(v, PixelNdc(v, gl_FragCoord.xy), depth);
        s.world = (v.inv_view * vec4(s.P, 1.0)).xyz;
        vec4 g_material = ImgFetch(IMG_MATERIAL, pixel);
        s.roughness = g_material.x;
        s.specular = g_material.y;
        s.material_u = g_material.z;
        vec3 to_light = (v.view * vec4(l.position.xyz, 1.0)).xyz - s.P;
        s.N = to_light / max(length(to_light), 1.0e-6);
        s.translucency = 0.0;
        vec3 beam;
        vec3 specular;
        if (EvaluateLight(l, v, s, false, gl_FragCoord.xy, beam, specular)) {
            factor = min(65504.0, 1.0 + Luma709(max(beam, vec3(0.0))) / pass.f0.x);
        }
    }
    out_color = vec4(hdr.rgb / factor, hdr.a);
    out_factor = vec4(factor, 0.0, 0.0, 0.0);
}
