#include "engine/render/d3d12/backend/d3d12_raytracing.h"

#ifdef _WIN32

#include "engine/render/d3d12/backend/d3d12_internal.h"

#include <d3dx12/d3dx12.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/core/log.h"

namespace pt::gpu::d3d12 {
namespace {

struct RtRecord {
    uint32_t vertices[2] = {};
    uint32_t indices[2] = {};
    uint32_t first_index = 0;
    int32_t vertex_offset = 0;
    uint32_t material = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(RtRecord) == 32);

struct SkinPush {
    uint32_t vertices[2] = {};
    uint32_t positions[2] = {};
    uint32_t count = 0;
    uint32_t skin_base = 0;
};
static_assert(sizeof(SkinPush) == 24);

constexpr uint64_t kMaxScratch = 128ull << 20;
constexpr uint64_t kAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
constexpr uint32_t kRawCapacity = 32768;
constexpr uint32_t kSkinOutCapacity = 4;
constexpr D3D12_RESOURCE_STATES kMeshRead = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

uint64_t AlignUp(uint64_t v, uint64_t a) {
    return (v + a - 1) / a * a;
}

void UavBarrier(ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &b);
}

class D3D12RayTracing final : public RayTracing {
public:
    explicit D3D12RayTracing(DeviceImpl& device) : dev_(device) {}
    ~D3D12RayTracing() override { Shutdown(); }

    bool Init(gpu::Device& device, gpu::BindingLayoutHandle textures_layout, gpu::BindingLayoutHandle frame_layout) override;
    void Shutdown() override;
    void Forget(const GpuMesh& mesh) override;
    bool Build(gpu::CommandList& cmd, uint32_t slot, std::span<const RtCaster> casters) override;
    void SetEffects(std::span<const Vertex> vertices, std::span<const uint32_t> indices, uint8_t mask = 64u) override;
    bool Valid(uint32_t slot) const override { return slots_[slot].valid; }
    bool Ready(uint32_t slot) const override { return slots_[slot].valid || std::chrono::steady_clock::now() >= retry_after_; }
    void SetReflectionImage(gpu::ImageViewHandle view, gpu::SamplerHandle sampler) override;
    void SetAoImages(std::span<const gpu::ImageViewHandle> views) override;
    void SetFullImages(std::span<const gpu::ImageViewHandle> views) override;
    void SetPathImages(std::span<const gpu::ImageViewHandle> views32, std::span<const gpu::ImageViewHandle> views16) override;
    gpu::BufferResource PathRemap(uint32_t slot) const override { return slots_[slot].path_remap; }
    gpu::PipelineLayoutHandle Layout() const override { return layout_; }
    gpu::BufferResource LightState(uint32_t slot) const override { return slots_[slot].light_state; }
    gpu::BindingSetHandle Set(uint32_t slot) const override { return slots_[slot].set; }
    const RtStats& Stats() const override { return stats_; }

private:
    struct Raw {
        ComPtr<ID3D12Resource> resource;
        D3D12MA::Allocation* allocation = nullptr;
        uint64_t size = 0;
    };
    struct Blas {
        Raw storage;
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        bool empty = false;
    };
    struct Slot {
        gpu::BufferResource instances;
        gpu::BufferResource records;
        uint32_t capacity = 0;
        Raw tlas;
        uint32_t tlas_capacity = 0;
        Raw scratch;
        gpu::BufferResource positions;
        Raw dynamic_storage;
        gpu::BufferResource effect_vertices;
        gpu::BufferResource effect_indices;
        Raw effect_storage;
        std::vector<Raw> retired;
        std::vector<gpu::BufferResource> retired_buffers;
        gpu::BindingSetHandle set;
        gpu::BufferResource light_state;
        gpu::BufferResource path_remap;
        bool written = false;
        bool valid = false;
    };
    struct BuildJob {
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        uint64_t scratch = 0;
        Blas* owner = nullptr;
    };

    void BackOff(const char* what);
    bool EnsureRaw(Slot& slot, Raw& raw, uint64_t size, D3D12_RESOURCE_STATES state, const char* name);
    uint32_t RawIndex(const gpu::BufferResource& buffer);
    void Unregister(const gpu::BufferResource& buffer);
    D3D12_RAYTRACING_GEOMETRY_DESC Triangles(D3D12_GPU_VIRTUAL_ADDRESS vertices, uint64_t stride, uint32_t vertex_count,
                                             D3D12_GPU_VIRTUAL_ADDRESS indices, uint32_t index_count) const;
    const Blas* StaticBlas(CommandListImpl& cl, const GpuMesh& mesh, uint32_t submesh, std::vector<BuildJob>& jobs);
    bool RunJobs(CommandListImpl& cl, ID3D12GraphicsCommandList4* list, Slot& slot, std::vector<BuildJob>& jobs);
    void WriteSet(Slot& slot);
    void MeshRead(CommandListImpl& cl, const GpuMesh& mesh);

