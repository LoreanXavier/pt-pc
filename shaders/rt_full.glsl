
const uint kRtFullMask = 128u;
const uint kRtMirrorLightMask = 32u;
uint RtFullCasters(Light l) {
    return (uint(l.info.x) & 0xFFu) == kRtMirrorLightMask ? kRtMirrorLightMask : kRtFullMask;
}

#include "rt_fitting_policy.glsl"

bool RtFittingPasses(uint record, Light l, vec3 hit) {
    return RtFittingAllows(rt_records[record].flags, uint(l.info.x), uint(l.scales.w), distance(hit, l.position.xyz));
}
const uint kRtFullOriginalShadow = 1u << 24u;
const uint kRtSurfaceMask = 8u;
const uint kRtReflectMask = 4u;

#ifdef RT_LIGHT_STATE_WRITE
layout(std430, set = 2, binding = 8) buffer RtLightStates {
    vec4 rt_light_state[];
};
#else
layout(std430, set = 2, binding = 8) readonly buffer RtLightStates {
    vec4 rt_light_state[];
};
#endif

uint RtHash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float RtRandom(inout uint state) {
    state = RtHash(state + 0x9e3779b9u);
    return float(state >> 8u) * (1.0 / 16777216.0);
}

float RtFullReach(Light l, vec3 origin, vec3 dir, float dist) {
    float t_max = dist - min(0.2, 0.5 * dist);
    if (l.position.w > 0.5 && (uint(l.info.x) & (1u << 24u)) != 0u) {
        vec3 axis = normalize(l.direction.xyz);
        float rate = -dot(dir, axis);
        if (rate > 1.0e-6) {
            t_max = min(t_max, (dot(origin - l.position.xyz, axis) - l.range.y) / rate);
        }
    }
    return t_max;
}

bool RtFullOccluded(Light l, vec3 origin, vec3 target) {
    vec3 to_light = target - origin;
    float dist = length(to_light);
    if (dist <= 1.0e-4) {
        return false;
    }
    vec3 dir = to_light / dist;
    float t_max = RtFullReach(l, origin, dir, dist);
    if (!RtRayValid(origin, dir, t_max)) {
        return false;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, rt_casters, gl_RayFlagsTerminateOnFirstHitEXT, RtFullCasters(l), origin, 0.0, dir, t_max);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            !RtFittingPasses(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), l,
                             origin + dir * rayQueryGetIntersectionTEXT(rq, false)) &&
            RtCasterOpaque(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), rayQueryGetIntersectionPrimitiveIndexEXT(rq, false),
                           rayQueryGetIntersectionBarycentricsEXT(rq, false))) {
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
}

float RtFullShadow(Light l, vec3 world, vec3 ng, float offset, vec3 n, float shadow_cone, uint samples, vec2 frag, float time) {
    bool translucent = dot(n, n) <= 0.0;
    float occluded = 0.0;
    if (samples <= 1u) {
        vec3 side = translucent && dot(ng, l.position.xyz - world) < 0.0 ? -ng : ng;
        occluded = RtFullOccluded(l, world + side * offset, l.position.xyz) ? 1.0 : 0.0;
    } else {
        RtDisc d = RtLightDisc(l, world, frag, time);
        float counted = 0.0;
        for (uint i = 0u; i < samples; ++i) {
            vec3 target = RtDiscPoint(l, d, i, samples);
            if (dot(n, target - world) <= 0.0 && !translucent) {
                continue;
            }
            counted += 1.0;
            vec3 side = translucent && dot(ng, target - world) < 0.0 ? -ng : ng;
            occluded += RtFullOccluded(l, world + side * offset, target) ? 1.0 : 0.0;
            if (i == 1u && counted == 2.0 && occluded != 1.0) {
                break;
            }
        }
        occluded = counted > 0.0 ? occluded / counted : 0.0;
    }
    float s = 1.0 - shadow_cone * occluded;
    return s * s;
}

