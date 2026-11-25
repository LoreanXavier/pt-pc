#pragma once

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <deque>
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
#include "engine/render/gpu/gpu_device.h"
#include "engine/render/gpu_types.h"
#include "engine/render/light_cull.h"
#include "engine/render/mesh.h"
#include "engine/render/motion_window.h"
#include "engine/render/raytracing.h"
#include "engine/render/render_util.h"
#include "engine/render/renderer.h"
#include "engine/render/scene_lighting.h"
#include "engine/render/subsurface_pass.h"
#include "engine/render/texture_manager.h"
#include "engine/render/upscale/upscale.h"

namespace pt {

namespace vfx {
struct RenderList;
}

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
    gpu::CommandList* cmd = nullptr;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 view_projection{1.0f};
    glm::vec3 eye{0.0f};
    gpu::Extent2D extent{};
    gpu::Format color_format = gpu::Format::Undefined;
    gpu::Format depth_format = gpu::Format::Undefined;
    gpu::ImageViewHandle depth_view;
    gpu::ImageViewHandle near_far_depth_view;
    float exposure = 1.0f;
    uint32_t frame_index = 0;
    bool mirrored = false;
    gpu::ImageViewHandle scene_copy_view;
    std::function<void()> copy_scene;
    glm::vec4 fog[8]{};
    uint32_t fog_mode = 0;
    uint32_t subset = 0;
    uint32_t* recorded = nullptr;
};

constexpr uint32_t kVfxAll = 0;
constexpr uint32_t kVfxScene = 1;
constexpr uint32_t kVfxOffscreen = 2;

struct SceneFilterContext {
    gpu::CommandList* cmd = nullptr;
    gpu::Extent2D extent{};
    gpu::Format color_format = gpu::Format::Undefined;
    gpu::ImageViewHandle source;
    gpu::ImageViewHandle depth_view;
    float exposure = 1.0f;
    uint32_t frame_index = 0;
    uint32_t layers = 3;
};

struct DrawItem {
    uint32_t light_fitting = 0;
    const GpuMesh* mesh = nullptr;
    glm::mat4 transform{1.0f};
    glm::vec4 tint{1.0f};
    int32_t material_override = -1;
    std::span<const glm::mat4> skin;
    MeshMask hidden_meshes;
    uint8_t hidden_views = 0;
    bool character_shadow = false;
    bool handy_shadowless = false;
    bool view_attached = false;
    bool turns_with_view = false;
    bool original_look = false;
    uint64_t source = 0;
};

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
    bool subsurface_scatter = true;
};

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
    float part_ms[6] = {};
    float light_ms[6] = {};
    float rt_ms[4] = {};
    float pt_ms[4] = {};
    float device_mb = 0.0f;
    glm::vec3 reflection_readback{0.0f};
    uint64_t reflection_readbacks = 0;
};

struct UpscaleStats {
    bool active = false;
    UpscalerKind kind = UpscalerKind::Off;
    gpu::Extent2D render{};
    gpu::Extent2D output{};
    float upscale_ms = 0.0f;
    float inputs_ms = 0.0f;
    bool ray_reconstruction = false;
    std::string error;
};

class SceneRenderer {
public:
    bool Init(Renderer& renderer, TextureManager& textures);
    void Shutdown();
    bool LoadResources(Vfs& vfs);

    std::unique_ptr<GpuMesh> Upload(const MeshData& data);
    void Destroy(GpuMesh& mesh);
    vk::Context& Context();
    gpu::Device& Gpu() { return *gpu_; }
    void WaitIdle() { gpu_->WaitIdle(); }

    void Render(const Camera& camera, const std::vector<DrawItem>& items);
    void ResetUpscaleHistory() { up_.reset = true; }
    void PrepareNgxReload();
    void SetUpscaleReactiveFrame(bool on) { reactive_frame_ = on; }
    void Render(const Camera& camera, const std::vector<DrawItem>& items, const SceneLighting& lighting, float dt);
    void DrawDebugUi();
    void ResetExposure() { adaptation_valid_ = false; }
    void SettleExposureClock() {
        if (exposure_camera_dt_ > 1.0 / 15.0) exposure_camera_dt_ = 1.0 / 30.0;
        exposure_previous_dt_ = 1.0f / 30.0f;
        exposure_clock_started_ = true;
    }
    void SetMotionReference(const Camera& previous, float interval, bool history);
    void AdvanceTime(float seconds) {
        pending_time_ += seconds;
        timed_ = true;
    }

