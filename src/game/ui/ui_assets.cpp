#include "engine/platform/os.h"
#include "game/ui/ui_assets.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>
#include <stb_image.h>

#include "engine/assets/ftex.h"
#include "engine/core/log.h"
#include "engine/core/localized_text.h"
#include "engine/core/pathcode.h"
#include "engine/core/strcode.h"
#include "engine/data/fox2.h"
#include "engine/fs/data_pack.h"
#include "engine/fs/vfs.h"
#include "engine/render/texture_manager.h"
#include "engine/ui/ui_batch.h"

namespace pt::game {
namespace {

bool PreviewAllocationFails() {
    static const bool fails = [] {
        const char* env = std::getenv("PT_TEST_PREVIEW_FAIL");
        return env && std::string_view(env) == "alloc";
    }();
    return fails;
}

constexpr std::chrono::seconds kPreviewMemoryRetry{1};

bool PictureExists(const std::string& path) {
    if (IsPackPath(path)) return ExtrasPack().Contains(PackEntryOf(path));
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(pt::os::PathFromUtf8(path), ec);
}

unsigned char* LoadPicture(const std::string& path, int& width, int& height, std::string& failure) {
    int channels = 0;
    unsigned char* pixels = nullptr;
    if (IsPackPath(path)) {
        const auto bytes = ExtrasPack().Read(PackEntryOf(path));
        if (!bytes || bytes->size() > 0x7FFFFFFFu) {
            failure = "not readable from the data pack";
            return nullptr;
        }
        pixels = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width, &height, &channels, 4);
    } else {
        pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    }
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        failure = reason ? reason : "unknown";
    }
    return pixels;
}

}

UiAssets::~UiAssets() {
    if (preview_worker_.joinable()) preview_worker_.join();
}

void UiAssets::PreloadPreviews(const std::vector<std::string>& paths) {
    UploadDecodedPreviews();
    std::vector<std::string> wanted;
    for (const std::string& path : paths) {
        if (path.empty() || texture_cache_.contains(path) || std::find(preview_pending_.begin(), preview_pending_.end(), path) != preview_pending_.end() ||
            PreviewWaitsForMemory(path) || !PictureExists(path)) {
            continue;
        }
        wanted.push_back(path);
        preview_pending_.push_back(path);
    }
    if (wanted.empty()) return;
    std::lock_guard lock(preview_mutex_);
    preview_queue_.insert(preview_queue_.end(), wanted.begin(), wanted.end());
    if (preview_worker_busy_) return;
    if (preview_worker_.joinable()) preview_worker_.join();
    preview_worker_busy_ = true;
    try {
        preview_worker_ = std::thread([this] { DecodePreviews(); });
    } catch (const std::system_error& e) {
        preview_worker_busy_ = false;
        for (const std::string& path : preview_queue_) std::erase(preview_pending_, path);
        preview_queue_.clear();
        LogWarn("ui: preview decoder thread not started ({}); previews are decoded when they show", e.what());
    }
}

void UiAssets::DecodePreviews() {
    for (;;) {
        std::string path;
        {
            std::lock_guard lock(preview_mutex_);
            if (preview_queue_.empty()) {
                preview_worker_busy_ = false;
                return;
            }
            path = std::move(preview_queue_.front());
            preview_queue_.erase(preview_queue_.begin());
        }
        try {
            DecodedPreview decoded{path};
            unsigned char* pixels = nullptr;
            try {
                if (PreviewAllocationFails()) throw std::bad_alloc();
                pixels = LoadPicture(path, decoded.width, decoded.height, decoded.failure);
                if (pixels) decoded.rgba.assign(pixels, pixels + static_cast<size_t>(decoded.width) * decoded.height * 4);
            } catch (const std::bad_alloc&) {
                decoded.rgba = {};
                decoded.failure = "out of memory";
                decoded.out_of_memory = true;
            }
            if (pixels) stbi_image_free(pixels);
            std::lock_guard lock(preview_mutex_);
            preview_done_.push_back(std::move(decoded));
        } catch (const std::exception&) {
            std::lock_guard lock(preview_mutex_);
            ++preview_lost_;
        }
    }
}

bool UiAssets::PreviewWaitsForMemory(const std::string& path) const {
    const auto it = preview_memory_retry_.find(path);
    return it != preview_memory_retry_.end() && std::chrono::steady_clock::now() < it->second;
}

