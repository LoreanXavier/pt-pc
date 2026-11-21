#include <chrono>
#include "engine/render/upscale/neural_native.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "engine/core/log.h"
#include "engine/render/scene_lighting.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/upscale/frame_generation.h"
#include "engine/render/d3d12/vk_d3d12_share.h"
#include "engine/render/upscale/streamline.h"
#ifdef _WIN32
#include "engine/render/d3d12/backend/d3d12_native.h"
#endif

namespace pt {
namespace {

constexpr gpu::Format kMotionFormat = gpu::Format::R16G16Sfloat;
constexpr gpu::Format kReactiveFormat = gpu::Format::R8Unorm;
constexpr gpu::Format kUpscaleColorFormat = gpu::Format::R16G16B16A16Sfloat;
constexpr gpu::Format kPostDepthFormat = gpu::Format::R32Sfloat;
constexpr gpu::Format kExposureFormat = gpu::Format::R32Sfloat;
constexpr gpu::Format kHandyFactorFormat = gpu::Format::R16Sfloat;
constexpr float kReactiveScale = 3.0f;
constexpr float kFlashlightReactiveGain = 1.5f;
constexpr float kFlashlightReactiveCap = 0.3f;
constexpr float kHandyDemodFloor = 0.02f;

UpscaleImage Wrap(bool on_vulkan, const RenderTarget& t) {
    UpscaleImage image;
    image.handle = t.image.image;
    image.extent = gpu::vk::Native(t.Extent());
    if (!on_vulkan) {
        return image;
    }
    image.image = gpu::vk::Native(t.image.image);
    image.view = gpu::vk::Native(t.image.view);
    image.format = gpu::vk::Native(t.image.format);
    image.layout = gpu::vk::Native(t.layout);
    image.usage = t.image.usage.bits;
    return image;
}

FrameGeneration* g_hud_backend = nullptr;

const vk::Image* PresentFrameGeneration(uint32_t image_index) {
    g_hud_backend = nullptr;
    FrameGeneration* fsr = UpscaleHost::Get().FrameGen();
    const vk::Image* hud = fsr ? fsr->Present(image_index) : nullptr;
    FrameGeneration* from = fsr;
    if (!hud) {
        if (FrameGeneration* xefg = UpscaleHost::Get().XefgFrameGen()) {
            hud = xefg->Present(image_index);
            from = xefg;
        }
    }
    if (!hud) {
        if (FrameGeneration* dlss = UpscaleHost::Get().DlssFrameGenImpl()) {
            hud = dlss->Present(image_index);
            from = dlss;
        }
    }
    if (!hud) {
        if (FrameGeneration* metalfx = UpscaleHost::Get().MetalFxFrameGen()) {
            hud = metalfx->Present(image_index);
            from = metalfx;
        }
    }
    g_hud_backend = hud ? from : nullptr;
    return hud;
}

void SetHudlessHooks(Renderer& renderer) {
    renderer.hudless = PresentFrameGeneration;
    renderer.ui_layer = [](uint32_t image_index) -> const vk::Image* { return g_hud_backend ? g_hud_backend->UiLayer(image_index) : nullptr; };
    renderer.ui_layer_over_rendered = [] { return g_hud_backend && g_hud_backend->UiLayerOverRendered(); };
}

void UpdateDlssFrameGeneration(Renderer& renderer, bool wanted) {
    FrameGeneration* fg = UpscaleHost::Get().DlssFrameGenImpl();
    if (!fg) {
        return;
    }
    if (!renderer.hudless) {
        SetHudlessHooks(renderer);
    }
    if (!renderer.frame_start) {
        renderer.frame_start = [](bool swapchain_recreation_pending) {
            FrameGeneration* dlss = UpscaleHost::Get().DlssFrameGenImpl();
            return dlss ? dlss->FrameStart(swapchain_recreation_pending) : FrameStartAction::Continue;
        };
        renderer.swapchain_failed = [] {
            FrameGeneration* dlss = UpscaleHost::Get().DlssFrameGenImpl();
            return dlss && dlss->SwapchainFailed();
        };
    }
    if (fg->Update(wanted)) {
        renderer.Resize(0, 0);
    }
}

void UpdateFrameGeneration(Renderer& renderer, bool wanted) {
    FrameGeneration* fg = UpscaleHost::Get().FrameGen();
    if (!fg) {
        return;
    }
    std::string reason;
    if (wanted && !fg->Available(reason)) {
        wanted = false;
    }
    vk::Context& ctx = renderer.Context();
    if (!wanted && !fg->Generating() && !ctx.SwapchainOwner() && !ctx.swapchain_hooks) {
        return;
    }
    if (!renderer.hudless) {
        SetHudlessHooks(renderer);
    }
    if (fg->Update(wanted)) {
        renderer.Resize(0, 0);
    }
}

void UpdateXefgFrameGeneration(Renderer& renderer, bool wanted) {
    FrameGeneration* fg = UpscaleHost::Get().XefgFrameGen();
    if (!fg) {
        return;
    }
    std::string reason;
    if (wanted && !fg->Available(reason)) {
        wanted = false;
    }
    if (wanted) {
        if (!renderer.hudless) {
            SetHudlessHooks(renderer);
        }
        if (!renderer.frame_start) {
            renderer.frame_start = [](bool pending) {
                FrameGeneration* xefg = UpscaleHost::Get().XefgFrameGen();
                return xefg ? xefg->FrameStart(pending) : FrameStartAction::Continue;
            };
        }
    }
    if (fg->Update(wanted)) {
        renderer.Resize(0, 0);
    }
}

void UpdateMetalfxFrameGeneration(Renderer& renderer, bool wanted) {
    FrameGeneration* fg = UpscaleHost::Get().MetalFxFrameGen();
    if (!fg) {
        return;
    }
    std::string reason;
    if (wanted && !fg->Available(reason)) {
        wanted = false;
    }
    if (!renderer.hudless) {
        SetHudlessHooks(renderer);
    }
    const bool was_generating = fg->Generating();
    fg->Update(wanted && !UpscaleHost::Get().MenuOpen());
    if (fg->Generating() != was_generating) {
        renderer.Resize(0, 0);
    }
    if (fg->Generating()) {
        renderer.generated_frame.generate = [fg]() -> const vk::Image* { return fg->GeneratedImage(); };
        renderer.generated_frame.sync = [fg](VkSemaphore& s, uint64_t& sv, VkSemaphore& w, uint64_t& wv) {
            return fg->ExternalSync(s, sv, w, wv);
        };
    } else {
        renderer.generated_frame.generate = nullptr;
        renderer.generated_frame.sync = nullptr;
    }
}

}

bool SceneRenderer::UpscalerAvailable(UpscalerKind kind, std::string& reason) {
    return UpscaleHost::Get().Available(kind, reason, false);
}

bool SceneRenderer::CreateUpscalePipelines() {
    gpu::GraphicsPipelineDesc motion;
    motion.layout = layout_;
    motion.fragment = "upscale_motion.frag";
    motion.colors = {kMotionFormat};
    motion.depth = kDepthTargetFormat;
    up_motion_ = gpu_->CreateGraphicsPipeline(motion);

    gpu::GraphicsPipelineDesc object;
    object.layout = layout_;
    object.vertex = "upscale_velocity.vert";
    object.fragment = "upscale_velocity.frag";
    object.mesh_input = true;
    object.colors = {kMotionFormat};
    object.depth = kDepthTargetFormat;
    object.depth_test = true;
    up_object_motion_ = gpu_->CreateGraphicsPipeline(object);

    gpu::GraphicsPipelineDesc reactive;
    reactive.layout = layout_;
    reactive.fragment = "upscale_reactive.frag";
    reactive.colors = {kReactiveFormat};
    up_reactive_ = gpu_->CreateGraphicsPipeline(reactive);

    gpu::GraphicsPipelineDesc resolve;
    resolve.layout = layout_;
    resolve.fragment = "upscale_resolve.frag";
    resolve.colors = {kUpscaleColorFormat, kPostDepthFormat, kUpscaleColorFormat};
    up_resolve_ = gpu_->CreateGraphicsPipeline(resolve);

    gpu::GraphicsPipelineDesc demod;
    demod.layout = layout_;
    demod.fragment = "upscale_demod.frag";
    demod.colors = {kHdrTargetFormat, kHandyFactorFormat};
    up_demod_ = gpu_->CreateGraphicsPipeline(demod);

    gpu::GraphicsPipelineDesc fg_lens;
    fg_lens.layout = layout_;
    fg_lens.fragment = "fg_lens_inputs.frag";
    fg_lens.colors = {kMotionFormat};
    fg_lens.depth = kDepthTargetFormat;
    fg_lens.depth_test = true;
    fg_lens.depth_write = true;
    fg_lens.depth_compare = gpu::CompareOp::Always;
    up_fg_lens_ = gpu_->CreateGraphicsPipeline(fg_lens);

    gpu::GraphicsPipelineDesc nr_encode;
    nr_encode.layout = layout_;
    nr_encode.fragment = "neural_encode.frag";
    nr_encode.colors = {kHdrTargetFormat};
    up_nr_encode_ = gpu_->CreateGraphicsPipeline(nr_encode);

    gpu::GraphicsPipelineDesc nr_decode;
    nr_decode.layout = layout_;
    nr_decode.fragment = "neural_decode.frag";
    nr_decode.colors = {kHdrTargetFormat};
    up_nr_decode_ = gpu_->CreateGraphicsPipeline(nr_decode);

    if (!up_motion_ || !up_object_motion_ || !up_reactive_ || !up_resolve_ || !up_demod_ || !up_fg_lens_ || !up_nr_encode_ || !up_nr_decode_) {
        LogError("scene renderer: upscale pipeline creation failed");
        return false;
    }
    return true;
}

void SceneRenderer::DestroyUpscalePipelines() {
    gpu::PipelineHandle* all[] = {&up_motion_, &up_object_motion_, &up_reactive_,  &up_resolve_,
                                  &up_demod_,  &up_fg_lens_,        &up_nr_encode_, &up_nr_decode_};
    for (gpu::PipelineHandle* p : all) {
        if (*p) {
            gpu_->DestroyPipeline(*p);
            *p = {};
        }
    }
}

bool SceneRenderer::CreateUpscaleTargets(gpu::Extent2D render, gpu::Extent2D output) {
    const gpu::ImageUsageFlags color = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled;
    const gpu::AspectFlags c = gpu::Aspect::Color;
    const gpu::ImageUsageFlags exposure_usage = gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferDst |
                                                (gpu_->Info().backend == gpu::Backend::D3D12 ? gpu::ImageUsageFlags(gpu::ImageUsage::ColorAttachment)
                                                                                             : gpu::ImageUsageFlags{});
    const bool ok = CreateTarget(motion_, kMotionFormat, render, color | gpu::ImageUsage::TransferSrc, c) && CreateTarget(reactive_, kReactiveFormat, render, color | gpu::ImageUsage::TransferSrc, c) &&
                    CreateTarget(opaque_, kHdrTargetFormat, render, color | gpu::ImageUsage::TransferDst | gpu::ImageUsage::Storage, c) &&
                    CreateTarget(handy_factor_, kHandyFactorFormat, render, color, c) &&
                    CreateTarget(exposure_image_, kExposureFormat, {1, 1}, exposure_usage, c) &&
                    CreateTarget(upscaled_, kUpscaleColorFormat, output,
                                 gpu::ImageUsage::Storage | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferDst |
                                     gpu::ImageUsage::ColorAttachment, c) &&
                    CreateTarget(post_hdr_, kUpscaleColorFormat, output, color, c) && CreateTarget(post_depth_, kPostDepthFormat, output, color, c) &&
                    CreateTarget(post_object_velocity_, kUpscaleColorFormat, output, color, c);
    if (!ok) {
        LogError("scene renderer: cannot create upscale targets {}x{} -> {}x{}", render.width, render.height, output.width, output.height);
    }
    return ok;
}

void SceneRenderer::PrepareNgxReload() {
    if (gpu_->Info().backend != gpu::Backend::D3D12) return;
    gpu_->WaitIdle();
    if (up_.kind == UpscalerKind::Dlss) {
        up_.backend = nullptr;
        up_.created = false;
        up_.reset = true;
        up_.failed_key.clear();
    }
    UpscaleHost::Get().ReleaseD3D12Ngx();
}

void SceneRenderer::DestroyUpscaleTargets() {
    ++neural_output_epoch_;
    RenderTarget* targets[] = {&motion_,    &reactive_,  &opaque_,     &handy_factor_,         &exposure_image_, &upscaled_,
                               &post_hdr_,  &post_depth_, &post_object_velocity_, &fg_motion_, &fg_depth_};
    for (RenderTarget* t : targets) {
        gpu_->DestroyImage(t->image);
        *t = RenderTarget{};
    }
    targets_upscaled_ = false;
}

void SceneRenderer::WriteUpscaleDescriptors(gpu::ImageBinding* images, bool post) {
    auto set = [&](uint32_t slot, const RenderTarget& t) {
        if (t.Valid()) {
            images[slot].view = t.image.view;
            images[slot].image = &t.image;
            images[slot].layout = gpu::Layout::ShaderReadOnly;
        }
    };
    set(gpu::kImgUpscaled, upscaled_);
    set(gpu::kImgOpaque, opaque_);
    set(gpu::kImgMotion, motion_);
    set(gpu::kImgHandyFactor, handy_factor_);
    if (post && targets_upscaled_) {
        set(gpu::kImgHdr, post_hdr_);
        set(gpu::kImgDepth, post_depth_);
        set(gpu::kImgObjectVelocity, post_object_velocity_);
    }
}

bool SceneRenderer::BeginUpscaleFrame(gpu::Extent2D output) {
    streamline::SetFrameGenMultiplier(static_cast<unsigned>(std::clamp(upscale.dlssg_multiplier, 2, 4)));
    if (gpu_->Info().backend == gpu::Backend::D3D12) {
        const bool active = SetupUpscaler(output);
#ifdef _WIN32
        using gpu::d3d12::FrameGenOwner;
        const bool generate = active && up_.backend != nullptr && vr_eye_ < 0;
        const FrameGenOwner owner = !generate                                         ? FrameGenOwner::None
                                    : upscale.frame_generation == FrameGenKind::Xess ? FrameGenOwner::Xess
                                    : upscale.frame_generation == FrameGenKind::Fsr  ? FrameGenOwner::Fsr
                                    : upscale.frame_generation == FrameGenKind::Dlss ? FrameGenOwner::Dlss
                                                                                      : FrameGenOwner::None;
        renderer_->UpdateFrameGenerationD3D12(owner, UpscaleHost::Get().MenuOpen());
#endif
        return active;
    }
    UpscaleHost::Get().FrameTick();
    const bool active = SetupUpscaler(output);
    const bool generate = active && up_.backend != nullptr;
    if (streamline::Active()) {
        UpdateDlssFrameGeneration(*renderer_, upscale.frame_generation == FrameGenKind::Dlss && generate);
    } else if (upscale.frame_generation == FrameGenKind::Metalfx) {
        UpdateMetalfxFrameGeneration(*renderer_, generate && vr_eye_ < 0);
    } else {
        if (upscale.frame_generation == FrameGenKind::Xess) {
            UpdateFrameGeneration(*renderer_, false);
            UpdateXefgFrameGeneration(*renderer_, generate && vr_eye_ < 0);
        } else {
            UpdateXefgFrameGeneration(*renderer_, false);
            UpdateFrameGeneration(*renderer_, upscale.frame_generation == FrameGenKind::Fsr && generate);
        }
    }
    return active;
}

bool SceneRenderer::SetupUpscaler(gpu::Extent2D output) {
    if (frame_counter_ == 0) {
        if (const char* kind = std::getenv("PT_UPSCALER")) {
            if (!ParseUpscaler(kind, upscale.kind)) {
                LogWarn("upscale: unknown PT_UPSCALER {}", kind);
            }
        }
        if (const char* quality = std::getenv("PT_UPSCALE_QUALITY")) {
            if (!ParseUpscaleQuality(quality, upscale.quality)) {
                LogWarn("upscale: unknown PT_UPSCALE_QUALITY {}", quality);
            }
        }
        if (const char* scale = std::getenv("PT_UPSCALE_SCALE")) {
            upscale.quality = UpscaleQuality::Custom;
            upscale.scale = std::clamp(static_cast<float>(std::atof(scale)), 0.25f, 1.0f);
        }
        if (const char* sharpness = std::getenv("PT_UPSCALE_SHARPNESS")) {
            upscale.sharpness = std::clamp(static_cast<float>(std::atof(sharpness)), 0.0f, 1.0f);
        }
        if (const char* model = std::getenv("PT_DLSS_MODEL")) {
            if (!ParseDlssModel(model, upscale.dlss_model)) {
                LogWarn("upscale: unknown PT_DLSS_MODEL {}", model);
            }
        }
        if (const char* frame_generation = std::getenv("PT_FRAME_GENERATION")) {
            if (!ParseFrameGen(frame_generation, upscale.frame_generation)) {
                LogWarn("upscale: unknown PT_FRAME_GENERATION {}", frame_generation);
            }
        }
    }
    const bool was_native_neural_only = up_.native_neural_only;
    up_.native_neural_only = false;
    up_.enabled = false;
    upscale_stats_.active = false;
    upscale_stats_.kind = upscale.kind;
    const bool lit = debug_view == DebugView::Lit || debug_view == DebugView::Ambient;
    const UpscalerKind kind = upscale.kind;
    if (kind == UpscalerKind::Off || !lit || output.width == 0 || output.height == 0) {
        if (up_.created && up_.backend) {
            gpu_->WaitIdle();
            up_.backend->Release();
        }
        up_.created = false;
        up_.ray_reconstruction = false;
        up_.backend = nullptr;
        up_.kind = UpscalerKind::Off;
        up_.failed_key.clear();
        upscale_stats_.error.clear();
        if (kind == UpscalerKind::Off && neural.enabled && nr_native::Enabled() &&
            gpu_->Info().backend == gpu::Backend::D3D12 && lit && output.width && output.height && vr_eye_ < 0) {
            up_.native_neural_only = true;
            up_.enabled = true;
            up_.render = output;
            up_.jitter_pixels = up_.jitter_ndc = glm::vec2(0.0f);
            up_.mip_bias = 0.0f;
            up_.warp_test = false;
            if (!was_native_neural_only) up_.reset = true;
            return true;
        }
        return false;
    }
    std::string reason;
    if (!UpscaleHost::Get().Available(kind, reason)) {
        upscale_stats_.error = reason;
        return false;
    }
    const gpu::Extent2D render = gpu::vk::FromNative(UpscaleRenderExtent(gpu::vk::Native(output), upscale.quality, upscale.scale));
    UpscaleSettings key_settings = upscale;
    key_settings.sharpness = 0.0f;
    key_settings.frame_generation = FrameGenKind::Off;
    key_settings.dlssg_multiplier = 2;
    if (kind != UpscalerKind::Dlss) {
        key_settings.dlss_model = DlssModel::Auto;
    }
    std::string rr_reason;
    const bool traced = raytracing.path_traced || raytracing.full || raytracing.shadows || raytracing.contact_shadows ||
                        raytracing.ambient_occlusion || raytracing.reflections;
    const bool ray_reconstruction = (kind == UpscalerKind::Dlss || kind == UpscalerKind::Fsr4) && upscale.ray_reconstruction && traced && rt_ != nullptr &&
                                    toggles.lights && vr_eye_ < 0 && RayReconstructionAvailable(rr_reason);
    key_settings.ray_reconstruction = ray_reconstruction;
    const bool changed = !up_.created || up_.kind != kind || !(up_.created_settings == key_settings) || up_.created_render.width != render.width ||
                         up_.created_render.height != render.height || up_.created_output.width != output.width ||
                         up_.created_output.height != output.height;
    if (changed) {
        const std::string failed_key = std::format("{}|{}|{}x{}|{}x{}", UpscalerKey(kind), UpscaleQualityKey(upscale.quality), render.width,
                                                   render.height, output.width, output.height);
        if (failed_key == up_.failed_key && std::chrono::steady_clock::now() - up_.failed_at < std::chrono::seconds(3)) {
            return false;
        }
        gpu_->WaitIdle();
        double create_ms = 0.0;
        if (up_.created && up_.backend) {
            up_.backend->Release();
        }
        up_.created = false;
        up_.ray_reconstruction = false;
        up_.backend = kind == UpscalerKind::Spatial ? nullptr : UpscaleHost::Get().Backend(kind);
        if (kind != UpscalerKind::Spatial) {
            if (!up_.backend) {
                up_.failed_key = failed_key;
                up_.failed_at = std::chrono::steady_clock::now();
                upscale_stats_.error = "backend unavailable";
                return false;
            }
            UpscaleCreate create;
            create.render = gpu::vk::Native(render);
            create.display = gpu::vk::Native(output);
            create.quality = upscale.quality;
            create.dlss_model = upscale.dlss_model;
            create.color_format = gpu::vk::Native(kHdrTargetFormat);
            create.ray_reconstruction = ray_reconstruction;
            bool created = false;
            const auto create_start = std::chrono::steady_clock::now();
            const bool on_vulkan = gpu_->Info().backend == gpu::Backend::Vulkan;
            gpu_->Submit([&](gpu::CommandList& cmd) {
                create.list = &cmd;
                created = up_.backend->Create(on_vulkan ? gpu::vk::Native(cmd) : VK_NULL_HANDLE, create);
            });
            create_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - create_start).count();
            if (!created) {
                LogError("upscale: {} could not be created for {}x{} -> {}x{}", UpscalerName(kind), render.width, render.height, output.width,
                         output.height);
                up_.failed_key = failed_key;
                up_.failed_at = std::chrono::steady_clock::now();
                upscale_stats_.error = "creation failed, see pt.log";
                up_.backend = nullptr;
                return false;
            }
        }
        LogInfo("upscale: {}{} {} at {}x{} -> {}x{} (created in {:.0f} ms)", UpscalerName(kind), ray_reconstruction ? " Ray Reconstruction" : "",
                UpscaleQualityName(upscale.quality), render.width, render.height, output.width, output.height, create_ms);
        up_.failed_key.clear();
        up_.kind = kind;
        up_.created = true;
        up_.ray_reconstruction = ray_reconstruction;
        up_.created_settings = key_settings;
        up_.created_render = render;
        up_.created_output = output;
        up_.reset = true;
        up_.jitter_index = 0;
        upscale_ms_sum_ = inputs_ms_sum_ = gpu_ms_sum_ = cpu_ms_sum_ = 0.0;
        upscale_samples_ = gpu_samples_ = cpu_samples_ = 0;
    }
    up_.render = render;
    up_.warp_test = kind == UpscalerKind::Spatial && render.width == output.width && render.height == output.height &&
                    std::getenv("PT_UPSCALE_WARP") != nullptr;
    up_.phase_count = std::max(1, JitterPhaseCount(render.width, output.width));
    up_.jitter_pixels = JitterOffset(up_.jitter_index, up_.phase_count);
    up_.jitter_index = (up_.jitter_index + 1) % up_.phase_count;
    if (up_.warp_test) {
        up_.jitter_pixels = glm::vec2(0.0f);
    }
    static const glm::vec2 jitter_scale = [] {
        glm::vec2 s(1.0f);
        if (const char* v = std::getenv("PT_UPSCALE_JITTER_SCALE")) std::sscanf(v, "%f %f", &s.x, &s.y);
        return s;
    }();
    up_.jitter_pixels *= jitter_scale;
    up_.jitter_ndc = 2.0f * up_.jitter_pixels / glm::vec2(static_cast<float>(render.width), static_cast<float>(render.height));
    up_.mip_bias = up_.backend ? up_.backend->MipBias(static_cast<float>(render.width) / static_cast<float>(output.width)) : 0.0f;
    if (const char* bias = std::getenv("PT_UPSCALE_MIP_BIAS")) {
        up_.mip_bias = static_cast<float>(std::atof(bias));
    }
    up_.enabled = true;
    upscale_stats_.active = true;
    upscale_stats_.render = render;
    upscale_stats_.output = output;
    upscale_stats_.error.clear();
    return true;
}

