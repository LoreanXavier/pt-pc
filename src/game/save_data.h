#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pt::game {

struct GameOptions {
    bool invert_x = false;
    bool invert_y = false;
    int subtitle_language = 0;
    bool subtitles = false;
    int brightness = 5;

    float BrightnessValue() const;
};

GameOptions DefaultOptionsForLocale(std::string_view locale);
std::string SystemLanguageTag();

struct SaveProgress {
    std::string floor = "f000";
    std::array<uint8_t, 5> cleared{};
    uint32_t photo_word = 0;
    bool game_plus = false;
    uint32_t finishes = 0;
};

struct SaveFile {
    GameOptions options;
    SaveProgress progress;
};

enum class SaveLoadStatus { Ok, NotFound, Broken, Unreadable, Old };

struct SaveLoadResult {
    SaveLoadStatus status = SaveLoadStatus::NotFound;
    std::optional<SaveFile> file;
};

enum class SaveWriteStatus { Ok, NoSpace, Failed };

class SaveStore {
public:
    static constexpr uint64_t kMagic = 0x6E69777470;
    static constexpr uint32_t kVersion = 3;

    void SetDirectory(const std::filesystem::path& directory, const std::string& name) {
        directory_ = directory;
        name_ = name;
    }
    bool Enabled() const { return !directory_.empty(); }
    std::optional<SaveFile> Load() { return LoadDetailed().file; }
    SaveLoadResult LoadDetailed();
    bool Save(const SaveFile& file);
    SaveWriteStatus LastWrite() const { return last_write_; }
    bool Reset();

private:
    std::filesystem::path Slot(int index) const;
    struct SlotRead {
        SaveLoadStatus status = SaveLoadStatus::NotFound;
        uint32_t sequence = 0;
        SaveFile file;
    };
    SlotRead Read(const std::filesystem::path& path) const;

    std::filesystem::path directory_;
    std::string name_ = "PT_Save_Data";
    uint32_t sequence_ = 0;
    int last_slot_ = 1;
    SaveWriteStatus last_write_ = SaveWriteStatus::Ok;
};

}
