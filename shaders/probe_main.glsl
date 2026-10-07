#include "common.glsl"
#ifdef PT_RT_AO
layout(set = 2, binding = 7, r32f) uniform readonly image2D rt_ao;
#endif

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) out vec4 out_diffuse;

vec3 EvaluateSh(Probe p, vec3 n) {
    vec3 e = p.sh[0].rgb;
    e += (p.sh[1].rgb * n.y + p.sh[2].rgb * n.z + p.sh[3].rgb * n.x);
    e += (p.sh[4].rgb * (n.x * n.y) + p.sh[5].rgb * (n.y * n.z) + p.sh[6].rgb * (3.0 * n.z * n.z - 1.0));
    e += (p.sh[7].rgb * (n.x * n.z) + p.sh[8].rgb * (n.x * n.x - n.y * n.y));
    return e;
}

vec3 SphereMapDirection(vec2 t) {
    float len2 = dot(t, t);
    float z = min(1.0, 2.0 * len2 - 1.0);
    vec2 xy = len2 > 0.0 ? vec2(-t.x, t.y) * inversesqrt(len2) * sqrt(max(0.0, 1.0 - z * z)) : vec2(0.0);
    return vec3(xy, z);
}

vec3 SphereMapTexel(Probe p, View v, ivec2 texel) {
    vec2 s = (vec2(texel) + 0.5) / 16.0;
    vec3 d = SphereMapDirection(vec2(2.0 * s.x - 1.0, 1.0 - 2.0 * s.y));
    vec3 w = mat3(v.inv_view) * d;
    return EvaluateSh(p, vec3(-w.x, -w.z, w.y));
}

vec3 SphereMapLookup(Probe p, View v, vec3 n_view) {
    float r = 0.40625 * sqrt(max(0.0, 0.5 * n_view.z + 0.5));
    float len = length(n_view.xy);
    vec2 dir = len > 0.0 ? n_view.xy / len : vec2(1.0, 0.0);
    vec2 s = 0.5 - r * dir;
    vec2 texel = s * 16.0 - 0.5;
    ivec2 i0 = ivec2(floor(texel));
    vec2 f = texel - vec2(i0);
    vec3 a = SphereMapTexel(p, v, i0);
    vec3 b = SphereMapTexel(p, v, i0 + ivec2(1, 0));
    vec3 c = SphereMapTexel(p, v, i0 + ivec2(0, 1));
    vec3 d = SphereMapTexel(p, v, i0 + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    View v = frame.views[pass.ids.x];
    Probe p = frame.probes[pass.ids.y];
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 frag = gl_FragCoord.xy;
    if (pass.ids.z != 0u) {
        pixel *= 2;
        frag = vec2(pixel) + 0.5;
    }
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    if (depth <= 0.0) {
        discard;
    }
    vec3 P = ViewPosition(v, PixelNdc(v, frag), depth);
    vec3 world = (v.inv_view * vec4(P, 1.0)).xyz;
    vec3 q = (p.box * vec4(world, 1.0)).xyz;
    vec3 f = min(clamp((1.0 - q) * p.positive.xyz, 0.0, 1.0), clamp((1.0 + q) * p.negative.xyz, 0.0, 1.0));
    float weight = p.positive.w * f.x * f.y * f.z;
    if (weight <= 0.0) {
        discard;
    }
    vec3 N = DecodeNormal(ImgFetch(IMG_NORMAL, pixel).xyz);
    vec3 e = SphereMapLookup(p, v, N) * v.exposure.x;
#ifdef PT_RT_AO
    e *= imageLoad(rt_ao, pixel).x;
#endif
    out_diffuse = vec4(max(vec3(0.0), weight * e), max(0.0, 1.0 - weight));
}