void SceneRenderer::RecordOpaqueSnapshot(gpu::CommandList& cmd, RenderTarget& output) {
    cmd.EndPass();
    cmd.UseTargets({{&output, gpu::Layout::TransferSrc}, {&opaque_, gpu::Layout::TransferDst}});
    gpu::ImageCopy copy;
    copy.src = {gpu::Aspect::Color, 0, 0, 1};
    copy.dst = copy.src;
    copy.extent = {extent_.width, extent_.height, 1};
    cmd.CopyImage(output.image, gpu::Layout::TransferSrc, opaque_.image, gpu::Layout::TransferDst, {&copy, 1});
    cmd.UseTargets({{&output, gpu::Layout::ColorAttachment}, {&opaque_, gpu::Layout::ShaderReadOnly}});
    cmd.BeginPass(extent_, {{&output, false, {}}}, &depth_, true, false);
}

void SceneRenderer::RecordUpscaleMotion(gpu::CommandList& cmd, const ViewSetup& view, bool fsr_rr) {
    RenderTarget& target = fsr_rr ? fsr_rr_motion_ : motion_;
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}, {&target, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent_, {{&target, false, {}}}, &depth_, true, false);
    gpu::PassPush push;
    push.ids = glm::uvec4(0, 0, 0, view.index);
    push.m = previous_view_projection_;
    Fullscreen(cmd, fsr_rr ? fsr_rr_motion_trace_ : up_motion_, push);
    cmd.BindPipeline(gpu::BindPoint::Graphics, fsr_rr ? fsr_rr_velocity_ : up_object_motion_);
    for (const Draw& d : draws_) {
        const SubMesh& sub = *d.sub;
        if (d.previous_model == gpu::kInvalid || sub.pass != RenderPass::Opaque || sub.kind != gpu::kKindDeferred || !Visible(d, view)) {
            continue;
        }
        cmd.SetCullMode(sub.double_sided ? gpu::CullMode::None : gpu::CullMode::Back);
        cmd.SetFrontFace(gpu::FrontFace::CounterClockwise);
        cmd.BindVertexBuffer(d.mesh->vertices.buffer, 0);
        cmd.BindIndexBuffer(d.mesh->indices.buffer, 0, gpu::IndexType::Uint32);
        gpu::DrawPush draw;
        draw.model = d.transform;
        draw.ids = glm::uvec4(view.index, d.material, motion_view_, sub.skinned ? d.skin_base : gpu::kInvalid);
        draw.tint = d.tint;
        const uint32_t previous_skin = sub.skinned ? d.previous_skin : gpu::kInvalid;
        draw.aux[0] = glm::vec4(std::bit_cast<float>(d.previous_model), std::bit_cast<float>(previous_skin), std::bit_cast<float>(post_view_), 0.0f);
        PushConstants(cmd, &draw, sizeof(draw));
        cmd.DrawIndexed(sub.index_count, 1, sub.first_index, sub.vertex_offset, 0);
    }
    cmd.EndPass();
    cmd.UseTargets({{&target, gpu::Layout::ShaderReadOnly}});
}

