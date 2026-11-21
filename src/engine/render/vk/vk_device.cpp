#include "engine/render/vk/vk_device.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include "engine/core/log.h"
#include "engine/render/pipeline_cache_store.h"
#include "engine/render/render_util.h"

namespace pt::gpu::vk {

BufferResource ToGpu(const pt::vk::Buffer& buffer) {
    BufferResource out;
    out.buffer = Wrap<BufferTag>(buffer.buffer);
    out.allocation = Wrap<AllocationTag>(buffer.allocation);
    out.mapped = buffer.mapped;
    out.size = buffer.size;
    return out;
}

pt::vk::Buffer FromGpu(const BufferResource& buffer) {
    pt::vk::Buffer out;
    out.buffer = Native(buffer.buffer);
    out.allocation = Native(buffer.allocation);
    out.mapped = buffer.mapped;
    out.size = buffer.size;
    return out;
}

Device::Device(pt::vk::Context& ctx) : ctx_(ctx) {
    info_.backend = Backend::Vulkan;
    info_.adapter = ctx.properties.deviceName;
    info_.vendor_id = ctx.properties.vendorID;
    info_.device_id = ctx.properties.deviceID;
    info_.dedicated_video_memory = ctx.allocator ? ctx.DeviceLocalBytes() : 0;
    info_.ray_query = ctx.ray_query;
    info_.depth_bounds = ctx.depth_bounds;
    info_.dynamic_depth_bias = true;
    info_.timestamp_period_ns = ctx.properties.limits.timestampPeriod;
    info_.max_sampler_anisotropy = ctx.properties.limits.maxSamplerAnisotropy;
}

bool Device::CreateImage(const ImageDesc& desc, ImageResource& out) {
    std::optional<pt::vk::AllocTag> tag;
    if (desc.name) {
        tag.emplace(desc.name);
    }
    return ctx_.CreateImage(out, Native(desc.format), {desc.extent.width, desc.extent.height, std::max(desc.extent.depth, 1u)}, desc.usage.bits,
                            desc.mip_levels, desc.layers, desc.aspect.bits, desc.cube);
}

void Device::DestroyImage(ImageResource& image) {
    if (image.image || image.view) {
        NoteImageDestroyed();
    }
    ctx_.DestroyImage(image);
}

bool Device::CreateBuffer(const BufferDesc& desc, BufferResource& out) {
    std::optional<pt::vk::AllocTag> tag;
    if (desc.name) {
        tag.emplace(desc.name);
    }
    pt::vk::Buffer buffer;
    const bool ok = ctx_.CreateBuffer(buffer, desc.size, desc.usage.bits, desc.host_visible || desc.readback);
    out = ToGpu(buffer);
    return ok;
}

void Device::DestroyBuffer(BufferResource& buffer) {
    pt::vk::Buffer native = FromGpu(buffer);
    if (native.buffer) {
        ctx_.DestroyBuffer(native);
    }
    buffer = ToGpu(native);
}

void Device::FlushMapped(const BufferResource& buffer, uint64_t offset, uint64_t size) {
    vmaFlushAllocation(ctx_.allocator, Native(buffer.allocation), offset, size);
}

void Device::InvalidateMapped(const BufferResource& buffer, uint64_t offset, uint64_t size) {
    vmaInvalidateAllocation(ctx_.allocator, Native(buffer.allocation), offset, size);
}

void Device::DeviceLocalUsage(double& used, double& budget) const {
    ctx_.DeviceLocalUsage(used, budget);
}

uint64_t Device::AllocatedBytes() const {
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(ctx_.allocator, budgets);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(ctx_.allocator, &memory);
    uint64_t bytes = 0;
    for (uint32_t h = 0; memory && h < memory->memoryHeapCount; ++h) {
        bytes += budgets[h].statistics.blockBytes;
    }
    return bytes;
}

uint64_t Device::ImageBytes(const ImageResource& image) const {
    if (!image.allocation) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(ctx_.allocator, Native(image.allocation), &info);
    return info.size;
}

SamplerHandle Device::CreateSampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = static_cast<VkFilter>(desc.mag);
    info.minFilter = static_cast<VkFilter>(desc.min);
    info.mipmapMode = static_cast<VkSamplerMipmapMode>(desc.mipmap);
    info.addressModeU = static_cast<VkSamplerAddressMode>(desc.address_u);
    info.addressModeV = static_cast<VkSamplerAddressMode>(desc.address_v);
    info.addressModeW = static_cast<VkSamplerAddressMode>(desc.address_w);
    info.anisotropyEnable = desc.max_anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = desc.max_anisotropy;
    info.compareEnable = desc.compare ? VK_TRUE : VK_FALSE;
    info.compareOp = desc.compare ? Native(desc.compare_op) : VK_COMPARE_OP_NEVER;
    info.minLod = desc.min_lod;
    info.maxLod = desc.max_lod;
    info.borderColor = static_cast<VkBorderColor>(desc.border);
    VkSampler sampler = VK_NULL_HANDLE;
    if (!pt::vk::Check(vkCreateSampler(ctx_.device, &info, nullptr, &sampler), "vkCreateSampler")) {
        return {};
    }
    return Wrap<SamplerTag>(sampler);
}

