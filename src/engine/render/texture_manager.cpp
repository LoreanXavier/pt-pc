#include "engine/platform/os.h"
#include "engine/render/texture_manager.h"

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

#include "engine/assets/ftex.h"
#include "engine/core/log.h"
#include "engine/fs/mods.h"
#include "engine/render/vk/vk_device.h"

namespace pt {
namespace {

bool ModImageLevels(const std::vector<uint8_t>& png, bool srgb, std::vector<std::vector<uint8_t>>& levels, uint32_t& width,
                    uint32_t& height, std::string& error) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4);
    if (!pixels) {
        error = stbi_failure_reason() ? stbi_failure_reason() : "not a PNG";
        return false;
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        stbi_image_free(pixels);
        error = std::format("{} x {} is outside 1 to 8192 px", w, h);
        return false;
    }
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    levels.clear();
    levels.emplace_back(pixels, pixels + size_t(width) * height * 4);
    stbi_image_free(pixels);
    float to_linear[256];
    for (int i = 0; i < 256; ++i) {
        const float f = i / 255.0f;
        to_linear[i] = srgb ? (f <= 0.04045f ? f / 12.92f : std::pow((f + 0.055f) / 1.055f, 2.4f)) : f;
    }
    uint32_t lw = width, lh = height;
    while (lw > 1 || lh > 1) {
        const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
        const std::vector<uint8_t>& above = levels.back();
        std::vector<uint8_t> next(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; ++y) {
            for (uint32_t x = 0; x < nw; ++x) {
                for (uint32_t c = 0; c < 4; ++c) {
                    float sum = 0.0f;
                    for (uint32_t dy = 0; dy < 2; ++dy) {
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            const uint8_t v = above[(size_t(std::min(y * 2 + dy, lh - 1)) * lw + std::min(x * 2 + dx, lw - 1)) * 4 + c];
                            sum += c < 3 ? to_linear[v] : v / 255.0f;
                        }
                    }
                    float f = sum * 0.25f;
                    if (srgb && c < 3) {
                        f = f <= 0.0031308f ? f * 12.92f : 1.055f * std::pow(f, 1.0f / 2.4f) - 0.055f;
                    }
                    next[(size_t(y) * nw + x) * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(f * 255.0f), 0l, 255l));
                }
            }
        }
        levels.push_back(std::move(next));
        lw = nw;
        lh = nh;
    }
    return true;
}

}

uint32_t FormatBlockBytes(gpu::Format gpu_format, bool& compressed) {
    const VkFormat format = gpu::vk::Native(gpu_format);
    compressed = true;
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return 16;
    default: break;
    }
    compressed = false;
    switch (format) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: return 2;
    case VK_FORMAT_R16_SFLOAT: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;
    }
}

