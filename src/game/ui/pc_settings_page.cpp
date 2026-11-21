#include "game/ui/pc_settings_page.h"

#include <algorithm>
#include <format>
#include <cstdlib>
#include <optional>
#include <tuple>

#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "game/game.h"
#include "game/ui/museum_layout.h"
#include "game/ui/options_menu.h"
#include "game/ui/uif_view.h"

namespace pt::game {
namespace {

constexpr uint16_t kRootNull = 1;
constexpr uint16_t kContent = 39;
constexpr uint16_t kHeaderText = 41;
constexpr uint16_t kTipText = 48;
constexpr uint16_t kHeaderLine = 74;
constexpr uint16_t kCameraBarGroup = 80;
constexpr uint16_t kCameraBar = 81;
constexpr uint16_t kBackIcon = 113;
constexpr uint16_t kBackText = 115;
constexpr uint16_t kBackGlow = 116;
constexpr uint16_t kEntryGroup = 120;
constexpr uint16_t kEntryText = 121;
constexpr uint16_t kSubtitleBarGroup = 136;
constexpr uint16_t kTitle = 150;

constexpr const char* kCursorSound = "Play_sys_cursor_01";
constexpr const char* kChangeSound = "Play_sys_change_01";

constexpr float kLabelX[2] = {-50.0f, 10.0f};
constexpr float kValueX[2] = {-9.0f, 51.0f};
constexpr float kWideValueX = 47.0f;
constexpr float kBrowserValueX = -13.0f;
constexpr float kCenterX = 7.5f;
constexpr float kTop = 23.0f;
constexpr float kFirstRow = 5.0f;
constexpr float kRowStep = 3.0f;
constexpr float kRowGap = 0.6f;
constexpr float kLabelFloor = 0.82f;
constexpr float kDefaultLine = 2.4f;
constexpr float kCompactRowStep = 2.0f;
constexpr glm::vec2 kLineOffset{16.0f, -1.0f};
constexpr glm::vec2 kBackIconAt{-52.0f, -31.0f};
constexpr glm::vec2 kBackTextAt{-50.0f, -31.0f};
constexpr glm::vec2 kQuitTextAt{-25.0f, -31.0f};
constexpr glm::vec2 kHelpAt{5.0f, -21.5f};
constexpr glm::vec2 kHelpSize{1100.0f, 60.0f};
constexpr int kBrowserRows = 18;
constexpr float kBrowserStep = 2.0f;
constexpr float kRepeatDelay = 0.4f;
constexpr float kTabEnd = 6.0f;
constexpr float kTabGap = 3.5f;
constexpr float kStripRight = 63.0f;
constexpr float kRepeatStep = 0.1f;

uint16_t Clone(ui::UifModel& model, uint16_t id, uint16_t parent, glm::vec2 at, std::string_view name) {
    ui::UifNode node = *model.FindById(id);
    node.parent = model.IndexOfId(parent);
    node.translate.x = at.x;
    node.translate.y = at.y;
    return model.AddNode(std::move(node), StrCode64(std::format("pc_settings_{}", name)));
}

void SetCell(ui::UifModel& model, uint16_t id, int texture, float u0, float u1) {
    ui::UifNode& node = *model.FindById(id);
    node.material.textures[ui::kUifBase] = texture;
    node.uvs = {{u0, 0.0f}, {u0, 1.0f}, {u1, 1.0f}, {u1, 0.0f}};
}

}

void PcSettingsPage::Build(ui::UifModel& model, int icon_atlas, int icon_glow) {
    root_ = Clone(model, kContent, kRootNull, glm::vec2(-kCenterX, 0.0f), "root");
    title_ = Clone(model, kTitle, root_, glm::vec2(model.FindById(kTitle)->translate), "title");
    for (int i = 0; i < kMaxSections; ++i) {
        section_nodes_[i].text = Clone(model, kHeaderText, root_, glm::vec2(0.0f), std::format("section_{}", i));
        section_nodes_[i].line = Clone(model, kHeaderLine, root_, glm::vec2(0.0f), std::format("line_{}", i));
    }
    for (int i = 0; i < kMaxRows; ++i) {
        row_nodes_[i].label = Clone(model, kHeaderText, root_, glm::vec2(0.0f), std::format("label_{}", i));
        row_nodes_[i].group = Clone(model, kEntryGroup, root_, glm::vec2(0.0f), std::format("group_{}", i));
        row_nodes_[i].value = Clone(model, kEntryText, row_nodes_[i].group, glm::vec2(0.0f), std::format("value_{}", i));
        row_nodes_[i].decrease = Clone(model, kEntryText, root_, glm::vec2(0.0f), std::format("decrease_{}", i));
        row_nodes_[i].increase = Clone(model, kEntryText, root_, glm::vec2(0.0f), std::format("increase_{}", i));
        model.FindById(row_nodes_[i].decrease)->size.x = 35.0f;
        model.FindById(row_nodes_[i].increase)->size.x = 35.0f;
    }
    bar_group_ = Clone(model, kCameraBarGroup, root_, glm::vec2(0.0f), "bar_group");
    bar_ = Clone(model, kCameraBar, bar_group_, glm::vec2(0.0f), "bar");
    for (const auto [node, name] : {std::pair{&arrow_up_, "arrow_up"}, std::pair{&arrow_down_, "arrow_down"}, std::pair{&bar_track_, "bar_track"},
                                    std::pair{&bar_thumb_, "bar_thumb"}}) {
        *node = Clone(model, kCameraBar, root_, glm::vec2(0.0f), name);
        ui::UifNode& shape = *model.FindById(*node);
        shape.size = glm::vec2(1.0f);
        shape.scale = glm::vec2(1.0f);
        shape.positions = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
        shape.uvs = shape.positions;
        shape.indices = {0, 1, 2, 0, 2, 3};
        shape.points.clear();
    }
    help_ = Clone(model, kTipText, root_, kHelpAt, "help");
    model.FindById(help_)->size = kHelpSize;
    back_icon_ = Clone(model, kBackIcon, root_, kBackIconAt, "back_icon");
    back_glow_ = Clone(model, kBackGlow, root_, kBackIconAt, "back_glow");
    back_text_ = Clone(model, kBackText, root_, kBackTextAt, "back_text");
    quit_text_ = Clone(model, kBackText, root_, kQuitTextAt, "quit_text");
    navigation_text_ = Clone(model, kBackText, root_, glm::vec2(2.0f, -31.0f), "navigation_text");
    strip_y_ = model.FindById(kTitle)->translate.y;
    strip_left_ = model.FindById(kTitle)->translate.x;
    for (int i = 0; i < kMaxTabs; ++i) tab_nodes_[i] = Clone(model, kHeaderText, root_, glm::vec2(0.0f), std::format("tab_{}", i));
    tab_rule_ = Clone(model, kHeaderLine, root_, glm::vec2(0.0f), "tab_rule");
    tab_bar_group_ = Clone(model, kCameraBarGroup, root_, glm::vec2(0.0f), "tab_bar_group");
    tab_bar_ = Clone(model, kCameraBar, tab_bar_group_, glm::vec2(0.0f), "tab_bar");
    if (const ui::UifNode* bar = model.FindById(kCameraBar); bar && !bar->positions.empty()) {
        float lo = bar->positions.front().x, hi = lo;
        for (const glm::vec2& v : bar->positions) {
            lo = std::min(lo, v.x);
            hi = std::max(hi, v.x);
        }
        bar_half_ = std::max(0.5f, (hi - lo) * bar->size.x * 0.5f);
    }
    if (const ui::UifNode* line = model.FindById(kHeaderLine); line && !line->positions.empty()) {
        float lo = line->positions.front().x, hi = lo;
        for (const glm::vec2& v : line->positions) {
            lo = std::min(lo, v.x);
            hi = std::max(hi, v.x);
        }
        line_half_ = std::max(0.5f, (hi - lo) * line->size.x * 0.5f);
        line_centre_ = (hi + lo) * 0.5f * line->size.x;
    }
    tab_prev_text_ = Clone(model, kBackText, root_, glm::vec2(0.0f), "tab_prev");
    tab_next_text_ = Clone(model, kBackText, root_, glm::vec2(0.0f), "tab_next");
    donate_text_ = Clone(model, kBackText, root_, glm::vec2(0.0f), "donate");
    footer_text_ = Clone(model, kBackText, root_, glm::vec2(0.0f), "update_button");
    if (icon_atlas >= 0 && icon_glow >= 0) {
        SetCell(model, back_icon_, icon_atlas, 0.5f, 1.0f);
        SetCell(model, back_glow_, icon_glow, 0.5f, 1.0f);
    }
    flash_node_ = static_cast<uint32_t>(model.Names()[kSubtitleBarGroup]);
}

void PcSettingsPage::Open(PcSettingsSource* source) {
    source_ = source;
    if (source_) {
        source_->Opened();
    }
    cursor_ = 0;
    scroll_y_ = 0.0f;
    tab_scroll_ = 0.0f;
    open_time_ = 0.0f;
    confirming_ = false;
    dropdown_open_ = false;
    flash_frame_ = 0.0f;
    held_dirs_ = 0;
    last_pointer_ = glm::vec2(-1.0f);
    Refresh(false);
}

void PcSettingsPage::Refresh(bool preserve_focus) {
    const PcSettingRow* previous = preserve_focus ? CurrentRow() : nullptr;
    const std::optional<int> focused = previous ? std::optional(previous->id) : std::nullopt;
    const bool back = preserve_focus && cursor_ == static_cast<int>(rows_.size());
    sections_ = source_ ? source_->Sections() : std::vector<PcSettingSection>{};
    footer_ = source_ ? source_->FooterAction() : std::nullopt;
    Layout();
    LayoutTabs();
    bool found = false;
    if (focused) {
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            if (RowOf(rows_[i]).id == *focused) {
                cursor_ = i;
                found = true;
                break;
            }
        }
    } else if (back) {
        cursor_ = static_cast<int>(rows_.size());
    }
    if (update_dialog_) found = footer_.has_value();
    cursor_ = std::clamp(cursor_, Tabbed() ? -1 : 0, static_cast<int>(rows_.size()));
    const PcSettingRow* current = CurrentRow();
    if (confirming_ && (!found || !current || !current->action || !current->enabled)) confirming_ = false;
    if (dropdown_open_ && (!found || !current || !current->dropdown || !current->enabled)) dropdown_open_ = false;
    if (!confirming_) update_dialog_ = false;
    if (preserve_focus && focused && !found) preview_id_ = -1;
    Scroll();
}