void Device::DestroySampler(SamplerHandle sampler) {
    if (sampler) {
        vkDestroySampler(ctx_.device, Native(sampler), nullptr);
    }
}

BindingLayoutHandle Device::CreateBindingLayout(const BindingLayoutDesc& desc) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorBindingFlags> flags;
    bool any_flags = false;
    for (const BindingDesc& b : desc.bindings) {
        bindings.push_back({b.binding, Native(b.type), b.count, b.stages.bits, nullptr});
        flags.push_back(b.flags.bits);
        any_flags = any_flags || b.flags.bits != 0;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flag_info.bindingCount = static_cast<uint32_t>(flags.size());
    flag_info.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.pNext = any_flags ? &flag_info : nullptr;
    info.flags = desc.update_after_bind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (!pt::vk::Check(vkCreateDescriptorSetLayout(ctx_.device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout")) {
        return {};
    }
    return Wrap<BindingLayoutTag>(layout);
}

void Device::DestroyBindingLayout(BindingLayoutHandle layout) {
    if (layout) {
        vkDestroyDescriptorSetLayout(ctx_.device, Native(layout), nullptr);
    }
}

BindingPoolHandle Device::CreateBindingPool(const BindingPoolDesc& desc) {
    std::vector<VkDescriptorPoolSize> sizes;
    for (const BindingPoolSize& s : desc.sizes) {
        sizes.push_back({Native(s.type), s.count});
    }
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.flags = desc.update_after_bind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
    info.maxSets = desc.max_sets;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!pt::vk::Check(vkCreateDescriptorPool(ctx_.device, &info, nullptr, &pool), "vkCreateDescriptorPool")) {
        return {};
    }
    return Wrap<BindingPoolTag>(pool);
}

void Device::DestroyBindingPool(BindingPoolHandle pool) {
    if (pool) {
        vkDestroyDescriptorPool(ctx_.device, Native(pool), nullptr);
    }
}

BindingSetHandle Device::AllocateBindingSet(BindingPoolHandle pool, BindingLayoutHandle layout, uint32_t variable_count) {
    const VkDescriptorSetLayout native = Native(layout);
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    variable.descriptorSetCount = 1;
    variable.pDescriptorCounts = &variable_count;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.pNext = variable_count ? &variable : nullptr;
    alloc.descriptorPool = Native(pool);
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &native;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!pt::vk::Check(vkAllocateDescriptorSets(ctx_.device, &alloc, &set), "vkAllocateDescriptorSets")) {
        return {};
    }
    return Wrap<BindingSetTag>(set);
}

bool Device::AllocateBindingSets(BindingPoolHandle pool, std::span<const BindingLayoutHandle> layouts, std::span<BindingSetHandle> out) {
    const size_t count = std::min(layouts.size(), out.size());
    std::vector<VkDescriptorSetLayout> native(count);
    std::vector<VkDescriptorSet> sets(count, VK_NULL_HANDLE);
    for (size_t i = 0; i < count; ++i) {
        native[i] = Native(layouts[i]);
    }
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = Native(pool);
    alloc.descriptorSetCount = static_cast<uint32_t>(count);
    alloc.pSetLayouts = native.data();
    const bool ok = pt::vk::Check(vkAllocateDescriptorSets(ctx_.device, &alloc, sets.data()), "vkAllocateDescriptorSets");
    for (size_t i = 0; i < count; ++i) {
        out[i] = ok ? Wrap<BindingSetTag>(sets[i]) : BindingSetHandle{};
    }
    return ok;
}

