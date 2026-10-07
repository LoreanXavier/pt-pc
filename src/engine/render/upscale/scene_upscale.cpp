#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "engine/core/log.h"
#include "engine/render/scene_lighting.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/upscale/frame_generation.h"
#include "engine/render/upscale/streamline.h"

namespace pt {
namespace {

constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kReactiveFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kUpscaleColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kPostDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kExposureFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kHandyFactorFormat = VK_FORMAT_R16_SFLOAT;
constexpr float kReactiveScale = 3.0f;
constexpr float kFlashlightReactiveGain = 1.5f;
constexpr float kFlashlightReactiveCap = 0.3f;
constexpr float kHandyDemodFloor = 0.02f;

UpscaleImage Wrap(const RenderTarget& t) {
    UpscaleImage image;
    image.image = t.image.image;
    image.view = t.image.view;
    image.format = t.image.format;
    image.extent = t.Extent();
    image.layout = t.layout;
    image.usage = t.image.usage;
    return image;
}

const vk::Image* PresentFrameGeneration(uint32_t image_index) {
    FrameGeneration* fsr = UpscaleHost::Get().FrameGen();
    const vk::Image* hud = fsr ? fsr->Present(image_index) : nullptr;
    if (!hud) {
        if (FrameGeneration* dlss = UpscaleHost::Get().DlssFrameGenImpl()) {
            hud = dlss->Present(image_index);
        }
    }
    return hud;
}

void UpdateDlssFrameGeneration(Renderer& renderer, bool wanted) {
    FrameGeneration* fg = UpscaleHost::Get().DlssFrameGenImpl();
    if (!fg) {
        return;
    }
    if (!renderer.hudless) {
        renderer.hudless = PresentFrameGeneration;
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
        renderer.hudless = PresentFrameGeneration;
    }
    if (fg->Update(wanted)) {
        renderer.Resize(0, 0);
    }
}

}

bool SceneRenderer::UpscalerAvailable(UpscalerKind kind, std::string& reason) {
    return UpscaleHost::Get().Available(kind, reason, false);
}

bool SceneRenderer::CreateUpscalePipelines() {
    PipelineDesc motion;
    motion.layout = layout_;
    motion.fragment = "upscale_motion.frag";
    motion.colors = {kMotionFormat};
    motion.depth = kDepthTargetFormat;
    up_motion_ = CreateGraphicsPipeline(device_, motion);

    PipelineDesc object;
    object.layout = layout_;
    object.vertex = "upscale_velocity.vert";
    object.fragment = "upscale_velocity.frag";
    object.mesh_input = true;
    object.colors = {kMotionFormat};
    object.depth = kDepthTargetFormat;
    object.depth_test = true;
    up_object_motion_ = CreateGraphicsPipeline(device_, object);

    PipelineDesc reactive;
    reactive.layout = layout_;
    reactive.fragment = "upscale_reactive.frag";
    reactive.colors = {kReactiveFormat};
    up_reactive_ = CreateGraphicsPipeline(device_, reactive);

    PipelineDesc resolve;
    resolve.layout = layout_;
    resolve.fragment = "upscale_resolve.frag";
    resolve.colors = {kUpscaleColorFormat, kPostDepthFormat, kUpscaleColorFormat};
    up_resolve_ = CreateGraphicsPipeline(device_, resolve);

    PipelineDesc demod;
    demod.layout = layout_;
    demod.fragment = "upscale_demod.frag";
    demod.colors = {kHdrTargetFormat, kHandyFactorFormat};
    up_demod_ = CreateGraphicsPipeline(device_, demod);

    if (!up_motion_ || !up_object_motion_ || !up_reactive_ || !up_resolve_ || !up_demod_) {
        LogError("scene renderer: upscale pipeline creation failed");
        return false;
    }
    return true;
}

void SceneRenderer::DestroyUpscalePipelines() {
    VkPipeline* all[] = {&up_motion_, &up_object_motion_, &up_reactive_, &up_resolve_, &up_demod_};
    for (VkPipeline* p : all) {
        if (*p) {
            vkDestroyPipeline(device_, *p, nullptr);
            *p = VK_NULL_HANDLE;
        }
    }
}

bool SceneRenderer::CreateUpscaleTargets(VkExtent2D render, VkExtent2D output) {
    const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageAspectFlags c = VK_IMAGE_ASPECT_COLOR_BIT;
    const bool ok = CreateTarget(motion_, kMotionFormat, render, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, c) && CreateTarget(reactive_, kReactiveFormat, render, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, c) &&
                    CreateTarget(opaque_, kHdrTargetFormat, render, color | VK_IMAGE_USAGE_TRANSFER_DST_BIT, c) &&
                    CreateTarget(handy_factor_, kHandyFactorFormat, render, color, c) &&
                    CreateTarget(exposure_image_, kExposureFormat, {1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, c) &&
                    CreateTarget(upscaled_, kUpscaleColorFormat, output,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, c) &&
                    CreateTarget(post_hdr_, kUpscaleColorFormat, output, color, c) && CreateTarget(post_depth_, kPostDepthFormat, output, color, c) &&
                    CreateTarget(post_object_velocity_, kUpscaleColorFormat, output, color, c);
    if (!ok) {
        LogError("scene renderer: cannot create upscale targets {}x{} -> {}x{}", render.width, render.height, output.width, output.height);
    }
    return ok;
}

void SceneRenderer::DestroyUpscaleTargets() {
    vk::Context& ctx = renderer_->Context();
    RenderTarget* targets[] = {&motion_, &reactive_, &opaque_, &handy_factor_, &exposure_image_, &upscaled_, &post_hdr_, &post_depth_, &post_object_velocity_};
    for (RenderTarget* t : targets) {
        ctx.DestroyImage(t->image);
        *t = RenderTarget{};
    }
    targets_upscaled_ = false;
}

void SceneRenderer::WriteUpscaleDescriptors(VkDescriptorImageInfo* images, bool post) {
    auto set = [&](uint32_t slot, const RenderTarget& t) {
        if (t.Valid()) {
            images[slot].imageView = t.image.view;
            images[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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

bool SceneRenderer::BeginUpscaleFrame(VkExtent2D output) {
    UpscaleHost::Get().FrameTick();
    const bool active = SetupUpscaler(output);
    const bool generate = active && up_.backend != nullptr;
    if (streamline::Active()) {
        UpdateDlssFrameGeneration(*renderer_, upscale.frame_generation == FrameGenKind::Dlss && generate);
    } else {
        UpdateFrameGeneration(*renderer_, upscale.frame_generation == FrameGenKind::Fsr && generate);
    }
    return active;
}

bool SceneRenderer::SetupUpscaler(VkExtent2D output) {
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
    up_.enabled = false;
    upscale_stats_.active = false;
    upscale_stats_.kind = upscale.kind;
    const bool lit = debug_view == DebugView::Lit || debug_view == DebugView::Ambient;
    const UpscalerKind kind = upscale.kind;
    if (kind == UpscalerKind::Off || !lit || output.width == 0 || output.height == 0) {
        if (up_.created && up_.backend) {
            vkDeviceWaitIdle(device_);
            up_.backend->Release();
        }
        up_.created = false;
        up_.backend = nullptr;
        up_.kind = UpscalerKind::Off;
        up_.failed_key.clear();
        upscale_stats_.error.clear();
        return false;
    }
    std::string reason;
    if (!UpscaleHost::Get().Available(kind, reason)) {
        upscale_stats_.error = reason;
        return false;
    }
    const VkExtent2D render = UpscaleRenderExtent(output, upscale.quality, upscale.scale);
    UpscaleSettings key_settings = upscale;
    key_settings.sharpness = 0.0f;
    key_settings.frame_generation = FrameGenKind::Off;
    if (kind != UpscalerKind::Dlss) {
        key_settings.dlss_model = DlssModel::Auto;
    }
    const bool changed = !up_.created || up_.kind != kind || !(up_.created_settings == key_settings) || up_.created_render.width != render.width ||
                         up_.created_render.height != render.height || up_.created_output.width != output.width ||
                         up_.created_output.height != output.height;
    if (changed) {
        const std::string failed_key = std::format("{}|{}|{}x{}|{}x{}", UpscalerKey(kind), UpscaleQualityKey(upscale.quality), render.width,
                                                   render.height, output.width, output.height);
        if (failed_key == up_.failed_key) {
            return false;
        }
        vkDeviceWaitIdle(device_);
        if (up_.created && up_.backend) {
            up_.backend->Release();
        }
        up_.created = false;
        up_.backend = kind == UpscalerKind::Spatial ? nullptr : UpscaleHost::Get().Backend(kind);
        if (kind != UpscalerKind::Spatial) {
            if (!up_.backend) {
                up_.failed_key = failed_key;
                upscale_stats_.error = "backend unavailable";
                return false;
            }
            UpscaleCreate create;
            create.render = render;
            create.display = output;
            create.quality = upscale.quality;
            create.dlss_model = upscale.dlss_model;
            create.color_format = kHdrTargetFormat;
            bool created = false;
            renderer_->Context().Submit([&](VkCommandBuffer cmd) { created = up_.backend->Create(cmd, create); });
            if (!created) {
                LogError("upscale: {} could not be created for {}x{} -> {}x{}", UpscalerName(kind), render.width, render.height, output.width,
                         output.height);
                up_.failed_key = failed_key;
                upscale_stats_.error = "creation failed, see pt.log";
                up_.backend = nullptr;
                return false;
            }
        }
        LogInfo("upscale: {} {} at {}x{} -> {}x{}", UpscalerName(kind), UpscaleQualityName(upscale.quality), render.width, render.height,
                output.width, output.height);
        up_.failed_key.clear();
        up_.kind = kind;
        up_.created = true;
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

void SceneRenderer::RecordOpaqueSnapshot(VkCommandBuffer cmd, RenderTarget& output) {
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&opaque_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {extent_.width, extent_.height, 1};
    vkCmdCopyImage(cmd, output.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, opaque_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &copy);
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {&opaque_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    BeginPass(cmd, extent_, {{&output, false, {}}}, &depth_, true, false);
}

void SceneRenderer::RecordUpscaleInputs(VkCommandBuffer cmd, const ViewSetup& view) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.upscale_queries, 0);
    UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}, {&motion_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, extent_, {{&motion_, false, {}}}, &depth_, true, false);
    gpu::PassPush push;
    push.ids = glm::uvec4(0, 0, 0, view.index);
    push.m = previous_view_projection_;
    Fullscreen(cmd, up_motion_, push);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, up_object_motion_);
    for (const Draw& d : draws_) {
        const SubMesh& sub = *d.sub;
        if (d.previous_model == gpu::kInvalid || sub.pass != RenderPass::Opaque || sub.kind != gpu::kKindDeferred || !Visible(d, view)) {
            continue;
        }
        vkCmdSetCullMode(cmd, sub.double_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &d.mesh->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, d.mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        gpu::DrawPush draw;
        draw.model = d.transform;
        draw.ids = glm::uvec4(view.index, d.material, motion_view_, sub.skinned ? d.skin_base : gpu::kInvalid);
        draw.tint = d.tint;
        const uint32_t previous_skin = sub.skinned ? d.previous_skin : gpu::kInvalid;
        draw.aux[0] = glm::vec4(std::bit_cast<float>(d.previous_model), std::bit_cast<float>(previous_skin), std::bit_cast<float>(post_view_), 0.0f);
        PushConstants(cmd, &draw, sizeof(draw));
        vkCmdDrawIndexed(cmd, sub.index_count, 1, sub.first_index, sub.vertex_offset, 0);
    }
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&motion_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&opaque_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&reactive_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, extent_, {{&reactive_, false, {}}});
    push = gpu::PassPush{};
    push.ids = glm::uvec4(0, 0, 0, view.index);
    static const glm::vec2 flashlight_reactive = [] {
        glm::vec2 v(kFlashlightReactiveGain, kFlashlightReactiveCap);
        if (const char* e = std::getenv("PT_FLASHLIGHT_REACTIVE")) {
            std::sscanf(e, "%f,%f", &v.x, &v.y);
        }
        return v;
    }();
    push.f0 = glm::vec4(kReactiveScale, 0.9f, flashlight_reactive.x, flashlight_reactive.y);
    static const bool reactive_all = std::getenv("PT_REACTIVE_ALL") != nullptr;
    push.ids.x = reactive_all ? 1u : 0u;
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
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&reactive_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});

    static const bool demod_on = [] {
        const char* e = std::getenv("PT_HANDY_DEMOD");
        return !e || std::atoi(e) != 0;
    }();
    static const float demod_floor = [] {
        const char* e = std::getenv("PT_HANDY_DEMOD_FLOOR");
        return e ? static_cast<float>(std::atof(e)) : kHandyDemodFloor;
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
    handy_demod_ = demod_on && handy != gpu::kInvalid && handy_factor_.Valid();
    if (handy_demod_) {
        UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&diffuse_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&opaque_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                         {&handy_factor_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, extent_, {{&opaque_, false, {}}, {&handy_factor_, false, {}}});
        gpu::PassPush demod;
        demod.ids = glm::uvec4(view.index, handy, handy_shadow ? 1u : 0u, 0u);
        demod.f0 = glm::vec4(demod_floor, 0.0f, 0.0f, 0.0f);
        Fullscreen(cmd, up_demod_, demod);
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{&opaque_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&handy_factor_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.upscale_queries, 1);
}

void SceneRenderer::RecordUpscale(VkCommandBuffer cmd, float dt) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.upscale_queries, 2);
    UseTargets(cmd, {{&exposure_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
    VkClearColorValue value{};
    value.float32[0] = exposure_;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, exposure_image_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    UseTargets(cmd, {{&exposure_image_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    bool upscaled = false;
    if (up_.backend) {
        UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&motion_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&reactive_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&upscaled_, VK_IMAGE_LAYOUT_GENERAL}});
        UpscaleDispatch d;
        d.cmd = cmd;
        d.color = Wrap(handy_demod_ ? opaque_ : hdr_);
        d.depth = Wrap(depth_);
        d.motion = Wrap(motion_);
        d.reactive = Wrap(reactive_);
        d.exposure = Wrap(exposure_image_);
        d.output = Wrap(upscaled_);
        d.render = extent_;
        d.display = output_extent_;
        d.jitter = up_.jitter_pixels;
        d.motion_scale = glm::vec2(static_cast<float>(extent_.width), static_cast<float>(extent_.height));
        d.pre_exposure = std::max(exposure_, 1.0e-12f);
        d.sharpness = upscale.sharpness;
        d.frame_ms = std::clamp(dt, 1.0e-4f, 0.25f) * 1000.0f;
        d.near_plane = camera_.near_plane;
        d.fov_y = camera_.fov_y;
        d.reset = up_.reset;
        d.frame_index = frame_counter_;
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
        upscaled = up_.backend->Dispatch(d);
        FrameGeneration* fg = streamline::Active() ? UpscaleHost::Get().DlssFrameGenImpl() : UpscaleHost::Get().FrameGen();
        if (upscaled && fg && fg->Generating()) {
            FrameGenPrepare prepare;
            prepare.cmd = cmd;
            prepare.depth = d.depth;
            prepare.motion = d.motion;
            prepare.render = extent_;
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
            fg->Prepare(prepare);
        }
        UseTargets(cmd, {{&upscaled_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        if (!upscaled && upscale_stats_.error.empty()) {
            upscale_stats_.error = "dispatch failed, see pt.log";
        }
    }
    up_.reset = false;
    UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                     {&object_velocity_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&post_hdr_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&post_depth_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&post_object_velocity_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, output_extent_, {{&post_hdr_, false, {}}, {&post_depth_, false, {}}, {&post_object_velocity_, false, {}}});
    gpu::PassPush push;
    const bool warp = up_.warp_test && up_.warp_history && !up_.reset;
    push.ids = glm::uvec4(upscaled ? 1u : (warp ? 2u : 0u), gpu::kImgObjectVelocity,
                          reflect_post_upscale_ ? gpu::kImgReflectHistoryA + reflect_history_index_ : 0u, main_view_.index);
    push.f0 = glm::vec4(up_.jitter_pixels.x / static_cast<float>(extent_.width), up_.jitter_pixels.y / static_cast<float>(extent_.height),
                        1.0f / static_cast<float>(output_extent_.width), 1.0f / static_cast<float>(output_extent_.height));
    push.f1 = glm::vec4(static_cast<float>(extent_.width), static_cast<float>(extent_.height), upscaled && handy_demod_ ? 1.0f : 0.0f, 0.0f);
    Fullscreen(cmd, up_resolve_, push);
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&post_hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&post_depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&post_object_velocity_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    if (up_.warp_test) {
        UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&upscaled_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {extent_.width, extent_.height, 1};
        vkCmdCopyImage(cmd, hdr_.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, upscaled_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &copy);
        UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&upscaled_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        up_.warp_history = true;
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.upscale_queries, 3);
    slot.upscale_timed = true;
}

void SceneRenderer::ReadUpscaleTimestamps(FrameSlot& slot) {
    if (!slot.upscale_timed) {
        return;
    }
    slot.upscale_timed = false;
    uint64_t stamps[4] = {};
    if (vkGetQueryPoolResults(device_, slot.upscale_queries, 0, 4, sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    const double period = renderer_->Context().properties.limits.timestampPeriod * 1.0e-6;
    upscale_stats_.inputs_ms = static_cast<float>(static_cast<double>(stamps[1] - stamps[0]) * period);
    upscale_stats_.upscale_ms = static_cast<float>(static_cast<double>(stamps[3] - stamps[2]) * period);
    if (frame_counter_ > 8) {
        inputs_ms_sum_ += upscale_stats_.inputs_ms;
        upscale_ms_sum_ += upscale_stats_.upscale_ms;
        ++upscale_samples_;
    }
}

}
