#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine/ui/ffnt.h"
#include "engine/ui/lang_file.h"
#include "engine/ui/uif.h"
#include "game/ui/ui_icons.h"

namespace pt {
class Vfs;
class TextureManager;
}

namespace pt::game {

enum class UiFontType { System = 0, Movie = 1, PcSystem = 2 };

struct UiFontStyle {
    std::string name;
    std::string language;
    float width = 20.0f;
    float height = 20.0f;
    float text_space = 0.0f;
    float line_space = 0.0f;
    glm::vec4 edge{0.0f};
};

struct SubtitleGeneratorSettings {
    glm::vec4 color{1.0f};
    glm::vec2 offset{0.0f, -16.0f};
    glm::vec2 size{27.0f, 38.0f};
    float font_space = 0.0f;
    float line_space = 0.0f;
    int h_align = 0;
    int v_align = 2;
    int b_align = 1;
    bool auto_line_feed = true;
};

struct UiFont {
    ui::FfntFont font;
    uint32_t atlas = 0;
    bool ready = false;
    bool crisp = false;
};

class UiAssets {
public:
    static constexpr int kLanguageCount = 13;
    static constexpr const char* kLanguageCodes[kLanguageCount] = {"eng", "fra", "deu", "spa", "jpn", "ita", "por", "tur", "zhs", "ara", "rus", "ukr", "ces"};

    UiAssets() = default;
    UiAssets(const UiAssets&) = delete;
    UiAssets& operator=(const UiAssets&) = delete;
    ~UiAssets();

    bool Init(Vfs& vfs, TextureManager& textures);

    UiFont* Font(UiFontType type, int language);
    const UiFontStyle* FontStyle(std::string_view name, int language) const;
    const UiFontStyle* FontStyleByHash(uint64_t hash, int language) const;
    const ui::LangFile* Options(int language);
    const ui::LangFile* System(int language);
    const SubtitleGeneratorSettings& Generator() const { return generator_; }
    uint32_t Texture(std::string_view path);
    uint32_t LocalPreview(const std::string& path);
    void PreloadPreviews(const std::vector<std::string>& paths);
    uint32_t TextureAddressBits(uint32_t index) const;
    const ui::UifModel* Model(std::string_view path);
    bool BuildPcIcons();
    PromptGlyph PromptPicture(const Prompt& prompt, const PromptStyle& style);
    Vfs& Files() { return *vfs_; }

private:
    struct PromptArt;

    void LoadPalettes();
    void LoadGenerator();
    uint32_t UploadImage(const std::string& name, int width, int height, bool srgb, const std::vector<uint8_t>& rgba);
    const PromptGlyph& GeneratedGlyph(const std::string& id);

    Vfs* vfs_ = nullptr;
    TextureManager* textures_ = nullptr;
    std::array<std::unique_ptr<UiFont>, 24> fonts_;
    std::vector<UiFontStyle> styles_;
    std::array<std::unique_ptr<ui::LangFile>, kLanguageCount> options_;
    std::array<std::unique_ptr<ui::LangFile>, kLanguageCount> system_;
    std::unordered_map<std::string, uint32_t> texture_cache_;
    std::unordered_map<uint32_t, uint32_t> texture_address_;
    std::map<std::string, std::unique_ptr<ui::UifModel>> models_;
    SubtitleGeneratorSettings generator_;
    bool pc_icons_built_ = false;
    bool pc_icons_ready_ = false;
    std::shared_ptr<PromptArt> prompt_art_;
    struct DecodedPreview {
        std::string path;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };
    void UploadDecodedPreviews();
    std::mutex preview_mutex_;
    std::vector<std::string> preview_queue_;
    std::vector<DecodedPreview> preview_done_;
    std::vector<std::string> preview_pending_;
    std::thread preview_worker_;
    bool preview_worker_busy_ = false;
};

}
