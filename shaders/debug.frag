#version 460
#include "common.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint mode = pass.ids.x;
    View v = frame.views[pass.ids.y];
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    vec4 albedo = ImgFetch(IMG_ALBEDO, pixel);
    vec4 normal = ImgFetch(IMG_NORMAL, pixel);
    vec4 material = ImgFetch(IMG_MATERIAL, pixel);
    vec3 c = vec3(0.0);
    if (mode == 1u) {
        c = depth > 0.0 ? DecodeNormal(normal.xyz) * 0.5 + 0.5 : vec3(0.0);
    } else if (mode == 2u || mode == 3u) {
        c = albedo.rgb;
    } else if (mode == 4u) {
        c = albedo.rgb;
    } else if (mode == 5u) {
        c = vec3(material.x);
    } else if (mode == 6u) {
        c = vec3(material.y);
    } else if (mode == 7u) {
        float index = floor(material.z * 255.0 + 0.5);
        c = vec3(fract(index * 0.137), fract(index * 0.419), fract(index * 0.713)) * (index > 0.0 ? 1.0 : 0.15);
    } else if (mode == 8u) {
        c = depth > 0.0 ? vec3(1.0 - exp(-ViewZ(v, depth) * 0.08)) : vec3(0.0);
    } else if (mode == 9u) {
        c = vec3(material.w);
    } else if (mode == 10u || mode == 12u) {
        c = SrgbEncode(clamp(ImgFetch(IMG_DIFFUSE, pixel).rgb * pass.f0.x, 0.0, 1.0));
    } else if (mode == 11u) {
        c = SrgbEncode(clamp(ImgFetch(IMG_SPECULAR, pixel).rgb * pass.f0.x, 0.0, 1.0));
    } else if (mode == 13u) {
        c = vec3(normal.w);
    } else if (mode == 14u) {
        c = vec3(ImgFetch(IMG_AO_BLUR, pixel).x);
    } else if (mode == 15u) {
        c = SrgbEncode(clamp(SrgbDecode(albedo.rgb) * pass.f0.y, 0.0, 1.0));
    } else if (mode == 16u) {
        c = SrgbEncode(clamp(ImgFetch(IMG_MIRROR, pixel).rgb * pass.f0.x, 0.0, 1.0));
    }
    out_color = vec4(c, 1.0);
}
