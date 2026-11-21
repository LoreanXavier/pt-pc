#include "engine/render/scene_renderer.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

#include "engine/core/log.h"
#include "engine/render/render_util.h"
#include "engine/render/upscale/streamline.h"

namespace pt {
namespace {

constexpr float kSpatialRadius = 24.0f;
constexpr float kDirectSamples = 4.0f;
constexpr float kPathReach = 30.0f;
constexpr float kPathBounces = 4.0f;
constexpr float kPathAlbedoMip = 2.0f;
constexpr float kPathClamp = 8.0f;
constexpr float kDiffuseFrames = 32.0f;
constexpr float kSpecularFrames = 16.0f;
constexpr float kLuminanceSigma = 4.0f;
constexpr gpu::Format kRrGuideFormat = gpu::Format::R16G16B16A16Sfloat;
constexpr gpu::Format kRrHitFormat = gpu::Format::R16Sfloat;

float EnvFloat(const char* name, float fallback, float lo, float hi) {
    const char* value = std::getenv(name);
    return value ? std::clamp(static_cast<float>(std::atof(value)), lo, hi) : fallback;
}

}

void SceneRenderer::CreatePathPipelines(gpu::Format light_format) {
    pt_direct_ = gpu_->CreateComputePipeline({rt_->Layout(), "pt_direct.comp"});
    pt_indirect_ = gpu_->CreateComputePipeline({rt_->Layout(), "pt_indirect.comp"});
    pt_filter_ = gpu_->CreateComputePipeline({rt_->Layout(), "pt_filter.comp"});
    pt_layer_ = gpu_->CreateComputePipeline({rt_->Layout(), "pt_layer.comp"});
    if (const char* reference = std::getenv("PT_PATHTRACE_REFERENCE"); reference && std::atoi(reference) != 0) {
        pt_reference_ = gpu_->CreateComputePipeline({rt_->Layout(), "pt_reference.comp"});
        gpu::GraphicsPipelineDesc out;
        out.layout = rt_->Layout();
        out.fragment = "pt_reference_out.frag";
        out.colors = {kHdrTargetFormat};
        out.depth = kDepthTargetFormat;
        pt_reference_out_ = gpu_->CreateGraphicsPipeline(out);
        if (!pt_reference_ || !pt_reference_out_) {
            LogError("ray tracing: the path traced reference mode's passes could not be created, it stays off");
        } else {
            LogInfo("ray tracing: path traced reference mode (camera rays, accumulated while the camera holds still, no denoiser)");
        }
    }
    gpu::GraphicsPipelineDesc resolve;
    resolve.layout = rt_->Layout();
    resolve.fragment = "pt_resolve.frag";
    resolve.colors = {light_format, light_format};
    resolve.blends = {gpu::BlendMode::Additive, gpu::BlendMode::Additive};
    pt_resolve_ = gpu_->CreateGraphicsPipeline(resolve);
    resolve.blends = {gpu::BlendMode::Multiply, gpu::BlendMode::Multiply};
    pt_resolve_multiply_ = gpu_->CreateGraphicsPipeline(resolve);
    gpu::GraphicsPipelineDesc layer_out;
    layer_out.layout = rt_->Layout();
    layer_out.fragment = "pt_layer_out.frag";
    layer_out.colors = {kHdrTargetFormat};
    layer_out.depth = kDepthTargetFormat;
    layer_out.blend = gpu::BlendMode::PremultipliedFadeAlpha;
    pt_layer_out_ = gpu_->CreateGraphicsPipeline(layer_out);
    gpu::GraphicsPipelineDesc guides;
    guides.layout = rt_->Layout();
    guides.fragment = "pt_rr_guides.frag";
    guides.colors = {kRrGuideFormat, kRrGuideFormat, kRrGuideFormat, kRrHitFormat};
    pt_rr_guides_ = gpu_->CreateGraphicsPipeline(guides);
    guides.fragment = "rt_rr_guides.frag";
    rt_rr_guides_ = gpu_->CreateGraphicsPipeline(guides);
    pt_indirect_rr_ = gpu_->CreateComputePipeline({rt_->Layout(), "pt_indirect_rr.comp"});
    resolve.fragment = "pt_resolve_rr.frag";
    resolve.blends = {gpu::BlendMode::Additive, gpu::BlendMode::Additive};
    pt_resolve_rr_ = gpu_->CreateGraphicsPipeline(resolve);
    if (gpu_->Info().backend == gpu::Backend::D3D12) {
        resolve.fragment = "pt_fsr_resolve.frag";
        resolve.blends = {};
        fsr_rr_resolve_ = gpu_->CreateGraphicsPipeline(resolve);
        guides.fragment = "pt_fsr_prepare.frag";
        guides.colors = {kRrGuideFormat, kRrGuideFormat, kRrGuideFormat, kRrGuideFormat,
                         gpu::Format::R32Sfloat, kRrGuideFormat};
        fsr_rr_prepare_ = gpu_->CreateGraphicsPipeline(guides);
        gpu::GraphicsPipelineDesc fsr_motion;
        fsr_motion.layout = layout_;
        fsr_motion.fragment = "pt_fsr_motion.frag";
        fsr_motion.colors = {kRrGuideFormat};
        fsr_motion.depth = kDepthTargetFormat;
        fsr_rr_motion_trace_ = gpu_->CreateGraphicsPipeline(fsr_motion);
        fsr_motion.vertex = "upscale_velocity.vert";
        fsr_motion.fragment = "pt_fsr_velocity.frag";
        fsr_motion.mesh_input = true;
        fsr_motion.depth_test = true;
        fsr_rr_velocity_ = gpu_->CreateGraphicsPipeline(fsr_motion);
    }
    if (!pt_rr_guides_ || !pt_indirect_rr_ || !pt_resolve_rr_) {
        for (gpu::PipelineHandle* p : {&pt_rr_guides_, &pt_indirect_rr_, &pt_resolve_rr_}) {
            if (*p) {
                gpu_->DestroyPipeline(*p);
                *p = {};
            }
        }
        LogWarn("ray tracing: the DLSS Ray Reconstruction passes could not be created; Path traced keeps its own denoiser");
    }
    if (!pt_direct_ || !pt_indirect_ || !pt_filter_ || !pt_layer_ || !pt_resolve_ || !pt_resolve_multiply_ || !pt_layer_out_) {
        LogError("ray tracing: the path traced lighting's passes could not be created, it stays off");
        DestroyPathPipelines();
    }
}

void SceneRenderer::DestroyPathPipelines() {
    for (gpu::PipelineHandle* p : {&pt_direct_, &pt_indirect_, &pt_filter_, &pt_layer_, &pt_resolve_, &pt_resolve_multiply_, &pt_layer_out_, &pt_reference_, &pt_reference_out_, &pt_gbuffer_, &pt_decal_,
                          &pt_rr_guides_, &pt_indirect_rr_, &pt_resolve_rr_, &rt_rr_guides_, &fsr_rr_prepare_, &fsr_rr_resolve_, &fsr_rr_motion_trace_, &fsr_rr_velocity_}) {
        if (*p) {
            gpu_->DestroyPipeline(*p);
            *p = {};
        }
    }
}

void SceneRenderer::DestroyPathTargets() {
    for (RenderTarget& t : pt_images32_) {
        gpu_->DestroyImage(t.image);
        t = RenderTarget{};
    }
    for (RenderTarget& t : pt_images16_) {
        gpu_->DestroyImage(t.image);
        t = RenderTarget{};
    }
    pt_history_ = false;
    DestroyRrTargets();
}

void SceneRenderer::DestroyRrTargets() {
    DestroyFsrRrSamples();
    for (RenderTarget* t : {&rr_diffuse_albedo_, &rr_specular_albedo_, &rr_normal_roughness_, &rr_hit_distance_,
                          &fsr_rr_input_[0], &fsr_rr_input_[1], &fsr_rr_input_[2], &fsr_rr_input_[3],
                          &fsr_rr_depth_, &fsr_rr_motion_, &fsr_rr_normal_}) {
        gpu_->DestroyImage(t->image);
        *t = RenderTarget{};
    }
}

bool SceneRenderer::EnsureRrTargets() {
    if (rr_diffuse_albedo_.Valid() && rr_diffuse_albedo_.Extent().width == extent_.width && rr_diffuse_albedo_.Extent().height == extent_.height) {
        return true;
    }
    const vk::AllocTag tag("ray tracing");
    gpu_->WaitIdle();
    DestroyRrTargets();
    const gpu::ImageUsageFlags usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc;
    const gpu::AspectFlags c = gpu::Aspect::Color;
    if (!CreateTarget(rr_diffuse_albedo_, kRrGuideFormat, extent_, usage, c) || !CreateTarget(rr_specular_albedo_, kRrGuideFormat, extent_, usage, c) ||
        !CreateTarget(rr_normal_roughness_, kRrGuideFormat, extent_, usage, c) || !CreateTarget(rr_hit_distance_, kRrHitFormat, extent_, usage, c)) {
        LogError("ray tracing: DLSS Ray Reconstruction's guide targets could not be made at {}x{}", extent_.width, extent_.height);
        DestroyRrTargets();
        return false;
    }
    LogInfo("ray tracing: DLSS Ray Reconstruction guides {}x{}", extent_.width, extent_.height);
    return true;
}

bool SceneRenderer::RayReconstructionAvailable(std::string& reason) {
    if (!pt_rr_guides_) {
        reason = rt_ ? "the guide pass is missing" : "needs ray tracing";
        return false;
    }
    if (upscale.kind != UpscalerKind::Dlss && upscale.kind != UpscalerKind::Fsr4) {
        reason = "needs DLSS Ray Reconstruction or FSR 4 Ray Regeneration";
        return false;
    }
    if (upscale.kind == UpscalerKind::Fsr4 && !raytracing.path_traced) {
        reason = "FSR Ray Regeneration needs Path traced lighting";
        return false;
    }
    if (!UpscaleHost::Get().Available(upscale.kind, reason, false)) {
        return false;
    }
    UpscaleBackend* backend = UpscaleHost::Get().Backend(upscale.kind);
    return backend && backend->RayReconstructionAvailable(reason);
}

bool SceneRenderer::EnsurePathTargets() {
    const vk::AllocTag tag("ray tracing");
    if (pt_images32_[0].Valid() && pt_images32_[0].Extent().width == extent_.width && pt_images32_[0].Extent().height == extent_.height) {
        return true;
    }
    if (pt_failed_extent_.width == extent_.width && pt_failed_extent_.height == extent_.height &&
        std::chrono::steady_clock::now() - pt_failed_at_ < std::chrono::seconds(5)) {
        return false;
    }
    gpu_->WaitIdle();
    DestroyPathTargets();
    const gpu::Extent2D half{(extent_.width + 1) / 2, (extent_.height + 1) / 2};
    gpu::ImageViewHandle views32[RayTracing::kPathImages32]{};
    gpu::ImageViewHandle views16[RayTracing::kPathImages16]{};
    const gpu::ImageUsageFlags usage = gpu::ImageUsage::Storage | gpu::ImageUsage::TransferDst | gpu::ImageUsage::TransferSrc;
    bool ok = true;
    for (uint32_t i = 0; i < RayTracing::kPathImages32 && ok; ++i) {
        ok = CreateTarget(pt_images32_[i], gpu::Format::R32G32B32A32Sfloat, extent_, usage, gpu::Aspect::Color);
        views32[i] = pt_images32_[i].image.view;
    }
    for (uint32_t i = 0; i < RayTracing::kPathImages16 && ok; ++i) {
        ok = CreateTarget(pt_images16_[i], gpu::Format::R16G16B16A16Sfloat, i == 0 || i >= 7 ? extent_ : half, usage, gpu::Aspect::Color);
        views16[i] = pt_images16_[i].image.view;
    }
    if (!ok) {
        LogError("ray tracing: the path traced lighting's images could not be made at {}x{}", extent_.width, extent_.height);
        pt_failed_extent_ = extent_;
        pt_failed_at_ = std::chrono::steady_clock::now();
        DestroyPathTargets();
        return false;
    }
    gpu_->Submit([&](gpu::CommandList& cmd) {
        auto clear = [&](RenderTarget& t, float x) {
            cmd.UseTargets({{&t, gpu::Layout::General}});
            gpu::ClearColor value{};
            value.float32[0] = x;
            cmd.ClearColorImage(t.image, gpu::Layout::General, value, 1, 1);
        };
        for (RenderTarget& t : pt_images32_) {
            clear(t, -1.0f);
        }
        for (RenderTarget& t : pt_images16_) {
            clear(t, 0.0f);
        }
    });
    rt_->SetPathImages(views32, views16);
    pt_history_ = false;
    LogInfo("ray tracing: path traced lighting images {}x{} (indirect light {}x{})", extent_.width, extent_.height, half.width, half.height);
    return true;
}

void SceneRenderer::SetupPathLights() {
    for (uint32_t i = 0; i < light_count_ && i < light_sources_.size(); ++i) {
        gpu::Light& g = frame_->lights[i];
        const SceneLight& l = *light_sources_[i];
        const bool hidden = (l.hidden_views & 1u) != 0;
        const bool handy_fill = l.id >= kHandyReflectionId && l.id < kHandyReflectionId + 4;
        g.shadow_cone.w = hidden || handy_fill ? -1.0f : 1.0f;
        const uint32_t casters = l.id == kMirrorLightId ? 32u : 128u;
        g.info.x = static_cast<int>(casters | (l.cast_shadow ? 1u << 24 : 0u) | ((l.light_fitting & 63u) << 25));
    }
    std::unordered_map<uint64_t, uint32_t> now;
    std::vector<uint64_t> ids(std::min<size_t>(light_count_, light_sources_.size()));
    for (uint32_t i = 0; i < ids.size(); ++i) {
        ids[i] = light_sources_[i]->id;
        if (auto [it, inserted] = now.emplace(ids[i], i); !inserted) {
            it->second = ~0u;
        }
    }
    const gpu::BufferResource& remap = rt_->PathRemap(renderer_->FrameIndex());
    uint32_t* out = static_cast<uint32_t*>(remap.mapped);
    for (uint32_t j = 0; j < gpu::kMaxLights; ++j) {
        uint32_t mapped = ~0u;
        if (j < pt_previous_ids_.size()) {
            if (auto it = now.find(pt_previous_ids_[j]); it != now.end()) {
                mapped = it->second;
            }
        }
        out[j] = mapped;
    }
    gpu_->FlushMapped(remap, 0, ~0ull);
    pt_previous_ids_ = std::move(ids);
}

void SceneRenderer::RecordPathTrace(gpu::CommandList& cmd, const ViewSetup& view) {
    const bool cut = glm::distance(rt_full_previous_eye_, view.eye) > kCameraCutDistance;
    const uint32_t previous = rt_full_written_;
    const uint32_t history = previous | (rt_full_history_ && pt_history_ && !cut ? 0u : 2u);
    cmd.UseTargets({{&rt_full_images_[0], gpu::Layout::General}, {&rt_full_images_[1], gpu::Layout::General}});
    for (RenderTarget& t : pt_images32_) {
        cmd.UseTargets({{&t, gpu::Layout::General}});
    }
    for (RenderTarget& t : pt_images16_) {
        cmd.UseTargets({{&t, gpu::Layout::General}});
    }
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, light_count_, static_cast<uint32_t>(frame_counter_ & 0xFFFFFFu), 0u);
    static const float reuse = EnvFloat("PT_PATH_RESTIR", 0.0f, 0.0f, 3.0f);
    static const float direct_samples = EnvFloat("PT_PATH_DIRECT_SAMPLES", kDirectSamples, 1.0f, 16.0f);
    static const float foliage_transmission = EnvFloat("PT_PATH_FOLIAGE_TRANSMISSION", 1.0f, 0.0f, 1.0f);
    static const float fill_unshadowed = [] {
        const char* value = std::getenv("PT_PATH_FILL");
        return value && std::string_view(value) == "unshadowed" ? 1.0f : 0.0f;
    }();
    push.f0 = glm::vec4(kSpatialRadius * static_cast<float>(extent_.height) / 1080.0f, static_cast<float>(history), reuse, fill_unshadowed);
    push.f1 = glm::vec4(rt_full_previous_jitter_, 0.0f, 0.0f);
    push.f2 = glm::vec4(std::floor(direct_samples), foliage_transmission, 0.0f, 0.0f);
    push.m = rt_full_previous_;
    const uint32_t groups_x = (extent_.width + 7) / 8;
    const uint32_t groups_y = (extent_.height + 7) / 8;
    const auto barrier = [&cmd] {
        cmd.GlobalBarrier(gpu::PipelineStage::ComputeShader, gpu::Bits(gpu::Access::ShaderStorageWrite), gpu::PipelineStage::ComputeShader | gpu::PipelineStage::FragmentShader, gpu::Access::ShaderStorageRead | gpu::Access::ShaderStorageWrite);
    };
    cmd.BindPipeline(gpu::BindPoint::Compute, pt_direct_);
    for (uint32_t pass = 0; pass < (reuse > 0.0f ? 2u : 1u); ++pass) {
        push.ids.w = pass;
        PushConstants(cmd, &push, sizeof(push));
        cmd.Dispatch(groups_x, groups_y, 1);
        barrier();
    }
    Stamp(cmd, 25);
    static const float reach = EnvFloat("PT_PATH_REACH", kPathReach, 0.5f, 200.0f);
    static const float bounces = EnvFloat("PT_PATH_BOUNCES", kPathBounces, 0.0f, 8.0f);
    static const float clamp_light = EnvFloat("PT_PATH_CLAMP", kPathClamp, 0.1f, 1.0e6f);
    pt_indirect_on_ = bounces >= 1.0f;
    const uint32_t half_x = ((extent_.width + 1) / 2 + 7) / 8;
    const uint32_t half_y = ((extent_.height + 1) / 2 + 7) / 8;
    if (pt_indirect_on_ && rr_frame_) {
        static const float one_path = EnvFloat("PT_RR_PATHS", 1.0f, 1.0f, 2.0f) < 1.5f ? 1.0f : 0.0f;
        cmd.BindPipeline(gpu::BindPoint::Compute, pt_indirect_rr_);
        push.ids = glm::uvec4(view.index, light_count_, static_cast<uint32_t>(frame_counter_ & 0xFFFFFFu), 0u);
        push.f0 = glm::vec4(reach, std::floor(bounces), kPathAlbedoMip, clamp_light);
        push.f1 = glm::vec4(fill_unshadowed, one_path, 0.0f, 0.0f);
        PushConstants(cmd, &push, sizeof(push));
        cmd.Dispatch(groups_x, groups_y, 1);
        barrier();
        Stamp(cmd, 26);
    } else if (pt_indirect_on_) {
        cmd.BindPipeline(gpu::BindPoint::Compute, pt_indirect_);
        push.ids = glm::uvec4(view.index, light_count_, static_cast<uint32_t>(frame_counter_ & 0xFFFFFFu), 0u);
        push.f0 = glm::vec4(reach, std::floor(bounces), kPathAlbedoMip, clamp_light);
        push.f1 = glm::vec4(fill_unshadowed, 0.0f, 0.0f, 0.0f);
        PushConstants(cmd, &push, sizeof(push));
        cmd.Dispatch(half_x, half_y, 1);
        barrier();
        Stamp(cmd, 26);
        static const float diffuse_frames = EnvFloat("PT_PATH_FRAMES", kDiffuseFrames, 1.0f, 256.0f);
        static const float specular_frames = EnvFloat("PT_PATH_SPECULAR_FRAMES", kSpecularFrames, 1.0f, 256.0f);
        static const float sigma = EnvFloat("PT_PATH_SIGMA", kLuminanceSigma, 0.1f, 1000.0f);
        static const bool full_filter = [] {
            const char* value = std::getenv("PT_PATH_FILTER");
            return value && std::string_view(value) == "full";
        }();
        cmd.BindPipeline(gpu::BindPoint::Compute, full_filter ? rt_full_filter_ : pt_filter_);
        const std::initializer_list<uint32_t> modes = full_filter ? std::initializer_list<uint32_t>{3u, 4u, 5u, 6u, 7u, 8u, 9u}
                                                                  : std::initializer_list<uint32_t>{0u, 1u, 2u, 3u, 4u, 5u, 6u};
        for (uint32_t mode : modes) {
            push.ids = glm::uvec4(view.index, light_count_, mode, history);
            push.f0 = glm::vec4(diffuse_frames, specular_frames, full_filter ? 0.0f : sigma, 0.0f);
            push.f1 = glm::vec4(rt_full_previous_jitter_, 0.0f, 0.0f);
            PushConstants(cmd, &push, sizeof(push));
            cmd.Dispatch(half_x, half_y, 1);
            barrier();
        }
    } else {
        Stamp(cmd, 26);
    }
    Stamp(cmd, 27);
    {
        cmd.BindPipeline(gpu::BindPoint::Compute, pt_layer_);
        const bool coat = toggles.local_reflections && lighting_->screen.local_reflections;
        const ScreenSettings& screen = lighting_->screen;
        static const float layer_bounces = EnvFloat("PT_PATH_LAYER_BOUNCES", 3.0f, 1.0f, 8.0f);
        const bool fsr_rr = rr_frame_ && (up_.kind == UpscalerKind::Fsr4 || fsr_rr_input_check_);
        for (uint32_t mode = 0; mode < 3u; ++mode) {
            push.ids = glm::uvec4(view.index, light_count_, mode, history);
            push.f0 = glm::vec4(reach, std::floor(layer_bounces), static_cast<float>(frame_counter_ & 0xFFFFFFu), clamp_light);
            static const bool mirror_linear = std::getenv("PT_PATH_MIRROR_LINEAR") != nullptr;
            const bool dlss_rr = rr_frame_ && up_.kind == UpscalerKind::Dlss;
            push.f1 = glm::vec4(0.0f, 1.0f, 0.0f, (coat ? 1.0f : 0.0f) + (mirror_linear ? 2.0f : 0.0f) + (fsr_rr ? 4.0f : 0.0f) +
                               (dlss_rr ? 8.0f : 0.0f));
            push.f2 = glm::vec4(screen.reflect_scale, screen.reflect_bias, rt_full_previous_jitter_);
            push.m = rt_full_previous_;
            PushConstants(cmd, &push, sizeof(push));
            cmd.Dispatch(groups_x, groups_y, 1);
            barrier();
        }
    }
    Stamp(cmd, 28);
    pt_history_ = true;
    if (PathReference()) {
        const glm::mat4& vp = frame_->views[view.index].view_projection;
        bool reset = !pt_reference_valid_;
        for (int c = 0; c < 4 && !reset; ++c) {
            for (int r = 0; r < 4; ++r) {
                reset = reset || std::abs(vp[c][r] - pt_reference_view_[c][r]) > 1.0e-6f;
            }
        }
        if (reset) {
            pt_reference_frames_ = 0;
        }
        pt_reference_view_ = vp;
        pt_reference_valid_ = true;
        static const float spp = EnvFloat("PT_PATHTRACE_REFERENCE_SPP", 4.0f, 1.0f, 64.0f);
        static const float ref_bounces = EnvFloat("PT_PATHTRACE_REFERENCE_BOUNCES", 8.0f, 0.0f, 16.0f);
        cmd.BindPipeline(gpu::BindPoint::Compute, pt_reference_);
        push.ids = glm::uvec4(view.index, light_count_, static_cast<uint32_t>(frame_counter_ & 0xFFFFFFu), reset ? 1u : 0u);
        static const float ref_debug = EnvFloat("PT_PATHTRACE_REFERENCE_DEBUG", 0.0f, 0.0f, 3.0f);
        push.f0 = glm::vec4(spp, std::floor(ref_bounces), 200.0f, ref_debug);
        push.f1 = glm::vec4(0.0f, 1.0f, 0.0f, toggles.local_reflections && lighting_->screen.local_reflections ? 1.0f : 0.0f);
        push.f2 = glm::vec4(lighting_->screen.reflect_scale, lighting_->screen.reflect_bias, 0.0f, 0.0f);
        PushConstants(cmd, &push, sizeof(push));
        cmd.Dispatch(groups_x, groups_y, 1);
        barrier();
        ++pt_reference_frames_;
        if (static const bool log = std::getenv("PT_RT_LOG") != nullptr; log || pt_reference_frames_ % 64 == 0) {
            LogInfo("ray tracing: path traced reference: {} frames, {} samples per pixel", pt_reference_frames_,
                    static_cast<uint32_t>(spp) * pt_reference_frames_);
        }
    }
}

