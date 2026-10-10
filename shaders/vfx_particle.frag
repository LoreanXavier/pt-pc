#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 0, binding = 2) uniform samplerCube cube_textures[];
layout(set = 1, binding = 1) uniform sampler2D depth_texture;
// the scene copy the liquid materials refract (Primitive_Liquid2Final inScreenTexture, taken before the liquid draws)
layout(set = 1, binding = 2) uniform sampler2D scene_copy;
// the frame's TPP fog block (FrameData::fog); mode.x bit 0 fog
layout(set = 1, binding = 3) uniform VfxFog {
    vec4 fog[8];
    uvec4 mode;
} fog_block;

layout(push_constant) uniform VfxPush {
    mat4 view_projection;
    vec4 eye;
    vec4 frame;
    uvec4 ids;
} push;

layout(location = 0) in vec4 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec4 in_params;
layout(location = 3) in vec4 in_luminance;
layout(location = 4) flat in uvec4 in_info;
layout(location = 5) in float in_view_depth;
layout(location = 6) flat in vec4 in_extra;
layout(location = 7) in vec3 in_world;
layout(location = 8) flat in vec3 in_tangent;
layout(location = 9) flat in vec3 in_bitangent;
layout(location = 10) flat in vec3 in_normal;
layout(location = 11) flat in vec4 in_rain_rotation;
layout(location = 12) flat in vec4 in_light_position0;
layout(location = 13) flat in vec4 in_light_position1;
layout(location = 14) flat in vec4 in_light_position2;
layout(location = 15) flat in vec4 in_light_color0;
layout(location = 16) flat in vec4 in_light_color1;
layout(location = 17) flat in vec4 in_light_color2;
layout(location = 18) flat in vec4 in_light_factors;

// The original source buffer 0 is the near layer and uses the minimum reverse-Z sample; source buffer 1 is the far layer.
layout(location = 0) out vec4 out_color_near;
layout(location = 1) out vec4 out_color_far;

const uint kSoft = 1u;
const uint kExposure = 2u;
const uint kLuminance = 4u;
const uint kLiquid = 8u;
const uint kScreen = 16u;
const uint kAnimeBlend = 32u;
const uint kFlare = 64u;
const uint kLiquidHnm = 128u;
const uint kClip = 256u;
const uint kRain = 1024u;
const uint kLit = 2048u;
const uint kNoTexture = 0xFFFFFFFFu;

const uint kBlendAlpha = 0u;
const uint kBlendAdd = 1u;
const uint kBlendSub = 2u;
const uint kBlendMul = 3u;
const uint kBlendMin = 4u;
const uint kBlendOpaque = 5u;

vec3 SrgbToLinear(vec3 c) {
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
}

// the Prim_* effect shaders' output curve: 12.92 c up to 0.0031308, else 1.055 max(c, 1e-5)^(1/2.4) - 0.055, not clamped at 1
vec3 LinearToSrgb(vec3 c) {
    return mix(1.055 * pow(max(c, vec3(1e-5)), vec3(1.0 / 2.4)) - 0.055, c * 12.92, lessThanEqual(c, vec3(0.0031308)));
}

vec3 LitTextureToLinear(vec3 c) {
    vec3 linear = c / 12.92;
    vec3 nonlinear = pow(max(vec3(1.0e-5), 0.94786733 * (vec3(0.055) + c)), vec3(2.4));
    return mix(nonlinear, linear, lessThanEqual(c, vec3(0.03928)));
}

vec3 PointLight(vec4 position, vec4 color) {
    if (dot(color.rgb, color.rgb) < 1.0e-12) {
        return vec3(0.0);
    }
    vec3 delta = position.xyz - in_world;
    float distance_to_light = max(length(delta), 0.01);
    float distance_squared = distance_to_light * distance_to_light;
    float attenuation = max(0.0, 1.0 / distance_squared - distance_squared * position.w);
    return color.rgb * attenuation;
}

// Primitive_Liquid2Final's diffuse term for one point light: pointLightRate (1 - T) saturate((1 - T) N.L + T) saturate(1/d^2 - w d^2)
vec3 LiquidPointLight(vec4 position, vec4 color, vec3 n, float t) {
    if (dot(color.rgb, color.rgb) < 1.0e-12) {
        return vec3(0.0);
    }
    vec3 delta = position.xyz - in_world;
    float distance_to_light = max(length(delta), 0.010002136);
    float distance_squared = distance_to_light * distance_to_light;
    float attenuation = clamp(1.0 / distance_squared - distance_squared * position.w, 0.0, 1.0);
    float diffuse = (1.0 - t) * clamp((1.0 - t) * dot(n, delta / distance_to_light) + t, 0.0, 1.0);
    return color.rgb * attenuation * diffuse;
}

