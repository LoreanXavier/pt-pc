#pragma once

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/render/d3d12/backend/d3d12_backend.h"
#include "engine/render/d3d12/present/d3d12_device.h"
#include "engine/render/d3d12/present/dxgi_presenter.h"
#include "engine/render/d3d12/present/sl_dlssg_presenter.h"
#include "engine/render/d3d12/present/xefg_presenter.h"

namespace D3D12MA {
class Allocator;
class Allocation;
}

namespace pt::gpu::d3d12 {

using Microsoft::WRL::ComPtr;

DXGI_FORMAT ToDxgi(Format f);
DXGI_FORMAT ResourceFormat(Format f, bool sampled);
DXGI_FORMAT ShaderViewFormat(Format f);
D3D12_RESOURCE_STATES StateFor(Layout layout);
D3D12_COMPARISON_FUNC ToD3D(CompareOp op);

class DescriptorHeap {
public:
    bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shader_visible, const wchar_t* name);
    uint32_t Allocate(uint32_t count);
    void Free(uint32_t index, uint32_t count);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const { return {cpu_.ptr + static_cast<SIZE_T>(index) * increment_}; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const { return {gpu_.ptr + static_cast<UINT64>(index) * increment_}; }
    ID3D12DescriptorHeap* Heap() const { return heap_.Get(); }
    uint32_t Used() const { return used_; }
    uint32_t Capacity() const { return count_; }

private:
    ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_{};
    uint32_t increment_ = 0;
    uint32_t count_ = 0;
    uint32_t used_ = 0;
    std::map<uint32_t, uint32_t> free_;
    std::mutex mutex_;
};

constexpr uint32_t kNone = UINT32_MAX;
constexpr uint32_t kSlotsPerClass = 16;
constexpr uint32_t kSetBlock = 3 * kSlotsPerClass;
constexpr uint32_t kMaxSets = 4;

struct ImageRecord {
    ComPtr<ID3D12Resource> resource;
    D3D12MA::Allocation* allocation = nullptr;
    ImageDesc desc;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t rtv = kNone;
    uint32_t dsv = kNone;
    uint32_t dsv_read_only = kNone;
    uint32_t clear_uav_cpu = kNone;
    uint32_t clear_uav_gpu = kNone;
};

enum class HeapKind : uint8_t { Default, Upload, Readback, HostStorage };

struct BufferRecord {
    ComPtr<ID3D12Resource> resource;
    D3D12MA::Allocation* allocation = nullptr;
    uint64_t size = 0;
    BufferUsageFlags usage{};
    HeapKind heap = HeapKind::Default;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    void* mapped = nullptr;
    bool gpu_read_only = false;
    std::vector<uint8_t> host_data;
    uint64_t dirty_begin = 0;
    uint64_t dirty_end = 0;
};

struct SamplerRecord {
    D3D12_SAMPLER_DESC desc{};
};

struct LayoutRecord {
    BindingLayoutDesc desc;
    bool IsArray(const BindingDesc& b) const { return b.count > 1 || b.flags.Has(BindingFlag::PartiallyBound); }
};

struct SetRecord {
    const LayoutRecord* layout = nullptr;
    uint32_t views = kNone;
    uint32_t samplers = kNone;
    struct Array {
        uint32_t binding = 0;
        BindingType type = BindingType::SampledImage;
        bool sampler = false;
        bool writable = false;
        uint32_t base = kNone;
        uint32_t count = 0;
    };
    std::vector<Array> arrays;
    std::map<uint64_t, BufferRecord*> uploads;
};

struct PoolRecord {
    std::vector<std::unique_ptr<SetRecord>> sets;
};

struct PipelineLayoutRecord {
    std::vector<const LayoutRecord*> sets;
    uint32_t push_bytes = 0;
};

struct PipelineRecord {
    bool compute = false;
    std::string vertex_name;
    std::string fragment_name;
    GraphicsPipelineDesc desc;
    std::vector<uint8_t> vs;
    std::vector<uint8_t> ps;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    uint32_t stride = 0;
    bool dynamic_bias = false;
    ComPtr<ID3D12PipelineState> compute_state;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> variants;
};

struct QueryPoolRecord {
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    D3D12MA::Allocation* allocation = nullptr;
    uint64_t* mapped = nullptr;
    uint32_t count = 0;
};

template <class R, class H>
R* Rec(H handle) {
    return reinterpret_cast<R*>(static_cast<uintptr_t>(handle.bits));
}

template <class H, class R>
H MakeHandle(R* record) {
    return H{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(record))};
}

