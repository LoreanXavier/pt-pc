#include "game/ui/ui_assets.h"

#include <algorithm>
#include <format>
#include <filesystem>
#include <stb_image.h>

#include "engine/assets/ftex.h"
#include "engine/core/log.h"
#include "engine/core/localized_text.h"
#include "engine/core/pathcode.h"
#include "engine/core/strcode.h"
#include "engine/data/fox2.h"
#include "engine/fs/vfs.h"
#include "engine/render/texture_manager.h"
#include "engine/ui/ui_batch.h"

namespace pt::game {
UiAssets::~UiAssets() {
    if (preview_worker_.joinable()) preview_worker_.join();
}

void UiAssets::PreloadPreviews(const std::vector<std::string>& paths) {
    UploadDecodedPreviews();
    std::vector<std::string> wanted;
    for (const std::string& path : paths) {
        std::error_code ec;
        if (path.empty() || texture_cache_.contains(path) || std::find(preview_pending_.begin(), preview_pending_.end(), path) != preview_pending_.end() ||
            !std::filesystem::exists(path, ec)) {
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
    preview_worker_ = std::thread([this] {
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
            DecodedPreview decoded{path};
            int channels = 0;
            if (unsigned char* pixels = stbi_load(path.c_str(), &decoded.width, &decoded.height, &channels, 4)) {
                decoded.rgba.assign(pixels, pixels + static_cast<size_t>(decoded.width) * decoded.height * 4);
                stbi_image_free(pixels);
            }
            std::lock_guard lock(preview_mutex_);
            preview_done_.push_back(std::move(decoded));
        }
    });
}

void UiAssets::UploadDecodedPreviews() {
    std::vector<DecodedPreview> done;
    {
        std::lock_guard lock(preview_mutex_);
        done.swap(preview_done_);
    }
    for (DecodedPreview& decoded : done) {
        std::erase(preview_pending_, decoded.path);
        // a picture that failed to decode is decoded again on demand (LocalPreview)
        if (!decoded.rgba.empty()) UploadImage(decoded.path, decoded.width, decoded.height, true, decoded.rgba);
    }
}

uint32_t UiAssets::LocalPreview(const std::string& path) {
    UploadDecodedPreviews();
    if (auto it = texture_cache_.find(path); it != texture_cache_.end()) return it->second;
    if (std::find(preview_pending_.begin(), preview_pending_.end(), path) != preview_pending_.end()) return 0;
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) return 0;
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) return 0;
    std::vector<uint8_t> rgba(pixels, pixels + width * height * 4);
    stbi_image_free(pixels);
    const auto texture = UploadImage(path, width, height, true, rgba);
    texture_cache_[path] = texture;
    return texture;
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
        font.atlas = textures_->Create(std::format("ui:font:{}:{}", kFontFiles[file_index], index), VK_FORMAT_R8_UNORM, texture_mips);
        font.ready = true;
        LogInfo("ui: font {} ({} glyphs, em {}, pad {}, atlas {}x{})", kFontFiles[file_index], font.font.Glyphs().size(), font.font.EmSize(), font.font.Pad(),
                font.font.AtlasWidth(), font.font.AtlasHeight());
    }
    return fonts_[index]->ready ? fonts_[index].get() : nullptr;
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
