#include "engine/render/raytracing.h"

#include <algorithm>
#include <cstring>

#include "engine/core/log.h"
#include "engine/render/render_util.h"

namespace pt {
namespace {

// std430 record of a TLAS instance (instanceCustomIndex), read by rt_shadow.glsl for the alpha test of non-opaque hits
struct RtRecord {
    uint64_t vertices = 0;
    uint64_t indices = 0;
    uint32_t first_index = 0;
    int32_t vertex_offset = 0;
    uint32_t material = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(RtRecord) == 32);

struct SkinPush {
    uint64_t vertices = 0;
    uint64_t positions = 0;
    uint32_t count = 0;
    uint32_t skin_base = 0;
};

constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
constexpr VkDeviceSize kMaxScratch = 128ull << 20;
constexpr VkDeviceSize kStorageAlignment = 256;

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) / a * a;
}

void MemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}

bool RayTracing::Init(vk::Context& ctx, VkDescriptorSetLayout textures_layout, VkDescriptorSetLayout frame_layout) {
    ctx_ = &ctx;
    device_ = ctx.device;
    // binding 2: the traced reflections' colour target (SetReflectionImage), read by reflect_blend_rt.frag; bindings 3 to 7: the
    // ambient occlusion's storage images (SetAoImages)
    constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding bindings[3 + kAoImages]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, kStages, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 0; i < kAoImages; ++i) {
        bindings[3 + i] = {3 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 3 + kAoImages;
    layout_info.pBindings = bindings;
    if (!vk::Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &set_layout_), "ray tracing set layout")) {
        return false;
    }
    const VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, Renderer::kFramesInFlight},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Renderer::kFramesInFlight},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Renderer::kFramesInFlight},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kAoImages * Renderer::kFramesInFlight}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = Renderer::kFramesInFlight;
    pool_info.poolSizeCount = 4;
    pool_info.pPoolSizes = sizes;
    if (!vk::Check(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_), "ray tracing descriptor pool")) {
        return false;
    }
    for (Slot& slot : slots_) {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &set_layout_;
        if (!vk::Check(vkAllocateDescriptorSets(device_, &alloc, &slot.set), "ray tracing descriptor set")) {
            return false;
        }
    }
    // sets 0 and 1 and the push constant range as the scene layout's, so the scene's bound sets stay valid (layouts compatible
    // for set 1) when a pipeline of this layout is bound
    const VkDescriptorSetLayout sets[3] = {textures_layout, frame_layout, set_layout_};
    const VkPushConstantRange push{kPushStages, 0, 128};
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 3;
    pipeline_layout.pSetLayouts = sets;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push;
    if (!vk::Check(vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &layout_), "ray tracing pipeline layout")) {
        return false;
    }
    skin_ = CreateComputePipeline(device_, layout_, "rt_skin.comp");
    if (!skin_) {
        LogError("ray tracing: skinning pipeline failed");
        return false;
    }
    return true;
}

void RayTracing::Shutdown() {
    if (!device_) {
        return;
    }
    vkDeviceWaitIdle(device_);
    for (auto& [mesh, list] : static_) {
        for (Blas& b : list) {
            if (b.handle) {
                vkDestroyAccelerationStructureKHR(device_, b.handle, nullptr);
            }
            ctx_->DestroyBuffer(b.buffer);
        }
    }
    static_.clear();
    for (Slot& slot : slots_) {
        for (VkAccelerationStructureKHR as : slot.dynamic) {
            vkDestroyAccelerationStructureKHR(device_, as, nullptr);
        }
        slot.dynamic.clear();
        if (slot.tlas) {
            vkDestroyAccelerationStructureKHR(device_, slot.tlas, nullptr);
        }
        vk::Buffer* buffers[] = {&slot.instances, &slot.records, &slot.tlas_buffer, &slot.scratch, &slot.positions, &slot.dynamic_storage};
        for (vk::Buffer* b : buffers) {
            ctx_->DestroyBuffer(*b);
        }
        for (vk::Buffer& b : slot.retired) {
            ctx_->DestroyBuffer(b);
        }
        slot = Slot{};
    }
    if (skin_) {
        vkDestroyPipeline(device_, skin_, nullptr);
    }
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    skin_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void RayTracing::Forget(const GpuMesh& mesh) {
    auto it = static_.find(&mesh);
    if (it == static_.end()) {
        return;
    }
    vkDeviceWaitIdle(device_);
    for (Blas& b : it->second) {
        if (b.handle) {
            vkDestroyAccelerationStructureKHR(device_, b.handle, nullptr);
        }
        ctx_->DestroyBuffer(b.buffer);
    }
    static_.erase(it);
}

bool RayTracing::EnsureBuffer(Slot& slot, vk::Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    if (buffer.buffer && buffer.size >= size) {
        return true;
    }
    if (buffer.buffer) {
        slot.retired.push_back(buffer);
        buffer = vk::Buffer{};
    }
    // grow by half again, so a slowly growing need does not replace the buffer every frame
    return ctx_->CreateBuffer(buffer, std::max<VkDeviceSize>(size + size / 2, 4096), usage, host_visible);
}

VkDeviceAddress RayTracing::Address(const vk::Buffer& buffer) const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer.buffer;
    return vkGetBufferDeviceAddress(device_, &info);
}

