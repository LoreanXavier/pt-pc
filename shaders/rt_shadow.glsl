// Ray traced shadows (rendering.md 12.21), the PC option in place of the shadow map lookup of lighting.glsl: one ray query
// from the lit point toward the light against the frame's TLAS of shadow casters. Needs common.glsl before it and the
// extensions GL_EXT_ray_query, GL_EXT_buffer_reference and GL_EXT_buffer_reference_uvec2 (light_rt.frag).

layout(set = 2, binding = 0) uniform accelerationStructureEXT rt_casters;

// RayTracing's record of a TLAS instance (instanceCustomIndex)
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

// The same alpha cutoff as shadow.frag at a candidate hit of an alpha tested caster.
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

// One ray from the lit point toward a point of the light: true when a caster lies between. l.info.x holds the light's caster
// mask in bits 0-7 (1: the camera view's casters, 2: the mirror view's) and the culling ray flag in bits 8-15: triangles
// facing the light are back faces for a ray toward it, and the shadow passes cull them (double-sided casters are not culled:
// their instances disable culling). The ray starts shadowBias (at least 2 mm) toward the light, as the maps test the receiver
// that much nearer to it, and a spot light's casters nearer to it than innerRange along its axis are left out, as its shadow
// projection's near plane clips them.
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

// The shadow factor of the maps' form, (1 - shadow_cone * occlusion)^2. Sharp (l.info.x bits 16-23 at most 1): one ray to the
// light's position, occlusion 0 or 1. Soft: that many rays to points of the light's disc facing the lit point, its radius
// lightSize / 2 (l.color.w = sin(atan(lightSize / 2))) up to 30 cm and half the distance, turned per pixel and per frame
// (interleaved gradient noise), so the occlusion is the covered share of the disc and a temporal upscaler averages the steps.
// The first two rays go to opposite points of the disc's rim: when they agree the pixel is taken as out of the penumbra and the
// others are skipped. A point of the disc below the lit surface's horizon (n, the surface normal; 0 for a translucent surface)
// lights nothing there and is not counted: the disc of a wall lamp (lightSize about 0.6, 7 cm from its wall) reaches behind the
// wall, whose rays would otherwise darken the wall beside the lamp.
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
            // two opposite rim points (0.9 of the radius), then two at half the radius turned by a quarter, then a spiral
            const float kAngles[4] = float[4](0.0, 3.14159265, 1.5707963, 4.712389);
            float r = radius * (i < 2u ? 0.9 : i < 4u ? 0.5 : sqrt((float(i) + 0.5) / float(samples)));
            float a = turn + (i < 4u ? kAngles[i] : float(i) * 2.39996323);
            vec3 target = l.position.xyz + (u * cos(a) + v * sin(a)) * r;
            if (dot(n, target - world) <= 0.0 && dot(n, n) > 0.0) {
                continue;
            }
            counted += 1.0;
            occluded += RtOccluded(l, world, target, spot, info) ? 1.0 : 0.0;
            // the two rim rays agree (both blocked or both free): out of the penumbra
            if (i == 1u && counted == 2.0 && occluded != 1.0) {
                break;
            }
        }
        occluded = counted > 0.0 ? occluded / counted : 0.0;
    }
    float s = 1.0 - shadow_cone * occluded;
    return s * s;
}

// Contact shadows (12.21), for the lights the original lets cast shadows: one ray from the lit point toward the light, up to
// the reach, against the camera view's G-buffer surfaces without the lights' own fittings (TLAS mask bit 4,
// SceneRenderer::RecordRayTracing), which also holds the props the shadow passes leave out; so what touches or nearly touches a
// surface shadows it. Both faces occlude: props are often single-sided shells (a photo frame's open back faces the wall), which
// the shadow passes' culling would let through. A fitting whose bounds miss the light (the metal frame of a hallway sconce, its
// lights in front of it) is left out by the ray stopping 20 cm short of the light. Spot rays also stop at the authored shadow
// near plane, as the map and ordinary RT shadow do. Returns the distance to the nearest occluder, -1 for none; EvaluateLight
// (lighting.glsl) turns it into the light's visibility.
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
            // Match the spot shadow map's near plane, measured from the offset ray origin.
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

