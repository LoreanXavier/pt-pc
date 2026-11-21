#include "engine/render/d3d12/backend/d3d12_internal.h"

#include <D3D12MemAlloc.h>
#include <d3dx12/d3dx12.h>

#include <algorithm>
#include <cstring>

#include "engine/core/log.h"

namespace pt::gpu::d3d12 {
namespace {

D3D12_RECT Scissor(Rect2D r) {
    return {r.offset.x, r.offset.y, r.offset.x + static_cast<LONG>(r.extent.width), r.offset.y + static_cast<LONG>(r.extent.height)};
}

D3D12_RESOURCE_STATES BufferState(uint64_t access) {
    if (access & (static_cast<uint64_t>(Access::ShaderWrite) | static_cast<uint64_t>(Access::MemoryWrite))) {
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (access & static_cast<uint64_t>(Access::TransferWrite)) {
        return D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (access & static_cast<uint64_t>(Access::TransferRead)) {
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
}

uint32_t Subresource(const ImageRecord& img, uint32_t mip, uint32_t layer) {
    return mip + layer * img.desc.mip_levels;
}

}

void CommandListImpl::Barriers(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    device_.Count().barriers += count;
    list_->ResourceBarrier(count, barriers);
}

CommandListImpl::CommandListImpl(DeviceImpl& device, ID3D12GraphicsCommandList* list) : device_(device), list_(list) {
    list_->QueryInterface(IID_PPV_ARGS(&list1_));
    list_->QueryInterface(IID_PPV_ARGS(&list9_));
    list_->QueryInterface(IID_PPV_ARGS(&list2_));
}

void CommandListImpl::Begin() {
    ID3D12DescriptorHeap* heaps[] = {device_.Views().Heap(), device_.Samplers().Heap()};
    list_->SetDescriptorHeaps(2, heaps);
    list_->SetGraphicsRootSignature(device_.RootSignature());
    list_->SetComputeRootSignature(device_.RootSignature());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    graphics_ = compute_ = nullptr;
    applied_ = nullptr;
    markers_ = nullptr;
    open_labels_.clear();
    cull_ = CullMode::None;
    front_ = FrontFace::CounterClockwise;
    std::memset(bias_, 0, sizeof(bias_));
    bias_dirty_ = false;
    vertex_buffer_ = nullptr;
    vertex_dirty_ = false;
    vertex_stride_ = 0;
    reclaimed_ = false;
    stage_failed_size_ = UINT64_MAX;
}

bool CommandListImpl::StageUpload(uint64_t size, ComPtr<ID3D12Resource>& upload, D3D12MA::Allocation*& allocation, void** mapped) {
    if (size >= stage_failed_size_) {
        device_.UploadRetried(size, false);
        return false;
    }
    if (device_.CreateUploadBuffer(size, upload, allocation, mapped)) {
        return true;
    }
    if (!reclaimed_) {
        reclaimed_ = true;
        if (!device_.ReclaimUploadMemory()) {
            return false;
        }
    }
    const bool made = device_.CreateUploadBuffer(size, upload, allocation, mapped);
    device_.UploadRetried(size, made);
    if (!made) {
        stage_failed_size_ = std::min(stage_failed_size_, size);
    }
    return made;
}

void CommandListImpl::ResumeAfterNative() {
    ID3D12DescriptorHeap* heaps[] = {device_.Views().Heap(), device_.Samplers().Heap()};
    list_->SetDescriptorHeaps(2, heaps);
    list_->SetGraphicsRootSignature(device_.RootSignature());
    list_->SetComputeRootSignature(device_.RootSignature());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    applied_ = nullptr;
    bias_dirty_ = true;
    vertex_dirty_ = vertex_buffer_ != nullptr;
}

void CommandListImpl::Transition(ImageRecord& image, D3D12_RESOURCE_STATES state) {
    if (image.state == state) {
        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(image.resource.Get());
            Barriers(1, &uav);
        }
        return;
    }
    const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(image.resource.Get(), image.state, state);
    Barriers(1, &b);
    image.state = state;
}

void CommandListImpl::Transition(BufferRecord& buffer, D3D12_RESOURCE_STATES state) {
    if (buffer.dirty_end > buffer.dirty_begin) {
        const uint64_t begin = buffer.dirty_begin;
        const uint64_t size = buffer.dirty_end - begin;
        ComPtr<ID3D12Resource> upload;
        D3D12MA::Allocation* allocation = nullptr;
        void* mapped = nullptr;
        if (StageUpload(size, upload, allocation, &mapped)) {
            std::memcpy(mapped, buffer.host_data.data() + begin, size);
            upload->Unmap(0, nullptr);
            buffer.dirty_begin = buffer.dirty_end = 0;
            Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST);
            list_->CopyBufferRegion(buffer.resource.Get(), begin, upload.Get(), 0, size);
            Keep(std::move(upload), allocation);
        }
    }
    if (buffer.heap == HeapKind::Upload || buffer.heap == HeapKind::Readback) {
        return;
    }
    if (buffer.state == state) {
        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(buffer.resource.Get());
            Barriers(1, &uav);
        }
        return;
    }
    constexpr D3D12_RESOURCE_STATES kWrites = D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RENDER_TARGET |
                                              D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT;
    if (state != D3D12_RESOURCE_STATE_COMMON && !(state & kWrites) && !(buffer.state & kWrites) && (buffer.state & state) == state) {
        return;
    }
    const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(), buffer.state, state);
    Barriers(1, &b);
    buffer.state = state;
}

