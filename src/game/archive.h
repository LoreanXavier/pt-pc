#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pt::game {

// The Archive (PC extra, Extras page; docs/gameplay.md 14.7): a museum of the game's own content, read from the player's installed
// game data at run time. Nothing of the base game changes: the pictures and sounds are shown and played as the game has them, and the
// cutscenes and models run in a session of their own (archive_theater.h) while the player's game stays paused behind the menu.
// Every entry is one row of this table; a new entry (the unused content fix-extras2 lists) is one more row.
enum class ArchiveSection : uint8_t { Images, Voices, Photos, Cutscenes, Models, Unused };
inline constexpr int kArchiveSectionCount = 6;

enum class ArchiveMedia : uint8_t {
    // a texture (asset: its .ftex), full screen
    Image,
    // a subliminal string (asset: its _alp .ftex, alpha only), full screen in the subliminal service's shading
    String,
    // a sound event (asset: the event name) with the transcript of its subtitle (extra: the subtitle id)
    Sound,
    // a gimmick dialogue (asset: the speaker's model prefix, extra: the marker condition), as the bag and the baby talk
    Dialogue,
    // a photo piece (asset: its .fmdl), or with no asset the photo the pieces make in the frame
    Photo,
    // a demo (asset: the demo id; extra: how it starts, ArchiveDemoStart) in the theater
    Demo,
    // a model (asset: a .fmdl, or a gimmick name with extra: its motion key) in the theater's model viewer
    Model,
};

struct ArchiveEntry {
    // stable name: the unlock record (pt.ini [progress] archive) and the input script's `sarchive`
    std::string_view id;
    ArchiveSection section = ArchiveSection::Images;
    ArchiveMedia media = ArchiveMedia::Image;
    // text key (pc_settings.cpp and the language tables); "{1}" in its text takes `number`. A key starting with "pc_loop_" is a
    // loop browser label and gets " (number)" after it when number > 0
    std::string_view label;
    int number = 0;
    std::string_view asset;
    std::string_view extra;
    // Demo, Model: the floor the theater's session plays it on
    std::string_view floor;
    // what reaching it in play records (Game::NoteArchive): "" is "demo:<asset>" for demos and the entry's own id otherwise.
    // Keys: sub:<1..10>, noise, overlay, voice:<subtitle id>, event:<sound event>, photo:<piece>, photo:complete, demo:<id>,
    // gimmick:<name>, finished (the game was finished once)
    std::string_view unlock;
    // the help line's text key
    std::string_view note;
};

std::span<const ArchiveEntry> ArchiveEntries();
const ArchiveEntry* FindArchiveEntry(std::string_view id);
// the unlock key of an entry (ArchiveEntry::unlock, or its default)
std::string ArchiveUnlockKey(const ArchiveEntry& entry);
// a key some entry is unlocked by (Game::NoteArchive keeps only these)
bool IsArchiveUnlockKey(std::string_view key);
// the section's title text key and its row's help text key
std::string_view ArchiveSectionTitle(ArchiveSection section);
std::string_view ArchiveSectionNote(ArchiveSection section);
// the row label of an entry in a language: its text with {1}, or a loop label with its number
std::string ArchiveLabel(const ArchiveEntry& entry, int language);
// the plaque's second line under an exhibit of the Museum: the entry's names in the game's data (its asset's stem, its motion,
// subtitle or floor), the same in every language
std::string ArchiveCaption(const ArchiveEntry& entry);
// the entry whose picture stands for this one on the Museum's wall when it has none of its own (a voice: what speaks it, the
// radio, the phone, the bag...); empty when the entry has its own
std::string_view ArchivePictureOf(const ArchiveEntry& entry);
// the exhibit whose picture is on a hall's doorway (the Museum's halls page) when it is open; another open one stands in else
std::string_view ArchiveSectionCover(ArchiveSection section);
// the Museum's thumbnail capture (main.cpp, --make-museum-previews): the frames the theater shows the entry before its shot
int ArchiveThumbnailFrames(const ArchiveEntry& entry);

}
