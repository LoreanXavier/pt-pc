#extension GL_EXT_nonuniform_qualifier : require

struct View {
    mat4 view_projection;
    mat4 view;
    mat4 inv_view;
    vec4 projection_param;
    vec4 exposure;
    vec4 viewport;
    vec4 eye;
    vec4 clip_plane;
    vec4 shadow;
    vec4 jitter;
    vec4 temporal;
    vec4 dominant_light;
};

struct Light {
    vec4 position;
    vec4 color;
    vec4 direction;
    vec4 range;
    vec4 cone;
    vec4 shadow_cone;
    vec4 scales;
    ivec4 info;
    vec4 shadow_rect;
    mat4 area;
    mat4 shadow;
    mat4 mask;
};

struct Probe {
    mat4 box;
    vec4 positive;
    vec4 negative;
    vec4 sh[9];
};

struct Material {
    uint albedo;
    uint normal;
    uint specular;
    uint flags;
    uint aux0;
    uint aux1;
    uint kind;
    uint aux2;
    vec4 albedo_factor;
    vec4 params;
    vec4 indices;
    vec4 extra;
};

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 0, binding = 2) uniform samplerCube cube_textures[];
layout(std430, set = 0, binding = 1) readonly buffer Materials {
    Material materials[];
};

layout(std430, set = 1, binding = 0) readonly buffer FrameData {
    View views[64];
    Light lights[256];
    Probe probes[512];
    vec4 fog[8];
    vec4 mirror;
} frame;

layout(std430, set = 1, binding = 1) readonly buffer SkinData {
    mat4 skin[];
};

layout(set = 1, binding = 2) uniform texture2D images[56];
layout(set = 1, binding = 3) uniform sampler samplers[5];
layout(set = 1, binding = 4) uniform texture3D lut2_image;

#define IMG_ALBEDO 0
#define IMG_NORMAL 1
#define IMG_MATERIAL 2
#define IMG_DEPTH 3
#define IMG_DIFFUSE 4
#define IMG_SPECULAR 5
#define IMG_HDR 6
#define IMG_BLOOM_A 7
#define IMG_BLOOM_B 8
#define IMG_LDR_A 9
#define IMG_LDR_B 10
#define IMG_HISTORY 11
#define IMG_DOF_HALF 12
#define IMG_DOF_QUARTER_A 13
#define IMG_MIRROR 14
#define IMG_SHADOW 15
#define IMG_AO 16
#define IMG_AO_BLUR 17
#define IMG_BLOOM_SUM 18
#define IMG_REFMAP 19
#define IMG_HDR_COPY 20
#define IMG_PARTICLES 46
#define IMG_PROBE_ACC 54
#define IMG_FLARE 47
#define RES_LUT1 24
#define RES_MATERIAL 25
#define RES_DITHER 26
#define RES_COLOR_LUT 27
#define RES_COLOR_LUT_PREV 28
#define RES_NOISE 29
#define RES_MASK 30
#define RES_WHITE 31

#define SMP_POINT_CLAMP 0
#define SMP_LINEAR_CLAMP 1
#define SMP_POINT_WRAP 2
#define SMP_LINEAR_WRAP 3
#define SMP_SHADOW 4

#define MAT_ALPHA_TEST 1u
#define MAT_NORMAL_MAP 2u
#define MAT_SUB_NORMAL 4u
#define MAT_INCIDENCE 8u
#define MAT_TRANSLUCENT_TEX 16u
#define MAT_MICRO_ROUGHNESS 32u
#define MAT_DECAL 64u
#define MAT_CONSTANT_COLOR 128u
#define MAT_SRGB_BASE 256u
#define MAT_COMMON_REFLECTION 512u
#define MAT_VIEW_REFLECTION 1024u
#define MAT_TWO_SIDED 2048u
#define MAT_EYE 16384u
#define MAT_ALPHA_DITHER 32768u
#define MAT_LIGHT_COVER 65536u
#define MAT_DIRECTIVE_ALPHA 131072u
#define MAT_LISA_HAIR_SHADOW 262144u
#define MAT_HAIR 524288u
#define MAT_LAYER 1048576u
#define MAT_NORMAL_WAVE 2097152u
#define MAT_REFLECTOR 4194304u
#define MAT_ALBEDO_VIEW 8388608u

