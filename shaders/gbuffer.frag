#version 460
#include "common.glsl"

layout(location = 0) in vec2 in_uv0;
layout(location = 1) in vec2 in_uv1;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec3 in_tangent;
layout(location = 4) in vec3 in_bitangent;
layout(location = 5) in vec3 in_normal;
layout(location = 6) in vec3 in_view_position;
layout(location = 7) in vec3 in_world;
layout(location = 8) in vec3 in_view_dir;
layout(location = 9) in vec2 in_uv2;

layout(push_constant) uniform DrawPush {
    mat4 model;
    uvec4 ids;
    vec4 tint;
    vec4 aux0;
    vec4 aux1;
} draw;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_material;

float g_mip_bias = 0.0;

vec4 Tex(uint index, vec2 uv) {
    return texture(textures[nonuniformEXT(index)], uv, g_mip_bias);
}

vec4 TexPoint(uint index, vec2 uv) {
    ivec2 size = textureSize(textures[nonuniformEXT(index)], 0);
    ivec2 texel = ivec2(fract(uv) * vec2(size)) % size;
    return texelFetch(textures[nonuniformEXT(index)], texel, 0);
}

vec3 ViewReflection(vec3 albedo, vec3 n, vec3 view_dir, float specular, float reflection, float u) {
    vec4 color = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(u, 0.25));
    vec4 param = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(u, 0.75));
    vec3 r = mat3(frame.views[draw.ids.x].inv_view) * reflect(-view_dir, n);
    vec3 env = textureLod(cube_textures[nonuniformEXT(uint(draw.aux1.w))], r, 5.0 * (1.0 - reflection)).rgb;
    float k = param.z * (1.0 - reflection) + reflection;
    float amount = param.z * k;
    float keep = 1.0 - (param.z * (1.0 - specular) + specular) * amount;
    return albedo * keep + specular * amount * param.x * k * color.rgb * env;
}

// Game+ tier 2's torn dress (GamePlusArm): a draw with tint.a above 1.5 tears where its vertex colour's alpha (1 in the cloth,
// falling to 0 at the cut) is under this ragged noise of the surface's texture coordinates, and a negative alpha marks her
// surface over the cut: there it is drawn with the creature's skin material (the vertex colour's r) at its texture coordinates
// (g and b)
float TearHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float TearNoise(vec2 p) {
    float sum = 0.0;
    float amp = 0.55;
    for (int i = 0; i < 3; ++i) {
        vec2 c = floor(p);
        vec2 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        float a = TearHash(c), b = TearHash(c + vec2(1.0, 0.0)), d = TearHash(c + vec2(0.0, 1.0)), e = TearHash(c + vec2(1.0, 1.0));
        sum += amp * mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
        p *= 2.3;
        amp *= 0.45;
    }
    return sum / 0.95;
}

