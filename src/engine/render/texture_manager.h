#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <atomic>
#include <future>
#include <mutex>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/assets/ftex.h"
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
    // Fox's default texture for a material slot whose file is missing or that the material leaves out: the 1x1 RGBA8 UNORM
    // (0x80, 0x80, 0x80, 0xFF) that 0xD43430 makes third (ending_fx_trace: the plant shsb_flpl001 binds it for its base, normal,
    // specular and translucency slots). kGrey reads it as stored (normal, specular, translucency, material maps); kGreySrgb
    // gives the albedo the composite's decode of it, 0.216 (rendering.md 3)
    static constexpr uint32_t kGrey = 4;
    static constexpr uint32_t kGreySrgb = 5;
    static constexpr uint32_t kMaxCubeTextures = 64;
    static constexpr uint32_t kNoCube = 0xFFFFFFFFu;

    bool Init(vk::Context& ctx);
    void Shutdown();

    uint32_t Create(const std::string& name, VkFormat format, std::span<const TextureMip> mips, uint32_t layers = 1, bool cube = false);
    uint32_t Find(const std::string& name) const;
    uint32_t LoadFox(const QarArchive& qar, const std::string& path, bool* ok = nullptr, bool raw = false);
    // Reads and unpacks the .ftex files of `paths` on a worker thread ahead of their LoadFox, which then only uploads: the f100
    // clock's maze package brings eleven effect pictures (fx_viweye01..11) whose unpacking took 311 ms of one frame. Paths loaded or
    // already queued are skipped; nothing is queued while a mod is active (LoadFox reads the overrides).
    void DecodeAhead(const QarArchive& qar, const std::vector<std::string>& paths);
    // Uploads up to `count` finished ahead decodes in the order they were queued (one or two a frame instead of all of them in
    // the frame that first draws them); returns how many were uploaded
    uint32_t PumpDecoded(const QarArchive& qar, uint32_t count);
    // A decode made elsewhere (a stage prefetch) for LoadFox to take; false when the texture is loaded or queued already, or a
    // mod is active. Unlike DecodeAhead's, an adopted decode is not uploaded by PumpDecoded: DropDecoded drops the ones not taken.
    bool AdoptDecoded(const std::string& path, std::shared_ptr<FtexTexture> decoded);
    void DropDecoded(const std::vector<std::string>& paths);
    std::unordered_set<std::string> Names() const;
    // milliseconds the calling (main) thread spent reading .ftex files and uploading textures, for the slow load logs
    double ReadMs() const { return read_ms_; }
    double UploadMs() const { return upload_ms_; }
    uint32_t AddMaterial(const MaterialGpu& material);
    void UpdateMaterial(uint32_t index, const MaterialGpu& material);
    // Every material whose base, normal or specular texture is `texture` shows `shown` there instead, also materials made later (the button
    // prompts painted into game textures); shown == texture undoes it
    void RedirectTexture(uint32_t texture, uint32_t shown);
    void FlushMaterials();
    uint32_t MaterialFlags(uint32_t index) const { return index < materials_.size() ? materials_[index].flags : 0u; }
    // Anisotropic filtering of the 2D textures (PC option, rendering.md 12.22): 0 or 1 keeps the original's trilinear sampler,
    // else the level up to the device's limit; waits for the device. Returns the level in use.
    int SetAnisotropy(int level);
    int Anisotropy() const { return anisotropy_; }
    void ConfigureEnhancedTextures(const QarArchive& qar, const std::filesystem::path& cache, uint64_t model);
    void SetEnhancedTextures(bool enabled);
    bool EnhancedTextures() const { return enhanced_enabled_; }
    // Switching enhanced textures on prepares the replacements of the textures already loaded on a worker (each a QAR read,
    // a key hash and a cache read of up to 21 MB) and uploads them here within the time budget of a frame. Doing all of it
    // at once on the main thread froze the game 3.4 to 4.1 s right after start (issue logs of 2026-10-08).
    void PumpEnhancedTextures(double budget_ms);
    bool EnhancedPending() const { return enhanced_queue_ != nullptr; }

    VkDescriptorSetLayout SetLayout() const { return set_layout_; }
    VkDescriptorSet Set() const { return set_; }
    uint32_t TextureCount() const { return static_cast<uint32_t>(images_.size()); }
    uint32_t CubeSlot(uint32_t texture) const;

private:
    VkSampler CreateSampler(int anisotropy) const;
    void UpdateTextureDescriptor(uint32_t index);
    void LoadEnhancedTexture(uint32_t index, const std::string& path, const FtexTexture* source = nullptr);
    // a mod's <stem>.png in place of the game's texture (docs/modding.md); kWhite when it cannot be used
    uint32_t LoadModImage(const QarArchive& qar, const std::string& key, const std::string& stem, const std::vector<uint8_t>& png, bool raw);

    vk::Context* ctx_ = nullptr;
    // the trilinear sampler of the original, kept for the cube maps; sampler_ is the 2D textures' (base_sampler_ unless
    // anisotropic filtering is on)
    VkSampler base_sampler_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    int anisotropy_ = 0;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    std::vector<vk::Image> images_;
    // per image: a cube map, which binding 0 shows as the white texture
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
    struct EnhancedPrepared {
        uint32_t index = 0;
        std::string path;
        uint64_t key = 0;
        FtexTexture cached;
    };
    struct EnhancedQueue {
        std::mutex mutex;
        std::deque<EnhancedPrepared> ready;
        std::atomic<bool> stop{false};
        std::atomic<size_t> done{0};
        size_t total = 0;
    };
    std::shared_ptr<EnhancedQueue> enhanced_queue_;
    std::future<void> enhanced_worker_;
    void StopEnhancedWorker();
    // DecodeAhead: the decodes by texture stem (nullptr when the file could not be read), their queue order, the workers
    std::unordered_map<std::string, std::shared_future<std::shared_ptr<FtexTexture>>> decoding_;
    std::deque<std::string> decode_order_;
    std::vector<std::future<void>> decode_workers_;
    double read_ms_ = 0.0;
    double upload_ms_ = 0.0;
};

uint32_t FormatBlockBytes(VkFormat format, bool& compressed);

}
