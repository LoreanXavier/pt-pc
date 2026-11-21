#include "engine/render/scene_renderer.h"
#include "engine/core/log.h"
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace pt {

namespace {

constexpr uint32_t kSampleAt[] = {240, 3600, 18000};
constexpr uint32_t kSampleWidth = 480, kSampleHeight = 270;
constexpr uint32_t kSampleWait = 8;

glm::vec4 SampleTexel(const uint8_t* base, size_t index, gpu::Format format, bool& known) {
    known = true;
    if (format == gpu::Format::R16G16B16A16Sfloat) {
        uint16_t h[4];
        std::memcpy(h, base + index * 8, 8);
        return {glm::unpackHalf1x16(h[0]), glm::unpackHalf1x16(h[1]), glm::unpackHalf1x16(h[2]), glm::unpackHalf1x16(h[3])};
    }
    if (format == gpu::Format::R32Sfloat) {
        float v;
        std::memcpy(&v, base + index * 4, 4);
        return {v, 0.0f, 0.0f, 0.0f};
    }
    if (format == gpu::Format::R8G8B8A8Unorm) {
        const uint8_t* t = base + index * 4;
        return glm::vec4(t[0], t[1], t[2], t[3]) / 255.0f;
    }
    known = false;
    return glm::vec4(0.0f);
}

void LogSampleStats(const char* name, const std::vector<glm::vec4>& t, uint32_t width, uint32_t height) {
    glm::dvec4 sum(0.0);
    double detail = 0.0;
    float low = 1.0e30f, high = -1.0e30f;
    size_t zero = 0, bad = 0, counted = 0;
    const auto finite = [](const glm::vec4& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); };
    const auto luma = [](const glm::vec4& v) { return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z; };
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const glm::vec4& v = t[size_t(y) * width + x];
            if (!finite(v)) {
                ++bad;
                continue;
            }
            sum += glm::dvec4(v);
            low = std::min({low, v.x, v.y, v.z});
            high = std::max({high, v.x, v.y, v.z});
            zero += v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
            if (x + 1 < width && y + 1 < height) {
                const glm::vec4& right = t[size_t(y) * width + x + 1];
                const glm::vec4& down = t[size_t(y + 1) * width + x];
                if (finite(right) && finite(down)) {
                    detail += std::abs(luma(right) - luma(v)) + std::abs(luma(down) - luma(v));
                    ++counted;
                }
            }
        }
    }
    const double n = std::max<double>(1.0, static_cast<double>(t.size() - bad));
    LogInfo("fsr rr sample: {}: mean {:.5g} {:.5g} {:.5g} a {:.5g}, min {:.5g}, max {:.5g}, black {:.1f}%, not finite {}, detail {:.5g}", name, sum.x / n, sum.y / n, sum.z / n, sum.w / n,
            low, high, 100.0 * static_cast<double>(zero) / n, bad, detail / std::max<double>(1.0, 2.0 * static_cast<double>(counted)));
}

}

void SceneRenderer::DestroyFsrRrSamples() {
    for (FsrRrSample& s : fsr_rr_samples_) {
        gpu_->DestroyBuffer(s.buffer);
    }
    fsr_rr_samples_.clear();
    fsr_rr_sample_age_ = 0;
}

void SceneRenderer::RecordFsrRrSamples(gpu::CommandList& cmd, RenderTarget* const* output) {
    const auto due = [&] {
        for (const uint32_t at : kSampleAt) {
            if (fsr_rr_denoised_ == at) {
                return true;
            }
        }
        return false;
    };
    if (!fsr_rr_samples_.empty() || !due()) {
        return;
    }
    const std::pair<const char*, RenderTarget*> targets[] = {
        {"direct diffuse in", &fsr_rr_input_[0]}, {"direct specular in", &fsr_rr_input_[1]}, {"indirect diffuse in", &fsr_rr_input_[2]}, {"indirect specular in", &fsr_rr_input_[3]},
        {"direct diffuse out", output[0]}, {"direct specular out", output[1]}, {"indirect diffuse out", output[2]}, {"indirect specular out", output[3]},
        {"linear depth", &fsr_rr_depth_}, {"motion", &fsr_rr_motion_}, {"normal roughness", &fsr_rr_normal_},
        {"diffuse albedo guide", &rr_diffuse_albedo_}, {"specular albedo guide", &rr_specular_albedo_},
        {"G-buffer albedo", &albedo_}, {"diffuse target", &diffuse_}, {"specular target", &specular_}};
    fsr_rr_sample_size_ = {std::min(kSampleWidth, extent_.width), std::min(kSampleHeight, extent_.height)};
    for (const auto& [name, target] : targets) {
        FsrRrSample s;
        s.name = name;
        s.format = target->image.format;
        const uint64_t size = uint64_t(fsr_rr_sample_size_.width) * fsr_rr_sample_size_.height * gpu::Describe(s.format).bytes;
        if (!target->Valid() || !MakeBuffer(s.buffer, size, gpu::BufferUsage::TransferDst, true)) {
            continue;
        }
        const gpu::Layout back = target->layout;
        cmd.UseTargets({{target, gpu::Layout::TransferSrc}});
        gpu::BufferImageCopy region;
        region.image = {target->aspect, 0, 0, 1};
        region.image_offset[0] = static_cast<int32_t>((extent_.width - fsr_rr_sample_size_.width) / 2);
        region.image_offset[1] = static_cast<int32_t>((extent_.height - fsr_rr_sample_size_.height) / 2);
        region.extent = {fsr_rr_sample_size_.width, fsr_rr_sample_size_.height, 1};
        cmd.CopyImageToBuffer(target->image, gpu::Layout::TransferSrc, s.buffer, {&region, 1});
        cmd.UseTargets({{target, back}});
        fsr_rr_samples_.push_back(s);
    }
    fsr_rr_sample_age_ = 0;
    LogInfo("fsr rr sample: copied at dispatch {}, {} images, {}x{} at the centre of {}x{}, exposure {:.5g}", fsr_rr_denoised_, fsr_rr_samples_.size(), fsr_rr_sample_size_.width,
            fsr_rr_sample_size_.height, extent_.width, extent_.height, exposure_);
}

