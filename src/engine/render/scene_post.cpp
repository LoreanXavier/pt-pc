#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "engine/core/log.h"
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

void SceneRenderer::CopyToHistory(gpu::CommandList& cmd, const RenderTarget& source) {
    gpu::ImageCopy copy;
    copy.src = {gpu::Aspect::Color, 0, 0, 1};
    copy.dst = copy.src;
    copy.extent = {output_extent_.width, output_extent_.height, 1};
    const gpu::Layout src = gpu::Layout::TransferSrc;
    cmd.CopyImage(source.image, src, history_.image, gpu::Layout::TransferDst, {&copy, 1});
}

void SceneRenderer::RecordReflectionSample(gpu::CommandList& cmd, const SceneLighting& lighting) {
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
    cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}});
    cmd.BindPipeline(gpu::BindPoint::Compute, reflect_colour_);
    BindSets(cmd, gpu::BindPoint::Compute);
    gpu::PassPush push;
    push.ids = glm::uvec4(gpu::kImgHdr, gpu::kReflectionReadback, 0, 0);
    push.f0 = glm::vec4(uv[0], uv[1]);
    push.f1 = glm::vec4(uv[2], uv[3]);
    PushConstants(cmd, &push, sizeof(push));
    cmd.Dispatch(1, 1, 1);
    cmd.GlobalBarrier(gpu::PipelineStage::ComputeShader, gpu::Bits(gpu::Access::ShaderStorageWrite), gpu::PipelineStage::Host, gpu::Bits(gpu::Access::HostRead));
    slots_[renderer_->FrameIndex()].reflection_sampled = true;
}

