#pragma once

#include <functional>

#include "engine/render/render_util.h"
#include "engine/render/vk_context.h"

namespace pt {

// The SUBSURFACE_SCATTER render plugin (GrPluginSubSurfaceScatter, execute 0xDDBEA0), rendering.md 12.26: CopyBuffer of the
// diffuse light buffer, then two SubSurfaceScattering passes (horizontal into a temporary target, vertical back into the
// light buffer, mixed with the copy). The floor environment (0x922C60) enables it on the main view only on the ending.
class SubsurfacePass {
public:
    // sets 0 and 1 as the scene layout's (the scene's bound sets stay valid), set 2 the copy and the temporary target
    bool Init(vk::Context& ctx, VkDescriptorSetLayout textures_layout, VkDescriptorSetLayout frame_layout, VkFormat light_format,
              VkSampler point_clamp);
    void Shutdown();
    bool CreateTargets(VkExtent2D extent);
    void DestroyTargets();
    // between the lighting and the composition; diffuse ends as a shader read target. bind_sets binds sets 0 and 1.
    void Record(VkCommandBuffer cmd, RenderTarget& diffuse, uint32_t view_index, const std::function<void()>& bind_sets);

private:
    vk::Context* ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    // [0]: (copy, copy) for the horizontal pass, [1]: (copy, temporary) for the vertical one
    VkDescriptorSet sets_[2] = {};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    RenderTarget copy_;
    RenderTarget temp_;
};

}