float LinearDepth(float d) {
    return push.frame.y / max(d, 1e-7);
}

// Primitive_Liquid2Final inReflectionTexture: the cube along the world reflection of the view ray (cubesc/cubetc/cubeid/cubema, the
// usual face selection), raw texel values (the cube is bound as UNORM), linear filtering with the implicit level
vec3 ReflectionSample(uint slot, vec3 direction) {
    return texture(cube_textures[nonuniformEXT(slot)], direction).rgb;
}

// Prim_Poly_LitDP3_NS binds its source texture with clamp addressing. The shared
// texture bank uses repeat addressing, so reproduce clamp and trilinear filtering
// here without changing sampling for the other particle materials. A texture scrolled
// past its edge repeats (tub_lit_trace_f060: the f060 bathtub's green dust plane, UV
// rect 0..1 x 0..0.5 moving along u, draws with Prim_Poly_LitDP1_NS and a wrap sampler);
// clamped, its rows stretched the edge texels across the water as stripes.
vec4 LitSample(uint slot, vec2 uv, float computed_lod) {
    if (any(lessThan(uv, vec2(-1.0e-4))) || any(greaterThan(uv, vec2(1.0001)))) {
        return textureLod(textures[nonuniformEXT(slot)], uv, computed_lod);
    }
    int level_count = max(textureQueryLevels(textures[nonuniformEXT(slot)]), 1);
    float lod = clamp(computed_lod, 0.0, float(level_count - 1));
    int low_level = int(floor(lod));
    int high_level = min(low_level + 1, level_count - 1);
    vec2 low_size = vec2(textureSize(textures[nonuniformEXT(slot)], low_level));
    vec2 high_size = vec2(textureSize(textures[nonuniformEXT(slot)], high_level));
    vec2 low_uv = clamp(uv, 0.5 / low_size, 1.0 - 0.5 / low_size);
    vec2 high_uv = clamp(uv, 0.5 / high_size, 1.0 - 0.5 / high_size);
    vec4 low_sample = textureLod(textures[nonuniformEXT(slot)], low_uv, float(low_level));
    vec4 high_sample = textureLod(textures[nonuniformEXT(slot)], high_uv, float(high_level));
    return mix(low_sample, high_sample, fract(lod));
}

// Primitive_Liquid2Final: a coordinate at or past 0.995 of the used area folds back from 0.995, a negative one is mirrored by abs()
float FoldScreen(float u) {
    if (u >= 0.995) {
        u = 0.995 - fract(u / 0.995) * 0.995;
    }
    return abs(u);
}

float SceneDepth(vec2 pixel) {
    return LinearDepth(texture(depth_texture, pixel * push.frame.zw).r);
}

// compose.frag TppFog with the fog block of this pass (the Prim shaders read the same fog from inFogVolume at the particle's depth)
void TppFog(vec3 eye, vec3 point, out vec3 inscatter, out float transmittance) {
    vec4 g0 = fog_block.fog[0];
    vec4 g1 = fog_block.fog[1];
    vec4 mie = fog_block.fog[2];
    vec4 ray = fog_block.fog[3];
    vec3 d = point - eye;
    vec3 dh = 0.01 * d;
    float dist = length(dh);
    float cos_theta = dot(fog_block.fog[7].xyz, dh / max(0.0001, dist));
    float height = max(1.0e-6, 100.0 * g0.y * dh.y);
    float amount = g0.x * pow(max(0.0, dist - g0.z * 0.01), g1.w);
    float depth = (amount * 100.0 - 100.0 * amount * exp(-height)) / height;
    float rayleigh_phase = 0.059683103 * cos_theta * cos_theta + 0.059683103;
    float m = 1.0 - cos_theta * mie.w;
    float mie_phase = 0.07957747 * (1.0 - mie.w * mie.w) / (m * m);
    vec3 sigma = mie.rgb + ray.rgb;
    vec3 directional = ray.w * (mie.rgb * mie_phase + ray.rgb * rayleigh_phase) / max(vec3(1.0e-6), sigma);
    float opacity = 1.0 - exp(-depth);
    vec3 color = g1.rgb + directional * (1.0 - exp(-depth * sigma));
    // no area box term (compose.frag)
    inscatter = clamp(color * opacity, 0.0, 1.0);
    transmittance = 1.0 - opacity;
}

