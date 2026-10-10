#pragma once

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/render/camera.h"
#include "engine/platform/settings.h"
#include "engine/render/dominant_light.h"
#include "engine/render/gpu_types.h"
#include "engine/render/light_cull.h"
#include "engine/render/mesh.h"
#include "engine/render/raytracing.h"
#include "engine/render/render_util.h"
#include "engine/render/renderer.h"
#include "engine/render/scene_lighting.h"
#include "engine/render/subsurface_pass.h"
#include "engine/render/texture_manager.h"
#include "engine/render/upscale/upscale.h"

namespace pt {

class Vfs;

enum class DebugView : int {
    Lit = 0,
    Normals = 1,
    UV0 = 2,
    VertexColor = 3,
    Albedo = 4,
    Roughness = 5,
    Specular = 6,
    MaterialIndex = 7,
    Depth = 8,
    Translucency = 9,
    LightDiffuse = 10,
    LightSpecular = 11,
    Ambient = 12,
    Reflection = 13,
    Occlusion = 14,
    Unlit = 15,
    Mirror = 16,
    Count = 17,
};

const char* DebugViewNames();

struct SceneVfxContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 view_projection{1.0f};
    glm::vec3 eye{0.0f};
    VkExtent2D extent{};
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkImageView depth_view = VK_NULL_HANDLE;
    // the RG half-resolution min/max reverse-Z depth range sampled by the effects pass
    VkImageView near_far_depth_view = VK_NULL_HANDLE;
    float exposure = 1.0f;
    uint32_t frame_index = 0;
    bool mirrored = false;
    // copies the HDR target into scene_copy_view within the forward pass (it ends and resumes the pass), for the liquid materials
    VkImageView scene_copy_view = VK_NULL_HANDLE;
    std::function<void()> copy_scene;
    // the frame's TPP fog block (FrameData::fog) and mode (bit 0 fog, bit 2 area fog), 0 without fog
    glm::vec4 fog[8]{};
    uint32_t fog_mode = 0;
    // which world draws to record: kVfxAll, kVfxScene (liquids, multiply, min and opaque draws, which stay on the scene target) or
    // kVfxOffscreen (alpha, additive and subtractive draws, into the effect buffer: sRGB encoded, premultiplied, transmittance in alpha)
    uint32_t subset = 0;
    // incremented by the number of draws recorded
    uint32_t* recorded = nullptr;
};

constexpr uint32_t kVfxAll = 0;
constexpr uint32_t kVfxScene = 1;
constexpr uint32_t kVfxOffscreen = 2;

struct SceneFilterContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    VkImageView source = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    float exposure = 1.0f;
    uint32_t frame_index = 0;
    // the effect layers to draw: bit 0 the lens flares, bit 1 the screen sprites
    uint32_t layers = 3;
};

struct DrawItem {
    const GpuMesh* mesh = nullptr;
    glm::mat4 transform{1.0f};
    glm::vec4 tint{1.0f};
    int32_t material_override = -1;
    std::span<const glm::mat4> skin;
    MeshMask hidden_meshes;
    // Views the model is hidden in, bit n for the views of index n: a model instance's mask (+0x1AC) is tested against
    // 1 << the view's index (+0x51C, 0xD5F1E0); index 0 is the camera view, 1 the MirrorCapture views (0x959070)
    uint8_t hidden_views = 0;
    bool character_shadow = false;
    // the object the draw belongs to (a level model's placement, a demo's model), 0 when not known: the frame's motion history
    // and the window's blend between ticks pair a draw with the last one of the same source and mesh (DrawPairKeys)
    uint64_t source = 0;
};

// Keys pairing each draw with its draw of the previous frame or tick: the source and mesh, and the occurrence among the draws
// of that source and mesh (draws without a source pair by mesh and occurrence among those). Pairing by mesh and occurrence
// alone handed one model's transform to another when a model of the same mesh appeared earlier in the list: on a first start
// the hallway's door (shsb_hous001_door001_hallway_in) turns visible while gc_p00_160's door, the same mesh, swings open
// behind the player, and the window's blend drew the hallway door half way to the swinging door's pose for one frame
class DrawPairKeys {
public:
    uint64_t Next(const DrawItem& item) {
        const uint64_t mesh = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(item.mesh));
        const uint64_t base = item.source ? (item.source * 0x9E3779B97F4A7C15ull) ^ (mesh * 0xC2B2AE3D27D4EB4Full) : mesh << 12;
        return base + 0xD6E8FEB86659FD93ull * occurrences_[base]++;
    }