void SceneRenderer::LogFsrRrSamples() {
    const uint32_t width = fsr_rr_sample_size_.width, height = fsr_rr_sample_size_.height;
    const size_t count = size_t(width) * height;
    std::vector<glm::vec4> albedo, diffuse, specular, texels(count);
    for (const FsrRrSample& s : fsr_rr_samples_) {
        gpu_->InvalidateMapped(s.buffer, 0, ~0ull);
        const auto* base = static_cast<const uint8_t*>(s.buffer.mapped);
        bool known = base != nullptr;
        for (size_t i = 0; known && i < count; ++i) {
            texels[i] = SampleTexel(base, i, s.format, known);
        }
        if (!known) {
            LogInfo("fsr rr sample: {}: format {} not read", s.name, static_cast<int>(s.format));
            continue;
        }
        LogSampleStats(s.name, texels, width, height);
        const std::string_view name(s.name);
        if (name == "G-buffer albedo") albedo = texels;
        if (name == "diffuse target") diffuse = texels;
        if (name == "specular target") specular = texels;
    }
    if (albedo.size() == count && diffuse.size() == count && specular.size() == count) {
        const auto decode = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
        for (size_t i = 0; i < count; ++i) {
            const glm::vec3 a(decode(albedo[i].x), decode(albedo[i].y), decode(albedo[i].z));
            texels[i] = glm::vec4(glm::vec3(diffuse[i]) * a + glm::vec3(specular[i]), 0.0f);
        }
        LogSampleStats("composed estimate", texels, width, height);
    }
    DestroyFsrRrSamples();
}

bool SceneRenderer::EnsureFsrRrTargets() {
    if (!fsr_rr_prepare_ || !fsr_rr_resolve_ || !fsr_rr_motion_trace_ || !fsr_rr_velocity_) return false;
    bool ready = true;
    for (const RenderTarget* t : {&fsr_rr_input_[0], &fsr_rr_input_[1], &fsr_rr_input_[2], &fsr_rr_input_[3],
                                &fsr_rr_depth_, &fsr_rr_motion_, &fsr_rr_normal_}) ready &= t->Valid() && t->Extent() == extent_;
    if (ready) return true;
    const auto usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc;
    for (RenderTarget* t : {&fsr_rr_input_[0], &fsr_rr_input_[1], &fsr_rr_input_[2], &fsr_rr_input_[3],
                          &fsr_rr_depth_, &fsr_rr_motion_, &fsr_rr_normal_}) {
        gpu_->DestroyImage(t->image);
        *t = {};
        const auto format = t == &fsr_rr_depth_ ? gpu::Format::R32Sfloat : gpu::Format::R16G16B16A16Sfloat;
        if (!CreateTarget(*t, format, extent_, usage, gpu::Aspect::Color)) return false;
    }
    LogInfo("fsr rr: radiance and geometry inputs {}x{}", extent_.width, extent_.height);
    return true;
}