bool TextureManager::Init(gpu::Device& device) {
    device_ = &device;
    const bool d3d12 = device.Info().backend == gpu::Backend::D3D12;
    base_sampler_ = CreateSampler(0);
    sampler_ = base_sampler_;

    const gpu::BindingFlags bindless = gpu::BindingFlag::PartiallyBound | gpu::BindingFlag::UpdateAfterBind | gpu::BindingFlag::UpdateUnusedWhilePending;
    gpu::BindingLayoutDesc layout;
    layout.update_after_bind = true;
    layout.bindings = {{0, gpu::BindingType::CombinedImageSampler, kMaxTextures, gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute, bindless},
                       {1, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::Fragment | gpu::ShaderStage::Vertex | gpu::ShaderStage::Compute,
                        gpu::BindingFlag::UpdateAfterBind},
                       {2, gpu::BindingType::CombinedImageSampler, kMaxCubeTextures, gpu::ShaderStage::Fragment, bindless}};
    if (d3d12) {
        layout.bindings.push_back({3, gpu::BindingType::Sampler, 1, gpu::ShaderStage::All, gpu::BindingFlag::UpdateAfterBind});
    }
    set_layout_ = device.CreateBindingLayout(layout);
    if (!set_layout_) {
        LogError("textures: texture set layout failed");
        return false;
    }
    gpu::BindingPoolDesc pool;
    pool.sizes = {{gpu::BindingType::CombinedImageSampler, kMaxTextures + kMaxCubeTextures}, {gpu::BindingType::StorageBuffer, 1}};
    if (d3d12) {
        pool.sizes.push_back({gpu::BindingType::Sampler, 1});
    }
    pool.max_sets = 1;
    pool.update_after_bind = true;
    pool_ = device.CreateBindingPool(pool);
    set_ = device.AllocateBindingSet(pool_, set_layout_);
    if (!set_) {
        LogError("textures: texture set failed");
        return false;
    }
    gpu::BufferDesc materials;
    materials.size = sizeof(MaterialGpu) * kMaxMaterials;
    materials.usage = gpu::BufferUsage::StorageBuffer;
    materials.host_visible = true;
    materials.gpu_read_only = true;
    materials.name = "materials";
    if (!device.CreateBuffer(materials, material_buffer_)) {
        return false;
    }
    const gpu::BufferBinding buffer_binding{material_buffer_.buffer, 0, ~0ull};
    gpu::BindingWrite write;
    write.set = set_;
    write.binding = 1;
    write.type = gpu::BindingType::StorageBuffer;
    write.buffers = std::span(&buffer_binding, 1);
    device.WriteBindings(std::span(&write, 1));
    if (d3d12) {
        gpu::BindingWrite sampler;
        sampler.set = set_;
        sampler.binding = 3;
        sampler.type = gpu::BindingType::Sampler;
        sampler.samplers = std::span(&sampler_, 1);
        device.WriteBindings(std::span(&sampler, 1));
    }

    const uint8_t white[4] = {255, 255, 255, 255};
    const uint8_t flat[4] = {128, 128, 255, 255};
    const uint8_t black[4] = {0, 0, 0, 255};
    const TextureMip white_mip{1, 1, white};
    const TextureMip flat_mip{1, 1, flat};
    const TextureMip black_mip{1, 1, black};
    Create("builtin:white", gpu::Format::R8G8B8A8Unorm, {&white_mip, 1});
    Create("builtin:flat_normal", gpu::Format::R8G8B8A8Unorm, {&flat_mip, 1});
    Create("builtin:black", gpu::Format::R8G8B8A8Unorm, {&black_mip, 1});
    const TextureMip black_faces[6] = {black_mip, black_mip, black_mip, black_mip, black_mip, black_mip};
    Create("builtin:black_cube", gpu::Format::R8G8B8A8Unorm, black_faces, 6, true);
    const uint8_t grey[4] = {128, 128, 128, 255};
    const TextureMip grey_mip{1, 1, grey};
    Create("builtin:grey", gpu::Format::R8G8B8A8Unorm, {&grey_mip, 1});
    Create("builtin:grey_srgb", gpu::Format::R8G8B8A8Srgb, {&grey_mip, 1});
    if (images_.size() != kGreySrgb + 1) return false;
    AddMaterial(MaterialGpu{});
    FlushMaterials();
    return true;
}

void TextureManager::Shutdown() {
    for (const std::shared_ptr<ParallelTask>& worker : decode_workers_) {
        worker->Wait();
    }
    decode_workers_.clear();
    decode_jobs_.clear();
    decoding_.clear();
    decode_order_.clear();
    if (!device_) {
        return;
    }
    for (gpu::ImageResource& image : images_) {
        device_->DestroyImage(image);
    }
    images_.clear();
    cube_.clear();
    names_.clear();
    model_refs_.clear();
    pinned_.clear();
    free_.clear();
    by_name_.clear();
    cube_slots_.clear();
    device_->DestroyBuffer(material_buffer_);
    device_->DestroyBindingPool(pool_);
    device_->DestroyBindingLayout(set_layout_);
    if (sampler_ != base_sampler_) {
        device_->DestroySampler(sampler_);
    }
    device_->DestroySampler(base_sampler_);
    sampler_ = {};
    base_sampler_ = {};
    pool_ = {};
    set_ = {};
    set_layout_ = {};
    anisotropy_ = 0;
    device_ = nullptr;
}

uint32_t TextureManager::CubeSlot(uint32_t texture) const {
    auto it = cube_slots_.find(texture);
    return it == cube_slots_.end() ? kNoCube : it->second;
}

gpu::SamplerHandle TextureManager::CreateSampler(int anisotropy) const {
    gpu::SamplerDesc desc;
    desc.max_anisotropy = anisotropy > 1 ? static_cast<float>(anisotropy) : 1.0f;
    return device_->CreateSampler(desc);
}