void SceneRenderer::RecordPathLayers(gpu::CommandList& cmd, const ViewSetup& view) {
    cmd.UseTargets({{&pt_images16_[8], gpu::Layout::General}, {&pt_images16_[9], gpu::Layout::General},
                     {&pt_images16_[10], gpu::Layout::General}, {&depth_, gpu::Layout::DepthReadOnly},
                     {&diffuse_, gpu::Layout::ColorAttachment}, {&specular_, gpu::Layout::ColorAttachment}});
    BindSets(cmd, gpu::BindPoint::Graphics);
    cmd.BeginPass(ViewArea(view), {{&diffuse_, false, {}}, {&specular_, false, {}}});
    const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0u, 2u, rt_full_written_);
    Fullscreen(cmd, pt_resolve_multiply_, push);
    push.ids.z = 3u;
    Fullscreen(cmd, pt_resolve_, push);
    cmd.EndPass();
    cmd.UseTargets({{&diffuse_, gpu::Layout::ShaderReadOnly}, {&specular_, gpu::Layout::ShaderReadOnly}});
    if (rr_frame_) {
        RecordRrGuides(cmd, view);
        if (up_.kind == UpscalerKind::Fsr4 || fsr_rr_input_check_) RecordFsrRayRegeneration(cmd, view);
    }
}

