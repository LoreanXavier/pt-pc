#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#include "common.glsl"
#include "rt_shadow.glsl"
#include "reflection_mix.glsl"
#include "reflection_depth.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_map;
layout(location = 1) out vec4 out_color;

const float kReach = 50.0;

View v;

float SceneZ(vec2 uv) {
    return ViewZ(v, ReflectionFloorDepth(v, uv));
}

bool Outside(vec2 uv) {
    return uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0;
}

vec2 Project(vec3 p) {
    return ReflectionProject(p, v.projection_param.xy, v.jitter.xy);
}

bool LightHidden(uint i) {
    uint word = i >> 5u;
    return (floatBitsToUint(pass.m[word >> 2u][word & 3u]) & (1u << (i & 31u))) != 0u;
}

vec3 EvaluateSh(Probe p, vec3 n) {
    vec3 e = p.sh[0].rgb;
    e += p.sh[1].rgb * n.y + p.sh[2].rgb * n.z + p.sh[3].rgb * n.x;
    e += p.sh[4].rgb * (n.x * n.y) + p.sh[5].rgb * (n.y * n.z) + p.sh[6].rgb * (3.0 * n.z * n.z - 1.0);
    e += p.sh[7].rgb * (n.x * n.z) + p.sh[8].rgb * (n.x * n.x - n.y * n.y);
    return e;
}

