#include "game/archive.h"

#include <array>
#include <format>
#include <unordered_set>

#include "game/ui/pc_settings.h"

namespace pt::game {
namespace {

using S = ArchiveSection;
using M = ArchiveMedia;

constexpr std::string_view kNoteImage = "pc_note_archive_image";
constexpr std::string_view kNoteVoice = "pc_note_archive_voice";
constexpr std::string_view kNotePhoto = "pc_note_archive_photo";
constexpr std::string_view kNoteDemo = "pc_note_archive_demo";
constexpr std::string_view kNoteModel = "pc_note_archive_model";

constexpr ArchiveEntry kEntries[] = {
    {.id = "sub01", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 1,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub01_alp.ftex", .unlock = "sub:1", .note = kNoteImage},
    {.id = "sub02", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 2,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub02_alp.ftex", .unlock = "sub:2", .note = kNoteImage},
    {.id = "sub03", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 3,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub03_alp.ftex", .unlock = "sub:3", .note = kNoteImage},
    {.id = "sub04", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 4,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub04_alp.ftex", .unlock = "sub:4", .note = kNoteImage},
    {.id = "sub05", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 5,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub05_alp.ftex", .unlock = "sub:5", .note = kNoteImage},
    {.id = "sub06", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 6,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub06_alp.ftex", .unlock = "sub:6", .note = kNoteImage},
    {.id = "sub07", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 7,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub07_alp.ftex", .unlock = "sub:7", .note = kNoteImage},
    {.id = "sub08", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 8,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub08_alp.ftex", .unlock = "sub:8", .note = kNoteImage},
    {.id = "sub09", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 9,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub09_alp.ftex", .unlock = "sub:9", .note = kNoteImage},
    {.id = "sub10", .section = S::Images, .media = M::String, .label = "pc_archive_sub", .number = 10,
     .asset = "/Assets/sh/effect/vfx_pic/text/text_sub10_alp.ftex", .unlock = "sub:10", .note = kNoteImage},
    {.id = "noise", .section = S::Images, .media = M::Image, .label = "pc_archive_noise",
     .asset = "/Assets/sh/effect/vfx_pic/noise/Noise_00007.ftex", .unlock = "noise", .note = kNoteImage},
    {.id = "noise_normal", .section = S::Images, .media = M::Image, .label = "pc_archive_noise_normal",
     .asset = "/Assets/sh/effect/vfx_pic/noise/Noise_00007_nrm.ftex", .unlock = "noise", .note = kNoteImage},
    {.id = "peephole_mask", .section = S::Images, .media = M::Image, .label = "pc_archive_peephole_mask",
     .asset = "/Assets/sh/effect/vfx_pic/holl/holl_002_alp.ftex", .unlock = "overlay", .note = kNoteImage},

    {.id = "radio_news", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_news", .asset = "Play_radio_f010",
     .extra = "tria1000_101010", .unlock = "voice:tria1000_101010", .note = kNoteVoice},
    {.id = "radio_news2", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_news2", .asset = "Play_radio_f050",
     .extra = "tria1000_1n1010", .unlock = "voice:tria1000_1n1010", .note = kNoteVoice},
    {.id = "radio_01", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_voice", .number = 1, .asset = "Play_radio_voice_01",
     .extra = "tria1000_181010", .unlock = "voice:tria1000_181010", .note = kNoteVoice},
    {.id = "radio_02", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_voice", .number = 2, .asset = "Play_radio_voice_02",
     .extra = "tria1000_191010", .unlock = "voice:tria1000_191010", .note = kNoteVoice},
    {.id = "radio_03", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_voice", .number = 3, .asset = "Play_radio_voice_03",
     .extra = "tria1000_1a1010", .unlock = "voice:tria1000_1a1010", .note = kNoteVoice},
    {.id = "radio_04", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_voice", .number = 4, .asset = "Play_radio_voice_04",
     .extra = "tria1000_1b1010", .unlock = "voice:tria1000_1b1010", .note = kNoteVoice},
    {.id = "radio_05", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_voice", .number = 5, .asset = "Play_radio_voice_05",
     .extra = "tria1000_1c1010", .unlock = "voice:tria1000_1c1010", .note = kNoteVoice},
    {.id = "radio_06", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_voice", .number = 6, .asset = "Play_radio_voice_06",
     .extra = "tria1000_1d1010", .unlock = "voice:tria1000_1d1010", .note = kNoteVoice},
    {.id = "radio_07", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_voice", .number = 7, .asset = "Play_radio_voice_07",
     .extra = "tria1000_1e1010", .unlock = "voice:tria1000_1e1010", .note = kNoteVoice},
    {.id = "radio_08", .section = S::Voices, .media = M::Sound, .label = "pc_archive_radio_voice", .number = 8, .asset = "Play_radio_voice_08",
     .extra = "tria1000_1f1010", .unlock = "voice:tria1000_1f1010", .note = kNoteVoice},
    {.id = "riddle_1", .section = S::Voices, .media = M::Sound, .label = "pc_archive_riddle", .number = 1, .asset = "Play_radio_riddle_01",
     .extra = "tria1000_151010", .unlock = "voice:tria1000_151010", .note = kNoteVoice},
    {.id = "riddle_2", .section = S::Voices, .media = M::Sound, .label = "pc_archive_riddle", .number = 2, .asset = "Play_radio_riddle_02",
     .extra = "tria1000_161010", .unlock = "voice:tria1000_161010", .note = kNoteVoice},
    {.id = "sermon", .section = S::Voices, .media = M::Sound, .label = "pc_archive_sermon", .asset = "Play_radio_religion_01",
     .unlock = "event:Play_radio_religion_01", .note = kNoteVoice},
    {.id = "phone", .section = S::Voices, .media = M::Sound, .label = "pc_archive_phone", .asset = "Play_tel_voice_01",
     .extra = "tria1000_171010", .unlock = "voice:tria1000_171010", .note = kNoteVoice},
    {.id = "peephole_voice", .section = S::Voices, .media = M::Sound, .label = "pc_archive_peephole_voice", .asset = "Play_Peephole_Theater",
     .extra = "tria1000_151010", .unlock = "event:Play_Peephole_Theater", .note = kNoteVoice},
    {.id = "bag", .section = S::Voices, .media = M::Dialogue, .label = "pc_archive_bag", .asset = "pab0", .extra = "TRIA1000_111010_0_mib",
     .unlock = "voice:tria1000_111010", .note = kNoteVoice},
    {.id = "fetus_1", .section = S::Voices, .media = M::Sound, .label = "pc_archive_fetus", .number = 1, .asset = "Play_voice_baby_mimicry_01",
     .extra = "tria1000_121010", .unlock = "voice:tria1000_121010", .note = kNoteVoice},
    {.id = "fetus_2", .section = S::Voices, .media = M::Sound, .label = "pc_archive_fetus", .number = 2, .asset = "Play_voice_baby_mimicry_02",
     .extra = "tria1000_1g1010", .unlock = "voice:tria1000_1g1010", .note = kNoteVoice},

    {.id = "photo_lisa", .section = S::Photos, .media = M::Photo, .label = "pc_archive_photo_lisa",
     .asset = "/Assets/sh/environ/object/shsb/label/shsb_labl001/scenes/shsb_labl001_mapc004.fmdl", .unlock = "photo:PhotoLisa", .note = kNotePhoto},
    {.id = "photo_tree", .section = S::Photos, .media = M::Photo, .label = "pc_archive_photo_tree",
     .asset = "/Assets/sh/environ/object/shsb/label/shsb_labl001/scenes/shsb_labl001_mapc005.fmdl", .unlock = "photo:PhotoTree", .note = kNotePhoto},
    {.id = "photo_gap", .section = S::Photos, .media = M::Photo, .label = "pc_archive_photo_gap",
     .asset = "/Assets/sh/environ/object/shsb/label/shsb_labl001/scenes/shsb_labl001_mapc008.fmdl", .unlock = "photo:PhotoGap", .note = kNotePhoto},
    {.id = "photo_bath", .section = S::Photos, .media = M::Photo, .label = "pc_archive_photo_bath",
     .asset = "/Assets/sh/environ/object/shsb/label/shsb_labl001/scenes/shsb_labl001_mapc003.fmdl", .unlock = "photo:PhotoBath", .note = kNotePhoto},
    {.id = "photo_stair", .section = S::Photos, .media = M::Photo, .label = "pc_archive_photo_stair",
     .asset = "/Assets/sh/environ/object/shsb/label/shsb_labl001/scenes/shsb_labl001_mapc006.fmdl", .unlock = "photo:PhotoStair", .note = kNotePhoto},
    {.id = "photo_option", .section = S::Photos, .media = M::Photo, .label = "pc_archive_photo_option",
     .asset = "/Assets/sh/environ/object/shsb/label/shsb_labl001/scenes/shsb_labl001_mapc007.fmdl", .unlock = "photo:PhotoOption", .note = kNotePhoto},
    {.id = "photo_complete", .section = S::Photos, .media = M::Photo, .label = "pc_archive_photo_complete", .unlock = "photo:complete",
     .note = kNotePhoto},

    {.id = "preface", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_preface", .asset = "gc_p02_500", .extra = "preface",
     .floor = "f000", .note = kNoteDemo},
    {.id = "awakening", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_awakening", .asset = "gc_p00_022", .extra = "opening",
     .floor = "f000", .note = kNoteDemo},
    {.id = "opening", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_opening", .asset = "gc_p00_020", .extra = "opening",
     .floor = "f010", .note = kNoteDemo},
    {.id = "first_exit", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_first_exit", .asset = "gc_p00_160", .extra = "room",
     .floor = "f000", .note = kNoteDemo},
    {.id = "door", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_door", .asset = "gc_p00_010", .extra = "room",
     .floor = "f010", .note = kNoteDemo},
    {.id = "clock", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_clock", .asset = "gc_p05_010", .floor = "f010", .note = kNoteDemo},
    {.id = "gc_p04_310", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f005", .number = 1, .asset = "gc_p04_310", .floor = "f005",
     .note = kNoteDemo},
    {.id = "gc_p01_070", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f005", .number = 2, .asset = "gc_p01_070", .floor = "f005",
     .note = kNoteDemo},
    {.id = "gc_p04_340", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f020", .number = 1, .asset = "gc_p04_340", .floor = "f020",
     .note = kNoteDemo},
    {.id = "gc_p01_081", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f020", .number = 2, .asset = "gc_p01_081", .floor = "f020",
     .note = kNoteDemo},
    {.id = "gc_p01_011", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f030", .number = 1, .asset = "gc_p01_011", .floor = "f030",
     .note = kNoteDemo},
    {.id = "gc_p01_020", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f030", .number = 2, .asset = "gc_p01_020", .floor = "f030",
     .note = kNoteDemo},
    {.id = "gc_p03_010", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f030", .number = 3, .asset = "gc_p03_010", .floor = "f030",
     .note = kNoteDemo},
    {.id = "gc_p01_021", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f030", .number = 4, .asset = "gc_p01_021", .floor = "f030",
     .note = kNoteDemo},
    {.id = "gc_p01_010", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f030", .number = 5, .asset = "gc_p01_010", .floor = "f030",
     .note = kNoteDemo},
    {.id = "gc_p04_300", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f040", .number = 1, .asset = "gc_p04_300", .floor = "f040",
     .note = kNoteDemo},
    {.id = "gc_p04_350", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f040", .number = 2, .asset = "gc_p04_350", .floor = "f040",
     .note = kNoteDemo},
    {.id = "gc_p03_070", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f060", .number = 1, .asset = "gc_p03_070", .floor = "f060",
     .note = kNoteDemo},
    {.id = "gc_p03_011", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f060", .number = 2, .asset = "gc_p03_011", .floor = "f060",
     .note = kNoteDemo},
    {.id = "gc_p01_110", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f060", .number = 3, .asset = "gc_p01_110", .floor = "f060",
     .note = kNoteDemo},
    {.id = "gc_p00_030", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f060", .number = 4, .asset = "gc_p00_030", .floor = "f060",
     .note = kNoteDemo},
    {.id = "gc_p01_050", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f060", .number = 5, .asset = "gc_p01_050", .extra = "lit", .floor = "f060",
     .note = kNoteDemo},
    {.id = "gc_p01_090", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f060", .number = 6, .asset = "gc_p01_090", .extra = "lit", .floor = "f060",
     .note = kNoteDemo},
    {.id = "gc_p01_022", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f060", .number = 7, .asset = "gc_p01_022", .extra = "lit", .floor = "f060",
     .note = kNoteDemo},
    {.id = "gc_p04_320", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f050a", .asset = "gc_p04_320", .floor = "f050", .note = kNoteDemo},
    {.id = "gc_p04_120", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f070", .number = 1, .asset = "gc_p04_120", .floor = "f070",
     .note = kNoteDemo},
    {.id = "gc_p04_280", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f070", .number = 2, .asset = "gc_p04_280", .floor = "f070",
     .note = kNoteDemo},
    {.id = "gc_p02_520", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f080", .asset = "gc_p02_520", .floor = "f080", .note = kNoteDemo},
    {.id = "gc_p02_090", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f110", .asset = "gc_p02_090", .floor = "f110", .note = kNoteDemo},
    {.id = "gc_p02_080", .section = S::Cutscenes, .media = M::Demo, .label = "pc_loop_f120", .asset = "gc_p02_080", .floor = "f120", .note = kNoteDemo},
    {.id = "kill", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_kill", .asset = "gc_p07_030", .extra = "kill", .floor = "f040",
     .note = kNoteDemo},
    {.id = "ending", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_ending", .asset = "gc_p06_010_final", .extra = "ending",
     .floor = "f160", .note = kNoteDemo},
    {.id = "teaser", .section = S::Cutscenes, .media = M::Demo, .label = "pc_archive_teaser", .asset = "gc_p06_010_final", .extra = "teaser",
     .floor = "f160", .unlock = "finished", .note = kNoteDemo},

    {.id = "lisa_stand", .section = S::Models, .media = M::Model, .label = "pc_archive_lisa_stand", .asset = "Ocho", .extra = "OchoStop",
     .floor = "f010", .unlock = "gimmick:Ocho", .note = kNoteModel},
    {.id = "lisa_walk", .section = S::Models, .media = M::Model, .label = "pc_archive_lisa_walk", .asset = "Ocho", .extra = "Ocho",
     .floor = "f010", .unlock = "gimmick:Ocho", .note = kNoteModel},
    {.id = "lisa_dash", .section = S::Models, .media = M::Model, .label = "pc_archive_lisa_dash", .asset = "Ocho", .extra = "OchoDash",
     .floor = "f010", .unlock = "gimmick:Ocho", .note = kNoteModel},
    {.id = "lisa_door", .section = S::Models, .media = M::Model, .label = "pc_archive_lisa_door",
     .asset = "/Assets/sh/environ/object/shsb/house/shsb_hous001/scenes/shsb_hous001_ocho001.fmdl", .floor = "f010", .unlock = "demo:gc_p00_022",
     .note = kNoteModel},
    {.id = "baby", .section = S::Models, .media = M::Model, .label = "pc_archive_baby", .asset = "Baby", .extra = "Baby", .floor = "f010",
     .unlock = "gimmick:Baby", .note = kNoteModel},
    {.id = "fridge", .section = S::Models, .media = M::Model, .label = "pc_archive_fridge", .asset = "Freezer", .extra = "Freezer", .floor = "f010",
     .unlock = "gimmick:Freezer", .note = kNoteModel},
    {.id = "bag_model", .section = S::Models, .media = M::Model, .label = "pc_archive_bag_model", .asset = "Bag", .extra = "BagTalk", .floor = "f010",
     .unlock = "gimmick:Bag", .note = kNoteModel},
    {.id = "lamp", .section = S::Models, .media = M::Model, .label = "pc_archive_lamp", .asset = "CeilLamp", .extra = "CeilLamp", .floor = "f010",
     .unlock = "gimmick:CeilLamp", .note = kNoteModel},
    {.id = "roach", .section = S::Models, .media = M::Model, .label = "pc_archive_roach", .asset = "/Assets/sh/chara/coc/Scenes/coc0_main0_def.fmdl",
     .floor = "f010", .unlock = "demo:gc_p02_510", .note = kNoteModel},
    {.id = "flashlight", .section = S::Models, .media = M::Model, .label = "pc_archive_flashlight",
     .asset = "/Assets/sh/item/lig/Scenes/shl0_main0_def.fmdl", .floor = "f010", .unlock = "demo:gc_p00_030", .note = kNoteModel},
    {.id = "radio", .section = S::Models, .media = M::Model, .label = "pc_archive_radio",
     .asset = "/Assets/sh/environ/object/shsb/radio/shsb_radi001/scenes/shsb_radi001.fmdl", .floor = "f010", .unlock = "floor:f010",
     .note = kNoteModel},
    {.id = "phone_model", .section = S::Models, .media = M::Model, .label = "pc_archive_phone_model",
     .asset = "/Assets/sh/environ/object/shsb/telephone/shsb_tlph001/scenes/shsb_tlph001.fmdl", .floor = "f010", .unlock = "floor:f160",
     .note = kNoteModel},
    {.id = "xmark_photo", .section = S::Models, .media = M::Model, .label = "pc_archive_xmark_photo",
     .asset = "/Assets/sh/environ/object/shsb/house/shsb_hous001/scenes/shsb_hous001_pc1a.fmdl", .floor = "f010", .unlock = "floor:f050",
     .note = kNoteModel},
    {.id = "clock_model", .section = S::Models, .media = M::Model, .label = "pc_archive_clock_model",
     .asset = "/Assets/sh/environ/object/shsb/clock/shsb_clck001/scenes/shsb_clck001.fmdl", .floor = "f010", .unlock = "floor:f005",
     .note = kNoteModel},
    {.id = "man", .section = S::Models, .media = M::Model, .label = "pc_archive_man", .asset = "/Assets/sh/chara/plr/Scenes/plr0_main0_def.fmdl",
     .floor = "f010", .unlock = "demo:gc_p06_010_final", .note = kNoteModel},

    {.id = "bathtub_lisa", .section = S::Unused, .media = M::Model, .label = "pc_archive_bathtub",
     .asset = "/Assets/sh/environ/object/shsb/bath/shsb_bath001/scenes/shsb_bath001_ocho001.fmdl", .floor = "f010", .unlock = "finished",
     .note = "pc_note_archive_unused"},
};

const std::unordered_set<std::string>& UnlockKeys() {
    static const std::unordered_set<std::string> keys = [] {
        std::unordered_set<std::string> out;
        for (const ArchiveEntry& e : kEntries) out.insert(ArchiveUnlockKey(e));
        return out;
    }();
    return keys;
}

}

std::span<const ArchiveEntry> ArchiveEntries() {
    return kEntries;
}

const ArchiveEntry* FindArchiveEntry(std::string_view id) {
    for (const ArchiveEntry& e : kEntries) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::string ArchiveUnlockKey(const ArchiveEntry& entry) {
    if (!entry.unlock.empty()) return std::string(entry.unlock);
    if (entry.media == ArchiveMedia::Demo) return "demo:" + std::string(entry.asset);
    return std::string(entry.id);
}

bool IsArchiveUnlockKey(std::string_view key) {
    return UnlockKeys().contains(std::string(key));
}

std::string_view ArchiveSectionTitle(ArchiveSection section) {
    static constexpr std::array<std::string_view, kArchiveSectionCount> kTitles = {
        "pc_archive_images", "pc_archive_voices", "pc_archive_photos", "pc_archive_cutscenes", "pc_archive_models", "pc_archive_unused"};
    return kTitles[static_cast<size_t>(section)];
}

std::string_view ArchiveSectionNote(ArchiveSection section) {
    static constexpr std::array<std::string_view, kArchiveSectionCount> kNotes = {
        "pc_note_archive_images", "pc_note_archive_voices", "pc_note_archive_photos", "pc_note_archive_cutscenes", "pc_note_archive_models",
        "pc_note_archive_unused_section"};
    return kNotes[static_cast<size_t>(section)];
}

std::string ArchiveLabel(const ArchiveEntry& entry, int language) {
    std::string text(PcText(entry.label, language));
    if (entry.label.starts_with("pc_loop_")) {
        return entry.number > 0 ? std::format("{} ({})", text, entry.number) : text;
    }
    if (const size_t at = text.find("{1}"); at != std::string::npos) text.replace(at, 3, std::to_string(entry.number));
    return text;
}

std::string ArchiveCaption(const ArchiveEntry& entry) {
    auto stem = [](std::string_view path) {
        const size_t slash = path.find_last_of('/');
        std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
    };
    switch (entry.media) {
    case ArchiveMedia::Image:
    case ArchiveMedia::String:
        return stem(entry.asset);
    case ArchiveMedia::Sound:
    case ArchiveMedia::Dialogue:
        return entry.extra.empty() ? std::string(entry.asset) : std::format("{} / {}", entry.asset, entry.extra);
    case ArchiveMedia::Photo:
        return entry.asset.empty() ? std::string("shsb_labl001 / pt14_hallway") : stem(entry.asset);
    case ArchiveMedia::Demo:
        return std::format("{} / {}", entry.asset, entry.floor);
    case ArchiveMedia::Model:
        return entry.extra.empty() ? stem(entry.asset) : std::format("{} / {}", entry.asset, entry.extra);
    }
    return {};
}

std::string_view ArchivePictureOf(const ArchiveEntry& entry) {
    if (entry.media != ArchiveMedia::Sound && entry.media != ArchiveMedia::Dialogue) return {};
    if (entry.asset.starts_with("Play_radio")) return "radio";
    if (entry.asset.starts_with("Play_tel")) return "phone_model";
    if (entry.asset.starts_with("Play_Peephole")) return "peephole_mask";
    if (entry.asset.starts_with("Play_voice_baby")) return "baby";
    if (entry.media == ArchiveMedia::Dialogue) return "bag_model";
    return {};
}

std::string_view ArchiveSectionCover(ArchiveSection section) {
    static constexpr std::array<std::string_view, kArchiveSectionCount> kCovers = {"sub01", "radio_news", "photo_complete", "door", "lisa_stand", "bathtub_lisa"};
    return kCovers[static_cast<size_t>(section)];
}

int ArchiveThumbnailFrames(const ArchiveEntry& entry) {
    if (entry.media == ArchiveMedia::Model) return 30;
    if (entry.id == "preface" || entry.id == "awakening") return 420;
    if (entry.id == "teaser") return 60;
    if (entry.id == "ending") return 600;
    return 150;
}

}