bool PcSettingsPage::Browser() const {
    return source_ && source_->IsBrowser();
}

PcGallery PcSettingsPage::Gallery() const {
    return source_ ? source_->Gallery() : PcGallery::None;
}

int PcSettingsPage::WallVisible() const {
    return MuseumLayout::WallVisible(wall_width_);
}

int PcSettingsPage::CellCount() const {
    int cells = 0;
    while (cells < static_cast<int>(rows_.size()) && RowOf(rows_[cells]).link) ++cells;
    return cells;
}

const PcSettingRow* PcSettingsPage::RowAtIndex(int index) const {
    return index >= 0 && index < static_cast<int>(rows_.size()) ? &RowOf(rows_[index]) : nullptr;
}

PcPanel PcSettingsPage::PanelOf(int index) const {
    const PcSettingRow* row = RowAtIndex(index);
    return row && source_ ? source_->Panel(row->id) : PcPanel{};
}

int PcSettingsPage::WindowRows() const {
    if (Gallery() == PcGallery::Wall) return WallVisible();
    if (Gallery() == PcGallery::Halls || !Browser()) return 1000;
    return kBrowserRows;
}

void PcSettingsPage::Scroll() {
    const int n = static_cast<int>(rows_.size());
    if (!Browser() && Gallery() == PcGallery::None) {
        first_ = 0;
        const float most = std::max(0.0f, WindowBottom() - lowest_);
        if (lowest_ >= Floor() - 0.01f) {
            scroll_y_ = 0.0f;
            return;
        }
        if (cursor_ < 0) scroll_y_ = 0.0f;
        if (cursor_ >= 0 && cursor_ < n) {
            const Placed& p = rows_[cursor_];
            const float top = p.first ? p.label.y + header_gap_ : p.label.y + row_step_ * 0.5f;
            if (top + scroll_y_ > kTop) scroll_y_ = kTop - top;
            if (RowBottom(p) + scroll_y_ < WindowBottom()) scroll_y_ = WindowBottom() - RowBottom(p);
        }
        scroll_y_ = std::clamp(scroll_y_, 0.0f, most);
        return;
    }
    const int window = WindowRows();
    if (n <= window) {
        first_ = 0;
        return;
    }
    if (cursor_ < n) {
        if (cursor_ < first_) first_ = cursor_;
        if (cursor_ >= first_ + window) first_ = cursor_ - window + 1;
    }
    first_ = std::clamp(first_, 0, n - window);
}

int PcSettingsPage::VisibleRows() const {
    return std::min(static_cast<int>(rows_.size()) - first_, WindowRows());
}

int PcSettingsPage::GalleryHit(glm::vec2 units) const {
    const glm::vec2 px(MuseumLayout::kCanvasWidth * 0.5f + units.x * UiCanvas::kUnit, MuseumLayout::kCanvasHeight * 0.5f - units.y * UiCanvas::kUnit);
    const int n = static_cast<int>(rows_.size());
    const int cells = CellCount();
    if (Gallery() == PcGallery::Halls) {
        for (int i = 0; i < n; ++i) {
            const MuseumLayout::Cell cell = i < cells ? MuseumLayout::HallCell(i, cells) : MuseumLayout::HallText(i - cells);
            if (cell.Contains(px)) return i;
        }
        return -1;
    }
    const int visible = VisibleRows();
    const int slots = std::min(n, WallVisible());
    for (int slot = 0; slot < visible; ++slot) {
        if (MuseumLayout::WallCell(slot, slots).Contains(px)) return first_ + slot;
    }
    if (MuseumLayout::kSpotlight.Contains(px) && cursor_ < n) return cursor_;
    return -1;
}

bool PcSettingsPage::GalleryMove(Game& game, uint32_t act) {
    const int n = static_cast<int>(rows_.size());
    const int cells = CellCount();
    if (cells == 0) return false;
    const bool halls = Gallery() == PcGallery::Halls;
    const int columns = halls ? MuseumLayout::HallColumns(cells) : cells;
    int target = cursor_;
    if (cursor_ < cells) {
        const bool last_row = cursor_ / columns == (cells - 1) / columns;
        if (act & kRawLeft) target = cursor_ > 0 ? cursor_ - 1 : halls ? cursor_ : cells - 1;
        else if (act & kRawRight) target = cursor_ + 1 < cells ? cursor_ + 1 : halls ? cursor_ : 0;
        else if (act & kRawUp) target = cursor_ - columns >= 0 ? cursor_ - columns : cursor_;
        else if (act & kRawDown) target = last_row ? cells : std::min(cursor_ + columns, cells - 1);
        else return false;
    } else {
        if (act & kRawUp) target = cursor_ - 1;
        else if (act & kRawDown) target = std::min(cursor_ + 1, n);
        else return false;
    }
    if (target != cursor_) Move(game, target - cursor_);
    return true;
}

glm::vec2 PcSettingsPage::Shown(const Placed& p) const {
    if (Browser()) return first_ > 0 ? p.label + glm::vec2(0.0f, kBrowserStep * static_cast<float>(first_)) : p.label;
    return p.label + glm::vec2(0.0f, scroll_y_);
}

bool PcSettingsPage::InWindow(float shown_y, float bottom_y) const {
    if (Browser() || (scroll_y_ <= 0.0f && lowest_ >= Floor() - 0.01f)) return true;
    return shown_y <= kTop + 0.01f && bottom_y >= WindowBottom() - 0.01f;
}

void PcSettingsPage::SourceChanged() {
    cursor_ = 0;
    first_ = 0;
    scroll_y_ = 0.0f;
    confirming_ = false;
    dropdown_open_ = false;
    flash_frame_ = 0.0f;
    Refresh(false);
    if (source_) {
        if (const int c = source_->TakeCursor(); c >= 0) {
            cursor_ = std::clamp(c, 0, static_cast<int>(rows_.size()));
            Scroll();
        }
    }
}

float PcSettingsPage::LabelX(int column) const {
    if (Browser()) return column == 0 ? -50.0f : 15.0f;
    return kLabelX[std::clamp(column, 0, 1)];
}

float PcSettingsPage::ValueX(int column) const {
    if (Browser()) return column == 0 ? kBrowserValueX : kValueX[1];
    return wide_ ? kWideValueX : kValueX[std::clamp(column, 0, 1)];
}

float PcSettingsPage::LabelEnd(const Placed& p, const PcSettingRow& row) const {
    if (Browser()) return p.value.x - 3.0f;
    return ValueLess(row) ? p.value.x + ArrowX() : p.value.x - ArrowX() - 2.0f;
}

void PcSettingsPage::SetMeasure(const UifView& view, int language) {
    if (measure_view_ == &view && measure_language_ == language) return;
    measure_view_ = &view;
    measure_language_ = language;
    Layout();
    LayoutTabs();
    Scroll();
}

void PcSettingsPage::Layout() {
    rows_.clear();
    headers_.clear();
    const bool browser = Browser();
    const bool compact = source_ && source_->CompactRows();
    wide_ = !browser && std::none_of(sections_.begin(), sections_.end(), [](const PcSettingSection& s) { return s.column > 0; });
    const int language = std::max(0, measure_language_);
    float measured = 0.0f;
    float ink_top = 1e9f;
    float ink_bottom = -1e9f;
    if (measure_view_ && !browser && !compact) {
        auto ink = [&](uint16_t node, std::string_view text) {
            if (const auto m = measure_view_->MeasureText(node, text, language, UiFontType::PcSystem)) {
                measured = std::max(measured, m->y / UiCanvas::kUnit);
                if (m->z <= m->w) {
                    ink_top = std::min(ink_top, m->z / UiCanvas::kUnit);
                    ink_bottom = std::max(ink_bottom, m->w / UiCanvas::kUnit);
                }
            }
        };
        ink(row_nodes_[0].label, "Hg");
        for (const PcSettingSection& section : sections_) {
            for (const PcSettingRow& row : section.rows) {
                ink(row_nodes_[0].label, PcText(row.label, language));
                for (const std::string& value : row.values) ink(row_nodes_[0].value, PcText(value, language));
            }
        }
    }
    line_ = measured > 0.0f ? measured : kDefaultLine;
    const float tall = ink_top <= ink_bottom ? ink_bottom - ink_top : line_;
    row_step_ = browser ? kBrowserStep : compact ? kCompactRowStep : std::max(kRowStep, tall + kRowGap);
    for (const bool comfortable : {true, false}) {
        const float base = sections_.size() >= 5 || !comfortable ? 3.0f : kFirstRow;
        const float gap = std::max(base, (comfortable ? std::max(line_, tall) + 1.6f : tall + 1.0f));
        const float between = comfortable ? std::max(gap, row_step_ + 1.6f) : gap;
        LayoutAt(gap, between);
        header_gap_ = gap;
        if (browser || compact || lowest_ >= Floor() - 0.01f) break;
    }
}

