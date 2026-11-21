#include "engine/ui/ui_batch.h"

#include "engine/render/pipeline_cache_store.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "engine/core/log.h"
#include "engine/render/renderer.h"
#include "engine/render/texture_manager.h"

namespace pt::ui {
namespace {

struct Push {
    glm::vec2 inverse_extent;
    uint32_t draw;
};

}

UiDrawParams UiDrawParams::Plain(uint32_t texture) {
    std::array<float, 28> params{};
    for (int g = 0; g < 4; ++g) {
        params[g * 6 + 0] = 0.5f;
        params[g * 6 + 1] = 0.5f;
        params[g * 6 + 4] = 1.0f;
        params[g * 6 + 5] = 1.0f;
    }
    params[24] = 1.0f;
    return FromMaterial({texture, TextureManager::kWhite, TextureManager::kWhite, TextureManager::kWhite}, params);
}

UiDrawParams UiDrawParams::FromMaterial(const std::array<uint32_t, 4>& textures, const std::array<float, 28>& p) {
    UiDrawParams out;
    out.textures = textures;
    out.material[0] = {p[0], p[1], p[2], p[3]};
    out.material[1] = {p[4], p[5], p[6], p[7]};
    out.material[2] = {p[8], p[9], p[10], p[11]};
    out.material[3] = {p[12], p[13], p[14], p[15]};
    out.material[4] = {p[16], p[17], p[18], p[19]};
    out.material[5] = {p[20], p[21], p[22], p[23]};
    out.material[6] = {p[24], p[25], p[26], p[27]};
    return out;
}

bool UiBatch::Init(Renderer& renderer, TextureManager& textures) {
    renderer_ = &renderer;
    textures_ = &textures;
    gpu_ = &renderer.Gpu();
    gpu::SamplerDesc sampler_info;
    sampler_info.mag = gpu::Filter::Linear;
    sampler_info.min = gpu::Filter::Linear;
    sampler_info.mipmap = gpu::MipmapMode::Nearest;
    sampler_info.address_u = gpu::AddressMode::ClampToEdge;
    sampler_info.address_v = gpu::AddressMode::ClampToEdge;
    sampler_info.address_w = gpu::AddressMode::ClampToEdge;
    sampler_info.max_anisotropy = 0.0f;
    sampler_info.max_lod = 0.0f;
    clamp_sampler_ = gpu_->CreateSampler(sampler_info);
    if (!clamp_sampler_) {
        return false;
    }
    gpu::BindingLayoutDesc set_info;
    set_info.bindings = {{0, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::Fragment},
                         {1, gpu::BindingType::CombinedImageSampler, 1, gpu::ShaderStage::Fragment}};
    set_layout_ = gpu_->CreateBindingLayout(set_info);
    if (!set_layout_) {
        LogError("ui: set layout failed");
        return false;
    }
    const uint32_t frame_count = Renderer::kFramesInFlight;
    gpu::BindingPoolDesc pool_info;
    pool_info.sizes = {{gpu::BindingType::StorageBuffer, frame_count}, {gpu::BindingType::CombinedImageSampler, frame_count}};
    pool_info.max_sets = frame_count;
    pool_ = gpu_->CreateBindingPool(pool_info);
    if (!pool_) {
        LogError("ui: pool failed");
        return false;
    }
    frames_.resize(frame_count);
    for (FrameData& frame : frames_) {
        if (!gpu_->CreateBuffer({sizeof(UiVertex) * kMaxVertices, gpu::BufferUsage::VertexBuffer, true}, frame.vertices) ||
            !gpu_->CreateBuffer({sizeof(UiDrawParams) * kMaxDraws, gpu::BufferUsage::StorageBuffer, true}, frame.draws)) {
            return false;
        }
        frame.set = gpu_->AllocateBindingSet(pool_, set_layout_);
        if (!frame.set) {
            LogError("ui: set failed");
            return false;
        }
        const gpu::BufferBinding buffer_info{frame.draws.buffer};
        const gpu::BindingWrite write{frame.set, 0, 0, gpu::BindingType::StorageBuffer, {}, {&buffer_info, 1}};
        gpu_->WriteBindings({&write, 1});
    }
    gpu::PipelineLayoutDesc layout_info;
    layout_info.sets = {textures.BindingLayout(), set_layout_};
    layout_info.push_bytes = sizeof(Push);
    layout_info.push_stages = gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment;
    layout_ = gpu_->CreatePipelineLayout(layout_info);
    if (!layout_) {
        LogError("ui: pipeline layout failed");
        return false;
    }
    return CreatePipelines(TargetFormat());
}

gpu::Format UiBatch::TargetFormat() const {
    return renderer_->FinalFormat();
}

bool UiBatch::CreatePipelines(gpu::Format format) {
    DestroyPipelines();
    gpu::GraphicsPipelineDesc descs[6];
    for (int i = 0; i < 6; ++i) {
        const bool marks = i >= 4;
        const bool additive = (i & 1) != 0;
        const bool coverage = !marks && i >= 2;
        gpu::GraphicsPipelineDesc& d = descs[i];
        d.vertex = "ui_sprite.vert";
        d.fragment = "ui_sprite.frag";
        d.layout = layout_;
        d.colors = {format};
        d.vertex_attributes = {{gpu::Format::R32G32Sfloat, static_cast<uint32_t>(offsetof(UiVertex, position))},
                               {gpu::Format::R32G32Sfloat, static_cast<uint32_t>(offsetof(UiVertex, uv))},
                               {gpu::Format::R32G32B32A32Sfloat, static_cast<uint32_t>(offsetof(UiVertex, color))}};
        d.vertex_stride = sizeof(UiVertex);
        d.fixed_state = true;
        d.depth_state = false;
        d.cull = gpu::CullMode::None;
        gpu::BlendState b;
        b.enable = true;
        b.src_color = gpu::BlendFactor::SrcAlpha;
        b.dst_color = !additive ? gpu::BlendFactor::OneMinusSrcAlpha : gpu::BlendFactor::One;
        b.color_op = gpu::BlendOp::Add;
        const bool covers = coverage && !additive;
        b.src_alpha = covers ? gpu::BlendFactor::One : gpu::BlendFactor::Zero;
        b.dst_alpha = covers || marks ? gpu::BlendFactor::OneMinusSrcAlpha : gpu::BlendFactor::One;
        b.alpha_op = gpu::BlendOp::Add;
        b.write_mask = gpu::kColorMaskAll;
        d.blend_states = {b};
    }
    const bool ok = gpu_->CreateGraphicsPipelines(descs, pipelines_);
    pipeline_format_ = format;
    return ok;
}

void UiBatch::DestroyPipelines() {
    for (gpu::PipelineHandle& pipeline : pipelines_) {
        if (pipeline) {
            gpu_->DestroyPipeline(pipeline);
            pipeline = {};
        }
    }
}

void UiBatch::Shutdown() {
    if (!gpu_) {
        return;
    }
    DestroyPipelines();
    for (FrameData& frame : frames_) {
        gpu_->DestroyBuffer(frame.vertices);
        gpu_->DestroyBuffer(frame.draws);
    }
    frames_.clear();
    gpu_->DestroyPipelineLayout(layout_);
    gpu_->DestroyBindingPool(pool_);
    gpu_->DestroyBindingLayout(set_layout_);
    gpu_->DestroySampler(clamp_sampler_);
    gpu_ = nullptr;
}

void UiBatch::Begin(gpu::Extent2D extent) {
    extent_ = extent;
    vertices_.clear();
    draws_.clear();
    commands_.clear();
    top_first_ = UINT32_MAX;
    uploaded_ = false;
}

void UiBatch::Draw(std::span<const UiVertex> triangles, const UiDrawParams& params, UiShade shade, UiBlend blend) {
    if (triangles.empty() || draws_.size() >= kMaxDraws || vertices_.size() + triangles.size() > kMaxVertices) {
        return;
    }
    UiDrawParams stored = params;
    stored.screen = glm::vec4(screen_.x, screen_.y, screen_.z, static_cast<float>(shade));
    Command command;
    command.first = static_cast<uint32_t>(vertices_.size());
    command.count = static_cast<uint32_t>(triangles.size());
    command.draw = static_cast<uint32_t>(draws_.size());
    command.blend = blend;
    vertices_.insert(vertices_.end(), triangles.begin(), triangles.end());
    draws_.push_back(stored);
    commands_.push_back(command);
}

void UiBatch::Quad(glm::vec2 min, glm::vec2 max, glm::vec2 uv_min, glm::vec2 uv_max, glm::vec4 color, const UiDrawParams& params, UiShade shade,
                   UiBlend blend) {
    const UiVertex a{min, uv_min, color};
    const UiVertex b{{max.x, min.y}, {uv_max.x, uv_min.y}, color};
    const UiVertex c{max, uv_max, color};
    const UiVertex d{{min.x, max.y}, {uv_min.x, uv_max.y}, color};
    const UiVertex quad[6] = {a, b, c, a, c, d};
    Draw(quad, params, shade, blend);
}

void UiBatch::Record(gpu::CommandList& cmd, gpu::Extent2D extent) {
    RecordRange(cmd, extent, 0, static_cast<uint32_t>(commands_.size()), coverage_);
}

void UiBatch::RecordBelow(gpu::CommandList& cmd, gpu::Extent2D extent) {
    RecordRange(cmd, extent, 0, std::min(top_first_, static_cast<uint32_t>(commands_.size())), coverage_);
}

void UiBatch::RecordTop(gpu::CommandList& cmd, gpu::Extent2D extent) {
    RecordRange(cmd, extent, top_first_, static_cast<uint32_t>(commands_.size()), true);
}

void UiBatch::RecordRange(gpu::CommandList& cmd, gpu::Extent2D extent, uint32_t first, uint32_t last, bool coverage) {
    if (commands_.empty() || !gpu_ || first >= last) {
        return;
    }
    const gpu::Format format = TargetFormat();
    if (format != pipeline_format_) {
        gpu_->WaitIdle();
        if (!CreatePipelines(format)) {
            return;
        }
    }
    FrameData& frame = frames_[renderer_->FrameIndex() % frames_.size()];
    if (!uploaded_) {
        std::memcpy(frame.vertices.mapped, vertices_.data(), vertices_.size() * sizeof(UiVertex));
        std::memcpy(frame.draws.mapped, draws_.data(), draws_.size() * sizeof(UiDrawParams));
        gpu_->FlushMapped(frame.vertices, 0, ~0ull);
        gpu_->FlushMapped(frame.draws, 0, ~0ull);
        uploaded_ = true;
    }
    const gpu::ImageViewHandle scene = renderer_->SceneColor().view;
    const uint64_t destroys = gpu_->ImageDestroyCount();
    if (scene != frame.scene_view || destroys != frame.scene_destroys) {
        const gpu::ImageBinding image_info{scene, gpu::Layout::ShaderReadOnly, clamp_sampler_};
        const gpu::BindingWrite write{frame.set, 1, 0, gpu::BindingType::CombinedImageSampler, {&image_info, 1}};
        gpu_->WriteBindings({&write, 1});
        frame.scene_view = scene;
        frame.scene_destroys = destroys;
    }
    cmd.BeginLabel("ui");
    cmd.SetViewport({{0, 0}, extent});
    const gpu::BindingSetHandle sets[2] = {textures_->BindingSet(), frame.set};
    cmd.BindSets(gpu::BindPoint::Graphics, layout_, 0, sets);
    cmd.BindVertexBuffer(frame.vertices.buffer, 0);
    const bool hdr = renderer_->OutputMode() != RendererOutputMode::Sdr;
    int bound = -1;
    for (uint32_t i = first; i < last; ++i) {
        const Command& command = commands_[i];
        const int additive = command.blend != UiBlend::Alpha ? 1 : 0;
        const int pipeline = coverage ? 2 + additive : hdr ? 4 + additive : additive;
        if (pipeline != bound) {
            cmd.BindPipeline(gpu::BindPoint::Graphics, pipelines_[pipeline]);
            bound = pipeline;
        }
        const Push push{glm::vec2(1.0f / std::max(1u, extent.width), 1.0f / std::max(1u, extent.height)), command.draw};
        cmd.PushConstants(layout_, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment, 0, sizeof(push), &push);
        cmd.Draw(command.count, 1, command.first, 0);
    }
    cmd.EndLabel();
}

}
