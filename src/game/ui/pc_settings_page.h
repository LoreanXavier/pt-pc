#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/ui/uia.h"
#include "engine/ui/uif.h"
#include "game/ui/dialog_view.h"
#include "game/ui/pc_settings.h"

namespace pt::game {

class Game;
class UifView;
struct MenuInput;

struct PcDropdownView {
    glm::vec2 lo{0.0f};
    glm::vec2 hi{0.0f};
    int cursor = 0;
    int active = 0;
    int scroll = 0;
    int visible = 8;
    float row_height = 34.0f;
    float flash = 1.0f;
    std::vector<std::string> items;
};

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
    uint16_t NavigationText() const { return navigation_text_; }
    uint16_t TabPrevText() const { return tab_prev_text_; }
    uint16_t TabNextText() const { return tab_next_text_; }
    uint16_t DonateText() const { return donate_text_; }
    uint16_t UpdateText() const { return footer_text_; }
    bool Tabbed() const { return source_ && source_->TabCount() > 0; }
    std::string_view ShownTab() const { return Tabbed() ? source_->TabLabel(source_->Tab()) : std::string_view(); }
    bool StripFocused() const { return Tabbed() && cursor_ < 0; }
    int Donations() const { return donations_; }
    int CursorSounds() const { return cursor_sounds_; }
    int ScrollFlags() const { const ScrollBar bar = Bar(); return (bar.scrollable ? 1 : 0) | (bar.above ? 2 : 0) | (bar.below ? 4 : 0); }
    void SetFlash(const ui::UiaAnimation* animation) { flash_animation_ = animation; }
    void Open(PcSettingsSource* source);
    void Refresh(bool preserve_focus = true);
    Result Update(Game& game, const MenuInput& input, float dt);
    void Step(bool active);
    void Apply(UifView& view, int language) const;
    void SetMeasure(const UifView& view, int language);
    void LogLayout(const UifView& view) const;
    int RowAt(glm::vec2 units) const;
    bool BackAt(glm::vec2 units) const;
    bool QuitAt(glm::vec2 units) const;
    bool IsLoopBrowser() const { return source_ && source_->IsLoopBrowser(); }
    bool Browser() const;
    PcGallery Gallery() const;
    void SetWallWidth(float canvas_px) { wall_width_ = canvas_px; }
    int WallVisible() const;
    int First() const { return first_; }
    int CellCount() const;
    const PcSettingRow* RowAtIndex(int index) const;
    PcPanel PanelOf(int index) const;
    bool Confirming() const { return confirming_; }
    bool IsQuestion() const;
    bool DialogView(dialog::View& view, int language) const;
    bool DropdownOpen() const { return dropdown_open_; }
    bool DropdownView(PcDropdownView& out) const;
    int DialogFocus() const { return dialog_nav_.Focus(); }
    float FlashFrame() const { return flash_frame_; }
    std::string PreviewFile() const { const auto* row = CurrentRow(); return row && source_ ? source_->PreviewFile(row->id) : std::string(); }
    PcPanel Panel() const { const auto* row = CurrentRow(); return row && source_ ? source_->Panel(row->id) : PcPanel{}; }
    bool FullScreen() const { return source_ && source_->FullScreen(); }
    bool ScreenShown() const { return source_ && source_->ScreenShown(); }
    bool SeeThrough() const { return source_ && source_->SeeThrough(); }
    PcScreen Screen() const { return source_ ? source_->Screen() : PcScreen{}; }
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
    bool ShowRowPage(std::string_view label);
    bool SetValue(std::string_view label, int value);
    bool Focus(std::string_view label);
    int ShownValue(const PcSettingRow& row) const { return row.id == preview_id_ ? preview_value_ : row.value; }
    std::string_view ShownNote(const PcSettingRow& row) const;
    bool ValueLess(const PcSettingRow& row) const;

private:
    struct Placed {
        int section = 0;
        int row = 0;
        glm::vec2 label{0.0f};
        glm::vec2 value{0.0f};
        int lines = 1;
        int column = 0;
        bool first = false;
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
    void LayoutAt(float gap, float between);
    void Scroll();
    int VisibleRows() const;
    int WindowRows() const;
    int GalleryHit(glm::vec2 units) const;
    bool GalleryMove(Game& game, uint32_t act);
    bool MusicControl(Game& game, int button, float value);
    glm::vec2 Shown(const Placed& p) const;
    bool InWindow(float shown_y, float bottom_y) const;
    float RowBottom(const Placed& p) const { return p.label.y - static_cast<float>(p.lines - 1) * line_; }
    float LabelX(int column) const;
    float ValueX(int column) const;
    float ArrowX() const { return wide_ ? kWideArrow : kArrow; }
    float Floor() const { return -17.4f + line_ * 0.5f; }
    float WindowBottom() const { return lowest_ < Floor() - 0.01f ? Floor() + 1.6f : Floor(); }
    float LabelEnd(const Placed& p, const PcSettingRow& row) const;
    float ValueRoom() const { return 2.0f * (ArrowX() - 1.5f); }
    void SourceChanged();
    void Move(Game& game, int delta);
    void PlayCursor(Game& game);
    void SwitchColumn(Game& game);
    void SwitchTab(Game& game, int visual);
    void ShowTab(Game& game, int tab);
    void LayoutTabs();
    int TabAt(glm::vec2 units) const;
    bool DonateShown() const;
    bool DonateAt(glm::vec2 units) const;
    struct ScrollBar {
        bool scrollable = false;
        bool above = false;
        bool below = false;
        glm::vec4 up{0.0f};
        glm::vec4 down{0.0f};
        glm::vec4 track{0.0f};
        glm::vec4 thumb{0.0f};
    };
    ScrollBar Bar() const;
    void ScrollBy(Game& game, float dy);
    void Donate(Game& game);
    void Change(Game& game, int delta, bool accept);
    Result UpdateDialog(Game& game, const MenuInput& input);
    Result UpdateDropdown(Game& game, const MenuInput& input, float dt);
    Result UpdateQuestion(Game& game, const MenuInput& input);
    Result UpdateScreen(Game& game, const MenuInput& input, float dt);

