#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "engine/render/scene_renderer.h"

namespace pt {
namespace {

glm::vec4 BloomPolynomial(float extraction) {
    glm::vec4 p(0.0f);
    const float e = std::clamp(extraction, 1.0f, 3.99999f);
    const int i = static_cast<int>(std::floor(e));
    const float f = e - static_cast<float>(i);
    p[i - 1] = 1.0f - f;
    p[i] = f;
    return p;
}

}

void SceneRenderer::CopyToHistory(VkCommandBuffer cmd, const RenderTarget& source) {
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {output_extent_.width, output_extent_.height, 1};
    const VkImageLayout src = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdCopyImage(cmd, source.image.image, src, history_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

void SceneRenderer::RecordReflectionSample(VkCommandBuffer cmd, const SceneLighting& lighting) {
    const SceneReflectionSample& sample = lighting.reflection_sample;
    if (!sample.active || !reflect_colour_ || !hdr_.Valid()) {
        return;
    }
    const glm::mat4 view_projection = main_view_.projection * main_view_.view;
    glm::vec2 uv[4];
    for (int i = 0; i < 4; ++i) {
        const glm::vec4 clip = view_projection * glm::vec4(sample.points[i], 1.0f);
        const float w = std::abs(clip.w) > 1.0e-6f ? clip.w : std::copysign(1.0e-6f, clip.w);
        uv[i] = glm::vec2(clip.x, clip.y) / w * 0.5f + 0.5f;
    }
    UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reflect_colour_);
    BindSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    gpu::PassPush push;
    push.ids = glm::uvec4(gpu::kImgHdr, gpu::kReflectionReadback, 0, 0);
    push.f0 = glm::vec4(uv[0], uv[1]);
    push.f1 = glm::vec4(uv[2], uv[3]);
    PushConstants(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    slots_[renderer_->FrameIndex()].reflection_sampled = true;
}

void SceneRenderer::RecordPost(VkCommandBuffer cmd, const SceneLighting& lighting, float dt) {
    const ScreenSettings& screen = lighting.screen;
    const ExposureSettings& exposure = lighting.exposure;
    gpu::PassPush push;
    push.ids.w = post_view_;
    const bool hdr_output = renderer_->OutputMode() != RendererOutputMode::Sdr;
    push.f1.x = hdr_output ? 1.0f : 0.0f;

    const char* native_order_override = std::getenv("PT_AA_NATIVE_ORDER");
    const bool native_aa_order = toggles.fxaa && !up_.enabled && native_order_override && std::string(native_order_override) == "1";
    if (native_aa_order) {
        UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&hdr_copy_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, output_extent_, {{&hdr_copy_, false, {}}});
        gpu::PassPush aa;
        aa.ids = glm::uvec4(gpu::kImgHdr, 1, 0, post_view_);
        Fullscreen(cmd, fxaa_, aa);
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{&hdr_copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&hdr_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, output_extent_, {{&hdr_, false, {}}});
        aa.ids = glm::uvec4(gpu::kImgHdrCopy, 2, 0, post_view_);
        Fullscreen(cmd, fxaa_, aa);
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    }

    const bool bloom = toggles.bloom && lighting.valid && std::abs(exposure.bloom_size) >= 1.0e-5f;
    if (bloom) {
        UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&bloom_[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, bloom_[0].Extent(), {{&bloom_[0], false, {}}});
        push.f0 = BloomPolynomial(exposure.bloom_extraction);
        push.f1 = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        Fullscreen(cmd, bright_, push);
        vkCmdEndRendering(cmd);
        static const uint32_t kSlots[3] = {gpu::kImgBloomA, gpu::kImgBloomB, gpu::kImgBloomSum};
        auto blur = [&](int src, int dst, VkPipeline pipeline, const glm::vec4& f0, bool clear) {
            UseTargets(cmd, {{&bloom_[src], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&bloom_[dst], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
            BeginPass(cmd, bloom_[dst].Extent(), {{&bloom_[dst], clear, {}}});
            push.ids.x = kSlots[src];
            push.f0 = f0;
            Fullscreen(cmd, pipeline, push);
            vkCmdEndRendering(cmd);
        };
        const float size = exposure.bloom_size;
        const float weight = exposure.bloom_weight;
        if (size >= 0.1f && size < 0.2f) {
            blur(0, 1, gaussian_, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), false);
            blur(1, 2, gaussian_, glm::vec4(0.0f, 1.0f, 0.0f, 0.0f), false);
        } else {
            const int passes = std::clamp(static_cast<int>(std::floor(size * 10.0f)), 1, 64);
            static const bool separate_add = std::getenv("PT_BLOOM_SEPARATE_ADD") != nullptr;
            const glm::vec4 saved_f1 = push.f1;
            for (int i = 0; i < passes; ++i) {
                const glm::vec4 kawase(static_cast<float>(i) + 0.5f, i == 0 ? weight : 1.0f, 1.0f, 0.0f);
                blur(0, 1, kawase_, kawase, false);
                if (separate_add) {
                    blur(1, 0, kawase_, kawase, false);
                    blur(0, 2, bloom_add_, glm::vec4(0.0f, weight / static_cast<float>(passes), 0.0f, 0.0f), i == 0);
                    continue;
                }
                UseTargets(cmd, {{&bloom_[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                 {&bloom_[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                 {&bloom_[2], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
                BeginPass(cmd, bloom_[0].Extent(), {{&bloom_[0], false, {}}, {&bloom_[2], i == 0, {}}});
                push.ids.x = kSlots[1];
                push.f0 = kawase;
                push.f1 = glm::vec4(weight / static_cast<float>(passes), 0.0f, 0.0f, 0.0f);
                Fullscreen(cmd, kawase_sum_, push);
                vkCmdEndRendering(cmd);
            }
            push.f1 = saved_f1;
        }
        UseTargets(cmd, {{&bloom_[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&bloom_[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    }
    Stamp(cmd, 11);

    UseTargets(cmd, {{&flare_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    BeginPass(cmd, output_extent_, {{&flare_, true, {}}});
    if (vfx_filter && vr_eye_ < 0) {
        vfx_filter(FilterContext(cmd, 1u));
        BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&flare_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});

    int current = 0;
    UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&ldr_[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, output_extent_, {{&ldr_[0], false, {}}});
    push.ids.x = 0;
    const float lut_blend = std::clamp(screen.lut_blend, 0.0f, 1.0f);
    push.f0 = glm::vec4(bloom ? 1.0f : 0.0f, lut_blend, lighting.valid ? 1.0f : renderer_->exposure, 0.0f);
    static const bool tonemap_test = std::getenv("PT_TONEMAP_TEST") != nullptr;
    if (tonemap_test) {
        push.f0.x = 0.0f;
        push.f0.y = 0.0f;
        push.f0.w = 0.0f;
    }
    Fullscreen(cmd, tonemap_, push);
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&ldr_[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});

    auto ldr_index = [](int i) { return i == 0 ? gpu::kImgLdrA : gpu::kImgLdrB; };
    auto run = [&](VkPipeline pipeline, const gpu::PassPush& p) {
        RenderTarget& dst = ldr_[1 - current];
        UseTargets(cmd, {{&ldr_[current], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, output_extent_, {{&dst, false, {}}});
        Fullscreen(cmd, pipeline, p);
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{&dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        current = 1 - current;
    };

    if (toggles.fxaa && !up_.enabled && !native_aa_order) {
        push.ids.x = ldr_index(current);
        run(fxaa_, push);
    }
    if (toggles.depth_of_field && screen.depth_of_field && lighting.valid) {
        RecordDepthOfField(cmd, screen, current);
    }
    if (toggles.motion_blur && screen.motion_blur && lighting.valid) {
        RecordMotionBlur(cmd, screen, current);
    }
    if (toggles.color_lut) {
        gpu::PassPush lut;
        lut.ids = glm::uvec4(ldr_index(current), 1, 0, post_view_);
        lut.f0 = glm::vec4(0.0f, lut_blend, 1.0f, 1.0f);
        run(tonemap_, lut);
    }

    const bool blur = toggles.full_screen_blur && screen.full_screen_blur && lighting.valid;
    if (blur) {
        if (!history_valid_) {
            UseTargets(cmd, {{&ldr_[current], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&history_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
            CopyToHistory(cmd, ldr_[current]);
            UseTargets(cmd, {{&ldr_[current], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        }
        UseTargets(cmd, {{&history_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        push.ids.x = ldr_index(current);
        push.ids.y = gpu::kImgHistory;
        const float share = std::clamp(dt * 30.0f, 0.05f, 2.0f);
        const float rate = std::pow(std::clamp(screen.blur_blend_rate, 0.0f, 0.999f), share);
        push.f0 = glm::vec4(screen.blur_fetch_band * std::sqrt(share), rate, 0.0f, 0.0f);
        run(fsblur_, push);
        UseTargets(cmd, {{&ldr_[current], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&history_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
        CopyToHistory(cmd, ldr_[current]);
        UseTargets(cmd, {{&ldr_[current], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&history_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        history_valid_ = true;
    } else {
        history_valid_ = false;
    }
    if (screen.colour_banding_canceller && lighting.valid) {
        UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
        push.ids = glm::uvec4(ldr_index(current), 0, 0, main_view_.index);
        run(banding_, push);
    }

    RenderTarget output;
    output.image = renderer_->SceneColor();
    output.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    BeginPass(cmd, output_extent_, {{&output, false, {}}});
    push.ids = glm::uvec4(ldr_index(current), 0, 0, post_view_);
    const bool grain = toggles.film_grain && screen.film_grain && lighting.valid;
    const bool distortion = toggles.distortion && screen.screen_distortion && lighting.valid;
    push.f0 = glm::vec4(distortion ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    push.f1 = glm::vec4(graphics.clarity, hdr_output ? 1.0f : 0.0f, 0.0f, 0.0f);
    Fullscreen(cmd, screen_fx_, push);
    renderer_->grain[0] = grain ? 1.0f : 0.0f;
    renderer_->grain[1] = screen.grain_alt ? 1.0f : 0.0f;
    renderer_->grain[2] = screen.grain_strength * graphics.film_grain;
    renderer_->grain_offset[0] = screen.grain_offset.x;
    renderer_->grain_offset[1] = screen.grain_offset.y;
    if (vfx_filter && vr_eye_ < 0) {
        SceneFilterContext context = FilterContext(cmd, 2u);
        context.color_format = renderer_->SceneColorFormat();
        vfx_filter(context);
        BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }
    vkCmdEndRendering(cmd);
    (void)dt;
}

SceneFilterContext SceneRenderer::FilterContext(VkCommandBuffer cmd, uint32_t layers) const {
    SceneFilterContext context;
    context.cmd = cmd;
    context.extent = output_extent_;
    context.color_format = kPostTargetFormat;
    context.source = ldr_[0].image.view;
    context.depth_view = depth_.image.view;
    context.exposure = exposure_;
    context.frame_index = renderer_->FrameIndex();
    context.layers = layers;
    return context;
}

void SceneRenderer::PostPass(VkCommandBuffer cmd, RenderTarget& target, VkPipeline pipeline, const gpu::PassPush& push) {
    UseTargets(cmd, {{&target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, target.Extent(), {{&target, false, {}}});
    Fullscreen(cmd, pipeline, push);
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
}

bool SceneRenderer::RecordDepthOfField(VkCommandBuffer cmd, const ScreenSettings& screen, int& current) {
    const float aperture = screen.aperture;
    if ((screen.dof_flags & 2u) == 0 && (aperture == 0.0f || !(aperture < 100.0f))) {
        return false;
    }
    const float f = screen.focal_length * 0.001f;
    const float d = std::max(screen.focus_distance, 1.0e-5f);
    const float s = std::max(0.0f, f * f / aperture / std::max(d - f, 1.0e-5f)) * 26666.666f;
    const float near_limit = std::min((d - d * 0.01f) / (d * 0.01f) * s, 20.0f);
    const float near_scale = s / near_limit < 1.0f ? s / near_limit : 1.0f;
    const uint32_t scene = current == 0 ? gpu::kImgLdrA : gpu::kImgLdrB;
    auto rt = [](const RenderTarget& t) {
        const VkExtent2D e = t.Extent();
        return glm::vec4(e.width, e.height, 1.0f / e.width, 1.0f / e.height);
    };
    gpu::PassPush push;
    push.ids = glm::uvec4(0, 0, 0, post_view_);
    push.f0 = glm::vec4(d, near_limit, near_scale, 1.0f);
    UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    PostPass(cmd, ldr_[current], dof_ratio_, push);

    push = gpu::PassPush{};
    push.ids = glm::uvec4(scene, 0, 0, post_view_);
    push.f2 = rt(dof_half_);
    PostPass(cmd, dof_half_, dof_down_, push);
    push.ids = glm::uvec4(gpu::kImgDofHalf, 0, 1, post_view_);
    push.f2 = rt(dof_quarter_[0]);
    PostPass(cmd, dof_quarter_[0], dof_down_, push);
    push.ids = glm::uvec4(gpu::kImgDofQuarterA, 0, 1, post_view_);
    push.f2 = rt(dof_eighth_[0]);
    PostPass(cmd, dof_eighth_[0], dof_down_, push);

    const float far_size = std::max(4.0f, std::min(s, 10.0f));
    const float far_span = std::max(0.0001f, far_size - 4.0f);
    push.f0 = glm::vec4(rt(dof_quarter_[0]).z, rt(dof_quarter_[0]).w, 0.0f, 0.0f);
    push.f1 = glm::vec4(far_size * 0.125f, 4.0f / far_size, std::max(1.0f, far_size / far_span), 0.0f);
    push.f2 = rt(dof_quarter_[0]);
    push.ids = glm::uvec4(gpu::kImgDofQuarterA, scene, 0, post_view_);
    PostPass(cmd, dof_quarter_[1], dof_blur_, push);
    push.ids = glm::uvec4(gpu::kImgDofQuarterB, scene, 0, post_view_);
    PostPass(cmd, dof_quarter_[0], dof_blur_, push);

    if ((screen.dof_flags & 1u) == 0) {
        const float near_span = std::max(0.0001f, near_limit - 8.0f);
        push.f0 = glm::vec4(rt(dof_eighth_[0]).z, rt(dof_eighth_[0]).w, 0.0f, 0.0f);
        push.f1 = glm::vec4(1.0f, 8.0f / near_limit, std::max(1.0f, near_limit / near_span), 0.0f);
        push.f2 = rt(dof_eighth_[0]);
        push.ids = glm::uvec4(gpu::kImgDofEighthA, scene, 1, post_view_);
        PostPass(cmd, dof_eighth_[1], dof_blur_, push);
        push.ids = glm::uvec4(gpu::kImgDofEighthB, scene, 1, post_view_);
        PostPass(cmd, dof_eighth_[0], dof_blur_, push);
        push = gpu::PassPush{};
        push.ids = glm::uvec4(gpu::kImgDofEighthA, 0, 2, post_view_);
        push.f2 = rt(dof_quarter_[1]);
        PostPass(cmd, dof_quarter_[1], dof_down_, push);
    }

    const float blend_size = std::min(s, 10.0f);
    const float near_amount = std::min(near_limit * 0.125f, 1.0f);
    push = gpu::PassPush{};
    push.ids = glm::uvec4(scene, gpu::kImgDofQuarterB, gpu::kImgDofQuarterA, post_view_);
    push.f0 = glm::vec4(blend_size > 4.0f ? blend_size * 0.25f : 1.0f, (screen.dof_flags & 1u) ? 0.0f : near_limit * 0.125f, s < 4.0f ? s * 0.25f : 1.0f,
                        near_limit >= 8.0f ? 1.0f : near_amount);
    push.f2 = rt(ldr_[1 - current]);
    PostPass(cmd, ldr_[1 - current], dof_blend_, push);
    current = 1 - current;
    return true;
}

void SceneRenderer::SetMotionReference(const Camera& previous, float interval, bool history) {
    motion_reference_ = previous;
    motion_interval_ = interval;
    motion_reference_history_ = history;
    motion_reference_set_ = true;
}

bool SceneRenderer::MotionBlurActive() const {
    const ScreenSettings& screen = lighting_->screen;
    return toggles.motion_blur && screen.motion_blur && lighting_->valid && screen.shutter_speed >= 0.001f &&
           (debug_view == DebugView::Lit || debug_view == DebugView::Ambient);
}

void SceneRenderer::UpdateMotion(const Camera& camera, float dt) {
    Camera previous = camera;
    float interval = dt;
    motion_history_valid_ = has_last_camera_;
    const bool reference = motion_reference_set_;
    if (reference) {
        previous = motion_reference_;
        interval = motion_interval_;
        motion_history_valid_ = motion_reference_history_;
    } else if (has_last_camera_) {
        previous = last_camera_;
    }
    motion_reference_set_ = false;
    last_camera_ = camera;
    has_last_camera_ = true;
    if (glm::distance(previous.position, camera.position) > kCameraCutDistance || glm::dot(previous.Forward(), camera.Forward()) < 0.7071f) {
        previous = camera;
        up_.reset = true;
    }
    if (!motion_history_valid_) {
        up_.reset = true;
    }
    const float aspect = static_cast<float>(output_extent_.width) / static_cast<float>(std::max(1u, output_extent_.height));
    previous_view_projection_ = previous.Projection(aspect) * previous.View();
    const float step = std::clamp(interval, 1.0e-4f, 1.0f);
    frame_time_ = reference ? static_cast<double>(step) : static_cast<double>(step * 0.016666668f) + frame_time_ * 0.9833333333333333;
}

bool SceneRenderer::RecordMotionBlur(VkCommandBuffer cmd, const ScreenSettings& screen, int& current) {
    const float shutter = screen.shutter_speed;
    if (!(shutter >= 0.001f)) {
        return false;
    }
    const float ratio = screen.fixed_shutter ? shutter * 60.0f : shutter * static_cast<float>(1.0 / frame_time_);
    const float scale = ratio * 0.5f;
    gpu::PassPush push;
    push.ids = glm::uvec4(gpu::kImgObjectVelocity, 0, 0, post_view_);
    push.f0 = glm::vec4(1920.0f / 128.0f, 1080.0f / 128.0f, 0.0f, 0.0f);
    push.m = previous_view_projection_;
    UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    PostPass(cmd, velocity_, mb_velocity_, push);
    push = gpu::PassPush{};
    for (uint32_t i = 0; i < 5; ++i) {
        const VkExtent2D e = mb_tile_[i].Extent();
        push.ids = glm::uvec4(i == 0 ? static_cast<uint32_t>(gpu::kImgVelocity) : gpu::kImgMbTile + i - 1, 0, 0, post_view_);
        push.f0 = glm::vec4(1.0f / e.width, 1.0f / e.height, 0.0f, 0.0f);
        PostPass(cmd, mb_tile_[i], mb_tile_pipeline_, push);
    }
    push.ids = glm::uvec4(gpu::kImgMbTile + 4, 0, 1, post_view_);
    PostPass(cmd, mb_neighbour_, mb_tile_pipeline_, push);
    push.ids = glm::uvec4(gpu::kImgVelocity, gpu::kImgMbNeighbour, 0, post_view_);
    push.f0 = glm::vec4(0.5f, 0.5f, 1.0f, scale);
    PostPass(cmd, mb_bake_, mb_bake_pipeline_, push);
    push.ids = glm::uvec4(current == 0 ? gpu::kImgLdrA : gpu::kImgLdrB, gpu::kImgMbBake, 0, post_view_);
    PostPass(cmd, mb_blur_[0], mb_mcguire_, push);
    push.ids = glm::uvec4(gpu::kImgMbBlurA, gpu::kImgMbBake, 0, post_view_);
    push.f0 = glm::vec4(0.5f, 0.5f, 0.16666667f, scale);
    PostPass(cmd, mb_blur_[1], mb_mcguire_, push);
    push.ids = glm::uvec4(gpu::kImgMbBlurB, 0, 0, post_view_);
    push.f0 = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    PostPass(cmd, ldr_[current], mb_composite_, push);
    return true;
}

void SceneRenderer::RecordDebug(VkCommandBuffer cmd) {
    UseTargets(cmd, {{&albedo_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&normal_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&material_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                     {&diffuse_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&specular_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    RenderTarget output;
    output.image = renderer_->SceneColor();
    output.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, output_extent_, {{&output, false, {}}});
    gpu::PassPush push;
    push.ids = glm::uvec4(static_cast<uint32_t>(debug_view), post_view_, 0, 0);
    push.f0 = glm::vec4(4.0f, 1.0f, 0.0f, 0.0f);
    Fullscreen(cmd, debug_, push);
    vkCmdEndRendering(cmd);
}

void SceneRenderer::ApplyEnvironmentOverrides() {
    if (const char* view = std::getenv("PT_DEBUG_VIEW")) {
        const int mode = std::atoi(view);
        if (mode >= 0 && mode < static_cast<int>(DebugView::Count)) {
            debug_view = static_cast<DebugView>(mode);
        }
    }
    if (const char* exposure = std::getenv("PT_EXPOSURE")) {
        renderer_->exposure = static_cast<float>(std::atof(exposure));
    }
    if (const char* off = std::getenv("PT_RENDER_OFF")) {
        const std::string list = off;
        auto has = [&](const char* name) { return list.find(name) != std::string::npos; };
        toggles.lights = !has("lights");
        toggles.probes = !has("probes");
        toggles.shadows = !has("shadows");
        toggles.bloom = !has("bloom");
        toggles.color_lut = !has("lut");
        toggles.fxaa = !has("fxaa");
        toggles.depth_of_field = !has("dof");
        toggles.film_grain = !has("grain");
        toggles.distortion = !has("distortion");
        toggles.full_screen_blur = !has("blur");
        toggles.adaptation = !has("adaptation");
        toggles.mirrors = !has("mirrors");
        toggles.tpp_atmosphere = !has("tpp");
        toggles.occlusion = !has("occlusion");
        toggles.local_reflections = !has("reflections");
        toggles.motion_blur = !has("motionblur");
    }
    if (const char* last = std::getenv("PT_TIMING_LAST")) {
        timing_ring_.assign(static_cast<size_t>(std::clamp(std::atoi(last), 0, 100000)), FrameTiming{});
    }
    if (const char* cull = std::getenv("PT_RT_CULL")) {
        const std::string c = cull;
        rt_cull_ = c == "front" ? 0x20u : c == "none" ? 0u : 0x10u;
    }
    if (const char* rays = std::getenv("PT_RT_AO_RAYS")) {
        rt_ao_rays_ = static_cast<uint32_t>(std::clamp(std::atoi(rays), 1, 64));
    }
    if (const char* reach = std::getenv("PT_RT_AO_REACH")) {
        rt_ao_reach_ = std::clamp(static_cast<float>(std::atof(reach)), 0.05f, 10.0f);
    }
    if (const char* frames = std::getenv("PT_RT_AO_FRAMES")) {
        rt_ao_frames_ = std::clamp(static_cast<float>(std::atof(frames)), 1.0f, 256.0f);
    }
    if (const char* reach = std::getenv("PT_RT_CONTACT_REACH")) {
        rt_contact_reach_ = std::clamp(static_cast<float>(std::atof(reach)), 0.01f, 10.0f);
    }
    if (const char* legacy = std::getenv("PT_CONTACT_LEGACY")) {
        rt_contact_legacy_ = std::atoi(legacy) != 0;
    }
}

void SceneRenderer::DrawDebugUi() {
    if (ImGui::Begin("Renderer")) {
        int mode = static_cast<int>(debug_view);
        ImGui::Combo("debug view", &mode, DebugViewNames());
        debug_view = static_cast<DebugView>(mode);
        ImGui::Text("ev %.2f exposure %.5f luminance %.4f", stats_.ev, stats_.exposure, stats_.luminance);
        ImGui::Text("gpu %.2f ms cpu %.2f ms", stats_.gpu_ms, stats_.cpu_ms);
        ImGui::Text("%u draws, %u lights, %u probes, %u shadow views", stats_.draws, stats_.lights, stats_.probes, stats_.shadow_views);
        ImGui::Checkbox("lights", &toggles.lights);
        ImGui::SameLine();
        ImGui::Checkbox("probes", &toggles.probes);
        ImGui::SameLine();
        ImGui::Checkbox("shadows", &toggles.shadows);
        ImGui::SameLine();
        ImGui::Checkbox("occlusion", &toggles.occlusion);
        if (rt_) {
            ImGui::Checkbox("ray traced shadows", &raytracing.shadows);
            ImGui::SameLine();
            ImGui::Checkbox("soft", &raytracing.soft_shadows);
            ImGui::SameLine();
            ImGui::Checkbox("reflections", &raytracing.reflections);
            ImGui::SameLine();
            ImGui::Checkbox("ambient occlusion", &raytracing.ambient_occlusion);
            ImGui::SameLine();
            ImGui::Checkbox("contact shadows", &raytracing.contact_shadows);
        }
        ImGui::Checkbox("bloom", &toggles.bloom);
        ImGui::SameLine();
        ImGui::Checkbox("color lut", &toggles.color_lut);
        ImGui::SameLine();
        ImGui::Checkbox("fxaa", &toggles.fxaa);
        ImGui::SameLine();
        ImGui::Checkbox("dof", &toggles.depth_of_field);
        ImGui::Checkbox("grain", &toggles.film_grain);
        ImGui::SameLine();
        ImGui::Checkbox("distortion", &toggles.distortion);
        ImGui::SameLine();
        ImGui::Checkbox("blur", &toggles.full_screen_blur);
        ImGui::SameLine();
        ImGui::Checkbox("mirrors", &toggles.mirrors);
        ImGui::Checkbox("tpp atmosphere", &toggles.tpp_atmosphere);
        ImGui::SameLine();
        ImGui::Checkbox("local reflections", &toggles.local_reflections);
        ImGui::SameLine();
        ImGui::Checkbox("motion blur", &toggles.motion_blur);
        if (ImGui::Checkbox("eye adaptation", &toggles.adaptation) && toggles.adaptation) {
            adaptation_valid_ = false;
        }
        if (ImGui::Button("reset exposure")) {
            adaptation_valid_ = false;
        }
    }
    ImGui::End();
}

}
