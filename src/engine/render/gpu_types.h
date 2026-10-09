#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace pt::gpu {

constexpr uint32_t kMaxViews = 64;
constexpr uint32_t kMaxLights = 256;
constexpr uint32_t kMaxProbes = 512;
constexpr uint32_t kMaxSkinMatrices = 16384;
constexpr uint32_t kImageSlots = 59;
constexpr uint32_t kInvalid = 0xFFFFFFFFu;
// The luminance buffer (set 1 binding 5) of each frame slot: luminance.comp's partial sums of up to kLuminanceGroups groups,
// then the flashlight reflection's colour (reflect_colour.comp, two vec2: r g, b 1)
constexpr uint32_t kLuminanceGroups = 16384;
constexpr uint32_t kReflectionReadback = kLuminanceGroups;
constexpr uint32_t kLuminanceEntries = kLuminanceGroups + 2;

enum ViewKind : uint32_t { kViewCamera = 0, kViewSpotShadow = 1, kViewParaboloid = 2 };

struct View {
    glm::mat4 view_projection{1.0f};
    glm::mat4 view{1.0f};
    glm::mat4 inv_view{1.0f};
    glm::vec4 projection_param{1.0f, 1.0f, 0.05f, 0.0f};
    glm::vec4 exposure{1.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 viewport{1.0f};
    glm::vec4 eye{0.0f};
    glm::vec4 clip_plane{0.0f};
    glm::vec4 shadow{0.0f};
    glm::vec4 jitter{0.0f};
    // the patterns a temporal upscaler accumulates (upscaling.md, Frame 2), all 0 without one: x the frame's offset of the
    // mesh dither's Bayer rank (MeshDither), y and z the frame's shadow tap offset d of all pixels (ShadowPcf), w 1 when set
    glm::vec4 temporal{0.0f};
    // g_psSystem.m_dominantLightDir in the view's space (SceneRenderer::UpdateDominantLight): the hair highlight's light
    glm::vec4 dominant_light{0.0f, 1.0f, 0.0f, 1.0f};
};

struct Light {
    glm::vec4 position{0.0f};
    glm::vec4 color{0.0f};
    glm::vec4 direction{0.0f};
    glm::vec4 range{0.0f};
    glm::vec4 cone{0.0f};
    glm::vec4 shadow_cone{0.0f};
    glm::vec4 scales{1.0f, 1.0f, 1.0f, 0.0f};
    glm::ivec4 info{-1, -1, 0, 0};
    glm::vec4 shadow_rect{0.0f};
    glm::mat4 area{0.0f};
    glm::mat4 shadow{1.0f};
    glm::mat4 mask{1.0f};
};

struct Probe {
    glm::mat4 box{1.0f};
    glm::vec4 positive{0.0f};
    glm::vec4 negative{0.0f};
    glm::vec4 sh[9]{};
};

struct FrameData {
    View views[kMaxViews];
    Light lights[kMaxLights];
    Probe probes[kMaxProbes];
    glm::vec4 fog[8]{};
    // the mirror capture: x the view index, y its size in pixels (the top left square of the mirror target)
    glm::vec4 mirror{0.0f};
};

struct DrawPush {
    glm::mat4 model{1.0f};
    glm::uvec4 ids{0, 0, 0, kInvalid};
    glm::vec4 tint{1.0f};
    glm::vec4 aux[2]{};
};

struct PassPush {
    glm::uvec4 ids{0};
    glm::vec4 f0{0.0f};
    glm::vec4 f1{0.0f};
    glm::vec4 f2{0.0f};
    glm::mat4 m{1.0f};
};

enum Image : uint32_t {
    kImgAlbedo = 0,
    kImgNormal = 1,
    kImgMaterial = 2,
    kImgDepth = 3,
    kImgDiffuse = 4,
    kImgSpecular = 5,
    kImgHdr = 6,
    kImgBloomA = 7,
    kImgBloomB = 8,
    kImgLdrA = 9,
    kImgLdrB = 10,
    kImgHistory = 11,
    kImgDofHalf = 12,
    kImgDofQuarterA = 13,
    kImgMirror = 14,
    kImgShadow = 15,
    kImgAo = 16,
    kImgAoBlur = 17,
    kImgBloomSum = 18,
    kImgRefMap = 19,
    kImgHdrCopy = 20,
    kImgDofQuarterB = 21,
    kImgDofEighthA = 22,
    kImgDofEighthB = 23,
    kResLut1 = 24,
    kResMaterial = 25,
    kResDither = 26,
    kResColorLut = 27,
    kResColorLutPrev = 28,
    kResNoise = 29,
    kResMask = 30,
    kResWhite = 31,
    kImgVelocity = 32,
    kImgObjectVelocity = 33,
    kImgMbTile = 34,
    kImgMbNeighbour = 39,
    kImgMbBake = 40,
    kImgMbBlurA = 41,
    kImgMbBlurB = 42,
    kImgUpscaled = 43,
    kImgOpaque = 44,
    kImgMotion = 45,
    // the effect buffer of SceneRenderer::RecordEffects (sRGB encoded premultiplied colour, transmittance in alpha)
    kImgParticles = 46,
    // the lens flare buffer of SceneRenderer::RecordPost, added to the encoded scene by the tonemap pass
    kImgFlare = 47,
    kImgMirrorHistory = 48,
    kImgMirrorTemporal = 49,
    // the local reflections' temporal accumulation with a temporal upscaler (SceneRenderer::RecordReflectionTemporal): the
    // reflection layer, where its history lies, and the two histories it alternates between
    kImgReflectLayer = 50,
    kImgReflectOffset = 51,
    kImgReflectHistoryA = 52,
    kImgReflectHistoryB = 53,
    // the SH probes' accumulation (diffuse, transmittance) before SSLighting2_SH_SkyLight_Encode and BilateralUpscale2x2
    kImgProbeAcc = 54,
    // the handy light's demodulation factor under a temporal upscaler (SceneRenderer::RecordUpscaleInputs, upscale_demod.frag)
    kImgHandyFactor = 55,
    // The original near/far effect pass (DownSampleDepth_NearFar and NearFarUpScale2x2): two premultiplied effect layers
    // and the 2x2 full-resolution depth range used to resolve them.
    kImgParticlesNear = 56,
    kImgParticlesFar = 57,
    kImgNearFarDepth = 58,
};

enum Sampler : uint32_t {
    kSmpPointClamp = 0,
    kSmpLinearClamp = 1,
    kSmpPointWrap = 2,
    kSmpLinearWrap = 3,
    kSmpShadow = 4,
    kSmpCount = 5,
};

enum MaterialFlag : uint32_t {
    kMatAlphaTest = 1u << 0,
    kMatNormalMap = 1u << 1,
    kMatSubNormal = 1u << 2,
    kMatIncidence = 1u << 3,
    kMatTranslucentTex = 1u << 4,
    kMatMicroRoughness = 1u << 5,
    kMatDecal = 1u << 6,
    kMatConstantColor = 1u << 7,
    kMatSrgbBase = 1u << 8,
    kMatCommonReflection = 1u << 9,
    kMatViewReflection = 1u << 10,
    kMatTwoSided = 1u << 11,
    kMatMultiShift = 12,
    kMatEye = 1u << 14,
    kMatAlphaDither = 1u << 15,
    // fox3DDF_LightCover: the albedo is the global reflection cube behind the cover, mixed with Base_Tex by the cover mask
    kMatLightCover = 1u << 16,
    // fox3DDF_*_DirectiveAlpha: the alpha falls with the view angle of the normal (StartFadeDot in params.y, EndFadeDot in extra.w)
    kMatDirectiveAlpha = 1u << 17,
    kMatLisaHairShadow = 1u << 18,
    // fox3DDF_Hair: the anisotropic highlight toward the view's dominant light is the stored specular and tints the albedo
    // with Incidence_Color (Shift_Tex in aux0, extra = Anistropic_Diffusion, HairShiftScale, URepeat_UV, VRepeat_UV)
    kMatHair = 1u << 19,
    // fox3DDF_Blin_LayerBl(_SubNorm)_MaskUV: Layer_Tex (aux0) at albedo_factor.xy (albedo_factor.zw + uv1) over the base by
    // LayerMask_Tex (aux2, at uv2) .x times the layer alpha; with a sub normal its weight takes the mask's y
    kMatLayer = 1u << 20,
    // fox3DDF_NormalWave_DirectiveAlpha: the fixed wind bend of the vertices (albedo_factor.xyz, the weight curve in
    // albedo_factor.w and params.w), Specular_Value, Roughness_Value and Translucent_Value in extra.xyz, back faces lit
    kMatNormalWave = 1u << 21,
    // fox3DDF_Reflector: the specular times saturate(n.v)^AnglePow (params.z)
    kMatReflector = 1u << 22,
    // fox3DDF_TempolaryAlbedoView: fixed roughness 0.643 and specular 1, the vertex normal, material index 0
    kMatAlbedoView = 1u << 23,
    kMatGBufferBase2 = 1u << 24,
};

enum MaterialKind : uint32_t {
    kKindDeferred = 0,
    kKindConstant = 1,
    kKindGlass = 2,
    kKindParallax = 3,
    kKindSky = 4,
    kKindShadowOnly = 5,
};

}