VkAccelerationStructureGeometryKHR RayTracing::Triangles(VkDeviceAddress vertices, VkDeviceSize stride, uint32_t vertex_count,
                                                         VkDeviceAddress indices) const {
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // opaque here; the instance flags make the alpha tested ones non-opaque
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR& t = geometry.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    t.vertexData.deviceAddress = vertices;
    t.vertexStride = stride;
    t.maxVertex = vertex_count > 0 ? vertex_count - 1 : 0;
    t.indexType = VK_INDEX_TYPE_UINT32;
    t.indexData.deviceAddress = indices;
    return geometry;
}

const RayTracing::Blas* RayTracing::StaticBlas(const GpuMesh& mesh, uint32_t submesh, std::vector<BuildJob>& jobs) {
    std::vector<Blas>& list = static_[&mesh];
    if (list.size() < mesh.submeshes.size()) {
        list.resize(mesh.submeshes.size());
    }
    if (submesh >= list.size()) {
        return nullptr;
    }
    Blas& b = list[submesh];
    if (b.handle || b.empty) {
        return b.handle ? &b : nullptr;
    }
    const SubMesh& sub = mesh.submeshes[submesh];
    const uint32_t vertex_count = static_cast<uint32_t>(mesh.vertices.size / sizeof(Vertex));
    uint32_t primitives = sub.index_count / 3;
    if (primitives == 0 || sub.vertex_offset < 0 || vertex_count == 0) {
        b.empty = true;
        return nullptr;
    }
    BuildJob job;
    job.geometry = Triangles(Address(mesh.vertices), sizeof(Vertex), vertex_count, Address(mesh.indices));
    job.range = {primitives, sub.first_index * static_cast<uint32_t>(sizeof(uint32_t)), static_cast<uint32_t>(sub.vertex_offset), 0};
    job.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    job.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    job.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    job.info.geometryCount = 1;
    job.info.pGeometries = &job.geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &job.info, &primitives, &sizes);
    if (!ctx_->CreateBuffer(b.buffer, sizes.accelerationStructureSize,
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false)) {
        b.empty = true;
        return nullptr;
    }
    VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = b.buffer.buffer;
    create.size = sizes.accelerationStructureSize;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (!vk::Check(vkCreateAccelerationStructureKHR(device_, &create, nullptr, &b.handle), "vkCreateAccelerationStructureKHR (BLAS)")) {
        ctx_->DestroyBuffer(b.buffer);
        b.empty = true;
        return nullptr;
    }
    VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    address.accelerationStructure = b.handle;
    b.address = vkGetAccelerationStructureDeviceAddressKHR(device_, &address);
    job.info.dstAccelerationStructure = b.handle;
    job.scratch = sizes.buildScratchSize;
    jobs.push_back(job);
    ++stats_.static_blas;
    stats_.static_triangles += primitives;
    stats_.static_bytes += sizes.accelerationStructureSize;
    return &b;
}

