#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/render/renderer.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/texture_manager.h"
#include "engine/vfx/vfx_system.h"

namespace pt {

class Vfs;

class VfxPass {
public:
    bool Init(Renderer& renderer, TextureManager& textures, Vfs& vfs);
    void Shutdown();

    uint32_t Texture(const std::string& path);
    // queues the textures not loaded yet for TextureManager::DecodeAhead
    void DecodeAhead(const std::vector<std::string>& paths);
    // cube slot of a liquid's reflection cube, vfx::kNoTexture when the file is missing or not a cube map
    uint32_t Cube(const std::string& path);
    // takes the list and hands back the previous one, cleared with its capacity kept, for the next frame's build
    void Submit(vfx::RenderList& list);
    void RecordForward(const SceneVfxContext& context);
    void RecordFilter(const SceneFilterContext& context);
    const vfx::RenderList& List() const { return list_; }

private:
    struct PipelineKey {
        VkFormat color = VK_FORMAT_UNDEFINED;
        VkFormat depth = VK_FORMAT_UNDEFINED;
        uint8_t blend = 0;
        bool depth_test = false;
        bool cull = false;
        // effect buffer draw: alpha is the transmittance (SceneVfxContext::subset kVfxOffscreen)
        bool offscreen = false;
        bool operator==(const PipelineKey&) const = default;
    };

    struct FrameSlot {
        vk::Buffer quads;
        // fog block of the forward set at 0 and of the filter set at kFogStride
        vk::Buffer fog;
        VkDescriptorSet forward = VK_NULL_HANDLE;
        VkDescriptorSet filter = VK_NULL_HANDLE;
        VkDescriptorSet effects = VK_NULL_HANDLE;
        VkImageView forward_view = VK_NULL_HANDLE;
        VkImageView filter_view = VK_NULL_HANDLE;
        VkImageView effects_depth_view = VK_NULL_HANDLE;
        VkImageView forward_scene = VK_NULL_HANDLE;
        VkImageView filter_scene = VK_NULL_HANDLE;
        VkImageView effects_scene = VK_NULL_HANDLE;
        uint64_t uploaded = 0;
    };

    VkPipeline Pipeline(const PipelineKey& key);
    bool Upload(uint32_t frame_index);
    void WriteSet(FrameSlot& slot, VkDescriptorSet set, VkImageView view, VkImageLayout depth_layout, VkImageView scene, VkDeviceSize fog_offset);
    uint32_t DrawLayer(VkCommandBuffer cmd, VkDescriptorSet set, vfx::Layer layer, VkFormat color, VkFormat depth, bool depth_test, uint32_t mode,
                       const glm::mat4& view_projection, const glm::vec3& eye, float exposure, float near_plane, VkExtent2D extent,
                       const std::function<void()>* copy_scene, uint32_t subset = kVfxAll, uint32_t* recorded = nullptr);

    Renderer* renderer_ = nullptr;
    TextureManager* textures_ = nullptr;
    Vfs* vfs_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSampler scene_sampler_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkShaderModule fragment_ = VK_NULL_HANDLE;
    std::vector<std::pair<PipelineKey, VkPipeline>> pipelines_;
    FrameSlot slots_[Renderer::kFramesInFlight];
    std::unordered_map<std::string, uint32_t> texture_cache_;
    std::unordered_map<std::string, uint32_t> cube_cache_;
    vfx::RenderList list_;
    std::vector<uint32_t> order_;
    uint64_t generation_ = 1;
};

}