struct RootLayout {
    static constexpr UINT kPush = 0;
    static constexpr UINT kSetViews = 1;
    static constexpr UINT kSetSamplers = 5;
    static constexpr UINT kArrays = 9;
    struct ArraySpace {
        uint32_t space;
        D3D12_DESCRIPTOR_RANGE_TYPE type;
        uint32_t count;
    };
    static constexpr ArraySpace kArraySpaces[] = {
        {100, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX},
        {102, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX},
        {118, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 62},
        {119, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 4},
        {134, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX},
        {135, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX},
        {141, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 15},
        {142, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2},
        {143, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4},
        {144, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 11},
    };
    static int ArrayParam(uint32_t set, uint32_t binding) {
        const uint32_t space = 100 + 16 * set + binding;
        for (size_t i = 0; i < std::size(kArraySpaces); ++i) {
            if (kArraySpaces[i].space == space) {
                return static_cast<int>(kArrays + i);
            }
        }
        return -1;
    }
};

class DeviceImpl;

struct PassMarkers {
    static constexpr uint32_t kLabels = 512;
    volatile uint32_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint32_t serial = 0;
    uint32_t count = 0;
    const char* names[kLabels] = {};
};

class CommandListImpl final : public gpu::CommandList {
public:
    CommandListImpl(DeviceImpl& device, ID3D12GraphicsCommandList* list);
    void Barriers(UINT count, const D3D12_RESOURCE_BARRIER* barriers);

    gpu::Backend GetBackend() const override { return gpu::Backend::D3D12; }
    void UseTargets(std::span<const TargetUse> uses) override;
    using gpu::CommandList::UseTargets;
    void Barrier(const ImageBarrier& barrier) override;
    void BufferBarrier(BufferHandle buffer, PipelineStage src_stage, uint64_t src_access, PipelineStage dst_stage, uint64_t dst_access) override;
    void GlobalBarrier(PipelineStage src_stage, uint64_t src_access, PipelineStage dst_stage, uint64_t dst_access) override;
    void BeginPass(Rect2D area, std::span<const ColorOutput> colors, RenderTarget* depth, bool depth_read_only, bool clear_depth) override;
    using gpu::CommandList::BeginPass;
    void EndPass() override;
    void BeginRendering(Rect2D area, std::span<const Attachment> colors) override;
    void SetViewport(Rect2D area) override;
    void SetCullMode(CullMode mode) override;
    void SetFrontFace(FrontFace face) override;
    void SetDepthBias(float constant, float clamp, float slope) override;
    void SetDepthBounds(float min, float max) override;
    void BindPipeline(BindPoint point, PipelineHandle pipeline) override;
    void BindSets(BindPoint point, PipelineLayoutHandle layout, uint32_t first_set, std::span<const BindingSetHandle> sets) override;
    void PushConstants(PipelineLayoutHandle layout, ShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data) override;
    void BindVertexBuffer(BufferHandle buffer, uint64_t offset) override;
    void BindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) override;
    void Draw(uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) override;
    void DrawIndexed(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) override;
    void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;
    void CopyImage(const ImageResource& src, Layout src_layout, const ImageResource& dst, Layout dst_layout,
                   std::span<const ImageCopy> regions) override;
    void CopyImageToBuffer(const ImageResource& src, Layout src_layout, const BufferResource& dst, std::span<const BufferImageCopy> regions) override;
    void CopyBufferToImage(const BufferResource& src, const ImageResource& dst, Layout dst_layout, std::span<const BufferImageCopy> regions) override;
    void CopyBuffer(const BufferResource& src, uint64_t src_offset, const BufferResource& dst, uint64_t dst_offset, uint64_t size) override;
    void ClearColorImage(const ImageResource& image, Layout layout, const ClearColor& value, uint32_t mip_levels, uint32_t layers) override;
    using gpu::CommandList::ClearColorImage;
    void FillBuffer(const BufferResource& buffer, uint64_t offset, uint64_t size, uint32_t value) override;
    void ResetQueries(QueryPoolHandle pool, uint32_t first, uint32_t count) override;
    void WriteTimestamp(QueryPoolHandle pool, uint32_t index) override;
    void BeginLabel(const char* name) override;
    void EndLabel() override;

    void Begin();
    void SetMarkers(PassMarkers* markers) { markers_ = markers; }
    ID3D12GraphicsCommandList* List() const { return list_; }
    void Transition(ImageRecord& image, D3D12_RESOURCE_STATES state);
    void Transition(BufferRecord& buffer, D3D12_RESOURCE_STATES state);
    void ResumeAfterNative();
    void Keep(ComPtr<ID3D12Resource> resource, D3D12MA::Allocation* allocation) { kept_.emplace_back(std::move(resource), allocation); }
    std::vector<std::pair<ComPtr<ID3D12Resource>, D3D12MA::Allocation*>> TakeKept() { return std::move(kept_); }

