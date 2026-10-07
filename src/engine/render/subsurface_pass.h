#pragma once

#include <functional>

#include "engine/render/render_util.h"
#include "engine/render/vk_context.h"

namespace pt {

class SubsurfacePass {
public:
    bool Init(vk::Context& ctx, VkDescriptorSetLayout textures_layout, VkDescriptorSetLayout frame_layout, VkFormat light_format,
              VkSampler point_clamp);
    void Shutdown();
    bool CreateTargets(VkExtent2D extent);
    void DestroyTargets();
    void Record(VkCommandBuffer cmd, RenderTarget& diffuse, uint32_t view_index, const std::function<void()>& bind_sets);

private:
    vk::Context* ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[2] = {};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    RenderTarget copy_;
    RenderTarget temp_;
};

}