private:
    std::unordered_map<uint64_t, uint32_t> occurrences_;
};

struct RenderToggles {
    bool lights = true;
    bool probes = true;
    bool shadows = true;
    bool bloom = true;
    bool color_lut = true;
    bool fxaa = true;
    bool depth_of_field = true;
    bool film_grain = true;
    bool distortion = true;
    bool full_screen_blur = true;
    bool adaptation = true;
    bool mirrors = true;
    bool tpp_atmosphere = true;
    bool occlusion = true;
    bool local_reflections = true;
    bool motion_blur = true;
    // PT_SUBSURFACE=0 turns the ending's subsurface scattering off (comparisons)
    bool subsurface_scatter = true;
};

// a light's box in the scene grid (SceneRenderer's LightGridBox)
struct LightBox {
    glm::vec3 lo{0.0f};
    glm::vec3 hi{0.0f};
};

struct RenderStats {
    uint32_t draws = 0;
    uint32_t lights = 0;
    uint32_t probes = 0;
    uint32_t shadow_views = 0;
    float ev = 0.0f;
    float exposure = 1.0f;
    float luminance = 0.0f;
    float gpu_ms = 0.0f;
    float cpu_ms = 0.0f;
    float pass_ms[6] = {};
    // finer splits of the passes above: forward draws (of compose), effects (of compose), upscale inputs, reflections, upscaler,
    // bloom (of post)
    float part_ms[6] = {};
    // the port's device memory (VMA blocks) when the frame was recorded, in MiB
    float device_mb = 0.0f;
    // the flashlight reflection's colour (SceneRenderer::RecordReflectionSample) and how many have come back
    glm::vec3 reflection_readback{0.0f};
    uint64_t reflection_readbacks = 0;
};

struct UpscaleStats {
    bool active = false;
    UpscalerKind kind = UpscalerKind::Off;
    VkExtent2D render{};
    VkExtent2D output{};
    float upscale_ms = 0.0f;
    float inputs_ms = 0.0f;
    std::string error;
};

class SceneRenderer {
public:
    bool Init(Renderer& renderer, TextureManager& textures);
    void Shutdown();
    bool LoadResources(Vfs& vfs);

    std::unique_ptr<GpuMesh> Upload(const MeshData& data);
    void Destroy(GpuMesh& mesh);

    void Render(const Camera& camera, const std::vector<DrawItem>& items);
    // the next frame's temporal upscaler starts without history
    void ResetUpscaleHistory() { up_.reset = true; }
    void Render(const Camera& camera, const std::vector<DrawItem>& items, const SceneLighting& lighting, float dt);
    void DrawDebugUi();
    void ResetExposure() { adaptation_valid_ = false; }
    void SetMotionReference(const Camera& previous, float interval, bool history);
    // the time of one pass of the main loop, rendered or not: the dominant light search (UpdateDominantLight) runs once a game
    // frame in the original, so its fade advances by the time since the last render (headless runs render only their shots)
    void AdvanceTime(float seconds) {
        pending_time_ += seconds;
        timed_ = true;
    }