void SceneRenderer::RecordRrGuides(gpu::CommandList& cmd, const ViewSetup& view) {
    cmd.UseTargets({{&pt_images16_[7], gpu::Layout::General}, {&pt_images16_[8], gpu::Layout::General}, {&rt_full_images_[10], gpu::Layout::General},
                     {&depth_, gpu::Layout::DepthReadOnly}, {&normal_, gpu::Layout::ShaderReadOnly},
                     {&material_, gpu::Layout::ShaderReadOnly}, {&albedo_, gpu::Layout::ShaderReadOnly},
                     {&rr_diffuse_albedo_, gpu::Layout::ColorAttachment}, {&rr_specular_albedo_, gpu::Layout::ColorAttachment},
                     {&rr_normal_roughness_, gpu::Layout::ColorAttachment}, {&rr_hit_distance_, gpu::Layout::ColorAttachment}});
    BindSets(cmd, gpu::BindPoint::Graphics);
    cmd.BeginPass(extent_, {{&rr_diffuse_albedo_, false, {}}, {&rr_specular_albedo_, false, {}}, {&rr_normal_roughness_, false, {}},
                             {&rr_hit_distance_, false, {}}});
    const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, up_.kind == UpscalerKind::Dlss ? 1u : 0u, 0u, 0u);
    Fullscreen(cmd, pt_rr_guides_, push);
    cmd.EndPass();
    cmd.UseTargets({{&rr_diffuse_albedo_, gpu::Layout::ShaderReadOnly}, {&rr_specular_albedo_, gpu::Layout::ShaderReadOnly},
                     {&rr_normal_roughness_, gpu::Layout::ShaderReadOnly}, {&rr_hit_distance_, gpu::Layout::ShaderReadOnly}});
}

