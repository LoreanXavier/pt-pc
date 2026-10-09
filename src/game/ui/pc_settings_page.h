#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/ui/uia.h"
#include "engine/ui/uif.h"
#include "game/ui/pc_settings.h"

namespace pt::game {

class Game;
class UifView;
struct MenuInput;

class PcSettingsPage {
public:
    enum class Result { None, Back, Quit };
    static constexpr int kMaxRows = 40;
    static constexpr int kMaxSections = 6;

    void Build(ui::UifModel& model, int icon_atlas, int icon_glow);
    uint16_t Root() const { return root_; }
    uint16_t BackIcon() const { return back_icon_; }
    uint16_t BackGlow() const { return back_glow_; }
    uint16_t Help() const { return help_; }
    void SetFlash(const ui::UiaAnimation* animation) { flash_animation_ = animation; }
    void Open(PcSettingsSource* source);
    void Refresh();
    Result Update(Game& game, const MenuInput& input, float dt);
    void Step(bool active);
    void Apply(UifView& view, int language) const;
    int RowAt(glm::vec2 units) const;
    bool BackAt(glm::vec2 units) const;
    bool QuitAt(glm::vec2 units) const;
    bool IsLoopBrowser() const { return source_ && source_->IsLoopBrowser(); }
    bool Browser() const;
    // the Museum's pages (museum_layout.h): the cursor moves over the grid or the strip, the pointer hits their frames
    PcGallery Gallery() const;
    // the screen's width in canvas pixels, for the wall's strip (OptionsMenu::Draw)
    void SetWallWidth(float canvas_px) { wall_width_ = canvas_px; }
    int WallVisible() const;
    // the rows (the exhibits) in page order: the first shown, one by index, its panel; the first rows that are links are the
    // gallery's cells, the rest (an action row) text under them
    int First() const { return first_; }
    int CellCount() const;
    const PcSettingRow* RowAtIndex(int index) const;
    PcPanel PanelOf(int index) const;
    bool Confirming() const { return confirming_; }
    std::string PreviewFile() const { const auto* row = CurrentRow(); return row && source_ ? source_->PreviewFile(row->id) : std::string(); }
    // the panel right of a browser page's rows, for the current row
    PcPanel Panel() const { const auto* row = CurrentRow(); return row && source_ ? source_->Panel(row->id) : PcPanel{}; }
    bool FullScreen() const { return source_ && source_->FullScreen(); }
    std::string_view FullScreenHint() const { return source_ ? source_->FullScreenHint() : std::string_view(); }
    std::vector<std::string> PreviewFiles() const {
        const auto* row = CurrentRow();
        return source_ ? source_->PreviewFiles(row ? row->id : -1) : std::vector<std::string>();
    }
    std::string_view Credit() const { return source_ ? source_->Credit() : std::string_view(); }
    int Cursor() const { return cursor_; }
    int RowCount() const { return static_cast<int>(rows_.size()); }
    const PcSettingRow* CurrentRow() const;
    const PcSettingRow* FindRow(std::string_view label) const;
    // the value the row shows: a greyed value the cursor stepped onto (PcSettingRow::value_notes), else the row's value
    int ShownValue(const PcSettingRow& row) const { return row.id == preview_id_ ? preview_value_ : row.value; }
    std::string_view ShownNote(const PcSettingRow& row) const;
    bool ValueLess(const PcSettingRow& row) const;

private:
    struct Placed {
        int section = 0;
        int row = 0;
        glm::vec2 label{0.0f};
        glm::vec2 value{0.0f};
    };
    struct RowNodes {
        uint16_t label = 0;
        uint16_t group = 0;
        uint16_t value = 0;
        uint16_t decrease = 0;
        uint16_t increase = 0;
    };
    struct SectionNodes {
        uint16_t text = 0;
        uint16_t line = 0;
    };

    const PcSettingRow& RowOf(const Placed& p) const { return sections_[p.section].rows[p.row]; }
    void Layout();
    void Scroll();
    int VisibleRows() const;
    int WindowRows() const;
    int GalleryHit(glm::vec2 units) const;
    bool GalleryMove(Game& game, uint32_t act);
    glm::vec2 Shown(const Placed& p) const;
    void SourceChanged();
    void Move(Game& game, int delta);
    void SwitchColumn(Game& game);
    void Change(Game& game, int delta, bool accept);

    PcSettingsSource* source_ = nullptr;
    std::vector<PcSettingSection> sections_;
    std::vector<Placed> rows_;
    std::vector<glm::vec2> headers_;
    float row_step_ = 3.0f;
    int cursor_ = 0;
    // the first row a browser page shows (Scroll)
    int first_ = 0;
    bool confirming_ = false;
    int preview_id_ = -1;
    int preview_value_ = 0;
    float flash_frame_ = 1000.0f;
    uint32_t held_dirs_ = 0;
    float repeat_time_ = 0.0f;
    glm::vec2 last_pointer_{-1.0f};
    int refresh_frames_ = 0;
    float wall_width_ = 1280.0f;

    uint16_t root_ = 0;
    uint16_t title_ = 0;
    uint16_t help_ = 0;
    uint16_t back_icon_ = 0;
    uint16_t back_glow_ = 0;
    uint16_t back_text_ = 0;
    uint16_t quit_text_ = 0;
    uint16_t navigation_text_ = 0;
    uint16_t bar_group_ = 0;
    uint16_t bar_ = 0;
    std::array<RowNodes, kMaxRows> row_nodes_{};
    std::array<SectionNodes, kMaxSections> section_nodes_{};
    const ui::UiaAnimation* flash_animation_ = nullptr;
    uint32_t flash_node_ = 0;
};

}