void main() {
    g_mip_bias = frame.views[draw.ids.x].jitter.z;
    float tear_edge = 0.0;
    bool creature = false;
    if (draw.tint.a > 1.5 && in_color.a < 0.999) {
        float noise = 0.15 + 0.85 * TearNoise(in_uv0 * 34.0);
        creature = in_color.a < noise;
        // the frayed edge of the cloth just before a tear, darkened
        tear_edge = creature ? 0.0 : 1.0 - smoothstep(0.0, 0.12, in_color.a - noise);
    }
    Material m = materials[creature ? uint(in_color.r + 0.5) : draw.ids.y];
    uint debug_mode = draw.ids.z & 0xFFu;
    vec2 uv = creature ? in_color.gb : in_uv0;
    vec4 base = Tex(m.albedo, uv);
    if ((m.flags & MAT_CONSTANT_COLOR) != 0u) {
        base = vec4(m.albedo_factor.rgb, 1.0);
    }
    vec4 dither = MeshDither(gl_FragCoord.xy, frame.views[draw.ids.x].temporal);
    float fade = min(draw.tint.a, 1.0);
    float alpha = base.a;
    if ((m.flags & MAT_DIRECTIVE_ALPHA) != 0u) {
        // fox3ddf_translucent_diralp_nc: the interpolated view space normal's |z| between EndFadeDot (0) and StartFadeDot (1)
        alpha = base.a * (abs(in_normal.z) - m.extra.w) / (m.params.y - m.extra.w);
    }
    if (fade - dither.x < 0.0 || alpha - m.params.x * AlphaReference(m.flags, dither) < 0.0) {
        discard;
    }
    vec3 n_geo = normalize(in_normal);
    if ((m.flags & MAT_TWO_SIDED) != 0u && !gl_FrontFacing) {
        n_geo = -n_geo;
    }
    vec3 t_geo = normalize(in_tangent);
    vec3 b_geo = normalize(in_bitangent);
    vec3 n = n_geo;
    if ((m.flags & MAT_NORMAL_MAP) != 0u) {
        vec4 nt = Tex(m.normal, uv);
        vec2 nxy = vec2(nt.w, nt.y) * 2.0 - 1.0;
        float nz = sqrt(Saturate(1.0 - dot(nxy, nxy)) + 1.00016594e-4);
        if ((m.flags & MAT_NORMAL_WAVE) != 0u && !gl_FrontFacing) {
            // fox3ddf_normal_wave_diralp (ps 7c9a7002da0ac9c6) draws both faces and negates the tangent space z on the back
            nz = -nz;
        }
        vec3 nts = vec3(nxy, nz);
        if ((m.flags & MAT_SUB_NORMAL) != 0u) {
            // the layer materials (fox3ddf_blin_layerb_subnm_mu) weight the sub normal by the layer mask's y at the mask UV
            bool layer = (m.flags & MAT_LAYER) != 0u;
            vec4 mask = layer ? Tex(m.aux2, in_uv2) : Tex(m.aux2, uv);
            vec4 sn = Tex(m.aux1, uv * m.extra.yz);
            float k = m.extra.x * (layer ? mask.y : mask.x);
            nts = normalize(vec3(nxy.x + k * (2.0 * sn.w - 1.0), nxy.y + k * (2.0 * sn.y - 1.0), nz));
        }
        n = normalize(t_geo * nts.x + b_geo * nts.y + n_geo * nts.z);
    }
    vec4 srm = Tex(m.specular, uv);
    if ((m.flags & MAT_NORMAL_WAVE) != 0u) {
        // Specular_Value, Roughness_Value and no reflection; Translucent_Value below
        srm = vec4(m.extra.x, m.extra.y, 0.0, 0.0);
    } else if ((m.flags & MAT_ALBEDO_VIEW) != 0u) {
        // fox3ddf_tempolary_albedoview: target 2 = (0.6430664, 1, 0, 0), target 1.w = 0
        srm = vec4(1.0, 0.6430664, 0.0, 0.0);
    }
    float roughness = srm.g;
    float specular = srm.r;
    float reflection = srm.b;
    float index = m.indices.x;
    uint multi = (m.flags >> 12u) & 3u;
    if (multi > 0u) {
        float mk = TexPoint(m.aux0, uv).x;
        if (mk >= 0.25) {
            index = m.indices.y;
        }
        if (multi >= 2u && mk >= 0.5) {
            index = m.indices.z;
        }
        if (multi >= 3u && mk >= 0.75) {
            index = m.indices.w;
        }
    }
    // inViewDir: normalized per vertex, interpolated, normalized again (mesh.vert out_view_dir)
    vec3 view_dir = normalize(in_view_dir);
    vec3 albedo = base.rgb;
    if ((m.flags & MAT_LAYER) != 0u) {
        // fox3ddf_blin_layer_bl_mu, fox3ddf_blin_layerb_subnm_mu: Layer_Tex at URepeat (UShift + uv1) over the decoded base by
        // LayerMask_Tex.x (at the third UV) times the layer's alpha
        vec4 layer = Tex(m.aux0, m.albedo_factor.xy * (m.albedo_factor.zw + in_uv1));
        albedo = mix(albedo, layer.rgb, Tex(m.aux2, in_uv2).x * layer.a);
    }
    albedo *= draw.tint.rgb;
    albedo *= 1.0 - 0.75 * tear_edge;
    if (creature) {
        // hsh0's skin texture is far darker than hers: brightened as its own draw is (GamePlusArm kSkinGain)
        albedo *= 1.5;
    }
    if ((m.flags & MAT_LIGHT_COVER) != 0u) {
        // fox3ddf_lightcover (the NoLnm table's shader for fox3DDF_LightCover_LNM): the view reflected at the internal normal map,
        // in world space, picks the global cube (g_ReflectionTexture, decoded), and the stored albedo is mask * (diffuse - cube) + cube
        // with the diffuse still encoded (outColor0 = t7.x * (t2 - t6) + t6); the _vr variant decodes that before its reflection term
        vec4 it = Tex(m.aux0, uv);
        vec2 ixy = vec2(it.w, it.y) * 2.0 - 1.0;
        vec3 inner = normalize(t_geo * ixy.x + b_geo * ixy.y + n_geo * sqrt(Saturate(1.0 - dot(ixy, ixy)) + 1.00016594e-4));
        vec3 r = mat3(frame.views[draw.ids.x].inv_view) * reflect(-view_dir, inner);
        vec3 cover = texture(cube_textures[nonuniformEXT(uint(draw.aux1.w))], r).rgb;
        albedo = SrgbDecode(mix(cover, SrgbEncode(albedo), Tex(m.aux1, uv).x));
    }
    if ((m.flags & MAT_EYE) != 0u) {
        vec3 v_ts = vec3(dot(view_dir, t_geo), dot(view_dir, b_geo), dot(view_dir, n_geo));
        float h = Tex(m.aux2, uv).y;
        vec3 iris = Tex(m.aux0, uv - 0.25 * m.params.z * h * vec2(v_ts.x, -v_ts.y) / v_ts.z).rgb;
        vec3 sphere = Tex(floatBitsToUint(m.extra.x), vec2(0.5 - 0.5 * n.x, 0.5 - 0.5 * n.y)).rgb;
        albedo = SrgbDecode(Saturate(sphere * srm.r + mix(SrgbEncode(albedo), iris, 1.0 - pow(1.0 - h, 4.0))));
    }
    if ((m.flags & MAT_HAIR) != 0u) {
        // fox3ddf_hair (ps 3ca1dbb1cfa11613): the tangent shifted along the bitangent by HairShiftScale x (2 Shift_Tex.x - 1)
        // (Shift_Tex at the URepeat_UV, VRepeat_UV repeat), the light (the view's dominant light) and the view direction
        // each taken into the frame (shifted tangent, bitangent, normal) and normalized there, h their sum; the highlight
        // is dominant w x specular x saturate(10 |h|)^4 x (1 - (h.b / |h|)^2)^(Anistropic_Diffusion x 2^(7 - 6 gloss)) with the
        // gloss of the material table, and it is the stored specular; Incidence_Color tints the albedo by highlight x the
        // incidence term (alpha x (1 - n.v)^Incidence_Roughness at the interpolated normal)
        vec4 dl = frame.views[draw.ids.x].dominant_light;
        float shift = m.extra.y * (2.0 * Tex(m.aux0, uv * m.extra.zw).x - 1.0);
        vec3 ts = normalize(in_tangent + shift * in_bitangent);
        vec3 l = normalize(dl.xyz);
        vec3 lf = vec3(dot(ts, -l), dot(in_bitangent, -l), dot(in_normal, -l));
        vec3 vf = vec3(dot(ts, view_dir), dot(in_bitangent, view_dir), dot(in_normal, view_dir));
        vec3 h = vf * inversesqrt(dot(vf, vf)) + lf * inversesqrt(dot(lf, lf));
        float hl = length(h);
        float hb = h.y / hl;
        float gloss = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(index, 0.75)).w;
        float highlight = dl.w * srm.r * exp2(log2(abs(Saturate(hl * 10.0))) * 4.0);
        highlight *= exp2(m.extra.x * exp2(-6.0 * gloss + 7.0) * log2(abs(sqrt(max(1.0 - hb * hb, 0.0)))));
        float rim = m.albedo_factor.w * exp2(m.params.z * log2(abs(Saturate(1.0 - dot(n_geo, view_dir)))));
        albedo = SrgbDecode(mix(SrgbEncode(albedo), m.albedo_factor.rgb, highlight * rim));
        specular = highlight;
    }
    if ((m.flags & MAT_INCIDENCE) != 0u) {
        float k = m.albedo_factor.w * srm.r * pow(Saturate(1.0 - dot(n, view_dir)), m.params.z);
        albedo = SrgbDecode(mix(SrgbEncode(albedo), m.albedo_factor.rgb, k));
    }
    if ((m.flags & MAT_MICRO_ROUGHNESS) != 0u) {
        float r18 = 0.5 / max(m.params.w, 1.0e-3);
        float r20 = 1.0 / max(1.0 - r18, 1.0e-3);
        float delta = Saturate(r18 * r20 - dot(n, view_dir) * r20) * (srm.g * m.params.z - srm.g);
        roughness = delta + srm.g;
        float r23 = Saturate(roughness / max(0.001, srm.g));
        reflection = 1.0 - (r23 - srm.b * r23);
    }
    if ((m.flags & MAT_REFLECTOR) != 0u) {
        // fox3ddf_reflector(_vr): the specular falls with the view angle, srm.x saturate(n.v)^AnglePow, in the view reflection too
        specular *= exp2(m.params.z * log2(abs(Saturate(dot(n, view_dir)))));
    }
    if ((m.flags & MAT_VIEW_REFLECTION) != 0u) {
        albedo = ViewReflection(albedo, n, view_dir, specular, reflection, index);
    }
    float translucency = 0.0;
    if ((m.flags & MAT_TRANSLUCENT_TEX) != 0u) {
        translucency = Tex(m.aux1, uv).y;
    } else if ((m.flags & MAT_NORMAL_WAVE) != 0u) {
        translucency = m.extra.z;
    }
    if ((m.flags & MAT_ALBEDO_VIEW) != 0u) {
        // the interpolated vertex normal as it is, material index 0
        n = in_normal;
        index = 0.0;
    }
    if ((m.flags & MAT_DECAL) != 0u) {
        float a = base.a;
        out_albedo = vec4(SrgbEncode(albedo), a);
        out_normal = vec4(EncodeNormal(n), a);
        out_material = vec4(srm.g, srm.r, srm.b, a);
    } else {
        out_albedo = vec4(SrgbEncode(albedo), 1.0);
        out_normal = vec4(EncodeNormal(n), reflection);
        out_material = vec4(roughness, specular, index, translucency);
    }
    if ((m.flags & MAT_GBUFFER_BASE2) != 0u) {
        // GrModelShaders GBuffersBase2 (ps ad9dce8d5b128146): outColor0 (diffuse.r, 0, 0, 1), outColor1 the normal with w 0,
        // outColor2 (specTexture.r, g_psMaterial.m_materials[0].x (1 in the f060 capture), 0.5 + 0.5 motion x, 0.5 - 0.5 motion y)
        // with motion = 0.0078125 x m_renderBuffer.xy x 0.5 x (current NDC - previous NDC), its direction scaled by
        // saturate(length). Its vertex shader takes the "previous" position as m_shadowProjection2 x (m_shadowProjection x
        // inPosition), and in the G-buffer pass m_shadowProjection is all zeros (bath_walk_rt 4001 and bath_base2 4280 cVSScene
        // +0xC0), so that position is (0, 0, 0, 0); the pixel shader divides by its w with v_rcp (inf) and multiplies with
        // v_mul_legacy_f32, where 0 x inf = 0. The "motion" is the pixel's own NDC: 0.4707 (byte 120) and 0.502 (128) at
        // (952, 540) of frame 4280, as captured. The lighting reads that target as (roughness, specular, material index,
        // translucency): only material rows 0 to 58 have a specular colour, which the cap reaches where it sits left of
        // about 13 % from the screen centre (bath_walk_rt 4001: bytes 0 to 4), and then the wet tile's specular and normal
        // map reflect the flashlight and the lamps in the hole, the bath hole's "eye". m_renderBuffer is the original's
        // 1920 x 1080 whatever the port's resolution, so the region stays the same.
        vec4 cur = frame.views[draw.ids.x].view_projection * vec4(in_world, 1.0);
        // the port's projection flips y for Vulkan; the original's clip y points up
        vec2 ndc = vec2(cur.x, -cur.y) / cur.w;
        vec2 d = 0.0078125 * vec2(1920.0, 1080.0) * 0.5 * ndc;
        float l2 = dot(d, d);
        // at l2 = 0 v_rsq gives inf and the legacy multiplies by 0 give 0: (0.5, 0.5)
        vec2 enc = l2 > 0.0 ? vec2(0.5 * d.x * inversesqrt(l2) * Saturate(sqrt(l2)) + 0.5,
                                   -(d.y * inversesqrt(l2) * Saturate(sqrt(l2))) * 0.5 + 0.5)
                            : vec2(0.5);
        out_albedo = vec4(base.r, 0.0, 0.0, 1.0);
        out_normal = vec4(EncodeNormal(n), 0.0);
        out_material = vec4(srm.r, 1.0, enc);
    }
    if (debug_mode == 2u) {
        out_albedo = vec4(fract(in_uv0), 0.0, 1.0);
    } else if (debug_mode == 3u) {
        out_albedo = vec4(in_color.rgb, 1.0);
    }
}