void SceneRenderer::RecordUpscaleInputs(gpu::CommandList& cmd, const ViewSetup& view) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    cmd.WriteTimestamp(slot.upscale_queries, 0);
    RecordUpscaleMotion(cmd, view);
    gpu::PassPush push;
    cmd.UseTargets({{&motion_, gpu::Layout::ShaderReadOnly},
                     {&hdr_, gpu::Layout::ShaderReadOnly},
                     {&opaque_, gpu::Layout::ShaderReadOnly},
                     {&reactive_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent_, {{&reactive_, false, {}}});
    push = gpu::PassPush{};
    push.ids = glm::uvec4(0, 0, 0, view.index);
    static const glm::vec2 flashlight_reactive = [] {
        glm::vec2 v(kFlashlightReactiveGain, kFlashlightReactiveCap);
        if (const char* e = std::getenv("PT_FLASHLIGHT_REACTIVE")) {
            std::sscanf(e, "%f,%f", &v.x, &v.y);
        }
        return v;
    }();
    static const float reactive_scale = [] {
        const char* e = std::getenv("PT_REACTIVE_SCALE");
        return e ? static_cast<float>(std::atof(e)) : kReactiveScale;
    }();
    push.f0 = glm::vec4(reactive_scale, 0.9f, flashlight_reactive.x, flashlight_reactive.y);
    static const bool reactive_all = std::getenv("PT_REACTIVE_ALL") != nullptr;
    push.ids.x = reactive_all || reactive_frame_ ? 1u : 0u;
    push.f1 = glm::vec4(0.0f, 0.0f, 0.0f, -2.0f);
    if (lighting_) {
        for (const SceneLight& l : lighting_->lights) {
            if (l.id == kHandyLightId && (l.intensity.r + l.intensity.g + l.intensity.b > 0.0f)) {
                push.f1 = glm::vec4(l.position, l.cos_outer);
                push.f2 = glm::vec4(glm::normalize(l.direction), l.inv_cone_range);
                break;
            }
        }
    }
    push.m[0] = previous_handy_[0];
    push.m[1] = previous_handy_[1];
    previous_handy_[0] = push.f1;
    previous_handy_[1] = push.f2;
    Fullscreen(cmd, up_reactive_, push);
    cmd.EndPass();
    cmd.UseTargets({{&reactive_, gpu::Layout::ShaderReadOnly}});

    RecordNeuralRendering(cmd);

    static const bool demod_on = [] {
        const char* e = std::getenv("PT_HANDY_DEMOD");
        return !e || std::atoi(e) != 0;
    }();
    static const float demod_floor = [] {
        const char* e = std::getenv("PT_HANDY_DEMOD_FLOOR");
        const float value = e ? static_cast<float>(std::atof(e)) : kHandyDemodFloor;
        return std::isfinite(value) && value >= 1.0e-4f ? value : kHandyDemodFloor;
    }();
    uint32_t handy = gpu::kInvalid;
    bool handy_shadow = false;
    for (uint32_t i = 0; i < light_count_ && i < light_sources_.size(); ++i) {
        if (light_sources_[i]->id == kHandyLightId && (light_sources_[i]->hidden_views & view.view_bit) == 0) {
            handy = i;
            handy_shadow = toggles.shadows && i < light_shadow_views_.size() && (light_shadow_views_[i] & view.view_bit) != 0;
            break;
        }
    }
    const bool street_beam = handy != gpu::kInvalid && light_sources_[handy]->mask_soft;
    handy_demod_ = demod_on && handy != gpu::kInvalid && !street_beam && handy_factor_.Valid() && !up_.ray_reconstruction && !up_.native_neural_only;
    if (handy_demod_) {
        cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly},
                         {&diffuse_, gpu::Layout::ShaderReadOnly},
                         {&opaque_, gpu::Layout::ColorAttachment},
                         {&handy_factor_, gpu::Layout::ColorAttachment}});
        cmd.BeginPass(extent_, {{&opaque_, false, {}}, {&handy_factor_, false, {}}});
        gpu::PassPush demod;
        demod.ids = glm::uvec4(view.index, handy, handy_shadow ? 1u : 0u, 0u);
        demod.f0 = glm::vec4(demod_floor, 0.0f, 0.0f, 0.0f);
        Fullscreen(cmd, up_demod_, demod);
        cmd.EndPass();
        cmd.UseTargets({{&opaque_, gpu::Layout::ShaderReadOnly}, {&handy_factor_, gpu::Layout::ShaderReadOnly}});
    }
    cmd.WriteTimestamp(slot.upscale_queries, 1);
}

