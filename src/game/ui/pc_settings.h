#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
    std::string note;
    std::vector<std::string> value_notes;
    bool ValueDisabled(int v) const { return v >= 0 && v < static_cast<int>(value_notes.size()) && !value_notes[v].empty(); }
};

struct PcSettingSection {
    std::string title;
    int column = 0;
    std::vector<PcSettingRow> rows;
};

struct PcPanel {
    std::string texture;
    bool string = false;
    std::string photo;
    std::string file;
    std::vector<std::string> lines;
    int current_line = -1;
    std::string title;
    std::string thumbnail;
    std::string caption;
    bool locked = false;
    float lift = 1.0f;
    bool Empty() const { return texture.empty() && photo.empty() && file.empty() && lines.empty() && thumbnail.empty(); }
    bool HasPicture() const { return !texture.empty() || !photo.empty() || !file.empty() || !thumbnail.empty(); }
};

enum class PcGallery : uint8_t { None, Halls, Wall };

class PcSettingsSource {
public:
    virtual ~PcSettingsSource() = default;
    virtual std::vector<PcSettingSection> Sections() = 0;
    virtual std::vector<std::string> DescriptionSamples() {
        std::vector<std::string> notes;
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
    virtual void CursorMoved() {}
    virtual int TakeCursor() { return -1; }
    virtual bool KeepsCursor() const { return false; }
    virtual std::vector<std::string> PreviewFiles(int) const { return {}; }
    virtual std::string_view Credit() const { return {}; }
};

std::vector<std::string_view> PcDescriptionKeys();

std::string_view PcText(std::string_view key, int language);

constexpr char kPcNoteArgument = '';
std::string PcNoteText(std::string_view note, int language);

}
