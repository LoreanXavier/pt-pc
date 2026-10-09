#ifdef PT_SHADOW_GATHER
/* macOS (docs/macos.md): a comparison sampler on images[] makes SPIRV-Cross declare the whole array depth2d, and Metal then reads the
   G-buffer through it as one channel (normal (r, r, r)); the sampler's bilinear 2x2 LESS comparison from a gather instead */
float ShadowTap(vec2 uv, float zref) {
    vec4 d = textureGather(sampler2D(images[IMG_SHADOW], samplers[SMP_POINT_CLAMP]), uv, 0);
    vec2 f = fract(uv * ImgSize(IMG_SHADOW) - 0.5);
    vec4 lit = vec4(lessThan(vec4(zref), d));
    return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
}
#else
float ShadowTap(vec2 uv, float zref) {
    return texture(sampler2DShadow(images[IMG_SHADOW], samplers[SMP_SHADOW]), vec3(uv, zref));
}
#endif

// The original's shadow taps take a frame-fixed 2x2 pattern: the four taps sit at +-0.5 +- 0.125 d around the sample, d
// alternating between neighbouring pixels. Where a shadow map texel covers many pixels (the hand's shadow on the floor in
// the wake-up, gc_p00_020) the shift of a quarter texel between neighbours turns the penumbra into a 2x2 checker of the
// full penumbra contrast; FXAA softens it in the original. A temporal upscaler keeps the checker as texture and shows it
// raw wherever the shadow moves over the floor and the history is dropped, so with an upscaler the main view takes one d
// for all pixels, cycled over the four values frame by frame (View temporal.yz): each frame's penumbra is smooth and the
// history averages the same 16 offsets the 2x2 pattern spreads over a block.
vec2 Dither2x2(vec3 frag) {
    return vec2(fract(0.5 * (frag.x - 0.5)) >= 0.3 ? 1.0 : -1.0, fract(0.5 * (frag.y - 0.5)) >= 0.3 ? 1.0 : -1.0);
}

// rotate (View jitter.w, PT_SHADOW_ROTATE=1 with an upscaler): the taps turned per pixel and frame. Off by default: the turning
// pattern only averages out while the view rests, and in motion it rippled around the handy light's circle on flat walls
float ShadowPcf(vec2 uv, float zref, vec2 lo, vec2 hi, vec2 texel, vec3 frag, float rotate, float frame_time, vec4 temporal) {
    vec2 d = temporal.w > 0.5 && rotate <= 0.5 ? temporal.yz : Dither2x2(frag);
    mat2 rot = mat2(1.0);
    if (rotate > 0.5) {
        float ign = fract(52.9829189 * fract(0.06711056 * frag.x + 0.00583715 * frag.y));
        float temporal_step = fract(frame_time * 60.0 * 0.6180339887);
        float angle = (ign + temporal_step) * 6.28318530718;
        float cos_a = cos(angle);
        float sin_a = sin(angle);
        rot = mat2(cos_a, -sin_a, sin_a, cos_a);
    }
    vec2 t0 = rot * vec2( 0.5, -0.5) + 0.125 * d;
    vec2 t1 = rot * vec2(-0.5, -0.5) + 0.125 * d;
    vec2 t2 = rot * vec2(-0.5,  0.5) + 0.125 * d;
    vec2 t3 = rot * vec2( 0.5,  0.5) + 0.125 * d;
    float sum = 0.0;
    sum += ShadowTap(clamp(uv + t0 * texel, lo, hi), zref);
    sum += ShadowTap(clamp(uv + t1 * texel, lo, hi), zref);
    sum += ShadowTap(clamp(uv + t2 * texel, lo, hi), zref);
    sum += ShadowTap(clamp(uv + t3 * texel, lo, hi), zref);
    return sum;
}

// The original's m_shadowProjection (0xDC07F0) is inverse(camera) * lightView * projection * texture, where the camera
// matrix has its translation replaced by (0, 0, -viewBias) and the light view's translation z has shadowBias added: the
// receiver moves viewBias (range.w) along the original's view z, which points forward, so the negative values in use move
// it toward the camera, and shadowBias (direction.w) along the light's axis. Both are translations, so their order does
// not matter. The port's View holds the Fox view (FoxView: z forward, ViewZ > 0 ahead), so inv_view[2] is the camera's
// forward axis and the original's offset is a plus. With a minus the receiver went 5 mm away from the camera: head-on that put
// the lobby paintings (f010 pict003, pict015) behind their frame's backing quad, 1.5 mm in front of the picture, so the
// CeilLamp's map shadowed them black, while seen obliquely the offset ran mostly along the wall.
float SpotShadow(Light l, View v, vec3 world, float shadow_cone, vec3 frag) {
    vec3 receiver = world + v.inv_view[2].xyz * l.range.w + normalize(l.direction.xyz) * l.direction.w;
    vec4 c = l.shadow * vec4(receiver, 1.0);
    if (c.w <= 0.0) {
        return 1.0;
    }
    vec3 ndc = c.xyz / c.w;
    vec2 local = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0)))) {
        return 1.0;
    }
    vec2 texel = 1.0 / ImgSize(IMG_SHADOW);
    vec2 lo = l.shadow_rect.xy + texel;
    vec2 hi = l.shadow_rect.xy + l.shadow_rect.zw - texel;
    vec2 uv = l.shadow_rect.xy + local * l.shadow_rect.zw;
    float s = 1.0 - 0.25 * shadow_cone * ShadowPcf(uv, ndc.z, lo, hi, texel, frag, v.jitter.w, v.exposure.w, v.temporal);
    return s * s;
}