void SceneRenderer::RecordNeuralRendering(gpu::CommandList& cmd) {
    if (!neural.enabled || gpu_->Info().backend != gpu::Backend::Vulkan) {
        return;
    }
    if (!nr_in_.Valid() || !nr_out_.Valid() || !up_nr_encode_ || !up_nr_decode_) {
        return;
    }
    static const bool passthrough = std::getenv("PT_NEURAL_PASSTHROUGH") != nullptr;
    NeuralBackend* nr = passthrough ? nullptr : UpscaleHost::Get().Neural();
    const gpu::Extent2D extent = extent_;
    if (!passthrough) {
        if (!nr) {
            return;
        }
        if (!nr_feature_ready_ || nr_extent_.width != extent.width || nr_extent_.height != extent.height || nr_small_model_ != neural.small_model) {
            nr_feature_ready_ = nr->Create(gpu::vk::Native(cmd), extent.width, extent.height, neural.style, neural.intensity, 1.0f, neural.small_model);
            nr_extent_ = extent;
            nr_small_model_ = neural.small_model;
        }
        if (!nr_feature_ready_ || !nr->Created()) {
            return;
        }
    }
    cmd.BeginLabel("neural rendering");
    gpu::PassPush push{};
    push.f0 = glm::vec4(std::max(neural.paper_white, 1.0e-4f), std::clamp(neural.intensity, 1.0f, 3.0f), 0.0f, 0.0f);
    cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}, {&nr_in_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent, {{&nr_in_, false, {}}});
    Fullscreen(cmd, up_nr_encode_, push);
    cmd.EndPass();
    if (passthrough) {
        cmd.UseTargets({{&nr_in_, gpu::Layout::TransferSrc}, {&nr_out_, gpu::Layout::TransferDst}});
        gpu::ImageCopy copy;
        copy.src = {gpu::Aspect::Color, 0, 0, 1};
        copy.dst = copy.src;
        copy.extent = {extent.width, extent.height, 1};
        cmd.CopyImage(nr_in_.image, gpu::Layout::TransferSrc, nr_out_.image, gpu::Layout::TransferDst, {&copy, 1});
    } else {
        cmd.UseTargets({{&nr_in_, gpu::Layout::ShaderReadOnly},
                         {&depth_, gpu::Layout::DepthReadOnly},
                         {&motion_, gpu::Layout::ShaderReadOnly},
                         {&nr_out_, gpu::Layout::General}});
        auto fill = [](NeuralDispatch::Image& im, const RenderTarget& t) {
            im.image = reinterpret_cast<uint64_t>(gpu::vk::Native(t.image.image));
            im.view = reinterpret_cast<uint64_t>(gpu::vk::Native(t.image.view));
            im.format = static_cast<uint32_t>(gpu::vk::Native(t.image.format));
            im.width = t.Extent().width;
            im.height = t.Extent().height;
        };
        NeuralDispatch d;
        d.cmd = gpu::vk::Native(cmd);
        fill(d.color, nr_in_);
        fill(d.output, nr_out_);
        fill(d.depth, depth_);
        fill(d.motion, motion_);
        d.depth_inverted = true;
        d.motion_scale_x = static_cast<float>(extent.width);
        d.motion_scale_y = static_cast<float>(extent.height);
        d.scaling_ratio = 1.0f;
        d.reset = up_.reset;
        d.style = neural.style;
        d.intensity = neural.intensity;
        nr->Dispatch(d);
    }
    cmd.UseTargets({{&nr_out_, gpu::Layout::ShaderReadOnly}, {&nr_in_, gpu::Layout::ShaderReadOnly}, {&hdr_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent, {{&hdr_, false, {}}});
    Fullscreen(cmd, up_nr_decode_, push);
    cmd.EndPass();
    cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}});
    cmd.EndLabel();
}

