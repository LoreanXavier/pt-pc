#include "engine/assets/enhanced_textures.h"
#include "engine/render/texture_manager.h"
#include "engine/render/vk_context.h"
#include "engine/render/vk.h"
#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 5) { std::fprintf(stderr, "usage: pt_texture_descriptor_test <game> <runtime> <cache> <texture-stem>\n"); return 2; }
    int failures = 0;
    auto check = [&](bool ok, const char* name) { if (!ok) { std::printf("FAIL: %s\n", name); ++failures; } };
    pt::QarArchive qar;
    if (!qar.Open(std::filesystem::path(argv[1]) / "texture.qar")) return 2;
    pt::FtexTexture source;
    if (!pt::LoadFtex(qar, argv[4], source)) return 2;
    pt::vk::Context ctx;
    if (!ctx.Init(nullptr, true)) return 2;
    pt::TextureManager textures;
    if (!textures.Init(ctx)) return 2;
    textures.ConfigureEnhancedTextures(qar, argv[3], pt::EnhancedModelKey(argv[2]));
    bool loaded = false;
    const uint32_t index = textures.LoadFox(qar, argv[4], &loaded);
    check(loaded, "original uploaded");
    pt::vk::Buffer buffer;
    if (!ctx.CreateBuffer(buffer, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return 2;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1; set_info.pBindings = &binding;
    VkDescriptorSetLayout result_layout;
    vkCreateDescriptorSetLayout(ctx.device, &set_info, nullptr, &result_layout);
    const VkDescriptorSetLayout layouts[] = {textures.SetLayout(), result_layout};
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 2; layout_info.pSetLayouts = layouts;
    layout_info.pushConstantRangeCount = 1; layout_info.pPushConstantRanges = &push;
    VkPipelineLayout layout;
    vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &layout);
    std::ifstream shader(PT_TEXTURE_DESCRIPTOR_SHADER, std::ios::binary | std::ios::ate);
    const size_t shader_size = static_cast<size_t>(shader.tellg());
    std::vector<uint32_t> code(shader_size / 4);
    shader.seekg(0); shader.read(reinterpret_cast<char*>(code.data()), shader_size);
    if (!shader) return 2;
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = shader_size; module_info.pCode = code.data();
    VkShaderModule module;
    vkCreateShaderModule(ctx.device, &module_info, nullptr, &module);
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
    vkCreateDescriptorPool(ctx.device, &pool_info, nullptr, &pool);
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &result_layout;
    VkDescriptorSet result;
    vkAllocateDescriptorSets(ctx.device, &allocation, &result);
    VkDescriptorBufferInfo buffer_info{buffer.buffer, 0, 32};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = result; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
    auto sample = [&](uint32_t scale, const char* name) {
        ctx.Submit([&](VkCommandBuffer cmd) {
            const VkDescriptorSet sets[] = {textures.Set(), result};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 0, nullptr);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &index);
            vkCmdDispatch(cmd, 1, 1, 1);
            VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; memory.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            memory.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; memory.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &memory;
            vkCmdPipelineBarrier2(cmd, &dep);
        });
        vmaInvalidateAllocation(ctx.allocator, buffer.allocation, 0, 32);
        const auto* size = static_cast<const uint32_t*>(buffer.mapped);
        check(size[0] == source.width * scale && size[1] == source.height * scale, name);
        std::printf("%s: %u x %u\n", name, size[0], size[1]);
        check(textures.Find(argv[4]) == index, "material texture index stable");
    };
    sample(1, "original descriptor");
    textures.SetEnhancedTextures(true); sample(2, "enhanced descriptor");
    textures.SetAnisotropy(16); sample(2, "enhanced descriptor after anisotropy");
    textures.ConfigureEnhancedTextures(qar, argv[3], pt::EnhancedModelKey(argv[2]));
    sample(2, "same texture configuration keeps enhanced descriptors active");
    textures.SetEnhancedTextures(false); sample(1, "original descriptor restored");
    textures.SetEnhancedTextures(true); sample(2, "enhanced descriptor restored");
    vkDeviceWaitIdle(ctx.device);
    vkDestroyPipeline(ctx.device, pipeline, nullptr); vkDestroyShaderModule(ctx.device, module, nullptr);
    vkDestroyDescriptorPool(ctx.device, pool, nullptr); vkDestroyPipelineLayout(ctx.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, result_layout, nullptr);
    ctx.DestroyBuffer(buffer); textures.Shutdown(); ctx.Shutdown();
    std::printf("texture descriptors: %d failures\n", failures);
    return failures ? 1 : 0;
}
