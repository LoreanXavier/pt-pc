#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/render/gpu_types.h"
#include "engine/render/mesh.h"
#include "engine/render/renderer.h"
#include "engine/render/vk_context.h"

namespace pt {

struct RayTracingSettings {
    bool shadows = false;
    bool soft_shadows = false;
    bool ambient_occlusion = false;
    bool contact_shadows = false;
    bool reflections = false;
    bool operator==(const RayTracingSettings&) const = default;
};

struct RtCaster {
    const GpuMesh* mesh = nullptr;
    uint32_t submesh = 0;
    glm::mat4 transform{1.0f};
    uint32_t material = 0;
    uint32_t skin_base = gpu::kInvalid;
    uint8_t mask = 3;
    bool alpha_test = false;
    bool double_sided = false;
};

struct RtStats {
    uint32_t instances = 0;
    uint32_t skinned_instances = 0;
    uint32_t skinned_vertices = 0;
    uint32_t static_blas = 0;
    uint32_t static_triangles = 0;
    VkDeviceSize static_bytes = 0;
    uint32_t built_this_frame = 0;
};

class RayTracing {
public:
    bool Init(vk::Context& ctx, VkDescriptorSetLayout textures_layout, VkDescriptorSetLayout frame_layout);
    void Shutdown();
    void Forget(const GpuMesh& mesh);
    void Build(VkCommandBuffer cmd, uint32_t slot, std::span<const RtCaster> casters);
    void SetReflectionImage(VkImageView view, VkSampler sampler);
    static constexpr uint32_t kAoImages = 5;
    void SetAoImages(std::span<const VkImageView> views);
    VkPipelineLayout Layout() const { return layout_; }
    VkDescriptorSet Set(uint32_t slot) const { return slots_[slot].set; }
    const RtStats& Stats() const { return stats_; }

private:
    struct Blas {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        vk::Buffer buffer;
        VkDeviceAddress address = 0;
        bool empty = false;
    };

    struct Slot {
        vk::Buffer instances;
        vk::Buffer records;
        uint32_t capacity = 0;
        vk::Buffer tlas_buffer;
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        uint32_t tlas_capacity = 0;
        vk::Buffer scratch;
        vk::Buffer positions;
        vk::Buffer dynamic_storage;
        std::vector<VkAccelerationStructureKHR> dynamic;
        std::vector<vk::Buffer> retired;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool written = false;
    };

    struct BuildJob {
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        VkDeviceSize scratch = 0;
    };

    bool EnsureBuffer(Slot& slot, vk::Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    VkDeviceAddress Address(const vk::Buffer& buffer) const;
    VkAccelerationStructureGeometryKHR Triangles(VkDeviceAddress vertices, VkDeviceSize stride, uint32_t vertex_count, VkDeviceAddress indices) const;
    void RunJobs(VkCommandBuffer cmd, Slot& slot, std::vector<BuildJob>& jobs);
    const Blas* StaticBlas(const GpuMesh& mesh, uint32_t submesh, std::vector<BuildJob>& jobs);
    void WriteSet(Slot& slot);

    vk::Context* ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline skin_ = VK_NULL_HANDLE;
    Slot slots_[Renderer::kFramesInFlight];
    std::unordered_map<const GpuMesh*, std::vector<Blas>> static_;
    RtStats stats_;
};

}