    bool resumed_threshold_artifact = false;
    DebugView debug_view = DebugView::Lit;
    RenderToggles toggles;
    AppSettings::Graphics graphics;
    bool original_look = false;
    static constexpr gpu::Format kHdrTargetFormat = gpu::Format::R16G16B16A16Sfloat;
    static constexpr gpu::Format kDepthTargetFormat = gpu::Format::D32Sfloat;
    static constexpr gpu::Format kPostTargetFormat = gpu::Format::R16G16B16A16Sfloat;
    static constexpr gpu::Format kNearFarDepthFormat = gpu::Format::R32G32Sfloat;
    static constexpr uint32_t kShadowTile = 2048;
    static constexpr uint32_t kShadowAtlasSize = 8192;
    std::function<void(const SceneVfxContext&)> vfx_forward;
    std::function<void(const SceneVfxContext&)> vfx_liquid_depth;
    std::function<void(const SceneFilterContext&)> vfx_filter;
    std::function<const vfx::RenderList*()> vfx_list;
    const RenderStats& Stats() const { return stats_; }
    UpscaleSettings upscale;
    struct NeuralState {
        bool enabled = false;
        NeuralStyle style = NeuralStyle::Default;
        float intensity = 1.0f;
        bool small_model = false;
        float scale = 1.0f;
        unsigned every = 1;
        bool pre = false;
        float paper_white = 1.0f;
    } neural;
    const UpscaleStats& UpscaleStatistics() const { return upscale_stats_; }
    const NeuralRenderingSupport* NativeNeuralStatus() const { return UpscaleHost::Get().NativeNeuralStatus(); }
    bool UpscalerAvailable(UpscalerKind kind, std::string& reason);
    bool RayReconstructionAvailable(std::string& reason);
    void RequestTargetDump() { dump_requested_ = true; }
    void DumpDraws(const std::vector<DrawItem>& items, const std::string& path);
    bool DumpTargets(const std::string& prefix);
    RayTracingSettings raytracing;
    bool RayTracingReady() const { return rt_ != nullptr; }
    bool RayTracingSupported(std::string& reason) const;
    void SetVrEye(int eye) { vr_eye_ = eye; }
    struct SubliminalString {
        uint32_t texture = 0;
        glm::vec2 position{0.0f};
        float value = 0.0f;
        float ink = 0.1f;
    };
    void SetSubliminalString(const SubliminalString& s) { subliminal_string_state_ = s; }
    struct Backdrop {
        uint32_t texture = 0;
        float opacity = 0.0f;
        float darken = 0.0f;
        float tear = 0.0f;
        float time = 0.0f;
        glm::vec3 tint{1.0f};
        float aspect = 16.0f / 9.0f;
        float focus = 0.5f;
        float zoom = 1.0f;
        glm::vec2 center{0.0f};
    };
    void SetBackdrop(const Backdrop& b) { backdrop_state_ = b; }
    void SetDateResolve(bool on, bool still) {
        date_resolve_on_ = on;
        date_still_ = still;
    }

private:
    struct FrameSlot {
        gpu::BufferResource frame_data;
        gpu::BufferResource skin;
        gpu::BufferResource luminance;
        gpu::BindingSetHandle set;
        gpu::BindingSetHandle post_set;
        gpu::QueryPoolHandle queries;
        gpu::QueryPoolHandle upscale_queries;
        gpu::QueryPoolHandle neural_queries;
        bool neural_timed = false;
        uint64_t neural_frame = 0;
        gpu::BufferResource dispatch;
        gpu::BindingSetHandle dispatch_set;
        bool upscale_timed = false;
        gpu::QueryPoolHandle light_queries;
        std::vector<std::string> light_names;
        bool light_timed = false;
        uint32_t luminance_groups = 0;
        float exposure_used = 1.0f;
        double exposure_sample_time = 0.0;
        bool measured = false;
        bool timed = false;
        int vr_eye = -1;
        bool reflection_sampled = false;
    };

    struct Draw {
        uint32_t light_fitting = 0;
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
        bool handy_shadowless = false;
        bool view_shadowless = false;
    };

    struct MotionHistory {
        glm::mat4 transform{1.0f};
        std::vector<glm::mat4> skin;
    };

    struct ShadowView {
        uint32_t view = 0;
        uint32_t light = 0;
        uint8_t casters = 1;
        gpu::Rect2D rect{};
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        glm::vec4 planes[6]{};
        bool frustum = false;
        float bias_constant = 0.0f;
        float bias_slope = 0.0f;
        gpu::CullMode cull = gpu::CullMode::Front;
    };