void SceneRenderer::RecordRasterRrGuides(gpu::CommandList& cmd, const ViewSetup& view) {
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}, {&normal_, gpu::Layout::ShaderReadOnly}, {&material_, gpu::Layout::ShaderReadOnly},
                     {&albedo_, gpu::Layout::ShaderReadOnly}, {&rr_diffuse_albedo_, gpu::Layout::ColorAttachment},
                     {&rr_specular_albedo_, gpu::Layout::ColorAttachment}, {&rr_normal_roughness_, gpu::Layout::ColorAttachment},
                     {&rr_hit_distance_, gpu::Layout::ColorAttachment}});
    BindSets(cmd, gpu::BindPoint::Graphics);
    cmd.BeginPass(extent_, {{&rr_diffuse_albedo_, false, {}}, {&rr_specular_albedo_, false, {}}, {&rr_normal_roughness_, false, {}},
                             {&rr_hit_distance_, false, {}}});
    const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0u, 0u, 0u);
    Fullscreen(cmd, rt_rr_guides_, push);
    cmd.EndPass();
    cmd.UseTargets({{&rr_diffuse_albedo_, gpu::Layout::ShaderReadOnly}, {&rr_specular_albedo_, gpu::Layout::ShaderReadOnly},
                     {&rr_normal_roughness_, gpu::Layout::ShaderReadOnly}, {&rr_hit_distance_, gpu::Layout::ShaderReadOnly}});
}