void SceneRenderer::RecordFsrRayRegeneration(gpu::CommandList& cmd, const ViewSetup& view) {
    if (!fsr_rr_samples_.empty() && ++fsr_rr_sample_age_ > kSampleWait) {
        LogFsrRrSamples();
    }
    for (auto& t : fsr_rr_input_) cmd.UseTargets({{&t, gpu::Layout::ColorAttachment}});
    cmd.UseTargets({{&fsr_rr_depth_, gpu::Layout::ColorAttachment}, {&fsr_rr_normal_, gpu::Layout::ColorAttachment}, {&depth_, gpu::Layout::DepthReadOnly},
                    {&motion_, gpu::Layout::ShaderReadOnly}});
    BindSets(cmd, gpu::BindPoint::Graphics);
    const auto rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    cmd.BeginPass(extent_, {{&fsr_rr_input_[0], false, {}}, {&fsr_rr_input_[1], false, {}},
                            {&fsr_rr_input_[2], false, {}}, {&fsr_rr_input_[3], false, {}},
                            {&fsr_rr_depth_, false, {}}, {&fsr_rr_normal_, false, {}}});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, pt_indirect_on_ ? 1u : 0u, 0u, 0u);
    push.m = previous_view_projection_;
    Fullscreen(cmd, fsr_rr_prepare_, push);
    cmd.EndPass();
    RecordUpscaleMotion(cmd, view, true);
    for (auto& t : fsr_rr_input_) cmd.UseTargets({{&t, gpu::Layout::ShaderReadOnly}});
    cmd.UseTargets({{&fsr_rr_depth_, gpu::Layout::ShaderReadOnly}, {&fsr_rr_motion_, gpu::Layout::ShaderReadOnly},
                    {&fsr_rr_normal_, gpu::Layout::ShaderReadOnly}});
    RenderTarget* output[] = {&rt_full_images_[10], &rt_full_images_[15], &rt_full_images_[11], &rt_full_images_[16]};
    for (auto* t : output) cmd.UseTargets({{t, gpu::Layout::General}});
    cmd.GlobalBarrier(gpu::PipelineStage::ComputeShader | gpu::PipelineStage::FragmentShader,
                      gpu::Access::ShaderStorageWrite | gpu::Access::ShaderStorageRead,
                      gpu::PipelineStage::ComputeShader, gpu::Access::ShaderStorageWrite | gpu::Access::ShaderStorageRead);
    const auto wrap = [](const RenderTarget& t) {
        UpscaleImage image;
        image.handle = t.image.image;
        image.extent = gpu::vk::Native(t.Extent());
        return image;
    };
    RayDenoiseDispatch d;
    d.list = &cmd;
    for (unsigned i = 0; i < 4; ++i) {
        d.input[i] = wrap(fsr_rr_input_[i]);
        d.output[i] = wrap(*output[i]);
    }
    d.depth = wrap(fsr_rr_depth_);
    d.motion = wrap(fsr_rr_motion_);
    d.normal_roughness = wrap(fsr_rr_normal_);
    d.diffuse_albedo = wrap(rr_diffuse_albedo_);
    d.specular_albedo = wrap(rr_specular_albedo_);
    d.render = gpu::vk::Native(extent_);
    d.jitter = up_.jitter_pixels;
    d.eye = camera_.position;
    d.world_to_view = camera_.View();
    d.view_to_clip = camera_.Projection(static_cast<float>(output_extent_.width) / output_extent_.height);
    d.view_to_clip[1][1] = -d.view_to_clip[1][1];
    d.near_plane = camera_.near_plane;
    d.frame_index = frame_counter_;
    d.reset = up_.reset;
    bool ok = false;
    static const bool through_backend = [] {
        const char* e = std::getenv("PT_FSR_RR_INPUT_CHECK");
        return e && e[0] == '2';
    }();
    if (fsr_rr_input_check_ && !through_backend) {
        static bool logged = false;
        if (!logged) {
            LogInfo("fsr rr input check: native four-signal pass-through, no AMD inference");
            logged = true;
        }
        for (unsigned i = 0; i < 4; ++i) {
            cmd.UseTargets({{&fsr_rr_input_[i], gpu::Layout::TransferSrc}, {output[i], gpu::Layout::TransferDst}});
            gpu::ImageCopy copy;
            copy.src = {gpu::Aspect::Color, 0, 0, 1}; copy.dst = copy.src;
            copy.extent = {extent_.width, extent_.height, 1};
            cmd.CopyImage(fsr_rr_input_[i].image, gpu::Layout::TransferSrc, output[i]->image, gpu::Layout::TransferDst, {&copy, 1});
            cmd.UseTargets({{output[i], gpu::Layout::General}, {&fsr_rr_input_[i], gpu::Layout::ShaderReadOnly}});
        }
        ok = true;
    } else ok = up_.backend->Denoise(d);
    BindSets(cmd, gpu::BindPoint::Graphics);
    BindSets(cmd, gpu::BindPoint::Compute);
    if (!ok) {
        static bool logged = false;
        if (!logged) {
            LogWarn("fsr rr: no denoised light this frame (dispatch {}): the noisy light stays in the targets", fsr_rr_denoised_);
            logged = true;
        }
        return;
    }
    cmd.GlobalBarrier(gpu::PipelineStage::ComputeShader, gpu::Bits(gpu::Access::ShaderStorageWrite),
                      gpu::PipelineStage::FragmentShader, gpu::Bits(gpu::Access::ShaderStorageRead));
    cmd.UseTargets({{&diffuse_, gpu::Layout::ColorAttachment}, {&specular_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent_, {{&diffuse_, false, {}}, {&specular_, false, {}}});
    cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
    Fullscreen(cmd, fsr_rr_resolve_, push);
    cmd.EndPass();
    cmd.UseTargets({{&diffuse_, gpu::Layout::ShaderReadOnly}, {&specular_, gpu::Layout::ShaderReadOnly}});
    ++fsr_rr_denoised_;
    RecordFsrRrSamples(cmd, output);
}

}