void RayTracing::RunJobs(VkCommandBuffer cmd, Slot& slot, std::vector<BuildJob>& jobs) {
    if (jobs.empty()) {
        return;
    }
    const VkDeviceSize align = std::max<VkDeviceSize>(ctx_->scratch_alignment, 1);
    VkDeviceSize total = 0;
    VkDeviceSize largest = 0;
    for (const BuildJob& job : jobs) {
        total += AlignUp(job.scratch, align);
        largest = std::max(largest, AlignUp(job.scratch, align));
    }
    const VkDeviceSize capacity = std::max(largest, std::min(total, kMaxScratch));
    if (!EnsureBuffer(slot, slot.scratch, capacity + align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false)) {
        jobs.clear();
        return;
    }
    const VkDeviceAddress scratch = AlignUp(Address(slot.scratch), align);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
    size_t next = 0;
    while (next < jobs.size()) {
        infos.clear();
        ranges.clear();
        VkDeviceSize used = 0;
        while (next < jobs.size() && (infos.empty() || used + AlignUp(jobs[next].scratch, align) <= capacity)) {
            BuildJob& job = jobs[next++];
            job.info.pGeometries = &job.geometry;
            job.info.scratchData.deviceAddress = scratch + used;
            used += AlignUp(job.scratch, align);
            infos.push_back(job.info);
            ranges.push_back(&job.range);
        }
        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<uint32_t>(infos.size()), infos.data(), ranges.data());
        // scratch reuse by the next batch, and the TLAS build reading these BLAS
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    }
    stats_.built_this_frame += static_cast<uint32_t>(jobs.size());
    jobs.clear();
}