// Draw2D_TppLensFlare ps, 0x8FCA80, 0xC72590
float FlareAlpha(float tex_alpha) {
    float a = in_color.a;
    if (in_luminance.z > 0.0) {
        float len = in_luminance.y * push.frame.x;
        if (len * len < 0.001 && in_luminance.z >= 1.0) {
            a = 0.0;
        }
        a *= mix(1.0, in_luminance.x * push.frame.x / max(len, 1.0), min(in_luminance.z, 1.0));
    }
    a = floor(clamp(a, 0.0, 1.0) * 255.0) / 255.0;
    vec2 pixel = in_params.xy / push.frame.zw;
    float tolerance = in_params.w;
    float v = clamp((SceneDepth(pixel + vec2(0.69, -3.94)) + tolerance - in_params.z) / tolerance, 0.0, 1.0);
    v += clamp((SceneDepth(pixel + vec2(-3.76, 1.37)) + tolerance - in_params.z) / tolerance, 0.0, 1.0);
    v += clamp((SceneDepth(pixel + vec2(3.06, 2.57)) + tolerance - in_params.z) / tolerance, 0.0, 1.0);
    float r = 1.0 - v * 0.33325195;
    return (1.0 - r * r) * tex_alpha * a;
}

void main() {
    const uint flags = in_info.y;
    vec2 base_uv = in_uv.xy;
    if ((flags & kRain) != 0u) {
        vec2 centered = base_uv - in_extra.xy;
        base_uv = in_extra.xy + vec2(in_extra.w * centered.x - in_extra.z * centered.y,
                                    in_extra.z * centered.x + in_extra.w * centered.y);
    }
    // Query derivatives before the per-particle flag branches so the LitDP3 LOD stays defined
    // even when lit and non-lit particles share a fragment wave.
    float lit_lod = textureQueryLod(textures[nonuniformEXT(in_info.x)], base_uv).y;
    float lit_next_lod = textureQueryLod(textures[nonuniformEXT(in_info.x)], in_uv.zw).y;
    vec4 tex = (flags & kLit) != 0u ? LitSample(in_info.x, base_uv, lit_lod) : texture(textures[nonuniformEXT(in_info.x)], base_uv);
    if ((flags & kAnimeBlend) != 0u) {
        vec4 next_tex = (flags & kLit) != 0u ? LitSample(in_info.x, in_uv.zw, lit_next_lod) : texture(textures[nonuniformEXT(in_info.x)], in_uv.zw);
        tex = mix(tex, next_tex, in_params.w);
    }
    vec3 rgb;
    float alpha;
    float alpha_near;
    float alpha_far;
    float transmit = 0.0;
    if ((flags & kFlare) != 0u) {
        rgb = tex.rgb * in_color.rgb;
        alpha = FlareAlpha(tex.a);
    } else if ((flags & kLiquid) != 0u) {
        vec3 nt;
        if ((flags & kLiquidHnm) != 0u) {
            nt.xy = vec2(tex.a, tex.g) * 2.0 - 1.0;
            nt.z = sqrt(clamp(1.0 - dot(nt.xy, nt.xy), 0.0, 1.0)) + 1e-4;
            alpha = in_color.a;
        } else {
            nt = tex.rgb * 2.0 - 1.0 + vec3(0.0, 0.0, 1e-4);
            alpha = tex.a * in_color.a;
        }
        vec3 n = normalize(nt.x * in_tangent + nt.y * in_bitangent + nt.z * in_normal);
        vec3 v = normalize(in_world - push.eye.xyz);
        float t = in_luminance.x;
        float ndv = dot(n, v);
        float through = (1.0 - t) * clamp((1.0 - t) * ndv + t, 0.0, 1.0);
        rgb = in_color.rgb * push.frame.x;
        if (in_light_factors.w > 0.0) {
            vec3 lights = LiquidPointLight(in_light_position0, in_light_color0, n, t) + LiquidPointLight(in_light_position1, in_light_color1, n, t);
            rgb += vec3(unpackHalf2x16(in_info.w), in_extra.w) * lights * (in_light_factors.w * push.frame.x);
        }
        if (push.eye.w > 0.0) {
            // Primitive_Liquid2Final: the scene copy at the pixel (FragCoord - 0.5 + 0.49609375) moved by the world-space normal's x and y
            // times the refraction in 1080p pixels (m_materials[0].w), folded into the copy; colour = inColor * (1 - T) *
            // saturate((1 - T) N.V + T) * scene + T^2 * scene + the lit terms, blended by the texture and particle alpha
            vec2 pixel = gl_FragCoord.xy - 0.00390625 - n.xy * (in_extra.y * push.eye.w);
            vec2 uv = pixel * push.frame.zw;
            vec3 scene = texture(scene_copy, vec2(FoldScreen(uv.x), FoldScreen(uv.y))).rgb;
            vec3 unlit = vec3(unpackHalf2x16(in_info.w), in_extra.w);
            rgb += (unlit * through + t * t) * scene;
        } else {
            transmit = t * t + through * in_luminance.y;
        }
        if (in_info.z != kNoTexture && in_extra.x > 0.0) {
            float fresnel = in_luminance.z + (1.0 - in_luminance.z) * pow(max(1.0 - max(-ndv, 0.0), 1e-5), in_luminance.w);
            vec3 c = ReflectionSample(in_info.z, reflect(v, n));
            rgb += in_extra.x * fresnel * c * c;
        }
        if ((flags & kClip) != 0u) {
            if (alpha < 0.49609375) {
                discard;
            }
            alpha = 1.0;
        }
    } else if ((flags & kRain) != 0u) {
        // Prim_Poly_RainScroll_VF: two alpha samples in screen space, then decode half the modulated vertex colour.
        vec2 pixel = gl_FragCoord.xy - 0.5;
        vec2 scale = in_luminance.xy * push.frame.zw;
        vec2 uv0 = scale * vec2(pixel.x * in_rain_rotation.y - pixel.y * in_rain_rotation.x,
                               pixel.y * in_rain_rotation.y + pixel.x * in_rain_rotation.x) + in_luminance.z;
        vec2 uv1 = scale * vec2(pixel.x * in_rain_rotation.w - pixel.y * in_rain_rotation.z,
                               pixel.y * in_rain_rotation.w + pixel.x * in_rain_rotation.z) + in_luminance.w;
        float rain = in_info.z == kNoTexture ? 1.0 : texture(textures[nonuniformEXT(in_info.z)], uv0).a +
                                                    texture(textures[nonuniformEXT(in_info.z)], uv1).a;
        vec3 c = tex.rgb * in_color.rgb * rain * 0.5;
        rgb = mix(pow(max((c + 0.055) / 1.055, vec3(1e-5)), vec3(2.4)), c / 12.92,
                  lessThanEqual(c, vec3(0.03928)));
        alpha = tex.a * in_color.a;
    } else if ((flags & kScreen) != 0u) {
        rgb = tex.rgb * in_color.rgb;
        alpha = tex.a * in_color.a;
    } else {
        rgb = SrgbToLinear(tex.rgb) * in_color.rgb;
        alpha = tex.a * in_color.a;
    }
    if ((flags & kLit) != 0u) {
        // Prim_Poly_LitDP3_NS evaluates its three point lights at each fragment's world position. Zero-colour
        // lanes are absent; w holds the captured inverse fourth-power radius.
        vec3 point_lights = PointLight(in_light_position0, in_light_color0) + PointLight(in_light_position1, in_light_color1) +
                            PointLight(in_light_position2, in_light_color2);
        vec3 illumination = in_light_factors.xyz + point_lights * in_light_factors.w;
        rgb = LitTextureToLinear(tex.rgb) * in_color.rgb * illumination;
    }
    if ((flags & kExposure) != 0u) {
        rgb *= push.frame.x;
    }
    if ((flags & kLuminance) != 0u) {
        float ev = -log2(max(push.frame.x, 1e-8));
        float t = clamp((ev - in_luminance.x) / max(in_luminance.y - in_luminance.x, 1e-4), 0.0, 1.0);
        rgb *= mix(max(in_luminance.z, 0.0), max(in_luminance.w, 0.0), t);
    }
    const uint blend_mode = push.ids.z;
    // Prim_Poly_FastDraw_DL_VF: alpha blended draws take colour * T + inscatter, the others (m_localParam[3].y < 0) fade by the
    // transmittance (ending_flare_trace: all 126 alpha blended draws have y >= 0, all 753 additive ones y < 0)
    const bool world = push.ids.y != 1u;
    if (world && (fog_block.mode.x & 1u) != 0u && (flags & (kLiquid | kFlare | kScreen)) == 0u) {
        vec3 inscatter;
        float transmittance;
        // the Prim shaders read the volume at the clamped slice of the particle's depth: the fog of view depth clamp(d, 1, far)
        const float fog_depth = clamp(in_view_depth, 1.0, fog_block.fog[0].w);
        TppFog(push.eye.xyz, push.eye.xyz + (in_world - push.eye.xyz) * (fog_depth / max(in_view_depth, 1.0e-4)), inscatter, transmittance);
        if (blend_mode == kBlendAlpha) {
            rgb = rgb * transmittance + inscatter;
        } else {
            alpha *= transmittance;
        }
    }
    if (world) {
        float near_fade = in_params.y;
        float far_fade = in_params.z;
        if (far_fade > 0.0 || near_fade > 0.0) {
            float d = in_view_depth;
            alpha *= d > far_fade ? 1.0 : (d < near_fade ? 0.0 : clamp((d - near_fade) / max(far_fade - near_fade, 1e-3), 0.0, 1.0));
        }
        if ((flags & kSoft) != 0u) {
            if (push.ids.y != 2u) {
                float scene = SceneDepth(gl_FragCoord.xy);
                alpha *= clamp((scene - in_view_depth) * in_params.x, 0.0, 1.0);
            }
        }
    }
    alpha = clamp(alpha, 0.0, 1.0);
    alpha_near = alpha;
    alpha_far = alpha;
    if (world && push.ids.y == 2u && (flags & kSoft) != 0u) {
        // The two half-resolution attachments keep independent soft-depth coverage for the minimum and maximum
        // reverse-Z values of the source 2x2 block. The resolve chooses between them at full resolution.
        vec2 range = texture(depth_texture, gl_FragCoord.xy / vec2(textureSize(depth_texture, 0))).rg;
        alpha_near *= clamp((LinearDepth(range.x) - in_view_depth) * in_params.x, 0.0, 1.0);
        alpha_far *= clamp((LinearDepth(range.y) - in_view_depth) * in_params.x, 0.0, 1.0);
    } else if (world && push.ids.y == 2u) {
        vec2 range = texture(depth_texture, gl_FragCoord.xy / vec2(textureSize(depth_texture, 0))).rg;
        alpha_near *= LinearDepth(range.x) >= in_view_depth ? 1.0 : 0.0;
        alpha_far *= LinearDepth(range.y) >= in_view_depth ? 1.0 : 0.0;
    }
    if (push.ids.w != 0u) {
        out_color_near = vec4(push.ids.y == 0u ? vec3(1.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0), 1.0) * 0.5;
        out_color_far = out_color_near;
        return;
    }
    const uint blend = push.ids.z;
    if (blend == kBlendMul) {
        out_color_far = vec4(mix(vec3(1.0), rgb, alpha_far), alpha_far);
        out_color_near = vec4(mix(vec3(1.0), rgb, alpha_near), alpha_near);
    } else if (blend == kBlendMin) {
        out_color_far = vec4(mix(vec3(65504.0), rgb, alpha_far), alpha_far);
        out_color_near = vec4(mix(vec3(65504.0), rgb, alpha_near), alpha_near);
    } else if (blend == kBlendOpaque) {
        out_color_far = vec4(rgb, 1.0);
        out_color_near = out_color_far;
    } else if (push.ids.y == 2u) {
        // offscreen particle buffer (Prim_Poly_LitDP3_NS and the other Prim_* shaders, lisa_balcony_f070 3881): the lit colour is
        // sRGB encoded, then premultiplied; the pipeline keeps the transmittance in alpha (dst.a (1 - a) for alpha blending)
        vec3 encoded = LinearToSrgb(max(rgb, vec3(0.0)));
        out_color_far = vec4(encoded * alpha_far, alpha_far);
        out_color_near = vec4(encoded * alpha_near, alpha_near);
    } else {
        out_color_far = vec4(rgb * alpha, alpha * (1.0 - transmit));
        out_color_near = out_color_far;
    }
}
