#include "common.glsl"
#include "rt_shadow.glsl"
#include "rt_full.glsl"
#include "pt_path.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

#include "rt_full_images.glsl"

View v;

float Cutoff() {
    return 1.0e-4 / max(v.exposure.x, 1.0e-12);
}

vec3 Clamp(vec3 c) {
    if (any(isnan(c)) || any(isinf(c))) {
        return vec3(0.0);
    }
    float bright = Luma709(c) * v.exposure.x;
    return bright > pass.f0.w ? c * (pass.f0.w / bright) : c;
}

void main() {
    v = frame.views[pass.ids.x];
    g_pt_fill_unshadowed = pass.f1.x > 0.5;
    ivec2 half_pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = ivec2(v.viewport.xy);
#ifdef PT_INDIRECT_RENDER_SIZE
    ivec2 half_size = size;
#else
    ivec2 half_size = (size + 1) / 2;
#endif
    if (half_pixel.x >= half_size.x || half_pixel.y >= half_size.y) {
        return;
    }
#ifdef PT_INDIRECT_RENDER_SIZE
    ivec2 pixel = half_pixel;
#else
    ivec2 pixel = min(half_pixel * 2, size - 1);
#endif
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    if (depth <= 0.0) {
        imageStore(rt_gi_raw, half_pixel, vec4(0.0));
        imageStore(rt_glossy_raw, half_pixel, vec4(0.0, 0.0, 0.0, -1.0));
        return;
    }
    vec3 c = ViewPosition(v, PixelNdc(v, vec2(pixel) + 0.5), depth);
    mat3 to_world = mat3(v.inv_view);
    vec3 ng = normalize(to_world * RtFaceNormal(v, pixel, size, c));
    vec3 n = normalize(to_world * DecodeNormal(ImgFetch(IMG_NORMAL, pixel).xyz));
    vec3 world = (v.inv_view * vec4(c, 1.0)).xyz;
    vec3 V = normalize(v.eye.xyz - world);
    if (dot(n, V) <= 0.0) {
        n = ng;
    }
    vec4 g = ImgFetch(IMG_MATERIAL, pixel);
    PtMaterial s;
    s.albedo = ImgFetch(IMG_ALBEDO, pixel).rgb;
    s.roughness = g.x;
    s.specular = g.y;
    s.material_u = g.z;
    s.translucency = g.w;
    PtMaterialRows(s);
    vec3 origin = world + ng * max(0.003, 0.0015 * c.z);
    uint bounces = uint(pass.f0.y + 0.5);
    uint rng = RtHash(uint(half_pixel.x) * 1973u + uint(half_pixel.y) * 9277u) ^ RtHash(pass.ids.z * 26699u + 3u);
#if defined(PT_D3D12) && defined(PT_INDIRECT_RENDER_SIZE)
    if (pass.f1.y > 0.5) {
        vec4 mat0_one = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(s.material_u, 0.25));
        vec3 level_one = s.specular * mat0_one.rgb;
        float ls = Luma709(PtSpecularAlbedo(n, V, s));
        bool specular_worth = !(ls < 0.002 || Luma709(level_one) < 1.0e-4);
        float ld = Luma709(s.albedo);
        float p_spec = specular_worth && ls + ld > 1.0e-5 ? clamp(ls / (ls + ld), 0.05, 0.95) : 0.0;
        float travel_one = pass.f0.x;
        bool diffuse_one = RtRandom(rng) >= p_spec;
        vec3 d_one;
        vec3 path_weight_one = vec3(1.0);
        bool valid_one = true;
        if (diffuse_one) {
            uint seed_one = RtHash(uint(half_pixel.x) * 7919u + uint(half_pixel.y) * 104729u);
            vec2 e_one = fract(vec2(float(seed_one & 0xFFFFu), float(seed_one >> 16u)) / 65536.0 + float(pass.ids.z) * vec2(0.7548776662, 0.5698402910));
            d_one = PtCosineDirection(n, e_one);
            if (dot(d_one, ng) < 0.0) {
                d_one = normalize(d_one - 2.0 * dot(d_one, ng) * ng);
            }
        } else {
            vec3 w_one;
            valid_one = PtSampleSpecular(n, V, s, vec2(RtRandom(rng), RtRandom(rng)), d_one, w_one);
            if (valid_one) {
                if (dot(d_one, ng) <= 0.0) {
                    d_one = normalize(d_one - 2.0 * dot(d_one, ng) * ng);
                }
                path_weight_one = w_one / max(level_one, vec3(1.0e-4));
            }
        }
        vec3 light = vec3(0.0);
        if (valid_one) {
            light = Clamp(PtTracePath(origin, d_one, path_weight_one, bounces, pass.f0.x, pass.f0.z, pass.ids.y, Cutoff(), rng, travel_one));
        }
        if (diffuse_one) {
            imageStore(rt_gi_raw, half_pixel, vec4(LightCap(light / (1.0 - p_spec), 64.0), travel_one));
            imageStore(rt_glossy_raw, half_pixel, vec4(0.0, 0.0, 0.0, specular_worth ? 0.0 : -1.0));
        } else {
            imageStore(rt_gi_raw, half_pixel, vec4(0.0, 0.0, 0.0, travel_one));
            imageStore(rt_glossy_raw, half_pixel, vec4(LightCap(light / p_spec, 64.0), travel_one));
        }
        return;
    }