void RayTracing::SetReflectionImage(VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo image{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (Slot& slot : slots_) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = slot.set;
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
}

void RayTracing::SetAoImages(std::span<const VkImageView> views) {
    VkDescriptorImageInfo images[kAoImages]{};
    VkWriteDescriptorSet writes[kAoImages]{};
    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(views.size()), kAoImages);
    for (Slot& slot : slots_) {
        for (uint32_t i = 0; i < count; ++i) {
            images[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = slot.set;
            writes[i].dstBinding = 3 + i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device_, count, writes, 0, nullptr);
    }
}

void RayTracing::WriteSet(Slot& slot) {
    VkWriteDescriptorSetAccelerationStructureKHR as_info{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as_info.accelerationStructureCount = 1;
    as_info.pAccelerationStructures = &slot.tlas;
    VkDescriptorBufferInfo records{slot.records.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].pNext = &as_info;
    writes[0].dstSet = slot.set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = slot.set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &records;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    slot.written = true;
}

void RayTracing::Build(VkCommandBuffer cmd, uint32_t slot_index, std::span<const RtCaster> casters) {
    Slot& slot = slots_[slot_index];
    // the slot's previous frame has completed (its fence), so its skinned BLAS can go
    for (VkAccelerationStructureKHR as : slot.dynamic) {
        vkDestroyAccelerationStructureKHR(device_, as, nullptr);
    }
    slot.dynamic.clear();
    for (vk::Buffer& b : slot.retired) {
        ctx_->DestroyBuffer(b);
    }
    slot.retired.clear();
    stats_.built_this_frame = 0;
    BeginLabel(cmd, "ray tracing structures");

    // BLAS of the static submeshes seen for the first time (built once, kept until the mesh goes)
    std::vector<BuildJob> jobs;
    std::vector<VkDeviceAddress> blas(casters.size(), 0);
    for (size_t i = 0; i < casters.size(); ++i) {
        const RtCaster& c = casters[i];
        if (c.skin_base == gpu::kInvalid) {
            if (const Blas* b = StaticBlas(*c.mesh, c.submesh, jobs)) {
                blas[i] = b->address;
            }
        }
    }
    RunJobs(cmd, slot, jobs);

    // skinned casters: their draw's vertices through the frame's skin matrices (as mesh.vert), once per draw, into
    // slot.positions, then one BLAS per submesh built from them
    struct SkinGroup {
        const GpuMesh* mesh = nullptr;
        uint32_t skin_base = 0;
        VkDeviceSize offset = 0;
        uint32_t count = 0;
    };
    std::vector<SkinGroup> groups;
    std::vector<int32_t> group_of(casters.size(), -1);
    VkDeviceSize positions_size = 0;
    for (size_t i = 0; i < casters.size(); ++i) {
        const RtCaster& c = casters[i];
        if (c.skin_base == gpu::kInvalid) {
            continue;
        }
        auto it = std::find_if(groups.begin(), groups.end(), [&](const SkinGroup& g) { return g.mesh == c.mesh && g.skin_base == c.skin_base; });
        if (it == groups.end()) {
            SkinGroup g;
            g.mesh = c.mesh;
            g.skin_base = c.skin_base;
            g.count = static_cast<uint32_t>(c.mesh->vertices.size / sizeof(Vertex));
            g.offset = positions_size;
            positions_size += AlignUp(static_cast<VkDeviceSize>(g.count) * 12, 256);
            groups.push_back(g);
            it = groups.end() - 1;
        }
        group_of[i] = static_cast<int32_t>(it - groups.begin());
    }
    stats_.skinned_instances = 0;
    stats_.skinned_vertices = 0;
    if (!groups.empty() &&
        EnsureBuffer(slot, slot.positions, positions_size,
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     false)) {
        const VkDeviceAddress positions = Address(slot.positions);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skin_);
        for (const SkinGroup& g : groups) {
            SkinPush push;
            push.vertices = Address(g.mesh->vertices);
            push.positions = positions + g.offset;
            push.count = g.count;
            push.skin_base = g.skin_base;
            vkCmdPushConstants(cmd, layout_, kPushStages, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (g.count + 63) / 64, 1, 1);
            stats_.skinned_vertices += g.count;
        }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        // sizes first, then the BLAS in one storage buffer (256 byte offsets)
        struct Pending {
            size_t caster = 0;
            BuildJob job;
            VkDeviceSize size = 0;
            VkDeviceSize offset = 0;
        };
        std::vector<Pending> pending;
        VkDeviceSize storage = 0;
        for (size_t i = 0; i < casters.size(); ++i) {
            if (group_of[i] < 0) {
                continue;
            }
            const RtCaster& c = casters[i];
            const SkinGroup& g = groups[group_of[i]];
            const SubMesh& sub = c.mesh->submeshes[c.submesh];
            uint32_t primitives = sub.index_count / 3;
            if (primitives == 0 || sub.vertex_offset < 0) {
                continue;
            }
            Pending p;
            p.caster = i;
            p.job.geometry = Triangles(positions + g.offset, 12, g.count, Address(c.mesh->indices));
            p.job.range = {primitives, sub.first_index * static_cast<uint32_t>(sizeof(uint32_t)), static_cast<uint32_t>(sub.vertex_offset), 0};
            p.job.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            p.job.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            p.job.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            p.job.info.geometryCount = 1;
            p.job.info.pGeometries = &p.job.geometry;
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &p.job.info, &primitives, &sizes);
            p.size = sizes.accelerationStructureSize;
            p.offset = storage;
            p.job.scratch = sizes.buildScratchSize;
            storage += AlignUp(p.size, kStorageAlignment);
            pending.push_back(p);
        }
        if (!pending.empty() && EnsureBuffer(slot, slot.dynamic_storage, storage,
                                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false)) {
            for (Pending& p : pending) {
                VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                create.buffer = slot.dynamic_storage.buffer;
                create.offset = p.offset;
                create.size = p.size;
                create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
                if (!vk::Check(vkCreateAccelerationStructureKHR(device_, &create, nullptr, &handle), "vkCreateAccelerationStructureKHR (skinned)")) {
                    continue;
                }
                slot.dynamic.push_back(handle);
                VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
                address.accelerationStructure = handle;
                blas[p.caster] = vkGetAccelerationStructureDeviceAddressKHR(device_, &address);
                p.job.info.dstAccelerationStructure = handle;
                jobs.push_back(p.job);
                ++stats_.skinned_instances;
            }
            RunJobs(cmd, slot, jobs);
        }
    }

    // TLAS of every caster with a BLAS; instanceCustomIndex = its record
    const uint32_t count = static_cast<uint32_t>(casters.size());
    if (count > slot.capacity || !slot.instances.buffer) {
        uint32_t capacity = 1024;
        while (capacity < count) {
            capacity *= 2;
        }
        slot.retired.push_back(slot.instances);
        slot.retired.push_back(slot.records);
        slot.instances = vk::Buffer{};
        slot.records = vk::Buffer{};
        if (!ctx_->CreateBuffer(slot.instances, sizeof(VkAccelerationStructureInstanceKHR) * capacity,
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true) ||
            !ctx_->CreateBuffer(slot.records, sizeof(RtRecord) * capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) {
            slot.capacity = 0;
            EndLabel(cmd);
            return;
        }
        slot.capacity = capacity;
        slot.written = false;
    }
    auto* instances = static_cast<VkAccelerationStructureInstanceKHR*>(slot.instances.mapped);
    auto* records = static_cast<RtRecord*>(slot.records.mapped);
    uint32_t used = 0;
    for (size_t i = 0; i < casters.size(); ++i) {
        const RtCaster& c = casters[i];
        if (blas[i] == 0 || c.mask == 0) {
            continue;
        }
        VkAccelerationStructureInstanceKHR& inst = instances[used];
        for (int r = 0; r < 3; ++r) {
            for (int col = 0; col < 4; ++col) {
                inst.transform.matrix[r][col] = c.transform[col][r];
            }
        }
        inst.instanceCustomIndex = used;
        inst.mask = c.mask;
        inst.instanceShaderBindingTableRecordOffset = 0;
        VkGeometryInstanceFlagsKHR flags = c.alpha_test ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        if (c.double_sided) {
            flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        }
        // facing is decided in object space; a mirroring transform turns the winding the rasterizer sees
        if (glm::determinant(glm::mat3(c.transform)) < 0.0f) {
            flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
        }
        inst.flags = flags;
        inst.accelerationStructureReference = blas[i];
        RtRecord& rec = records[used];
        rec = RtRecord{};
        const SubMesh& sub = c.mesh->submeshes[c.submesh];
        rec.vertices = Address(c.mesh->vertices);
        rec.indices = Address(c.mesh->indices);
        rec.first_index = sub.first_index;
        rec.vertex_offset = sub.vertex_offset;
        rec.material = c.material;
        ++used;
    }
    vmaFlushAllocation(ctx_->allocator, slot.instances.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(ctx_->allocator, slot.records.allocation, 0, VK_WHOLE_SIZE);
    stats_.instances = used;

    BuildJob tlas;
    tlas.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas.geometry.flags = 0;
    tlas.geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlas.geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    tlas.geometry.geometry.instances.data.deviceAddress = Address(slot.instances);
    tlas.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas.info.geometryCount = 1;
    tlas.info.pGeometries = &tlas.geometry;
    if (!slot.tlas || slot.tlas_capacity < slot.capacity) {
        uint32_t max_count = slot.capacity;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas.info, &max_count, &sizes);
        if (slot.tlas) {
            vkDestroyAccelerationStructureKHR(device_, slot.tlas, nullptr);
            slot.tlas = VK_NULL_HANDLE;
        }
        if (slot.tlas_buffer.buffer) {
            slot.retired.push_back(slot.tlas_buffer);
            slot.tlas_buffer = vk::Buffer{};
        }
        if (!ctx_->CreateBuffer(slot.tlas_buffer, sizes.accelerationStructureSize,
                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false)) {
            EndLabel(cmd);
            return;
        }
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = slot.tlas_buffer.buffer;
        create.size = sizes.accelerationStructureSize;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (!vk::Check(vkCreateAccelerationStructureKHR(device_, &create, nullptr, &slot.tlas), "vkCreateAccelerationStructureKHR (TLAS)")) {
            EndLabel(cmd);
            return;
        }
        slot.tlas_capacity = slot.capacity;
        slot.written = false;
    }
    uint32_t primitives = used;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas.info, &primitives, &sizes);
    tlas.info.dstAccelerationStructure = slot.tlas;
    tlas.range = {used, 0, 0, 0};
    tlas.scratch = sizes.buildScratchSize;
    jobs.push_back(tlas);
    RunJobs(cmd, slot, jobs);
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    if (!slot.written) {
        WriteSet(slot);
    }
    EndLabel(cmd);
}

}
