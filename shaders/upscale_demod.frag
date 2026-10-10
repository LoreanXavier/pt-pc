#version 460
// Remove the smooth flashlight beam before temporal reconstruction. Shadow visibility stays in
// the input so the upscaler can accumulate soft-ray noise and shadow-map tap changes.
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

// An opaque effect replaced the surface here (vfx_pass.cpp BlendState: hdr.a >= 1, the alpha tested liquids, the Freezer's view
// blood refracting another part of the view)
bool OpaqueEffectAt(ivec2 pixel) {
    pixel = clamp(pixel, ivec2(0), ivec2(ImgSize(IMG_HDR)) - 1);
    return ImgFetch(IMG_HDR, pixel).a >= 1.0;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 hdr = ImgFetch(IMG_HDR, pixel);
    float factor = 1.0;
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    if (depth > 0.0 && pass.ids.y != 0xFFFFFFFFu) {
        View v = frame.views[pass.ids.x];
        Light l = frame.lights[pass.ids.y];
        Surface s;
        s.P = ViewPosition(v, PixelNdc(v, gl_FragCoord.xy), depth);
        s.world = (v.inv_view * vec4(s.P, 1.0)).xyz;
        vec4 g_material = ImgFetch(IMG_MATERIAL, pixel);
        s.roughness = g_material.x;
        s.specular = g_material.y;
        s.material_u = g_material.z;
        // Only the beam's incident radiance belongs in this factor. Surface normals and
        // shadow samples introduce edges and frame noise that must stay inside the upscaler.
        vec3 to_light = (v.view * vec4(l.position.xyz, 1.0)).xyz - s.P;
        s.N = to_light / max(length(to_light), 1.0e-6);
        s.translucency = 0.0;
        vec3 beam;
        vec3 specular;
        if (EvaluateLight(l, v, s, false, gl_FragCoord.xy, beam, specular)) {
            factor = min(65504.0, 1.0 + Luma709(max(beam, vec3(0.0))) / pass.f0.x);
        }
    }
    // Effects keep the F of the surface under them: the upscaler mixes each pixel with its neighbours, so a pixel with another F
    // than the pixels round it comes back from the multiply by the filtered F as a rim (F = 1 on every effect pixel left the rain
    // drops on the windows and the falling glass of the last loop with white rims under DLSS, the wider the lower the render
    // scale, and a step of F inside the f060 bathtub water drew a white rectangle on it). Only inside a large opaque effect does
    // F ease toward 1, by the share of opaque effect pixels 4, 8, 16 and 32 away: the Freezer's view blood refracts another part
    // of the view, and the F of the fridge's ropes and creases under it showed through as faint lines. Small drops and shards
    // never reach that share
    if (factor > 1.0 && hdr.a >= 1.0) {
        float covered = 0.0;
        for (int r = 4; r <= 32; r *= 2) {
            covered += float(OpaqueEffectAt(pixel + ivec2(r, 0))) + float(OpaqueEffectAt(pixel - ivec2(r, 0))) +
                       float(OpaqueEffectAt(pixel + ivec2(0, r))) + float(OpaqueEffectAt(pixel - ivec2(0, r)));
        }
        factor = mix(factor, 1.0, smoothstep(0.5, 1.0, covered / 16.0));
    }
    out_color = vec4(hdr.rgb / factor, hdr.a);
    out_factor = vec4(factor, 0.0, 0.0, 0.0);
}