void UiAssets::PreviewFailed(const std::string& path, std::string_view reason, bool out_of_memory) noexcept {
    try {
        if (out_of_memory) preview_memory_retry_[path] = std::chrono::steady_clock::now() + kPreviewMemoryRetry;
        std::string key = path + '\n' + std::string(reason);
        if (preview_logged_.insert(std::move(key)).second) {
            LogWarn("ui: preview {} not shown ({}); {}", path, reason, out_of_memory ? "tried again in a second" : "tried again when it shows");
        }
    } catch (const std::exception&) {
    }
}

void UiAssets::UploadDecodedPreviews() {
    std::vector<DecodedPreview> done;
    bool idle = false;
    size_t lost = 0;
    {
        std::lock_guard lock(preview_mutex_);
        done.swap(preview_done_);
        idle = !preview_worker_busy_ && preview_queue_.empty();
        lost = std::exchange(preview_lost_, 0);
    }
    for (DecodedPreview& decoded : done) {
        std::erase(preview_pending_, decoded.path);
        if (decoded.rgba.empty()) {
            PreviewFailed(decoded.path, decoded.failure, decoded.out_of_memory);
            continue;
        }
        try {
            if (PreviewAllocationFails()) throw std::bad_alloc();
            UploadImage(decoded.path, decoded.width, decoded.height, false, decoded.rgba);
            preview_memory_retry_.erase(decoded.path);
        } catch (const std::bad_alloc&) {
            texture_cache_.erase(decoded.path);
            PreviewFailed(decoded.path, "out of memory for its texture", true);
        }
    }
    if (idle && !preview_pending_.empty()) {
        LogWarn("ui: {} previews not handed over by the decoder ({} lost); they are decoded when they show", preview_pending_.size(), lost);
        preview_pending_.clear();
    }
}

