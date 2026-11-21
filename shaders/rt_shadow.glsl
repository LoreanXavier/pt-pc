
layout(set = 2, binding = 0) uniform accelerationStructureEXT rt_casters;

struct RtRecord {
    uvec2 vertices;
    uvec2 indices;
    uint first_index;
    int vertex_offset;
    uint material;
    uint flags;
};

layout(std430, set = 2, binding = 1) readonly buffer RtRecords {
    RtRecord rt_records[];
};

vec3 RtRecordTint(uint flags) {
    return vec3(float(((flags >> 12u) & 63u) ^ 32u), float(((flags >> 18u) & 63u) ^ 32u), float(((flags >> 24u) & 63u) ^ 32u)) / 32.0;
}

#ifdef PT_D3D12
/* D3D12: raw (byte address) views of every vertex and index buffer a TLAS record can name, one unbounded descriptor array: set 2
   binding 2 as 32-bit words, nonuniform (the records of one wave differ). RtRecord.vertices.x / indices.x index it. A vertex is 22
   floats (mesh.h); the index buffer holds 32-bit indices. */
layout(std430, set = 2, binding = 2) readonly buffer RtRaw {
    uint d[];
} rt_raw[];
#define RtVertexRef uvec2
#define RtIndexRef uvec2
#define RT_VERTEX_REF(address) (address)
#define RT_INDEX_REF(address) (address)
#define RT_VERTEX(buf, k) uintBitsToFloat(rt_raw[nonuniformEXT(buf.x)].d[(buf.y >> 2u) + (k)])
#define RT_INDEX(buf, k) rt_raw[nonuniformEXT(buf.x)].d[(buf.y >> 2u) + (k)]
#else
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtVertexData {
    float v[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtIndexData {
    uint i[];
};
#define RtVertexRef RtVertexData
#define RtIndexRef RtIndexData
#define RT_VERTEX_REF(address) RtVertexData(address)
#define RT_INDEX_REF(address) RtIndexData(address)
#define RT_VERTEX(buf, k) buf.v[k]
#define RT_INDEX(buf, k) buf.i[k]
#endif

/* A ray query with a NaN or infinite origin, direction or extent is undefined in Vulkan and can hang or fault the GPU; a bad
   G-buffer texel (zero normal, invalid depth) would make one. Such a ray is not cast. */
bool RtRayValid(vec3 origin, vec3 dir, float t_max) {
    return !any(isnan(origin)) && !any(isinf(origin)) && !any(isnan(dir)) && !any(isinf(dir)) && !isnan(t_max) && !isinf(t_max) &&
           t_max > 0.0 && dot(dir, dir) > 0.0;
}

bool RtCasterOpaque(uint record, uint primitive, vec2 bary) {
    RtRecord r = rt_records[record];
    if ((r.flags & 3u) == 1u) {
        return true;
    }
    RtIndexRef ib = RT_INDEX_REF(r.indices);
    RtVertexRef vb = RT_VERTEX_REF(r.vertices);
    uint base = r.first_index + 3u * primitive;
    uint i0 = (RT_INDEX(ib, base) + uint(r.vertex_offset)) * 22u + 10u;
    uint i1 = (RT_INDEX(ib, base + 1u) + uint(r.vertex_offset)) * 22u + 10u;
    uint i2 = (RT_INDEX(ib, base + 2u) + uint(r.vertex_offset)) * 22u + 10u;
    vec2 uv = vec2(RT_VERTEX(vb, i0), RT_VERTEX(vb, i0 + 1u)) * (1.0 - bary.x - bary.y) + vec2(RT_VERTEX(vb, i1), RT_VERTEX(vb, i1 + 1u)) * bary.x + vec2(RT_VERTEX(vb, i2), RT_VERTEX(vb, i2 + 1u)) * bary.y;
    Material m = materials[r.material];
    return textureLod(PT_TEX2D(m.albedo), uv, 0.0).a >= ShadowAlphaCutoff(m.flags);
}

const uint kRtEffect = 2048u;

bool RtEffectRecord(uint record) {
    return (rt_records[record].flags & kRtEffect) != 0u;
}

vec4 RtEffectLayer(uint record, uint primitive, vec2 bary) {
    RtRecord r = rt_records[record];
    RtIndexRef ib = RT_INDEX_REF(r.indices);
    RtVertexRef vb = RT_VERTEX_REF(r.vertices);
    uint base = 3u * primitive;
    uint i0 = RT_INDEX(ib, base) * 22u;
    uint i1 = RT_INDEX(ib, base + 1u) * 22u;
    uint i2 = RT_INDEX(ib, base + 2u) * 22u;
    vec2 uv = vec2(RT_VERTEX(vb, i0 + 10u), RT_VERTEX(vb, i0 + 11u)) * (1.0 - bary.x - bary.y) +
              vec2(RT_VERTEX(vb, i1 + 10u), RT_VERTEX(vb, i1 + 11u)) * bary.x + vec2(RT_VERTEX(vb, i2 + 10u), RT_VERTEX(vb, i2 + 11u)) * bary.y;
    uint slot = floatBitsToUint(RT_VERTEX(vb, i0 + 12u));
    uint mode = floatBitsToUint(RT_VERTEX(vb, i0 + 13u));
    vec4 color = vec4(RT_VERTEX(vb, i0 + 14u), RT_VERTEX(vb, i0 + 15u), RT_VERTEX(vb, i0 + 16u), RT_VERTEX(vb, i0 + 17u));
    vec4 tex = textureLod(PT_TEX2D(slot), uv, 1.0);
    vec3 c = tex.rgb;
    if ((mode & 4u) != 0u) {
        c = mix(pow(max(vec3(1.0e-5), 0.94786733 * (vec3(0.055) + c)), vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.03928)));
    } else if ((mode & 2u) != 0u) {
        c = mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
    }
    float a = clamp(tex.a * color.a, 0.0, 1.0);
    return vec4(max(c * color.rgb, vec3(0.0)) * a, (mode & 1u) != 0u ? 0.0 : a);
}

bool RtOccluded(Light l, vec3 world, vec3 target, bool spot, uint info) {
    vec3 to_light = target - world;
    float dist = length(to_light);
    if (dist <= 1.0e-4) {
        return false;
    }
    vec3 dir = to_light / dist;
    float offset = max(abs(l.direction.w), 0.002);
    float reach = dist;
    if (spot) {
        vec3 axis = normalize(l.direction.xyz);
        float rate = -dot(dir, axis);
        if (rate > 1.0e-6) {
            reach = min(reach, (dot(world - l.position.xyz, axis) - l.range.y) / rate);
        }
    }
    float t_max = reach - offset;
    if (!RtRayValid(world + dir * offset, dir, t_max)) {
        return false;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, rt_casters, gl_RayFlagsTerminateOnFirstHitEXT | ((info >> 8u) & 0xFFu), info & 0xFFu, world + dir * offset, 0.0, dir,
                          t_max);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            RtCasterOpaque(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), rayQueryGetIntersectionPrimitiveIndexEXT(rq, false),
                           rayQueryGetIntersectionBarycentricsEXT(rq, false))) {
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
}