    DeviceImpl& dev_;
    ComPtr<ID3D12Device5> device5_;
    gpu::BindingLayoutHandle set_layout_;
    gpu::BindingPoolHandle pool_;
    gpu::PipelineLayoutHandle layout_;
    gpu::PipelineHandle skin_;
    Slot slots_[Renderer::kFramesInFlight];
    std::unordered_map<const GpuMesh*, std::vector<Blas>> static_;
    std::unordered_map<const BufferRecord*, uint32_t> raw_index_;
    std::vector<uint32_t> raw_free_;
    uint32_t raw_next_ = 0;
    std::vector<Vertex> effect_vertices_;
    std::vector<uint32_t> effect_indices_;
    uint8_t effect_mask_ = 64u;
    RtStats stats_;
    std::chrono::steady_clock::time_point retry_after_{};
    bool ready_ = false;
};

bool D3D12RayTracing::Init(gpu::Device& device, gpu::BindingLayoutHandle textures_layout, gpu::BindingLayoutHandle frame_layout) {
    (void)device;
    if (FAILED(dev_.Dev()->QueryInterface(IID_PPV_ARGS(&device5_)))) {
        LogError("ray tracing: no ID3D12Device5 (DXR)");
        return false;
    }
    using gpu::BindingType;
    const gpu::ShaderStageFlags stages = gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute;
    gpu::BindingLayoutDesc set;
    set.bindings = {{0, BindingType::AccelerationStructure, 1, stages},
                    {1, BindingType::StorageBuffer, 1, stages},
                    {2, BindingType::CombinedImageSampler, 1, gpu::ShaderStage::Fragment},
                    {2, BindingType::StorageBuffer, kRawCapacity, stages},
                    {3, BindingType::StorageBuffer, kSkinOutCapacity, stages, {}, true}};
    for (uint32_t i = 0; i < kAoImages; ++i) {
        set.bindings.push_back({3 + i, BindingType::StorageImage, 1, stages});
    }
    set.bindings.push_back({3 + kAoImages, BindingType::StorageBuffer, 1, stages});
    set.bindings.push_back({4 + kAoImages, BindingType::StorageImage, kFullImages - kFullDepthImages, stages});
    set.bindings.push_back({5 + kAoImages, BindingType::StorageImage, kFullDepthImages, stages});
    set.bindings.push_back({6 + kAoImages, BindingType::StorageImage, kPathImages32, stages});
    set.bindings.push_back({7 + kAoImages, BindingType::StorageImage, kPathImages16, stages});
    set.bindings.push_back({8 + kAoImages, BindingType::StorageBuffer, 1, stages});
    set_layout_ = dev_.CreateBindingLayout(set);
    pool_ = dev_.CreateBindingPool({});
    gpu::PipelineLayoutDesc layout;
    layout.sets = {textures_layout, frame_layout, set_layout_};
    layout.push_bytes = 128;
    layout_ = dev_.CreatePipelineLayout(layout);
    for (Slot& slot : slots_) {
        slot.set = dev_.AllocateBindingSet(pool_, set_layout_, 0);
        if (!slot.set) {
            return false;
        }
        const ::pt::vk::AllocTag tag("ray tracing");
        if (!dev_.CreateBuffer({sizeof(glm::vec4) * gpu::kMaxLights,
                                gpu::BufferUsage::StorageBuffer | gpu::BufferUsage::TransferSrc | gpu::BufferUsage::TransferDst, false, false,
                                "ray tracing light state"},
                               slot.light_state) ||
            !dev_.CreateBuffer({sizeof(uint32_t) * gpu::kMaxLights, gpu::BufferUsage::StorageBuffer, true, false, "ray tracing light remap", true},
                               slot.path_remap)) {
            return false;
        }
        std::fill_n(static_cast<uint32_t*>(slot.path_remap.mapped), gpu::kMaxLights, ~0u);
        const gpu::BufferBinding state{slot.light_state.buffer};
        const gpu::BufferBinding remap{slot.path_remap.buffer};
        const gpu::BindingWrite writes[2] = {{slot.set, 3 + kAoImages, 0, BindingType::StorageBuffer, {}, {&state, 1}},
                                             {slot.set, 8 + kAoImages, 0, BindingType::StorageBuffer, {}, {&remap, 1}}};
        dev_.WriteBindings(writes);
    }
    skin_ = dev_.CreateComputePipeline({layout_, "rt_skin.comp"});
    if (!skin_) {
        LogError("ray tracing: skinning pipeline failed");
        return false;
    }
    ready_ = true;
    LogInfo("ray tracing: DirectX Raytracing 1.1 (inline ray queries)");
    return true;
}

void D3D12RayTracing::Shutdown() {
    if (!ready_ && !layout_) {
        return;
    }
    dev_.WaitIdle();
    for (auto& [mesh, list] : static_) {
        for (Blas& b : list) {
            dev_.Retire(std::move(b.storage.resource), b.storage.allocation);
        }
    }
    static_.clear();
    for (Slot& slot : slots_) {
        for (Raw* r : {&slot.tlas, &slot.scratch, &slot.dynamic_storage, &slot.effect_storage}) {
            dev_.Retire(std::move(r->resource), r->allocation);
        }
        for (Raw& r : slot.retired) {
            dev_.Retire(std::move(r.resource), r.allocation);
        }
        for (gpu::BufferResource* b : {&slot.instances, &slot.records, &slot.positions, &slot.effect_vertices, &slot.effect_indices, &slot.light_state,
                                       &slot.path_remap}) {
            dev_.DestroyBuffer(*b);
        }
        for (gpu::BufferResource& b : slot.retired_buffers) {
            dev_.DestroyBuffer(b);
        }
        slot = Slot{};
    }
    if (skin_) dev_.DestroyPipeline(skin_);
    if (layout_) dev_.DestroyPipelineLayout(layout_);
    if (pool_) dev_.DestroyBindingPool(pool_);
    if (set_layout_) dev_.DestroyBindingLayout(set_layout_);
    skin_ = {};
    layout_ = {};
    pool_ = {};
    set_layout_ = {};
    raw_index_.clear();
    raw_free_.clear();
    raw_next_ = 0;
    effect_vertices_.clear();
    effect_indices_.clear();
    effect_mask_ = 64u;
    ready_ = false;
}

void D3D12RayTracing::BackOff(const char* what) {
    const bool first = std::chrono::steady_clock::now() >= retry_after_;
    retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    if (first) LogWarn("ray tracing: {} not allocated (out of video memory), tried again in 1 s", what);
}

bool D3D12RayTracing::EnsureRaw(Slot& slot, Raw& raw, uint64_t size, D3D12_RESOURCE_STATES state, const char* name) {
    if (raw.resource && raw.size >= size) {
        return true;
    }
    if (raw.resource) {
        slot.retired.push_back(std::move(raw));
        raw = Raw{};
    }
    const uint64_t want = AlignUp(std::max<uint64_t>(size + size / 2, 4096), kAlignment);
    if (!dev_.CreateRawBuffer(want, state, true, raw.resource, raw.allocation, name)) {
        return false;
    }
    raw.size = want;
    return true;
}

uint32_t D3D12RayTracing::RawIndex(const gpu::BufferResource& buffer) {
    const BufferRecord* rec = Rec<BufferRecord>(buffer.buffer);
    if (auto it = raw_index_.find(rec); it != raw_index_.end()) {
        return it->second;
    }
    uint32_t index = 0;
    if (!raw_free_.empty()) {
        index = raw_free_.back();
        raw_free_.pop_back();
    } else if (raw_next_ < kRawCapacity) {
        index = raw_next_++;
    } else {
        LogError("ray tracing: more than {} mesh buffers for the raw buffer array", kRawCapacity);
        return 0;
    }
    raw_index_.emplace(rec, index);
    const gpu::BufferBinding binding{buffer.buffer};
    for (Slot& slot : slots_) {
        const gpu::BindingWrite write{slot.set, 2, index, gpu::BindingType::StorageBuffer, {}, {&binding, 1}};
        dev_.WriteBindings({&write, 1});
    }
    return index;
}

void D3D12RayTracing::Unregister(const gpu::BufferResource& buffer) {
    const BufferRecord* rec = Rec<BufferRecord>(buffer.buffer);
    auto it = raw_index_.find(rec);
    if (it == raw_index_.end()) {
        return;
    }
    raw_free_.push_back(it->second);
    raw_index_.erase(it);
}

void D3D12RayTracing::Forget(const GpuMesh& mesh) {
    const bool registered = raw_index_.contains(Rec<BufferRecord>(mesh.vertices.buffer)) || raw_index_.contains(Rec<BufferRecord>(mesh.indices.buffer));
    auto it = static_.find(&mesh);
    if (it == static_.end() && !registered) {
        return;
    }
    dev_.WaitIdle();
    if (it != static_.end()) {
        for (Blas& b : it->second) {
            dev_.Retire(std::move(b.storage.resource), b.storage.allocation);
        }
        static_.erase(it);
    }
    Unregister(mesh.vertices);
    Unregister(mesh.indices);
}

D3D12_RAYTRACING_GEOMETRY_DESC D3D12RayTracing::Triangles(D3D12_GPU_VIRTUAL_ADDRESS vertices, uint64_t stride, uint32_t vertex_count,
                                                          D3D12_GPU_VIRTUAL_ADDRESS indices, uint32_t index_count) const {
    D3D12_RAYTRACING_GEOMETRY_DESC g{};
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    g.Triangles.VertexBuffer.StartAddress = vertices;
    g.Triangles.VertexBuffer.StrideInBytes = stride;
    g.Triangles.VertexCount = vertex_count;
    g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    g.Triangles.IndexBuffer = indices;
    g.Triangles.IndexCount = index_count;
    return g;
}

void D3D12RayTracing::MeshRead(CommandListImpl& cl, const GpuMesh& mesh) {
    for (const gpu::BufferResource* b : {&mesh.vertices, &mesh.indices}) {
        if (BufferRecord* rec = Rec<BufferRecord>(b->buffer)) {
            cl.Transition(*rec, kMeshRead);
        }
    }
}

const D3D12RayTracing::Blas* D3D12RayTracing::StaticBlas(CommandListImpl& cl, const GpuMesh& mesh, uint32_t submesh, std::vector<BuildJob>& jobs) {
    std::vector<Blas>& list = static_[&mesh];
    if (list.size() < mesh.submeshes.size()) {
        list.resize(mesh.submeshes.size());
    }
    if (submesh >= list.size()) {
        return nullptr;
    }
    Blas& b = list[submesh];
    if (b.storage.resource || b.empty) {
        return b.storage.resource ? &b : nullptr;
    }
    const SubMesh& sub = mesh.submeshes[submesh];
    const uint32_t vertex_count = static_cast<uint32_t>(mesh.vertices.size / sizeof(Vertex));
    const uint32_t primitives = sub.index_count / 3;
    if (primitives == 0 || sub.vertex_offset < 0 || vertex_count == 0 || static_cast<uint32_t>(sub.vertex_offset) >= vertex_count) {
        b.empty = true;
        return nullptr;
    }
    if (std::chrono::steady_clock::now() < retry_after_) {
        return nullptr;
    }
    const BufferRecord* vertices = Rec<BufferRecord>(mesh.vertices.buffer);
    const BufferRecord* indices = Rec<BufferRecord>(mesh.indices.buffer);
    if (!vertices || !indices) {
        b.empty = true;
        return nullptr;
    }
    MeshRead(cl, mesh);
    BuildJob job;
    job.geometry = Triangles(vertices->resource->GetGPUVirtualAddress() + uint64_t(sub.vertex_offset) * sizeof(Vertex), sizeof(Vertex),
                             vertex_count - static_cast<uint32_t>(sub.vertex_offset),
                             indices->resource->GetGPUVirtualAddress() + uint64_t(sub.first_index) * sizeof(uint32_t), primitives * 3);
    job.desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    job.desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    job.desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    job.desc.Inputs.NumDescs = 1;
    job.desc.Inputs.pGeometryDescs = &job.geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&job.desc.Inputs, &info);
    const uint64_t size = AlignUp(info.ResultDataMaxSizeInBytes, kAlignment);
    if (!dev_.CreateRawBuffer(size, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, b.storage.resource, b.storage.allocation,
                              "static BLAS")) {
        BackOff("a static BLAS");
        return nullptr;
    }
    b.storage.size = size;
    b.address = b.storage.resource->GetGPUVirtualAddress();
    job.desc.DestAccelerationStructureData = b.address;
    job.scratch = info.ScratchDataSizeInBytes;
    job.owner = &b;
    jobs.push_back(job);
    ++stats_.static_blas;
    stats_.static_triangles += primitives;
    stats_.static_bytes += size;
    return &b;
}