void CommandListImpl::UseTargets(std::span<const TargetUse> uses) {
    D3D12_RESOURCE_BARRIER barriers[16];
    UINT count = 0;
    for (const TargetUse& use : uses) {
        if (!use.target || !use.target->Valid() || count == std::size(barriers)) {
            continue;
        }
        ImageRecord* img = Rec<ImageRecord>(use.target->image.image);
        use.target->layout = use.layout;
        const D3D12_RESOURCE_STATES state = StateFor(use.layout);
        if (use.layout == Layout::Undefined) {
            continue;
        }
        if (img->state == state) {
            if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                barriers[count++] = CD3DX12_RESOURCE_BARRIER::UAV(img->resource.Get());
            }
            continue;
        }
        barriers[count++] = CD3DX12_RESOURCE_BARRIER::Transition(img->resource.Get(), img->state, state);
        img->state = state;
    }
    if (count) {
        Barriers(count, barriers);
    }
}

void CommandListImpl::Barrier(const ImageBarrier& barrier) {
    ImageRecord* img = Rec<ImageRecord>(barrier.image);
    if (img && barrier.new_layout != Layout::Undefined) {
        Transition(*img, StateFor(barrier.new_layout));
    }
}

void CommandListImpl::BufferBarrier(BufferHandle buffer, PipelineStage, uint64_t, PipelineStage, uint64_t dst_access) {
    if (BufferRecord* b = Rec<BufferRecord>(buffer)) {
        Transition(*b, BufferState(dst_access));
    }
}

void CommandListImpl::GlobalBarrier(PipelineStage, uint64_t, PipelineStage, uint64_t) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;
    Barriers(1, &b);
}

