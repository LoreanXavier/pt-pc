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
    // opens another page of the source (Activate), without the confirmation of an action
    bool link = false;
    int label_lines = 1;
    std::string note;
    // a value with a note here cannot be chosen: the page shows it greyed with this note while the cursor rests on it and
    // does not pass it to the source (an upscaler the GPU or the SDK cannot run)
    std::vector<std::string> value_notes;
    bool ValueDisabled(int v) const { return v >= 0 && v < static_cast<int>(value_notes.size()) && !value_notes[v].empty(); }
};

struct PcSettingSection {
    std::string title;
    int column = 0;
    std::vector<PcSettingRow> rows;
};

// The panel right of a browser page's rows (the loop browser's preview, the Archive's pictures and transcripts; options_menu.cpp
// draws it), and the same picture over the whole screen while FullScreen() holds
struct PcPanel {
    // a texture of the game's data (.ftex); `string`: a subliminal string, drawn in the subliminal service's shading
    std::string texture;
    bool string = false;
    // a photo piece (.fmdl) cut out of its picture, or "complete": the photo the pieces make in the frame
    std::string photo;
    // a picture file on disk (the loop browser's previews)
    std::string file;
    // lines of text (a transcript), the one being spoken highlighted
    std::vector<std::string> lines;
    int current_line = -1;
    // the entry's name, over the full screen view
    std::string title;
    // the Museum (archive_view.h): a picture file on disk shot of the entry (a cutscene's or model's thumbnail), the plaque's
    // second line, and a locked exhibit (an empty frame, no picture)
    std::string thumbnail;
    std::string caption;
    bool locked = false;
    // how much the wall lifts a thumbnail shot in the theater (the hallway and the start room are dark; a model on a table darker)
    float lift = 1.0f;
    bool Empty() const { return texture.empty() && photo.empty() && file.empty() && lines.empty() && thumbnail.empty(); }
    bool HasPicture() const { return !texture.empty() || !photo.empty() || !file.empty() || !thumbnail.empty(); }
};

// a page laid out as the Museum's (archive_view.h): the halls, six framed doorways in a grid, or a hall's wall, a strip of framed
// exhibits under the one in the spotlight
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
    // the page was opened from the options menu: the source shows its first page
    virtual void Opened() {}
    virtual void Closed() {}
    virtual std::string_view Title() const { return "pc_title"; }
    virtual int RefreshEveryFrames() const { return 15; }
    // back was pressed: true when the source went back to its first page from another one (the page stays open)
    virtual bool Back() { return false; }
    virtual bool IsLoopBrowser() const { return false; }
    // rows 2 units apart with smaller labels (the loop browser, the speedrun's results)
    virtual bool CompactRows() const { return IsLoopBrowser(); }
    virtual std::string PreviewFile(int) const { return {}; }
    // a page laid out as the loop browser's (compact rows, a panel on the right) and the panel for a row
    virtual bool IsBrowser() const { return IsLoopBrowser(); }
    virtual PcGallery Gallery() const { return PcGallery::None; }
    virtual PcPanel Panel(int id) const {
        PcPanel panel;
        panel.file = PreviewFile(id);
        return panel;
    }
    // the current row's panel fills the screen (an Archive picture); left and right step through the page's rows (StepRow), any
    // other button goes back (Back)
    virtual bool FullScreen() const { return false; }
    // the hint shown at the bottom of the full screen view
    virtual std::string_view FullScreenHint() const { return {}; }
    // a row was left by moving the cursor (an Archive voice stops playing)
    virtual void CursorMoved() {}
    // the page's cursor row to restore after the source changed the page by itself (the theater's return), -1 none
    virtual int TakeCursor() { return -1; }
    // the last Activate kept its page (an Archive entry that plays or opens in place): the cursor stays on its row
    virtual bool KeepsCursor() const { return false; }
    // every preview picture the page can show, the current row's first (preloaded when the page opens)
    virtual std::vector<std::string> PreviewFiles(int) const { return {}; }
    // a small line in the page's bottom right corner (the Extras page's credit), not translated
    virtual std::string_view Credit() const { return {}; }
};

std::vector<std::string_view> PcDescriptionKeys();

std::string_view PcText(std::string_view key, int language);

// a row note that carries values: its text key, then each value after a kPcNoteArgument, which replace {1}, {2}... in the
// translated text
constexpr char kPcNoteArgument = '';
std::string PcNoteText(std::string_view note, int language);

}