bool D3D12RayTracing::RunJobs(CommandListImpl& cl, ID3D12GraphicsCommandList4* list, Slot& slot, std::vector<BuildJob>& jobs) {
    (void)cl;
    if (jobs.empty()) {
        return true;
    }
    uint64_t total = 0;
    uint64_t largest = 0;
    for (const BuildJob& job : jobs) {
        total += AlignUp(job.scratch, kAlignment);
        largest = std::max(largest, AlignUp(job.scratch, kAlignment));
    }
    const uint64_t capacity = std::max(largest, std::min(total, kMaxScratch));
    if (!EnsureRaw(slot, slot.scratch, capacity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "ray tracing scratch")) {
        BackOff("the build scratch buffer");
        return false;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS scratch = slot.scratch.resource->GetGPUVirtualAddress();
    size_t next = 0;
    while (next < jobs.size()) {
        uint64_t used = 0;
        size_t first = next;
        while (next < jobs.size() && (next == first || used + AlignUp(jobs[next].scratch, kAlignment) <= capacity)) {
            BuildJob& job = jobs[next++];
            if (job.desc.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
                job.desc.Inputs.pGeometryDescs = &job.geometry;
            }
            job.desc.ScratchAccelerationStructureData = scratch + used;
            used += AlignUp(job.scratch, kAlignment);
            list->BuildRaytracingAccelerationStructure(&job.desc, 0, nullptr);
        }
        UavBarrier(list);
    }
    stats_.built_this_frame += static_cast<uint32_t>(jobs.size());
    jobs.clear();
    return true;
}

void D3D12RayTracing::SetEffects(std::span<const Vertex> vertices, std::span<const uint32_t> indices, uint8_t mask) {
    effect_vertices_.assign(vertices.begin(), vertices.end());
    effect_indices_.assign(indices.begin(), indices.end());
    effect_mask_ = mask;
}

void D3D12RayTracing::SetReflectionImage(gpu::ImageViewHandle view, gpu::SamplerHandle sampler) {
    const gpu::ImageBinding image{view, gpu::Layout::ShaderReadOnly, sampler};
    for (Slot& slot : slots_) {
        const gpu::BindingWrite write{slot.set, 2, 0, gpu::BindingType::CombinedImageSampler, {&image, 1}};
        dev_.WriteBindings({&write, 1});
    }
}

void D3D12RayTracing::SetAoImages(std::span<const gpu::ImageViewHandle> views) {
    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(views.size()), kAoImages);
    for (Slot& slot : slots_) {
        for (uint32_t i = 0; i < count; ++i) {
            const gpu::ImageBinding image{views[i], gpu::Layout::General};
            const gpu::BindingWrite write{slot.set, 3 + i, 0, gpu::BindingType::StorageImage, {&image, 1}};
            dev_.WriteBindings({&write, 1});
        }
    }
}

