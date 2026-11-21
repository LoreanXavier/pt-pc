#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/assets/ftex.h"
#include "engine/core/parallel_for.h"
#include "engine/render/gpu/gpu_device.h"
#include "engine/render/gpu/gpu_vk_native.h"
#include "engine/render/vk_context.h"

namespace pt {
class QarArchive;
}

namespace pt {

struct TextureMip {
    uint32_t width = 0;
    uint32_t height = 0;
    std::span<const uint8_t> data;
};

struct MaterialGpu {
    uint32_t albedo = 0;
    uint32_t normal = 0;
    uint32_t specular = 0;
    uint32_t flags = 0;
    uint32_t aux0 = 0;
    uint32_t aux1 = 0;
    uint32_t kind = 0;
    uint32_t aux2 = 0;
    glm::vec4 albedo_factor{1.0f};
    glm::vec4 params{0.0f};
    glm::vec4 indices{0.0f};
    glm::vec4 extra{0.0f};
};

class TextureManager {
public:
    static constexpr uint32_t kMaxTextures = 8192;
    static constexpr uint32_t kMaxMaterials = 16384;
    static constexpr uint32_t kWhite = 0;
    static constexpr uint32_t kFlatNormal = 1;
    static constexpr uint32_t kBlack = 2;
    static constexpr uint32_t kGrey = 4;
    static constexpr uint32_t kGreySrgb = 5;
    static constexpr uint32_t kMaxCubeTextures = 64;
    static constexpr uint32_t kNoCube = 0xFFFFFFFFu;

    bool Init(gpu::Device& device);
    void Shutdown();

    uint32_t Create(const std::string& name, gpu::Format format, std::span<const TextureMip> mips, uint32_t layers = 1, bool cube = false);
    uint32_t Find(const std::string& name) const;
    class ModelScope {
    public:
        explicit ModelScope(TextureManager& textures) : textures_(textures) { ++textures_.model_scope_; }
        ~ModelScope() { --textures_.model_scope_; }
        ModelScope(const ModelScope&) = delete;
        ModelScope& operator=(const ModelScope&) = delete;

    private:
        TextureManager& textures_;
    };
    void AddModelRef(uint32_t index);
    uint64_t ReleaseModelRef(uint32_t index);
    uint32_t FreeSlots() const { return static_cast<uint32_t>(free_.size()); }
    uint32_t LoadFox(const QarArchive& qar, const std::string& path, bool* ok = nullptr, bool raw = false);
    void PreloadModImages(const QarArchive& qar, std::span<const std::string> paths);
    uint32_t LoadImageFile(const std::filesystem::path& path, const std::string& key);
    void DecodeAhead(const QarArchive& qar, const std::vector<std::string>& paths);
    uint32_t PumpDecoded(const QarArchive& qar, uint32_t count);
    bool AdoptDecoded(const std::string& path, std::shared_ptr<FtexTexture> decoded);
    void DropDecoded(const std::vector<std::string>& paths);
    std::unordered_set<std::string> Names() const;
    double ReadMs() const { return read_ms_; }
    double UploadMs() const { return upload_ms_; }
    uint32_t AddMaterial(const MaterialGpu& material);
    void UpdateMaterial(uint32_t index, const MaterialGpu& material);
    void RedirectTexture(uint32_t texture, uint32_t shown);
    void FlushMaterials();
    uint32_t MaterialFlags(uint32_t index) const { return index < materials_.size() ? materials_[index].flags : 0u; }
    const MaterialGpu* Material(uint32_t index) const { return index < materials_.size() ? &materials_[index] : nullptr; }
    int SetAnisotropy(int level);
    int Anisotropy() const { return anisotropy_; }
    gpu::BindingLayoutHandle BindingLayout() const { return set_layout_; }
    gpu::BindingSetHandle BindingSet() const { return set_; }
    uint32_t TextureCount() const { return static_cast<uint32_t>(images_.size()); }
    uint32_t MaterialCount() const { return static_cast<uint32_t>(materials_.size()); }
    uint32_t CubeSlot(uint32_t texture) const;

private:
    gpu::SamplerHandle CreateSampler(int anisotropy) const;
    void UpdateTextureDescriptor(uint32_t index);
    void Pin(uint32_t index) const;
    uint64_t FreeTexture(uint32_t index);
    uint32_t LoadModImage(const QarArchive& qar, const std::string& key, const std::string& stem, const std::vector<uint8_t>& png, bool raw);

    gpu::Device* device_ = nullptr;
    gpu::SamplerHandle base_sampler_;
    gpu::SamplerHandle sampler_;
    int anisotropy_ = 0;
    gpu::BindingLayoutHandle set_layout_;
    gpu::BindingPoolHandle pool_;
    gpu::BindingSetHandle set_;
    std::vector<gpu::ImageResource> images_;
    std::vector<std::string> names_;
    std::vector<uint32_t> model_refs_;
    mutable std::vector<uint8_t> pinned_;
    std::vector<uint32_t> free_;
    int model_scope_ = 0;
    std::vector<uint8_t> cube_;
    std::unordered_map<std::string, uint32_t> by_name_;
    std::unordered_map<uint32_t, uint32_t> cube_slots_;
    std::vector<MaterialGpu> materials_;
    std::unordered_map<uint32_t, uint32_t> redirects_;
    gpu::BufferResource material_buffer_;
    bool materials_dirty_ = false;
    bool memory_short_ = false;
    std::unordered_map<std::string, std::shared_future<std::shared_ptr<FtexTexture>>> decoding_;
    std::deque<std::string> decode_order_;
    std::vector<std::shared_ptr<ParallelTask>> decode_workers_;
    std::unordered_map<std::string, std::shared_ptr<ParallelTask>> decode_jobs_;
    double read_ms_ = 0.0;
    double upload_ms_ = 0.0;
};

uint32_t FormatBlockBytes(gpu::Format format, bool& compressed);

}
