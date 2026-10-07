#include "game/save_data.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "engine/core/log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pt::game {
namespace {

constexpr float kBrightness[11] = {0.48f, 0.55f, 0.60f, 0.68f, 0.76f, 0.84f, 0.92f, 1.00f, 1.05f, 1.09f, 1.13f};
constexpr size_t kBufferSize = 0x78;

template <typename T>
void Put(std::vector<uint8_t>& buffer, size_t offset, T value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

template <typename T>
T Get(const std::vector<uint8_t>& buffer, size_t offset) {
    T value{};
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    return value;
}

}

float GameOptions::BrightnessValue() const {
    return kBrightness[std::clamp(brightness, 0, 10)];
}

GameOptions DefaultOptionsForLocale(std::string_view locale) {
    GameOptions options;
    options.subtitles = true;
    options.subtitle_language = 0;
    std::string tag(locale);
    for (char& ch : tag) {
        ch = ch == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    const std::string primary = tag.substr(0, tag.find('-'));
    if (primary == "en") {
        options.subtitles = false;
    } else if (primary == "ja") {
        options.subtitle_language = 4;
    } else if (primary == "fr") {
        options.subtitle_language = 1;
    } else if (primary == "es") {
        options.subtitle_language = 3;
    } else if (primary == "de") {
        options.subtitle_language = 2;
    } else if (primary == "it") {
        options.subtitle_language = 5;
    } else if (primary == "pt") {
        options.subtitle_language = 6;
    } else if (primary == "tr") {
        options.subtitle_language = 7;
    } else if (primary == "zh") {
        const bool traditional = tag.find("hant") != std::string::npos || tag.find("-tw") != std::string::npos ||
                                 tag.find("-hk") != std::string::npos || tag.find("-mo") != std::string::npos;
        if (!traditional) {
            options.subtitle_language = 8;
        }
    } else if (primary == "ar") {
        options.subtitle_language = 9;
    } else if (primary == "ru") {
        options.subtitle_language = 10;
    } else if (primary == "uk") {
        options.subtitle_language = 11;
    } else if (primary == "cs") {
        options.subtitle_language = 12;
    }
    return options;
}

std::string SystemLanguageTag() {
    if (const char* v = std::getenv("PT_SYSTEM_LANGUAGE"); v && *v) {
        return v;
    }
#ifdef _WIN32
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        std::string tag;
        for (const wchar_t* p = name; *p; ++p) {
            tag.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
        }
        return tag;
    }
#endif
    return "en-US";
}

bool SaveStore::Reset() {
    if (!Enabled()) return false;
    for (int i=0;i<2;++i) {
        std::error_code error;
        const auto path=Slot(i);
        std::filesystem::remove(path,error);
        if(error) {
            LogWarn("save: cannot reset {}: {}",path.string(),error.message());
            return false;
        }
    }
    directory_.clear();
    sequence_=0;
    last_slot_=1;
    return true;
}

std::filesystem::path SaveStore::Slot(int index) const {
    return directory_ / (name_ + std::to_string(index));
}

SaveStore::SlotRead SaveStore::Read(const std::filesystem::path& path) const {
    SlotRead slot;
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return slot;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> buffer(kBufferSize + 8);
    if (in) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }
    /* No checksum in the PC file; the PS4 savedata layer did that job, so a short read is the only corruption we can detect. */
    if (!in || in.gcount() != static_cast<std::streamsize>(buffer.size())) {
        LogWarn("save: {} is broken (cannot read {} bytes)", path.string(), buffer.size());
        slot.status = SaveLoadStatus::Broken;
        return slot;
    }
    const uint32_t version = Get<uint32_t>(buffer, 0x08);
    if (Get<uint64_t>(buffer, 0x00) != kMagic || version > kVersion) {
        LogWarn("save: {} has a bad header (magic {:#x}, version {})", path.string(), Get<uint64_t>(buffer, 0x00), version);
        slot.status = SaveLoadStatus::Unreadable;
        return slot;
    }
    if (version < kVersion) {
        LogWarn("save: {} has the old version {}", path.string(), version);
        slot.status = SaveLoadStatus::Old;
        return slot;
    }
    slot.status = SaveLoadStatus::Ok;
    SaveFile& file = slot.file;
    file.options.invert_y = buffer[0x2C] != 0;
    file.options.invert_x = buffer[0x2D] != 0;
    file.options.subtitle_language = buffer[0x2E];
    file.options.subtitles = buffer[0x2F] != 0;
    file.options.brightness = buffer[0x38];
    char floor[8] = {};
    std::memcpy(floor, buffer.data() + 0x40, 7);
    file.progress.floor = floor;
    file.progress.photo_word = Get<uint32_t>(buffer, 0x5C);
    /* 'NGP1' at 0x70, bytes the original always leaves zero, so a save from the PS4 never reads as finished. */
    file.progress.game_plus = Get<uint32_t>(buffer, 0x70) == 0x3150474E;
    file.progress.finishes = file.progress.game_plus ? std::max<uint32_t>(1, Get<uint32_t>(buffer, 0x74)) : 0;
    for (size_t i = 0; i < 5; ++i) {
        file.progress.cleared[i] = buffer[0x68 + i];
    }
    slot.sequence = Get<uint32_t>(buffer, kBufferSize);
    return slot;
}

