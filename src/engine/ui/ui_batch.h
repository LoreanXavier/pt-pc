#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/vk.h"

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

    void Begin(VkExtent2D extent);
    void Draw(std::span<const UiVertex> triangles, const UiDrawParams& params, UiShade shade, UiBlend blend);
    void Quad(glm::vec2 min, glm::vec2 max, glm::vec2 uv_min, glm::vec2 uv_max, glm::vec4 color, const UiDrawParams& params, UiShade shade,
              UiBlend blend);
    void Record(VkCommandBuffer cmd, VkExtent2D extent);

    VkExtent2D Extent() const { return extent_; }
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
        vk::Buffer vertices;
        vk::Buffer draws;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkImageView scene_view = VK_NULL_HANDLE;
    };

    VkFormat TargetFormat() const;
    bool CreatePipelines(VkFormat format);
    void DestroyPipelines();

    Renderer* renderer_ = nullptr;
    TextureManager* textures_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler clamp_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool coverage_ = false;
    VkFormat pipeline_format_ = VK_FORMAT_UNDEFINED;
    std::vector<FrameData> frames_;
    std::vector<UiVertex> vertices_;
    std::vector<UiDrawParams> draws_;
    std::vector<Command> commands_;
    VkExtent2D extent_{};
    glm::vec4 screen_{0.0f};
};

}
