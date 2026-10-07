#include "engine/render/subsurface_pass.h"

#include "engine/core/log.h"
#include "engine/render/gpu_types.h"

namespace pt {
namespace {

constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

}

bool SubsurfacePass::Init(vk::Context& ctx, VkDescriptorSetLayout textures_layout, VkDescriptorSetLayout frame_layout, VkFormat light_format,
                          VkSampler point_clamp) {
    ctx_ = &ctx;
    device_ = ctx.device;
    format_ = light_format;
    sampler_ = point_clamp;
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 2;
    layout_info.pBindings = bindings;
    if (!vk::Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &set_layout_), "subsurface set layout")) {
        return false;
    }
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    if (!vk::Check(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_), "subsurface descriptor pool")) {
        return false;
    }
    const VkDescriptorSetLayout layouts[2] = {set_layout_, set_layout_};
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts = layouts;
    if (!vk::Check(vkAllocateDescriptorSets(device_, &alloc, sets_), "subsurface descriptor sets")) {
        return false;
    }
    const VkDescriptorSetLayout sets[3] = {textures_layout, frame_layout, set_layout_};
    const VkPushConstantRange push{kPushStages, 0, 128};
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 3;
    pipeline_layout.pSetLayouts = sets;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push;
    if (!vk::Check(vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &layout_), "subsurface pipeline layout")) {
        return false;
    }
    PipelineDesc desc;
    desc.layout = layout_;
    desc.fragment = "subsurface.frag";
    desc.colors = {format_};
    pipeline_ = CreateGraphicsPipeline(device_, desc);
    if (!pipeline_) {
        LogError("subsurface: pipeline failed");
        return false;
    }
    return true;
}

void SubsurfacePass::Shutdown() {
    if (!device_) {
        return;
    }
    DestroyTargets();
    if (pipeline_) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (layout_) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
    if (pool_) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
    }
    if (set_layout_) {
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    sets_[0] = sets_[1] = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

bool SubsurfacePass::CreateTargets(VkExtent2D extent) {
    DestroyTargets();
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkExtent3D size{std::max(extent.width, 1u), std::max(extent.height, 1u), 1};
    if (!ctx_->CreateImage(copy_.image, format_, size, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, 1,
                           VK_IMAGE_ASPECT_COLOR_BIT) ||
        !ctx_->CreateImage(temp_.image, format_, size, usage, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT)) {
        LogError("subsurface: cannot create targets {}x{}", extent.width, extent.height);
        return false;
    }
    ctx_->Submit([&](VkCommandBuffer cmd) {
        UseTargets(cmd, {{&copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&temp_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    });
    const VkDescriptorImageInfo copy{sampler_, copy_.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo temp{sampler_, temp_.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo* infos[4] = {&copy, &copy, &copy, &temp};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = sets_[i / 2];
        writes[i].dstBinding = i % 2;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = infos[i];
    }
    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
    return true;
}

void SubsurfacePass::DestroyTargets() {
    if (!ctx_) {
        return;
    }
    ctx_->DestroyImage(copy_.image);
    ctx_->DestroyImage(temp_.image);
    copy_ = RenderTarget{};
    temp_ = RenderTarget{};
}

void SubsurfacePass::Record(VkCommandBuffer cmd, RenderTarget& diffuse, uint32_t view_index, const std::function<void()>& bind_sets) {
    if (!pipeline_ || !copy_.Valid()) {
        return;
    }
    UseTargets(cmd, {{&diffuse, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&copy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {copy_.Extent().width, copy_.Extent().height, 1};
    vkCmdCopyImage(cmd, diffuse.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);
    UseTargets(cmd, {{&diffuse, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    struct Step {
        RenderTarget* target;
        uint32_t mode;
    };
    const Step steps[2] = {{&temp_, 1u}, {&diffuse, 2u}};
    for (const Step& step : steps) {
        UseTargets(cmd, {{step.target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, step.target->Extent(), {{step.target, false, {}}});
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        bind_sets();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &sets_[step.mode - 1], 0, nullptr);
        gpu::PassPush push;
        push.ids = glm::uvec4(view_index, step.mode, 0, 0);
        vkCmdPushConstants(cmd, layout_, kPushStages, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{step.target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    }
}

}
