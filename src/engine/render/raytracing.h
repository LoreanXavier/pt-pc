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

// PC option, off by default: ray traced shadows (rendering.md 12.21). The device gets the ray query extensions only when the
// option is on at start (vk::Context::want_ray_query); without them nothing here is created.
struct RayTracingSettings {
    bool shadows = false;
    // penumbrae from each light's size (several rays per lit pixel) instead of the maps' sharp edges
    bool soft_shadows = false;
    // the probe ambient occluded by the scene within a short reach (rt_ao.comp)
    bool ambient_occlusion = false;
    // a short ray toward every light against every G-buffer surface (rt_shadow.glsl RtContact)
    bool contact_shadows = false;
    // the local reflections (12.16) traced against the scene, with the reflected surfaces off screen shaded at the hit
    bool reflections = false;
    bool operator==(const RayTracingSettings&) const = default;
};

// A shadow caster of the frame: one submesh of a draw, with the draw's transform (the casters of the shadow map pass)
struct RtCaster {
    const GpuMesh* mesh = nullptr;
    uint32_t submesh = 0;
    glm::mat4 transform{1.0f};
    uint32_t material = 0;
    // the draw's first matrix in the frame's skin buffer for a skinned submesh, else gpu::kInvalid
    uint32_t skin_base = gpu::kInvalid;
    // bit 0: a caster for the camera view's lights, bit 1: for the mirror view's lights (the shadow pass's two caster sets)
    uint8_t mask = 3;
    bool alpha_test = false;
    bool double_sided = false;
};

struct RtStats {
    uint32_t instances = 0;
    uint32_t skinned_instances = 0;
    uint32_t skinned_vertices = 0;
    // BLAS of static submeshes built during the run, with their triangles and storage
    uint32_t static_blas = 0;
    uint32_t static_triangles = 0;
    VkDeviceSize static_bytes = 0;
    uint32_t built_this_frame = 0;
};

class RayTracing {
public:
    bool Init(vk::Context& ctx, VkDescriptorSetLayout textures_layout, VkDescriptorSetLayout frame_layout);
    void Shutdown();
    // Drops the mesh's acceleration structures (waits for the device when it has any)
    void Forget(const GpuMesh& mesh);
    // Records the frame slot's acceleration structures: a BLAS for every submesh seen for the first time, the skinned
    // casters' positions (compute, with the frame's skin buffer: sets 0 and 1 must be bound for compute) and their BLAS,
    // then the TLAS of all casters; ends with a barrier for the fragment shaders' ray queries
    void Build(VkCommandBuffer cmd, uint32_t slot, std::span<const RtCaster> casters);
    // the traced reflections' colour target for binding 2 of both sets (the device must be idle)
    void SetReflectionImage(VkImageView view, VkSampler sampler);
    // the ambient occlusion's storage images (GENERAL layout) for bindings 3 to 7 of both sets: the raw value, the two
    // histories, the horizontal blur and the result (the device must be idle)
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
        // buffers replaced while recording: the command buffer may still use them, so they go at the slot's next frame
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
