#include "engine/render/vk_context.h"
#include "engine/render/vk.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

int main() {
    pt::vk::Context ctx;
    if (!ctx.Init(nullptr, true)) return 2;
    pt::vk::Buffer buffer;
    constexpr uint32_t bytes = 29 * 4 * sizeof(float);
    if (!ctx.CreateBuffer(buffer, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return 2;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1; set_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout;
    if (vkCreateDescriptorSetLayout(ctx.device, &set_info, nullptr, &set_layout) != VK_SUCCESS) return 2;
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1; layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &layout) != VK_SUCCESS) return 2;
    std::ifstream shader(PT_REFLECTION_MIX_SHADER, std::ios::binary | std::ios::ate);
    if (!shader) return 2;
    const size_t shader_size = static_cast<size_t>(shader.tellg());
    std::vector<uint32_t> code(shader_size / 4);
    shader.seekg(0); shader.read(reinterpret_cast<char*>(code.data()), shader_size);
    if (!shader) return 2;
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = shader_size; module_info.pCode = code.data();
    VkShaderModule module;
    if (vkCreateShaderModule(ctx.device, &module_info, nullptr, &module) != VK_SUCCESS) return 2;
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline_info.stage.module = module; pipeline_info.stage.pName = "main";
    pipeline_info.layout = layout;
    VkPipeline pipeline;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) return 2;
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1; pool_info.poolSizeCount = 1; pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool;
    if (vkCreateDescriptorPool(ctx.device, &pool_info, nullptr, &pool) != VK_SUCCESS) return 2;
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &set_layout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(ctx.device, &allocation, &set) != VK_SUCCESS) return 2;
    VkDescriptorBufferInfo buffer_info{buffer.buffer, 0, bytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
    ctx.Submit([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; memory.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; memory.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    vmaInvalidateAllocation(ctx.allocator, buffer.allocation, 0, bytes);
    const std::array<std::array<float, 4>, 29> expected{{
        {0.7f, 0.4f, 0.0f, 0.0f}, {2.0f / 3.0f, 1.0f / 3.0f, 0.0f, 0.3f},
        {0.2f, 0.4f, 0.6f, 0.8f}, {0.1f, 0.2f, 0.3f, 0.3f}, {0.0f, 0.0f, 0.0f, 0.0f},
        {0.6f, 0.0f, 0.8f, 0.0f}, {0.0f, -0.6f, 0.8f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {2.4f, 1.6f, 5.0f, 1.0f}, {0.44f, -0.38f, 0.0f, 0.0f},
        {.010175f, 0, 0, 0}, {.01f, 0, 0, 0}, {.01f, 0, 0, 0}, {.01f, 0, 0, 0},
        {1, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {.29f, .48f, .31f, 0}, {.2f, .2f, .2f, 0},
        { .49f, -.48f, 0, 0 }, { .71875f, 0, 0, 0 }, { .6875f, 0, 0, 0 }, { .6875f, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
        {.01175f, 0, 0, 0}, {.01f, 0, 0, 0}, {.01f, 0, 0, 0}

    }};
    const char* names[] = {"recover screen coordinate at half coverage", "blend both reflection sources", "screen only", "traced only", "no hit finite", "mapped tangent detail", "mirrored bitangent detail", "flat map preserves geometry", "invalid map does not distort reflection", "reconstruct jittered depth location", "project into jittered scene", "continuous planar reversed depth", "native point depth retained", "depth silhouette not interpolated", "sky boundary not interpolated", "mirror matching depth accepted", "mirror disocclusion rejected", "mirror empty history rejected", "mirror history clipped to current neighborhood", "mirror exposure compensated", "beam sample matches jittered surface", "near wall negative X beam", "positive X beam", "negative Z beam", "outside beam unchanged", "missing light unchanged", "planar footprint interpolates", "crease keeps the texel depth", "far step keeps the texel depth"};
    const auto* actual = static_cast<const float*>(buffer.mapped);
    int failures = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        bool good = true;
        for (size_t c = 0; c < 4; ++c) good &= std::isfinite(actual[i * 4 + c]) && std::abs(actual[i * 4 + c] - expected[i][c]) < 1e-5f;
        std::printf("%s: %s (%g %g %g %g)\n", good ? "PASS" : "FAIL", names[i], actual[i*4], actual[i*4+1], actual[i*4+2], actual[i*4+3]);
        failures += !good;
    }
    vkDeviceWaitIdle(ctx.device);
    vkDestroyPipeline(ctx.device, pipeline, nullptr); vkDestroyShaderModule(ctx.device, module, nullptr);
    vkDestroyDescriptorPool(ctx.device, pool, nullptr); vkDestroyPipelineLayout(ctx.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, set_layout, nullptr);
    ctx.DestroyBuffer(buffer); ctx.Shutdown();
    std::printf("reflection mix: %d failures\n", failures);
    return failures ? 1 : 0;
}
