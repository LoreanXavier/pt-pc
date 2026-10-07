#version 460
#include "common.glsl"
#include "lighting.glsl"

layout(location = 0) in vec2 in_uv0;
layout(location = 1) in vec2 in_uv1;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec3 in_tangent;
layout(location = 4) in vec3 in_bitangent;
layout(location = 5) in vec3 in_normal;
layout(location = 6) in vec3 in_view_position;
layout(location = 7) in vec3 in_world;

layout(push_constant) uniform DrawPush {
    mat4 model;
    uvec4 ids;
    vec4 tint;
    vec4 aux0;
    vec4 aux1;
} draw;

layout(location = 0) out vec4 out_color;

float g_mip_bias = 0.0;

vec4 Tex(uint index, vec2 uv) {
    return texture(textures[nonuniformEXT(index)], uv, g_mip_bias);
}

vec4 ForwardOut(vec3 encoded_premultiplied, float a) {
    if ((draw.ids.z & 0x80u) != 0u) {
        return vec4(encoded_premultiplied, a);
    }
    return vec4(SrgbDecode(encoded_premultiplied), a);
}

vec4 GammaSource(vec3 c, float a) {
    return ForwardOut(min(SrgbEncode(max(c, vec3(0.0))) * a, vec3(1.0)), a);
}

void main() {
    View v = frame.views[draw.ids.x];
    g_mip_bias = v.jitter.z;
    Material m = materials[draw.ids.y];
    uint flags = draw.ids.z & 0xFFu;
    float e = v.exposure.x;
    if (m.kind == KIND_CONSTANT || m.kind == KIND_SKY) {
        vec4 t = Tex(m.albedo, in_uv0);
        vec3 c = min(vec3(32.0), m.albedo_factor.rgb * t.rgb * m.params.y * e);
        float a = Saturate(m.albedo_factor.a * t.a * draw.tint.a);
        out_color = GammaSource(c, a);
        return;
    }
    vec3 N = normalize(in_normal);
    vec3 T = normalize(in_tangent);
    vec3 B = normalize(in_bitangent);
    if (m.kind == KIND_PARALLAX) {
        vec4 dirt = m.params.w > 0.5 ? Tex(m.aux2, clamp(in_uv0, 0.0, 1.0)) : vec4(0.0);
        if ((flags & 2u) != 0u) {
            dirt.a = (flags & 4u) != 0u ? 1.0 : 0.0;
        }
        vec3 Nw = normalize(mat3(v.inv_view) * N);
        vec3 irradiance = draw.aux0.rgb * Nw.y + draw.aux1.rgb;
        uint cones = floatBitsToUint(draw.aux0.w);
        for (uint i = 0u; i < 3u; ++i) {
            uint index = (draw.ids.z >> (8u + 8u * i)) & 0xFFu;
            if (index == 0xFFu) {
                continue;
            }
            Light l = frame.lights[index];
            vec3 lv = l.position.xyz - in_world;
            float dist = length(lv);
            float d = l.range.x + max(l.range.y, dist);
            float d2 = d * d;
            vec3 c = l.color.rgb * (float((cones >> (10u * i)) & 1023u) / 1023.0) * max(0.0, 1.0 / d2 - l.range.z * d2);
            irradiance += c * Saturate(dot(lv / max(dist, 1.0e-6), Nw));
        }
        vec3 lit = irradiance * e * ((flags & 16u) != 0u ? dirt.rgb : SrgbDecode(dirt.rgb));
        vec3 reflection = vec3(0.0);
        if ((flags & 1u) != 0u) {
            View capture = frame.views[uint(frame.mirror.x)];
            vec4 clip = capture.view_projection * vec4(in_world, 1.0);
            clip.xy -= capture.jitter.xy * clip.w;
            float size = frame.mirror.y;
            vec2 pixel = clamp((clip.xy / clip.w * 0.5 + 0.5) * size, vec2(0.5), vec2(size - 0.5));
            reflection = SrgbEncode(min(Img(IMG_MIRROR, SMP_LINEAR_CLAMP, pixel * v.viewport.zw).rgb, vec3(1.0)));
            if ((flags & 8u) != 0u) {
                reflection = min(Img(IMG_MIRROR, SMP_LINEAR_CLAMP, pixel * v.viewport.zw).rgb, vec3(1.0));
            }
        }
        float dither = ImgFetch(RES_DITHER, ivec2(gl_FragCoord.xy) & 7).z * 0.0078125;
        vec3 color = mix(reflection, lit, dirt.a) * (1.0 + dither) + dither;
        out_color = ForwardOut(min(color, vec3(1.0)), 1.0);
        return;
    }
    vec3 n = N;
    if ((m.flags & MAT_NORMAL_MAP) != 0u) {
        vec4 nt = Tex(m.normal, in_uv0);
        vec2 nxy = vec2(nt.w, nt.y) * 2.0 - 1.0;
        float nz = sqrt(Saturate(1.0 - dot(nxy, nxy)) + 1.0e-4);
        n = normalize(T * nxy.x + B * nxy.y + N * nz);
    }
    vec3 Nw = normalize(mat3(v.inv_view) * n);
    vec3 I = normalize(in_world - v.eye.xyz);
    vec3 Rw = I - 2.0 * dot(Nw, I) * Nw;
    vec3 h = Rw - I;
    float nv = Saturate(dot(h * inversesqrt(max(dot(h, h), 1.0e-12)), Rw));
    float schlick = exp2(nv * (-5.5546875 * nv - 6.984375));
    float f0 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(m.indices.x, 0.75)).x;
    float F = Saturate(f0 + schlick - f0 * schlick);
    float power = exp2(12.0 - 11.0 * m.params.y);
    vec3 irradiance = draw.aux0.rgb * Nw.y + draw.aux1.rgb;
    vec3 highlight = vec3(0.0);
    uint cones = floatBitsToUint(draw.aux0.w);
    for (uint i = 0u; i < 3u; ++i) {
        uint index = (draw.ids.z >> (8u + 8u * i)) & 0xFFu;
        if (index == 0xFFu) {
            continue;
        }
        Light l = frame.lights[index];
        vec3 lv = l.position.xyz - in_world;
        float dist = length(lv);
        vec3 L = lv / max(dist, 1.0e-6);
        float d = l.range.x + max(l.range.y, dist);
        float d2 = d * d;
        vec3 c = l.color.rgb * (float((cones >> (10u * i)) & 1023u) / 1023.0) * max(0.0, 1.0 / d2 - l.range.z * d2);
        float ndl = Saturate(dot(L, Nw));
        float spec = ndl > 0.0 ? exp2(power * log2(max(0.0, Saturate(dot(normalize(L - I), Nw))))) : 0.0;
        irradiance += c * ndl;
        highlight += c * spec * ndl;
    }
    uint cube = (m.flags & MAT_COMMON_REFLECTION) != 0u ? uint(draw.aux1.w) : m.aux0;
    vec3 refl = texture(cube_textures[nonuniformEXT(cube)], Rw).rgb;
    vec3 tint = (1.0 - F) * (m.albedo_factor.rgb - m.extra.rgb * refl);
    vec3 color = (m.albedo_factor.w * tint + m.extra.rgb * refl) * irradiance + highlight;
    float dither = ImgFetch(RES_DITHER, ivec2(gl_FragCoord.xy) & 7).z * 0.0078125;
    color = color * (e * (1.0 + dither)) + dither;
    float a = F * Tex(m.albedo, in_uv0).x * draw.tint.a;
    out_color = GammaSource(color, a);
}
