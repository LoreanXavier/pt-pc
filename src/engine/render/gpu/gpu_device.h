#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "engine/render/gpu/gpu_command_list.h"
#include "engine/render/gpu/gpu_resources.h"

namespace pt::gpu {

class Device {
public:
    virtual ~Device() = default;

    virtual const DeviceInfo& Info() const = 0;

    virtual bool CreateImage(const ImageDesc& desc, ImageResource& out) = 0;
    virtual void DestroyImage(ImageResource& image) = 0;
    uint64_t ImageDestroyCount() const { return image_destroys_.load(std::memory_order_relaxed); }
    virtual bool CreateBuffer(const BufferDesc& desc, BufferResource& out) = 0;
    virtual void DestroyBuffer(BufferResource& buffer) = 0;
    virtual void FlushMapped(const BufferResource& buffer, uint64_t offset, uint64_t size) {
        (void)buffer;
        (void)offset;
        (void)size;
    }
    virtual void InvalidateMapped(const BufferResource& buffer, uint64_t offset, uint64_t size) {
        (void)buffer;
        (void)offset;
        (void)size;
    }
    virtual uint64_t AllocatedBytes() const { return 0; }
    virtual void DeviceLocalUsage(double& used, double& budget) const {
        used = 0.0;
        budget = 0.0;
    }
    virtual uint64_t ImageBytes(const ImageResource& image) const = 0;
    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
    virtual void DestroySampler(SamplerHandle sampler) = 0;

    virtual BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc& desc) = 0;
    virtual void DestroyBindingLayout(BindingLayoutHandle layout) = 0;
    virtual BindingPoolHandle CreateBindingPool(const BindingPoolDesc& desc) = 0;
    virtual void DestroyBindingPool(BindingPoolHandle pool) = 0;
    virtual BindingSetHandle AllocateBindingSet(BindingPoolHandle pool, BindingLayoutHandle layout, uint32_t variable_count = 0) = 0;
    virtual bool AllocateBindingSets(BindingPoolHandle pool, std::span<const BindingLayoutHandle> layouts, std::span<BindingSetHandle> out) {
        for (size_t i = 0; i < layouts.size() && i < out.size(); ++i) {
            out[i] = AllocateBindingSet(pool, layouts[i]);
            if (!out[i]) {
                return false;
            }
        }
        return true;
    }
    virtual void WriteBindings(std::span<const BindingWrite> writes) = 0;
    virtual PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) = 0;
    virtual void DestroyPipelineLayout(PipelineLayoutHandle layout) = 0;

    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual bool CreateGraphicsPipelines(std::span<const GraphicsPipelineDesc> descs, std::span<PipelineHandle> out) {
        bool ok = true;
        for (size_t i = 0; i < descs.size() && i < out.size(); ++i) {
            out[i] = CreateGraphicsPipeline(descs[i]);
            ok = ok && static_cast<bool>(out[i]);
        }
        return ok;
    }
    virtual bool PreloadShaders(std::span<const char* const> names) {
        (void)names;
        return true;
    }
    virtual void ReleaseShaders(std::span<const char* const> names) { (void)names; }
    virtual void DestroyPipeline(PipelineHandle pipeline) = 0;

    virtual QueryPoolHandle CreateTimestampPool(uint32_t count) = 0;
    virtual void DestroyQueryPool(QueryPoolHandle pool) = 0;
    virtual bool ReadTimestamps(QueryPoolHandle pool, uint32_t first, uint32_t count, uint64_t* ticks) = 0;

    virtual void Submit(const std::function<void(CommandList&)>& record) = 0;
    virtual void SubmitDeferred(const std::function<void(CommandList&)>& record, BufferResource staging) = 0;
    virtual void ReapDeferred(bool wait) = 0;
    virtual void WaitIdle() = 0;

    virtual CommandList* BeginFrame(uint32_t slot) = 0;
    virtual bool EndFrame(uint32_t slot, RenderTarget* final_image) = 0;

    virtual bool ReadImage(RenderTarget& image, std::vector<uint8_t>& out) = 0;

protected:
    void NoteImageDestroyed() { image_destroys_.fetch_add(1, std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> image_destroys_{0};
};

inline bool UploadBuffer(Device& device, BufferResource& dst, const void* data, uint64_t size) {
    BufferResource staging;
    if (!device.CreateBuffer({size, BufferUsage::TransferSrc, true}, staging)) {
        return false;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    device.FlushMapped(staging, 0, size);
    device.Submit([&](CommandList& list) { list.CopyBuffer(staging, 0, dst, 0, size); });
    device.DestroyBuffer(staging);
    return true;
}

}