void SceneRenderer::RecordPost(gpu::CommandList& cmd, const SceneLighting& lighting, float dt) {
    const ScreenSettings& screen = lighting.screen;
    const ExposureSettings& exposure = lighting.exposure;
    gpu::PassPush push;
    push.ids.w = post_view_;
    push.f1.x = 0.0f;

    const char* native_order_override = std::getenv("PT_AA_NATIVE_ORDER");
    const bool native_aa_order = toggles.fxaa && !up_.enabled && native_order_override && std::string(native_order_override) == "1";
    if (native_aa_order) {
        cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}, {&hdr_copy_, gpu::Layout::ColorAttachment}});
        cmd.BeginPass(output_extent_, {{&hdr_copy_, false, {}}});
        gpu::PassPush aa;
        aa.ids = glm::uvec4(gpu::kImgHdr, 1, 0, post_view_);
        Fullscreen(cmd, fxaa_, aa);
        cmd.EndPass();
        cmd.UseTargets({{&hdr_copy_, gpu::Layout::ShaderReadOnly}, {&hdr_, gpu::Layout::ColorAttachment}});
        cmd.BeginPass(output_extent_, {{&hdr_, false, {}}});
        aa.ids = glm::uvec4(gpu::kImgHdrCopy, 2, 0, post_view_);
        Fullscreen(cmd, fxaa_, aa);
        cmd.EndPass();
        cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}});
    }

    const bool bloom = toggles.bloom && lighting.valid && std::abs(exposure.bloom_size) >= 1.0e-5f;
    if (bloom) {
        cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}, {&bloom_[0], gpu::Layout::ColorAttachment}});
        cmd.BeginPass(bloom_[0].Extent(), {{&bloom_[0], false, {}}});
        push.f0 = BloomPolynomial(exposure.bloom_extraction);
        push.f1 = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        push.f2 = bloom_shoulder_;
        Fullscreen(cmd, bright_, push);
        cmd.EndPass();
        static const uint32_t kSlots[3] = {gpu::kImgBloomA, gpu::kImgBloomB, gpu::kImgBloomSum};
        auto blur = [&](int src, int dst, gpu::PipelineHandle pipeline, const glm::vec4& f0, bool clear) {
            cmd.UseTargets({{&bloom_[src], gpu::Layout::ShaderReadOnly}, {&bloom_[dst], gpu::Layout::ColorAttachment}});
            cmd.BeginPass(bloom_[dst].Extent(), {{&bloom_[dst], clear, {}}});
            push.ids.x = kSlots[src];
            push.f0 = f0;
            Fullscreen(cmd, pipeline, push);
            cmd.EndPass();
        };
        const float size = exposure.bloom_size;
        const float weight = exposure.bloom_weight;
        if (hdr_post_ && !(size >= 0.1f && size < 0.2f)) {
            const int passes = std::clamp(static_cast<int>(std::floor(size * 10.0f)), 1, 64);
            const glm::vec4 saved_f1 = push.f1;
            for (int i = 0; i < passes; ++i) {
                const float sigma = std::sqrt(static_cast<float>(i * i + (i + 1) * (i + 1)));
                const float gain = i == 0 ? weight : 1.0f;
                blur(0, 1, kawase_, glm::vec4(sigma, gain, 0.0f, 1.0f), false);
                cmd.UseTargets({{&bloom_[1], gpu::Layout::ShaderReadOnly},
                                 {&bloom_[0], gpu::Layout::ColorAttachment},
                                 {&bloom_[2], gpu::Layout::ColorAttachment}});
                cmd.BeginPass(bloom_[0].Extent(), {{&bloom_[0], false, {}}, {&bloom_[2], i == 0, {}}});
                push.ids.x = kSlots[1];
                push.f0 = glm::vec4(sigma, gain, 0.0f, 2.0f);
                push.f1 = glm::vec4(weight / static_cast<float>(passes), 0.0f, 0.0f, 0.0f);
                Fullscreen(cmd, kawase_sum_, push);
                cmd.EndPass();
            }
            push.f1 = saved_f1;
        } else if (size >= 0.1f && size < 0.2f) {
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
                cmd.UseTargets({{&bloom_[1], gpu::Layout::ShaderReadOnly},
                                 {&bloom_[0], gpu::Layout::ColorAttachment},
                                 {&bloom_[2], gpu::Layout::ColorAttachment}});
                cmd.BeginPass(bloom_[0].Extent(), {{&bloom_[0], false, {}}, {&bloom_[2], i == 0, {}}});
                push.ids.x = kSlots[1];
                push.f0 = kawase;
                push.f1 = glm::vec4(weight / static_cast<float>(passes), 0.0f, 0.0f, 0.0f);
                Fullscreen(cmd, kawase_sum_, push);
                cmd.EndPass();
            }
            push.f1 = saved_f1;
        }
        cmd.UseTargets({{&bloom_[2], gpu::Layout::ShaderReadOnly}, {&bloom_[0], gpu::Layout::ShaderReadOnly}});
    }
    Stamp(cmd, 11);

    cmd.UseTargets({{&flare_, gpu::Layout::ColorAttachment}, {&depth_, gpu::Layout::DepthReadOnly}});
    cmd.BeginPass(output_extent_, {{&flare_, true, {}}});
    if (vfx_filter && vr_eye_ < 0) {
        vfx_filter(FilterContext(cmd, 1u));
        BindSets(cmd, gpu::BindPoint::Graphics);
    }
    cmd.EndPass();
    cmd.UseTargets({{&flare_, gpu::Layout::ShaderReadOnly}});

    int current = 0;
    cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}, {&ldr_[0], gpu::Layout::ColorAttachment}});
    cmd.BeginPass(output_extent_, {{&ldr_[0], false, {}}});
    push.ids.x = 0;
    const float lut_blend = std::clamp(screen.lut_blend, 0.0f, 1.0f);
    const float bloom_strength = static_cast<float>(graphics.bloom_strength) * 0.01f;
    push.f0 = glm::vec4(bloom ? bloom_strength : 0.0f, lut_blend, lighting.valid ? 1.0f : renderer_->exposure, 0.0f);
    static const bool tonemap_test = std::getenv("PT_TONEMAP_TEST") != nullptr;
    if (tonemap_test) {
        push.f0.x = 0.0f;
        push.f0.y = 0.0f;
        push.f0.w = 0.0f;
    }
    gpu::PassPush tone = push;
    tone.f1 = glm::vec4(graphics.deband ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    Fullscreen(cmd, tonemap_, tone);
    cmd.EndPass();
    cmd.UseTargets({{&ldr_[0], gpu::Layout::ShaderReadOnly}});

    auto ldr_index = [](int i) { return i == 0 ? gpu::kImgLdrA : gpu::kImgLdrB; };
    auto run = [&](gpu::PipelineHandle pipeline, const gpu::PassPush& p) {
        RenderTarget& dst = ldr_[1 - current];
        cmd.UseTargets({{&ldr_[current], gpu::Layout::ShaderReadOnly}, {&dst, gpu::Layout::ColorAttachment}});
        cmd.BeginPass(output_extent_, {{&dst, false, {}}});
        Fullscreen(cmd, pipeline, p);
        cmd.EndPass();
        cmd.UseTargets({{&dst, gpu::Layout::ShaderReadOnly}});
        current = 1 - current;
    };

    if (toggles.fxaa && !up_.enabled && !native_aa_order) {
        push.ids.x = ldr_index(current);
        run(fxaa_, push);
    }
    if (toggles.depth_of_field && screen.depth_of_field && lighting.valid) {
        RecordDepthOfField(cmd, screen, current);
    }
    const bool street_blur = toggles.motion_blur && screen.motion_blur && lighting.valid && screen.smooth_camera_motion_blur;
    if (toggles.motion_blur && screen.motion_blur && lighting.valid && !street_blur) {
        RecordMotionBlur(cmd, screen, current);
    }
    if (subliminal_string_state_.value > 0.0f && subliminal_string_) {
        gpu::PassPush s;
        s.ids = glm::uvec4(subliminal_string_state_.texture, ldr_index(current), 0, 0);
        const float across = output_extent_.height ? (static_cast<float>(output_extent_.width) / static_cast<float>(output_extent_.height)) / (16.0f / 9.0f) : 1.0f;
        s.f0 = glm::vec4(subliminal_string_state_.position, subliminal_string_state_.value, across);
        s.f1 = glm::vec4(bloom && !tonemap_test ? bloom_strength : 0.0f, lighting.valid ? 1.0f : renderer_->exposure, 0.0f, 0.0f);
        s.f2 = glm::vec4(subliminal_string_state_.ink, 0.0f, 0.0f, 0.0f);
        run(subliminal_string_, s);
    }
    if (toggles.color_lut) {
        gpu::PassPush lut;
        lut.ids = glm::uvec4(ldr_index(current), 1, 0, post_view_);
        lut.f0 = glm::vec4(0.0f, lut_blend, 1.0f, 1.0f);
        lut.f1 = glm::vec4(graphics.deband ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        lut.f2 = glm::vec4(screen.black_lift, 0.0f);
        run(tonemap_, lut);
    }
    if (street_blur) {
        RecordMotionBlur(cmd, screen, current);
    }

    if (backdrop_state_.opacity > 0.0f && backdrop_state_.texture && backdrop_) {
        cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}});
        gpu::PassPush b;
        b.ids = glm::uvec4(ldr_index(current), backdrop_state_.texture, 0, main_view_.index);
        b.f0 = glm::vec4(backdrop_state_.opacity, backdrop_state_.darken, backdrop_state_.tear, backdrop_state_.time);
        b.f1 = glm::vec4(backdrop_state_.tint, backdrop_state_.aspect);
        b.f2 = glm::vec4(output_extent_.height ? static_cast<float>(output_extent_.width) / static_cast<float>(output_extent_.height) : 16.0f / 9.0f,
                         0.0f, 0.0f, 0.0f);
        b.m = glm::mat4(0.0f);
        b.m[0] = glm::vec4(backdrop_state_.zoom, backdrop_state_.center, backdrop_state_.focus);
        run(backdrop_, b);
    }

    if (date_active_) {
        if (!date_history_valid_) {
            date_live_frames_ = 0;
            date_still_frames_ = 0;
        }
        float weight = 1.0f;
        if (date_still_now_) {
            if (date_still_frames_ == 0) date_still_frames_ = std::min(date_live_frames_, 8u);
            ++date_still_frames_;
            weight = 1.0f / static_cast<float>(std::min(date_still_frames_, 64u));
        } else {
            date_still_frames_ = 0;
            ++date_live_frames_;
            weight = std::max(1.0f / static_cast<float>(std::min(date_live_frames_, 1000u)), 0.125f);
        }
        const uint32_t from = date_history_;
        const uint32_t to = 1u - from;
        cmd.UseTargets({{&mb_blur_[from], gpu::Layout::ShaderReadOnly}});
        gpu::PassPush r;
        r.ids = glm::uvec4(ldr_index(current), gpu::kImgMbBlurA + from, 0u, post_view_);
        r.f0 = glm::vec4(weight, date_still_now_ ? 0.0f : 1.0f, 0.0f, 0.0f);
        PostPass(cmd, mb_blur_[to], date_resolve_, r);
        gpu::PassPush o;
        o.ids = glm::uvec4(gpu::kImgMbBlurA + to, 0u, 1u, post_view_);
        run(date_resolve_out_, o);
        date_history_ = to;
        date_history_valid_ = true;
    }

    const bool blur = toggles.full_screen_blur && screen.full_screen_blur && lighting.valid;
    if (blur) {
        if (!history_valid_) {
            cmd.UseTargets({{&ldr_[current], gpu::Layout::TransferSrc}, {&history_, gpu::Layout::TransferDst}});
            CopyToHistory(cmd, ldr_[current]);
            cmd.UseTargets({{&ldr_[current], gpu::Layout::ShaderReadOnly}});
        }
        cmd.UseTargets({{&history_, gpu::Layout::ShaderReadOnly}});
        push.ids.x = ldr_index(current);
        push.ids.y = gpu::kImgHistory;
        const float share = std::clamp(dt * 30.0f, 0.05f, 2.0f);
        const float rate = std::pow(std::clamp(screen.blur_blend_rate, 0.0f, 0.999f), share);
        push.f0 = glm::vec4(screen.blur_fetch_band * std::sqrt(share), rate, 0.0f, 0.0f);
        run(fsblur_, push);
        cmd.UseTargets({{&ldr_[current], gpu::Layout::TransferSrc}, {&history_, gpu::Layout::TransferDst}});
        CopyToHistory(cmd, ldr_[current]);
        cmd.UseTargets({{&ldr_[current], gpu::Layout::ShaderReadOnly}, {&history_, gpu::Layout::ShaderReadOnly}});
        history_valid_ = true;
    } else {
        history_valid_ = false;
    }
    if (screen.colour_banding_canceller && lighting.valid) {
        cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}});
        push.ids = glm::uvec4(ldr_index(current), 0, 0, main_view_.index);
        run(banding_, push);
    }

    const bool grain = toggles.film_grain && screen.film_grain && lighting.valid;
    const bool distortion = toggles.distortion && screen.screen_distortion && lighting.valid;
    const bool hdr_sharpen = hdr_post_ && graphics.clarity > 0.0f;
    if (hdr_sharpen && distortion) {
        push.ids = glm::uvec4(ldr_index(current), 0, 0, post_view_);
        push.f0 = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        push.f1 = glm::vec4(0.0f);
        run(screen_fx_, push);
    }
    RenderTarget output;
    output.image = renderer_->SceneColor();
    output.layout = gpu::Layout::ColorAttachment;
    cmd.UseTargets({{&output, gpu::Layout::ColorAttachment}, {&depth_, gpu::Layout::DepthReadOnly}});
    cmd.BeginPass(output_extent_, {{&output, false, {}}});
    push.ids = glm::uvec4(ldr_index(current), 0, 0, post_view_);
    push.f0 = glm::vec4(distortion && !hdr_sharpen ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    push.f1 = glm::vec4(graphics.clarity, graphics.deband ? 1.0f : 0.0f, hdr_sharpen ? 1.0f : 0.0f, 0.0f);
    Fullscreen(cmd, screen_fx_, push);
    renderer_->grain[0] = grain ? 1.0f : 0.0f;
    renderer_->grain[1] = screen.grain_alt ? 1.0f : 0.0f;
    renderer_->grain[2] = screen.grain_strength * graphics.film_grain;
    renderer_->grain[3] = static_cast<float>(graphics.film_grain_type);
    renderer_->grain_offset[0] = screen.grain_offset.x;
    renderer_->grain_offset[1] = screen.grain_offset.y;
    if (vfx_filter && vr_eye_ < 0) {
        SceneFilterContext context = FilterContext(cmd, 2u);
        context.color_format = renderer_->SceneColorFormat();
        vfx_filter(context);
        BindSets(cmd, gpu::BindPoint::Graphics);
    }
    cmd.EndPass();
    (void)dt;
}