void D3D12RayTracing::SetFullImages(std::span<const gpu::ImageViewHandle> views) {
    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(views.size()), kFullImages);
    for (Slot& slot : slots_) {
        for (uint32_t i = 0; i < count; ++i) {
            const bool depth = i >= kFullDepthFirst && i < kFullDepthFirst + kFullDepthImages;
            const gpu::ImageBinding image{views[i], gpu::Layout::General};
            const gpu::BindingWrite write{slot.set, depth ? 5 + kAoImages : 4 + kAoImages,
                                          depth ? i - kFullDepthFirst : i < kFullDepthFirst ? i : i - kFullDepthImages,
                                          gpu::BindingType::StorageImage, {&image, 1}};
            dev_.WriteBindings({&write, 1});
        }
    }
}

void D3D12RayTracing::SetPathImages(std::span<const gpu::ImageViewHandle> views32, std::span<const gpu::ImageViewHandle> views16) {
    for (Slot& slot : slots_) {
        for (uint32_t i = 0; i < kPathImages32 + kPathImages16; ++i) {
            const bool wide = i < kPathImages32;
            const uint32_t element = wide ? i : i - kPathImages32;
            const std::span<const gpu::ImageViewHandle> views = wide ? views32 : views16;
            if (element >= views.size()) {
                continue;
            }
            const gpu::ImageBinding image{views[element], gpu::Layout::General};
            const gpu::BindingWrite write{slot.set, wide ? 6 + kAoImages : 7 + kAoImages, element, gpu::BindingType::StorageImage, {&image, 1}};
            dev_.WriteBindings({&write, 1});
        }
    }
}