void PcSettingsPage::LayoutAt(float gap, float between) {
    rows_.clear();
    headers_.clear();
    header_rows_.clear();
    const bool browser = Browser();
    const bool compact = source_ && source_->CompactRows();
    const int language = std::max(0, measure_language_);
    float y[2] = {kTop, kTop};
    float lowest = kTop;
    for (int column = 0; column < 2; ++column) {
        for (size_t s = 0; s < sections_.size() && static_cast<int>(headers_.size()) < kMaxSections; ++s) {
            if (std::clamp(sections_[s].column, 0, 1) != column) {
                continue;
            }
            headers_.emplace_back(LabelX(column), y[column]);
            header_rows_.push_back(sections_[s].rows.empty() ? -1 : static_cast<int>(rows_.size()));
            float row_y = y[column] - gap;
            for (size_t r = 0; r < sections_[s].rows.size() && (browser || static_cast<int>(rows_.size()) < kMaxRows); ++r) {
                const PcSettingRow& row = sections_[s].rows[r];
                Placed placed{static_cast<int>(s), static_cast<int>(r), {LabelX(column), row_y}, {ValueX(column), row_y}};
                placed.column = column;
                placed.first = r == 0;
                placed.lines = browser ? 1 : std::max(1, row.label_lines);
                if (!browser && !compact && measure_view_ && placed.lines == 1) {
                    const std::string text(PcText(row.label, language));
                    if (const auto m = measure_view_->MeasureText(row_nodes_[0].label, text, language, UiFontType::PcSystem)) {
                        const float room = LabelEnd(placed, row) - placed.label.x;
                        if (m->x / UiCanvas::kUnit * kLabelFloor > room) placed.lines = 2;
                    }
                }
                rows_.push_back(placed);
                const float extra = static_cast<float>(placed.lines - 1) * line_;
                if (!browser) lowest = std::min(lowest, row_y - extra);
                row_y -= (browser ? kBrowserStep : row_step_) + extra;
            }
            y[column] = row_y + (browser ? kBrowserStep : row_step_) - between;
        }
    }
    lowest_ = lowest;
}

std::string_view PcSettingsPage::ShownNote(const PcSettingRow& row) const {
    const int shown = ShownValue(row);
    if (row.ValueDisabled(shown)) return row.value_notes[shown];
    return !row.ValueWarning(shown).empty() ? row.ValueWarning(shown) : std::string_view(row.note);
}

const PcSettingRow* PcSettingsPage::CurrentRow() const {
    if (UpdateDialogOpen()) return &*footer_;
    return cursor_ >= 0 && cursor_ < static_cast<int>(rows_.size()) ? &RowOf(rows_[cursor_]) : nullptr;
}

const PcSettingRow* PcSettingsPage::FindRow(std::string_view label) const {
    if (footer_ && footer_->label == label) return &*footer_;
    for (const PcSettingSection& section : sections_) {
        for (const PcSettingRow& row : section.rows) {
            if (row.label == label) {
                return &row;
            }
        }
    }
    if (source_ && source_->FindSettingRow(label, found_row_)) return &found_row_;
    return nullptr;
}

bool PcSettingsPage::ShowRowPage(std::string_view label) {
    if (footer_ && footer_->label == label) return true;
    for (const PcSettingSection& section : sections_) {
        for (const PcSettingRow& row : section.rows) {
            if (row.label == label) return true;
        }
    }
    if (!source_ || !source_->ShowSettingPage(label)) return false;
    SourceChanged();
    return true;
}

bool PcSettingsPage::SetValue(std::string_view label, int value) {
    ShowRowPage(label);
    const PcSettingRow* row = FindRow(label);
    if (!row || !source_) {
        return false;
    }
    source_->Set(row->id, value);
    Refresh();
    return true;
}

bool PcSettingsPage::Focus(std::string_view label) {
    ShowRowPage(label);
    if (footer_ && footer_->label == label) return true;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        if (RowOf(rows_[i]).label == label) {
            cursor_ = i;
            confirming_ = false;
            Scroll();
            return true;
        }
    }
    return false;
}

int PcSettingsPage::RowAt(glm::vec2 units) const {
    if (Gallery() != PcGallery::None) return GalleryHit(units);
    units.x += kCenterX;
    const int visible = VisibleRows();
    for (int i = first_; i < first_ + visible; ++i) {
        const Placed& p = rows_[i];
        const glm::vec2 at = Shown(p);
        const float half = Browser() ? 1.0f : row_step_ * 0.5f;
        const float bottom = at.y - static_cast<float>(p.lines - 1) * line_;
        if (!InWindow(at.y, bottom)) continue;
        if (units.x >= at.x - 1.0f && units.x <= p.value.x + ArrowX() + 1.0f && units.y <= at.y + half && units.y >= bottom - half) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool PcSettingsPage::UpdateAt(glm::vec2 units) const {
    if (!footer_ || !Tabbed() || update_rect_.z <= update_rect_.x) return false;
    const glm::vec2 px(640.0f + units.x * UiCanvas::kUnit, 360.0f - units.y * UiCanvas::kUnit);
    return px.x >= update_rect_.x - 4.0f && px.x <= update_rect_.z + 4.0f && px.y >= update_rect_.y - 6.0f && px.y <= update_rect_.w + 6.0f;
}

void PcSettingsPage::OpenUpdateDialog(Game& game) {
    if (!footer_ || !footer_->enabled) return;
    confirming_ = true;
    update_dialog_ = true;
    dialog_nav_.Open(footer_->confirm_default_yes ? 0 : 1, held_dirs_);
    flash_frame_ = 0.0f;
    if (game.Audio()) PlayCursor(game);
}

bool PcSettingsPage::BackAt(glm::vec2 units) const {
    units.x += kCenterX;
    return units.x >= kBackIconAt.x - 3.0f && units.x <= kBackTextAt.x + 10.0f && std::abs(units.y - kBackTextAt.y) <= 2.0f;
}

bool PcSettingsPage::QuitAt(glm::vec2 units) const {
    units.x += kCenterX;
    return units.x >= kQuitTextAt.x - 2.0f && units.x <= kQuitTextAt.x + 18.0f && std::abs(units.y - kQuitTextAt.y) <= 2.0f;
}

void PcSettingsPage::Move(Game& game, int delta) {
    const int strip = Tabbed() ? 1 : 0;
    const int n = static_cast<int>(rows_.size()) + 1 + strip;
    const bool music_on = Gallery() == PcGallery::Wall && PanelOf(cursor_).music && PanelOf(cursor_).music_playing;
    cursor_ = ((cursor_ + strip + delta) % n + n) % n - strip;
    confirming_ = false;
    preview_id_ = -1;
    flash_frame_ = 0.0f;
    Scroll();
    if (source_) source_->CursorMoved();
    if (music_on && source_) {
        const PcSettingRow* row = CurrentRow();
        const PcPanel panel = PanelOf(cursor_);
        if (row && row->enabled && panel.music && !panel.locked && !panel.music_playing) source_->Music(row->id, PcMusicCommand::Restart, 0.0f);
    }
    if (game.Audio()) {
        PlayCursor(game);
    }
}

void PcSettingsPage::Change(Game& game, int delta, bool accept) {
    const PcSettingRow* row = CurrentRow();
    if (!row || !row->enabled || !source_) {
        return;
    }
    if (row->link) {
        if (!accept) {
            return;
        }
        const int keep = cursor_;
        source_->Activate(row->id);
        SourceChanged();
        if (source_->KeepsCursor()) {
            cursor_ = std::clamp(keep, 0, static_cast<int>(rows_.size()));
            Scroll();
        }
        if (game.Audio()) {
            game.Audio()->PostEvent(kChangeSound, nullptr);
        }
        return;
    }
    if (row->action) {
        if (!accept) {
            const int n = static_cast<int>(row->values.size());
            if (n > 1) {
                const int value = std::clamp(row->value + delta, 0, n - 1);
                if (value != row->value) {
                    source_->Set(row->id, value);
                    Refresh();
                    if (game.Audio()) {
                        PlayCursor(game);
                    }
                }
            }
            return;
        }
        confirming_ = true;
        update_dialog_ = false;
        dialog_nav_.Open(row->confirm_default_yes ? 0 : 1, held_dirs_);
        flash_frame_ = 0.0f;
        if (game.Audio()) {
            PlayCursor(game);
        }
        return;
    }
    const int n = static_cast<int>(row->values.size());
    if (n <= 1) {
        return;
    }
    if (row->dropdown) {
        dropdown_open_ = true;
        dropdown_cursor_ = std::clamp(row->value, 0, n - 1);
        constexpr int kVisible = 8;
        const int max_scroll = std::max(0, n - kVisible);
        dropdown_scroll_ = std::clamp(dropdown_cursor_ - kVisible / 2, 0, max_scroll);
        dropdown_flash_ = 0.0f;
        repeat_time_ = -kRepeatDelay;
        if (game.Audio()) PlayCursor(game);
        return;
    }
    const int from = ShownValue(*row);
    int value = from + delta;
    value = row->wrap ? (value % n + n) % n : std::clamp(value, 0, n - 1);
    if (value == from) {
        return;
    }
    if (row->ValueDisabled(value) && value != row->value) {
        preview_id_ = row->id;
        preview_value_ = value;
    } else {
        preview_id_ = -1;
        if (value != row->value) {
            source_->Set(row->id, value);
            Refresh();
        }
    }
    if (game.Audio()) {
        PlayCursor(game);
    }
}

void PcSettingsPage::SwitchColumn(Game& game) {
    if (!CurrentRow()) return;
    const Placed& from = rows_[cursor_];
    int nearest = -1;
    float distance = 1000.0f;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].label.x == from.label.x) continue;
        const float dy = std::abs(rows_[i].label.y - from.label.y);
        if (dy < distance) {
            distance = dy;
            nearest = static_cast<int>(i);
        }
    }
    if (nearest >= 0) Move(game, nearest - cursor_);
}