void CommandListImpl::BeginPass(Rect2D area, std::span<const ColorOutput> colors, RenderTarget* depth, bool depth_read_only, bool clear_depth) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
    UINT count = 0;
    const D3D12_RECT rect = Scissor(area);
    for (const ColorOutput& c : colors) {
        ImageRecord* img = Rec<ImageRecord>(c.target->image.image);
        if (img->rtv == kNone || count == 8) {
            LogError("d3d12 backend: a pass draws into an image without colour attachment usage");
            continue;
        }
        Transition(*img, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[count++] = device_.Rtvs().Cpu(img->rtv);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    ImageRecord* depth_img = depth && depth->Valid() ? Rec<ImageRecord>(depth->image.image) : nullptr;
    if (depth_img) {
        if (depth_read_only) {
            if (!(depth_img->state & D3D12_RESOURCE_STATE_DEPTH_READ)) {
                Transition(*depth_img, StateFor(Layout::DepthReadOnly));
            }
            dsv = device_.Dsvs().Cpu(depth_img->dsv_read_only);
        } else {
            Transition(*depth_img, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            dsv = device_.Dsvs().Cpu(depth_img->dsv);
        }
    }
    list_->OMSetRenderTargets(count, rtvs, FALSE, depth_img ? &dsv : nullptr);
    UINT i = 0;
    for (const ColorOutput& c : colors) {
        if (i >= count) {
            break;
        }
        if (c.clear) {
            list_->ClearRenderTargetView(rtvs[i], c.clear_value.float32, 1, &rect);
        }
        ++i;
    }
    if (depth_img && clear_depth) {
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &rect);
    }
    SetViewport(area);
    SetCullMode(CullMode::None);
    SetFrontFace(FrontFace::CounterClockwise);
}

void CommandListImpl::BeginRendering(Rect2D area, std::span<const Attachment> colors) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
    UINT count = 0;
    const D3D12_RECT rect = Scissor(area);
    for (const Attachment& c : colors) {
        ImageRecord* img = reinterpret_cast<ImageRecord*>(static_cast<uintptr_t>(c.view.bits));
        if (!img || img->rtv == kNone || count == 8) {
            LogError("d3d12 backend: BeginRendering into an image without colour attachment usage");
            continue;
        }
        Transition(*img, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[count] = device_.Rtvs().Cpu(img->rtv);
        if (c.load == LoadOp::Clear) {
            list_->ClearRenderTargetView(rtvs[count], c.clear_value.float32, 1, &rect);
        }
        ++count;
    }
    list_->OMSetRenderTargets(count, rtvs, FALSE, nullptr);
}

void CommandListImpl::EndPass() {}

void CommandListImpl::SetViewport(Rect2D area) {
    const D3D12_VIEWPORT viewport{static_cast<float>(area.offset.x), static_cast<float>(area.offset.y), static_cast<float>(area.extent.width),
                                  static_cast<float>(area.extent.height), 0.0f, 1.0f};
    const D3D12_RECT scissor = Scissor(area);
    list_->RSSetViewports(1, &viewport);
    list_->RSSetScissorRects(1, &scissor);
}

void CommandListImpl::SetCullMode(CullMode mode) {
    cull_ = mode;
}

void CommandListImpl::SetFrontFace(FrontFace face) {
    front_ = face;
}

void CommandListImpl::SetDepthBias(float constant, float clamp, float slope) {
    bias_[0] = constant;
    bias_[1] = clamp;
    bias_[2] = slope;
    bias_dirty_ = true;
}

void CommandListImpl::SetDepthBounds(float min, float max) {
    if (list1_) {
        list1_->OMSetDepthBounds(min, max);
    }
}

void CommandListImpl::BindPipeline(BindPoint point, PipelineHandle pipeline) {
    PipelineRecord* p = Rec<PipelineRecord>(pipeline);
    if (point == BindPoint::Compute) {
        compute_ = p;
    } else {
        graphics_ = p;
        if (p && p->stride != vertex_stride_) {
            vertex_dirty_ = true;
        }
    }
}

void CommandListImpl::BindSets(BindPoint point, PipelineLayoutHandle, uint32_t first_set, std::span<const BindingSetHandle> sets) {
    const bool compute = point == BindPoint::Compute;
    auto table = [&](UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h) {
        if (compute) {
            list_->SetComputeRootDescriptorTable(param, h);
        } else {
            list_->SetGraphicsRootDescriptorTable(param, h);
        }
    };
    for (size_t i = 0; i < sets.size(); ++i) {
        const uint32_t index = first_set + static_cast<uint32_t>(i);
        const SetRecord* set = Rec<SetRecord>(sets[i]);
        if (!set || index >= kMaxSets) {
            continue;
        }
        for (const auto& [key, buffer] : set->uploads) {
            Transition(*buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        table(RootLayout::kSetViews + index, device_.Views().Gpu(set->views));
        if (set->samplers != kNone) {
            table(RootLayout::kSetSamplers + index, device_.Samplers().Gpu(set->samplers));
        }
        for (const SetRecord::Array& a : set->arrays) {
            const int param = RootLayout::ArrayParam(index, a.binding);
            if (param < 0) {
                continue;
            }
            table(static_cast<UINT>(param), a.sampler ? device_.Samplers().Gpu(a.base) : device_.Views().Gpu(a.base));
        }
    }
}

void CommandListImpl::PushConstants(PipelineLayoutHandle, ShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data) {
    const UINT values = size / 4;
    const UINT first = offset / 4;
    if (stages.Has(ShaderStage::Compute)) {
        list_->SetComputeRoot32BitConstants(RootLayout::kPush, values, data, first);
    }
    if (stages.bits & (static_cast<uint32_t>(ShaderStage::Vertex) | static_cast<uint32_t>(ShaderStage::Fragment))) {
        list_->SetGraphicsRoot32BitConstants(RootLayout::kPush, values, data, first);
    }
}

void CommandListImpl::BindVertexBuffer(BufferHandle buffer, uint64_t offset) {
    vertex_buffer_ = Rec<BufferRecord>(buffer);
    vertex_offset_ = offset;
    vertex_dirty_ = true;
}

void CommandListImpl::BindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) {
    BufferRecord* b = Rec<BufferRecord>(buffer);
    if (!b) {
        return;
    }
    Transition(*b, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = b->resource->GetGPUVirtualAddress() + offset;
    v.SizeInBytes = static_cast<UINT>(b->size - offset);
    v.Format = type == IndexType::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    list_->IASetIndexBuffer(&v);
}

void CommandListImpl::Apply(ID3D12PipelineState* state) {
    if (state && state != applied_) {
        list_->SetPipelineState(state);
        applied_ = state;
    }
}

void CommandListImpl::FlushGraphics() {
    if (!graphics_) {
        return;
    }
    const bool fixed = graphics_->desc.fixed_state;
    Apply(device_.Variant(*graphics_, fixed ? graphics_->desc.cull : cull_, fixed ? FrontFace::CounterClockwise : front_, bias_));
    if (graphics_->dynamic_bias && bias_dirty_ && list9_) {
        list9_->RSSetDepthBias(bias_[0], bias_[1], bias_[2]);
        bias_dirty_ = false;
    }
    if (vertex_dirty_ && vertex_buffer_ && graphics_->stride) {
        Transition(*vertex_buffer_, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        D3D12_VERTEX_BUFFER_VIEW v{};
        v.BufferLocation = vertex_buffer_->resource->GetGPUVirtualAddress() + vertex_offset_;
        v.SizeInBytes = static_cast<UINT>(vertex_buffer_->size - vertex_offset_);
        v.StrideInBytes = graphics_->stride;
        list_->IASetVertexBuffers(0, 1, &v);
        vertex_stride_ = graphics_->stride;
        vertex_dirty_ = false;
    }
}

void CommandListImpl::FlushCompute() {
    if (compute_) {
        Apply(compute_->compute_state.Get());
    }
}

void CommandListImpl::Draw(uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) {
    FlushGraphics();
    list_->DrawInstanced(vertices, instances, first_vertex, first_instance);
}

void CommandListImpl::DrawIndexed(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    FlushGraphics();
    list_->DrawIndexedInstanced(indices, instances, first_index, vertex_offset, first_instance);
}

void CommandListImpl::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
    FlushCompute();
    list_->Dispatch(x, y, z);
}

void CommandListImpl::CopyImage(const ImageResource& src, Layout, const ImageResource& dst, Layout, std::span<const ImageCopy> regions) {
    ImageRecord* s = Rec<ImageRecord>(src.image);
    ImageRecord* d = Rec<ImageRecord>(dst.image);
    if (!s || !d) {
        return;
    }
    Transition(*s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(*d, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const ImageCopy& r : regions) {
        for (uint32_t layer = 0; layer < r.src.layers; ++layer) {
            const D3D12_TEXTURE_COPY_LOCATION from = CD3DX12_TEXTURE_COPY_LOCATION(s->resource.Get(), Subresource(*s, r.src.mip, r.src.base_layer + layer));
            const D3D12_TEXTURE_COPY_LOCATION to = CD3DX12_TEXTURE_COPY_LOCATION(d->resource.Get(), Subresource(*d, r.dst.mip, r.dst.base_layer + layer));
            const D3D12_BOX box{static_cast<UINT>(r.src_offset[0]), static_cast<UINT>(r.src_offset[1]), static_cast<UINT>(r.src_offset[2]),
                                r.src_offset[0] + r.extent.width, r.src_offset[1] + r.extent.height,
                                r.src_offset[2] + std::max(r.extent.depth, 1u)};
            const auto mip_size = [](uint32_t size, uint32_t mip) { return std::max(size >> mip, 1u); };
            const bool whole = r.src_offset[0] == 0 && r.src_offset[1] == 0 && r.src_offset[2] == 0 && r.dst_offset[0] == 0 &&
                               r.dst_offset[1] == 0 && r.dst_offset[2] == 0 && r.extent.width == mip_size(s->desc.extent.width, r.src.mip) &&
                               r.extent.height == mip_size(s->desc.extent.height, r.src.mip) &&
                               std::max(r.extent.depth, 1u) == mip_size(std::max(s->desc.extent.depth, 1u), r.src.mip) &&
                               r.extent.width == mip_size(d->desc.extent.width, r.dst.mip) &&
                               r.extent.height == mip_size(d->desc.extent.height, r.dst.mip) &&
                               std::max(r.extent.depth, 1u) == mip_size(std::max(d->desc.extent.depth, 1u), r.dst.mip);
            list_->CopyTextureRegion(&to, r.dst_offset[0], r.dst_offset[1], r.dst_offset[2], &from, whole ? nullptr : &box);
        }
    }
}

void CommandListImpl::CopyFootprint(bool to_image, const BufferRecord& buffer, ImageRecord& image, const BufferImageCopy& r) {
    const FormatInfo info = Describe(image.desc.format);
    const uint32_t width = r.row_length ? r.row_length : r.extent.width;
    const uint32_t height = r.image_height ? r.image_height : r.extent.height;
    const uint32_t row_bytes = info.block ? ((width + 3) / 4) * info.bytes : width * info.bytes;
    const uint32_t rows = info.block ? (r.extent.height + 3) / 4 : r.extent.height;
    const uint32_t slice_rows = info.block ? (height + 3) / 4 : height;
    const uint32_t depth = std::max(r.extent.depth, 1u);
    const uint32_t copy_row = info.block ? ((r.extent.width + 3) / 4) * info.bytes : r.extent.width * info.bytes;
    const bool aligned = (r.buffer_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) == 0 && (row_bytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) == 0;
    for (uint32_t layer = 0; layer < r.image.layers; ++layer) {
        const uint64_t layer_offset = r.buffer_offset + static_cast<uint64_t>(layer) * row_bytes * slice_rows * depth;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        fp.Footprint.Format = image.resource->GetDesc().Format;
        fp.Footprint.Width = r.extent.width;
        fp.Footprint.Height = r.extent.height;
        fp.Footprint.Depth = depth;
        ID3D12Resource* placed = buffer.resource.Get();
        ComPtr<ID3D12Resource> temp;
        D3D12MA::Allocation* temp_alloc = nullptr;
        const uint32_t pitch = (copy_row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        if (aligned && width == r.extent.width) {
            fp.Offset = layer_offset;
            fp.Footprint.RowPitch = row_bytes;
        } else {
            fp.Offset = 0;
            fp.Footprint.RowPitch = pitch;
            if (!device_.CreateTempBuffer(static_cast<uint64_t>(pitch) * rows * depth, temp, temp_alloc)) {
                LogError("d3d12 backend: no staging buffer for an unaligned image copy");
                return;
            }
            placed = temp.Get();
            if (to_image) {
                for (uint32_t z = 0; z < depth; ++z) {
                    for (uint32_t y = 0; y < rows; ++y) {
                        list_->CopyBufferRegion(temp.Get(), (static_cast<uint64_t>(z) * rows + y) * pitch, buffer.resource.Get(),
                                                layer_offset + (static_cast<uint64_t>(z) * slice_rows + y) * row_bytes, copy_row);
                    }
                }
                const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(temp.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                                      D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barriers(1, &b);
            }
        }
        const UINT sub = Subresource(image, r.image.mip, r.image.base_layer + layer);
        if (to_image) {
            const D3D12_TEXTURE_COPY_LOCATION to = CD3DX12_TEXTURE_COPY_LOCATION(image.resource.Get(), sub);
            const D3D12_TEXTURE_COPY_LOCATION from = CD3DX12_TEXTURE_COPY_LOCATION(placed, fp);
            list_->CopyTextureRegion(&to, r.image_offset[0], r.image_offset[1], r.image_offset[2], &from, nullptr);
        } else {
            const D3D12_TEXTURE_COPY_LOCATION to = CD3DX12_TEXTURE_COPY_LOCATION(placed, fp);
            const D3D12_TEXTURE_COPY_LOCATION from = CD3DX12_TEXTURE_COPY_LOCATION(image.resource.Get(), sub);
            const D3D12_BOX box{static_cast<UINT>(r.image_offset[0]), static_cast<UINT>(r.image_offset[1]), static_cast<UINT>(r.image_offset[2]),
                                r.image_offset[0] + r.extent.width, r.image_offset[1] + r.extent.height, r.image_offset[2] + depth};
            list_->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            if (temp) {
                const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(temp.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                                      D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barriers(1, &b);
                for (uint32_t z = 0; z < depth; ++z) {
                    for (uint32_t y = 0; y < rows; ++y) {
                        list_->CopyBufferRegion(buffer.resource.Get(), layer_offset + (static_cast<uint64_t>(z) * slice_rows + y) * row_bytes, temp.Get(),
                                                (static_cast<uint64_t>(z) * rows + y) * pitch, copy_row);
                    }
                }
            }
        }
        if (temp) {
            Keep(std::move(temp), temp_alloc);
        }
    }
}

void CommandListImpl::CopyImageToBuffer(const ImageResource& src, Layout, const BufferResource& dst, std::span<const BufferImageCopy> regions) {
    ImageRecord* s = Rec<ImageRecord>(src.image);
    BufferRecord* d = Rec<BufferRecord>(dst.buffer);
    if (!s || !d) {
        return;
    }
    Transition(*s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(*d, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const BufferImageCopy& r : regions) {
        CopyFootprint(false, *d, *s, r);
    }
}

void CommandListImpl::CopyBufferToImage(const BufferResource& src, const ImageResource& dst, Layout, std::span<const BufferImageCopy> regions) {
    BufferRecord* s = Rec<BufferRecord>(src.buffer);
    ImageRecord* d = Rec<ImageRecord>(dst.image);
    if (!s || !d) {
        return;
    }
    Transition(*s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(*d, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const BufferImageCopy& r : regions) {
        CopyFootprint(true, *s, *d, r);
    }
}

void CommandListImpl::CopyBuffer(const BufferResource& src, uint64_t src_offset, const BufferResource& dst, uint64_t dst_offset, uint64_t size) {
    BufferRecord* s = Rec<BufferRecord>(src.buffer);
    BufferRecord* d = Rec<BufferRecord>(dst.buffer);
    if (!s || !d) {
        return;
    }
    Transition(*s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(*d, D3D12_RESOURCE_STATE_COPY_DEST);
    list_->CopyBufferRegion(d->resource.Get(), dst_offset, s->resource.Get(), src_offset, size);
}

void CommandListImpl::FillBuffer(const BufferResource& buffer, uint64_t offset, uint64_t size, uint32_t value) {
    BufferRecord* b = Rec<BufferRecord>(buffer.buffer);
    if (!b || offset >= b->size) {
        return;
    }
    if (size == ~0ull || offset + size > b->size) {
        size = (b->size - offset) & ~3ull;
    }
    ComPtr<ID3D12Resource> upload;
    D3D12MA::Allocation* allocation = nullptr;
    void* mapped = nullptr;
    if (size == 0 || !StageUpload(size, upload, allocation, &mapped)) {
        LogError("d3d12 backend: FillBuffer: no upload buffer");
        return;
    }
    uint32_t* words = static_cast<uint32_t*>(mapped);
    for (uint64_t i = 0; i < size / 4; ++i) {
        words[i] = value;
    }
    upload->Unmap(0, nullptr);
    Transition(*b, D3D12_RESOURCE_STATE_COPY_DEST);
    list_->CopyBufferRegion(b->resource.Get(), offset, upload.Get(), 0, size);
    Keep(std::move(upload), allocation);
}

void CommandListImpl::ClearColorImage(const ImageResource& image, Layout layout, const ClearColor& value, uint32_t, uint32_t) {
    ImageRecord* img = Rec<ImageRecord>(image.image);
    if (!img) {
        return;
    }
    const D3D12_RESOURCE_STATES back = StateFor(layout);
    if (img->rtv != kNone) {
        Transition(*img, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list_->ClearRenderTargetView(device_.Rtvs().Cpu(img->rtv), value.float32, 0, nullptr);
    } else if (img->clear_uav_gpu != kNone) {
        Transition(*img, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list_->ClearUnorderedAccessViewFloat(device_.Views().Gpu(img->clear_uav_gpu), device_.CpuViews().Cpu(img->clear_uav_cpu),
                                             img->resource.Get(), value.float32, 0, nullptr);
    } else {
        LogError("d3d12 backend: ClearColorImage on an image that is neither a colour attachment nor a storage image");
        return;
    }
    if (back != D3D12_RESOURCE_STATE_COMMON) {
        Transition(*img, back);
    }
}

void CommandListImpl::ResetQueries(QueryPoolHandle, uint32_t, uint32_t) {}

void CommandListImpl::WriteTimestamp(QueryPoolHandle pool, uint32_t index) {
    QueryPoolRecord* q = Rec<QueryPoolRecord>(pool);
    if (!q || index >= q->count) {
        return;
    }
    list_->EndQuery(q->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    list_->ResolveQueryData(q->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index, 1, q->readback.Get(), sizeof(uint64_t) * index);
}

void CommandListImpl::BeginLabel(const char* name) {
    list_->BeginEvent(1, name, static_cast<UINT>(std::strlen(name) + 1));
    if (!markers_ || !list2_) {
        return;
    }
    if (markers_->count >= PassMarkers::kLabels) {
        open_labels_.push_back(kNone);
        return;
    }
    const uint32_t index = markers_->count++;
    markers_->names[index] = name;
    open_labels_.push_back(index);
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER begun{markers_->gpu + uint64_t(index) * 8, markers_->serial};
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
    list2_->WriteBufferImmediate(1, &begun, &mode);
}

void CommandListImpl::EndLabel() {
    list_->EndEvent();
    if (open_labels_.empty()) {
        return;
    }
    const uint32_t index = open_labels_.back();
    open_labels_.pop_back();
    if (index == kNone || !markers_ || !list2_) {
        return;
    }
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER ended{markers_->gpu + uint64_t(index) * 8 + 4, markers_->serial};
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    list2_->WriteBufferImmediate(1, &ended, &mode);
}

}
