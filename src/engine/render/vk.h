#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <span>
#include <vector>

namespace pt::vk {

const char* ResultName(VkResult result);
bool Check(VkResult result, const char* what);

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    uint32_t mip_levels = 1;
    uint32_t layers = 1;
    VkImageUsageFlags usage = 0;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkImageLayout old_layout,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout new_layout,
                  uint32_t mip_levels = VK_REMAINING_MIP_LEVELS, uint32_t layers = VK_REMAINING_ARRAY_LAYERS);

VkShaderModule LoadShaderModule(VkDevice device, const char* file_name);

}