bool PcSettingsPage::MusicControl(Game& game, int button, float value) {
    const PcSettingRow* row = CurrentRow();
    if (!row || !source_ || Gallery() != PcGallery::Wall) return false;
    const PcPanel panel = PanelOf(cursor_);
    if (!panel.music || panel.locked || !row->enabled) return false;
    using L = MuseumLayout;
    const int cells = CellCount();
    switch (button) {
    case L::kMusicPrevious:
        if (cursor_ > 0) Move(game, -1);
        return true;
    case L::kMusicNext:
        if (cursor_ + 1 < cells) Move(game, 1);
        return true;
    case L::kMusicBack: source_->Music(row->id, PcMusicCommand::SeekBy, -10.0f); break;
    case L::kMusicForward: source_->Music(row->id, PcMusicCommand::SeekBy, 10.0f); break;
    case L::kMusicPlay: source_->Music(row->id, PcMusicCommand::PlayPause, 0.0f); break;
    case L::kMusicRestart: source_->Music(row->id, PcMusicCommand::Restart, 0.0f); break;
    case L::kMusicLoop: source_->Music(row->id, PcMusicCommand::Loop, 0.0f); break;
    case L::kMusicButtons: source_->Music(row->id, PcMusicCommand::SeekTo, value); break;
    default: return false;
    }
    if (game.Audio()) PlayCursor(game);
    return true;
}

void PcSettingsPage::PlayCursor(Game& game) {
    ++cursor_sounds_;
    if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
}

PcSettingsPage::Result PcSettingsPage::Update(Game& game, const MenuInput& input, float dt) {
    if (source_ && source_->ScreenShown()) return UpdateScreen(game, input, dt);
    if (source_ && source_->FullScreen()) {
        const uint32_t pressed = input.held_dirs & ~held_dirs_;
        held_dirs_ = input.held_dirs;
        const int n = static_cast<int>(rows_.size());
        const int step = (pressed & kRawLeft) ? -1 : (pressed & kRawRight) ? 1 : 0;
        if (step != 0 && n > 0) {
            cursor_ = ((std::min(cursor_, n - 1) + step) % n + n) % n;
            flash_frame_ = 0.0f;
            Scroll();
            source_->CursorMoved();
            if (game.Audio()) PlayCursor(game);
        } else if (input.back || input.close || input.accept || input.click || input.right_click) {
            const int keep = cursor_;
            source_->Back();
            Refresh();
            cursor_ = std::clamp(keep, 0, static_cast<int>(rows_.size()));
            Scroll();
            if (game.Audio()) PlayCursor(game);
        }
        return Result::None;
    }
    if (IsQuestion()) return UpdateQuestion(game, input);
    if (confirming_) return UpdateDialog(game, input);
    if (dropdown_open_) return UpdateDropdown(game, input, dt);
    open_time_ += dt;
    if (input.donate && open_time_ > 0.5f && DonateShown()) {
        Donate(game);
        return Result::None;
    }
    if (input.update && Tabbed() && footer_) {
        OpenUpdateDialog(game);
        return Result::None;
    }
    if (Tabbed() && (input.tab_prev || input.tab_next)) {
        held_dirs_ = input.held_dirs;
        SwitchTab(game, input.tab_prev ? -1 : 1);
        return Result::None;
    }
    if (const ScrollBar bar = Bar(); bar.scrollable) {
        const auto inside = [&](glm::vec4 box, float grow) {
            const float x = input.pointer.x + kCenterX;
            return input.pointer_valid && x >= box.x - grow && x <= box.z + grow && input.pointer.y >= box.y - grow && input.pointer.y <= box.w + grow;
        };
        arrow_up_hover_ = bar.above && inside(bar.up, 0.8f);
        arrow_down_hover_ = bar.below && inside(bar.down, 0.8f);
        const float page = std::max(row_step_, kTop - WindowBottom() - row_step_);
        float dy = 0.0f;
        if (input.wheel != 0.0f) dy = -input.wheel * row_step_;
        else if (input.click && arrow_up_hover_) dy = -row_step_ * 2.0f;
        else if (input.click && arrow_down_hover_) dy = row_step_ * 2.0f;
        else if (input.click && inside(bar.track, 0.8f)) dy = (input.pointer.y > (bar.thumb.y + bar.thumb.w) * 0.5f) ? -page : page;
        if (dy != 0.0f) {
            ScrollBy(game, dy);
            last_pointer_ = input.pointer;
            return Result::None;
        }
    } else {
        arrow_up_hover_ = arrow_down_hover_ = false;
    }
    if (input.pointer_valid && DonateShown() && !Tabbed()) {
        donate_hover_ = DonateAt(input.pointer);
        if (input.click && donate_hover_) {
            Donate(game);
            last_pointer_ = input.pointer;
            return Result::None;
        }
    }
    if (input.pointer_valid && Tabbed()) {
        donate_hover_ = DonateAt(input.pointer);
        if (input.click) {
            if (donate_hover_) {
                Donate(game);
                last_pointer_ = input.pointer;
                return Result::None;
            }
            const int tab = TabAt(input.pointer);
            if (tab >= 0 || tab == -2 || tab == -3) {
                if (tab >= 0) ShowTab(game, tab);
                else SwitchTab(game, tab == -2 ? -1 : 1);
                last_pointer_ = input.pointer;
                return Result::None;
            }
        }
    }
    update_hover_ = input.pointer_valid && UpdateAt(input.pointer);
    if (update_hover_ && input.click) {
        last_pointer_ = input.pointer;
        OpenUpdateDialog(game);
        return Result::None;
    }
    if (input.pointer_valid) {
        if (Gallery() == PcGallery::Wall && input.click && !rows_.empty()) {
            const int slots = std::min(static_cast<int>(rows_.size()), WallVisible());
            const glm::vec2 px(MuseumLayout::kCanvasWidth * 0.5f + input.pointer.x * UiCanvas::kUnit,
                               MuseumLayout::kCanvasHeight * 0.5f - input.pointer.y * UiCanvas::kUnit);
            if (cursor_ < static_cast<int>(rows_.size()) && PanelOf(cursor_).music && MuseumLayout::kMusicSheet.Contains(px)) {
                for (int b = 0; b < MuseumLayout::kMusicButtons; ++b) {
                    if (MuseumLayout::MusicButton(b).Contains(px)) MusicControl(game, b, 0.0f);
                }
                const MuseumLayout::Cell bar = MuseumLayout::kMusicBar;
                if (bar.Contains(px)) MusicControl(game, MuseumLayout::kMusicButtons, (px.x - bar.lo.x) / (bar.hi.x - bar.lo.x));
                last_pointer_ = input.pointer;
                return Result::None;
            }
            for (const int delta : {-1, 1}) {
                if (MuseumLayout::WallArrow(delta > 0, slots).Contains(px)) {
                    const int target = std::clamp(cursor_ + delta, 0, static_cast<int>(rows_.size()) - 1);
                    if (target != cursor_) Move(game, target - cursor_);
                    last_pointer_ = input.pointer;
                    return Result::None;
                }
            }
        }
        const int hovered = RowAt(input.pointer);
        if (input.pointer != last_pointer_ && hovered >= 0 && hovered != cursor_) {
            cursor_ = hovered;
            confirming_ = false;
            preview_id_ = -1;
            flash_frame_ = 0.0f;
            if (source_) source_->CursorMoved();
            if (game.Audio()) {
                PlayCursor(game);
            }
        }
        last_pointer_ = input.pointer;
        if (input.click || input.right_click) {
            if (hovered >= 0) {
                cursor_ = hovered;
                const Placed& placed = rows_[cursor_];
                const PcSettingRow& row = RowOf(placed);
                const bool decrement_arrow = !row.link && (!row.action || row.values.size() > 1) &&
                    std::abs(input.pointer.x + kCenterX - (placed.value.x - ArrowX())) <= 2.0f;
                const bool decrease = input.right_click || decrement_arrow;
                const bool increment_arrow = row.action && row.values.size() > 1 &&
                    std::abs(input.pointer.x + kCenterX - (placed.value.x + ArrowX())) <= 2.0f;
                Change(game, decrease ? -1 : 1, !decrease && !increment_arrow);
            } else if (input.click && BackAt(input.pointer)) {
                if (source_ && source_->Back()) {
                    SourceChanged();
                    if (game.Audio()) {
                        PlayCursor(game);
                    }
                    return Result::None;
                }
                return Result::Back;
            }
        }
    }
    if (input.back || input.close) {
        if (source_ && source_->Back()) {
            SourceChanged();
            if (game.Audio()) {
                PlayCursor(game);
            }
            return Result::None;
        }
        return Result::Back;
    }
    if (input.music && cursor_ < static_cast<int>(rows_.size())) {
        using L = MuseumLayout;
        if (input.music & kMenuMusicBack) MusicControl(game, L::kMusicBack, 0.0f);
        if (input.music & kMenuMusicForward) MusicControl(game, L::kMusicForward, 0.0f);
        if (input.music & kMenuMusicRestart) MusicControl(game, L::kMusicRestart, 0.0f);
        if (input.music & kMenuMusicLoop) MusicControl(game, L::kMusicLoop, 0.0f);
    }
    const uint32_t pressed = input.held_dirs & ~held_dirs_;
    uint32_t act = pressed;
    if (pressed) {
        repeat_time_ = -kRepeatDelay;
    } else if (input.held_dirs) {
        repeat_time_ += dt;
        if (repeat_time_ >= kRepeatStep) {
            repeat_time_ -= kRepeatStep;
            act = input.held_dirs;
        }
    }
    held_dirs_ = input.held_dirs;
    if (input.switch_column && !Tabbed()) SwitchColumn(game);
    if (act && Gallery() != PcGallery::None && GalleryMove(game, act)) {
        act = 0;
    }
    if (act & kRawUp) {
        Move(game, -1);
    } else if (act & kRawDown) {
        Move(game, 1);
    } else if (act & kRawLeft) {
        if (cursor_ < 0) SwitchTab(game, -1);
        else if (cursor_ >= static_cast<int>(rows_.size())) Move(game, -1);
        else Change(game, -1, false);
    } else if (act & kRawRight) {
        if (cursor_ < 0) SwitchTab(game, 1);
        else if (cursor_ >= static_cast<int>(rows_.size())) Move(game, 1);
        else Change(game, 1, false);
    }
    if (input.accept) {
        if (cursor_ < 0) {
            Move(game, 1);
        } else if (cursor_ == static_cast<int>(rows_.size())) {
            if (source_ && source_->Back()) {
                SourceChanged();
            } else return Result::Back;
        } else Change(game, 1, true);
    }
    return Result::None;
}

