#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) flat in uint in_draw;

layout(location = 0) out vec4 out_color;

struct UiDraw {
    uvec4 textures;
    vec4 m[7];
    vec4 screen;
    vec4 extra;
    vec4 extra2;
};

layout(push_constant) uniform UiPush {
    vec2 inverse_extent;
    uint draw;
} push;

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(std430, set = 1, binding = 0) readonly buffer UiDraws {
    UiDraw draws[];
};
layout(set = 1, binding = 1) uniform sampler2D scene_color;

vec4 Sample(uint packed, vec2 uv) {
    uint index = packed & 0x3FFFFFFFu;
    vec2 size = vec2(textureSize(textures[nonuniformEXT(index)], 0));
    float lod = log2(max(max(length(dFdx(uv) * size), length(dFdy(uv) * size)), 1e-6));
    if ((packed & 0xC0000000u) != 0u) {
        vec2 half_texel = 0.5 / vec2(textureSize(textures[nonuniformEXT(index)], 0));
        if ((packed & 0x80000000u) != 0u) {
            uv.x = clamp(uv.x, half_texel.x, 1.0 - half_texel.x);
        }
        if ((packed & 0x40000000u) != 0u) {
            uv.y = clamp(uv.y, half_texel.y, 1.0 - half_texel.y);
        }
    }
    return textureLod(textures[nonuniformEXT(index)], uv, lod);
}

vec4 ShadeMaterial(UiDraw d) {
    vec2 base_uv = (in_uv - d.m[0].xy) * d.m[1].xy + d.m[0].xy + d.m[0].zw;
    vec2 layer_uv = (in_uv - d.m[1].zw) * d.m[2].zw + d.m[1].zw + d.m[2].xy;
    vec2 mask_uv = (in_uv - d.m[3].xy) * d.m[4].xy + d.m[3].xy + d.m[3].zw;
    vec2 screen = (gl_FragCoord.xy - d.screen.xy) * d.screen.z;
    vec2 screen_uv = (screen - d.m[4].zw) * d.m[5].zw + d.m[4].zw + d.m[5].xy;
    vec4 base = Sample(d.textures.x, base_uv);
    vec4 layer = Sample(d.textures.y, layer_uv);
    vec4 mask = Sample(d.textures.z, mask_uv);
    vec4 noise = Sample(d.textures.w, screen_uv);
    vec3 rgb = clamp(mix(base.rgb, layer.rgb, d.m[6].y), 0.0, 1.0) * in_color.rgb;
    float alpha = clamp(mix(base.a, layer.a, d.m[6].y), 0.0, 1.0) * (d.m[6].z * (mask.g - 1.0) + 1.0);
    alpha = clamp(alpha * (d.m[6].w * (noise.g - 1.0) + 1.0), 0.0, 1.0) * in_color.a;
    return vec4(rgb, alpha);
}

vec4 ShadeText(UiDraw d) {
    float coverage = Sample(d.textures.x, in_uv).r;
    return vec4(in_color.rgb, coverage * in_color.a);
}

// Draw2D_Border, the subtitles' technique: the glyph cache at the pixel and 1.2 pixels to either side across and down (the UV's screen
// derivatives, scaled by d.extra.z so the taps stay 1.2 pixels of the original's 1080p, each offset clamped to +-d.extra.xy, 4 texels).
// A pixel the glyph covers at all is opaque, grey by its coverage; a pixel beside one is black with alpha (the mean of its covered
// neighbours)^0.4. COLOR0 is not read, so the text is white.
vec4 ShadeBorder(UiDraw d) {
    vec2 dx = dFdxFine(in_uv) * d.extra.z;
    vec2 dy = dFdyFine(in_uv) * d.extra.z;
    vec2 lo = -d.extra.xy;
    vec2 hi = d.extra.xy;
    float center = Sample(d.textures.x, in_uv).r;
    float n0 = Sample(d.textures.x, in_uv + clamp(-1.2 * dy, lo, hi)).r;
    float n1 = Sample(d.textures.x, in_uv + clamp(-1.2 * dx, lo, hi)).r;
    float n2 = Sample(d.textures.x, in_uv + clamp(1.2 * dx, lo, hi)).r;
    float n3 = Sample(d.textures.x, in_uv + clamp(1.2 * dy, lo, hi)).r;
    float sum = n0 + n1 + n2 + n3;
    float covered = float(n0 > 0.0) + float(n1 > 0.0) + float(n2 > 0.0) + float(n3 > 0.0);
    float any_ink = center + sum >= 0.0001 ? 1.0 : 0.0;
    float rim = covered > 0.0 ? pow(sum / covered, 0.4) : 0.0;
    return vec4(vec3(center * any_ink), (center > 0.0 ? 1.0 : rim) * any_ink);
}

vec3 SceneDisplay(vec2 uv, UiDraw d) {
    vec3 hdr = texture(scene_color, uv).rgb * d.extra2.x;
    return pow(clamp(hdr, 0.0, 1.0), vec3(1.0 / max(d.extra2.y, 0.001)));
}

vec4 ShadeNoise(UiDraw d) {
    float phase = d.extra.x;
    // the noise textures span the original's 16:9 frame: d.extra2.z is the window's width in such frames (0 taken as 1)
    float across = d.extra2.z > 0.0 ? d.extra2.z : 1.0;
    vec2 noise_uv = vec2((in_uv.x - 0.5) * across + 0.5, in_uv.y);
    vec4 normal = Sample(d.textures.y, vec2(phase + noise_uv.x, d.extra.y + noise_uv.y));
    vec2 uv = clamp(in_uv + vec2((-0.001 + normal.w * 0.002) / across, -0.03 + normal.y * 0.06), vec2(0.0), vec2(1.0));
    vec3 screen = SceneDisplay(uv, d);
    float noise = Sample(d.textures.x, vec2(phase + noise_uv.x, noise_uv.y)).x;
    vec3 rgb;
    rgb.r = clamp(clamp(d.extra.z * 0.4 + (noise - screen.r) * 0.1 + screen.r, 0.0, 1.0) * in_color.r, 0.0, 1.0);
    rgb.g = clamp(((noise - screen.g) * 0.1 + screen.g) * in_color.g, 0.0, 1.0);
    rgb.b = clamp(((noise - screen.b) * 0.1 + screen.b) * in_color.b, 0.0, 1.0);
    return vec4(rgb, clamp(d.extra.w * in_color.a, 0.0, 1.0));
}

vec4 ShadeString(UiDraw d) {
    vec4 t = Sample(d.textures.x, d.extra.xy + in_uv);
    // Draw2D_ShSubliminalString writes 0.1 x colour into the original's sRGB target (img f43), which stores it encoded: 0.35, a
    // light grey hand. The UI target here holds encoded values, so the 0.1 is encoded first (it wrote 0.1 raw: near black)
    vec3 c = clamp(0.1 * in_color.rgb, 0.0, 1.0);
    vec3 encoded = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, 12.92 * c, lessThanEqual(c, vec3(0.0031308)));
    return vec4(encoded, clamp(d.extra.w * t.a * in_color.a, 0.0, 1.0));
}

void main() {
    UiDraw d = draws[in_draw];
    uint mode = uint(d.screen.w + 0.5);
    if (mode == 1u) {
        out_color = ShadeText(d);
    } else if (mode == 2u) {
        out_color = ShadeNoise(d);
    } else if (mode == 3u) {
        out_color = ShadeString(d);
    } else if (mode == 4u) {
        out_color = in_color;
    } else if (mode == 5u) {
        out_color = ShadeBorder(d);
    } else {
        out_color = ShadeMaterial(d);
    }
}
