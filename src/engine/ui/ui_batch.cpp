#include "engine/ui/ui_batch.h"

#include <algorithm>
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
    device_ = renderer.Context().device;
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!vk::Check(vkCreateSampler(device_, &sampler_info, nullptr, &clamp_sampler_), "ui sampler")) {
        return false;
    }
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    if (!vk::Check(vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &set_layout_), "ui set layout")) {
        return false;
    }
    const uint32_t frame_count = Renderer::kFramesInFlight;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame_count}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frame_count}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = frame_count;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    if (!vk::Check(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_), "ui pool")) {
        return false;
    }
    frames_.resize(frame_count);
    for (FrameData& frame : frames_) {
        if (!renderer.Context().CreateBuffer(frame.vertices, sizeof(UiVertex) * kMaxVertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true) ||
            !renderer.Context().CreateBuffer(frame.draws, sizeof(UiDrawParams) * kMaxDraws, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) {
            return false;
        }
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &set_layout_;
        if (!vk::Check(vkAllocateDescriptorSets(device_, &alloc, &frame.set), "ui set")) {
            return false;
        }
        VkDescriptorBufferInfo buffer_info{frame.draws.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = frame.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    VkDescriptorSetLayout layouts[2] = {textures.SetLayout(), set_layout_};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (!vk::Check(vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_), "ui pipeline layout")) {
        return false;
    }
    return CreatePipelines(TargetFormat());
}

VkFormat UiBatch::TargetFormat() const {
    const VkFormat swapchain = renderer_->Context().swapchain.format;
    return swapchain != VK_FORMAT_UNDEFINED ? swapchain : VK_FORMAT_R8G8B8A8_UNORM;
}

bool UiBatch::CreatePipelines(VkFormat format) {
    DestroyPipelines();
    VkShaderModule vert = vk::LoadShaderModule(device_, "ui_sprite.vert");
    VkShaderModule frag = vk::LoadShaderModule(device_, "ui_sprite.frag");
    if (!vert || !frag) {
        if (vert) {
            vkDestroyShaderModule(device_, vert, nullptr);
        }
        if (frag) {
            vkDestroyShaderModule(device_, frag, nullptr);
        }
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        const bool additive = (i & 1) != 0;
        const bool coverage = i >= 2;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = !additive ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = coverage && !additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        blend_attachment.dstAlphaBlendFactor = coverage && !additive ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout_;
        ok = ok && vk::Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipelines_[i]), "ui pipeline");
    }
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    pipeline_format_ = format;
    return ok;
}

void UiBatch::DestroyPipelines() {
    for (VkPipeline& pipeline : pipelines_) {
        if (pipeline) {
            vkDestroyPipeline(device_, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
}

void UiBatch::Shutdown() {
    if (!device_) {
        return;
    }
    DestroyPipelines();
    for (FrameData& frame : frames_) {
        renderer_->Context().DestroyBuffer(frame.vertices);
        renderer_->Context().DestroyBuffer(frame.draws);
    }
    frames_.clear();
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    vkDestroySampler(device_, clamp_sampler_, nullptr);
    device_ = VK_NULL_HANDLE;
}

void UiBatch::Begin(VkExtent2D extent) {
    extent_ = extent;
    vertices_.clear();
    draws_.clear();
    commands_.clear();
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

void UiBatch::Record(VkCommandBuffer cmd, VkExtent2D extent) {
    if (commands_.empty() || !device_) {
        return;
    }
    const VkFormat format = TargetFormat();
    if (format != pipeline_format_) {
        vkDeviceWaitIdle(device_);
        if (!CreatePipelines(format)) {
            return;
        }
    }
    FrameData& frame = frames_[renderer_->FrameIndex() % frames_.size()];
    std::memcpy(frame.vertices.mapped, vertices_.data(), vertices_.size() * sizeof(UiVertex));
    std::memcpy(frame.draws.mapped, draws_.data(), draws_.size() * sizeof(UiDrawParams));
    vmaFlushAllocation(renderer_->Context().allocator, frame.vertices.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(renderer_->Context().allocator, frame.draws.allocation, 0, VK_WHOLE_SIZE);
    const VkImageView scene = renderer_->SceneColor().view;
    if (scene != frame.scene_view) {
        VkDescriptorImageInfo image_info{clamp_sampler_, scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = frame.set;
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        frame.scene_view = scene;
    }
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    VkDescriptorSet sets[2] = {textures_->Set(), frame.set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &frame.vertices.buffer, &offset);
    int bound = -1;
    for (const Command& command : commands_) {
        const int pipeline = (command.blend == UiBlend::Alpha ? 0 : 1) + (coverage_ ? 2 : 0);
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[pipeline]);
            bound = pipeline;
        }
        const Push push{glm::vec2(1.0f / std::max(1u, extent.width), 1.0f / std::max(1u, extent.height)), command.draw};
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, command.count, 1, command.first, 0);
    }
}

}