PcSettingsPage::Result PcSettingsPage::UpdateDialog(Game& game, const MenuInput& input) {
    const PcSettingRow* row = CurrentRow();
    if (!row || !row->action || !row->enabled || !source_) {
        confirming_ = false;
        return Result::None;
    }
    held_dirs_ = input.held_dirs;
    const dialog::Result result = dialog_nav_.Update(input, 2, language_, true);
    if (dialog_nav_.Moved()) {
        flash_frame_ = 0.0f;
        if (game.Audio()) PlayCursor(game);
    }
    if (result == dialog::Result::None) return Result::None;
    const int id = row->id;
    const bool yes = result == dialog::Result::Chosen && dialog_nav_.Chosen() == 0;
    confirming_ = false;
    dropdown_open_ = false;
    flash_frame_ = 0.0f;
    if (yes) {
        source_->Activate(id);
        Refresh();
    }
    if (game.Audio()) game.Audio()->PostEvent(yes ? kChangeSound : kCursorSound, nullptr);
    return Result::None;
}

bool PcSettingsPage::DropdownView(PcDropdownView& out) const {
    if (!dropdown_open_ || cursor_ < 0 || cursor_ >= static_cast<int>(rows_.size())) return false;
    const Placed& placed = rows_[static_cast<size_t>(cursor_)];
    const PcSettingRow& row = RowOf(placed);
    if (!row.dropdown || !row.enabled || row.values.size() <= 1) return false;

    const int n = static_cast<int>(row.values.size());
    constexpr int kMaxVisible = 8;
    const int vis = std::min(n, kMaxVisible);
    constexpr float kRowH = 34.0f;
    constexpr float kPadY = 8.0f;
    constexpr float kFooterH = 30.0f;
    constexpr float kWidth = 312.0f;

    const glm::vec2 shown_lbl = Shown(placed);
    const float val_cx = 640.0f + UiCanvas::kUnit * (placed.value.x - kCenterX);
    const float val_cy = 360.0f - UiCanvas::kUnit * shown_lbl.y;
    const float total_h = static_cast<float>(vis) * kRowH + 2.0f * kPadY + kFooterH;

    float x0 = std::clamp(val_cx - kWidth * 0.5f, 40.0f, UiCanvas::kWidth - kWidth - 40.0f);
    float y0 = val_cy + 18.0f;
    if (y0 + total_h > 642.0f) {
        y0 = std::max(84.0f, 642.0f - total_h);
    }

    out = PcDropdownView{};
    out.lo = {x0, y0};
    out.hi = {x0 + kWidth, y0 + total_h};
    out.cursor = std::clamp(dropdown_cursor_, 0, n - 1);
    out.active = std::clamp(row.value, 0, n - 1);
    const int max_scroll = std::max(0, n - vis);
    out.scroll = std::clamp(dropdown_scroll_, 0, max_scroll);
    out.visible = vis;
    out.row_height = kRowH;
    out.flash = std::clamp(dropdown_flash_, 0.0f, 1000.0f);
    out.items = row.values;
    return true;
}

PcSettingsPage::Result PcSettingsPage::UpdateDropdown(Game& game, const MenuInput& input, float dt) {
    PcDropdownView dd;
    if (!DropdownView(dd) || !source_) {
        dropdown_open_ = false;
        return Result::None;
    }
    const PcSettingRow* row = CurrentRow();
    if (!row) {
        dropdown_open_ = false;
        return Result::None;
    }
    dropdown_flash_ += dt * 60.0f;
    const int n = static_cast<int>(dd.items.size());
    const int max_scroll = std::max(0, n - dd.visible);

    auto close_cancel = [&]() {
        dropdown_open_ = false;
        held_dirs_ = input.held_dirs;
        if (game.Audio()) PlayCursor(game);
    };
    auto apply_pick = [&](int chosen) {
        chosen = std::clamp(chosen, 0, n - 1);
        const int row_id = row->id;
        const bool changed = chosen != row->value;
        dropdown_open_ = false;
        held_dirs_ = input.held_dirs;
        if (changed) {
            source_->Set(row_id, chosen);
            Refresh();
        }
        if (game.Audio()) game.Audio()->PostEvent(changed ? kChangeSound : kCursorSound, nullptr);
    };

    if (input.back || input.close || input.right_click) {
        close_cancel();
        return Result::None;
    }

    if (std::abs(input.wheel) > 0.01f && max_scroll > 0) {
        dropdown_scroll_ = std::clamp(dropdown_scroll_ + (input.wheel > 0.0f ? -1 : 1), 0, max_scroll);
    }

    if (input.pointer_valid) {
        const glm::vec2 px(640.0f + input.pointer.x * UiCanvas::kUnit, 360.0f - input.pointer.y * UiCanvas::kUnit);
        const bool inside = px.x >= dd.lo.x && px.x <= dd.hi.x && px.y >= dd.lo.y && px.y <= dd.hi.y;
        const float list_top = dd.lo.y + 8.0f;
        const float list_bot = list_top + static_cast<float>(dd.visible) * dd.row_height;
        int hovered = -1;
        if (px.x >= dd.lo.x && px.x <= dd.hi.x && px.y >= list_top && px.y < list_bot) {
            const int slot = static_cast<int>((px.y - list_top) / dd.row_height);
            if (slot >= 0 && slot < dd.visible && dd.scroll + slot < n) {
                hovered = dd.scroll + slot;
            }
        }
        if (input.pointer != last_pointer_ && hovered >= 0 && hovered != dropdown_cursor_) {
            dropdown_cursor_ = hovered;
            dropdown_flash_ = 0.0f;
            if (game.Audio()) PlayCursor(game);
        }
        last_pointer_ = input.pointer;
        if (input.click) {
            if (hovered >= 0) {
                apply_pick(hovered);
            } else if (!inside) {
                close_cancel();
            }
            return Result::None;
        }
    }

    const uint32_t pressed = input.held_dirs & ~held_dirs_;
    uint32_t act = pressed;
    if (pressed) {
        repeat_time_ = -kRepeatDelay;
    } else if (input.held_dirs & (kRawUp | kRawDown)) {
        repeat_time_ += dt;
        if (repeat_time_ >= kRepeatStep) {
            repeat_time_ -= kRepeatStep;
            act = input.held_dirs & (kRawUp | kRawDown);
        }
    }
    held_dirs_ = input.held_dirs;

    int step = 0;
    if (act & kRawUp) step = -1;
    else if (act & kRawDown) step = 1;
    if (step != 0) {
        const int next = std::clamp(dropdown_cursor_ + step, 0, n - 1);
        if (next != dropdown_cursor_) {
            dropdown_cursor_ = next;
            if (dropdown_cursor_ < dropdown_scroll_) dropdown_scroll_ = dropdown_cursor_;
            if (dropdown_cursor_ >= dropdown_scroll_ + dd.visible) dropdown_scroll_ = dropdown_cursor_ - dd.visible + 1;
            dropdown_scroll_ = std::clamp(dropdown_scroll_, 0, max_scroll);
            dropdown_flash_ = 0.0f;
            if (game.Audio()) PlayCursor(game);
        }
    }

    if (input.accept) {
        apply_pick(dropdown_cursor_);
    }
    return Result::None;
}

PcSettingsPage::Result PcSettingsPage::UpdateScreen(Game& game, const MenuInput& input, float dt) {
    const uint32_t pressed = input.held_dirs & ~held_dirs_;
    uint32_t act = pressed;
    if (pressed) {
        repeat_time_ = -kRepeatDelay;
    } else if (input.held_dirs & (kRawLeft | kRawRight)) {
        repeat_time_ += dt;
        if (repeat_time_ >= kRepeatStep) {
            repeat_time_ -= kRepeatStep;
            act = input.held_dirs & (kRawLeft | kRawRight);
        }
    }
    held_dirs_ = input.held_dirs;
    const int dx = (act & kRawLeft) ? -1 : (act & kRawRight) ? 1 : 0;
    const int dy = (pressed & kRawUp) ? -1 : (pressed & kRawDown) ? 1 : 0;
    const bool accept = input.accept || input.click;
    const bool back = input.back || input.close || input.right_click;
    if (dx == 0 && dy == 0 && !accept && !back) {
        return Result::None;
    }
    source_->ScreenInput(dx, dy, accept, back);
    flash_frame_ = 0.0f;
    if (!source_->ScreenShown()) {
        Refresh();
    }
    if (game.Audio()) game.Audio()->PostEvent(dx != 0 || accept ? kChangeSound : kCursorSound, nullptr);
    return Result::None;
}

PcSettingsPage::Result PcSettingsPage::UpdateQuestion(Game& game, const MenuInput& input) {
    const int count = static_cast<int>(rows_.size());
    cursor_ = std::clamp(cursor_, 0, count - 1);
    dialog_nav_.SetFocus(cursor_);
    held_dirs_ = input.held_dirs;
    const dialog::Result result = dialog_nav_.Update(input, count, language_, true);
    if (dialog_nav_.Focus() != cursor_) {
        cursor_ = dialog_nav_.Focus();
        flash_frame_ = 0.0f;
        if (source_) source_->CursorMoved();
        if (game.Audio()) PlayCursor(game);
    }
    if (result == dialog::Result::Cancelled) {
        if (source_ && source_->Back()) {
            SourceChanged();
            if (game.Audio()) PlayCursor(game);
            return Result::None;
        }
        return Result::Back;
    }
    if (result == dialog::Result::Chosen) {
        cursor_ = dialog_nav_.Chosen();
        Change(game, 1, true);
    }
    return Result::None;
}

bool PcSettingsPage::IsQuestion() const {
    return source_ && !source_->Question().empty() && !rows_.empty() && Gallery() == PcGallery::None;
}