vec4 ShadeHit(uint record, uint primitive, vec2 bary, mat4x3 to_world, vec3 hit, vec3 ray) {
    RtRecord r = rt_records[record];
    RtIndexData ib = RtIndexData(r.indices);
    RtVertexData vb = RtVertexData(r.vertices);
    uint base = r.first_index + 3u * primitive;
    uint a = (ib.i[base] + uint(r.vertex_offset)) * 22u;
    uint b = (ib.i[base + 1u] + uint(r.vertex_offset)) * 22u;
    uint c = (ib.i[base + 2u] + uint(r.vertex_offset)) * 22u;
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    vec2 uv = vec2(vb.v[a + 10u], vb.v[a + 11u]) * w.x + vec2(vb.v[b + 10u], vb.v[b + 11u]) * w.y + vec2(vb.v[c + 10u], vb.v[c + 11u]) * w.z;
    vec3 n_object = vec3(vb.v[a + 3u], vb.v[a + 4u], vb.v[a + 5u]) * w.x + vec3(vb.v[b + 3u], vb.v[b + 4u], vb.v[b + 5u]) * w.y +
                    vec3(vb.v[c + 3u], vb.v[c + 4u], vb.v[c + 5u]) * w.z;
    vec3 n = normalize(mat3(to_world) * n_object);
    vec4 tangent_object = vec4(vb.v[a + 6u], vb.v[a + 7u], vb.v[a + 8u], vb.v[a + 9u]) * w.x +
                          vec4(vb.v[b + 6u], vb.v[b + 7u], vb.v[b + 8u], vb.v[b + 9u]) * w.y +
                          vec4(vb.v[c + 6u], vb.v[c + 7u], vb.v[c + 8u], vb.v[c + 9u]) * w.z;
    vec3 tangent = mat3(to_world) * tangent_object.xyz;
    tangent = dot(tangent, tangent) > 1.0e-12 ? normalize(tangent) : vec3(1.0, 0.0, 0.0);
    vec3 bitangent = tangent_object.w * normalize(cross(n, tangent) + vec3(0.0, 0.0, 1.0e-9));
    if (dot(n, ray) > 0.0) {
        n = -n;
    }
    Material m = materials[r.material];
    if (m.kind == KIND_CONSTANT || m.kind == KIND_SKY) {
        vec4 t = textureLod(textures[nonuniformEXT(m.albedo)], uv, 2.0);
        return vec4(min(vec3(32.0), m.albedo_factor.rgb * t.rgb * m.params.y * v.exposure.x) * Saturate(m.albedo_factor.a * t.a), 1.0);
    }
    if (m.kind != KIND_DEFERRED) {
        return vec4(0.0);
    }
    if ((m.flags & MAT_NORMAL_MAP) != 0u) {
        vec4 nt = textureLod(textures[nonuniformEXT(m.normal)], uv, 2.0);
        vec2 nxy = vec2(nt.w, nt.y) * 2.0 - 1.0;
        float nz = sqrt(Saturate(1.0 - dot(nxy, nxy)) + 1.00016594e-4);
        vec3 tangent_normal = vec3(nxy, nz);
        if ((m.flags & MAT_SUB_NORMAL) != 0u) {
            vec4 mask = textureLod(textures[nonuniformEXT(m.aux2)], uv, 2.0);
            vec4 detail = textureLod(textures[nonuniformEXT(m.aux1)], uv * m.extra.yz, 2.0);
            float k = m.extra.x * mask.x;
            tangent_normal = normalize(vec3(nxy.x + k * (2.0 * detail.w - 1.0), nxy.y + k * (2.0 * detail.y - 1.0), nz));
        }
        n = RtMappedNormal(n, tangent, bitangent, tangent_normal);
    }
    vec3 albedo = (m.flags & MAT_CONSTANT_COLOR) != 0u ? m.albedo_factor.rgb : textureLod(textures[nonuniformEXT(m.albedo)], uv, 2.0).rgb;

    vec3 ambient = vec3(0.0);
    vec3 probe_n = vec3(-n.x, -n.z, n.y);
    for (uint i = 0u; i < pass.ids.z; ++i) {
        Probe p = frame.probes[i];
        vec3 q = (p.box * vec4(hit, 1.0)).xyz;
        vec3 f = min(clamp((1.0 - q) * p.positive.xyz, 0.0, 1.0), clamp((1.0 + q) * p.negative.xyz, 0.0, 1.0));
        float weight = p.positive.w * f.x * f.y * f.z;
        if (weight > 0.0) {
            ambient = max(vec3(0.0), weight * EvaluateSh(p, probe_n) * v.exposure.x) + ambient * max(0.0, 1.0 - weight);
        }
    }

    vec3 direct = vec3(0.0);
    for (uint i = 0u; i < pass.ids.y; ++i) {
        if (LightHidden(i)) {
            continue;
        }
        Light l = frame.lights[i];
        if (l.info.z == 1) {
            vec3 q = (l.area * vec4(hit, 1.0)).xyz;
            if (1.0 - max(abs(q.z), max(abs(q.x), abs(q.y))) < 0.0) {
                continue;
            }
        } else if (l.info.z == 2) {
            vec4 q = l.area * vec4(hit, 1.0);
            if (q.w <= 0.0) {
                continue;
            }
            vec3 aperture = q.xyz / q.w;
            if (0.5 - max(abs(aperture.z), max(abs(aperture.x), abs(aperture.y))) < 0.0) {
                continue;
            }
        }
        bool spot = l.position.w > 0.5;
        vec3 lv = l.position.xyz - hit;
        float dist = length(lv);
        vec3 L = lv / max(dist, 1.0e-6);
        float ndl = dot(n, L);
        if (ndl <= 0.0) {
            continue;
        }
        float d = spot ? max(l.range.y, dist + l.range.x) : max(l.range.y, dist) + l.range.x;
        float d2 = d * d;
        vec3 radiance = l.color.rgb * max(0.0, 1.0 / d2 - d2 * l.range.z) * v.exposure.x;
        float shadow_cone = 1.0;
        if (spot) {
            float cos_a = dot(normalize(l.direction.xyz), -L);
            radiance *= pow(abs(Saturate((cos_a - l.cone.x) * l.cone.y)), l.cone.z);
            float sc = Saturate((cos_a - l.shadow_cone.x) * l.shadow_cone.y);
            shadow_cone = sc * sc;
        }
        if (length(radiance) <= 1.0e-4) {
            continue;
        }
        vec3 mask = vec3(1.0);
        if (l.info.y >= 0 || l.info.w > 0) {
            vec4 mc = l.mask * vec4(hit, 1.0);
            if (mc.w <= 0.0) {
                continue;
            }
            vec2 muv = vec2(0.5 * mc.x / mc.w + 0.5, 1.0 - (0.5 * mc.y / mc.w + 0.5));
            mask = l.info.w > 0 ? textureLod(textures[nonuniformEXT(uint(l.info.w - 1))], muv, 0.0).rgb : ImgLod(uint(l.info.y), SMP_LINEAR_WRAP, muv, 0.0).rgb;
        }
        float vis = 1.0;
        if (l.info.x >= 0 && RtOccluded(l, hit, l.position.xyz, spot, 1u | (0x10u << 8u))) {
            float s = 1.0 - shadow_cone;
            vis = 1.0 + l.scales.z * (s * s - 1.0);
        }
        direct += radiance * l.scales.y * ndl * vis * mask;
    }
    return vec4(albedo * (ambient + direct), 1.0);
}

