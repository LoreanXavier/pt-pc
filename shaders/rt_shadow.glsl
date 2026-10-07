
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

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtVertexData {
    float v[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtIndexData {
    uint i[];
};

bool RtCasterOpaque(uint record, uint primitive, vec2 bary) {
    RtRecord r = rt_records[record];
    RtIndexData ib = RtIndexData(r.indices);
    RtVertexData vb = RtVertexData(r.vertices);
    uint base = r.first_index + 3u * primitive;
    uint i0 = (ib.i[base] + uint(r.vertex_offset)) * 22u + 10u;
    uint i1 = (ib.i[base + 1u] + uint(r.vertex_offset)) * 22u + 10u;
    uint i2 = (ib.i[base + 2u] + uint(r.vertex_offset)) * 22u + 10u;
    vec2 uv = vec2(vb.v[i0], vb.v[i0 + 1u]) * (1.0 - bary.x - bary.y) + vec2(vb.v[i1], vb.v[i1 + 1u]) * bary.x + vec2(vb.v[i2], vb.v[i2 + 1u]) * bary.y;
    Material m = materials[r.material];
    return textureLod(textures[nonuniformEXT(m.albedo)], uv, 0.0).a >= ShadowAlphaCutoff(m.flags);
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
    if (t_max <= 0.0) {
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

float RtShadow(Light l, vec3 world, vec3 n, bool spot, float shadow_cone, vec2 frag, float time) {
    uint info = uint(l.info.x);
    uint samples = (info >> 16u) & 0xFFu;
    float occluded = 0.0;
    if (samples <= 1u) {
        occluded = RtOccluded(l, world, l.position.xyz, spot, info) ? 1.0 : 0.0;
    } else {
        vec3 to_light = l.position.xyz - world;
        float w = clamp(l.color.w, 0.0, 0.999);
        float radius = min(min(w * inversesqrt(1.0 - w * w), 0.3), 0.5 * length(to_light));
        vec3 axis = normalize(to_light);
        vec3 u = normalize(cross(axis, abs(axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        vec3 v = cross(axis, u);
        float frame = floor(time * 60.0 + 0.5);
        float turn = 6.2831853 * fract(52.9829189 * fract(dot(frag + 5.588238 * mod(frame, 64.0), vec2(0.06711056, 0.00583715))));
        float counted = 0.0;
        for (uint i = 0u; i < samples; ++i) {
            const float kAngles[4] = float[4](0.0, 3.14159265, 1.5707963, 4.712389);
            float r = radius * (i < 2u ? 0.9 : i < 4u ? 0.5 : sqrt((float(i) + 0.5) / float(samples)));
            float a = turn + (i < 4u ? kAngles[i] : float(i) * 2.39996323);
            vec3 target = l.position.xyz + (u * cos(a) + v * sin(a)) * r;
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

float RtContactDistance(Light l, vec3 world, vec3 n, float reach) {
    vec3 to_light = l.position.xyz - world;
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
    if (t_max <= 0.0) {
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