const float kRtSourceScale = 0.2;
const float kRtSourceMax = 0.1;
struct RtDisc {
    float radius;
    vec3 u;
    vec3 v;
    float turn;
};

RtDisc RtLightDisc(Light l, vec3 world, vec2 frag, float time) {
    RtDisc d;
    vec3 to_light = l.position.xyz - world;
    float w = clamp(l.color.w, 0.0, 0.999);
    d.radius = min(min(kRtSourceScale * w * inversesqrt(1.0 - w * w), kRtSourceMax), 0.5 * length(to_light));
    vec3 axis = normalize(to_light);
    d.u = normalize(cross(axis, abs(axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    d.v = cross(axis, d.u);
    float frame = floor(time * 60.0 + 0.5);
    d.turn = 6.2831853 * fract(52.9829189 * fract(dot(frag + 5.588238 * mod(frame, 64.0), vec2(0.06711056, 0.00583715))));
    return d;
}

vec3 RtDiscPoint(Light l, RtDisc d, uint i, uint samples) {
    const float kAngles[4] = float[4](0.0, 3.14159265, 1.5707963, 4.712389);
    float r = d.radius * (i < 2u ? 0.9 : i < 4u ? 0.5 : sqrt((float(i) + 0.5) / float(samples)));
    float a = d.turn + (i < 4u ? kAngles[i] : float(i) * 2.39996323);
    return l.position.xyz + (d.u * cos(a) + d.v * sin(a)) * r;
}

float RtShadow(Light l, vec3 world, vec3 n, bool spot, float shadow_cone, vec2 frag, float time) {
    uint info = uint(l.info.x);
    uint samples = (info >> 16u) & 0xFFu;
    float occluded = 0.0;
    if (samples <= 1u) {
        occluded = RtOccluded(l, world, l.position.xyz, spot, info) ? 1.0 : 0.0;
    } else {
        RtDisc d = RtLightDisc(l, world, frag, time);
        float counted = 0.0;
        for (uint i = 0u; i < samples; ++i) {
            vec3 target = RtDiscPoint(l, d, i, samples);
            if (dot(n, target - world) <= 0.0 && dot(n, n) > 0.0) {
                continue;
            }
            counted += 1.0;
            occluded += RtOccluded(l, world, target, spot, info) ? 1.0 : 0.0;
            if (i == 1u && counted == 2.0 && occluded != 1.0) {
                break;
            }
        }
        occluded = counted > 0.0 ? occluded / counted : 0.0;
    }
    float s = 1.0 - shadow_cone * occluded;
    return s * s;
}

float RtContactDistanceTo(Light l, vec3 world, vec3 n, vec3 target, float reach) {
    vec3 to_light = target - world;
    float dist = length(to_light);
    if (dist <= 1.0e-4) {
        return -1.0;
    }
    vec3 dir = to_light / dist;
    vec3 origin = world + n * 0.003 + dir * 0.002;
    float t_max = min(reach, dist - 0.2);
    if (l.position.w > 0.5) {
        vec3 axis = normalize(l.direction.xyz);
        float rate = -dot(dir, axis);
        if (rate > 1.0e-6) {
            t_max = min(t_max, (dot(origin - l.position.xyz, axis) - l.range.y) / rate);
        }
    }
    if (!RtRayValid(origin, dir, t_max)) {
        return -1.0;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, rt_casters, gl_RayFlagsNoneEXT, 16u, origin, 0.0, dir, t_max);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            RtCasterOpaque(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), rayQueryGetIntersectionPrimitiveIndexEXT(rq, false),
                           rayQueryGetIntersectionBarycentricsEXT(rq, false))) {
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        return -1.0;
    }
    return rayQueryGetIntersectionTEXT(rq, true);
}

float RtContactDistance(Light l, vec3 world, vec3 n, float reach) {
    return RtContactDistanceTo(l, world, n, l.position.xyz, reach);
}

float RtContactSoft(Light l, vec3 world, vec3 n, bool translucent, float reach, float shadow_cone, uint rays, vec2 frag, float time) {
    RtDisc d = RtLightDisc(l, world, frag, time);
    float sum = 0.0;
    float counted = 0.0;
    for (uint i = 0u; i < rays; ++i) {
        vec3 target = RtDiscPoint(l, d, i, 8u);
        if (!translucent && dot(n, target - world) <= 0.0) {
            continue;
        }
        float t = RtContactDistanceTo(l, world, n, target, reach);
        sum += t >= 0.0 ? 1.0 - shadow_cone * (1.0 - smoothstep(0.0, reach, t)) : 1.0;
        counted += 1.0;
    }
    return counted > 0.0 ? sum / counted : 1.0;
}
