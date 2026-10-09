#pragma once

#include <initializer_list>
#include <span>
#include <vector>

#include "engine/render/vk.h"

namespace pt {

struct RenderTarget {
    vk::Image image;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    bool Valid() const { return image.image != VK_NULL_HANDLE; }
    VkExtent2D Extent() const { return {image.extent.width, image.extent.height}; }
};

struct TargetUse {
    RenderTarget* target = nullptr;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

void UseTargets(VkCommandBuffer cmd, std::initializer_list<TargetUse> uses);

struct ColorOutput {
    RenderTarget* target = nullptr;
    bool clear = false;
    VkClearColorValue clear_value{};
};

void BeginPass(VkCommandBuffer cmd, VkExtent2D extent, std::initializer_list<ColorOutput> colors, RenderTarget* depth = nullptr,
               bool depth_read_only = false, bool clear_depth = false);
void BeginPass(VkCommandBuffer cmd, VkRect2D area, std::span<const ColorOutput> colors, RenderTarget* depth, bool depth_read_only, bool clear_depth);
void SetViewport(VkCommandBuffer cmd, VkRect2D area);
// also a VK_NV_device_diagnostic_checkpoints checkpoint when g_checkpoints is set (Context::checkpoints); name must outlive the frame
void BeginLabel(VkCommandBuffer cmd, const char* name);
extern bool g_checkpoints;
void EndLabel(VkCommandBuffer cmd);

enum class BlendMode : uint8_t { None, Alpha, PremultipliedFadeAlpha, Additive, ProbeLerp, ProbeAccumulate };

struct PipelineDesc {
    const char* vertex = "fullscreen.vert";
    const char* fragment = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkFormat> colors;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    bool mesh_input = false;
    bool depth_test = false;
    bool depth_write = false;
    VkCompareOp depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    bool depth_bias = false;
    BlendMode blend = BlendMode::None;
    // per colour attachment in place of blend where given
    std::vector<BlendMode> blends;
    VkColorComponentFlags write_mask = 0xF;
    std::vector<VkColorComponentFlags> write_masks;
};

VkPipeline CreateGraphicsPipeline(VkDevice device, const PipelineDesc& desc);
VkPipeline CreateComputePipeline(VkDevice device, VkPipelineLayout layout, const char* shader);

}