bool SceneRenderer::PathReference() const {
    return rt_path_ && pt_reference_ && pt_reference_out_;
}

void SceneRenderer::RecordPathReferenceOut(gpu::CommandList& cmd, const ViewSetup& view) {
    const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0u, 0u, 0u);
    Fullscreen(cmd, pt_reference_out_, push);
}

void SceneRenderer::RecordPathLayerOut(gpu::CommandList& cmd, const ViewSetup& view) {
    const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0u, 0u, 0u);
    Fullscreen(cmd, pt_layer_out_, push);
}

void SceneRenderer::RecordPathIndirect(gpu::CommandList& cmd, const ViewSetup& view) {
    cmd.UseTargets({{&rt_full_images_[10], gpu::Layout::General}, {&rt_full_images_[16], gpu::Layout::General},
                     {&depth_, gpu::Layout::DepthReadOnly}, {&normal_, gpu::Layout::ShaderReadOnly},
                     {&material_, gpu::Layout::ShaderReadOnly},
                     {&diffuse_, gpu::Layout::ColorAttachment}, {&specular_, gpu::Layout::ColorAttachment}});
    BindSets(cmd, gpu::BindPoint::Graphics);
    cmd.BeginPass(ViewArea(view), {{&diffuse_, false, {}}, {&specular_, false, {}}});
    const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0u, 1u, 0u);
    if (pt_indirect_on_) {
        Fullscreen(cmd, rr_frame_ ? pt_resolve_rr_ : pt_resolve_, push);
    }
    cmd.EndPass();
    cmd.UseTargets({{&diffuse_, gpu::Layout::ShaderReadOnly}, {&specular_, gpu::Layout::ShaderReadOnly}});
}

void SceneRenderer::RecordPathResolve(gpu::CommandList& cmd, const ViewSetup& view, uint32_t written) {
    cmd.UseTargets({{&rt_full_images_[0], gpu::Layout::General}, {&pt_images16_[0], gpu::Layout::General},
                     {&diffuse_, gpu::Layout::ColorAttachment}, {&specular_, gpu::Layout::ColorAttachment}});
    BindSets(cmd, gpu::BindPoint::Graphics);
    cmd.BeginPass(ViewArea(view), {{&diffuse_, false, {}}, {&specular_, false, {}}});
    const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0u, 0u, written ? 4u : 0u);
    Fullscreen(cmd, rr_frame_ ? pt_resolve_rr_ : pt_resolve_, push);
    cmd.EndPass();
}

}
