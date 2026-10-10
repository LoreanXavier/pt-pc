#include "engine/render/texture_manager.h"

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

#include "engine/assets/ftex.h"
#include "engine/assets/enhanced_textures.h"
#include "engine/core/log.h"
#include "engine/fs/mods.h"

namespace pt {
namespace {

// A mod's PNG (docs/modding.md) as RGBA8 levels down to 1 x 1, each the average of four texels of the one above (in linear
// light for a colour texture). Uncompressed, so loading never stalls on an encoder.
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

uint32_t FormatBlockBytes(VkFormat format, bool& compressed) {
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

bool TextureManager::Init(vk::Context& ctx) {
    ctx_ = &ctx;
    base_sampler_ = CreateSampler(0);
    sampler_ = base_sampler_;

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures,
                   VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxCubeTextures, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorBindingFlags bindless = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                             VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags binding_flags[3] = {bindless, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, bindless};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags_info.bindingCount = 3;
    flags_info.pBindingFlags = binding_flags;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.pNext = &flags_info;
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    if (!vk::Check(vkCreateDescriptorSetLayout(ctx.device, &layout_info, nullptr, &set_layout_), "texture set layout")) {
        return false;
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + kMaxCubeTextures},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    vkCreateDescriptorPool(ctx.device, &pool_info, nullptr, &pool_);
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &set_layout_;
    if (!vk::Check(vkAllocateDescriptorSets(ctx.device, &alloc, &set_), "texture set")) {
        return false;
    }
    ctx.CreateBuffer(material_buffer_, sizeof(MaterialGpu) * kMaxMaterials, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    VkDescriptorBufferInfo buffer_info{material_buffer_.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);

    const uint8_t white[4] = {255, 255, 255, 255};
    const uint8_t flat[4] = {128, 128, 255, 255};
    const uint8_t black[4] = {0, 0, 0, 255};
    const TextureMip white_mip{1, 1, white};
    const TextureMip flat_mip{1, 1, flat};
    const TextureMip black_mip{1, 1, black};
    Create("builtin:white", VK_FORMAT_R8G8B8A8_UNORM, {&white_mip, 1});
    Create("builtin:flat_normal", VK_FORMAT_R8G8B8A8_UNORM, {&flat_mip, 1});
    Create("builtin:black", VK_FORMAT_R8G8B8A8_UNORM, {&black_mip, 1});
    const TextureMip black_faces[6] = {black_mip, black_mip, black_mip, black_mip, black_mip, black_mip};
    Create("builtin:black_cube", VK_FORMAT_R8G8B8A8_UNORM, black_faces, 6, true);
    const uint8_t grey[4] = {128, 128, 128, 255};
    const TextureMip grey_mip{1, 1, grey};
    Create("builtin:grey", VK_FORMAT_R8G8B8A8_UNORM, {&grey_mip, 1});
    Create("builtin:grey_srgb", VK_FORMAT_R8G8B8A8_SRGB, {&grey_mip, 1});
    AddMaterial(MaterialGpu{});
    FlushMaterials();
    return true;
}

void TextureManager::Shutdown() {
    StopEnhancedWorker();
    for (std::future<void>& worker : decode_workers_) {
        worker.wait();
    }
    decode_workers_.clear();
    decoding_.clear();
    decode_order_.clear();
    if (!ctx_) {
        return;
    }
    for (vk::Image& image : images_) {
        ctx_->DestroyImage(image);
    }
    images_.clear();
    cube_.clear();
    by_name_.clear();
    cube_slots_.clear();
    fox_sources_.clear();
    enhanced_images_.clear();
    enhanced_bytes_ = 0;
    enhanced_qar_ = nullptr;
    enhanced_enabled_ = false;
    ctx_->DestroyBuffer(material_buffer_);
    vkDestroyDescriptorPool(ctx_->device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_->device, set_layout_, nullptr);
    if (sampler_ != base_sampler_) {
        vkDestroySampler(ctx_->device, sampler_, nullptr);
    }
    vkDestroySampler(ctx_->device, base_sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    base_sampler_ = VK_NULL_HANDLE;
    anisotropy_ = 0;
    ctx_ = nullptr;
}

uint32_t TextureManager::CubeSlot(uint32_t texture) const {
    auto it = cube_slots_.find(texture);
    return it == cube_slots_.end() ? kNoCube : it->second;
}

VkSampler TextureManager::CreateSampler(int anisotropy) const {
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.anisotropyEnable = anisotropy > 1 ? VK_TRUE : VK_FALSE;
    sampler_info.maxAnisotropy = anisotropy > 1 ? static_cast<float>(anisotropy) : 1.0f;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    vk::Check(vkCreateSampler(ctx_->device, &sampler_info, nullptr, &sampler), "texture sampler");
    return sampler;
}

int TextureManager::SetAnisotropy(int level) {
    if (!ctx_) {
        return 0;
    }
    const int limit = static_cast<int>(ctx_->properties.limits.maxSamplerAnisotropy);
    level = level > 1 ? std::min(level, limit) : 0;
    if (level == anisotropy_) {
        return anisotropy_;
    }
    const VkSampler sampler = level > 1 ? CreateSampler(level) : base_sampler_;
    if (!sampler) {
        return anisotropy_;
    }
    // every texture descriptor takes the new sampler; frames in flight still read the old ones
    vkDeviceWaitIdle(ctx_->device);
    std::vector<VkDescriptorImageInfo> infos(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        uint32_t shown = static_cast<uint32_t>(i);
        if (enhanced_enabled_) {
            if (auto it = enhanced_images_.find(shown); it != enhanced_images_.end()) shown = it->second;
        }
        infos[i] = {sampler, cube_[i] ? images_[kWhite].view : images_[shown].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    if (!infos.empty()) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set_;
        write.dstBinding = 0;
        write.descriptorCount = static_cast<uint32_t>(infos.size());
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = infos.data();
        vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);
    }
    if (sampler_ != base_sampler_) {
        vkDestroySampler(ctx_->device, sampler_, nullptr);
    }
    sampler_ = sampler;
    anisotropy_ = level;
    LogInfo("textures: anisotropic filtering {}", level > 1 ? std::format("{}x", level) : std::string("off (trilinear)"));
    return anisotropy_;
}

uint32_t TextureManager::Find(const std::string& name) const {
    auto it = by_name_.find(name);
    return it == by_name_.end() ? kWhite : it->second;
}

uint32_t TextureManager::Create(const std::string& name, VkFormat format, std::span<const TextureMip> mips, uint32_t layers, bool cube) {
    if (auto it = by_name_.find(name); it != by_name_.end()) {
        return it->second;
    }
    if (mips.empty() || images_.size() >= kMaxTextures) {
        return kWhite;
    }
    vk::Image image;
    const uint32_t mip_count = static_cast<uint32_t>(mips.size()) / layers;
    if (!ctx_->CreateImage(image, format, {mips[0].width, mips[0].height, 1}, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           mip_count, layers, VK_IMAGE_ASPECT_COLOR_BIT, cube)) {
        return kWhite;
    }
    VkDeviceSize total = 0;
    for (const TextureMip& mip : mips) {
        total += (mip.data.size() + 15) & ~VkDeviceSize(15);
    }
    vk::Buffer staging;
    ctx_->CreateBuffer(staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        for (uint32_t level = 0; level < mip_count; ++level) {
            const TextureMip& mip = mips[layer * mip_count + level];
            std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, mip.data.data(), mip.data.size());
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
            region.imageExtent = {mip.width, mip.height, 1};
            regions.push_back(region);
            offset += (mip.data.size() + 15) & ~VkDeviceSize(15);
        }
    }
    vmaFlushAllocation(ctx_->allocator, staging.allocation, 0, total);
    ctx_->Submit([&](VkCommandBuffer cmd) {
        vk::ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                               regions.data());
        vk::ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    ctx_->DestroyBuffer(staging);
    const uint32_t index = static_cast<uint32_t>(images_.size());
    const bool cube_slot = cube && cube_slots_.size() < kMaxCubeTextures;
    VkDescriptorImageInfo image_info{sampler_, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    if (cube && !images_.empty()) {
        image_info.imageView = images_[kWhite].view;
    }
    VkDescriptorImageInfo cube_info{base_sampler_, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = set_;
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = index;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &image_info;
    writes[1] = writes[0];
    writes[1].dstBinding = 2;
    writes[1].dstArrayElement = static_cast<uint32_t>(cube_slots_.size());
    writes[1].pImageInfo = &cube_info;
    vkUpdateDescriptorSets(ctx_->device, cube_slot ? 2 : 1, writes, 0, nullptr);
    if (cube_slot) {
        const uint32_t slot = static_cast<uint32_t>(cube_slots_.size());
        cube_slots_[index] = slot;
    }
    images_.push_back(image);
    cube_.push_back(cube && index > 0 ? 1 : 0);
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
        return it->second;
    }
    if (mods::Active()) {
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
    if (!read || ftex.Format() == VK_FORMAT_UNDEFINED || ftex.depth > 1) {
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
    if (index != kWhite && !raw && EnhancedTextureEligible(stem, ftex)) {
        fox_sources_[index] = stem;
        if (enhanced_enabled_) LoadEnhancedTexture(index, stem, &ftex);
    }
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
    decode_workers_.push_back(std::async(std::launch::async, [&qar, jobs = std::move(jobs)]() mutable {
        for (auto& [stem, promise] : jobs) {
            auto ftex = std::make_shared<FtexTexture>();
            if (!LoadFtex(qar, stem, *ftex)) {
                ftex.reset();
            }
            promise.set_value(std::move(ftex));
        }
    }));
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
    std::erase_if(decode_workers_, [](std::future<void>& worker) { return worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    return uploaded;
}

uint32_t TextureManager::LoadModImage(const QarArchive& qar, const std::string& key, const std::string& stem, const std::vector<uint8_t>& png,
                                      bool raw) {
    // the game's texture, when there is one, says whether the picture is colour (sRGB) and whether a PNG can stand in for it
    FtexTexture original;
    const bool has_original = LoadFtex(qar, stem, original);
    if (has_original && (original.faces != 1 || original.depth > 1)) {
        LogWarn("mods: {}.png ignored: a cube or volume texture is replaced by a .ftex only", stem);
        return kWhite;
    }
    const bool srgb = !raw && (!has_original || original.Srgb());
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
    const uint32_t index = Create(key, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, mips);
    if (index != kWhite) {
        LogInfo("mods: texture {} from a {} x {} PNG", stem, width, height);
    }
    return index;
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
    return Create(key, VK_FORMAT_R8G8B8A8_SRGB, mips);
}

void TextureManager::ConfigureEnhancedTextures(const QarArchive& qar, const std::filesystem::path& cache, uint64_t model) {
    if(enhanced_qar_ == &qar && enhanced_cache_ == cache && enhanced_model_ == model) return;
    SetEnhancedTextures(false);
    enhanced_qar_ = &qar;
    enhanced_cache_ = cache;
    enhanced_model_ = model;
    enhanced_images_.clear();
    enhanced_bytes_ = 0;
}

void TextureManager::LoadEnhancedTexture(uint32_t index, const std::string& path, const FtexTexture* source) {
    if (!enhanced_qar_ || !enhanced_model_ || enhanced_images_.contains(index)) return;
    FtexTexture loaded, cached;
    if (!source) {
        if (!LoadFtex(*enhanced_qar_, path, loaded)) return;
        source = &loaded;
    }
    const uint64_t key = EnhancedTextureKey(*source, enhanced_model_);
    if (!ReadTextureCache(EnhancedCacheFile(enhanced_cache_, path), key, cached)) return;
    // twice the source's size, or the source's size where the capped mode reduced the 2x result (a 2048 source capped at 2048)
    const bool doubled = cached.width == source->width * 2 && cached.height == source->height * 2;
    const bool kept = cached.width == source->width && cached.height == source->height;
    if (!(doubled || kept) || cached.Srgb() != source->Srgb()) return;
    std::vector<TextureMip> mips;
    uint64_t bytes = 0;
    for (uint32_t level = 0; level < cached.mip_count; ++level) {
        mips.push_back({cached.MipWidth(level), cached.MipHeight(level), cached.mips[level]});
        bytes += cached.mips[level].size();
    }
    const uint32_t replacement = Create(path + std::format("#enhanced-{:016x}", key), cached.Format(), mips);
    if (replacement == kWhite) return;
    enhanced_images_[index] = replacement;
    enhanced_bytes_ += bytes;
    if (enhanced_enabled_) {
        vkDeviceWaitIdle(ctx_->device);
        UpdateTextureDescriptor(index);
    }
}

void TextureManager::UpdateTextureDescriptor(uint32_t index) {
    uint32_t shown = index;
    if (enhanced_enabled_) {
        if (auto it = enhanced_images_.find(index); it != enhanced_images_.end()) shown = it->second;
    }
    VkDescriptorImageInfo info{sampler_, images_[shown].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_; write.dstBinding = 0; write.dstArrayElement = index;
    write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
    vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);
}

void TextureManager::StopEnhancedWorker() {
    if (enhanced_queue_) enhanced_queue_->stop = true;
    if (enhanced_worker_.valid()) enhanced_worker_.wait();
    enhanced_worker_ = {};
    enhanced_queue_.reset();
}

void TextureManager::SetEnhancedTextures(bool enabled) {
    if (!ctx_ || enabled == enhanced_enabled_) return;
    StopEnhancedWorker();
    if (enabled && enhanced_qar_ && enhanced_model_) {
        std::vector<std::pair<uint32_t, std::string>> todo;
        for (const auto& [index, path] : fox_sources_) {
            if (!enhanced_images_.contains(index)) todo.emplace_back(index, path);
        }
        auto queue = std::make_shared<EnhancedQueue>();
        queue->total = todo.size();
        enhanced_queue_ = queue;
        const QarArchive* qar = enhanced_qar_;
        const std::filesystem::path cache = enhanced_cache_;
        const uint64_t model = enhanced_model_;
        enhanced_worker_ = std::async(std::launch::async, [queue, todo = std::move(todo), qar, cache, model] {
            for (const auto& [index, path] : todo) {
                if (queue->stop) break;
                FtexTexture source;
                EnhancedPrepared prepared;
                if (LoadFtex(*qar, path, source)) {
                    prepared.key = EnhancedTextureKey(source, model);
                    if (ReadTextureCache(EnhancedCacheFile(cache, path), prepared.key, prepared.cached)) {
                        const FtexTexture& c = prepared.cached;
                        const bool doubled = c.width == source.width * 2 && c.height == source.height * 2;
                        const bool kept = c.width == source.width && c.height == source.height;
                        if ((doubled || kept) && c.Srgb() == source.Srgb()) {
                            prepared.index = index;
                            prepared.path = path;
                            std::lock_guard lock(queue->mutex);
                            queue->ready.push_back(std::move(prepared));
                        }
                    }
                }
                ++queue->done;
            }
        });
    }
    vkDeviceWaitIdle(ctx_->device);
    enhanced_enabled_ = enabled;
    for (const auto& [index, replacement] : enhanced_images_) UpdateTextureDescriptor(index);
    LogInfo("textures: enhanced textures {}, {} loaded replacements ({:.0f} MB of texture data)", enabled ? "on" : "off", enhanced_images_.size(),
            static_cast<double>(enhanced_bytes_) / (1024.0 * 1024.0));
}

void TextureManager::PumpEnhancedTextures(double budget_ms) {
    if (!enhanced_queue_ || !ctx_) return;
    const auto start = std::chrono::steady_clock::now();
    std::vector<uint32_t> swapped;
    for (;;) {
        EnhancedPrepared prepared;
        {
            std::lock_guard lock(enhanced_queue_->mutex);
            if (enhanced_queue_->ready.empty()) break;
            prepared = std::move(enhanced_queue_->ready.front());
            enhanced_queue_->ready.pop_front();
        }
        if (!enhanced_images_.contains(prepared.index)) {
            std::vector<TextureMip> mips;
            uint64_t bytes = 0;
            for (uint32_t level = 0; level < prepared.cached.mip_count; ++level) {
                mips.push_back({prepared.cached.MipWidth(level), prepared.cached.MipHeight(level), prepared.cached.mips[level]});
                bytes += prepared.cached.mips[level].size();
            }
            const uint32_t replacement = Create(prepared.path + std::format("#enhanced-{:016x}", prepared.key), prepared.cached.Format(), mips);
            if (replacement != kWhite) {
                enhanced_images_[prepared.index] = replacement;
                enhanced_bytes_ += bytes;
                swapped.push_back(prepared.index);
            }
        }
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budget_ms) break;
    }
    if (!swapped.empty() && enhanced_enabled_) {
        vkDeviceWaitIdle(ctx_->device);
        for (const uint32_t index : swapped) UpdateTextureDescriptor(index);
    }
    bool finished = false;
    {
        std::lock_guard lock(enhanced_queue_->mutex);
        finished = enhanced_queue_->ready.empty() && enhanced_queue_->done.load() >= enhanced_queue_->total;
    }
    if (finished) {
        StopEnhancedWorker();
        LogInfo("textures: enhanced textures ready, {} replacements ({:.0f} MB of texture data)", enhanced_images_.size(),
                static_cast<double>(enhanced_bytes_) / (1024.0 * 1024.0));
    }
}

uint32_t TextureManager::AddMaterial(const MaterialGpu& material) {
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
    vmaFlushAllocation(ctx_->allocator, material_buffer_.allocation, 0, count * sizeof(MaterialGpu));
    materials_dirty_ = false;
}

}
