#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/ui/crosshair.h"

namespace pt::game {

struct PcSettingRow {
    int id = 0;
    std::string label;
    std::vector<std::string> values;
    int value = 0;
    bool enabled = true;
    bool wrap = true;
    bool action = false;
    bool link = false;
    bool dropdown = false;
    int label_lines = 1;
    std::string note;
    std::string confirm_note;
    bool confirm_default_yes = false;
    std::vector<std::string> value_notes;
    bool ValueDisabled(int v) const { return v >= 0 && v < static_cast<int>(value_notes.size()) && !value_notes[v].empty(); }
    std::vector<std::string> value_warnings;
    std::string_view ValueWarning(int v) const {
        return v >= 0 && v < static_cast<int>(value_warnings.size()) ? std::string_view(value_warnings[v]) : std::string_view();
    }
};

struct PcSettingSection {
    std::string title;
    int column = 0;
    std::vector<PcSettingRow> rows;
};

struct PcPanel {
    std::string texture;
    bool string = false;
    float aspect = 0.0f;
    std::string photo;
    std::string file;
    std::vector<std::string> lines;
    int current_line = -1;
    std::string title;
    std::string thumbnail;
    std::string caption;
    bool locked = false;
    bool audio = false;
    bool music = false;
    float music_position = 0.0f;
    float music_length = 0.0f;
    bool music_playing = false;
    bool music_loop = false;
    float lift = 1.0f;
    std::optional<CrosshairStyle> crosshair;
    bool Empty() const { return texture.empty() && photo.empty() && file.empty() && lines.empty() && thumbnail.empty(); }
    bool HasPicture() const { return !texture.empty() || !photo.empty() || !file.empty() || !thumbnail.empty(); }
};

struct PcScreen {
    std::string header;
    std::string label;
    std::string value;
    std::string note;
    std::string hint;
    bool scene = false;
    int pattern = 0;
    bool can_left = true;
    bool can_right = true;
};

enum class PcGallery : uint8_t { None, Halls, Wall };

enum class PcMusicCommand : uint8_t { PlayPause, SeekBy, SeekTo, Restart, Loop };

struct ArmorySnapshot {
    int weapon = 1;
    int mode = 0;
    int filter = 0;
    int selected_skin = 0;
    int selected_anim = 0;
    int selected_torch = 0;
    int credits = 0;
    int best_wave = 0;
    int daily_wins = 0;
    int equipped_skin = 0;
    int equipped_anim = 0;
    int equipped_torch = 0;
    bool playing_reload = false;
    int playing_style = 0;
    uint64_t owned_skins = 1ull;
    uint32_t owned_reloads = 1u;
    uint32_t owned_torches = 1u;
    std::vector<int> filtered_skins;
};

struct NightHubStat {
    std::string label_key;
    std::string value;
    bool value_is_key = false;
};

struct NightHubItem {
    int id = 0;
    std::string label_key;
    std::string tag;
    bool tag_is_key = false;
    std::string note;
    std::string detail_sub;
    std::vector<NightHubStat> stats;
};

struct NightHubSection {
    std::string title_key;
    std::vector<NightHubItem> items;
};

struct NightHubSnapshot {
    int credits = 0;
    int best_wave = 0;
    int daily_streak = 0;
    std::vector<NightHubSection> sections;
};

class PcSettingsSource {
public:
    virtual ~PcSettingsSource() = default;
    virtual std::vector<PcSettingSection> Sections() = 0;
    virtual std::optional<PcSettingRow> FooterAction() { return std::nullopt; }
    virtual std::vector<std::string> DescriptionSamples() {
        std::vector<std::string> notes;
        if (const auto footer = FooterAction(); footer && !footer->note.empty()) notes.push_back(footer->note);
        for (const auto& section : Sections()) for (const auto& row : section.rows) if (!row.note.empty()) notes.push_back(row.note);
        return notes;
    }
    virtual void Set(int id, int value) = 0;
    virtual void Activate(int id) = 0;
    virtual void Opened() {}
    virtual void Closed() {}
    virtual std::string_view Title() const { return "pc_title"; }
    virtual int RefreshEveryFrames() const { return 15; }
    virtual bool Back() { return false; }
    virtual bool IsLoopBrowser() const { return false; }
    virtual bool CompactRows() const { return IsLoopBrowser(); }
    virtual std::string PreviewFile(int) const { return {}; }
    virtual bool IsBrowser() const { return IsLoopBrowser(); }
    virtual PcGallery Gallery() const { return PcGallery::None; }
    virtual PcPanel Panel(int id) const {
        PcPanel panel;
        panel.file = PreviewFile(id);
        return panel;
    }
    virtual bool FullScreen() const { return false; }
    virtual std::string_view FullScreenHint() const { return {}; }
    virtual bool ScreenShown() const { return false; }
    virtual bool SeeThrough() const { return false; }
    virtual PcScreen Screen() const { return {}; }
    virtual void ScreenInput(int, int, bool, bool) {}
    virtual void CursorMoved() {}
    virtual void Music(int, PcMusicCommand, float) {}
    virtual int TakeCursor() { return -1; }
    virtual bool KeepsCursor() const { return false; }
    virtual std::vector<std::string> PreviewFiles(int) const { return {}; }
    virtual std::string_view Credit() const { return {}; }
    virtual std::string_view Question() const { return {}; }
    virtual bool FindSettingRow(std::string_view, PcSettingRow&) { return false; }
    virtual int TabCount() const { return 0; }
    virtual int Tab() const { return 0; }
    virtual std::string_view TabLabel(int) const { return {}; }
    virtual std::string_view TabNote(int) const { return {}; }
    virtual void SelectTab(int) {}
    virtual void Donate() {}
    virtual bool SupportPrompts() const { return true; }
    virtual void MenuShown() {}
    virtual bool ShowSettingPage(std::string_view) { return false; }
    virtual bool OpenCrosshair() { return false; }
    virtual bool OpenSurvivalNight() { return false; }
    virtual bool OpenSurvivalNightFromDeath() { return false; }
    virtual bool OpenSurvivalPromo() { return false; }
    virtual bool TakeReturnToOptions() { return false; }
    virtual bool TakeCloseToGame() { return false; }
    virtual bool SurvivalPromoDue() const { return false; }
    virtual bool SurvivalUnlocked() const { return false; }
    virtual void DismissSurvivalPromo() {}
    virtual bool IsArmory() const { return false; }
    virtual ArmorySnapshot ArmoryData() const { return {}; }
    virtual void ArmorySelectWeapon(int) {}
    virtual void ArmorySelectMode(int) {}
    virtual void ArmorySelectFilter(int) {}
    virtual void ArmoryFocusItem(int) {}
    virtual void ArmoryAction(bool) {}
    virtual void ArmoryPreviewReload() {}
    virtual void ArmoryOrbit(float, float) {}
    virtual bool IsNightHub() const { return false; }
    virtual NightHubSnapshot NightHubData() const { return {}; }
};

std::vector<std::string_view> PcDescriptionKeys();

std::string_view PcText(std::string_view key, int language);

constexpr char kPcNoteArgument = '';
std::string PcNoteText(std::string_view note, int language);

}