float PointShadow(Light l, View v, vec3 view_pos, vec3 frag) {
    vec3 biased = vec3(view_pos.xy, view_pos.z + l.direction.w * l.cone.w);
    vec3 world = (v.inv_view * vec4(biased, 1.0)).xyz;
    vec3 d = (l.shadow * vec4(world, 1.0)).xyz;
    float len = max(length(d), 1.0e-5);
    float s = d.z <= 0.0 ? -1.0 : 1.0;
    vec2 p = vec2(-d.x, d.y) / (len + abs(d.z));
    vec2 local = vec2(0.5 * ((p.x * 0.99902344 + s) * 0.5) + 0.5, 0.5 - 0.5 * p.y * 0.99902344);
    float zref = 1.0 - len * l.cone.z;
    vec2 texel = 1.0 / ImgSize(IMG_SHADOW);
    vec2 uv = l.shadow_rect.xy + local * l.shadow_rect.zw;
    float half_width = l.shadow_rect.z * 0.5;
    vec2 lo = vec2(l.shadow_rect.x + (s > 0.0 ? half_width : 0.0), l.shadow_rect.y) + texel;
    vec2 hi = vec2(l.shadow_rect.x + (s > 0.0 ? l.shadow_rect.z : half_width), l.shadow_rect.y + l.shadow_rect.w) - texel;
    float sh = 1.0 - 0.25 * ShadowPcf(uv, zref, lo, hi, texel, frag, v.jitter.w, v.exposure.w, v.temporal);
    return sh * sh;
}

struct Surface {
    vec3 P;
    vec3 world;
    vec3 N;
    float roughness;
    float specular;
    float material_u;
    float translucency;
};

#if defined(PT_RT_SHADOWS) || defined(PT_RT_CONTACT)
// the reach of the contact shadow rays (RtContactDistance), 0 for none; light_main.glsl sets it from the pass
float g_contact_reach = 0.0;
// PT_CONTACT_LEGACY=1: the contact shadow's first form (full up to half the reach, multiplied into the light at full strength)
bool g_contact_legacy = false;
#endif

