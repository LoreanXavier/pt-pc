#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/gpu/gpu_device.h"

namespace pt {
class Renderer;
class TextureManager;
}

namespace pt::ui {

enum class UiBlend : uint8_t { Alpha, Additive };

enum class UiShade : uint32_t { Material = 0, Text = 1, Noise = 2, String = 3, Solid = 4, Border = 5 };

struct UiVertex {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec4 color;
};

constexpr uint32_t kUiClampU = 0x80000000u;
constexpr uint32_t kUiClampV = 0x40000000u;

struct UiDrawParams {
    std::array<uint32_t, 4> textures{0, 0, 0, 0};
    std::array<glm::vec4, 7> material{};
    glm::vec4 screen{0.0f};
    glm::vec4 extra{0.0f};
    glm::vec4 extra2{0.0f};

    static UiDrawParams Plain(uint32_t texture);
    static UiDrawParams FromMaterial(const std::array<uint32_t, 4>& textures, const std::array<float, 28>& params);
};

class UiBatch {
public:
    static constexpr uint32_t kMaxVertices = 65536;
    static constexpr uint32_t kMaxDraws = 2048;

    bool Init(Renderer& renderer, TextureManager& textures);
    void Shutdown();

    void Begin(gpu::Extent2D extent);
    void MarkTop() { top_first_ = static_cast<uint32_t>(commands_.size()); }
    bool HasTop() const { return top_first_ < commands_.size(); }
    void Draw(std::span<const UiVertex> triangles, const UiDrawParams& params, UiShade shade, UiBlend blend);
    void Quad(glm::vec2 min, glm::vec2 max, glm::vec2 uv_min, glm::vec2 uv_max, glm::vec4 color, const UiDrawParams& params, UiShade shade,
              UiBlend blend);
    void Record(gpu::CommandList& cmd, gpu::Extent2D extent);
    void RecordBelow(gpu::CommandList& cmd, gpu::Extent2D extent);
    void RecordTop(gpu::CommandList& cmd, gpu::Extent2D extent);

    gpu::Extent2D Extent() const { return extent_; }
    void SetCoverageAlpha(bool enabled) { coverage_ = enabled; }
    void SetScreenArea(glm::vec2 origin, float height) { screen_ = glm::vec4(origin, 1.0f / std::max(height, 1.0f), 0.0f); }

private:
    struct Command {
        uint32_t first = 0;
        uint32_t count = 0;
        uint32_t draw = 0;
        UiBlend blend = UiBlend::Alpha;
    };
    struct FrameData {
        gpu::BufferResource vertices;
        gpu::BufferResource draws;
        gpu::BindingSetHandle set;
        gpu::ImageViewHandle scene_view;
        uint64_t scene_destroys = 0;
    };

    void RecordRange(gpu::CommandList& cmd, gpu::Extent2D extent, uint32_t first, uint32_t last, bool coverage);
    gpu::Format TargetFormat() const;
    bool CreatePipelines(gpu::Format format);
    void DestroyPipelines();

    Renderer* renderer_ = nullptr;
    TextureManager* textures_ = nullptr;
    gpu::Device* gpu_ = nullptr;
    gpu::SamplerHandle clamp_sampler_;
    gpu::BindingLayoutHandle set_layout_;
    gpu::BindingPoolHandle pool_;
    gpu::PipelineLayoutHandle layout_;
    gpu::PipelineHandle pipelines_[6] = {};
    bool coverage_ = false;
    uint32_t top_first_ = UINT32_MAX;
    bool uploaded_ = false;
    gpu::Format pipeline_format_ = gpu::Format::Undefined;
    std::vector<FrameData> frames_;
    std::vector<UiVertex> vertices_;
    std::vector<UiDrawParams> draws_;
    std::vector<Command> commands_;
    gpu::Extent2D extent_{};
    glm::vec4 screen_{0.0f};
};

}