int TextureManager::SetAnisotropy(int level) {
    if (!device_) {
        return 0;
    }
    const int limit = static_cast<int>(device_->Info().max_sampler_anisotropy);
    level = level > 1 ? std::min(level, limit) : 0;
    if (level == anisotropy_) {
        return anisotropy_;
    }
    const gpu::SamplerHandle sampler = level > 1 ? CreateSampler(level) : base_sampler_;
    if (!sampler) {
        return anisotropy_;
    }
    device_->WaitIdle();
    std::vector<gpu::ImageBinding> infos(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        uint32_t shown = static_cast<uint32_t>(i);
        const uint32_t view = cube_[i] || !images_[shown].view ? kWhite : shown;
        infos[i] = {images_[view].view, gpu::Layout::ShaderReadOnly, sampler, &images_[view]};
    }
    if (!infos.empty()) {
        gpu::BindingWrite write;
        write.set = set_;
        write.binding = 0;
        write.type = gpu::BindingType::CombinedImageSampler;
        write.images = infos;
        device_->WriteBindings(std::span(&write, 1));
    }
    if (device_->Info().backend == gpu::Backend::D3D12) {
        gpu::BindingWrite write;
        write.set = set_;
        write.binding = 3;
        write.type = gpu::BindingType::Sampler;
        write.samplers = std::span(&sampler, 1);
        device_->WriteBindings(std::span(&write, 1));
    }
    if (sampler_ != base_sampler_) {
        device_->DestroySampler(sampler_);
    }
    sampler_ = sampler;
    anisotropy_ = level;
    LogInfo("textures: anisotropic filtering {}", level > 1 ? std::format("{}x", level) : std::string("off (trilinear)"));
    return anisotropy_;
}

uint32_t TextureManager::Find(const std::string& name) const {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return kWhite;
    Pin(it->second);
    return it->second;
}

void TextureManager::Pin(uint32_t index) const {
    if (index < pinned_.size()) pinned_[index] = 1;
}

void TextureManager::AddModelRef(uint32_t index) {
    if (index < model_refs_.size()) ++model_refs_[index];
}

uint64_t TextureManager::ReleaseModelRef(uint32_t index) {
    if (index >= model_refs_.size() || model_refs_[index] == 0) return 0;
    if (--model_refs_[index] > 0 || pinned_[index] || index <= kGreySrgb) return 0;
    return FreeTexture(index);
}

uint64_t TextureManager::FreeTexture(uint32_t index) {
    uint64_t bytes = 0;
    auto destroy = [&](uint32_t slot) {
        if (slot >= images_.size() || !images_[slot].image) return;
        bytes += device_->ImageBytes(images_[slot]);
        device_->DestroyImage(images_[slot]);
        if (auto it = by_name_.find(names_[slot]); it != by_name_.end() && it->second == slot) by_name_.erase(it);
        names_[slot].clear();
        model_refs_[slot] = 0;
        pinned_[slot] = 0;
        cube_[slot] = 0;
        if (redirects_.erase(slot) > 0) materials_dirty_ = true;
        for (auto it = redirects_.begin(); it != redirects_.end();) {
            if (it->second == slot) {
                it = redirects_.erase(it);
                materials_dirty_ = true;
            } else {
                ++it;
            }
        }
        free_.push_back(slot);
    };
    destroy(index);
    UpdateTextureDescriptor(index);
    return bytes;
}