void D3D12RayTracing::WriteSet(Slot& slot) {
    SetRecord* set = Rec<SetRecord>(slot.set);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.RaytracingAccelerationStructure.Location = slot.tlas.resource->GetGPUVirtualAddress();
    dev_.Dev()->CreateShaderResourceView(nullptr, &v, dev_.Views().Cpu(set->views + 0));
    const gpu::BufferBinding records{slot.records.buffer};
    const gpu::BindingWrite write{slot.set, 1, 0, gpu::BindingType::StorageBuffer, {}, {&records, 1}};
    dev_.WriteBindings({&write, 1});
    slot.written = true;
}

bool D3D12RayTracing::Build(gpu::CommandList& cmd, uint32_t slot_index, std::span<const RtCaster> casters) {
    CommandListImpl& cl = static_cast<CommandListImpl&>(cmd);
    ComPtr<ID3D12GraphicsCommandList4> list;
    if (FAILED(cl.List()->QueryInterface(IID_PPV_ARGS(&list)))) {
        return false;
    }
    const ::pt::vk::AllocTag tag("ray tracing");
    Slot& slot = slots_[slot_index];
    slot.valid = false;
    for (Raw& r : slot.retired) {
        dev_.Retire(std::move(r.resource), r.allocation);
    }
    slot.retired.clear();
    for (gpu::BufferResource& b : slot.retired_buffers) {
        Unregister(b);
        dev_.DestroyBuffer(b);
    }
    slot.retired_buffers.clear();
    stats_.built_this_frame = 0;
    cmd.BeginLabel("ray tracing structures");

    std::vector<BuildJob> jobs;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> blas(casters.size(), 0);
    for (size_t i = 0; i < casters.size(); ++i) {
        const RtCaster& c = casters[i];
        if (c.skin_base == gpu::kInvalid) {
            if (const Blas* b = StaticBlas(cl, *c.mesh, c.submesh, jobs)) {
                blas[i] = b->address;
            }
        }
    }
    if (!RunJobs(cl, list.Get(), slot, jobs)) {
        std::unordered_set<D3D12_GPU_VIRTUAL_ADDRESS> dropped;
        for (BuildJob& job : jobs) {
            if (!job.owner) continue;
            dropped.insert(job.owner->address);
            dev_.Retire(std::move(job.owner->storage.resource), job.owner->storage.allocation);
            *job.owner = Blas{};
        }
        jobs.clear();
        for (D3D12_GPU_VIRTUAL_ADDRESS& address : blas) {
            if (dropped.contains(address)) address = 0;
        }
    }

    struct SkinGroup {
        const GpuMesh* mesh = nullptr;
        uint32_t skin_base = 0;
        uint64_t offset = 0;
        uint32_t count = 0;
    };
    std::vector<SkinGroup> groups;
    std::vector<int32_t> group_of(casters.size(), -1);
    uint64_t positions_size = 0;
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
            positions_size += AlignUp(static_cast<uint64_t>(g.count) * 12, 256);
            groups.push_back(g);
            it = groups.end() - 1;
        }
        group_of[i] = static_cast<int32_t>(it - groups.begin());
    }
    stats_.skinned_instances = 0;
    stats_.skinned_vertices = 0;
    bool positions_ready = !groups.empty() && slot.positions.size >= positions_size;
    if (!groups.empty() && !positions_ready) {
        gpu::BufferResource grown;
        if (dev_.CreateBuffer({std::max<uint64_t>(positions_size + positions_size / 2, 4096),
                               gpu::BufferUsage::StorageBuffer | gpu::BufferUsage::AccelerationStructureBuildInput, false, false,
                               "ray tracing skinned positions"},
                              grown)) {
            if (slot.positions.buffer) slot.retired_buffers.push_back(slot.positions);
            slot.positions = grown;
            const gpu::BufferBinding binding{slot.positions.buffer};
            const gpu::BindingWrite write{slot.set, 3, 0, gpu::BindingType::StorageBuffer, {}, {&binding, 1}};
            dev_.WriteBindings({&write, 1});
            positions_ready = true;
        } else {
            BackOff("the skinned positions");
        }
    }
    if (positions_ready) {
        BufferRecord* positions = Rec<BufferRecord>(slot.positions.buffer);
        for (const SkinGroup& g : groups) {
            RawIndex(g.mesh->vertices);
            MeshRead(cl, *g.mesh);
        }
        cl.Transition(*positions, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd.BindPipeline(gpu::BindPoint::Compute, skin_);
        cmd.BindSets(gpu::BindPoint::Compute, layout_, 2, {&slot.set, 1});
        for (const SkinGroup& g : groups) {
            SkinPush push;
            push.vertices[0] = RawIndex(g.mesh->vertices);
            push.positions[0] = 0;
            push.positions[1] = static_cast<uint32_t>(g.offset);
            push.count = g.count;
            push.skin_base = g.skin_base;
            cmd.PushConstants(layout_, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute, 0, sizeof(push), &push);
            cmd.Dispatch((g.count + 63) / 64, 1, 1);
            stats_.skinned_vertices += g.count;
        }
        cl.Transition(*positions, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const D3D12_GPU_VIRTUAL_ADDRESS positions_va = positions->resource->GetGPUVirtualAddress();
        struct Pending {
            size_t caster = 0;
            BuildJob job;
            uint64_t size = 0;
            uint64_t offset = 0;
        };
        std::vector<Pending> pending;
        uint64_t storage = 0;
        for (size_t i = 0; i < casters.size(); ++i) {
            if (group_of[i] < 0) {
                continue;
            }
            const RtCaster& c = casters[i];
            const SkinGroup& g = groups[group_of[i]];
            const SubMesh& sub = c.mesh->submeshes[c.submesh];
            const uint32_t primitives = sub.index_count / 3;
            const BufferRecord* indices = Rec<BufferRecord>(c.mesh->indices.buffer);
            if (primitives == 0 || sub.vertex_offset < 0 || static_cast<uint32_t>(sub.vertex_offset) >= g.count || !indices) {
                continue;
            }
            Pending p;
            p.caster = i;
            p.job.geometry = Triangles(positions_va + g.offset + uint64_t(sub.vertex_offset) * 12, 12, g.count - static_cast<uint32_t>(sub.vertex_offset),
                                       indices->resource->GetGPUVirtualAddress() + uint64_t(sub.first_index) * sizeof(uint32_t), primitives * 3);
            p.job.desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            p.job.desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            p.job.desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            p.job.desc.Inputs.NumDescs = 1;
            p.job.desc.Inputs.pGeometryDescs = &p.job.geometry;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            device5_->GetRaytracingAccelerationStructurePrebuildInfo(&p.job.desc.Inputs, &info);
            p.size = AlignUp(info.ResultDataMaxSizeInBytes, kAlignment);
            p.offset = storage;
            p.job.scratch = info.ScratchDataSizeInBytes;
            storage += p.size;
            pending.push_back(p);
        }
        if (!pending.empty() && EnsureRaw(slot, slot.dynamic_storage, storage, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "skinned BLAS")) {
            const D3D12_GPU_VIRTUAL_ADDRESS base = slot.dynamic_storage.resource->GetGPUVirtualAddress();
            for (Pending& p : pending) {
                p.job.desc.DestAccelerationStructureData = base + p.offset;
                blas[p.caster] = base + p.offset;
                jobs.push_back(p.job);
                ++stats_.skinned_instances;
            }
        } else if (!pending.empty()) {
            BackOff("the skinned BLAS");
        }
    }

    D3D12_GPU_VIRTUAL_ADDRESS effects_blas = 0;
    stats_.effect_triangles = 0;
    const uint32_t effect_primitives = static_cast<uint32_t>(effect_indices_.size() / 3);
    if (effect_primitives > 0) {
        const uint64_t vbytes = effect_vertices_.size() * sizeof(Vertex);
        const uint64_t ibytes = effect_indices_.size() * sizeof(uint32_t);
        auto ensure_effect = [&](gpu::BufferResource& target, uint64_t needed, const char* name) {
            if (target.buffer && target.size >= needed) {
                return true;
            }
            gpu::BufferResource grown;
            const uint64_t want = std::max<uint64_t>(needed + needed / 2, 4096);
            if (!dev_.CreateBuffer({want, gpu::BufferUsage::StorageBuffer | gpu::BufferUsage::AccelerationStructureBuildInput, true, false, name, true},
                                   grown)) {
                return false;
            }
            if (target.buffer) slot.retired_buffers.push_back(target);
            target = grown;
            return true;
        };
        if (ensure_effect(slot.effect_vertices, vbytes, "ray tracing effect vertices") &&
            ensure_effect(slot.effect_indices, ibytes, "ray tracing effect indices")) {
            std::memcpy(slot.effect_vertices.mapped, effect_vertices_.data(), vbytes);
            std::memcpy(slot.effect_indices.mapped, effect_indices_.data(), ibytes);
            dev_.FlushMapped(slot.effect_vertices, 0, vbytes);
            dev_.FlushMapped(slot.effect_indices, 0, ibytes);
            BufferRecord* vrec = Rec<BufferRecord>(slot.effect_vertices.buffer);
            BufferRecord* irec = Rec<BufferRecord>(slot.effect_indices.buffer);
            if (vrec && irec) {
                cl.Transition(*vrec, kMeshRead);
                cl.Transition(*irec, kMeshRead);
                BuildJob job;
                job.geometry = Triangles(vrec->resource->GetGPUVirtualAddress(), sizeof(Vertex), static_cast<uint32_t>(effect_vertices_.size()),
                                         irec->resource->GetGPUVirtualAddress(), effect_primitives * 3);
                job.desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                job.desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                job.desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                job.desc.Inputs.NumDescs = 1;
                job.desc.Inputs.pGeometryDescs = &job.geometry;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
                device5_->GetRaytracingAccelerationStructurePrebuildInfo(&job.desc.Inputs, &info);
                const uint64_t size = AlignUp(info.ResultDataMaxSizeInBytes, kAlignment);
                if (EnsureRaw(slot, slot.effect_storage, size, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "effects BLAS")) {
                    effects_blas = slot.effect_storage.resource->GetGPUVirtualAddress();
                    job.desc.DestAccelerationStructureData = effects_blas;
                    job.scratch = info.ScratchDataSizeInBytes;
                    jobs.push_back(job);
                    stats_.effect_triangles = effect_primitives;
                }
            }
        }
    }

    if (!jobs.empty() && !RunJobs(cl, list.Get(), slot, jobs)) {
        for (size_t i = 0; i < casters.size(); ++i) {
            if (casters[i].skin_base != gpu::kInvalid) blas[i] = 0;
        }
        effects_blas = 0;
        stats_.effect_triangles = 0;
        jobs.clear();
    }

    const uint32_t count = static_cast<uint32_t>(casters.size()) + (effects_blas != 0 ? 1u : 0u);
    if (count > slot.capacity || !slot.instances.buffer) {
        uint32_t capacity = 1024;
        while (capacity < count) {
            capacity *= 2;
        }
        gpu::BufferResource instances_buffer;
        gpu::BufferResource records_buffer;
        if (!dev_.CreateBuffer({sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * capacity, gpu::BufferUsage::AccelerationStructureBuildInput, true, false,
                                "ray tracing instances"},
                               instances_buffer) ||
            !dev_.CreateBuffer({sizeof(RtRecord) * capacity, gpu::BufferUsage::StorageBuffer, true, false, "ray tracing records", true}, records_buffer)) {
            dev_.DestroyBuffer(instances_buffer);
            dev_.DestroyBuffer(records_buffer);
            BackOff("the TLAS instance buffers");
            cmd.EndLabel();
            return false;
        }
        if (slot.instances.buffer) slot.retired_buffers.push_back(slot.instances);
        if (slot.records.buffer) slot.retired_buffers.push_back(slot.records);
        slot.instances = instances_buffer;
        slot.records = records_buffer;
        slot.capacity = capacity;
        slot.written = false;
    }
    auto* instances = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(slot.instances.mapped);
    auto* records = static_cast<RtRecord*>(slot.records.mapped);
    uint32_t used = 0;
    for (size_t i = 0; i < casters.size(); ++i) {
        const RtCaster& c = casters[i];
        if (blas[i] == 0 || c.mask == 0) {
            continue;
        }
        D3D12_RAYTRACING_INSTANCE_DESC& inst = instances[used];
        inst = {};
        for (int r = 0; r < 3; ++r) {
            for (int col = 0; col < 4; ++col) {
                inst.Transform[r][col] = c.transform[col][r];
            }
        }
        inst.InstanceID = used;
        inst.InstanceMask = c.mask;
        inst.InstanceContributionToHitGroupIndex = 0;
        UINT flags = c.alpha_test || c.fitting ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE : D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        if (c.double_sided) {
            flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        }
        if (glm::determinant(glm::mat3(c.transform)) < 0.0f) {
            flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
        }
        inst.Flags = flags;
        inst.AccelerationStructure = blas[i];
        RtRecord& rec = records[used];
        rec = RtRecord{};
        const SubMesh& sub = c.mesh->submeshes[c.submesh];
        rec.vertices[0] = RawIndex(c.mesh->vertices);
        rec.indices[0] = RawIndex(c.mesh->indices);
        rec.first_index = sub.first_index;
        rec.vertex_offset = sub.vertex_offset;
        rec.material = c.material;
        rec.flags = RtRecordFlags(c);
        MeshRead(cl, *c.mesh);
        ++used;
    }
    if (effects_blas != 0) {
        D3D12_RAYTRACING_INSTANCE_DESC& inst = instances[used];
        inst = {};
        inst.Transform[0][0] = 1.0f;
        inst.Transform[1][1] = 1.0f;
        inst.Transform[2][2] = 1.0f;
        inst.InstanceID = used;
        inst.InstanceMask = effect_mask_;
        inst.InstanceContributionToHitGroupIndex = 0;
        inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE | D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        inst.AccelerationStructure = effects_blas;
        RtRecord& rec = records[used];
        rec = RtRecord{};
        rec.vertices[0] = RawIndex(slot.effect_vertices);
        rec.indices[0] = RawIndex(slot.effect_indices);
        rec.flags = kRtRecordEffect;
        ++used;
    }
    stats_.instances = used;
    dev_.FlushMapped(slot.records, 0, sizeof(RtRecord) * used);

    BuildJob tlas;
    tlas.desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas.desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlas.desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas.desc.Inputs.InstanceDescs = Rec<BufferRecord>(slot.instances.buffer)->resource->GetGPUVirtualAddress();
    if (!slot.tlas.resource || slot.tlas_capacity < slot.capacity) {
        tlas.desc.Inputs.NumDescs = slot.capacity;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(&tlas.desc.Inputs, &info);
        Raw grown;
        const uint64_t size = AlignUp(info.ResultDataMaxSizeInBytes, kAlignment);
        if (!dev_.CreateRawBuffer(size, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, grown.resource, grown.allocation, "TLAS")) {
            BackOff("the TLAS");
            cmd.EndLabel();
            return false;
        }
        grown.size = size;
        if (slot.tlas.resource) slot.retired.push_back(std::move(slot.tlas));
        slot.tlas = std::move(grown);
        slot.tlas_capacity = slot.capacity;
        slot.written = false;
    }
    tlas.desc.Inputs.NumDescs = used;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&tlas.desc.Inputs, &info);
    tlas.desc.DestAccelerationStructureData = slot.tlas.resource->GetGPUVirtualAddress();
    tlas.scratch = info.ScratchDataSizeInBytes;
    jobs.push_back(tlas);
    if (!RunJobs(cl, list.Get(), slot, jobs)) {
        jobs.clear();
        cmd.EndLabel();
        return false;
    }
    if (!slot.written) {
        WriteSet(slot);
    }
    slot.valid = true;
    cmd.EndLabel();
    return true;
}

}

std::unique_ptr<RayTracing> CreateRayTracing(gpu::Device& device) {
    if (!device.Info().ray_query) {
        return nullptr;
    }
    return std::make_unique<D3D12RayTracing>(static_cast<DeviceImpl&>(device));
}

}

#endif