    struct ViewSetup {
        uint32_t index = 0;
        glm::vec4 planes[6]{};
        bool mirrored = false;
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec3 eye{0.0f};
        uint8_t view_bit = 1;
        gpu::Extent2D area{0, 0};
    };

    gpu::Extent2D ViewArea(const ViewSetup& view) const { return view.area.width > 0 ? view.area : extent_; }
    bool CreatePipelines();
    void SetHdrPost(bool on);
    float Meter(const glm::vec2* texels, uint32_t count) const;
    gpu::Format BloomFormat() const;
    void DestroyPipelines();
    bool CreateDescriptors();
    bool EnsureTargets(gpu::Extent2D extent, gpu::Extent2D output, bool upscaling);
    void DestroyTargets();
    void WriteImageDescriptors();
    bool MakeBuffer(gpu::BufferResource& out, uint64_t size, gpu::BufferUsageFlags usage, bool host_visible) {
        return gpu_->CreateBuffer({size, usage, host_visible}, out);
    }
    bool CreateTarget(RenderTarget& target, gpu::Format format, gpu::Extent2D extent, gpu::ImageUsageFlags usage, gpu::AspectFlags aspect);
    bool UploadImage(gpu::ImageResource& image, gpu::Format format, gpu::Extent3D extent, const void* data, size_t size);
    bool UploadImageMips(gpu::ImageResource& image, gpu::Format format, gpu::Extent3D extent, const std::vector<std::vector<uint8_t>>& mips);
    void CreateBuiltinResources();
    void UpdateColorLut(const ScreenSettings& screen);
    bool BuildColorLut(const std::string& path, const ScreenSettings& screen, std::vector<uint8_t>& out, std::vector<float>* fine = nullptr);

    void PrepareFrame(const Camera& camera, const std::vector<DrawItem>& items, const SceneLighting& lighting);
    void BuildShadowViews(const SceneLighting& lighting, const glm::vec3& eye);
    void UploadFrame(FrameSlot& slot);
    void SetTppFog(const TppAtmosphereSettings& tpp);
    void ReadMeasurements(FrameSlot& slot, float dt, const ExposureSettings& settings);
    void SettleExposure(const ExposureSettings& settings);
    static float MeterLuminance(const glm::vec2* texels, uint32_t count);
    float ExposureCompensationFor(float ev, const ExposureSettings& settings) const;
    float ExposureFor(float ev, const ExposureSettings& settings) const;

