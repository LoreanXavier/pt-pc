#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pt::game {

enum class ArchiveSection : uint8_t { Images, Voices, Photos, Cutscenes, Models, Unused };
inline constexpr int kArchiveSectionCount = 6;

enum class ArchiveMedia : uint8_t {
    Image,
    String,
    Sound,
    Dialogue,
    Photo,
    Demo,
    Model,
};

struct ArchiveEntry {
    std::string_view id;
    ArchiveSection section = ArchiveSection::Images;
    ArchiveMedia media = ArchiveMedia::Image;
    std::string_view label;
    int number = 0;
    std::string_view asset;
    std::string_view extra;
    std::string_view floor;
    std::string_view unlock;
    std::string_view note;
};

std::span<const ArchiveEntry> ArchiveEntries();
const ArchiveEntry* FindArchiveEntry(std::string_view id);
std::string ArchiveUnlockKey(const ArchiveEntry& entry);
bool IsArchiveUnlockKey(std::string_view key);
std::string_view ArchiveSectionTitle(ArchiveSection section);
std::string_view ArchiveSectionNote(ArchiveSection section);
std::string ArchiveLabel(const ArchiveEntry& entry, int language);
std::string ArchiveCaption(const ArchiveEntry& entry);
std::string_view ArchivePictureOf(const ArchiveEntry& entry);
std::string_view ArchiveSectionCover(ArchiveSection section);
int ArchiveThumbnailFrames(const ArchiveEntry& entry);

}