uint32_t TextureManager::Create(const std::string& name, gpu::Format format, std::span<const TextureMip> mips, uint32_t layers, bool cube) {
    if (auto it = by_name_.find(name); it != by_name_.end()) {
        if (!model_scope_) Pin(it->second);
        return it->second;
    }
    if (mips.empty() || (images_.size() >= kMaxTextures && (free_.empty() || cube))) {
        return kWhite;
    }
    gpu::ImageResource image;
    uint32_t mip_count = static_cast<uint32_t>(mips.size()) / layers;
    uint32_t dropped = 0;
    if (memory_short_ && layers == 1 && mip_count > 1 && std::max(mips[0].width, mips[0].height) > 512) {
        mips = mips.subspan(1);
        --mip_count;
        ++dropped;
    }
    const auto create_image = [&] {
        gpu::ImageDesc desc;
        desc.format = format;
        desc.extent = {mips[0].width, mips[0].height, 1};
        desc.usage = gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferDst;
        desc.mip_levels = mip_count;
        desc.layers = layers;
        desc.cube = cube;
        desc.name = "textures";
        return device_->CreateImage(desc, image);
    };
    while (!create_image()) {
        if (!memory_short_) {
            memory_short_ = true;
            LogWarn("textures: out of video memory: from now on textures over 512 px load without their largest mip");
        }
        if (layers != 1 || mip_count < 2 || std::max(mips[1].width, mips[1].height) < 64) {
            LogWarn("texture: {} not loaded: out of video memory", name);
            return kWhite;
        }
        mips = mips.subspan(1);
        --mip_count;
        ++dropped;
    }
    if (dropped) {
        LogWarn("texture: {} loaded at {}x{} without its {} largest mips: out of video memory", name, mips[0].width, mips[0].height, dropped);
    }
    uint64_t total = 0;
    for (const TextureMip& mip : mips) {
        total += (mip.data.size() + 15) & ~uint64_t(15);
    }
    gpu::BufferResource staging;
    gpu::BufferDesc staging_desc;
    staging_desc.size = total;
    staging_desc.usage = gpu::BufferUsage::TransferSrc;
    staging_desc.host_visible = true;
    staging_desc.name = "textures";
    if (!device_->CreateBuffer(staging_desc, staging)) {
        device_->DestroyImage(image);
        LogWarn("texture: {} not uploaded: staging allocation failed", name);
        return kWhite;
    }
    std::vector<gpu::BufferImageCopy> regions;
    uint64_t offset = 0;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        for (uint32_t level = 0; level < mip_count; ++level) {
            const TextureMip& mip = mips[layer * mip_count + level];
            std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, mip.data.data(), mip.data.size());
            gpu::BufferImageCopy region;
            region.buffer_offset = offset;
            region.image = {gpu::Aspect::Color, level, layer, 1};
            region.extent = {mip.width, mip.height, 1};
            regions.push_back(region);
            offset += (mip.data.size() + 15) & ~uint64_t(15);
        }
    }
    device_->FlushMapped(staging, 0, total);
    static const bool upload_wait = std::getenv("PT_TEXTURE_UPLOAD_WAIT") != nullptr;
    const auto record_upload = [&](gpu::CommandList& list) {
        gpu::ImageBarrier to_copy;
        to_copy.image = image.image;
        to_copy.src_stage = gpu::PipelineStage::TopOfPipe;
        to_copy.dst_stage = gpu::PipelineStage::Transfer;
        to_copy.dst_access = static_cast<uint64_t>(gpu::Access::TransferWrite);
        to_copy.new_layout = gpu::Layout::TransferDst;
        list.Barrier(to_copy);
        list.CopyBufferToImage(staging, image, gpu::Layout::TransferDst, regions);
        gpu::ImageBarrier to_read;
        to_read.image = image.image;
        to_read.src_stage = gpu::PipelineStage::Transfer;
        to_read.src_access = static_cast<uint64_t>(gpu::Access::TransferWrite);
        to_read.old_layout = gpu::Layout::TransferDst;
        to_read.dst_stage = gpu::PipelineStage::FragmentShader | gpu::PipelineStage::ComputeShader;
        to_read.dst_access = static_cast<uint64_t>(gpu::Access::ShaderSampledRead);
        to_read.new_layout = gpu::Layout::ShaderReadOnly;
        list.Barrier(to_read);
    };
    if (upload_wait) {
        device_->Submit(record_upload);
        device_->DestroyBuffer(staging);
    } else {
        device_->SubmitDeferred(record_upload, staging);
    }
    const bool reuse = !cube && !free_.empty();
    const uint32_t index = reuse ? free_.back() : static_cast<uint32_t>(images_.size());
    if (reuse) free_.pop_back();
    const bool cube_slot = cube && cube_slots_.size() < kMaxCubeTextures;
    gpu::ImageBinding image_info{image.view, gpu::Layout::ShaderReadOnly, sampler_, &image};
    if (cube && !images_.empty()) {
        image_info.view = images_[kWhite].view;
        image_info.image = &images_[kWhite];
    }
    const gpu::ImageBinding cube_info{image.view, gpu::Layout::ShaderReadOnly, base_sampler_, &image};
    gpu::BindingWrite writes[2];
    writes[0].set = set_;
    writes[0].binding = 0;
    writes[0].first = index;
    writes[0].type = gpu::BindingType::CombinedImageSampler;
    writes[0].images = std::span(&image_info, 1);
    writes[1] = writes[0];
    writes[1].binding = 2;
    writes[1].first = static_cast<uint32_t>(cube_slots_.size());
    writes[1].images = std::span(&cube_info, 1);
    device_->WriteBindings(std::span(writes, cube_slot ? 2 : 1));
    if (cube_slot) {
        const uint32_t slot = static_cast<uint32_t>(cube_slots_.size());
        cube_slots_[index] = slot;
    }
    const uint8_t pinned = model_scope_ == 0 || cube || index <= kGreySrgb ? 1 : 0;
    if (reuse) {
        images_[index] = image;
        cube_[index] = 0;
        names_[index] = name;
        model_refs_[index] = 0;
        pinned_[index] = pinned;
    } else {
        images_.push_back(image);
        cube_.push_back(cube && index > 0 ? 1 : 0);
        names_.push_back(name);
        model_refs_.push_back(0);
        pinned_.push_back(pinned);
    }
    by_name_[name] = index;
    return index;
}