void SceneRenderer::RecordUpscalePrepare(float dt) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    gpu::CommandList& cmd = renderer_->List();
    up_dispatch_ = UpscaleDispatch{};
    cmd.WriteTimestamp(slot.upscale_queries, 2);
    cmd.UseTargets({{&exposure_image_, gpu::Layout::TransferDst}});
    gpu::ClearColor value{};
    value.float32[0] = exposure_;
    cmd.ClearColorImage(exposure_image_.image, gpu::Layout::TransferDst, value, 1, 1);
    cmd.UseTargets({{&exposure_image_, gpu::Layout::ShaderReadOnly}});
    if (!up_.backend && !up_.native_neural_only) {
        return;
    }
    if (rr_raster_) {
        RecordRasterRrGuides(cmd, main_view_);
    }
    cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly},
                     {&depth_, gpu::Layout::ShaderReadOnly},
                     {&motion_, gpu::Layout::ShaderReadOnly},
                     {&reactive_, gpu::Layout::ShaderReadOnly},
                     {&upscaled_, gpu::Layout::General}});
    const bool on_vulkan = gpu_->Info().backend == gpu::Backend::Vulkan;
    UpscaleDispatch d;
    d.neural_enabled = neural.enabled;
    d.neural_style = neural.style;
    d.neural_intensity = neural.intensity;
    d.neural_small_model = neural.small_model;
    d.neural_scale = neural.scale;
    d.neural_every = neural.every;
    d.neural_pre = neural.pre;
    d.neural_output_epoch = neural_output_epoch_;
    d.cmd = on_vulkan ? gpu::vk::Native(cmd) : VK_NULL_HANDLE;
    d.list = &cmd;
    d.color = Wrap(on_vulkan, handy_demod_ ? opaque_ : hdr_);
    d.depth = Wrap(on_vulkan, depth_);
    d.motion = Wrap(on_vulkan, motion_);
    d.reactive = Wrap(on_vulkan, reactive_);
    d.exposure = Wrap(on_vulkan, exposure_image_);
    d.output = Wrap(on_vulkan, upscaled_);
    d.render = gpu::vk::Native(extent_);
    d.display = gpu::vk::Native(output_extent_);
    static const glm::vec2 told = [] {
        glm::vec2 s(1.0f);
        if (const char* e = std::getenv("PT_UPSCALER_JITTER")) std::sscanf(e, "%f %f", &s.x, &s.y);
        return s;
    }();
    d.jitter = up_.jitter_pixels * told;
    d.motion_scale = glm::vec2(static_cast<float>(extent_.width), static_cast<float>(extent_.height));
    d.pre_exposure = std::max(exposure_, 1.0e-12f);
    d.sharpness = upscale.sharpness;
    d.frame_ms = std::clamp(dt, 1.0e-4f, 0.25f) * 1000.0f;
    d.near_plane = camera_.near_plane;
    d.fov_y = camera_.fov_y;
    d.reset = up_.reset;
    d.frame_index = frame_counter_;
    upscale_stats_.ray_reconstruction = false;
    if (up_.ray_reconstruction) {
        if (!rr_frame_ && !rr_raster_) {
            return;
        }
        d.diffuse_albedo = Wrap(on_vulkan, rr_diffuse_albedo_);
        d.specular_albedo = Wrap(on_vulkan, rr_specular_albedo_);
        d.normal_roughness = Wrap(on_vulkan, rr_normal_roughness_);
        d.specular_hit_distance = Wrap(on_vulkan, rr_hit_distance_);
        static const bool before_transparency = [] {
            const char* e = std::getenv("PT_RR_TRANSPARENCY");
            return !e || std::atoi(e) != 0;
        }();
        if (before_transparency) {
            d.color_before_transparency = Wrap(on_vulkan, opaque_);
        }
        const float aspect = static_cast<float>(output_extent_.width) / static_cast<float>(std::max(1u, output_extent_.height));
        d.world_to_view = camera_.View();
        d.view_to_clip = camera_.Projection(aspect);
        upscale_stats_.ray_reconstruction = true;
    }
    if (streamline::Active()) {
        const float aspect = static_cast<float>(output_extent_.width) / static_cast<float>(std::max(1u, output_extent_.height));
        streamline::FrameConstants k;
        k.view_to_clip = camera_.Projection(aspect);
        k.world_to_clip = k.view_to_clip * camera_.View();
        k.previous_world_to_clip = previous_view_projection_;
        k.jitter = d.jitter;
        k.motion_scale = glm::vec2(1.0f);
        k.position = camera_.position;
        k.up = camera_.Up();
        k.right = camera_.Right();
        k.forward = camera_.Forward();
        k.near_plane = camera_.near_plane;
        k.fov_y = camera_.fov_y;
        k.aspect = aspect;
        k.reset = d.reset;
        streamline::SetConstants(k);
    }
    up_dispatch_ = d;
}