    DebugView debug_view = DebugView::Lit;
    RenderToggles toggles;
    AppSettings::Graphics graphics;
    static constexpr VkFormat kHdrTargetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthTargetFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kPostTargetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // 32-bit as the original's ScaleDownDepth (D32): 16-bit floats quantized the reverse-Z range, and the near/far blend drew depth contours over
    // effects in front of a sloping surface (the bathtub water's "barcode" lines, longer under the zoom's nearer clip)
    static constexpr VkFormat kNearFarDepthFormat = VK_FORMAT_R32G32_SFLOAT;
    // Shadow atlas tiles: the original renders every spot shadow into its 2048x2048 target and every point shadow into the
    // 4096x2048 paraboloid target (shadow_f010 1706), one light at a time; the atlas holds 4x4 tiles of 2048, room for the
    // view's 8 shadowed lights (12.5) even when all are points
    static constexpr uint32_t kShadowTile = 2048;
    static constexpr uint32_t kShadowAtlasSize = 8192;
    std::function<void(const SceneVfxContext&)> vfx_forward;
    std::function<void(const SceneFilterContext&)> vfx_filter;
    const RenderStats& Stats() const { return stats_; }
    UpscaleSettings upscale;
    const UpscaleStats& UpscaleStatistics() const { return upscale_stats_; }
    bool UpscalerAvailable(UpscalerKind kind, std::string& reason);
    // PT_TARGET_DUMP (a tool for comparisons with the capture's render target dumps): the next rendered frame keeps raw
    // copies of the main view's HDR image after the composition, the forward draws and the effects; DumpTargets then
    // writes those and the G-buffer and light accumulation targets as <prefix>.<name>.bin with <prefix>.targets.txt
    void RequestTargetDump() { dump_requested_ = true; }
    bool DumpTargets(const std::string& prefix);
    // PC option (rendering.md 12.21): applies at once when the device was created with ray queries, else at the next start
    RayTracingSettings raytracing;
    bool RayTracingReady() const { return rt_ != nullptr; }
    bool RayTracingSupported(std::string& reason) const;
    // VR (docs/vr.md): the eye the next Render draws, 0 or 1, -1 without VR. Both eyes leave out the screen effects that do not
    // belong to a view per eye (depth of field, motion blur, film grain, lens distortion, the full screen blur and its history,
    // the lens flares and the screen sprites); the second eye repeats the first eye's frame (the frame counter, which drives the
    // time, the temporal patterns and the ray tracing structures, does not advance)
    void SetVrEye(int eye) { vr_eye_ = eye; }

private:
    struct FrameSlot {
        vk::Buffer frame_data;
        vk::Buffer skin;
        vk::Buffer luminance;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSet post_set = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE;
        VkQueryPool upscale_queries = VK_NULL_HANDLE;
        bool upscale_timed = false;
        uint32_t luminance_groups = 0;
        float exposure_used = 1.0f;
        bool measured = false;
        bool timed = false;
        bool reflection_sampled = false;
    };

    struct Draw {
        const GpuMesh* mesh = nullptr;
        const SubMesh* sub = nullptr;
        glm::mat4 transform{1.0f};
        glm::vec4 tint{1.0f};
        uint32_t material = 0;
        uint32_t skin_base = gpu::kInvalid;
        uint32_t previous_model = gpu::kInvalid;
        uint32_t previous_skin = gpu::kInvalid;
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        uint8_t hidden_views = 0;
        bool character_shadow = false;
    };

    struct MotionHistory {
        glm::mat4 transform{1.0f};
        std::vector<glm::mat4> skin;
    };

    struct ShadowView {
        uint32_t view = 0;
        uint32_t light = 0;
        uint8_t casters = 1;
        VkRect2D rect{};
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        glm::vec4 planes[6]{};
        bool frustum = false;
        float bias_constant = 0.0f;
        float bias_slope = 0.0f;
        VkCullModeFlags cull = VK_CULL_MODE_FRONT_BIT;
    };

    struct ViewSetup {
        uint32_t index = 0;
        glm::vec4 planes[6]{};
        bool mirrored = false;
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec3 eye{0.0f};
        uint8_t view_bit = 1;
        // the render area, from the top left of the targets (zero: the whole extent)
        VkExtent2D area{0, 0};
    };