SceneFilterContext SceneRenderer::FilterContext(gpu::CommandList& cmd, uint32_t layers) const {
    SceneFilterContext context;
    context.cmd = &cmd;
    context.extent = output_extent_;
    context.color_format = kPostTargetFormat;
    context.source = ldr_[0].image.view;
    context.depth_view = flare_depth_.Valid() ? flare_depth_.image.view : depth_.image.view;
    context.exposure = exposure_;
    context.frame_index = renderer_->FrameIndex();
    context.layers = layers;
    return context;
}

void SceneRenderer::PostPass(gpu::CommandList& cmd, RenderTarget& target, gpu::PipelineHandle pipeline, const gpu::PassPush& push) {
    cmd.UseTargets({{&target, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(target.Extent(), {{&target, false, {}}});
    Fullscreen(cmd, pipeline, push);
    cmd.EndPass();
    cmd.UseTargets({{&target, gpu::Layout::ShaderReadOnly}});
}

bool SceneRenderer::RecordDepthOfField(gpu::CommandList& cmd, const ScreenSettings& screen, int& current) {
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
        const gpu::Extent2D e = t.Extent();
        return glm::vec4(e.width, e.height, 1.0f / e.width, 1.0f / e.height);
    };
    gpu::PassPush push;
    push.ids = glm::uvec4(0, 0, 0, post_view_);
    push.f0 = glm::vec4(d, near_limit, near_scale, 1.0f);
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}});
    PostPass(cmd, ldr_[current], dof_ratio_, push);

    if (hdr_post_) {
        const bool near_on = (screen.dof_flags & 1u) == 0;
        const float rows = static_cast<float>(output_extent_.height) / 1080.0f;
        const float far_size = std::max(4.0f, std::min(s, 10.0f));
        const float far_radius = 0.5f * far_size * rows;
        const float near_radius = near_on ? 0.4f * std::min(near_limit, 20.0f) * rows : 0.0f;
        const auto two_targets = [&](gpu::PipelineHandle pipeline, RenderTarget& a, RenderTarget& b, const gpu::PassPush& p,
                                     std::initializer_list<RenderTarget*> reads) {
            for (RenderTarget* r : reads) {
                cmd.UseTargets({{r, gpu::Layout::ShaderReadOnly}});
            }
            cmd.UseTargets({{&a, gpu::Layout::ColorAttachment}, {&b, gpu::Layout::ColorAttachment}});
            cmd.BeginPass(a.Extent(), {{&a, false, {}}, {&b, false, {}}});
            Fullscreen(cmd, pipeline, p);
            cmd.EndPass();
            cmd.UseTargets({{&a, gpu::Layout::ShaderReadOnly}, {&b, gpu::Layout::ShaderReadOnly}});
        };
        gpu::PassPush layers;
        layers.ids = glm::uvec4(scene, 0, 0, post_view_);
        two_targets(dof_layers_, dof_quarter_[0], dof_quarter_[1], layers, {&ldr_[current]});
        gpu::PassPush gather;
        gather.ids = glm::uvec4(gpu::kImgDofQuarterA, gpu::kImgDofQuarterB, 0, post_view_);
        gather.f0 = glm::vec4(0.5f * far_radius, 0.5f * near_radius, std::min(4.0f / far_size, 0.95f), near_limit * 0.125f);
        two_targets(dof_gather_, dof_eighth_[0], dof_eighth_[1], gather, {});
        gpu::PassPush merge;
        merge.ids = glm::uvec4(scene, gpu::kImgDofEighthA, gpu::kImgDofEighthB, post_view_);
        const float blend_size = std::min(s, 10.0f);
        merge.f0 = glm::vec4(blend_size > 4.0f ? blend_size * 0.25f : 1.0f, s < 4.0f ? s * 0.25f : 1.0f, near_on ? std::min(near_limit * 0.125f, 1.0f) : 0.0f, 0.0f);
        PostPass(cmd, ldr_[1 - current], dof_merge_, merge);
        current = 1 - current;
        return true;
    }

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
    bool cut = false;
    motion_cut_ = false;
    if (glm::distance(previous.position, camera.position) > kCameraCutDistance || glm::dot(previous.Forward(), camera.Forward()) < 0.7071f) {
        previous = camera;
        up_.reset = true;
        cut = true;
        motion_cut_ = true;
    }
    if (!motion_history_valid_) {
        up_.reset = true;
    }
    const float aspect = static_cast<float>(output_extent_.width) / static_cast<float>(std::max(1u, output_extent_.height));
    previous_view_projection_ = previous.Projection(aspect) * previous.View();
    const float step = std::clamp(interval, 1.0e-4f, 1.0f);
    motion_step_ = step;
    frame_time_ = reference ? static_cast<double>(step) : static_cast<double>(step * 0.016666668f) + frame_time_ * 0.9833333333333333;
    if (!lighting_->screen.smooth_camera_motion_blur) {
        const bool held = reference && previous.position == camera.position && previous.yaw == camera.yaw && previous.pitch == camera.pitch &&
                          previous.roll == camera.roll && previous.fov_y == camera.fov_y;
        double span = 0.0;
        const Camera from = motion_window_.Advance(previous, camera, step, motion_history_valid_ && !cut && !held, &span);
        blur_span_ = static_cast<float>(std::max(span, 1.0e-4));
        blur_previous_view_projection_ = from.Projection(aspect) * from.View();
        blur_rates_valid_ = false;
        return;
    }
    motion_window_.Reset();
    if (reference) {
        blur_previous_view_projection_ = previous_view_projection_;
        blur_rates_valid_ = false;
        return;
    }
    const auto wrap = [](float a) { return std::remainder(a, glm::two_pi<float>()); };
    const glm::vec4 rates(wrap(camera.yaw - previous.yaw) / step, (camera.pitch - previous.pitch) / step, wrap(camera.roll - previous.roll) / step, 0.0f);
    const glm::vec3 velocity = (camera.position - previous.position) / step;
    if (cut || !motion_history_valid_ || !blur_rates_valid_) {
        blur_rates_ = cut ? glm::vec4(0.0f) : rates;
        blur_velocity_ = cut ? glm::vec3(0.0f) : velocity;
        blur_rates_valid_ = true;
    } else {
        const float follow = 1.0f - std::exp(-step * 30.0f);
        blur_rates_ += (rates - blur_rates_) * follow;
        blur_velocity_ += (velocity - blur_velocity_) * follow;
    }
    const float span = static_cast<float>(frame_time_);
    Camera blurred = camera;
    blurred.yaw = camera.yaw - blur_rates_.x * span;
    blurred.pitch = camera.pitch - blur_rates_.y * span;
    blurred.roll = camera.roll - blur_rates_.z * span;
    blurred.position = camera.position - blur_velocity_ * span;
    blur_previous_view_projection_ = blurred.Projection(aspect) * blurred.View();
}