bool SceneRenderer::RecordUpscaleDispatch() {
    if ((!up_.backend && !up_.native_neural_only) || (!up_dispatch_.cmd && !up_dispatch_.list)) {
        return false;
    }
    bool upscaled = false;
    static const bool bypass = std::getenv("PT_UPSCALE_BYPASS") != nullptr;
    if (up_.native_neural_only || bypass) {
        auto& cmd = *up_dispatch_.list;
        cmd.UseTargets({{&hdr_, gpu::Layout::TransferSrc}, {&upscaled_, gpu::Layout::TransferDst}});
        gpu::ImageCopy copy;
        copy.src = {gpu::Aspect::Color, 0, 0, 1}; copy.dst = copy.src;
        copy.extent = {extent_.width, extent_.height, 1};
        cmd.CopyImage(hdr_.image, gpu::Layout::TransferSrc, upscaled_.image, gpu::Layout::TransferDst, {&copy, 1});
        cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}, {&upscaled_, gpu::Layout::General}});
        upscaled = true;
    } else {
#ifdef _WIN32
        static const bool force_pre = std::getenv("PT_NR_PRE") != nullptr;
        if ((up_dispatch_.neural_pre || force_pre) && gpu_->Info().backend == gpu::Backend::D3D12 && nr_native::Enabled() && up_dispatch_.neural_enabled) {
            RenderTarget& input = handy_demod_ ? opaque_ : hdr_;
            auto& cmd = *up_dispatch_.list;
            cmd.UseTargets({{&input, gpu::Layout::General}});
            UpscaleDispatch before = up_dispatch_;
            before.output = before.color;
            FrameSlot& slot = slots_[renderer_->FrameIndex()];
            const bool timed = std::getenv("PT_NR_TIMING") != nullptr;
            if (timed && !slot.neural_queries) slot.neural_queries = gpu_->CreateTimestampPool(4);
            before.neural_queries = timed ? slot.neural_queries : gpu::QueryPoolHandle{};
            slot.neural_timed = UpscaleHost::Get().RecordNativeNeural(before) && timed && slot.neural_queries;
            slot.neural_frame = frame_counter_;
            gpu::d3d12::ResumeAfterNative(cmd);
            BindSets(cmd, gpu::BindPoint::Graphics);
            BindSets(cmd, gpu::BindPoint::Compute);
            cmd.UseTargets({{&input, gpu::Layout::ShaderReadOnly}});
        }
#endif
        if (up_dispatch_.list) up_dispatch_.list->BeginLabel("upscaler evaluate");
        upscaled = up_.backend->Dispatch(up_dispatch_);
        if (up_dispatch_.list) up_dispatch_.list->EndLabel();
    }
    if (gpu_->Info().backend == gpu::Backend::D3D12) {
#ifdef _WIN32
        const bool pre_neural = (up_dispatch_.neural_pre || std::getenv("PT_NR_PRE") != nullptr) && !up_.native_neural_only;
        if (upscaled && nr_native::Enabled() && !pre_neural) {
            FrameSlot& slot = slots_[renderer_->FrameIndex()];
            const bool timed = up_dispatch_.neural_enabled && std::getenv("PT_NR_TIMING");
            if (timed && !slot.neural_queries) slot.neural_queries = gpu_->CreateTimestampPool(4);
            up_dispatch_.neural_queries = timed ? slot.neural_queries : gpu::QueryPoolHandle{};
            slot.neural_timed = UpscaleHost::Get().RecordNativeNeural(up_dispatch_) && timed && slot.neural_queries;
            slot.neural_frame = frame_counter_;
            gpu::d3d12::ResumeAfterNative(*up_dispatch_.list);
        }
#endif
        BindSets(*up_dispatch_.list, gpu::BindPoint::Graphics);
        BindSets(*up_dispatch_.list, gpu::BindPoint::Compute);
#ifdef _WIN32
        if (up_dispatch_.list) up_dispatch_.list->BeginLabel("frame generation inputs");
        PrepareFrameGenerationD3D12(upscaled);
        if (up_dispatch_.list) up_dispatch_.list->EndLabel();
#endif
        return upscaled;
    }
    if (up_dispatch_.list) up_dispatch_.list->BeginLabel("frame generation inputs");
    PrepareFrameGeneration(up_dispatch_, upscaled);
    if (up_dispatch_.list) up_dispatch_.list->EndLabel();
    return upscaled;
}