uint32_t TextureManager::LoadFox(const QarArchive& qar, const std::string& path, bool* ok, bool raw) {
    const std::string stem = FtexStem(path);
    const std::string key = raw ? stem + "#raw" : stem;
    if (auto it = by_name_.find(key); it != by_name_.end()) {
        if (ok) {
            *ok = true;
        }
        if (!model_scope_) Pin(it->second);
        return it->second;
    }
    {
        if (const auto png = mods::ReadOverride(stem + ".png")) {
            const uint32_t index = LoadModImage(qar, key, stem, *png, raw);
            if (index != kWhite) {
                if (ok) {
                    *ok = true;
                }
                return index;
            }
        }
    }
    FtexTexture ftex;
    bool read = false;
    if (auto it = decoding_.find(stem); it != decoding_.end() && !raw) {
        if (auto job = decode_jobs_.find(stem); job != decode_jobs_.end()) {
            job->second->Wait();
            decode_jobs_.erase(job);
        }
        const std::shared_ptr<FtexTexture> decoded = it->second.get();
        decoding_.erase(it);
        if (decoded) {
            ftex = std::move(*decoded);
            read = true;
        }
    } else {
        const auto started = std::chrono::steady_clock::now();
        read = LoadFtex(qar, stem, ftex);
        read_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    if (!read || ftex.Format() == gpu::Format::Undefined || ftex.depth > 1) {
        if (ok) {
            *ok = false;
        }
        return kWhite;
    }
    if (raw) {
        ftex.flags &= ~2u;
    }
    uint32_t first_level = 0;
    while (first_level < ftex.mip_count && ftex.mips[first_level].empty()) {
        ++first_level;
    }
    std::vector<TextureMip> mips;
    for (uint32_t face = 0; face < ftex.faces; ++face) {
        for (uint32_t level = first_level; level < ftex.mip_count; ++level) {
            const auto& data = ftex.mips[face * ftex.mip_count + level];
            mips.push_back({ftex.MipWidth(level), ftex.MipHeight(level), data});
        }
    }
    bool compressed = false;
    FormatBlockBytes(ftex.Format(), compressed);
    if (compressed) {
        for (TextureMip& mip : mips) {
            mip.width = std::max(mip.width, 1u);
            mip.height = std::max(mip.height, 1u);
        }
    }
    const auto upload_started = std::chrono::steady_clock::now();
    const uint32_t index = Create(key, ftex.Format(), mips, ftex.faces, ftex.faces == 6);
    upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - upload_started).count();
    if (ok) {
        *ok = index != kWhite;
    }
    return index;
}

void TextureManager::DecodeAhead(const QarArchive& qar, const std::vector<std::string>& paths) {
    if (mods::Active()) {
        return;
    }
    std::vector<std::pair<std::string, std::promise<std::shared_ptr<FtexTexture>>>> jobs;
    for (const std::string& path : paths) {
        std::string stem = FtexStem(path);
        if (stem.empty() || by_name_.contains(stem) || decoding_.contains(stem)) {
            continue;
        }
        std::promise<std::shared_ptr<FtexTexture>> promise;
        decoding_.emplace(stem, promise.get_future().share());
        decode_order_.push_back(stem);
        jobs.emplace_back(std::move(stem), std::move(promise));
    }
    if (jobs.empty()) {
        return;
    }
    std::vector<std::string> stems;
    for (const auto& [stem, promise] : jobs) {
        stems.push_back(stem);
    }
    auto batch = std::make_shared<decltype(jobs)>(std::move(jobs));
    auto task = std::make_shared<ParallelTask>([&qar, batch] {
        for (auto& [stem, promise] : *batch) {
            auto ftex = std::make_shared<FtexTexture>();
            if (!LoadFtex(qar, stem, *ftex)) {
                ftex.reset();
            }
            promise.set_value(std::move(ftex));
        }
    });
    for (std::string& stem : stems) {
        decode_jobs_[std::move(stem)] = task;
    }
    decode_workers_.push_back(std::move(task));
}