bool PcSettingsPage::DialogView(dialog::View& view, int language) const {
    view = dialog::View{};
    view.language = language;
    view.back_label = std::string(PcText("pc_back", language));
    if (IsQuestion()) {
        view.text = PcNoteText(source_->Question(), language);
        for (const Placed& p : rows_) view.choices.push_back(std::string(PcText(RowOf(p).label, language)));
        view.focus = cursor_;
        view.back_label.clear();
        view.dim = 0.0f;
        return true;
    }
    const PcSettingRow* row = confirming_ ? CurrentRow() : nullptr;
    if (!row || !row->action) return false;
    view.text = PcNoteText(row->confirm_note.empty() ? std::string_view("pc_dlg_reset") : std::string_view(row->confirm_note), language);
    view.choices = {std::string(PcText("pc_save_move_yes", language)), std::string(PcText("pc_save_move_no", language))};
    view.back_label.clear();
    view.focus = dialog_nav_.Focus();
    return true;
}

void PcSettingsPage::Step(bool active) {
    flash_frame_ += 2.0f;
    if (active && ++refresh_frames_ >= (source_ ? std::max(1, source_->RefreshEveryFrames()) : 15)) {
        refresh_frames_ = 0;
        Refresh();
    }
}

bool PcSettingsPage::ValueLess(const PcSettingRow& row) const {
    if (row.values.empty()) return true;
    const int shown = std::clamp(ShownValue(row), 0, static_cast<int>(row.values.size()) - 1);
    return row.values[shown].empty();
}

void PcSettingsPage::Apply(UifView& view, int language) const {
    language_ = language;
    auto text = [&](uint16_t id, std::string_view key) {
        view.State(id).text = std::string(PcText(key, language));
        view.State(id).font_type = UiFontType::PcSystem;
    };
    text(title_, source_ ? source_->Title() : "pc_title");
    const bool tabbed = Tabbed();
    view.State(title_).visible = !tabbed;
    for (int i = 0; i < kMaxTabs; ++i) {
        auto& state = view.State(tab_nodes_[i]);
        const bool used = tabbed && i < static_cast<int>(tab_spans_.size());
        const glm::vec2 span = used ? tab_spans_[i] : glm::vec2(0.0f);
        state.visible = used && span.x >= strip_left_ + kTabEnd - 0.01f && span.y <= kStripRight - kTabEnd + 0.01f;
        if (!used) continue;
        const int tab = tab_order_[i];
        const bool active = source_ && tab == source_->Tab();
        state.text = std::string(PcText(source_->TabLabel(tab), language));
        state.font_type = UiFontType::PcSystem;
        state.offset = glm::vec2(span.x, strip_y_);
        state.mirror_rtl = false;
        state.color = glm::vec4(1.0f, 1.0f, 1.0f, active ? 1.0f : 0.5f);
        if (active) {
            view.State(tab_bar_group_).offset = glm::vec2((span.x + span.y) * 0.5f, strip_y_);
            view.State(tab_bar_group_).scale = glm::vec2(std::max(0.2f, (span.y - span.x + 3.0f) / (2.0f * bar_half_)), 1.0f);
            const float sx = (span.y - span.x) / (2.0f * line_half_);
            view.State(tab_rule_).offset = glm::vec2((span.x + span.y) * 0.5f - sx * line_centre_, strip_y_ + kLineOffset.y);
            view.State(tab_rule_).scale = glm::vec2(sx, 1.0f);
        }
    }
    view.State(tab_rule_).visible = tabbed;
    view.State(tab_bar_group_).visible = tabbed;
    view.State(tab_bar_group_).color = glm::vec4(1.0f, 1.0f, 1.0f, 0.45f);
    for (const auto [node, token, x] : {std::tuple{tab_prev_text_, "{tabprev}", strip_left_}, std::tuple{tab_next_text_, "{tabnext}", kStripRight - kTabEnd + 1.0f}}) {
        auto& state = view.State(node);
        state.visible = tabbed && tab_spans_.size() > 1;
        state.text = token;
        state.font_type = UiFontType::PcSystem;
        state.offset = glm::vec2(x, strip_y_);
        state.color = glm::vec4(1.0f, 1.0f, 1.0f, 0.85f);
    }
    {
        auto& state = view.State(donate_text_);
        state.visible = DonateShown();
        state.text = "{donate} " + std::string(PcText("pc_support_patreon", language));
        state.font_type = UiFontType::PcSystem;
        const std::optional<glm::vec4> drawn = view.TextRect(donate_text_);
        if (drawn) donate_rect_ = *drawn;
        const int latch = language;
        const std::string shown_text = state.text.value_or(std::string());
        const bool same_text = donate_prev_text_ == shown_text;
        donate_prev_text_ = shown_text;
        if (drawn && same_text && donate_latch_language_ != latch) {
            donate_latched_ = (drawn->z - drawn->x) / UiCanvas::kUnit;
            donate_latch_language_ = latch;
        }
        const float width = donate_latch_language_ == latch ? donate_latched_ : donate_width_;
        state.offset = glm::vec2(std::round((kStripRight - 1.0f - width) * 10.0f) * 0.1f, -31.0f);
        state.color = glm::vec4(1.0f, 1.0f, 1.0f, donate_hover_ ? 1.0f : 0.8f);
    }
    text(back_text_, "pc_back");
    const bool quit_selected = false;
    {
        auto& state = view.State(footer_text_);
        state.visible = footer_.has_value() && tabbed;
        state.font_type = UiFontType::PcSystem;
        const std::string shown = footer_ ? "{update} " + std::string(PcText(footer_->label, language)) +
                                                (footer_->values.empty() ? std::string() : "  " + footer_->values.front())
                                          : std::string();
        state.text = shown;
        const std::optional<glm::vec4> drawn = view.TextRect(footer_text_);
        if (drawn) update_rect_ = *drawn;
        if (drawn && update_latch_text_ != shown) {
            update_latched_ = (drawn->z - drawn->x) / UiCanvas::kUnit;
            update_latch_text_ = shown;
        }
        const float width = update_latch_text_ == shown ? update_latched_ : 0.0f;
        state.offset = glm::vec2(std::round((kCenterX - 0.5f * width) * 10.0f) * 0.1f, -31.0f);
        state.color = glm::vec4(1.0f, 1.0f, 1.0f, footer_ && !footer_->enabled ? 0.4f : update_hover_ ? 1.0f : 0.8f);
    }
    text(quit_text_, "pc_quit");
    view.State(quit_text_).visible = false;
    const PcGallery gallery = Gallery();
    text(navigation_text_, source_ && source_->IsLoopBrowser() ? "pc_loop_start_hint" : Browser() || gallery != PcGallery::None ? "pc_archive_hint" : "pc_nav_columns");
    view.State(navigation_text_).font_type = UiFontType::PcSystem;
    view.State(navigation_text_).inline_pictures.clear();
    const PcPanel cursor_panel = gallery == PcGallery::Wall ? PanelOf(cursor_) : PcPanel{};
    const bool audio_entry = cursor_panel.audio;
    if (audio_entry && CurrentRow()) view.State(navigation_text_).text = PcNoteText(ShownNote(*CurrentRow()), language);
    if (cursor_panel.music && !cursor_panel.locked && CurrentRow() && CurrentRow()->enabled) {
        view.State(navigation_text_).text = std::string(PcText(cursor_panel.music_playing ? "pc_archive_music_pause_hint" : "pc_archive_music_play_hint", language));
    }
    const bool two_columns = std::any_of(sections_.begin(), sections_.end(), [](const PcSettingSection& s) { return s.column > 0; });
    view.State(navigation_text_).visible = !tabbed && ((source_ && source_->IsLoopBrowser()) || two_columns || gallery != PcGallery::None);
    view.State(back_text_).font_type = UiFontType::PcSystem;
    view.State(quit_text_).font_type = UiFontType::PcSystem;
    view.State(back_text_).color = glm::vec4(1.0f, 1.0f, 1.0f, cursor_ == static_cast<int>(rows_.size()) ? 1.0f : 0.55f);
    view.State(quit_text_).color = glm::vec4(1.0f, 1.0f, 1.0f, quit_selected ? 1.0f : 0.55f);
    for (int i = 0; i < kMaxSections; ++i) {
        const glm::vec2 at = i < static_cast<int>(headers_.size()) ? headers_[i] + glm::vec2(0.0f, Browser() ? 0.0f : scroll_y_) : glm::vec2(0.0f);
        const int first_row = i < static_cast<int>(header_rows_.size()) ? header_rows_[i] : -1;
        const bool row_shown = first_row < 0 || first_row >= static_cast<int>(rows_.size()) || Browser() ||
                               InWindow(Shown(rows_[first_row]).y, Shown(rows_[first_row]).y - static_cast<float>(rows_[first_row].lines - 1) * line_);
        const bool used = i < static_cast<int>(headers_.size()) && InWindow(at.y, at.y - 1.0f) && row_shown;
        view.State(section_nodes_[i].text).visible = used;
        view.State(section_nodes_[i].line).visible = used;
        if (!used) {
            continue;
        }
        view.State(section_nodes_[i].text).offset = at;
        view.State(section_nodes_[i].line).offset = at + kLineOffset;
        view.State(section_nodes_[i].text).mirror_rtl = false;
        const float end = Browser() ? 60.0f : wide_ ? kWideValueX + kWideArrow : at.x < 0.0f ? kValueX[0] + kArrow : kValueX[1] + kArrow;
        view.State(section_nodes_[i].text).text_end = end - at.x;
    }
    int header = 0;
    for (int column = 0; column < 2; ++column) {
        for (const PcSettingSection& section : sections_) {
            if (std::clamp(section.column, 0, 1) == column && header < kMaxSections) {
                text(section_nodes_[header++].text, section.title);
                if (header == 1 && Browser() && static_cast<int>(rows_.size()) > kBrowserRows) {
                    auto& t = view.State(section_nodes_[0].text).text;
                    if (t) *t += std::format("   {} / {}", std::min(cursor_ + 1, static_cast<int>(rows_.size())), rows_.size());
                }
            }
        }
    }
    const int visible = gallery != PcGallery::None ? 0 : VisibleRows();
    const float arrow = ArrowX();
    for (int i = 0; i < kMaxRows; ++i) {
        const int index = first_ + i;
        const bool used = i < visible && InWindow(Shown(rows_[index]).y, Shown(rows_[index]).y - static_cast<float>(rows_[index].lines - 1) * line_);
        view.State(row_nodes_[i].label).visible = used;
        view.State(row_nodes_[i].group).visible = used;
        view.State(row_nodes_[i].decrease).visible = false;
        view.State(row_nodes_[i].increase).visible = false;
        if (!used) {
            continue;
        }
        Placed p = rows_[index];
        p.label = Shown(p);
        p.value.y = p.label.y;
        const PcSettingRow& row = RowOf(p);
        const bool selected = index == cursor_;
        const bool adjustable = !row.link && row.values.size() > 1;
        for (const auto [node, delta] : {std::pair{row_nodes_[i].decrease, -1}, std::pair{row_nodes_[i].increase, 1}}) {
            auto& state = view.State(node);
            if (row.dropdown && adjustable) {
                state.visible = delta > 0;
                state.font_type = UiFontType::PcSystem;
                state.text = "v";
                state.font_language = 0;
                state.offset = p.value + glm::vec2(arrow, 0.0f);
                state.color = glm::vec4(1.0f, 1.0f, 1.0f, row.enabled ? (selected ? 1.0f : 0.65f) : 0.25f);
            } else {
                state.visible = adjustable;
                state.font_type = UiFontType::PcSystem;
                state.text = delta < 0 ? "<" : ">";
                state.font_language = 0;
                state.offset = p.value + glm::vec2(arrow * static_cast<float>(delta), 0.0f);
                const int at = ShownValue(row);
                const bool available = row.enabled && (row.wrap || (delta < 0 ? at > 0 : at + 1 < static_cast<int>(row.values.size())));
                state.color = glm::vec4(1.0f, 1.0f, 1.0f, available ? (selected ? 1.0f : .65f) : .25f);
            }
        }
        text(row_nodes_[i].label, row.label);
        view.State(row_nodes_[i].label).offset = p.label;
        view.State(row_nodes_[i].label).color = glm::vec4(1.0f, 1.0f, 1.0f, row.enabled ? 1.0f : 0.35f);
        const float label_scale = Browser() || (source_ && source_->CompactRows()) ? .8f : 1.0f;
        view.State(row_nodes_[i].label).scale = glm::vec2(label_scale);
        const float end = LabelEnd(p, row);
        view.State(row_nodes_[i].label).mirror_rtl = true;
        view.State(row_nodes_[i].label).text_end = (end - p.label.x) / label_scale;
        view.State(row_nodes_[i].label).text_box.reset();
        view.State(row_nodes_[i].label).text_line_pitch.reset();
        if (p.lines > 1) {
            auto& label = view.State(row_nodes_[i].label);
            label.text_end.reset();
            const float x = 640.0f + UiCanvas::kUnit * (p.label.x - kCenterX);
            const float y = 360.0f - UiCanvas::kUnit * p.label.y - UiCanvas::kUnit * line_ * 0.5f;
            label.text_box = glm::vec4(x, y, 640.0f + UiCanvas::kUnit * (end - kCenterX), y + UiCanvas::kUnit * line_ * static_cast<float>(p.lines));
            label.text_line_pitch = UiCanvas::kUnit * line_;
        }
        view.State(row_nodes_[i].group).offset = p.value;
        view.State(row_nodes_[i].group).color = glm::vec4(1.0f, 1.0f, 1.0f, (selected ? 1.0f : 0.65f) * (row.enabled ? 1.0f : 0.5f));
        const int shown = std::clamp(ShownValue(row), 0, std::max(0, static_cast<int>(row.values.size()) - 1));
        std::string_view value = row.values.empty() ? std::string_view() : std::string_view(row.values[shown]);
        text(row_nodes_[i].value, value);
        view.State(row_nodes_[i].value).text_fit = UiCanvas::kUnit * (Browser() ? 30.0f : ValueRoom());
        if (row.ValueDisabled(shown)) {
            auto& color = view.State(row_nodes_[i].group).color;
            if (color) color->a *= 0.45f;
        }
    }
    {
        const ScrollBar bar = Bar();
        const auto shape = [&](uint16_t node, bool shown, glm::vec4 box, bool triangle_up, bool triangle_down, float alpha) {
            auto& state = view.State(node);
            state.visible = shown;
            state.texture = -1;
            state.color = glm::vec4(1.0f, 1.0f, 1.0f, alpha);
            const glm::vec2 lo(box.x, box.y), hi(box.z, box.w);
            const float mid = (box.x + box.z) * 0.5f;
            if (triangle_up) state.positions = std::vector<glm::vec2>{{mid, hi.y}, {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, lo.y}};
            else if (triangle_down) state.positions = std::vector<glm::vec2>{{mid, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}, {lo.x, hi.y}};
            else state.positions = std::vector<glm::vec2>{{lo.x, hi.y}, {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}};
        };
        shape(arrow_up_, bar.scrollable && bar.above, bar.up, true, false, arrow_up_hover_ ? 1.0f : 0.95f);
        shape(arrow_down_, bar.scrollable && bar.below, bar.down, false, true, arrow_down_hover_ ? 1.0f : 0.95f);
        shape(bar_track_, bar.scrollable, bar.track, false, false, 0.22f);
        shape(bar_thumb_, bar.scrollable, bar.thumb, false, false, 0.75f);
    }
    const PcSettingRow* current = CurrentRow();
    view.State(bar_group_).visible = (current != nullptr || (tabbed && cursor_ < 0)) && gallery == PcGallery::None;
    if (tabbed && cursor_ < 0) {
        view.State(bar_group_).visible = false;
        float alpha = 1.0f;
        if (flash_animation_) {
            for (const ui::UiaNode& node : flash_animation_->Nodes()) {
                if (node.node != flash_node_) continue;
                for (const ui::UiaTrack& track : node.tracks) {
                    if (track.unit == ui::UiaAnimation::kColor) alpha = ui::UiaAnimation::Sample(track, flash_frame_).a;
                }
            }
        }
        view.State(tab_bar_group_).color = glm::vec4(1.0f, 1.0f, 1.0f, alpha);
    }
    if (current && !UpdateDialogOpen()) {
        view.State(bar_group_).offset = rows_[cursor_].value + glm::vec2(0.0f, scroll_y_);
        if (Browser() && ValueLess(*current)) view.State(bar_group_).offset = glm::vec2(-27.0f, Shown(rows_[cursor_]).y);
        else if (ValueLess(*current)) {
            const Placed& p = rows_[cursor_];
            const std::optional<glm::vec2> span = view.TextSpan(row_nodes_[cursor_].label);
            view.State(bar_group_).offset = glm::vec2(p.label.x + (span ? (span->x + span->y) * 0.5f : 8.0f), p.value.y + scroll_y_);
        }
        float alpha = 1.0f;
        if (flash_animation_) {
            for (const ui::UiaNode& node : flash_animation_->Nodes()) {
                if (node.node != flash_node_) {
                    continue;
                }
                for (const ui::UiaTrack& track : node.tracks) {
                    if (track.unit == ui::UiaAnimation::kColor) {
                        alpha = ui::UiaAnimation::Sample(track, flash_frame_).a;
                    }
                }
            }
        }
        view.State(bar_group_).color = glm::vec4(1.0f, 1.0f, 1.0f, alpha * (current->enabled ? 1.0f : 0.4f));
    }
    std::string_view note = current && !audio_entry ? ShownNote(*current) : std::string_view();
    if (tabbed && cursor_ < 0) note = source_->TabNote(source_->Tab());
    if (DonateShown() && donate_hover_) note = "pc_note_patreon";
    if (tabbed && update_hover_ && footer_) note = footer_->note;
    if (quit_selected) note = "pc_note_quit";
    if (source_ && std::getenv("PT_AUDIT_PC_DESCRIPTIONS")) view.State(help_).description_samples = source_->DescriptionSamples();
    view.State(help_).text_box = gallery == PcGallery::None ? glm::vec4(92.0f, 540.0f, 1186.0f, 626.0f)
        : glm::vec4(90.0f, MuseumLayout::kHelpTop, 1190.0f, MuseumLayout::kHelpBottom);
    view.State(help_).font_type = UiFontType::PcSystem;
    view.State(help_).mirror_rtl = true;
    view.State(help_).text = note.empty() ? std::string() : PcNoteText(note, language);
}