    void BindSets(gpu::CommandList& cmd, gpu::BindPoint point);
    void RecordShadows(gpu::CommandList& cmd);
    void RecordRayTracing(gpu::CommandList& cmd);
    void CollectEffects();
    void RecordView(gpu::CommandList& cmd, const ViewSetup& view, RenderTarget& output, bool main_view);
    void RecordGBuffer(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordOcclusion(gpu::CommandList& cmd, const ViewSetup& view);
    bool EnsureAoTargets();
    bool EnsureShadowTarget();
    uint32_t shadow_tile_cap_ = 2048;
    std::chrono::steady_clock::time_point targets_retry_at_{};
    gpu::Extent2D rt_ao_failed_extent_{0, 0};
    std::chrono::steady_clock::time_point rt_ao_failed_at_{};
    void RecordRtAmbientOcclusion(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordLighting(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordLuminance(gpu::CommandList& cmd);
    void RecordForward(gpu::CommandList& cmd, const ViewSetup& view, RenderTarget& output);
    void RecordPost(gpu::CommandList& cmd, const SceneLighting& lighting, float dt);
    void RecordReflectionSample(gpu::CommandList& cmd, const SceneLighting& lighting);
    void RecordReflections(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordReflectMap(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordReflectMapMarch(gpu::CommandList& cmd, const ViewSetup& view, const glm::vec3& plane);
    bool reflect_map_early_ = false;
    bool reflect_map_made_ = false;
    void RecordReflectionTemporal(gpu::CommandList& cmd, const ViewSetup& view, gpu::PassPush push);
    void RecordMirrorTemporal(gpu::CommandList& cmd);
    bool RecordDepthOfField(gpu::CommandList& cmd, const ScreenSettings& screen, int& current);
    bool RecordMotionBlur(gpu::CommandList& cmd, const ScreenSettings& screen, int& current);
    bool MotionBlurActive() const;
    void RecordObjectVelocity(gpu::CommandList& cmd, const ViewSetup& view);
    void PostPass(gpu::CommandList& cmd, RenderTarget& target, gpu::PipelineHandle pipeline, const gpu::PassPush& push);
    SceneFilterContext FilterContext(gpu::CommandList& cmd, uint32_t layers) const;
    void UpdateMotion(const Camera& camera, float dt);
    void RecordDebug(gpu::CommandList& cmd);
    void DrawMesh(gpu::CommandList& cmd, const Draw& draw, uint32_t view, uint32_t flags, bool mirrored, bool no_cull = false,
                  const glm::vec4* aux = nullptr, gpu::CullMode cull = gpu::CullMode::Back, bool shadow_pass = false);
    uint32_t ForwardLights(const Draw& draw, glm::vec4 aux[2], uint8_t view_bit) const;
    void UpdateDominantLight(const Camera& camera, const SceneLighting& lighting, float dt);
    uint32_t MirrorLights(const Draw& draw, glm::vec4 aux[2]) const;
    void UpdateReflectionTexture(const std::string& path);
    void ApplyEnvironmentOverrides();
    void Stamp(gpu::CommandList& cmd, uint32_t index);
    void ReadTimestamps(FrameSlot& slot);
    void PushConstants(gpu::CommandList& cmd, const void* data, uint32_t size);
    void CopyToHistory(gpu::CommandList& cmd, const RenderTarget& source);
    void Fullscreen(gpu::CommandList& cmd, gpu::PipelineHandle pipeline, const gpu::PassPush& push);
    bool Visible(const Draw& draw, const glm::vec4* planes) const;
    bool Visible(const Draw& draw, const ViewSetup& view) const;

    bool CreateUpscalePipelines();
    void DestroyUpscalePipelines();
    bool CreateUpscaleTargets(gpu::Extent2D render, gpu::Extent2D output);
    void DestroyUpscaleTargets();
    void WriteUpscaleDescriptors(gpu::ImageBinding* images, bool post);
    bool BeginUpscaleFrame(gpu::Extent2D output);
    bool SetupUpscaler(gpu::Extent2D output);
    void RecordUpscalePrepare(float dt);
    bool RecordUpscaleDispatch();
    void PrepareFrameGeneration(const UpscaleDispatch& d, bool upscaled);
    bool RecordFrameGenLensInputs(gpu::CommandList& cmd);
#ifdef _WIN32
    void PrepareFrameGenerationD3D12(bool upscaled);
#endif
    void RecordUpscaleResolve(gpu::CommandList& cmd, bool upscaled);
    void RecordOpaqueSnapshot(gpu::CommandList& cmd, RenderTarget& output);
    void RecordSceneCopy(gpu::CommandList& cmd, RenderTarget& output);
    void RecordDumpCopy(gpu::CommandList& cmd, RenderTarget& target, gpu::BufferResource& buffer);
    void RecordEffects(gpu::CommandList& cmd, RenderTarget& output, SceneVfxContext context);
    void RecordUpscaleMotion(gpu::CommandList& cmd, const ViewSetup& view, bool fsr_rr = false);
    void RecordUpscaleInputs(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordNeuralRendering(gpu::CommandList& cmd);
    void RecordUpscale(gpu::CommandList& cmd, float dt);
    UpscaleDispatch up_dispatch_;
    void ReadUpscaleTimestamps(FrameSlot& slot);

    static constexpr uint32_t kTimestamps = 29;
    static constexpr uint32_t kReflectMapSize = 512;
    static constexpr float kCameraCutDistance = 3.0f;

    Renderer* renderer_ = nullptr;
    gpu::Format ldr_format_ = gpu::Format::R8G8B8A8Unorm;
    bool hdr_post_ = false;
    glm::vec4 bloom_shoulder_{0.0f};
    TextureManager* textures_ = nullptr;
    gpu::QueryPoolHandle queries_;
    glm::vec3 last_eye_{0.0f};
    DominantLightState dominant_;
    float pending_time_ = 0.0f;
    bool timed_ = false;
    bool has_last_eye_ = false;
    uint32_t last_timed_slot_ = Renderer::kFramesInFlight;
    gpu::Device* gpu_ = nullptr;
    FrameSlot slots_[Renderer::kFramesInFlight];
    gpu::BindingLayoutHandle frame_layout_;
    gpu::BindingPoolHandle pool_;
    gpu::PipelineLayoutHandle layout_;
    gpu::BindingLayoutHandle dispatch_layout_;
    gpu::BindingPoolHandle dispatch_pool_;
    gpu::SamplerHandle samplers_[gpu::kSmpCount] = {};

    gpu::Extent2D extent_{0, 0};
    RenderTarget albedo_;
    RenderTarget normal_;
    RenderTarget material_;
    RenderTarget depth_;
    RenderTarget flare_depth_;
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
    RenderTarget mb_original_bake_;
    RenderTarget mb_original_blur_[2];
    RenderTarget mirror_;
    RenderTarget mirror_capture_cache_[2];
    RenderTarget mirror_history_;
    RenderTarget mirror_temporal_;
    RenderTarget reflect_layer_;
    RenderTarget reflect_offset_;
    RenderTarget reflect_history_[2];
    RenderTarget shadow_;
    RenderTarget ao_[2];
    RenderTarget refmap_;
    RenderTarget hdr_copy_;
    RenderTarget particles_;
    RenderTarget particles_near_;
    RenderTarget particles_far_;
    RenderTarget near_far_depth_;
    RenderTarget near_far_depth_attachment_;
    RenderTarget motion_;
    RenderTarget reactive_;
    glm::vec4 previous_handy_[2] = {glm::vec4(0.0f, 0.0f, 0.0f, -2.0f), glm::vec4(0.0f)};
    RenderTarget opaque_;
    RenderTarget nr_in_;
    RenderTarget nr_out_;
    RenderTarget handy_factor_;
    bool handy_demod_ = false;
    RenderTarget exposure_image_;
    RenderTarget upscaled_;
    RenderTarget post_hdr_;
    RenderTarget post_depth_;
    RenderTarget post_object_velocity_;
    RenderTarget fg_motion_;
    RenderTarget fg_depth_;
    bool dump_requested_ = false;
    bool dump_recorded_ = false;
    gpu::BufferResource dump_compose_;
    gpu::BufferResource dump_forward_;
    gpu::BufferResource dump_scene_;
    gpu::BufferResource dump_mirror_;
    gpu::Extent2D output_extent_{0, 0};
    bool targets_upscaled_ = false;
    uint64_t neural_output_epoch_ = 1;
    bool history_valid_ = false;

    gpu::ImageResource lut1_;
    gpu::ImageResource lut2_;
    gpu::ImageResource material_tex_;
    gpu::ImageResource dither_;
    gpu::ImageResource color_lut_;
    gpu::ImageResource color_lut_prev_;
    gpu::ImageResource noise_;
    gpu::ImageResource mask_;
    gpu::ImageResource white_;
    std::string color_lut_key_;
    bool color_lut_fine_ = false;
    std::string color_lut_prev_key_;
    std::string reflection_key_;
    uint32_t reflection_cube_ = 0;
    Vfs* vfs_ = nullptr;

    gpu::PipelineHandle gbuffer_;
    gpu::PipelineHandle decal_;
    gpu::PipelineHandle shadow_pipeline_;
    gpu::PipelineHandle probe_;
    gpu::PipelineHandle probe_resolve_;
    gpu::PipelineHandle light_;
    gpu::PipelineHandle compose_;
    gpu::PipelineHandle forward_;
    gpu::PipelineHandle luminance_;
    gpu::PipelineHandle reflect_colour_;
    gpu::PipelineHandle occlusion_;
    gpu::PipelineHandle occlusion_blur_;
    gpu::PipelineHandle bright_;
    gpu::PipelineHandle kawase_;
    gpu::PipelineHandle forward_emissive_;
    gpu::PipelineHandle forward_glow_;
    gpu::PipelineHandle bloom_add_;
    gpu::PipelineHandle kawase_sum_;
    gpu::PipelineHandle gaussian_;
    gpu::PipelineHandle tonemap_;
    gpu::PipelineHandle fxaa_;
    gpu::PipelineHandle mirror_temporal_pipeline_;
    gpu::PipelineHandle dof_ratio_;
    gpu::PipelineHandle dof_down_;
    gpu::PipelineHandle dof_blur_;
    gpu::PipelineHandle dof_blend_;
    gpu::PipelineHandle dof_layers_;
    gpu::PipelineHandle dof_gather_;
    gpu::PipelineHandle dof_merge_;
    gpu::PipelineHandle mb_velocity_;
    gpu::PipelineHandle mb_tile_pipeline_;
    gpu::PipelineHandle mb_bake_pipeline_;
    gpu::PipelineHandle mb_mcguire_;
    gpu::PipelineHandle mb_composite_;
    gpu::PipelineHandle mb_decode_;
    gpu::PipelineHandle velocity_pipeline_;
    gpu::PipelineHandle fsblur_;
    gpu::PipelineHandle banding_;
    gpu::PipelineHandle screen_fx_;
    gpu::PipelineHandle subliminal_string_;
    gpu::PipelineHandle backdrop_;
    gpu::PipelineHandle debug_;
    gpu::PipelineHandle reflect_make_;
    gpu::PipelineHandle reflect_blend_;
    gpu::PipelineHandle reflect_layer_pipeline_;
    gpu::PipelineHandle reflect_temporal_pipeline_;
    bool reflect_post_upscale_ = false;
    uint32_t shown_shadow_limit_ = 3;
    gpu::PipelineHandle vfx_composite_;
    gpu::PipelineHandle vfx_depth_down_;
    gpu::PipelineHandle up_motion_;
    gpu::PipelineHandle up_object_motion_;
    gpu::PipelineHandle up_reactive_;
    gpu::PipelineHandle up_resolve_;
    gpu::PipelineHandle up_demod_;
    gpu::PipelineHandle up_fg_lens_;
    gpu::PipelineHandle up_nr_encode_;
    gpu::PipelineHandle up_nr_decode_;
    bool nr_feature_ready_ = false;
    gpu::Extent2D nr_extent_{};
    bool nr_small_model_ = false;

    std::unique_ptr<RayTracing> rt_;
    SubsurfacePass sss_;
    bool sss_ready_ = false;
    gpu::PipelineHandle light_rt_;
    bool rt_active_ = false;
    bool rt_reflections_ = false;
    gpu::PipelineHandle reflect_make_rt_;
    gpu::PipelineHandle reflect_blend_rt_;
    gpu::PipelineHandle reflect_layer_rt_;
    RenderTarget refmap_color_;
    uint32_t rt_cull_ = 0x10;
    static constexpr uint32_t kRtSoftSamples = 4;
    uint64_t rt_built_frame_[Renderer::kFramesInFlight] = {~0ull, ~0ull};
    std::vector<RtCaster> rt_casters_;
    std::vector<Vertex> rt_effect_vertices_;
    std::vector<uint32_t> rt_effect_indices_;
    bool rt_contact_active_ = false;
    gpu::PipelineHandle light_contact_;
    float rt_contact_reach_ = 0.35f;
    bool rt_contact_legacy_ = false;
    bool light_depth_bounds_ = false;
    static glm::vec2 LightDepthBounds(const gpu::Light& light, const gpu::View& view, const glm::mat4* volume);
    static glm::mat4 SpotVolume(const SceneLight& l, float radius, float& size);
    bool rt_ao_active_ = false;
    bool rt_ao_ready_ = false;
    gpu::PipelineHandle rt_ao_trace_;
    gpu::PipelineHandle rt_ao_filter_;
    gpu::PipelineHandle probe_ao_;
    RenderTarget rt_ao_images_[RayTracing::kAoImages];
    uint32_t rt_ao_written_ = 0;
    bool rt_ao_history_ = false;
    glm::mat4 rt_ao_previous_{1.0f};
    glm::vec3 rt_ao_previous_eye_{0.0f};
    uint32_t rt_ao_rays_ = 2;
    float rt_ao_reach_ = 0.75f;
    float rt_ao_frames_ = 16.0f;
    bool rt_full_shadows_ = false;
    bool rt_gi_active_ = false;
    bool rt_glossy_active_ = false;
    bool RtFullAny() const { return rt_full_shadows_ || rt_gi_active_ || rt_glossy_active_ || rt_path_; }
    gpu::PipelineHandle light_full_;
    gpu::PipelineHandle rt_classify_;
    gpu::PipelineHandle rt_gi_trace_;
    gpu::PipelineHandle rt_glossy_trace_;
    gpu::PipelineHandle rt_full_filter_;
    gpu::PipelineHandle rt_full_resolve_;
    gpu::PipelineHandle rt_full_resolve_glossy_;
    gpu::PipelineHandle rt_full_resolve_gi_;
    bool rt_full_traced_ = false;
    RenderTarget rt_full_images_[RayTracing::kFullImages];
    gpu::Extent2D rt_full_failed_extent_{0, 0};
    std::chrono::steady_clock::time_point rt_full_failed_at_{};
    uint32_t rt_full_written_ = 0;
    bool rt_full_history_ = false;
    glm::mat4 rt_full_previous_{1.0f};
    glm::vec2 rt_full_previous_jitter_{0.0f};
    glm::vec3 rt_full_previous_eye_{0.0f};
    float rt_gi_strength_ = 1.0f;
    float rt_gi_static_ = 0.6f;
    float rt_glossy_strength_ = 1.0f;
    float rt_handy_fill_ = 0.35f;
    bool EnsureFullTargets();
    void DestroyFullTargets();
    void CreateFullPipelines(gpu::Format light_format, bool depth_bounds);
    void DestroyFullPipelines();
    void SetupFullLights();
    void RecordRtFullTrace(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordRtFullResolve(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordRtFullBounce(gpu::CommandList& cmd, const ViewSetup& view);
    bool rt_path_ = false;
    gpu::PipelineHandle pt_direct_;
    gpu::PipelineHandle pt_indirect_;
    gpu::PipelineHandle pt_filter_;
    gpu::PipelineHandle pt_layer_;
    gpu::PipelineHandle pt_resolve_multiply_;
    gpu::PipelineHandle pt_layer_out_;
    void RecordPathLayerOut(gpu::CommandList& cmd, const ViewSetup& view);
    gpu::PipelineHandle pt_gbuffer_;
    gpu::PipelineHandle pt_decal_;
    gpu::PipelineHandle pt_reference_;
    gpu::PipelineHandle pt_reference_out_;
    glm::mat4 pt_reference_view_{1.0f};
    bool pt_reference_valid_ = false;
    uint32_t pt_reference_frames_ = 0;
    bool PathReference() const;
    void RecordPathReferenceOut(gpu::CommandList& cmd, const ViewSetup& view);
    gpu::PipelineHandle pt_resolve_;
    RenderTarget pt_images32_[RayTracing::kPathImages32];
    RenderTarget pt_images16_[RayTracing::kPathImages16];
    gpu::Extent2D pt_failed_extent_{0, 0};
    std::chrono::steady_clock::time_point pt_failed_at_{};
    std::vector<uint64_t> pt_previous_ids_;
    bool pt_history_ = false;
    bool pt_indirect_on_ = true;
    bool rr_frame_ = false;
    bool fsr_rr_input_check_ = false;
    bool rr_raster_ = false;
    gpu::PipelineHandle rt_rr_guides_;
    void RecordRasterRrGuides(gpu::CommandList& cmd, const ViewSetup& view);
    bool rt_full_render_size_ = false;
    gpu::PipelineHandle pt_rr_guides_;
    gpu::PipelineHandle pt_indirect_rr_;
    gpu::PipelineHandle pt_resolve_rr_;
    RenderTarget rr_diffuse_albedo_;
    RenderTarget rr_specular_albedo_;
    RenderTarget rr_normal_roughness_;
    RenderTarget rr_hit_distance_;
    RenderTarget fsr_rr_input_[4];
    RenderTarget fsr_rr_depth_, fsr_rr_motion_, fsr_rr_normal_;
    gpu::PipelineHandle fsr_rr_prepare_, fsr_rr_resolve_, fsr_rr_motion_trace_, fsr_rr_velocity_;
    bool EnsureFsrRrTargets();
    void RecordFsrRayRegeneration(gpu::CommandList& cmd, const ViewSetup& view);
    struct FsrRrSample {
        const char* name = nullptr;
        gpu::BufferResource buffer;
        gpu::Format format{};
    };
    std::vector<FsrRrSample> fsr_rr_samples_;
    uint32_t fsr_rr_denoised_ = 0;
    uint32_t fsr_rr_sample_age_ = 0;
    gpu::Extent2D fsr_rr_sample_size_{};
    void RecordFsrRrSamples(gpu::CommandList& cmd, RenderTarget* const* output);
    void LogFsrRrSamples();
    void DestroyFsrRrSamples();
    bool EnsureRrTargets();
    void DestroyRrTargets();
    void RecordRrGuides(gpu::CommandList& cmd, const ViewSetup& view);
    void CreatePathPipelines(gpu::Format light_format);
    void DestroyPathPipelines();
    bool EnsurePathTargets();
    void DestroyPathTargets();
    void SetupPathLights();
    void RecordPathTrace(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordPathResolve(gpu::CommandList& cmd, const ViewSetup& view, uint32_t written);
    void RecordPathIndirect(gpu::CommandList& cmd, const ViewSetup& view);
    void RecordPathLayers(gpu::CommandList& cmd, const ViewSetup& view);

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
    bool reflect_history_valid_ = false;
    uint32_t reflect_history_index_ = 0;
    uint32_t reflect_history_frame_ = 0;
    float reflect_history_exposure_ = 1.0f;
    std::vector<const SceneMirror*> mirrors_;
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
    MirrorStale mirror_stale_cache_[2];
    uint32_t mirror_cache_index_ = 0;
    uint32_t mirror_render_cache_index_ = 0;
    bool mirror_stale_active_ = false;
    const SceneLighting* lighting_ = nullptr;
    SceneLighting fallback_;
    Camera camera_;

    float ev_ = 0.0f;
    double exposure_camera_dt_ = static_cast<double>(1.0f / 60.0f);
    float exposure_previous_dt_ = 0.0f;
    bool exposure_clock_started_ = false;
    double exposure_time_ = 0.0;
    double exposure_updated_at_ = 0.0;
    std::array<double, 5> exposure_tick_times_{};
    uint32_t exposure_tick_index_ = 0;
    double exposure_feedback_time_ = 0.0;
    struct ExposureSample {
        double time;
        float luminance;
    };
    std::deque<ExposureSample> exposure_samples_;
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
    glm::mat4 blur_previous_view_projection_{1.0f};
    glm::vec4 blur_rates_{0.0f};
    glm::vec3 blur_velocity_{0.0f};
    bool blur_rates_valid_ = false;
    double frame_time_ = 1.0 / 60.0;
    float motion_step_ = 1.0f / 60.0f;
    render::MotionWindow motion_window_;
    float blur_span_ = static_cast<float>(render::kOriginalFrameSeconds);
    bool motion_cut_ = false;
    uint32_t frame_counter_ = 0;
    void LayerDiag();
    int layer_diag_variant_ = -1;
    int layer_diag_last_ = -1;
    int layer_diag_skip_ = 0;
    uint32_t layer_diag_frames_ = 0;
    std::array<double, 4> layer_diag_sum_{};
    RenderStats stats_;

    struct UpscaleState {
        UpscalerKind kind = UpscalerKind::Off;
        UpscaleBackend* backend = nullptr;
        UpscaleSettings created_settings;
        gpu::Extent2D render{};
        gpu::Extent2D created_render{};
        gpu::Extent2D created_output{};
        bool enabled = false;
        bool native_neural_only = false;
        bool created = false;
        bool ray_reconstruction = false;
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
        std::chrono::steady_clock::time_point failed_at{};
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
    struct FrameTiming {
        float gpu_ms = 0.0f;
        float pass_ms[6] = {};
        float part_ms[6] = {};
        float light_ms[6] = {};
        float rt_ms[4] = {};
        float pt_ms[4] = {};
        float device_mb = 0.0f;
        uint64_t frame = 0;
    };
    std::vector<FrameTiming> timing_ring_;
    uint64_t timing_count_ = 0;
    struct FrameTrace {
        float cpu_ms = 0.0f;
        float interval_ms = 0.0f;
        uint32_t lights = 0;
        uint32_t shadow_views = 0;
        uint32_t draws = 0;
        uint32_t pipelines = 0;
        float pipeline_ms = 0.0f;
    };
    std::vector<FrameTrace> frame_trace_;
    uint64_t frame_trace_pipelines_ = 0;
    uint64_t frame_trace_pipeline_ns_ = 0;
    std::map<std::string, std::pair<bool, float>> light_change_state_;
    std::set<std::string> previous_shadowed_;
    lightcull::OccluderSet main_occluders_;
    bool mesh_occlusion_ = false;
    lightcull::OccluderSet mirror_occluders_;
    bool legacy_light_cull_ = false;
    glm::mat4 main_cull_view_projection_{1.0f};
    bool BoxInCullView(const ViewSetup& view, const LightBox& box) const;
    std::map<std::string, std::string> cull_reasons_;
    std::map<std::string, std::string> shadow_reasons_;
    std::chrono::steady_clock::time_point previous_frame_start_{};
    int vr_eye_ = -1;
    uint64_t vr_trace_submissions_ = 0;
    uint32_t vr_trace_lines_ = 0;
    SubliminalString subliminal_string_state_;
    Backdrop backdrop_state_;
    bool reactive_frame_ = false;
    bool date_resolve_on_ = false;
    bool date_still_ = false;
    bool date_active_ = false;
    bool date_still_now_ = false;
    bool date_history_valid_ = false;
    uint32_t date_history_ = 0;
    uint32_t date_live_frames_ = 0;
    uint32_t date_still_frames_ = 0;
    glm::vec2 date_jitter_ndc_{0.0f};
    glm::mat4 date_previous_view_projection_{1.0f};
    gpu::PipelineHandle date_resolve_;
    gpu::PipelineHandle date_resolve_out_;
};

}
