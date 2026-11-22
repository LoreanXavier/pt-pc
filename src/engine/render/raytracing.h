#pragma once

#include <glm/glm.hpp>

#include <chrono>
#include <memory>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/render/gpu_types.h"
#include "engine/render/mesh.h"
#include "engine/render/gpu/gpu_device.h"
#include "engine/render/renderer.h"
#include "engine/render/rt_caster.h"
#include "engine/render/vk/vk_device.h"
#include "engine/render/vk_context.h"

namespace pt {

struct RayTracingSettings {
    bool shadows = false;
    bool soft_shadows = false;
    bool ambient_occlusion = false;
    bool contact_shadows = false;
    bool reflections = false;
    bool full = false;
    bool full_shadows = true;
    bool bounce_light = true;
    bool glossy_reflections = true;
    bool full_unused = false;
    bool path_traced = false;
    bool operator==(const RayTracingSettings&) const = default;
};

struct RtStats {
    uint32_t instances = 0;
    uint32_t skinned_instances = 0;
    uint32_t skinned_vertices = 0;
    uint32_t static_blas = 0;
    uint32_t static_triangles = 0;
    VkDeviceSize static_bytes = 0;
    uint32_t built_this_frame = 0;
    uint32_t effect_triangles = 0;
};

class RayTracing {
public:
    virtual ~RayTracing() = default;
    virtual bool Init(gpu::Device& device, gpu::BindingLayoutHandle textures_layout, gpu::BindingLayoutHandle frame_layout) = 0;
    virtual void Shutdown() = 0;
    virtual void Forget(const GpuMesh& mesh) = 0;
    virtual bool Build(gpu::CommandList& cmd, uint32_t slot, std::span<const RtCaster> casters) = 0;
    virtual void SetEffects(std::span<const Vertex> vertices, std::span<const uint32_t> indices, uint8_t mask = 64u) {
        (void)vertices;
        (void)indices;
        (void)mask;
    }
    virtual bool Valid(uint32_t slot) const = 0;
    virtual bool Ready(uint32_t slot) const = 0;
    virtual void SetReflectionImage(gpu::ImageViewHandle view, gpu::SamplerHandle sampler) = 0;
    static constexpr uint32_t kAoImages = 5;
    virtual void SetAoImages(std::span<const gpu::ImageViewHandle> views) = 0;
    static constexpr uint32_t kFullImages = 17;
    static constexpr uint32_t kFullDepthFirst = 4;
    static constexpr uint32_t kFullDepthImages = 2;
    virtual void SetFullImages(std::span<const gpu::ImageViewHandle> views) = 0;
    static constexpr uint32_t kPathImages32 = 4;
    static constexpr uint32_t kPathImages16 = 11;
    virtual void SetPathImages(std::span<const gpu::ImageViewHandle> views32, std::span<const gpu::ImageViewHandle> views16) = 0;
    virtual gpu::BufferResource PathRemap(uint32_t slot) const = 0;
    virtual gpu::PipelineLayoutHandle Layout() const = 0;
    virtual gpu::BufferResource LightState(uint32_t slot) const = 0;
    virtual gpu::BindingSetHandle Set(uint32_t slot) const = 0;
    virtual const RtStats& Stats() const = 0;
};

using RayTracingFactory = std::unique_ptr<RayTracing> (*)(gpu::Device& device);
void SetD3D12RayTracingFactory(RayTracingFactory factory);
bool D3D12RayTracingAvailable();
std::unique_ptr<RayTracing> CreateRayTracing(gpu::Device& device);

class VkRayTracing final : public RayTracing {
public:
    bool Init(gpu::Device& device, gpu::BindingLayoutHandle textures_layout, gpu::BindingLayoutHandle frame_layout) override;
    void Shutdown() override;
    void Forget(const GpuMesh& mesh) override;
    bool Build(gpu::CommandList& cmd, uint32_t slot, std::span<const RtCaster> casters) override;
    void SetEffects(std::span<const Vertex> vertices, std::span<const uint32_t> indices, uint8_t mask = 64u) override;
    bool Valid(uint32_t slot) const override { return slots_[slot].valid; }
    bool Ready(uint32_t slot) const override;
    void SetReflectionImage(gpu::ImageViewHandle view, gpu::SamplerHandle sampler) override;
    void SetAoImages(std::span<const gpu::ImageViewHandle> views) override;
    void SetFullImages(std::span<const gpu::ImageViewHandle> views) override;
    void SetPathImages(std::span<const gpu::ImageViewHandle> views32, std::span<const gpu::ImageViewHandle> views16) override;
    gpu::BufferResource PathRemap(uint32_t slot) const override { return gpu::vk::ToGpu(slots_[slot].path_remap); }
    gpu::PipelineLayoutHandle Layout() const override { return gpu::vk::Wrap<gpu::PipelineLayoutTag>(layout_); }
    gpu::BufferResource LightState(uint32_t slot) const override { return gpu::vk::ToGpu(slots_[slot].light_state); }
    gpu::BindingSetHandle Set(uint32_t slot) const override { return gpu::vk::Wrap<gpu::BindingSetTag>(slots_[slot].set); }
    const RtStats& Stats() const override { return stats_; }

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
        vk::Buffer effect_vertices;
        vk::Buffer effect_indices;
        vk::Buffer effect_storage;
        std::vector<VkAccelerationStructureKHR> dynamic;
        std::vector<vk::Buffer> retired;
        VkDescriptorSet set = VK_NULL_HANDLE;
        vk::Buffer light_state;
        vk::Buffer path_remap;
        bool written = false;
        bool valid = false;
    };

    struct BuildJob {
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        VkDeviceSize scratch = 0;
        Blas* owner = nullptr;
    };

    bool EnsureBuffer(Slot& slot, vk::Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    VkDeviceAddress Address(const vk::Buffer& buffer) const;
    VkDeviceAddress Address(const gpu::BufferResource& buffer) const { return Address(gpu::vk::FromGpu(buffer)); }
    VkAccelerationStructureGeometryKHR Triangles(VkDeviceAddress vertices, VkDeviceSize stride, uint32_t vertex_count, VkDeviceAddress indices) const;
    bool RunJobs(VkCommandBuffer cmd, Slot& slot, std::vector<BuildJob>& jobs);
    void BackOff(const char* what);
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
    std::vector<Vertex> effect_vertices_;
    std::vector<uint32_t> effect_indices_;
    uint8_t effect_mask_ = 64u;
    RtStats stats_;
    std::chrono::steady_clock::time_point retry_after_{};
};

}