void PcSettingsPage::LayoutTabs() {
    tab_spans_.clear();
    tab_order_.clear();
    const int count = source_ ? std::min(source_->TabCount(), kMaxTabs) : 0;
    if (count == 0) return;
    const int language = std::max(0, measure_language_);
    if (measure_view_) {
        if (const auto m = measure_view_->MeasureText(donate_text_, " " + std::string(PcText("pc_support_patreon", language)), language, UiFontType::PcSystem)) {
            donate_width_ = (m->x + 1.5f * m->y) / UiCanvas::kUnit;
        }
    }
    const bool rtl = language == 9;
    for (int v = 0; v < count; ++v) tab_order_.push_back(rtl ? count - 1 - v : v);
    float x = 0.0f;
    std::vector<glm::vec2> spans(count);
    for (int v = 0; v < count; ++v) {
        float width = 8.0f;
        if (measure_view_) {
            if (const auto m = measure_view_->MeasureText(tab_nodes_[0], PcText(source_->TabLabel(tab_order_[v]), language), language, UiFontType::PcSystem)) {
                width = m->x / UiCanvas::kUnit;
            }
        }
        spans[v] = glm::vec2(x, x + width);
        x += width + kTabGap;
    }
    const float start = strip_left_ + kTabEnd;
    const float end = kStripRight - kTabEnd;
    const float total = x - kTabGap;
    const int shown = std::find(tab_order_.begin(), tab_order_.end(), std::clamp(source_->Tab(), 0, count - 1)) - tab_order_.begin();
    if (total <= end - start) {
        tab_scroll_ = rtl ? -(end - start - total) : 0.0f;
    } else {
        if (spans[shown].x - tab_scroll_ < 0.0f) tab_scroll_ = spans[shown].x;
        if (spans[shown].y - tab_scroll_ > end - start) tab_scroll_ = spans[shown].y - (end - start);
        tab_scroll_ = std::clamp(tab_scroll_, 0.0f, total - (end - start));
    }
    for (const glm::vec2& span : spans) tab_spans_.push_back(span + glm::vec2(start - tab_scroll_));
}