vec3 RtFullLightDiffuse(Light l, vec3 p, vec3 n, out float shadow_cone) {
    shadow_cone = 1.0;
    if (l.info.z == 1) {
        vec3 q = (l.area * vec4(p, 1.0)).xyz;
        if (1.0 - max(abs(q.z), max(abs(q.x), abs(q.y))) < 0.0) {
            return vec3(0.0);
        }
    } else if (l.info.z == 2) {
        vec4 q = l.area * vec4(p, 1.0);
        if (q.w <= 0.0) {
            return vec3(0.0);
        }
        vec3 aperture = q.xyz / q.w;
        if (0.5 - max(abs(aperture.z), max(abs(aperture.x), abs(aperture.y))) < 0.0) {
            return vec3(0.0);
        }
    }
    bool spot = l.position.w > 0.5;
    vec3 lv = l.position.xyz - p;
    float dist = length(lv);
    vec3 L = lv / max(dist, 1.0e-6);
    float ndl = dot(n, L);
    if (ndl <= 0.0) {
        return vec3(0.0);
    }
    float d = spot ? max(l.range.y, dist + l.range.x) : max(l.range.y, dist) + l.range.x;
    float d2 = d * d;
    vec3 radiance = l.color.rgb * max(0.0, 1.0 / d2 - d2 * l.range.z);
    if (spot) {
        float cos_a = dot(normalize(l.direction.xyz), -L);
        radiance *= pow(abs(Saturate((cos_a - l.cone.x) * l.cone.y)), l.cone.z);
        float sc = Saturate((cos_a - l.shadow_cone.x) * l.shadow_cone.y);
        shadow_cone = sc * sc;
    }
    if (dot(radiance, radiance) <= 1.0e-12) {
        return vec3(0.0);
    }
    vec3 mask = vec3(1.0);
    if (l.info.y >= 0 || l.info.w > 0) {
        vec4 mc = l.mask * vec4(p, 1.0);
        if (mc.w <= 0.0) {
            return vec3(0.0);
        }
        vec2 muv = vec2(0.5 * mc.x / mc.w + 0.5, 1.0 - (0.5 * mc.y / mc.w + 0.5));
        mask = l.info.w > 0 ? textureLod(PT_TEX2D(uint(l.info.w - 1)), LightMaskUv(uint(l.scales.w), muv), LightMaskLod(uint(l.scales.w))).rgb : ImgLod(uint(l.info.y), SMP_LINEAR_WRAP, LightMaskUv(uint(l.scales.w), muv), LightMaskLod(uint(l.scales.w))).rgb;
    }
    return radiance * l.scales.y * ndl * mask;
}

vec3 RtFullDirect(vec3 p, vec3 n, uint light_count, bool bounce, inout uint rng, vec2 frag, float time) {
    float u = RtRandom(rng);
    float total = 0.0;
    float chosen_weight = 0.0;
    vec3 chosen_light = vec3(0.0);
    float chosen_cone = 1.0;
    uint chosen = 0xFFFFFFFFu;
    for (uint i = 0u; i < light_count; ++i) {
        Light l = frame.lights[i];
        if (l.shadow_cone.w < 0.0) {
            continue;
        }
        float weight = bounce ? l.shadow_cone.w * rt_light_state[i].w : 1.0;
        if (weight <= 0.0) {
            continue;
        }
        float cone;
        vec3 c = RtFullLightDiffuse(l, p, n, cone) * weight;
        float w = Luma709(c);
        if (!(w > 0.0)) {
            continue;
        }
        total += w;
        float pick = w / total;
        if (u < pick) {
            chosen = i;
            chosen_weight = w;
            chosen_light = c;
            chosen_cone = cone;
            u = u / pick;
        } else {
            u = (u - pick) / max(1.0 - pick, 1.0e-6);
        }
    }
    if (chosen == 0xFFFFFFFFu) {
        return vec3(0.0);
    }
    Light l = frame.lights[chosen];
    float strength = l.shadow_cone.z * rt_light_state[chosen].w;
    float vis = 1.0;
    if (strength > 0.0) {
        RtDisc d = RtLightDisc(l, p, frag, time);
        float r = d.radius * sqrt(RtRandom(rng));
        float a = 6.2831853 * RtRandom(rng);
        vec3 target = l.position.xyz + (d.u * cos(a) + d.v * sin(a)) * r;
        if (RtFullOccluded(l, p + n * 0.01, target)) {
            float s = 1.0 - chosen_cone;
            vis = 1.0 + strength * (s * s - 1.0);
        }
    }
    return chosen_light * (total / chosen_weight) * vis;
}

struct RtSurfaceHit {
    vec3 normal;
    vec2 uv;
    uint material;
    vec3 tint;
};

