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

#include "engine/render/vk_context.h"

namespace pt {
class QarArchive;
struct FtexTexture;
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

    bool Init(vk::Context& ctx);
    void Shutdown();

    uint32_t Create(const std::string& name, VkFormat format, std::span<const TextureMip> mips, uint32_t layers = 1, bool cube = false);
    uint32_t Find(const std::string& name) const;
    uint32_t LoadFox(const QarArchive& qar, const std::string& path, bool* ok = nullptr, bool raw = false);
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
    int SetAnisotropy(int level);
    int Anisotropy() const { return anisotropy_; }
    void ConfigureEnhancedTextures(const QarArchive& qar, const std::filesystem::path& cache, uint64_t model);
    void SetEnhancedTextures(bool enabled);
    bool EnhancedTextures() const { return enhanced_enabled_; }

    VkDescriptorSetLayout SetLayout() const { return set_layout_; }
    VkDescriptorSet Set() const { return set_; }
    uint32_t TextureCount() const { return static_cast<uint32_t>(images_.size()); }
    uint32_t CubeSlot(uint32_t texture) const;

private:
    VkSampler CreateSampler(int anisotropy) const;
    void UpdateTextureDescriptor(uint32_t index);
    void LoadEnhancedTexture(uint32_t index, const std::string& path, const FtexTexture* source = nullptr);
    uint32_t LoadModImage(const QarArchive& qar, const std::string& key, const std::string& stem, const std::vector<uint8_t>& png, bool raw);

    vk::Context* ctx_ = nullptr;
    VkSampler base_sampler_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    int anisotropy_ = 0;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    std::vector<vk::Image> images_;
    std::vector<uint8_t> cube_;
    std::unordered_map<std::string, uint32_t> by_name_;
    std::unordered_map<uint32_t, uint32_t> cube_slots_;
    std::vector<MaterialGpu> materials_;
    std::unordered_map<uint32_t, uint32_t> redirects_;
    vk::Buffer material_buffer_;
    bool materials_dirty_ = false;
    const QarArchive* enhanced_qar_ = nullptr;
    std::filesystem::path enhanced_cache_;
    uint64_t enhanced_model_ = 0;
    bool enhanced_enabled_ = false;
    std::unordered_map<uint32_t, std::string> fox_sources_;
    std::unordered_map<uint32_t, uint32_t> enhanced_images_;
    uint64_t enhanced_bytes_ = 0;
    std::unordered_map<std::string, std::shared_future<std::shared_ptr<FtexTexture>>> decoding_;
    std::deque<std::string> decode_order_;
    std::vector<std::future<void>> decode_workers_;
    double read_ms_ = 0.0;
    double upload_ms_ = 0.0;
};

uint32_t FormatBlockBytes(VkFormat format, bool& compressed);

}