SaveLoadResult SaveStore::LoadDetailed() {
    SaveLoadResult result;
    if (!Enabled()) {
        return result;
    }
    std::optional<SlotRead> best;
    std::optional<SlotRead> newest_bad;
    std::filesystem::file_time_type newest_time{};
    for (int i = 0; i < 2; ++i) {
        SlotRead slot = Read(Slot(i));
        if (slot.status == SaveLoadStatus::Ok) {
            if (!best || slot.sequence > best->sequence) {
                best = slot;
                last_slot_ = i;
            }
        } else if (slot.status != SaveLoadStatus::NotFound) {
            std::error_code error;
            const auto time = std::filesystem::last_write_time(Slot(i), error);
            if (!newest_bad || time > newest_time) {
                newest_bad = slot;
                newest_time = time;
            }
        }
    }
    if (best) {
        sequence_ = best->sequence;
        result.status = SaveLoadStatus::Ok;
        result.file = best->file;
    } else if (newest_bad) {
        result.status = newest_bad->status;
    }
    return result;
}

bool SaveStore::Save(const SaveFile& file) {
    if (!Enabled()) {
        return false;
    }
    static int forced_left = -1;
    static SaveWriteStatus forced = SaveWriteStatus::Failed;
    if (forced_left < 0) {
        forced_left = 0;
        if (const char* v = std::getenv("PT_SAVE_FAIL")) {
            const std::string text(v);
            const size_t colon = text.find(':');
            forced = text.rfind("nospace", 0) == 0 ? SaveWriteStatus::NoSpace : SaveWriteStatus::Failed;
            forced_left = colon == std::string::npos ? 1 : std::atoi(text.c_str() + colon + 1);
        }
    }
    if (forced_left > 0) {
        --forced_left;
        last_write_ = forced;
        LogInfo("save: write failed ({}, forced by PT_SAVE_FAIL)", forced == SaveWriteStatus::NoSpace ? "no space" : "error");
        return false;
    }
    std::vector<uint8_t> buffer(kBufferSize + 8, 0);
    Put<uint64_t>(buffer, 0x00, kMagic);
    Put<uint32_t>(buffer, 0x08, kVersion);
    buffer[0x2C] = file.options.invert_y ? 1 : 0;
    buffer[0x2D] = file.options.invert_x ? 1 : 0;
    buffer[0x2E] = static_cast<uint8_t>(file.options.subtitle_language);
    buffer[0x2F] = file.options.subtitles ? 1 : 0;
    buffer[0x38] = static_cast<uint8_t>(file.options.brightness);
    std::memcpy(buffer.data() + 0x40, file.progress.floor.data(), std::min<size_t>(file.progress.floor.size(), 7));
    Put<uint32_t>(buffer, 0x5C, file.progress.photo_word);
    Put<uint32_t>(buffer, 0x70, file.progress.game_plus ? 0x3150474E : 0);
    Put<uint32_t>(buffer, 0x74, file.progress.game_plus ? file.progress.finishes : 0);
    for (size_t i = 0; i < 5; ++i) {
        buffer[0x68 + i] = file.progress.cleared[i];
    }
    Put<uint32_t>(buffer, kBufferSize, ++sequence_);
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    last_slot_ = 1 - last_slot_;
    std::ofstream out(Slot(last_slot_), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    out.close();
    const bool ok = static_cast<bool>(out);
    last_write_ = SaveWriteStatus::Ok;
    if (!ok) {
        std::error_code space_error;
        const auto space = std::filesystem::space(directory_, space_error);
        last_write_ = !space_error && space.available < 64 * 1024 ? SaveWriteStatus::NoSpace : SaveWriteStatus::Failed;
        last_slot_ = 1 - last_slot_;
        --sequence_;
    }
    LogInfo("save: slot {} written (floor {}, cleared {}{}{}{}{}, photo {:#x}) {}", last_slot_, file.progress.floor, file.progress.cleared[0],
            file.progress.cleared[1], file.progress.cleared[2], file.progress.cleared[3], file.progress.cleared[4], file.progress.photo_word,
            ok ? "" : "FAILED");
    return ok;
}

}