bool SceneRenderer::RecordMotionBlur(gpu::CommandList& cmd, const ScreenSettings& screen, int& current) {
    const float shutter = screen.shutter_speed;
    if (!(shutter >= 0.001f)) {
        return false;
    }
    const bool street = screen.smooth_camera_motion_blur;
    const bool hdr = hdr_post_ && (street || !original_look);
    const bool smooth = street || hdr;
    const double span = street ? frame_time_ : static_cast<double>(blur_span_);
    const float original_frames = static_cast<float>(span / render::kOriginalFrameSeconds);
    const float ratio = screen.fixed_shutter && !hdr ? shutter * 60.0f / (street ? 1.0f : std::max(original_frames, 1.0e-3f))
                                                      : shutter * static_cast<float>(1.0 / span);
    const float mb_strength = std::clamp(graphics.motion_blur_strength, 0, 100) * 0.01f;
    if (mb_strength <= 0.001f) return false;
    const float scale = ratio * 0.5f * mb_strength;
    const float limit = (street ? std::min(1.0f, static_cast<float>(span) * 30.0f) : std::min(1.0f, original_frames)) * mb_strength;
    const float object_stretch = street ? 1.0f : std::max(1.0f, blur_span_ / std::max(motion_step_, 1.0e-4f));
    if (static const bool trace = std::getenv("PT_TRACE_MOTION_BLUR") != nullptr; trace) {
        static uint32_t traced = 0;
        if (traced++ % 120 == 0) {
            LogInfo("motion blur: {} passes, frame {:.2f} ms, movement over {:.2f} ms, scale {:.3f}, limit {:.3f} of 128 px, objects x{:.2f}",
                    street ? "street" : hdr ? "hdr" : "original", motion_step_ * 1000.0f, span * 1000.0, scale, limit, object_stretch);
        }
    }
    RenderTarget& bake = smooth ? mb_bake_ : mb_original_bake_;
    RenderTarget* blur = smooth ? mb_blur_ : mb_original_blur_;
    const uint32_t bake_image = smooth ? gpu::kImgMbBake : gpu::kImgMbOriginalBake;
    const uint32_t blur_image = smooth ? gpu::kImgMbBlurA : gpu::kImgMbOriginalBlurA;
    const uint32_t blur_mode = hdr ? 2u : smooth ? 1u : 0u;
    gpu::PassPush push;
    push.ids = glm::uvec4(gpu::kImgObjectVelocity, 0, 0, post_view_);
    push.f0 = glm::vec4(1920.0f / 128.0f, 1080.0f / 128.0f, limit, graphics.camera_motion_blur ? 0.0f : 1.0f);
    push.f1 = glm::vec4(hdr && motion_cut_ ? 1.0f : 0.0f, object_stretch, 0.0f, 0.0f);
    push.m = blur_previous_view_projection_;
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}});
    PostPass(cmd, velocity_, mb_velocity_, push);
    push = gpu::PassPush{};
    for (uint32_t i = 0; i < 5; ++i) {
        const gpu::Extent2D e = mb_tile_[i].Extent();
        push.ids = glm::uvec4(i == 0 ? static_cast<uint32_t>(gpu::kImgVelocity) : gpu::kImgMbTile + i - 1, 0, 0, post_view_);
        push.f0 = glm::vec4(1.0f / e.width, 1.0f / e.height, 0.0f, 0.0f);
        PostPass(cmd, mb_tile_[i], mb_tile_pipeline_, push);
    }
    push.ids = glm::uvec4(gpu::kImgMbTile + 4, 0, 1, post_view_);
    PostPass(cmd, mb_neighbour_, mb_tile_pipeline_, push);
    push.ids = glm::uvec4(gpu::kImgVelocity, gpu::kImgMbNeighbour, 0, post_view_);
    push.f0 = glm::vec4(0.5f, 0.5f, 1.0f, scale);
    PostPass(cmd, bake, mb_bake_pipeline_, push);
    const uint32_t picture = current == 0 ? gpu::kImgLdrA : gpu::kImgLdrB;
    const float shown = smooth ? 2.2f / renderer_->OutputBrightness() : 0.0f;
    if (smooth) {
        gpu::PassPush decode;
        decode.ids = glm::uvec4(picture, 0, 0, post_view_);
        decode.f0 = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        decode.f1 = glm::vec4(0.0f, shown, 0.0f, 0.0f);
        PostPass(cmd, blur[1], mb_decode_, decode);
    }
    push.ids = glm::uvec4(smooth ? blur_image + 1 : picture, bake_image, blur_mode, post_view_);
    PostPass(cmd, blur[0], mb_mcguire_, push);
    if (hdr) {
        gpu::PassPush lay;
        lay.ids = glm::uvec4(blur_image, picture, 0, post_view_);
        lay.f0 = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        lay.f1 = glm::vec4(0.0f, 0.0f, shown, 0.0f);
        PostPass(cmd, ldr_[1 - current], mb_decode_, lay);
        current = 1 - current;
        return true;
    }
    push.ids = glm::uvec4(blur_image, bake_image, smooth ? 1u : 0u, post_view_);
    push.f0 = glm::vec4(0.5f, 0.5f, 0.16666667f, scale);
    PostPass(cmd, blur[1], mb_mcguire_, push);
    push.ids = glm::uvec4(blur_image + 1, 0, 0, post_view_);
    push.f0 = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    push.f1 = glm::vec4(shown, 0.0f, 0.0f, 0.0f);
    PostPass(cmd, ldr_[current], mb_composite_, push);
    return true;
}

void SceneRenderer::RecordDebug(gpu::CommandList& cmd) {
    cmd.UseTargets({{&albedo_, gpu::Layout::ShaderReadOnly},
                     {&normal_, gpu::Layout::ShaderReadOnly},
                     {&material_, gpu::Layout::ShaderReadOnly},
                     {&depth_, gpu::Layout::DepthReadOnly},
                     {&diffuse_, gpu::Layout::ShaderReadOnly},
                     {&specular_, gpu::Layout::ShaderReadOnly}});
    RenderTarget output;
    output.image = renderer_->SceneColor();
    output.layout = gpu::Layout::ColorAttachment;
    cmd.UseTargets({{&output, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(output_extent_, {{&output, false, {}}});
    gpu::PassPush push;
    push.ids = glm::uvec4(static_cast<uint32_t>(debug_view), post_view_, 0, 0);
    push.f0 = glm::vec4(4.0f, 1.0f, 0.0f, 0.0f);
    Fullscreen(cmd, debug_, push);
    cmd.EndPass();
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