    bool UpdateDialogOpen() const { return confirming_ && update_dialog_ && footer_.has_value(); }
    bool UpdateAt(glm::vec2 units) const;
    void OpenUpdateDialog(Game& game);
    std::optional<PcSettingRow> footer_;
    uint16_t footer_text_ = 0;
    bool update_dialog_ = false;
    mutable bool update_hover_ = false;
    mutable glm::vec4 update_rect_{0.0f};
    mutable std::string update_latch_text_;
    mutable float update_latched_ = 0.0f;
    PcSettingsSource* source_ = nullptr;
    std::vector<PcSettingSection> sections_;
    std::vector<Placed> rows_;
    std::vector<glm::vec2> headers_;
    std::vector<int> header_rows_;
    float row_step_ = 3.0f;
    float line_ = 2.4f;
    bool wide_ = false;
    float lowest_ = 0.0f;
    float scroll_y_ = 0.0f;
    static constexpr int kMaxTabs = 10;
    std::vector<glm::vec2> tab_spans_;
    std::vector<int> tab_order_;
    float tab_scroll_ = 0.0f;
    float strip_y_ = 28.0f;
    float strip_left_ = -50.0f;
    float line_half_ = 16.0f;
    float line_centre_ = 16.0f;
    float open_time_ = 0.0f;
    int donations_ = 0;
    int cursor_sounds_ = 0;
    mutable glm::vec4 donate_rect_{0.0f};
    float donate_width_ = 24.0f;
    mutable bool donate_hover_ = false;
    float header_gap_ = 5.0f;
    const UifView* measure_view_ = nullptr;
    int measure_language_ = -1;
    mutable PcSettingRow found_row_;
    mutable std::string layout_logged_;
    static constexpr float kArrow = 12.0f;
    static constexpr float kWideArrow = 14.0f;
    int cursor_ = 0;
    int first_ = 0;
    bool confirming_ = false;
    bool dropdown_open_ = false;
    int dropdown_cursor_ = 0;
    int dropdown_scroll_ = 0;
    float dropdown_flash_ = 1000.0f;
    dialog::Nav dialog_nav_;
    mutable int language_ = 0;
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
    std::array<uint16_t, kMaxTabs> tab_nodes_{};
    uint16_t tab_rule_ = 0;
    uint16_t tab_bar_group_ = 0;
    uint16_t tab_bar_ = 0;
    float bar_half_ = 10.0f;
    uint16_t tab_prev_text_ = 0;
    uint16_t tab_next_text_ = 0;
    uint16_t donate_text_ = 0;
    uint16_t bar_group_ = 0;
    uint16_t bar_ = 0;
    uint16_t arrow_up_ = 0;
    uint16_t arrow_down_ = 0;
    uint16_t bar_track_ = 0;
    uint16_t bar_thumb_ = 0;
    mutable float donate_latched_ = 0.0f;
    mutable int donate_latch_language_ = -1;
    mutable std::string donate_prev_text_;
    mutable bool arrow_up_hover_ = false;
    mutable bool arrow_down_hover_ = false;
    std::array<RowNodes, kMaxRows> row_nodes_{};
    std::array<SectionNodes, kMaxSections> section_nodes_{};
    const ui::UiaAnimation* flash_animation_ = nullptr;
    uint32_t flash_node_ = 0;
};

}