bool TextureManager::AdoptDecoded(const std::string& path, std::shared_ptr<FtexTexture> decoded) {
    std::string stem = FtexStem(path);
    if (mods::Active() || stem.empty() || by_name_.contains(stem) || decoding_.contains(stem)) {
        return false;
    }
    std::promise<std::shared_ptr<FtexTexture>> promise;
    promise.set_value(std::move(decoded));
    decoding_.emplace(std::move(stem), promise.get_future().share());
    return true;
}

void TextureManager::DropDecoded(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        decoding_.erase(FtexStem(path));
        decode_jobs_.erase(FtexStem(path));
    }
}

std::unordered_set<std::string> TextureManager::Names() const {
    std::unordered_set<std::string> names;
    for (const auto& [name, index] : by_name_) {
        names.insert(name);
    }
    return names;
}

uint32_t TextureManager::PumpDecoded(const QarArchive& qar, uint32_t count) {
    uint32_t uploaded = 0;
    while (uploaded < count && !decode_order_.empty()) {
        auto it = decoding_.find(decode_order_.front());
        if (it == decoding_.end()) {
            decode_order_.pop_front();
            continue;
        }
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            break;
        }
        const std::string stem = decode_order_.front();
        decode_order_.pop_front();
        LoadFox(qar, stem);
        ++uploaded;
    }
    std::erase_if(decode_workers_, [](const std::shared_ptr<ParallelTask>& worker) { return worker->Done(); });
    return uploaded;
}

uint32_t TextureManager::LoadModImage(const QarArchive& qar, const std::string& key, const std::string& stem, const std::vector<uint8_t>& png,
                                      bool raw) {
    FtexTexture original;
    const bool has_original = LoadFtex(qar, stem, original);
    if (has_original && (original.faces != 1 || original.depth > 1)) {
        LogWarn("mods: {}.png ignored: a cube or volume texture is replaced by a .ftex only", stem);
        return kWhite;
    }
    const bool data_name = stem.ends_with("_nrm") || stem.ends_with("_srm") || stem.ends_with("_lin");
    const bool srgb = !raw && (has_original ? original.Srgb() : !data_name);
    std::vector<std::vector<uint8_t>> levels;
    uint32_t width = 0, height = 0;
    std::string error;
    if (!ModImageLevels(png, srgb, levels, width, height, error)) {
        LogWarn("mods: {}.png ignored: {}", stem, error);
        return kWhite;
    }
    std::vector<TextureMip> mips;
    for (uint32_t level = 0; level < levels.size(); ++level) {
        mips.push_back({std::max(1u, width >> level), std::max(1u, height >> level), levels[level]});
    }
    const uint32_t index = Create(key, srgb ? gpu::Format::R8G8B8A8Srgb : gpu::Format::R8G8B8A8Unorm, mips);
    if (index != kWhite) {
        LogInfo("mods: texture {} from a {} x {} PNG", stem, width, height);
    }
    return index;
}