void SceneRenderer::PrepareFrameGeneration(const UpscaleDispatch& d, bool upscaled) {
    FrameGeneration* fg = upscale.frame_generation == FrameGenKind::Metalfx ? UpscaleHost::Get().MetalFxFrameGen()
                          : streamline::Active()                             ? UpscaleHost::Get().DlssFrameGenImpl()
                          : upscale.frame_generation == FrameGenKind::Xess   ? UpscaleHost::Get().XefgFrameGen()
                                                                             : UpscaleHost::Get().FrameGen();
    if (!upscaled || !fg || !fg->Generating()) {
        return;
    }
    FrameGenPrepare prepare;
    prepare.cmd = d.cmd;
    prepare.depth = d.depth;
    prepare.motion = d.motion;
    prepare.render = gpu::vk::Native(extent_);
    prepare.jitter = d.jitter;
    prepare.motion_scale = d.motion_scale;
    prepare.frame_ms = d.frame_ms;
    prepare.near_plane = d.near_plane;
    prepare.fov_y = d.fov_y;
    prepare.position = camera_.position;
    prepare.forward = camera_.Forward();
    prepare.up = camera_.Up();
    prepare.right = camera_.Right();
    prepare.reset = d.reset;
    static const bool lens_inputs = [] {
        const char* e = std::getenv("PT_FG_LENS_INPUTS");
        return !e || std::atoi(e) != 0;
    }();
    const bool lens = toggles.distortion && lighting_ && lighting_->valid && lighting_->screen.screen_distortion;
    if (lens && lens_inputs && upscale.frame_generation != FrameGenKind::Metalfx && up_fg_lens_ && vr_eye_ < 0) {
        gpu::vk::CommandList dispatch_list(d.cmd);
        if (RecordFrameGenLensInputs(dispatch_list)) {
            prepare.depth = Wrap(true, fg_depth_);
            prepare.motion = Wrap(true, fg_motion_);
            prepare.render = gpu::vk::Native(output_extent_);
            prepare.motion_scale = glm::vec2(static_cast<float>(output_extent_.width), static_cast<float>(output_extent_.height));
            prepare.jitter = d.jitter * glm::vec2(static_cast<float>(output_extent_.width) / static_cast<float>(extent_.width),
                                                  static_cast<float>(output_extent_.height) / static_cast<float>(extent_.height)) / 0.9f;
        }
    }
    fg->Prepare(prepare);
}

bool SceneRenderer::RecordFrameGenLensInputs(gpu::CommandList& cmd) {
    if (fg_motion_.Extent().width != output_extent_.width || fg_motion_.Extent().height != output_extent_.height || !fg_depth_.Valid()) {
        gpu_->WaitIdle();
        gpu_->DestroyImage(fg_motion_.image);
        gpu_->DestroyImage(fg_depth_.image);
        const gpu::ImageUsageFlags usage = gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc;
        if (!CreateTarget(fg_motion_, kMotionFormat, output_extent_, usage | gpu::ImageUsage::ColorAttachment, gpu::Aspect::Color) ||
            !CreateTarget(fg_depth_, kDepthTargetFormat, output_extent_, usage | gpu::ImageUsage::DepthStencilAttachment, gpu::Aspect::Depth)) {
            LogError("frame generation: the lens inputs could not be created at {}x{}", output_extent_.width, output_extent_.height);
            gpu_->DestroyImage(fg_motion_.image);
            gpu_->DestroyImage(fg_depth_.image);
            fg_motion_ = RenderTarget{};
            fg_depth_ = RenderTarget{};
        }
    }
    if (!fg_motion_.Valid() || !fg_depth_.Valid()) {
        return false;
    }
    BindSets(cmd, gpu::BindPoint::Graphics);
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly},
                     {&motion_, gpu::Layout::ShaderReadOnly},
                     {&fg_motion_, gpu::Layout::ColorAttachment},
                     {&fg_depth_, gpu::Layout::DepthAttachment}});
    cmd.BeginPass(output_extent_, {{&fg_motion_, false, {}}}, &fg_depth_, false, false);
    gpu::PassPush push;
    push.f0 = glm::vec4(static_cast<float>(output_extent_.width), static_cast<float>(output_extent_.height), static_cast<float>(extent_.width),
                        static_cast<float>(extent_.height));
    Fullscreen(cmd, up_fg_lens_, push);
    cmd.EndPass();
    cmd.UseTargets({{&fg_motion_, gpu::Layout::ShaderReadOnly}, {&fg_depth_, gpu::Layout::ShaderReadOnly}});
    return true;
}