vec4 ScreenMarch(vec2 uv, vec3 P, vec3 R, vec2 start, vec2 stride, float scale, float inv_z, float inv_z_step, float far_limit) {
    float offset = 0.0;
    if (pass.f2.z > 0.5) {
        vec2 noise_pixel = gl_FragCoord.xy + 5.588238 * pass.f2.w;
        offset = fract(52.9829189 * fract(dot(noise_pixel, vec2(0.06711056, 0.00583715))));
    }
    bool terminated = false;
    bool behind_previous = false;
    int hit = -1;
    for (int k = 0; k < 16; ++k) {
        float s = k == 0 && pass.f1.w < 0.5 ? 1.0 : 1.0 + 25.0 * (float(k) + offset);
        vec2 suv = 0.5 * (start + s * stride) + 0.5;
        float sz = SceneZ(suv);
        terminated = terminated || behind_previous || far_limit < sz || Outside(suv);
        bool behind = 1.0 / (inv_z_step * s + inv_z) > sz;
        if (k > 0 && behind && !terminated && hit < 0) {
            hit = k - 1;
        }
        behind_previous = behind;
    }
    if (hit < 0) {
        return vec4(0.0);
    }
    float lo = hit == 0 && pass.f1.w < 0.5 ? 1.0 : 1.0 + 25.0 * (float(hit) + offset);
    float hi = 1.0 + 25.0 * (float(hit + 1) + offset);
    float best_error = 999.0;
    float best_step = 0.0;
    vec2 best_uv = vec2(0.0);
    bool invalid = false;
    for (int i = 0; i < 6; ++i) {
        float mid = (hi - lo) * 0.5 + lo;
        vec2 muv = 0.5 * (start + mid * stride) + 0.5;
        float mz = SceneZ(muv);
        float ray_z = 1.0 / (inv_z_step * mid + inv_z);
        invalid = invalid || Outside(muv) || far_limit < mz;
        float error = abs(ray_z - mz);
        if (best_error > error) {
            best_error = error;
            best_step = mid;
            best_uv = muv;
        }
        if (ray_z > mz) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    if (invalid) {
        return vec4(0.0);
    }
    float travel = best_step * scale * length(R);
    float screen_fade = Saturate(best_step * 0.0026595744);
    screen_fade = 1.0 - screen_fade * screen_fade;
    float ray_fade = 1.0 - Saturate(travel * 0.02);
    float path_fade = 1.0 - Saturate((sqrt(P.z * P.z + 1.0 + P.y * P.y + P.x * P.x) + travel) * 0.002);
    float confidence = min(path_fade, min(screen_fade, ray_fade)) * (1.0 - Saturate(best_error * 10.0));
    return vec4(clamp(0.5 * (best_uv - uv) + 0.5, 0.0, 1.0), confidence, 1.0);
}

float MarchConfidence(vec3 P, vec3 X, vec2 start, vec2 stride, float scale) {
    if (!(X.z > v.projection_param.z)) {
        return 0.0;
    }
    float s = max(1.0, dot(Project(X) - start, stride) / max(dot(stride, stride), 1.0e-20));
    float travel = s * scale;
    float screen_fade = Saturate(s * 0.0026595744);
    screen_fade = 1.0 - screen_fade * screen_fade;
    float ray_fade = 1.0 - Saturate(travel * 0.02);
    float path_fade = 1.0 - Saturate((sqrt(P.z * P.z + 1.0 + P.y * P.y + P.x * P.x) + travel) * 0.002);
    return min(path_fade, min(screen_fade, ray_fade));
}

void main() {
    v = frame.views[pass.ids.x];
    g_reflection_depth_legacy = pass.f1.z > 0.5;
    vec3 plane = pass.f0.xyz;
    float far_limit = pass.f0.w;
    vec2 uv = (gl_FragCoord.xy - 0.00390625) * pass.f2.xy;
    out_color = vec4(0.0);
    vec3 N = DecodeNormal(ImgLod(IMG_NORMAL, SMP_POINT_CLAMP, uv, 0.0).xyz);
    if (!(abs(dot(plane, N)) >= 0.9)) {
        out_map = vec4(0.0, 0.0, 1.0, 0.0);
        return;
    }
    float z = SceneZ(uv);
    if (far_limit < z) {
        out_map = vec4(0.0, 0.0, 1.0, 0.0);
        return;
    }
    vec2 ndc = in_uv * 2.0 - 1.0;
    vec3 P = ReflectionViewPosition(ndc, v.projection_param.xy, v.jitter.xy, z);
    vec3 D = P * inversesqrt(dot(P, P));
    vec3 R = D - 2.0 * dot(plane, D) * plane;
    vec3 P2 = P + R;
    vec2 start = Project(P);
    vec2 delta = Project(P2) - start;
    float scale = 1.0 / length(0.4 * pass.f1.xy * delta);
    vec2 stride = delta * scale;
    float inv_z = 1.0 / P.z;
    float inv_z_step = (1.0 / P2.z - inv_z) * scale;
    vec4 march = ScreenMarch(uv, P, R, start, stride, scale, inv_z, inv_z_step, far_limit);
    out_map = march;

    vec3 up = mat3(v.inv_view) * plane;
    vec3 origin = (v.inv_view * vec4(P, 1.0)).xyz + up * 0.005;
    vec3 ray = normalize(mat3(v.inv_view) * R);
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, rt_casters, gl_RayFlagsCullBackFacingTrianglesEXT, 4u, origin, 0.0, ray, kReach);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            RtCasterOpaque(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), rayQueryGetIntersectionPrimitiveIndexEXT(rq, false),
                           rayQueryGetIntersectionBarycentricsEXT(rq, false))) {
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        if (pass.ids.w == 1u) {
            out_color = vec4(1.0, 1.0, 0.0, 1.0);
        }
        return;
    }
    float travel = rayQueryGetIntersectionTEXT(rq, true);
    vec3 hit = origin + ray * travel;
    vec3 H = (v.view * vec4(hit, 1.0)).xyz;
    if (H.z > v.projection_param.z && pass.ids.w != 2u) {
        vec2 hit_uv = 0.5 * Project(H) + 0.5;
        vec3 X = H;
        bool unresolved = false;
        if (!Outside(hit_uv) && SceneZ(hit_uv) < H.z * 0.98 - 0.02) {
            const int kSteps = 48;
            float lo = 0.0;
            float hi = -1.0;
            for (int k = 1; k <= kSteps; ++k) {
                float t = float(k) / float(kSteps);
                vec3 Q = mix(P, H, t);
                vec2 quv = 0.5 * Project(Q) + 0.5;
                if (Outside(quv) || Q.z <= v.projection_param.z) {
                    break;
                }
                if (Q.z > SceneZ(quv)) {
                    hi = t;
                    break;
                }
                lo = t;
            }
            if (hi > 0.0) {
                for (int i = 0; i < 6; ++i) {
                    float mid = 0.5 * (lo + hi);
                    vec3 Q = mix(P, H, mid);
                    if (Q.z > SceneZ(0.5 * Project(Q) + 0.5)) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                X = mix(P, H, hi);
                hit_uv = 0.5 * Project(X) + 0.5;
            } else {
                unresolved = true;
            }
        }
        if (!unresolved && !Outside(hit_uv)) {
            float confidence = MarchConfidence(P, X, start, stride, scale);
            out_map = vec4(clamp(0.5 * (hit_uv - uv) + 0.5, 0.0, 1.0), confidence, 1.0);
            if (pass.ids.w == 1u) {
                out_map = march;
                out_color = vec4(0.0, confidence, 0.0, confidence);
            }
            return;
        }
    }
    float weight = pass.ids.w == 2u ? 1.0 : MarchConfidence(P, H, start, stride, scale);
    if (!(weight > 0.0)) {
        return;
    }
    vec4 color = ShadeHit(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true), rayQueryGetIntersectionPrimitiveIndexEXT(rq, true),
                          rayQueryGetIntersectionBarycentricsEXT(rq, true), rayQueryGetIntersectionObjectToWorldEXT(rq, true), hit, ray);
    weight *= color.w;
    out_color = vec4(color.rgb * weight, weight);
    out_map = vec4(march.xy, march.z * (1.0 - weight), march.w);
    if (pass.ids.w == 1u) {
        out_map = march;
        out_color = vec4(weight, 0.0, 0.0, weight);
    }
}