uint32_t UiAssets::LocalPreview(const std::string& path) {
    UploadDecodedPreviews();
    if (auto it = texture_cache_.find(path); it != texture_cache_.end()) return it->second;
    if (std::find(preview_pending_.begin(), preview_pending_.end(), path) != preview_pending_.end()) return 0;
    if (PreviewWaitsForMemory(path)) return 0;
    if (!PictureExists(path)) return 0;
    unsigned char* pixels = nullptr;
    try {
        if (PreviewAllocationFails()) throw std::bad_alloc();
        int width = 0, height = 0;
        std::string failure;
        pixels = LoadPicture(path, width, height, failure);
        if (!pixels) {
            PreviewFailed(path, failure, false);
            return 0;
        }
        std::vector<uint8_t> rgba(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
        pixels = nullptr;
        const auto texture = UploadImage(path, width, height, false, rgba);
        texture_cache_[path] = texture;
        preview_memory_retry_.erase(path);
        return texture;
    } catch (const std::bad_alloc&) {
        if (pixels) stbi_image_free(pixels);
        texture_cache_.erase(path);
        PreviewFailed(path, "out of memory", true);
        return 0;
    }
}

namespace {

constexpr const char* kFontFiles[4] = {
    "/Assets/sh/font/font_def_ltn.ffnt",
    "/Assets/sh/font/font_def_jp.ffnt",
    "/Assets/sh/font/LatinFont.ffnt",
    "/Assets/sh/font/KanjiFont.ffnt",
};

constexpr int kJapanese = 4;

}

bool UiAssets::Init(Vfs& vfs, TextureManager& textures) {
    vfs_ = &vfs;
    textures_ = &textures;
    vfs.LoadPackage("/Assets/sh/ui/ui_default_data.fpk");
    vfs.LoadPackage("/Assets/sh/ui/ui_default_data.fpkd");
    vfs.LoadPackage("/Assets/sh/ui/ui_default_lang.fpkd");
    vfs.LoadPackage("/Assets/sh/level/common/resident.fpkd");
    LoadPalettes();
    LoadGenerator();
    if (!Font(UiFontType::System, 0) || !Font(UiFontType::Movie, 0)) {
        LogError("ui: Latin fonts missing");
        return false;
    }
    return Options(0) != nullptr;
}

void UiAssets::LoadPalettes() {
    for (const char* path : {"/Assets/sh/ui/GraphAsset/Common/data/common_art.fox2", "/Assets/sh/ui/common_subtitle.fox2"}) {
        auto data = vfs_->ReadFile(path);
        fox2::DataSetFile file;
        if (!data || !file.Load(path, *data)) {
            LogWarn("ui: font palette {} missing", path);
            continue;
        }
        for (const fox2::Entity& e : file.Entities()) {
            if (e.class_name != "UiFontDataElement") {
                continue;
            }
            UiFontStyle style;
            style.name = file.GetString(e, "fontName");
            style.language = file.GetString(e, "language");
            style.width = file.GetFloat(e, "fontWidth", 0, 20.0f);
            style.height = file.GetFloat(e, "fontHeight", 0, 20.0f);
            style.text_space = file.GetFloat(e, "textSpace");
            style.line_space = file.GetFloat(e, "lineSpace");
            style.edge = file.GetVec4(e, "fontEdge");
            if (!style.name.empty()) {
                styles_.push_back(std::move(style));
            }
        }
    }
    LogInfo("ui: {} font styles", styles_.size());
}

void UiAssets::LoadGenerator() {
    const char* path = "/Assets/sh/ui/Subtitles/boot/subtitle_boot.fox2";
    auto data = vfs_->ReadFile(path);
    fox2::DataSetFile file;
    if (!data || !file.Load(path, *data)) {
        LogWarn("ui: {} missing, subtitle generator defaults used", path);
        return;
    }
    for (const fox2::Entity& e : file.Entities()) {
        if (e.class_name != "SubtitlesGenerator") {
            continue;
        }
        generator_.color = file.GetVec4(e, "color");
        const glm::vec4 offset = file.GetVec4(e, "offset");
        const glm::vec4 size = file.GetVec4(e, "size");
        generator_.offset = {offset.x, offset.y};
        generator_.size = {size.x, size.y};
        generator_.font_space = file.GetFloat(e, "fontSpace");
        generator_.line_space = file.GetFloat(e, "lineSpace");
        generator_.h_align = file.GetInt(e, "hAlign");
        generator_.v_align = file.GetInt(e, "vAlign");
        generator_.b_align = file.GetInt(e, "bAlign");
        generator_.auto_line_feed = file.GetBool(e, "autoLineFeed", 0, true);
    }
}

UiFont* UiAssets::Font(UiFontType type, int language) {
    const int index = language >= 7 && language < kLanguageCount ? 6 + (language - 7) * 3 + static_cast<int>(type) : static_cast<int>(type) * 2 + (language == kJapanese ? 1 : 0);
    const int file_index = language >= 7 ? (type == UiFontType::Movie ? 2 : 0) : index % 4;
    if (!fonts_[index] && std::chrono::steady_clock::now() < font_retry_[index]) {
        return nullptr;
    }
    if (!fonts_[index]) {
        fonts_[index] = std::make_unique<UiFont>();
        UiFont& font = *fonts_[index];
        auto data = vfs_->ReadFile(kFontFiles[file_index]);
        std::string error;
        if (!data || !font.font.Parse(*data, &error)) {
            LogError("ui: font {}: {}", kFontFiles[file_index], data ? error : std::string("not found"));
            return nullptr;
        }
        std::vector<std::vector<uint8_t>> mips;
        if (language >= 7) {
            const char* family = language == 8 ? "Noto Sans SC" : language == 9 ? (type == UiFontType::Movie ? "Noto Naskh Arabic" : "Noto Kufi Arabic") : "Noto Sans";
            if (!font.font.LoadUnicodeFont(family, localized::Characters(language), language == 9, &error)) {
                LogError("ui: Unicode font {}: {}", family, error); fonts_[index].reset(); return nullptr;
            }
        } else if (language != kJapanese) font.font.AddTurkishGlyphs();
        font.crisp = type == UiFontType::PcSystem;
        font.font.BuildAtlas(mips, font.crisp ? 3 : 1, !font.crisp);
        std::vector<TextureMip> texture_mips;
        uint32_t w = font.font.AtlasWidth();
        uint32_t h = font.font.AtlasHeight();
        for (const auto& mip : mips) {
            texture_mips.push_back({w, h, mip});
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        font.atlas = textures_->Create(std::format("ui:font:{}:{}", kFontFiles[file_index], index), gpu::Format::R8Unorm, texture_mips);
        if (font.atlas == TextureManager::kWhite) {
            LogWarn("ui: font {} atlas not made (out of video memory), tried again in 2 s", kFontFiles[file_index]);
            fonts_[index].reset();
            font_retry_[index] = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            return nullptr;
        }
        font.ready = true;
        LogInfo("ui: font {} ({} glyphs, em {}, pad {}, atlas {}x{})", kFontFiles[file_index], font.font.Glyphs().size(), font.font.EmSize(), font.font.Pad(),
                font.font.AtlasWidth(), font.font.AtlasHeight());
    }
    return fonts_[index]->ready ? fonts_[index].get() : nullptr;
}

namespace {

std::vector<uint32_t> CodePoints(std::string_view text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t code = c;
        size_t n = 1;
        if (c >= 0xF0) { code = c & 0x07; n = 4; }
        else if (c >= 0xE0) { code = c & 0x0F; n = 3; }
        else if (c >= 0xC0) { code = c & 0x1F; n = 2; }
        for (size_t k = 1; k < n && i + k < text.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += n;
        if (code >= 32) out.push_back(code);
    }
    return out;
}

}

UiFont* UiAssets::HandFont(int language, std::string_view characters) {
    if (language < 0 || language >= kLanguageCount || !vfs_) return nullptr;
    std::unique_ptr<UiFont>& slot = hand_fonts_[static_cast<size_t>(language)];
    if (slot) return slot->ready ? slot.get() : nullptr;
    slot = std::make_unique<UiFont>();
    const char* family = language == kJapanese ? "Yomogi" : language == 8 ? "Long Cang" : language == 9 ? "Aref Ruqaa" : "Marck Script";
    auto data = vfs_->ReadFile(kFontFiles[2]);
    std::string error;
    if (!data || !slot->font.Parse(*data, &error) || !slot->font.LoadUnicodeFont(family, characters, language == 9, &error)) {
        LogError("ui: the hand font {} for language {}: {}", family, language, error);
        return nullptr;
    }
    std::vector<std::vector<uint8_t>> mips;
    slot->font.BuildAtlas(mips, 1, true);
    std::vector<TextureMip> texture_mips;
    uint32_t w = slot->font.AtlasWidth(), h = slot->font.AtlasHeight();
    for (const auto& mip : mips) {
        texture_mips.push_back({w, h, mip});
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    slot->atlas = textures_->Create(std::format("ui:font:hand:{}", language), gpu::Format::R8Unorm, texture_mips);
    if (slot->atlas == TextureManager::kWhite) {
        LogWarn("ui: the hand font for language {}: no video memory for its atlas", language);
        return nullptr;
    }
    slot->ready = true;
    LogInfo("ui: hand font {} for language {} ({} glyphs, atlas {}x{})", family, language, slot->font.Glyphs().size(), slot->font.AtlasWidth(),
            slot->font.AtlasHeight());
    return slot.get();
}

UiFont* UiAssets::FontCovering(UiFontType type, int language, std::string_view text) {
    UiFont* base = Font(type, language);
    if (!base) return nullptr;
    const std::vector<uint32_t> codes = CodePoints(text);
    bool missing = false;
    for (uint32_t code : codes) missing = missing || (code != 0x20 && code != 0x3000 && !base->font.Find(code));
    if (!missing) return base;
    const std::pair<int, int> key(static_cast<int>(type), language);
    if (auto it = covering_fonts_.find(key); it != covering_fonts_.end()) return it->second && it->second->ready ? it->second.get() : base;
    auto made = std::make_unique<UiFont>();
    const int file_index = language >= 7 ? (type == UiFontType::Movie ? 2 : 0) : (static_cast<int>(type) * 2 + (language == kJapanese ? 1 : 0)) % 4;
    auto data = vfs_->ReadFile(kFontFiles[file_index]);
    std::string error;
    const char* family = language == 8 || language == kJapanese ? "Noto Sans SC"
                         : language == 9                         ? (type == UiFontType::Movie ? "Noto Naskh Arabic" : "Noto Kufi Arabic")
                                                                 : "Noto Sans";
    if (!data || !made->font.Parse(*data, &error) ||
        !made->font.LoadUnicodeFont(family, localized::Characters(language) + std::string(text), language == 9, &error)) {
        LogError("ui: a font covering the text in language {} ({}): {}", language, family, error);
        covering_fonts_[key] = nullptr;
        return base;
    }
    made->crisp = type == UiFontType::PcSystem;
    std::vector<std::vector<uint8_t>> mips;
    made->font.BuildAtlas(mips, made->crisp ? 3 : 1, !made->crisp);
    std::vector<TextureMip> texture_mips;
    uint32_t w = made->font.AtlasWidth(), h = made->font.AtlasHeight();
    for (const auto& mip : mips) {
        texture_mips.push_back({w, h, mip});
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    made->atlas = textures_->Create(std::format("ui:font:covering:{}:{}", static_cast<int>(type), language), gpu::Format::R8Unorm, texture_mips);
    if (made->atlas == TextureManager::kWhite) {
        LogWarn("ui: a font covering the text in language {}: no video memory for its atlas", language);
        return base;
    }
    made->ready = true;
    LogInfo("ui: font {} covering the port's text in language {} ({} glyphs, atlas {}x{})", family, language, made->font.Glyphs().size(),
            made->font.AtlasWidth(), made->font.AtlasHeight());
    UiFont* out = made.get();
    covering_fonts_[key] = std::move(made);
    return out;
}

const UiFontStyle* UiAssets::FontStyle(std::string_view name, int language) const {
    const std::string_view code = language >= 0 && language < kLanguageCount ? kLanguageCodes[language] : "";
    const UiFontStyle* fallback = nullptr;
    for (const UiFontStyle& style : styles_) {
        if (style.name != name) {
            continue;
        }
        if (!code.empty() && style.language == code) {
            return &style;
        }
        if (style.language.empty() && !fallback) {
            fallback = &style;
        }
    }
    return fallback;
}

const UiFontStyle* UiAssets::FontStyleByHash(uint64_t hash, int language) const {
    for (const UiFontStyle& style : styles_) {
        if (StrCode64(style.name) == hash) {
            return FontStyle(style.name, language);
        }
    }
    return nullptr;
}

const ui::LangFile* UiAssets::Options(int language) {
    if (language >= 7) return Options(0);
    if (language < 0 || language >= kLanguageCount) {
        language = 0;
    }
    if (!options_[language]) {
        auto file = std::make_unique<ui::LangFile>();
        const std::string path = std::format("/Assets/sh/lang/ui/OPTIONS.lng#{}", kLanguageCodes[language]);
        auto data = vfs_->ReadFile(path);
        std::string error;
        if (!data || !file->Parse(*data, &error)) {
            LogError("ui: {}: {}", path, data ? error : std::string("not found"));
            return language == 0 ? nullptr : Options(0);
        }
        options_[language] = std::move(file);
    }
    return options_[language].get();
}

const ui::LangFile* UiAssets::System(int language) {
    if (language < 0 || language >= 7 || language >= kLanguageCount) {
        language = 0;
    }
    if (!system_[language]) {
        auto file = std::make_unique<ui::LangFile>();
        const std::string path = std::format("/Assets/sh/lang/ui/SYSTEM.lng#{}", kLanguageCodes[language]);
        auto data = vfs_->ReadFile(path);
        std::string error;
        if (!data || !file->Parse(*data, &error)) {
            LogError("ui: {}: {}", path, data ? error : std::string("not found"));
            return language == 0 ? nullptr : System(0);
        }
        system_[language] = std::move(file);
    }
    return system_[language].get();
}

uint32_t UiAssets::MakeTexture(const std::string& name, uint32_t width, uint32_t height, std::span<const uint8_t> rgba) {
    if (!textures_ || rgba.size() != size_t(width) * height * 4) return 0;
    const TextureMip mip{width, height, rgba};
    const uint32_t made = textures_->Create(name, gpu::Format::R8G8B8A8Unorm, std::span<const TextureMip>(&mip, 1));
    return made == TextureManager::kWhite ? 0 : made;
}

uint32_t UiAssets::Texture(std::string_view path) {
    auto it = texture_cache_.find(std::string(path));
    if (it != texture_cache_.end()) {
        return it->second;
    }
    bool ok = false;
    const uint32_t index = textures_->LoadFox(vfs_->Textures(), std::string(path), &ok);
    if (!ok) {
        LogWarn("ui: texture {} missing", path);
    } else if (auto header = ReadTextureFile(vfs_->Textures(), FtexStem(path) + ".ftex"); header && header->size() >= 0x14) {
        const uint16_t address = static_cast<uint16_t>((*header)[0x12] | ((*header)[0x13] << 8));
        texture_address_[index] = ((address & 0xF) == 0 ? ui::kUiClampU : 0u) | (((address >> 4) & 0xF) == 0 ? ui::kUiClampV : 0u);
    }
    texture_cache_[std::string(path)] = index;
    return index;
}

uint32_t UiAssets::TextureAddressBits(uint32_t index) const {
    auto it = texture_address_.find(index);
    return it != texture_address_.end() ? it->second : 0u;
}

const ui::UifModel* UiAssets::Model(std::string_view path) {
    auto it = models_.find(std::string(path));
    if (it != models_.end()) {
        return it->second.get();
    }
    std::unique_ptr<ui::UifModel> model;
    if (auto data = vfs_->ReadFile(path)) {
        model = std::make_unique<ui::UifModel>();
        std::string error;
        if (!model->Parse(*data, &error)) {
            LogError("ui: {}: {}", path, error);
            model.reset();
        }
    } else {
        LogWarn("ui: model {} not found", path);
    }
    const ui::UifModel* out = model.get();
    models_[std::string(path)] = std::move(model);
    return out;
}

}