#ifdef _WIN32
void SceneRenderer::PrepareFrameGenerationD3D12(bool upscaled) {
    if (!upscaled || !renderer_->FrameGeneratingD3D12()) {
        return;
    }
    const UpscaleDispatch& d = up_dispatch_;
    FrameGenPrepare p;
    p.render = gpu::vk::Native(extent_);
    p.jitter = d.jitter;
    p.motion_scale = d.motion_scale;
    p.frame_ms = d.frame_ms;
    p.near_plane = d.near_plane;
    p.fov_y = d.fov_y;
    p.position = camera_.position;
    p.forward = camera_.Forward();
    p.up = camera_.Up();
    p.right = camera_.Right();
    p.reset = d.reset;
    RenderTarget* depth = &depth_;
    RenderTarget* motion = &motion_;
    static const bool lens_inputs = [] {
        const char* e = std::getenv("PT_FG_LENS_INPUTS");
        return !e || std::atoi(e) != 0;
    }();
    const bool lens = toggles.distortion && lighting_ && lighting_->valid && lighting_->screen.screen_distortion;
    if (lens && lens_inputs && up_fg_lens_ && vr_eye_ < 0 && RecordFrameGenLensInputs(*d.list)) {
        depth = &fg_depth_;
        motion = &fg_motion_;
        p.render = gpu::vk::Native(output_extent_);
        p.motion_scale = glm::vec2(static_cast<float>(output_extent_.width), static_cast<float>(output_extent_.height));
        p.jitter = d.jitter * glm::vec2(static_cast<float>(output_extent_.width) / static_cast<float>(extent_.width),
                                        static_cast<float>(output_extent_.height) / static_cast<float>(extent_.height)) / 0.9f;
    }
    gpu::d3d12::FrameGenInputs in;
    in.depth = depth;
    in.motion = motion;
    in.input_width = p.render.width;
    in.input_height = p.render.height;
    const XefgMatrices m = XefgCameraMatrices(p);
    std::memcpy(in.view, m.view, sizeof(in.view));
    std::memcpy(in.projection, m.projection, sizeof(in.projection));
    in.jitter[0] = p.jitter.x;
    in.jitter[1] = p.jitter.y;
    in.motion_scale[0] = p.motion_scale.x;
    in.motion_scale[1] = p.motion_scale.y;
    in.reset = p.reset;
    in.frame_ms = p.frame_ms;
    in.near_plane = p.near_plane;
    in.fov_y = p.fov_y;
    for (int i = 0; i < 3; ++i) {
        in.position[i] = p.position[i];
        in.up[i] = p.up[i];
        in.right[i] = p.right[i];
        in.forward[i] = p.forward[i];
    }
    renderer_->SetFrameGenInputsD3D12(in);
}
#endif

void SceneRenderer::RecordUpscaleResolve(gpu::CommandList& cmd, bool upscaled) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    cmd.UseTargets({{&upscaled_, gpu::Layout::ShaderReadOnly}});
    if (!upscaled && upscale_stats_.error.empty()) {
        upscale_stats_.error = "dispatch failed, see pt.log";
    }
    up_.reset = false;
    cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly},
                     {&depth_, gpu::Layout::DepthReadOnly},
                     {&object_velocity_, gpu::Layout::ShaderReadOnly},
                     {&post_hdr_, gpu::Layout::ColorAttachment},
                     {&post_depth_, gpu::Layout::ColorAttachment},
                     {&post_object_velocity_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(output_extent_, {{&post_hdr_, false, {}}, {&post_depth_, false, {}}, {&post_object_velocity_, false, {}}});
    gpu::PassPush push;
    const bool warp = up_.warp_test && up_.warp_history && !up_.reset;
    push.ids = glm::uvec4(upscaled ? 1u : (warp ? 2u : 0u), gpu::kImgObjectVelocity,
                          reflect_post_upscale_ ? gpu::kImgReflectHistoryA + reflect_history_index_ : 0u, main_view_.index);
    push.f0 = glm::vec4(up_.jitter_pixels.x / static_cast<float>(extent_.width), up_.jitter_pixels.y / static_cast<float>(extent_.height),
                        1.0f / static_cast<float>(output_extent_.width), 1.0f / static_cast<float>(output_extent_.height));
    push.f1 = glm::vec4(static_cast<float>(extent_.width), static_cast<float>(extent_.height), upscaled && handy_demod_ ? 1.0f : 0.0f,
                        rt_reflections_ && renderer_->OutputMode() != RendererOutputMode::Sdr ? 1.0f : 0.0f);
    Fullscreen(cmd, up_resolve_, push);
    cmd.EndPass();
    cmd.UseTargets({{&post_hdr_, gpu::Layout::ShaderReadOnly},
                     {&post_depth_, gpu::Layout::ShaderReadOnly},
                     {&post_object_velocity_, gpu::Layout::ShaderReadOnly}});
    if (up_.warp_test) {
        cmd.UseTargets({{&hdr_, gpu::Layout::TransferSrc}, {&upscaled_, gpu::Layout::TransferDst}});
        gpu::ImageCopy copy;
        copy.src = {gpu::Aspect::Color, 0, 0, 1};
        copy.dst = copy.src;
        copy.extent = {extent_.width, extent_.height, 1};
        cmd.CopyImage(hdr_.image, gpu::Layout::TransferSrc, upscaled_.image, gpu::Layout::TransferDst, {&copy, 1});
        cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}, {&upscaled_, gpu::Layout::ShaderReadOnly}});
        up_.warp_history = true;
    }
    cmd.WriteTimestamp(slot.upscale_queries, 3);
    slot.upscale_timed = true;
}

void SceneRenderer::RecordUpscale(gpu::CommandList& cmd, float dt) {
    RecordUpscalePrepare(dt);
    const bool upscaled = RecordUpscaleDispatch();
    cmd.BeginLabel("upscale resolve");
    RecordUpscaleResolve(cmd, upscaled);
    cmd.EndLabel();
}

void SceneRenderer::ReadUpscaleTimestamps(FrameSlot& slot) {
    if (slot.neural_timed) {
        slot.neural_timed = false;
        uint64_t stamps[4] = {};
        if (gpu_->ReadTimestamps(slot.neural_queries, 0, 4, stamps)) {
            const double ms = gpu_->Info().timestamp_period_ns * 1.0e-6;
            LogInfo("native nr gpu: frame {} encode {:.4f} ms, inference {:.4f} ms, resolve {:.4f} ms, total {:.4f} ms",
                    slot.neural_frame, static_cast<double>(stamps[1] - stamps[0]) * ms,
                    static_cast<double>(stamps[2] - stamps[1]) * ms, static_cast<double>(stamps[3] - stamps[2]) * ms,
                    static_cast<double>(stamps[3] - stamps[0]) * ms);
        }
    }
    if (!slot.upscale_timed) {
        return;
    }
    slot.upscale_timed = false;
    uint64_t stamps[4] = {};
    if (!gpu_->ReadTimestamps(slot.upscale_queries, 0, 4, stamps)) {
        return;
    }
    const double period = gpu_->Info().timestamp_period_ns * 1.0e-6;
    upscale_stats_.inputs_ms = static_cast<float>(static_cast<double>(stamps[1] - stamps[0]) * period);
    upscale_stats_.upscale_ms = static_cast<float>(static_cast<double>(stamps[3] - stamps[2]) * period);
    if (frame_counter_ > 8) {
        inputs_ms_sum_ += upscale_stats_.inputs_ms;
        upscale_ms_sum_ += upscale_stats_.upscale_ms;
        ++upscale_samples_;
    }
}

}