#define KIND_DEFERRED 0u
#define KIND_CONSTANT 1u
#define KIND_GLASS 2u
#define KIND_PARALLAX 3u
#define KIND_SKY 4u
#define KIND_SHADOW_ONLY 5u

vec3 WaveOffset(uint material, vec2 uv0) {
    Material m = materials[material];
    if ((m.flags & MAT_NORMAL_WAVE) == 0u) {
        return vec3(0.0);
    }
    float weight = clamp(m.params.w + exp2(m.albedo_factor.w * log2(abs(clamp(1.0 - uv0.y, 0.0, 1.0)))), 0.0, 1.0);
    return m.albedo_factor.xyz * weight;
}

vec4 Img(uint index, uint smp, vec2 uv) {
    return texture(sampler2D(images[index], samplers[smp]), uv);
}

vec4 ImgLod(uint index, uint smp, vec2 uv, float lod) {
    return textureLod(sampler2D(images[index], samplers[smp]), uv, lod);
}

vec4 ImgFetch(uint index, ivec2 pixel) {
    return texelFetch(sampler2D(images[index], samplers[SMP_POINT_CLAMP]), pixel, 0);
}

vec2 ImgSize(uint index) {
    return vec2(textureSize(sampler2D(images[index], samplers[SMP_POINT_CLAMP]), 0));
}

/* The Bayer rank rotates by temporal.x a frame so a temporal upscaler averages all 64 references; a fixed pattern survives upscaling as a visible texture. */
vec4 MeshDither(vec2 frag, vec4 temporal) {
    vec4 d = ImgFetch(RES_DITHER, ivec2(frag) & 7);
    if (temporal.w > 0.5) {
        float rank = mod(floor((d.x * 255.0 - 2.0) * 0.25 + 0.5) + temporal.x, 64.0);
        d.xy = vec2(4.0 * rank + 2.0, 2.0 * rank + 1.0) / 255.0;
    }
    return d;
}

float AlphaReference(uint flags, vec4 dither) {
    return (flags & MAT_ALPHA_DITHER) != 0u ? dither.y : dither.z;
}

float ShadowAlphaCutoff(uint flags) {
    /* 64/255 is the mean of the 64 dithered references (1..127)/255, so the shadow keeps the hair's coverage without a second animated pattern. */
    return (flags & MAT_LISA_HAIR_SHADOW) != 0u ? 64.0 / 255.0 : 0.5;
}

float ViewZ(View v, float depth) {
    return v.projection_param.z / max(depth, 1.0e-9);
}

vec3 ViewPosition(View v, vec2 ndc, float depth) {
    float z = ViewZ(v, depth);
    return vec3(ndc * v.projection_param.xy * z, z);
}

vec2 PixelNdc(View v, vec2 frag) {
    return frag * v.viewport.zw * 2.0 - 1.0 - v.jitter.xy;
}

vec3 EncodeNormal(vec3 n) {
    return vec3(n.xy * 0.5 + 0.5, sqrt(max(n.z * 0.5 + 0.5, 0.0)));
}

vec3 DecodeNormal(vec3 g) {
    float z = 2.0 * g.z * g.z - 1.0;
    vec2 xy = 2.0 * g.xy - 1.0;
    float s = 1.0 - z * z;
    xy *= s * inversesqrt(s * dot(xy, xy) + 1.0e-7);
    return vec3(xy, z);
}

float Luma709(vec3 c) {
    return dot(c, vec3(0.21252441, 0.71533203, 0.07208252));
}

float Luma601(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 SrgbEncode(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(1.0e-5)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

vec3 SrgbDecode(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}

float Saturate(float x) {
    return clamp(x, 0.0, 1.0);
}

vec3 Saturate(vec3 x) {
    return clamp(x, vec3(0.0), vec3(1.0));
}
