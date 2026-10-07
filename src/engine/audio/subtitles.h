#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pt {
class Vfs;
}

namespace pt::audio {

struct SubtitleLine {
    float start_seconds = 0.0f;
    float end_seconds = 0.0f;
    std::string text;
};

struct SubtitleEntry {
    uint32_t key = 0;
    std::string id;
    uint8_t category = 0;
    uint8_t range = 0;
    uint16_t character = 0;
    std::vector<SubtitleLine> lines;
};

class SubtitleTable {
public:
    static std::string SubpPath(std::string_view language);
    static std::string PackagePath(std::string_view language);
    static uint32_t SubtitleKey(std::string_view subtitle_id);
    static uint64_t MarkerKey(std::string_view label);

    bool Load(Vfs& vfs, std::string_view language, std::span<const std::vector<uint8_t>> sab_tables, std::string* error);
    bool Parse(std::span<const uint8_t> subp, std::span<const std::vector<uint8_t>> sab_tables, std::string* error);

    const std::string& Language() const { return language_; }
    const std::vector<SubtitleEntry>& Entries() const { return entries_; }
    const SubtitleEntry* FindById(std::string_view subtitle_id) const;
    const SubtitleEntry* FindByKey(uint32_t key) const;
    const SubtitleEntry* FindByMarker(std::string_view label) const;
    std::string SubtitleIdForMarker(std::string_view label) const;
    const std::unordered_map<uint64_t, std::string>& MarkerLinks() const { return marker_links_; }

private:
    std::string language_;
    std::vector<SubtitleEntry> entries_;
    std::unordered_map<uint32_t, size_t> by_key_;
    std::unordered_map<uint64_t, std::string> marker_links_;
};

}