void TextureManager::PreloadModImages(const QarArchive& qar, std::span<const std::string> paths) {
    struct Job {
        std::string key;
        std::string stem;
        std::vector<uint8_t> png;
        bool srgb = true;
        std::vector<std::vector<uint8_t>> levels;
        uint32_t width = 0;
        uint32_t height = 0;
        bool ok = false;
        std::string error;
    };
    std::vector<Job> jobs;
    std::unordered_set<std::string> seen;
    for (const std::string& path : paths) {
        const std::string stem = FtexStem(path);
        if (stem.empty() || !seen.insert(stem).second || by_name_.contains(stem)) continue;
        auto png = mods::ReadOverride(stem + ".png");
        if (!png) continue;
        FtexTexture original;
        const bool has_original = LoadFtex(qar, stem, original);
        if (has_original && (original.faces != 1 || original.depth > 1)) continue;
        const bool data_name = stem.ends_with("_nrm") || stem.ends_with("_srm") || stem.ends_with("_lin");
        Job j;
        j.key = stem;
        j.stem = stem;
        j.png = std::move(*png);
        j.srgb = has_original ? original.Srgb() : !data_name;
        jobs.push_back(std::move(j));
    }
    if (jobs.size() < 2) return;
    ParallelChunks(jobs.size(), jobs.size(), [&](size_t, size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            Job& j = jobs[i];
            j.ok = ModImageLevels(j.png, j.srgb, j.levels, j.width, j.height, j.error);
        }
    });
    for (Job& j : jobs) {
        if (!j.ok) {
            LogWarn("mods: {}.png ignored: {}", j.stem, j.error);
            continue;
        }
        std::vector<TextureMip> mips;
        for (uint32_t level = 0; level < j.levels.size(); ++level) {
            mips.push_back({std::max(1u, j.width >> level), std::max(1u, j.height >> level), j.levels[level]});
        }
        const uint32_t idx = Create(j.key, j.srgb ? gpu::Format::R8G8B8A8Srgb : gpu::Format::R8G8B8A8Unorm, mips);
        if (idx != kWhite) {
            LogInfo("mods: texture {} from a {} x {} PNG", j.stem, j.width, j.height);
        }
    }
}

uint32_t TextureManager::LoadImageFile(const std::filesystem::path& path, const std::string& key) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return kWhite;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::vector<uint8_t>> levels;
    uint32_t width = 0, height = 0;
    std::string error;
    if (!ModImageLevels(data, true, levels, width, height, error)) {
        LogWarn("texture: {} unreadable: {}", key, error);
        return kWhite;
    }
    std::vector<TextureMip> mips;
    for (uint32_t level = 0; level < levels.size(); ++level) {
        mips.push_back({std::max(1u, width >> level), std::max(1u, height >> level), levels[level]});
    }
    return Create(key, gpu::Format::R8G8B8A8Unorm, mips);
}

void TextureManager::UpdateTextureDescriptor(uint32_t index) {
    uint32_t shown = index;
    const gpu::ImageResource& view = images_[shown].view ? images_[shown] : images_[kWhite];
    const gpu::ImageBinding info{view.view, gpu::Layout::ShaderReadOnly, sampler_, &view};
    gpu::BindingWrite write;
    write.set = set_; write.binding = 0; write.first = index;
    write.type = gpu::BindingType::CombinedImageSampler; write.images = std::span(&info, 1);
    device_->WriteBindings(std::span(&write, 1));
}

uint32_t TextureManager::AddMaterial(const MaterialGpu& material) {
    if (materials_.size() >= kMaxMaterials) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LogWarn("textures: material table full ({} entries), further materials use the default one", kMaxMaterials);
        }
        return 0;
    }
    materials_.push_back(material);
    materials_dirty_ = true;
    return static_cast<uint32_t>(materials_.size() - 1);
}

void TextureManager::UpdateMaterial(uint32_t index, const MaterialGpu& material) {
    if (index < materials_.size()) {
        materials_[index] = material;
        materials_dirty_ = true;
    }
}

void TextureManager::RedirectTexture(uint32_t texture, uint32_t shown) {
    if (shown == texture) {
        materials_dirty_ = redirects_.erase(texture) > 0 || materials_dirty_;
        return;
    }
    redirects_[texture] = shown;
    materials_dirty_ = true;
}

void TextureManager::FlushMaterials() {
    if (!materials_dirty_) {
        return;
    }
    const size_t count = std::min<size_t>(materials_.size(), kMaxMaterials);
    if (redirects_.empty()) {
        std::memcpy(material_buffer_.mapped, materials_.data(), count * sizeof(MaterialGpu));
    } else {
        auto* out = static_cast<MaterialGpu*>(material_buffer_.mapped);
        for (size_t i = 0; i < count; ++i) {
            out[i] = materials_[i];
            for (uint32_t* slot : {&out[i].albedo, &out[i].normal, &out[i].specular}) {
                if (auto it = redirects_.find(*slot); it != redirects_.end()) {
                    *slot = it->second;
                }
            }
        }
    }
    device_->FlushMapped(material_buffer_, 0, count * sizeof(MaterialGpu));
    materials_dirty_ = false;
}

}
