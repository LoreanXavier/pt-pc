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

// The options before any save is read (Options_StaticInit 0x91D7B0): brightness 5, no axis inversion, subtitles on, and the
// subtitle language from the system language (sceSystemServiceParamGetInt(1)): Japanese 4, English (US, UK) 0 with the subtitles
// off, French 1, Spanish (and Latin American) 3, German 2, Italian 5, Portuguese (PT, BR) 6, any other language 0 with the
// subtitles on. The port's added languages map the same way: Turkish 7, Simplified Chinese 8, Arabic 9, Russian 10,
// Ukrainian 11, Czech 12. `locale` is a BCP 47 tag ("en-US", "zh-Hans-CN", "pt-BR").
GameOptions DefaultOptionsForLocale(std::string_view locale);
// the Windows UI language (GetUserDefaultUILanguage) as a BCP 47 tag; PT_SYSTEM_LANGUAGE overrides it (the tests set en-US, the
// language of the reference captures' system)
std::string SystemLanguageTag();

struct SaveProgress {
    std::string floor = "f000";
    std::array<uint8_t, 5> cleared{};
    uint32_t photo_word = 0;
    bool game_plus = false;
    // finished games counted by the port (Game+ content by finish: 1 the bathtub Lisa, 2 Lisa's hsh0 arm); a tag without a
    // count (before the count was kept) reads as 1
    uint32_t finishes = 0;
};

struct SaveFile {
    GameOptions options;
    SaveProgress progress;
};

// The boot load's outcome as SaveRequest_OnBootLoadDone (0x947C60) branches on the job's result (0x928DA0):
// Ok (0): the data is applied; NotFound (4): first boot, a new save; Broken (0xD, the system's check of the data failed): dialog
// sys_load_failed_3, then a new save; Unreadable (0x14 wrong magic, 0x13 a newer version than 3, other errors): dialog
// sys_load_failed_2 and nothing written; Old (version below 3): the buffer is cleared and the result stays 0, so nothing is loaded
// and nothing is shown.
enum class SaveLoadStatus { Ok, NotFound, Broken, Unreadable, Old };

struct SaveLoadResult {
    SaveLoadStatus status = SaveLoadStatus::NotFound;
    std::optional<SaveFile> file;
};

// a failed write: the job's result 9 (no space; the system dialog, then one retry) or another error (sys_save_failed_4)
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