private:
    bool StageUpload(uint64_t size, ComPtr<ID3D12Resource>& upload, D3D12MA::Allocation*& allocation, void** mapped);
    void FlushGraphics();
    void FlushCompute();
    void Apply(ID3D12PipelineState* state);
    void CopyFootprint(bool to_image, const BufferRecord& buffer, ImageRecord& image, const BufferImageCopy& region);

    DeviceImpl& device_;
    ID3D12GraphicsCommandList* list_ = nullptr;
    ComPtr<ID3D12GraphicsCommandList1> list1_;
    ComPtr<ID3D12GraphicsCommandList9> list9_;
    ComPtr<ID3D12GraphicsCommandList2> list2_;
    PassMarkers* markers_ = nullptr;
    std::vector<uint32_t> open_labels_;
    PipelineRecord* graphics_ = nullptr;
    PipelineRecord* compute_ = nullptr;
    ID3D12PipelineState* applied_ = nullptr;
    CullMode cull_ = CullMode::None;
    FrontFace front_ = FrontFace::CounterClockwise;
    float bias_[3] = {};
    bool bias_dirty_ = false;
    BufferRecord* vertex_buffer_ = nullptr;
    uint64_t vertex_offset_ = 0;
    uint32_t vertex_stride_ = 0;
    bool vertex_dirty_ = false;
    std::vector<std::pair<ComPtr<ID3D12Resource>, D3D12MA::Allocation*>> kept_;
    bool reclaimed_ = false;
    uint64_t stage_failed_size_ = UINT64_MAX;
};

bool FsrFrameGenAvailable(pt::d3d12::Device& device, std::string& reason);
std::unique_ptr<pt::d3d12::SwapchainOwner> CreateFsrFgPresenter(pt::d3d12::Device& device, std::string& error);

class DeviceImpl final : public D3D12Device {
public:
    explicit DeviceImpl(const CreateOptions& options) : options_(options), presenter_(bridge_) {}
    ~DeviceImpl() override;
    bool Init(std::string& error);