#elif defined(PT_INDIRECT_RENDER_SIZE)
    if (pass.f1.y > 0.5) {
        vec4 mat0_one = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(s.material_u, 0.25));
        vec3 level_one = s.specular * mat0_one.rgb;
        float ls = Luma709(PtSpecularAlbedo(n, V, s));
        bool specular_worth = !(ls < 0.002 || Luma709(level_one) < 1.0e-4);
        float ld = Luma709(s.albedo);
        float p_spec = specular_worth && ls + ld > 1.0e-5 ? clamp(ls / (ls + ld), 0.05, 0.95) : 0.0;
        float travel_one = pass.f0.x;
        if (RtRandom(rng) >= p_spec) {
            uint seed_one = RtHash(uint(half_pixel.x) * 7919u + uint(half_pixel.y) * 104729u);
            vec2 e_one = fract(vec2(float(seed_one & 0xFFFFu), float(seed_one >> 16u)) / 65536.0 + float(pass.ids.z) * vec2(0.7548776662, 0.5698402910));
            vec3 d_one = PtCosineDirection(n, e_one);
            if (dot(d_one, ng) < 0.0) {
                d_one = normalize(d_one - 2.0 * dot(d_one, ng) * ng);
            }
            vec3 light = Clamp(PtTracePath(origin, d_one, vec3(1.0), bounces, pass.f0.x, pass.f0.z, pass.ids.y, Cutoff(), rng, travel_one));
            imageStore(rt_gi_raw, half_pixel, vec4(LightCap(light / (1.0 - p_spec), 64.0), travel_one));
            imageStore(rt_glossy_raw, half_pixel, vec4(0.0, 0.0, 0.0, specular_worth ? 0.0 : -1.0));
            return;
        }
        vec3 w_one;
        vec3 d_one;
        vec3 light = vec3(0.0);
        if (PtSampleSpecular(n, V, s, vec2(RtRandom(rng), RtRandom(rng)), d_one, w_one)) {
            if (dot(d_one, ng) <= 0.0) {
                d_one = normalize(d_one - 2.0 * dot(d_one, ng) * ng);
            }
            light = Clamp(PtTracePath(origin, d_one, w_one / max(level_one, vec3(1.0e-4)), bounces, pass.f0.x, pass.f0.z, pass.ids.y, Cutoff(), rng, travel_one));
        }
        imageStore(rt_gi_raw, half_pixel, vec4(0.0, 0.0, 0.0, travel_one));
        imageStore(rt_glossy_raw, half_pixel, vec4(LightCap(light / p_spec, 64.0), travel_one));
        return;
    }
#endif
    uint seed = RtHash(uint(half_pixel.x) * 7919u + uint(half_pixel.y) * 104729u);
    vec2 e = fract(vec2(float(seed & 0xFFFFu), float(seed >> 16u)) / 65536.0 + float(pass.ids.z) * vec2(0.7548776662, 0.5698402910));
    vec3 dir = PtCosineDirection(n, e);
    float below = dot(dir, ng);
    if (below < 0.0) {
        dir = normalize(dir - 2.0 * below * ng);
    }
    float travel;
    vec3 diffuse = Clamp(PtTracePath(origin, dir, vec3(1.0), bounces, pass.f0.x, pass.f0.z, pass.ids.y, Cutoff(), rng, travel));
    imageStore(rt_gi_raw, half_pixel, vec4(LightCap(diffuse, 64.0), travel));
    vec4 mat0 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(s.material_u, 0.25));
    vec3 level = s.specular * mat0.rgb;
    if (Luma709(PtSpecularAlbedo(n, V, s)) < 0.002 || Luma709(level) < 1.0e-4) {
        imageStore(rt_glossy_raw, half_pixel, vec4(0.0, 0.0, 0.0, -1.0));
        return;
    }
    vec3 weight;
    vec3 specular = vec3(0.0);
    travel = pass.f0.x;
    if (PtSampleSpecular(n, V, s, vec2(RtRandom(rng), RtRandom(rng)), dir, weight)) {
        if (dot(dir, ng) <= 0.0) {
            dir = normalize(dir - 2.0 * dot(dir, ng) * ng);
        }
        specular = Clamp(PtTracePath(origin, dir, weight / max(level, vec3(1.0e-4)), bounces, pass.f0.x, pass.f0.z, pass.ids.y, Cutoff(), rng, travel));
    }
    imageStore(rt_glossy_raw, half_pixel, vec4(LightCap(specular, 64.0), travel));
}