    VkExtent2D ViewArea(const ViewSetup& view) const { return view.area.width > 0 ? view.area : extent_; }
    bool CreatePipelines();
    void DestroyPipelines();
    bool CreateDescriptors();
    bool EnsureTargets(VkExtent2D extent, VkExtent2D output, bool upscaling);
    void DestroyTargets();
    void WriteImageDescriptors();
    bool CreateTarget(RenderTarget& target, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    bool UploadImage(vk::Image& image, VkFormat format, VkExtent3D extent, const void* data, size_t size);
    bool UploadImageMips(vk::Image& image, VkFormat format, VkExtent3D extent, const std::vector<std::vector<uint8_t>>& mips);
    void CreateBuiltinResources();
    void UpdateColorLut(const ScreenSettings& screen);
    bool BuildColorLut(const std::string& path, const ScreenSettings& screen, std::vector<uint8_t>& out);

    void PrepareFrame(const Camera& camera, const std::vector<DrawItem>& items, const SceneLighting& lighting);
    void BuildShadowViews(const SceneLighting& lighting, const glm::vec3& eye);
    void UploadFrame(FrameSlot& slot);
    void SetTppFog(const TppAtmosphereSettings& tpp);
    void ReadMeasurements(FrameSlot& slot, float dt, const ExposureSettings& settings);
    void SettleExposure(const ExposureSettings& settings);
    float ExposureFor(float ev, const ExposureSettings& settings) const;

    void BindSets(VkCommandBuffer cmd, VkPipelineBindPoint point);
    void RecordShadows(VkCommandBuffer cmd);
    void RecordRayTracing(VkCommandBuffer cmd);
    void RecordView(VkCommandBuffer cmd, const ViewSetup& view, RenderTarget& output, bool main_view);
    void RecordGBuffer(VkCommandBuffer cmd, const ViewSetup& view);
    void RecordOcclusion(VkCommandBuffer cmd, const ViewSetup& view);
    bool EnsureAoTargets();
    bool EnsureShadowTarget();
    void RecordRtAmbientOcclusion(VkCommandBuffer cmd, const ViewSetup& view);
    void RecordLighting(VkCommandBuffer cmd, const ViewSetup& view);
    void RecordLuminance(VkCommandBuffer cmd);
    void RecordForward(VkCommandBuffer cmd, const ViewSetup& view, RenderTarget& output);
    void RecordPost(VkCommandBuffer cmd, const SceneLighting& lighting, float dt);
    void RecordReflectionSample(VkCommandBuffer cmd, const SceneLighting& lighting);
    void RecordReflections(VkCommandBuffer cmd, const ViewSetup& view);
    // the local reflections' layer, its temporal accumulation and the mix into the scene, with a temporal upscaler (12.16)
    void RecordReflectionTemporal(VkCommandBuffer cmd, const ViewSetup& view, gpu::PassPush push);
    void RecordMirrorTemporal(VkCommandBuffer cmd);
    bool RecordDepthOfField(VkCommandBuffer cmd, const ScreenSettings& screen, int& current);
    bool RecordMotionBlur(VkCommandBuffer cmd, const ScreenSettings& screen, int& current);
    bool MotionBlurActive() const;
    void RecordObjectVelocity(VkCommandBuffer cmd, const ViewSetup& view);
    void PostPass(VkCommandBuffer cmd, RenderTarget& target, VkPipeline pipeline, const gpu::PassPush& push);
    SceneFilterContext FilterContext(VkCommandBuffer cmd, uint32_t layers) const;
    void UpdateMotion(const Camera& camera, float dt);
    void RecordDebug(VkCommandBuffer cmd);
    void DrawMesh(VkCommandBuffer cmd, const Draw& draw, uint32_t view, uint32_t flags, bool mirrored, bool no_cull = false,
                  const glm::vec4* aux = nullptr, VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT, bool shadow_pass = false);
    uint32_t ForwardLights(const Draw& draw, glm::vec4 aux[2], uint8_t view_bit) const;
    void UpdateDominantLight(const Camera& camera, const SceneLighting& lighting, float dt);
    uint32_t MirrorLights(const Draw& draw, glm::vec4 aux[2]) const;
    void UpdateReflectionTexture(const std::string& path);
    void ApplyEnvironmentOverrides();
    void Stamp(VkCommandBuffer cmd, uint32_t index);
    void ReadTimestamps(FrameSlot& slot);
    void PushConstants(VkCommandBuffer cmd, const void* data, uint32_t size);
    void CopyToHistory(VkCommandBuffer cmd, const RenderTarget& source);
    void Fullscreen(VkCommandBuffer cmd, VkPipeline pipeline, const gpu::PassPush& push);
    bool Visible(const Draw& draw, const glm::vec4* planes) const;
    bool Visible(const Draw& draw, const ViewSetup& view) const;

    bool CreateUpscalePipelines();
    void DestroyUpscalePipelines();
    bool CreateUpscaleTargets(VkExtent2D render, VkExtent2D output);
    void DestroyUpscaleTargets();
    void WriteUpscaleDescriptors(VkDescriptorImageInfo* images, bool post);
    bool BeginUpscaleFrame(VkExtent2D output);
    bool SetupUpscaler(VkExtent2D output);
    void RecordOpaqueSnapshot(VkCommandBuffer cmd, RenderTarget& output);
    void RecordSceneCopy(VkCommandBuffer cmd, RenderTarget& output);
    void RecordDumpCopy(VkCommandBuffer cmd, RenderTarget& target, vk::Buffer& buffer);
    void RecordEffects(VkCommandBuffer cmd, RenderTarget& output, SceneVfxContext context);
    void RecordUpscaleInputs(VkCommandBuffer cmd, const ViewSetup& view);
    void RecordUpscale(VkCommandBuffer cmd, float dt);
    void ReadUpscaleTimestamps(FrameSlot& slot);

    // 0 start, 1 shadows, 2 mirror, 3 G-buffer, 4 lighting, 5 compose, forward and effects, 6 end; within them 7 the forward draws,
    // 8 the upscale inputs, 9 the reflections, 10 the upscaler, 11 the bloom (written on every frame, in place when a pass is off)
    static constexpr uint32_t kTimestamps = 12;
    static constexpr uint32_t kReflectMapSize = 512;
    static constexpr float kCameraCutDistance = 3.0f;

    Renderer* renderer_ = nullptr;
    VkFormat ldr_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    TextureManager* textures_ = nullptr;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    glm::vec3 last_eye_{0.0f};
    // DominantLightSearch (ShEffet, 0x8D12A0): the camera view's g_psSystem.m_dominantLightDir in world space (w the fade)
    DominantLightState dominant_;
    float pending_time_ = 0.0f;
    bool timed_ = false;
    bool has_last_eye_ = false;
    uint32_t last_timed_slot_ = Renderer::kFramesInFlight;
    VkDevice device_ = VK_NULL_HANDLE;
    FrameSlot slots_[Renderer::kFramesInFlight];
    VkDescriptorSetLayout frame_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkSampler samplers_[gpu::kSmpCount] = {};

    VkExtent2D extent_{0, 0};
    RenderTarget albedo_;
    RenderTarget normal_;
    RenderTarget material_;
    RenderTarget depth_;
    RenderTarget diffuse_;
    RenderTarget specular_;
    RenderTarget probe_acc_;
    RenderTarget hdr_;
    RenderTarget bloom_[3];
    RenderTarget flare_;
    RenderTarget ldr_[2];
    RenderTarget history_;
    RenderTarget dof_half_;
    RenderTarget dof_quarter_[2];
    RenderTarget dof_eighth_[2];
    RenderTarget velocity_;
    RenderTarget object_velocity_;
    RenderTarget mb_tile_[5];
    RenderTarget mb_neighbour_;
    RenderTarget mb_bake_;
    RenderTarget mb_blur_[2];
    RenderTarget mirror_;
    RenderTarget mirror_history_;
    RenderTarget mirror_temporal_;
    // with upscaling (12.16, PC): the reflection layer (premultiplied colour, amount), where its history lies, and its history
    RenderTarget reflect_layer_;
    RenderTarget reflect_offset_;
    RenderTarget reflect_history_[2];
    RenderTarget shadow_;
    RenderTarget ao_[2];
    RenderTarget refmap_;
    RenderTarget hdr_copy_;
    // RecordForward's full-resolution encoded blend target, plus the original half-resolution near/far effect layers and their
    // downsampled depth range / min-depth attachment (DownSampleDepth_NearFar, NearFarUpScale2x2).
    RenderTarget particles_;
    RenderTarget particles_near_;
    RenderTarget particles_far_;
    RenderTarget near_far_depth_;
    RenderTarget near_far_depth_attachment_;
    RenderTarget motion_;
    RenderTarget reactive_;
    // the handy light's cone of the last reactive pass (position, cos outer; direction, cone range), w -2 when it was off
    glm::vec4 previous_handy_[2] = {glm::vec4(0.0f, 0.0f, 0.0f, -2.0f), glm::vec4(0.0f)};
    RenderTarget opaque_;
    RenderTarget handy_factor_;
    // this frame's upscaler colour is the handy light demodulated copy in opaque_ (upscale_demod.frag)
    bool handy_demod_ = false;
    RenderTarget exposure_image_;
    RenderTarget upscaled_;
    RenderTarget post_hdr_;
    RenderTarget post_depth_;
    RenderTarget post_object_velocity_;
    bool dump_requested_ = false;
    bool dump_recorded_ = false;
    vk::Buffer dump_compose_;
    vk::Buffer dump_forward_;
    vk::Buffer dump_scene_;
    // the mirror view's own target: the capture the mirror material samples
    vk::Buffer dump_mirror_;
    VkExtent2D output_extent_{0, 0};
    bool targets_upscaled_ = false;
    bool history_valid_ = false;

    vk::Image lut1_;
    vk::Image lut2_;
    vk::Image material_tex_;
    vk::Image dither_;
    vk::Image color_lut_;
    vk::Image color_lut_prev_;
    vk::Image noise_;
    vk::Image mask_;
    vk::Image white_;
    std::string color_lut_key_;
    std::string color_lut_prev_key_;
    std::string reflection_key_;
    uint32_t reflection_cube_ = 0;
    Vfs* vfs_ = nullptr;

    VkPipeline gbuffer_ = VK_NULL_HANDLE;
    VkPipeline decal_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipeline probe_ = VK_NULL_HANDLE;
    VkPipeline probe_resolve_ = VK_NULL_HANDLE;
    VkPipeline light_ = VK_NULL_HANDLE;
    VkPipeline compose_ = VK_NULL_HANDLE;
    VkPipeline forward_ = VK_NULL_HANDLE;
    VkPipeline luminance_ = VK_NULL_HANDLE;
    VkPipeline reflect_colour_ = VK_NULL_HANDLE;
    VkPipeline occlusion_ = VK_NULL_HANDLE;
    VkPipeline occlusion_blur_ = VK_NULL_HANDLE;
    VkPipeline bright_ = VK_NULL_HANDLE;
    VkPipeline kawase_ = VK_NULL_HANDLE;
    VkPipeline forward_emissive_ = VK_NULL_HANDLE;
    VkPipeline bloom_add_ = VK_NULL_HANDLE;
    VkPipeline kawase_sum_ = VK_NULL_HANDLE;
    VkPipeline gaussian_ = VK_NULL_HANDLE;
    VkPipeline tonemap_ = VK_NULL_HANDLE;
    VkPipeline fxaa_ = VK_NULL_HANDLE;
    VkPipeline mirror_temporal_pipeline_ = VK_NULL_HANDLE;
    VkPipeline dof_ratio_ = VK_NULL_HANDLE;
    VkPipeline dof_down_ = VK_NULL_HANDLE;
    VkPipeline dof_blur_ = VK_NULL_HANDLE;
    VkPipeline dof_blend_ = VK_NULL_HANDLE;
    VkPipeline mb_velocity_ = VK_NULL_HANDLE;
    VkPipeline mb_tile_pipeline_ = VK_NULL_HANDLE;
    VkPipeline mb_bake_pipeline_ = VK_NULL_HANDLE;
    VkPipeline mb_mcguire_ = VK_NULL_HANDLE;
    VkPipeline mb_composite_ = VK_NULL_HANDLE;
    VkPipeline velocity_pipeline_ = VK_NULL_HANDLE;
    VkPipeline fsblur_ = VK_NULL_HANDLE;
    VkPipeline banding_ = VK_NULL_HANDLE;
    VkPipeline screen_fx_ = VK_NULL_HANDLE;
    VkPipeline debug_ = VK_NULL_HANDLE;
    VkPipeline reflect_make_ = VK_NULL_HANDLE;
    VkPipeline reflect_blend_ = VK_NULL_HANDLE;
    VkPipeline reflect_layer_pipeline_ = VK_NULL_HANDLE;
    VkPipeline reflect_temporal_pipeline_ = VK_NULL_HANDLE;
    // this frame's accumulated floor reflection is mixed in after the upscaler (upscale_resolve.frag), not into hdr_
    bool reflect_post_upscale_ = false;
    // the main view's shadow limit last logged (BuildShadowViews: 3 or, with the view byte +0x5D4 on f110, 10)
    uint32_t shown_shadow_limit_ = 3;
    VkPipeline vfx_composite_ = VK_NULL_HANDLE;
    VkPipeline vfx_depth_down_ = VK_NULL_HANDLE;
    VkPipeline up_motion_ = VK_NULL_HANDLE;
    VkPipeline up_object_motion_ = VK_NULL_HANDLE;
    VkPipeline up_reactive_ = VK_NULL_HANDLE;
    VkPipeline up_resolve_ = VK_NULL_HANDLE;
    VkPipeline up_demod_ = VK_NULL_HANDLE;

    // ray traced shadows: created only on a device with ray queries (vk::Context::ray_query)
    std::unique_ptr<RayTracing> rt_;
    // the SUBSURFACE_SCATTER plugin (rendering.md 12.26), only on the ending
    SubsurfacePass sss_;
    bool sss_ready_ = false;
    VkPipeline light_rt_ = VK_NULL_HANDLE;
    bool rt_active_ = false;
    // ray traced local reflections (12.21): the reflection map traced against the TLAS, off screen hits shaded there
    bool rt_reflections_ = false;
    VkPipeline reflect_make_rt_ = VK_NULL_HANDLE;
    VkPipeline reflect_blend_rt_ = VK_NULL_HANDLE;
    VkPipeline reflect_layer_rt_ = VK_NULL_HANDLE;
    RenderTarget refmap_color_;
    uint32_t rt_cull_ = 0x10;
    // rays per lit pixel and shadowed light for soft shadows (rt_shadow.glsl)
    static constexpr uint32_t kRtSoftSamples = 4;
    uint64_t rt_built_frame_ = ~0ull;
    std::vector<RtCaster> rt_casters_;
    // ray traced contact shadows (12.21): light_contact.frag (the maps and the contact rays) unless light_rt.frag runs
    bool rt_contact_active_ = false;
    VkPipeline light_contact_ = VK_NULL_HANDLE;
    float rt_contact_reach_ = 0.35f;
    // PT_CONTACT_LEGACY=1: the contact shadows' first form (lighting.glsl), full strength whatever the light's shadow, for A/B
    bool rt_contact_legacy_ = false;
    // ray traced ambient occlusion (12.21): rt_ao.comp, rt_ao_filter.comp and probe_ao.frag; the storage images (raw, two
    // histories, blur, result, at the render size, GENERAL layout) are made when the option is first on
    bool rt_ao_active_ = false;
    // the main view's lighting pass reads the result this frame
    bool rt_ao_ready_ = false;
    VkPipeline rt_ao_trace_ = VK_NULL_HANDLE;
    VkPipeline rt_ao_filter_ = VK_NULL_HANDLE;
    VkPipeline probe_ao_ = VK_NULL_HANDLE;
    RenderTarget rt_ao_images_[RayTracing::kAoImages];
    // the history written last frame (0 or 1), and the view projection it was made with
    uint32_t rt_ao_written_ = 0;
    bool rt_ao_history_ = false;
    glm::mat4 rt_ao_previous_{1.0f};
    glm::vec3 rt_ao_previous_eye_{0.0f};
    uint32_t rt_ao_rays_ = 2;
    float rt_ao_reach_ = 0.75f;
    float rt_ao_frames_ = 16.0f;

    gpu::FrameData* frame_ = nullptr;
    std::vector<glm::mat4> skin_matrices_;
    std::vector<Draw> draws_;
    std::vector<const Draw*> decal_order_;
    std::vector<ShadowView> shadow_views_;
    std::vector<uint32_t> probe_order_;
    std::vector<const SceneLight*> light_sources_;
    std::vector<LightBox> light_boxes_;
    std::vector<uint8_t> light_shadow_views_;
    std::vector<uint32_t> mirror_shadow_lights_;
    uint32_t light_upload_count_ = 0;
    uint32_t view_count_ = 0;
    uint32_t light_count_ = 0;
    uint32_t probe_count_ = 0;
    ViewSetup main_view_;
    ViewSetup mirror_view_;
    bool mirror_active_ = false;
    bool mirror_temporal_enabled_ = false;
    bool mirror_history_valid_ = false;
    uint32_t mirror_history_size_ = 0;
    glm::mat4 mirror_current_view_projection_{1.0f};
    glm::mat4 mirror_previous_view_projection_{1.0f};
    glm::vec3 mirror_history_origin_{0.0f};
    float mirror_history_exposure_ = 1.0f;
    // the local reflections' history (RecordReflectionTemporal): which of reflect_history_ holds it, the frame that wrote it and
    // the exposure it was rendered with; PT_REFLECT_TEMPORAL_OFF=1 keeps the original blend with an upscaler too
    bool reflect_history_valid_ = false;
    uint32_t reflect_history_index_ = 0;
    uint32_t reflect_history_frame_ = 0;
    float reflect_history_exposure_ = 1.0f;
    std::vector<const SceneMirror*> mirrors_;
    // The last capture. 0x95B650 sets the mirror's texture (0x958390) and its ProjViewCol (0x958790 with the capture view's
    // +0x270 and +0x230) only in the frames a capture runs (0x959FD0), and the capture texture is the mirror's own, so with the
    // capture off the mirror goes on showing the reflection it was last given, frozen, not black.
    struct MirrorStale {
        bool valid = false;
        const GpuMesh* mesh = nullptr;
        glm::mat4 transform{1.0f};
        glm::mat4 projection{1.0f};
        glm::mat4 view{1.0f};
        glm::vec3 eye{0.0f};
        float near_distance = 0.0f;
        uint32_t size = 0;
    };
    MirrorStale mirror_stale_;
    bool mirror_stale_active_ = false;
    const SceneLighting* lighting_ = nullptr;
    SceneLighting fallback_;
    Camera camera_;

    float ev_ = 0.0f;
    bool adaptation_valid_ = false;
    bool ev_pinned_ = false;
    float exposure_ = 1.0f;
    Camera motion_reference_;
    float motion_interval_ = 1.0f / 60.0f;
    bool motion_reference_set_ = false;
    bool motion_reference_history_ = true;
    bool motion_history_valid_ = false;
    uint32_t motion_view_ = 0;
    std::unordered_map<uint64_t, MotionHistory> motion_history_;
    std::unordered_map<uint64_t, MotionHistory> motion_history_next_;
    Camera last_camera_;
    bool has_last_camera_ = false;
    glm::mat4 previous_view_projection_{1.0f};
    double frame_time_ = 1.0 / 60.0;
    uint32_t frame_counter_ = 0;
    RenderStats stats_;

    struct UpscaleState {
        UpscalerKind kind = UpscalerKind::Off;
        UpscaleBackend* backend = nullptr;
        UpscaleSettings created_settings;
        VkExtent2D render{};
        VkExtent2D created_render{};
        VkExtent2D created_output{};
        bool enabled = false;
        bool created = false;
        bool reset = true;
        bool cut = false;
        int32_t jitter_index = 0;
        int32_t phase_count = 1;
        glm::vec2 jitter_pixels{0.0f};
        glm::vec2 jitter_ndc{0.0f};
        float mip_bias = 0.0f;
        bool warp_test = false;
        bool warp_history = false;
        std::string failed_key;
    } up_;
    uint32_t post_view_ = 0;
    bool post_bindings_ = false;
    UpscaleStats upscale_stats_;
    double upscale_ms_sum_ = 0.0;
    double inputs_ms_sum_ = 0.0;
    double gpu_ms_sum_ = 0.0;
    double cpu_ms_sum_ = 0.0;
    uint32_t upscale_samples_ = 0;
    uint32_t gpu_samples_ = 0;
    uint32_t cpu_samples_ = 0;
    // PT_TIMING_LAST=<n>: the GPU time and the passes of the last n timed frames, logged at shutdown (timing runs)
    struct FrameTiming {
        float gpu_ms = 0.0f;
        float pass_ms[6] = {};
        float part_ms[6] = {};
        float device_mb = 0.0f;
        // the renderer's frame count when the frame was recorded (the parts trace's row in main.cpp)
        uint64_t frame = 0;
    };
    std::vector<FrameTiming> timing_ring_;
    uint64_t timing_count_ = 0;
    // PT_FRAME_CSV=<file>: per rendered frame, the renderer's CPU time and the wall time since the previous frame started
    // (a stutter is a long interval), written at shutdown
    struct FrameTrace {
        float cpu_ms = 0.0f;
        float interval_ms = 0.0f;
        uint32_t lights = 0;
        uint32_t shadow_views = 0;
        uint32_t draws = 0;
    };
    std::vector<FrameTrace> frame_trace_;
    // PT_LIGHT_CHANGES: the previous frame's lights, whether each had a shadow and its diffuse scale
    std::map<std::string, std::pair<bool, float>> light_change_state_;
    // the lights the main view shadowed last frame (BuildShadowViews' hysteresis, a port rule kept only for
    // PT_LIGHT_CULL_LEGACY=1)
    std::set<std::string> previous_shadowed_;
    // the occluder volumes of the main and mirror views this frame (the occluder stage of 0xD53AC0, light_cull.h); empty
    // with PT_LIGHT_CULL_LEGACY=1, which tests every OccluderEx as before
    lightcull::OccluderSet main_occluders_;
    lightcull::OccluderSet mirror_occluders_;
    // PT_LIGHT_CULL_LEGACY=1 (PrepareFrame)
    bool legacy_light_cull_ = false;
    // the camera view's unjittered view projection, the matrix of its culling query
    glm::mat4 main_cull_view_projection_{1.0f};
    // the view's culling query keeps the box (the grid test with PT_LIGHT_GRID, else the four side planes)
    bool BoxInCullView(const ViewSetup& view, const LightBox& box) const;
    // PT_LIGHT_CHANGES: why each light this frame dropped was dropped
    std::map<std::string, std::string> cull_reasons_;
    // and why a light that is in the frame has no shadow (BuildShadowViews)
    std::map<std::string, std::string> shadow_reasons_;
    std::chrono::steady_clock::time_point previous_frame_start_{};
    int vr_eye_ = -1;
};

}