    const DeviceInfo& Info() const override { return info_; }
    bool CreateImage(const ImageDesc& desc, ImageResource& out) override;
    void DestroyImage(ImageResource& image) override;
    bool CreateBuffer(const BufferDesc& desc, BufferResource& out) override;
    void DestroyBuffer(BufferResource& buffer) override;
    void FlushMapped(const BufferResource& buffer, uint64_t offset, uint64_t size) override;
    uint64_t ImageBytes(const ImageResource& image) const override;
    SamplerHandle CreateSampler(const SamplerDesc& desc) override;
    void DestroySampler(SamplerHandle sampler) override;
    BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc& desc) override;
    void DestroyBindingLayout(BindingLayoutHandle layout) override;
    BindingPoolHandle CreateBindingPool(const BindingPoolDesc& desc) override;
    void DestroyBindingPool(BindingPoolHandle pool) override;
    BindingSetHandle AllocateBindingSet(BindingPoolHandle pool, BindingLayoutHandle layout, uint32_t variable_count) override;
    void WriteBindings(std::span<const BindingWrite> writes) override;
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override;
    void DestroyPipelineLayout(PipelineLayoutHandle layout) override;
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
    void DestroyPipeline(PipelineHandle pipeline) override;
    QueryPoolHandle CreateTimestampPool(uint32_t count) override;
    void DestroyQueryPool(QueryPoolHandle pool) override;
    bool ReadTimestamps(QueryPoolHandle pool, uint32_t first, uint32_t count, uint64_t* ticks) override;
    void Submit(const std::function<void(CommandList&)>& record) override;
    void SubmitDeferred(const std::function<void(CommandList&)>& record, BufferResource staging) override;
    void ReapDeferred(bool wait) override;
    void WaitIdle() override;
    CommandList* BeginFrame(uint32_t slot) override;
    bool EndFrame(uint32_t slot, RenderTarget* final_image) override;
    bool ReadImage(RenderTarget& image, std::vector<uint8_t>& out) override;

    bool CreateSwapchain(HWND__* window, uint32_t width, uint32_t height, Format format, bool vsync, std::string& error,
                         float hdr_peak_nits) override;
    bool DisplayHdr(HWND__* window, float& peak_nits) override;
    bool InitImGui(Format target_format) override;
    void ImGuiNewFrame() override;
    void RenderImGui(gpu::CommandList& list, ImDrawData* data) override;
    void ShutdownImGui() override;
    void DestroySwapchain() override;
    bool FrameGenAvailable(FrameGenOwner owner, std::string& reason) override;
    void SetFrameGenOwner(FrameGenOwner owner) override { wanted_owner_ = owner; }
    FrameGenOwner ActiveFrameGenOwner() const override { return active_owner_; }
    void SetGenerating(bool on) override;
    void* NativeDevice() const override { return bridge_.Get(); }
    void* NativeQueue() const override { return bridge_.Queue(); }
    bool WrapRenderTarget(void* resource, Format format, Extent2D extent, ImageResource& out) override;
    void SetFrameGenInputs(const FrameGenInputs& inputs) override {
        fg_inputs_ = inputs;
        fg_inputs_set_ = true;
    }
    bool CheckRootSignature(const std::string& shader_dir, std::string& report) const override;
    std::string RuntimeDescription() const override;
    uint32_t DebugErrors() override {
        bridge_.DrainDebugMessages();
        return bridge_.TotalDebugErrors();
    }
    uint64_t AllocatedBytes() const override;
    void DeviceLocalUsage(double& used, double& budget) const override;
    bool OverTestBudget(uint64_t bytes, const char* what) const;

    ID3D12Device* Dev() const { return bridge_.Get(); }
    pt::d3d12::Device& Bridge() { return bridge_; }
    ID3D12RootSignature* RootSignature() const { return root_.Get(); }
    DescriptorHeap& Views() { return views_; }
    DescriptorHeap& Samplers() { return samplers_; }
    DescriptorHeap& Rtvs() { return rtvs_; }
    DescriptorHeap& Dsvs() { return dsvs_; }
    DescriptorHeap& CpuViews() { return cpu_views_; }
    struct Counters {
        uint64_t frames = 0;
        uint64_t pso_creates = 0;
        uint64_t fence_waits = 0;
        double fence_wait_ms = 0.0;
        uint64_t submits = 0;
        uint64_t executes = 0;
        uint64_t barriers = 0;
    };
    Counters& Count() { return counters_; }
    ID3D12PipelineState* Variant(PipelineRecord& p, CullMode cull, FrontFace front, const float bias[3]);
    bool CreateTempBuffer(uint64_t size, ComPtr<ID3D12Resource>& out, D3D12MA::Allocation*& allocation);
    bool CreateRawBuffer(uint64_t size, D3D12_RESOURCE_STATES state, bool uav, ComPtr<ID3D12Resource>& out, D3D12MA::Allocation*& allocation,
                         const char* name);
    void Retire(ComPtr<ID3D12Resource> resource, D3D12MA::Allocation* allocation) { Release(std::move(resource), allocation); }
    bool CreateUploadBuffer(uint64_t size, ComPtr<ID3D12Resource>& out, D3D12MA::Allocation*& allocation, void** mapped);
    bool ReclaimUploadMemory();
    void UploadRetried(uint64_t size, bool succeeded);
    void DeviceLost(const char* where);
    void LostResult(HRESULT hr, const char* where);

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::unique_ptr<CommandListImpl> recorder;
        uint64_t value = 0;
    };
    struct Deferred {
        uint64_t value = 0;
        Frame frame;
        BufferResource staging;
    };
    struct Garbage {
        uint64_t value = 0;
        ComPtr<ID3D12Resource> resource;
        D3D12MA::Allocation* allocation = nullptr;
        DescriptorHeap* heap = nullptr;
        uint32_t index = kNone;
        uint32_t count = 0;
    };

    bool CreateRootSignature(std::string& error);
    bool LoadDxil(const std::string& name, std::vector<uint8_t>& out) const;
    void FillNull(SetRecord& set);
    void WriteNullView(uint32_t index, BindingType type, bool uav);
    bool NewFrame(Frame& frame, const wchar_t* name);
    bool CreatePassMarkers();
    void LogPassMarkers();
    uint64_t Execute(Frame& frame);
    void WaitValue(uint64_t value);
    void Collect();
    void Release(ComPtr<ID3D12Resource> resource, D3D12MA::Allocation* allocation, DescriptorHeap* heap = nullptr, uint32_t index = kNone,
                 uint32_t count = 0);
    void ReleaseDescriptors(DescriptorHeap& heap, uint32_t index, uint32_t count);
    D3D12_CPU_DESCRIPTOR_HANDLE SamplerCpu(const D3D12_SAMPLER_DESC& desc, uint32_t index);

    CreateOptions options_;
    pt::d3d12::Device bridge_;
    pt::d3d12::DxgiPresenter presenter_;
    pt::d3d12::SwapchainOwner* active_ = &presenter_;
    FrameGenOwner active_owner_ = FrameGenOwner::None;
    FrameGenOwner wanted_owner_ = FrameGenOwner::None;
    std::unique_ptr<pt::d3d12::XefgPresenter> xefg_;
    std::unique_ptr<pt::d3d12::SwapchainOwner> fsr_fg_;
    std::unique_ptr<pt::d3d12::SlDlssgPresenter> dlss_fg_;
    FrameGenInputs fg_inputs_;
    bool fg_inputs_set_ = false;
    bool generating_ = true;
    bool CreateOwnerSwapchain(const pt::d3d12::SwapchainDesc& desc, std::string& error);
    D3D12MA::Allocator* allocator_ = nullptr;
    uint64_t test_budget_bytes_ = 0;
    int test_upload_fail_ = 0;
    uint64_t test_upload_attempts_ = 0;
    HRESULT last_upload_hr_ = S_OK;
    uint64_t upload_failures_ = 0;
    uint64_t upload_retries_ = 0;
    std::chrono::steady_clock::time_point upload_failure_logged_{};
    std::chrono::steady_clock::time_point upload_retry_logged_{};
    bool rendering_ = false;
    bool presenting_ = false;
    bool lost_reported_ = false;
    bool hung_ = false;
    uint32_t test_remove_frame_ = 0;
    uint32_t test_late_fence_ = 0;
    uint32_t test_frames_begun_ = 0;
    ComPtr<ID3D12Device2> device2_;
    ComPtr<ID3D12RootSignature> root_;
    DescriptorHeap views_;
    DescriptorHeap samplers_;
    DescriptorHeap rtvs_;
    DescriptorHeap dsvs_;
    DescriptorHeap cpu_views_;
    DeviceInfo info_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t submitted_ = 0;
    std::vector<Frame> frames_;
    std::vector<Frame> spare_;
    void* marker_memory_ = nullptr;
    ComPtr<ID3D12Heap> marker_heap_;
    ComPtr<ID3D12Resource> marker_buffer_;
    std::vector<PassMarkers> markers_;
    uint32_t marker_serial_ = 0;
    std::vector<Deferred> deferred_;
    std::vector<Garbage> garbage_;
    std::mutex mutex_;
    Format swapchain_format_ = Format::Undefined;
    Extent2D swapchain_extent_{};
    uint32_t frame_id_ = 0;
    bool imgui_ = false;
    bool gpu_upload_ = false;
    Counters counters_;
    uint64_t counters_start_ns_ = 0;
    void ReportCounters();
};

}