RtSurfaceHit RtFullSurface(uint record, uint primitive, vec2 bary, mat4x3 to_world, vec3 ray) {
    RtRecord r = rt_records[record];
    RtIndexRef ib = RT_INDEX_REF(r.indices);
    RtVertexRef vb = RT_VERTEX_REF(r.vertices);
    uint base = r.first_index + 3u * primitive;
    uint a = (RT_INDEX(ib, base) + uint(r.vertex_offset)) * 22u;
    uint b = (RT_INDEX(ib, base + 1u) + uint(r.vertex_offset)) * 22u;
    uint c = (RT_INDEX(ib, base + 2u) + uint(r.vertex_offset)) * 22u;
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    RtSurfaceHit h;
    h.uv = vec2(RT_VERTEX(vb, a + 10u), RT_VERTEX(vb, a + 11u)) * w.x + vec2(RT_VERTEX(vb, b + 10u), RT_VERTEX(vb, b + 11u)) * w.y +
           vec2(RT_VERTEX(vb, c + 10u), RT_VERTEX(vb, c + 11u)) * w.z;
    vec3 n_object = vec3(RT_VERTEX(vb, a + 3u), RT_VERTEX(vb, a + 4u), RT_VERTEX(vb, a + 5u)) * w.x +
                    vec3(RT_VERTEX(vb, b + 3u), RT_VERTEX(vb, b + 4u), RT_VERTEX(vb, b + 5u)) * w.y +
                    vec3(RT_VERTEX(vb, c + 3u), RT_VERTEX(vb, c + 4u), RT_VERTEX(vb, c + 5u)) * w.z;
    vec3 n = mat3(to_world) * n_object;
    n = dot(n, n) > 1.0e-12 ? normalize(n) : -ray;
    h.normal = dot(n, ray) > 0.0 ? -n : n;
    h.material = r.material;
    h.tint = RtRecordTint(r.flags);
    return h;
}

vec3 RtFullAmbient(vec3 p, vec3 n, uint probe_count) {
    vec3 ambient = vec3(0.0);
    vec3 probe_n = vec3(-n.x, -n.z, n.y);
    for (uint i = 0u; i < probe_count; ++i) {
        Probe pr = frame.probes[i];
        vec3 q = (pr.box * vec4(p, 1.0)).xyz;
        vec3 f = min(clamp((1.0 - q) * pr.positive.xyz, 0.0, 1.0), clamp((1.0 + q) * pr.negative.xyz, 0.0, 1.0));
        float weight = pr.positive.w * f.x * f.y * f.z;
        if (weight > 0.0) {
            vec3 e = pr.sh[0].rgb;
            e += pr.sh[1].rgb * probe_n.y + pr.sh[2].rgb * probe_n.z + pr.sh[3].rgb * probe_n.x;
            e += pr.sh[4].rgb * (probe_n.x * probe_n.y) + pr.sh[5].rgb * (probe_n.y * probe_n.z) + pr.sh[6].rgb * (3.0 * probe_n.z * probe_n.z - 1.0);
            e += pr.sh[7].rgb * (probe_n.x * probe_n.z) + pr.sh[8].rgb * (probe_n.x * probe_n.x - probe_n.y * probe_n.y);
            ambient = max(vec3(0.0), weight * e) + ambient * max(0.0, 1.0 - weight);
        }
    }
    return ambient;
}

vec3 RtFaceNormal(View v, ivec2 pixel, ivec2 size, vec3 c) {
    ivec2 pl = max(pixel - ivec2(1, 0), ivec2(0));
    ivec2 pr = min(pixel + ivec2(1, 0), size - 1);
    ivec2 pd = max(pixel - ivec2(0, 1), ivec2(0));
    ivec2 pu = min(pixel + ivec2(0, 1), size - 1);
    vec3 l = ViewPosition(v, PixelNdc(v, vec2(pl) + 0.5), max(ImgFetch(IMG_DEPTH, pl).x, 1.0e-7));
    vec3 r = ViewPosition(v, PixelNdc(v, vec2(pr) + 0.5), max(ImgFetch(IMG_DEPTH, pr).x, 1.0e-7));
    vec3 d = ViewPosition(v, PixelNdc(v, vec2(pd) + 0.5), max(ImgFetch(IMG_DEPTH, pd).x, 1.0e-7));
    vec3 u = ViewPosition(v, PixelNdc(v, vec2(pu) + 0.5), max(ImgFetch(IMG_DEPTH, pu).x, 1.0e-7));
    vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    vec3 dy = abs(u.z - c.z) < abs(c.z - d.z) ? u - c : c - d;
    vec3 face = cross(dx, dy);
    face = dot(face, face) > 0.0 ? normalize(face) : vec3(0.0, 0.0, -1.0);
    return dot(face, c) > 0.0 ? -face : face;
}