bool EvaluateLight(Light l, View v, Surface s, bool shadows, vec2 frag, out vec3 diffuse, out vec3 specular) {
    // the shadow taps' dither takes the frame with the pixel, so a temporal renderer can rotate its sample pattern
    const vec3 dither_frag = vec3(frag, v.exposure.w);
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    if (l.info.z == 1) {
        vec3 q = (l.area * vec4(s.world, 1.0)).xyz;
        if (1.0 - max(abs(q.z), max(abs(q.x), abs(q.y))) < 0.0) {
            return false;
        }
    } else if (l.info.z == 2) {
        vec4 q = l.area * vec4(s.world, 1.0);
        if (q.w <= 0.0) {
            return false;
        }
        vec3 aperture = q.xyz / q.w;
        if (0.5 - max(abs(aperture.z), max(abs(aperture.x), abs(aperture.y))) < 0.0) {
            return false;
        }
    }
    bool spot = l.position.w > 0.5;
    vec3 light_pos = (v.view * vec4(l.position.xyz, 1.0)).xyz;
    vec3 lv = light_pos - s.P;
    float dist = length(lv);
    vec3 L = lv / max(dist, 1.0e-6);
    float d = spot ? max(l.range.y, dist + l.range.x) : max(l.range.y, dist) + l.range.x;
    float d2 = d * d;
    float att = max(0.0, 1.0 / d2 - d2 * l.range.z);
    vec3 radiance = l.color.rgb * att * v.exposure.x;
    float shadow_cone = 1.0;
    if (spot) {
        vec3 axis = mat3(v.view) * l.direction.xyz;
        float cos_a = dot(axis, -L);
        float cone = pow(abs(Saturate((cos_a - l.cone.x) * l.cone.y)), l.cone.z);
        radiance *= cone;
        float sc = Saturate((cos_a - l.shadow_cone.x) * l.shadow_cone.y);
        shadow_cone = sc * sc;
    }
    if (length(radiance) <= 1.0e-4) {
        return false;
    }
    float shadow = 1.0;
    if (l.info.x >= 0 && shadows) {
#ifdef PT_RT_SHADOWS
        // rays only where the light can add something: a surface facing it, or a translucent one (the diffuse and specular
        // terms below are 0 otherwise)
        if (dot(s.N, L) > 0.0 || s.translucency > 0.0) {
            vec3 n = s.translucency > 0.0 ? vec3(0.0) : normalize(mat3(v.inv_view) * s.N);
            shadow = RtShadow(l, s.world, n, spot, shadow_cone, frag, v.exposure.w);
        }
#else
        shadow = spot ? SpotShadow(l, v, s.world, shadow_cone, dither_frag) : PointShadow(l, v, s.P, dither_frag);
#endif
    }
    float vis = 1.0 + l.scales.z * (shadow - 1.0);
#if defined(PT_RT_SHADOWS) || defined(PT_RT_CONTACT)
    // Contact shadows (12.21), a PC addition. A contact shadow is a shadow of this light, so it takes the light's own shadow
    // strength (scales.z: shadowStrength times the LOD's fade) and the spot's shadow cone as the map does, and joins the map's
    // shadow by the darker of the two instead of multiplying into it: a prop the map holds too (a picture frame on the wall)
    // would otherwise be counted twice, and under a light whose authored shadow is faint (the hallway table lamp's
    // 3SL_Rstand_down0000 casts at 0.047 at f060 1580) the ray's full shadow drew a picture frame's rim and its offset from the
    // wall as a black rectangle the original never shows. The occlusion falls smoothly from the surface to the reach, so a frame hung off a wall shades the
    // wall where it nears it and the long projection of a grazing light fades out instead of ending in hard parallel edges.
    if (g_contact_reach > 0.0 && l.scales.w > 0.0 && (g_contact_legacy || l.scales.z > 0.0) && (dot(s.N, L) > 0.0 || s.translucency > 0.0)) {
        float t = RtContactDistance(l, s.world, normalize(mat3(v.inv_view) * s.N), g_contact_reach);
        if (t >= 0.0) {
            if (g_contact_legacy) {
                vis *= 1.0 - Saturate(2.0 * (1.0 - t / g_contact_reach));
            } else {
                float contact = 1.0 - shadow_cone * (1.0 - smoothstep(0.0, g_contact_reach, t));
                vis = 1.0 + l.scales.z * (min(shadow, contact * contact) - 1.0);
            }
        }
    }
#endif
    vec3 mask = vec3(1.0);
    if (l.info.y >= 0 || l.info.w > 0) {
        vec4 c = l.mask * vec4(s.world, 1.0);
        if (c.w > 0.0) {
            vec2 uv = vec2(0.5 * c.x / c.w + 0.5, 1.0 - (0.5 * c.y / c.w + 0.5));
            if (l.info.w > 0) {
                mask = textureLod(textures[nonuniformEXT(uint(l.info.w - 1))], uv, 0.0).rgb;
            } else {
                mask = ImgLod(uint(l.info.y), SMP_LINEAR_WRAP, uv, 0.0).rgb;
            }
        } else {
            mask = vec3(0.0);
        }
    }
    vec3 N = s.N;
    vec3 V = -normalize(s.P);
    vec3 R = reflect(-V, N);
    vec3 to_ray = R - L;
    float k = Saturate(Saturate(0.9 * l.color.w / d) / max(length(to_ray), 1.0e-6));
    vec3 Ls = normalize(L + to_ray * k);
    vec3 H = normalize(Ls + V);
    float NdotH = Saturate(dot(N, H));
    float NdotL = dot(N, L);
    float LdotH = dot(H, L);
    vec4 mat0 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(s.material_u, 0.25));
    vec4 mat1 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(s.material_u, 0.75));
    vec3 lut_uv = vec3(NdotH * NdotH * 0.984375 + 0.0078125, s.roughness * s.roughness * 0.9375 + 0.03125, mat1.w * mat1.w * 0.9375 + 0.03125);
    vec2 ndf = texture(sampler3D(lut2_image, samplers[SMP_LINEAR_CLAMP]), lut_uv).xy;
    float D = ndf.x * ndf.x / max(ndf.y * ndf.y, 1.0e-8);
    float F = Img(RES_LUT1, SMP_LINEAR_CLAMP, vec2(LdotH, mat1.x)).x;
    float spec = Saturate(4.0 * NdotL) * F * D;
    float w = s.translucency;
    float diff = Saturate(max(NdotL + w * (1.0 - NdotL * NdotL), w * mat0.w));
    diffuse = radiance * l.scales.y * diff * vis * mask;
    specular = radiance * l.scales.x * s.specular * mat0.rgb * spec * vis * mask;
    return true;
}