void Device::WriteBindings(std::span<const BindingWrite> writes) {
    size_t image_count = 0;
    size_t buffer_count = 0;
    for (const BindingWrite& w : writes) {
        image_count += w.images.size() + w.samplers.size();
        buffer_count += w.buffers.size();
    }
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> structures;
    std::vector<VkAccelerationStructureKHR> structure_handles;
    images.reserve(image_count);
    buffers.reserve(buffer_count);
    structures.reserve(writes.size());
    structure_handles.reserve(writes.size());
    std::vector<VkWriteDescriptorSet> native;
    native.reserve(writes.size());
    for (const BindingWrite& w : writes) {
        VkWriteDescriptorSet n{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        n.dstSet = Native(w.set);
        n.dstBinding = w.binding;
        n.dstArrayElement = w.first;
        n.descriptorType = Native(w.type);
        if (w.type == BindingType::AccelerationStructure) {
            structure_handles.push_back(Native(w.acceleration_structure));
            VkWriteDescriptorSetAccelerationStructureKHR& s = structures.emplace_back();
            s = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
            s.accelerationStructureCount = 1;
            s.pAccelerationStructures = &structure_handles.back();
            n.pNext = &s;
            n.descriptorCount = 1;
        } else if (!w.buffers.empty()) {
            n.pBufferInfo = buffers.data() + buffers.size();
            for (const BufferBinding& b : w.buffers) {
                buffers.push_back({Native(b.buffer), b.offset, b.range});
            }
            n.descriptorCount = static_cast<uint32_t>(w.buffers.size());
        } else if (w.type == BindingType::Sampler) {
            n.pImageInfo = images.data() + images.size();
            for (SamplerHandle s : w.samplers) {
                images.push_back({Native(s), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            }
            n.descriptorCount = static_cast<uint32_t>(w.samplers.size());
        } else {
            n.pImageInfo = images.data() + images.size();
            for (const ImageBinding& b : w.images) {
                images.push_back({Native(b.sampler), Native(b.view), Native(b.layout)});
            }
            n.descriptorCount = static_cast<uint32_t>(w.images.size());
        }
        native.push_back(n);
    }
    if (!native.empty()) {
        vkUpdateDescriptorSets(ctx_.device, static_cast<uint32_t>(native.size()), native.data(), 0, nullptr);
    }
}

PipelineLayoutHandle Device::CreatePipelineLayout(const PipelineLayoutDesc& desc) {
    std::vector<VkDescriptorSetLayout> sets;
    for (BindingLayoutHandle h : desc.sets) {
        sets.push_back(Native(h));
    }
    const VkPushConstantRange push{desc.push_stages.bits, 0, desc.push_bytes};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = static_cast<uint32_t>(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = desc.push_bytes ? 1u : 0u;
    info.pPushConstantRanges = desc.push_bytes ? &push : nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (!pt::vk::Check(vkCreatePipelineLayout(ctx_.device, &info, nullptr, &layout), "vkCreatePipelineLayout")) {
        return {};
    }
    return Wrap<PipelineLayoutTag>(layout);
}

void Device::DestroyPipelineLayout(PipelineLayoutHandle layout) {
    if (layout) {
        vkDestroyPipelineLayout(ctx_.device, Native(layout), nullptr);
    }
}

PipelineHandle Device::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    PipelineHandle out;
    CreateGraphicsPipelines({&desc, 1}, {&out, 1});
    return out;
}

bool Device::PreloadShaders(std::span<const char* const> names) {
    std::lock_guard lock(modules_mutex_);
    bool ok = true;
    for (const char* name : names) {
        if (modules_.contains(name)) {
            continue;
        }
        const VkShaderModule module = pt::vk::LoadShaderModule(ctx_.device, name);
        ok = ok && module != VK_NULL_HANDLE;
        if (module) {
            modules_.emplace(name, module);
        }
    }
    return ok;
}

void Device::ReleaseShaders(std::span<const char* const> names) {
    std::lock_guard lock(modules_mutex_);
    for (const char* name : names) {
        if (auto it = modules_.find(name); it != modules_.end()) {
            vkDestroyShaderModule(ctx_.device, it->second, nullptr);
            modules_.erase(it);
        }
    }
}

bool Device::CreateGraphicsPipelines(std::span<const GraphicsPipelineDesc> descs, std::span<PipelineHandle> out) {
    std::vector<std::pair<std::string, VkShaderModule>> loaded;
    std::map<std::string, VkShaderModule> preloaded;
    {
        std::lock_guard lock(modules_mutex_);
        preloaded = modules_;
    }
    bool missing = false;
    auto module = [&](const char* name) -> VkShaderModule {
        if (auto it = preloaded.find(name); it != preloaded.end()) {
            return it->second;
        }
        for (const auto& [n, m] : loaded) {
            if (n == name) {
                return m;
            }
        }
        const VkShaderModule m = pt::vk::LoadShaderModule(ctx_.device, name);
        missing = missing || m == VK_NULL_HANDLE;
        loaded.emplace_back(name, m);
        return m;
    };
    std::vector<std::pair<VkShaderModule, VkShaderModule>> stages(descs.size());
    for (size_t i = 0; i < descs.size(); ++i) {
        stages[i].first = module(descs[i].vertex);
        stages[i].second = descs[i].fragment ? module(descs[i].fragment) : VK_NULL_HANDLE;
    }
    bool ok = !missing;
    for (size_t i = 0; i < descs.size() && i < out.size(); ++i) {
        out[i] = {};
        if (!missing) {
            out[i] = Wrap<PipelineTag>(CreatePipeline(descs[i], stages[i].first, stages[i].second));
            ok = ok && static_cast<bool>(out[i]);
        }
    }
    for (const auto& [n, m] : loaded) {
        if (m) {
            vkDestroyShaderModule(ctx_.device, m, nullptr);
        }
    }
    return ok;
}

VkPipeline Device::CreatePipeline(const GraphicsPipelineDesc& desc, VkShaderModule vert, VkShaderModule frag) {
    if (desc.fixed_state) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};
        const VkVertexInputBindingDescription binding{0, desc.vertex_stride, VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (size_t i = 0; i < desc.vertex_attributes.size(); ++i) {
            attributes.push_back({static_cast<uint32_t>(i), 0, Native(desc.vertex_attributes[i].format), desc.vertex_attributes[i].offset});
        }
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (!attributes.empty()) {
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &binding;
            vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
            vertex_input.pVertexAttributeDescriptions = attributes.data();
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = Native(desc.cull);
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = Native(desc.depth_compare);
        VkPipelineColorBlendAttachmentState attachments[8]{};
        const uint32_t color_count = static_cast<uint32_t>(std::min<size_t>(desc.colors.size(), 8));
        for (uint32_t i = 0; i < color_count && i < desc.blend_states.size(); ++i) {
            const BlendState& b = desc.blend_states[i];
            VkPipelineColorBlendAttachmentState& a = attachments[i];
            a.blendEnable = b.enable ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = static_cast<VkBlendFactor>(b.src_color);
            a.dstColorBlendFactor = static_cast<VkBlendFactor>(b.dst_color);
            a.colorBlendOp = static_cast<VkBlendOp>(b.color_op);
            a.srcAlphaBlendFactor = static_cast<VkBlendFactor>(b.src_alpha);
            a.dstAlphaBlendFactor = static_cast<VkBlendFactor>(b.dst_alpha);
            a.alphaBlendOp = static_cast<VkBlendOp>(b.alpha_op);
            a.colorWriteMask = b.write_mask.bits;
        }
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = color_count;
        blend.pAttachments = attachments;
        const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkFormat color_formats[8]{};
        for (uint32_t i = 0; i < color_count; ++i) {
            color_formats[i] = Native(desc.colors[i]);
        }
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = color_count;
        rendering.pColorAttachmentFormats = color_formats;
        rendering.depthAttachmentFormat = Native(desc.depth);
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = desc.depth_state ? &depth : nullptr;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = Native(desc.layout);
        const char* label = desc.fragment ? desc.fragment : desc.vertex;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (!pt::vk::Check(pt::vk::CreateGraphicsPipelinesCached(ctx_.device, 1, &info, nullptr, &pipeline, label), label)) {
            pipeline = VK_NULL_HANDLE;
        }
        return pipeline;
    }
    static_assert(static_cast<uint8_t>(gpu::BlendMode::ProbeAccumulate) == static_cast<uint8_t>(pt::BlendMode::ProbeAccumulate));
    static_assert(static_cast<uint8_t>(gpu::BlendMode::Multiply) == static_cast<uint8_t>(pt::BlendMode::Multiply));
    static_assert(static_cast<uint8_t>(gpu::BlendMode::AdditiveAlpha) == static_cast<uint8_t>(pt::BlendMode::AdditiveAlpha));
    PipelineDesc p;
    p.vertex = desc.vertex;
    p.fragment = desc.fragment;
    p.layout = Native(desc.layout);
    for (Format f : desc.colors) {
        p.colors.push_back(Native(f));
    }
    p.depth = Native(desc.depth);
    p.mesh_input = desc.mesh_input;
    for (size_t i = 0; i < desc.vertex_attributes.size(); ++i) {
        p.vertex_attributes.push_back({static_cast<uint32_t>(i), 0, Native(desc.vertex_attributes[i].format), desc.vertex_attributes[i].offset});
    }
    p.vertex_stride = desc.vertex_stride;
    p.depth_test = desc.depth_test;
    p.depth_write = desc.depth_write;
    p.depth_compare = Native(desc.depth_compare);
    p.cull = Native(desc.cull);
    p.depth_bias = desc.depth_bias;
    p.depth_bounds = desc.depth_bounds;
    p.blend = static_cast<pt::BlendMode>(desc.blend);
    for (gpu::BlendMode b : desc.blends) {
        p.blends.push_back(static_cast<pt::BlendMode>(b));
    }
    p.write_mask = desc.write_mask.bits;
    for (ColorMask m : desc.write_masks) {
        p.write_masks.push_back(m.bits);
    }
    return pt::CreateGraphicsPipeline(ctx_.device, p, vert, frag);
}

PipelineHandle Device::CreateComputePipeline(const ComputePipelineDesc& desc) {
    return Wrap<PipelineTag>(pt::CreateComputePipeline(ctx_.device, Native(desc.layout), desc.shader));
}

void Device::DestroyPipeline(PipelineHandle pipeline) {
    if (pipeline) {
        vkDestroyPipeline(ctx_.device, Native(pipeline), nullptr);
    }
}

QueryPoolHandle Device::CreateTimestampPool(uint32_t count) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (!pt::vk::Check(vkCreateQueryPool(ctx_.device, &info, nullptr, &pool), "vkCreateQueryPool")) {
        return {};
    }
    return Wrap<QueryPoolTag>(pool);
}

void Device::DestroyQueryPool(QueryPoolHandle pool) {
    if (pool) {
        vkDestroyQueryPool(ctx_.device, Native(pool), nullptr);
    }
}

bool Device::ReadTimestamps(QueryPoolHandle pool, uint32_t first, uint32_t count, uint64_t* ticks) {
    return vkGetQueryPoolResults(ctx_.device, Native(pool), first, count, sizeof(uint64_t) * count, ticks, sizeof(uint64_t),
                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
}

void Device::Submit(const std::function<void(gpu::CommandList&)>& record) {
    ctx_.Submit([&](VkCommandBuffer cmd) {
        CommandList list(cmd);
        record(list);
    });
}

void Device::SubmitDeferred(const std::function<void(gpu::CommandList&)>& record, BufferResource staging) {
    ctx_.SubmitDeferred(
        [&](VkCommandBuffer cmd) {
            CommandList list(cmd);
            record(list);
        },
        FromGpu(staging));
}

void Device::ReapDeferred(bool wait) {
    ctx_.ReapDeferred(wait);
}

void Device::WaitIdle() {
    vkDeviceWaitIdle(ctx_.device);
}

gpu::CommandList* Device::BeginFrame(uint32_t) {
    LogError("gpu::vk::Device::BeginFrame: the Vulkan frame loop is the Renderer's until stage a6.2");
    return nullptr;
}

bool Device::EndFrame(uint32_t, RenderTarget*) {
    LogError("gpu::vk::Device::EndFrame: the Vulkan frame loop is the Renderer's until stage a6.2");
    return false;
}

bool Device::ReadImage(RenderTarget& image, std::vector<uint8_t>& out) {
    const FormatInfo info = Describe(image.image.format);
    if (!image.Valid() || info.bytes == 0 || info.block) {
        return false;
    }
    const uint32_t width = image.image.extent.width;
    const uint32_t height = image.image.extent.height;
    BufferDesc bd;
    bd.size = static_cast<uint64_t>(width) * height * info.bytes;
    bd.usage = BufferUsage::TransferDst;
    bd.readback = true;
    BufferResource buffer;
    if (!CreateBuffer(bd, buffer)) {
        return false;
    }
    const Layout back = image.layout;
    Submit([&](gpu::CommandList& list) {
        list.UseTargets({{&image, Layout::TransferSrc}});
        BufferImageCopy region;
        region.image.aspect = image.aspect;
        region.extent = {width, height, 1};
        list.CopyImageToBuffer(image.image, Layout::TransferSrc, buffer, std::span(&region, 1));
        if (back != Layout::Undefined && back != Layout::TransferSrc) {
            list.UseTargets({{&image, back}});
        }
    });
    out.assign(static_cast<const uint8_t*>(buffer.mapped), static_cast<const uint8_t*>(buffer.mapped) + bd.size);
    DestroyBuffer(buffer);
    return true;
}

}