int PcSettingsPage::TabAt(glm::vec2 units) const {
    if (!Tabbed()) return -1;
    units.x += kCenterX;
    if (std::abs(units.y - strip_y_) > 1.6f) return -1;
    if (units.x >= strip_left_ - 1.0f && units.x <= strip_left_ + kTabEnd - 0.5f) return -2;
    if (units.x >= kStripRight - kTabEnd + 0.5f && units.x <= kStripRight + 1.0f) return -3;
    for (size_t v = 0; v < tab_spans_.size(); ++v) {
        const glm::vec2 span = tab_spans_[v];
        if (span.x < strip_left_ + kTabEnd - 0.01f || span.y > kStripRight - kTabEnd + 0.01f) continue;
        if (units.x >= span.x - kTabGap * 0.5f && units.x <= span.y + kTabGap * 0.5f) return tab_order_[v];
    }
    return -1;
}

bool PcSettingsPage::DonateShown() const {
    if (Tabbed()) return true;
    if (!source_ || !source_->SupportPrompts() || FullScreen()) return false;
    return source_->IsLoopBrowser() || Browser() || Gallery() != PcGallery::None;
}

bool PcSettingsPage::DonateAt(glm::vec2 units) const {
    if (!DonateShown()) return false;
    const glm::vec2 px(640.0f + units.x * UiCanvas::kUnit, 360.0f - units.y * UiCanvas::kUnit);
    if (donate_rect_.z <= donate_rect_.x) {
        const float right = 640.0f + UiCanvas::kUnit * (kStripRight - 1.0f - kCenterX);
        return px.x >= right - UiCanvas::kUnit * donate_width_ - 4.0f && px.x <= right + 4.0f && std::abs(px.y - 670.0f) <= 16.0f;
    }
    return px.x >= donate_rect_.x - 4.0f && px.x <= donate_rect_.z + 4.0f && px.y >= donate_rect_.y - 6.0f && px.y <= donate_rect_.w + 6.0f;
}

PcSettingsPage::ScrollBar PcSettingsPage::Bar() const {
    ScrollBar bar;
    if (Browser() || Gallery() != PcGallery::None || rows_.empty() || lowest_ >= Floor() - 0.01f) return bar;
    bar.scrollable = true;
    const float most = std::max(0.0f, WindowBottom() - lowest_);
    bar.above = scroll_y_ > 0.01f;
    bar.below = scroll_y_ < most - 0.01f;
    const float centre = 6.5f;
    const float half_width = 3.6f;
    const float low = WindowBottom() + 0.2f;
    bar.up = glm::vec4(centre - half_width, kTop + 1.3f, centre + half_width, kTop + 3.5f);
    bar.down = glm::vec4(centre - half_width, low, centre + half_width, low + 2.4f);
    const float x = kStripRight + 2.0f;
    const float top = kTop + 0.5f;
    const float bottom = low;
    bar.track = glm::vec4(x, bottom, x + 0.9f, top);
    const float window = kTop - WindowBottom();
    const float total = window + most;
    const float height = top - bottom;
    const float thumb = std::clamp(height * window / std::max(window, total), 3.0f, height);
    const float travel = height - thumb;
    const float at = most > 0.0f ? scroll_y_ / most : 0.0f;
    bar.thumb = glm::vec4(x - 0.15f, top - travel * at - thumb, x + 1.05f, top - travel * at);
    return bar;
}

void PcSettingsPage::ScrollBy(Game& game, float dy) {
    const ScrollBar bar = Bar();
    if (!bar.scrollable) return;
    const float most = std::max(0.0f, WindowBottom() - lowest_);
    const float before = scroll_y_;
    scroll_y_ = std::clamp(scroll_y_ + dy, 0.0f, most);
    if (scroll_y_ == before) return;
    const int n = static_cast<int>(rows_.size());
    const auto shown = [&](int i) {
        const Placed& p = rows_[i];
        const float top = p.first ? p.label.y + header_gap_ : p.label.y + row_step_ * 0.5f;
        return top + scroll_y_ <= kTop + 0.01f && RowBottom(p) + scroll_y_ >= WindowBottom() - 0.01f;
    };
    if (cursor_ >= 0 && cursor_ < n && shown(cursor_)) return;
    int target = -1;
    if (dy > 0.0f) {
        for (int i = 0; i < n && target < 0; ++i) {
            if (shown(i)) target = i;
        }
    } else {
        for (int i = n - 1; i >= 0 && target < 0; --i) {
            if (shown(i)) target = i;
        }
    }
    if (target >= 0 && target != cursor_) {
        cursor_ = target;
        confirming_ = false;
        preview_id_ = -1;
        flash_frame_ = 0.0f;
        if (source_) source_->CursorMoved();
        if (game.Audio()) PlayCursor(game);
    }
}

void PcSettingsPage::Donate(Game& game) {
    if (!source_) return;
    source_->Donate();
    ++donations_;
    flash_frame_ = 0.0f;
    if (game.Audio()) game.Audio()->PostEvent(kChangeSound, nullptr);
}

void PcSettingsPage::ShowTab(Game& game, int tab) {
    if (!source_ || tab == source_->Tab()) return;
    const bool strip = cursor_ < 0;
    source_->SelectTab(tab);
    SourceChanged();
    if (strip) cursor_ = -1;
    source_->CursorMoved();
    if (game.Audio()) PlayCursor(game);
}

void PcSettingsPage::SwitchTab(Game& game, int visual) {
    const int count = static_cast<int>(tab_order_.size());
    if (!source_ || count < 2) return;
    const int at = static_cast<int>(std::find(tab_order_.begin(), tab_order_.end(), source_->Tab()) - tab_order_.begin());
    ShowTab(game, tab_order_[((at + visual) % count + count) % count]);
}

void PcSettingsPage::LogLayout(const UifView& view) const {
    static const bool enabled = std::getenv("PT_PC_LAYOUT_LOG") != nullptr;
    if (!enabled || !source_ || Gallery() != PcGallery::None || IsQuestion()) return;
    auto rect = [&](uint16_t id) {
        const std::optional<glm::vec4> r = view.TextRect(id);
        return r ? std::format("{:.1f} {:.1f} {:.1f} {:.1f}", r->x, r->y, r->z, r->w) : std::string("none");
    };
    auto px = [](float x) { return 640.0f + UiCanvas::kUnit * (x - kCenterX); };
    std::string out = std::format("pc-layout page {} lang {} wide {} step {:.2f} line {:.2f} scroll {:.2f} cursor {} rows {}\n", source_->Title(),
                                  language_, wide_ ? 1 : 0, row_step_, line_, scroll_y_, cursor_, rows_.size());
    const int visible = VisibleRows();
    for (int i = 0; i < visible && i < kMaxRows; ++i) {
        const int index = first_ + i;
        const Placed& p = rows_[index];
        const glm::vec2 at = Shown(p);
        if (!InWindow(at.y, at.y - static_cast<float>(p.lines - 1) * line_)) continue;
        const PcSettingRow& row = RowOf(p);
        const float value_x = px(p.value.x);
        const float half = UiCanvas::kUnit * (Browser() ? 15.0f : ValueRoom() * 0.5f);
        out += std::format("pc-layout row {} {} col {} lines {} label {} labelroom {:.1f} {:.1f} value {} valueroom {:.1f} {:.1f} dec {} inc {}\n", index,
                           row.label.find(' ') == std::string::npos ? row.label : std::string("custom"), p.column, p.lines, rect(row_nodes_[i].label),
                           px(p.label.x), px(LabelEnd(p, row)), rect(row_nodes_[i].value), value_x - half, value_x + half,
                           rect(row_nodes_[i].decrease), rect(row_nodes_[i].increase));
    }
    for (int i = 0; i < kMaxSections && i < static_cast<int>(headers_.size()); ++i) {
        out += std::format("pc-layout header {} {}\n", i, rect(section_nodes_[i].text));
    }
    for (int i = 0; i < kMaxTabs && i < static_cast<int>(tab_spans_.size()); ++i) {
        out += std::format("pc-layout tab {} {} {}\n", i, source_->TabLabel(tab_order_[i]), rect(tab_nodes_[i]));
    }
    if (Tabbed()) {
        out += std::format("pc-layout strip prev {} next {} room {:.1f} {:.1f}\n", rect(tab_prev_text_), rect(tab_next_text_),
                           640.0f + UiCanvas::kUnit * (strip_left_ - kCenterX), 640.0f + UiCanvas::kUnit * (kStripRight + 1.0f - kCenterX));
        out += std::format("pc-layout donate {}\n", rect(donate_text_));
    }
    if (const ScrollBar bar = Bar(); bar.scrollable) {
        const auto box = [&](glm::vec4 b) { return std::format("{:.1f} {:.1f} {:.1f} {:.1f}", px(b.x), 360.0f - UiCanvas::kUnit * b.w, px(b.z), 360.0f - UiCanvas::kUnit * b.y); };
        out += std::format("pc-layout scroll flags {} up {} down {} bar {}\n", ScrollFlags(), box(bar.up), box(bar.down), box(bar.track));
    }
    out += std::format("pc-layout help {} box 90 540 1190 626\n", rect(help_));
    out += std::format("pc-layout nav {} back {}\n", rect(navigation_text_), rect(back_text_));
    std::string key = out;
    if (key == layout_logged_) return;
    layout_logged_ = key;
    LogInfo("{}pc-layout end", out);
}

}
