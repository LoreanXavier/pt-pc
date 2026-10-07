#include "game/ui/pc_settings_page.h"

#include <algorithm>
#include <format>
#include <cstdlib>

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

constexpr float kLabelX[2] = {-50.0f, 15.0f};
constexpr float kValueX[2] = {-18.0f, 47.0f};
constexpr float kCenterX = (kLabelX[0] + kValueX[1] + 13.0f) * 0.5f;
constexpr float kTop = 23.0f;
constexpr float kFirstRow = 5.0f;
constexpr float kRowStep = 3.0f;
constexpr float kRowFloor = -20.0f;
constexpr float kMinRowStep = 2.0f;
constexpr glm::vec2 kLineOffset{16.0f, -1.0f};
constexpr glm::vec2 kBackIconAt{-52.0f, -27.0f};
constexpr glm::vec2 kBackTextAt{-50.0f, -27.0f};
constexpr glm::vec2 kQuitTextAt{-25.0f, -27.0f};
constexpr glm::vec2 kHelpAt{5.0f, -21.5f};
constexpr glm::vec2 kHelpSize{1100.0f, 60.0f};
constexpr int kBrowserRows = 18;
constexpr float kBrowserStep = 2.0f;
constexpr float kRepeatDelay = 0.4f;
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
    help_ = Clone(model, kTipText, root_, kHelpAt, "help");
    model.FindById(help_)->size = kHelpSize;
    back_icon_ = Clone(model, kBackIcon, root_, kBackIconAt, "back_icon");
    back_glow_ = Clone(model, kBackGlow, root_, kBackIconAt, "back_glow");
    back_text_ = Clone(model, kBackText, root_, kBackTextAt, "back_text");
    quit_text_ = Clone(model, kBackText, root_, kQuitTextAt, "quit_text");
    navigation_text_ = Clone(model, kBackText, root_, glm::vec2(2.0f, -27.0f), "navigation_text");
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
    confirming_ = false;
    flash_frame_ = 0.0f;
    held_dirs_ = 0;
    last_pointer_ = glm::vec2(-1.0f);
    Refresh();
}

void PcSettingsPage::Refresh() {
    sections_ = source_ ? source_->Sections() : std::vector<PcSettingSection>{};
    Layout();
    cursor_ = std::clamp(cursor_, 0, static_cast<int>(rows_.size()));
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
            const MuseumLayout::Cell cell = i < cells ? MuseumLayout::HallCell(i) : MuseumLayout::HallText(i - cells);
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
    const int columns = halls ? MuseumLayout::kHallColumns : cells;
    int target = cursor_;
    if (cursor_ < cells) {
        if (act & kRawLeft) target = cursor_ > 0 ? cursor_ - 1 : halls ? cursor_ : cells - 1;
        else if (act & kRawRight) target = cursor_ + 1 < cells ? cursor_ + 1 : halls ? cursor_ : 0;
        else if (act & kRawUp) target = cursor_ - columns >= 0 ? cursor_ - columns : cursor_;
        else if (act & kRawDown) target = std::min(cursor_ + columns, cells);
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
    return Browser() && first_ > 0 ? p.label + glm::vec2(0.0f, kBrowserStep * static_cast<float>(first_)) : p.label;
}

void PcSettingsPage::SourceChanged() {
    cursor_ = 0;
    first_ = 0;
    confirming_ = false;
    flash_frame_ = 0.0f;
    Refresh();
    if (source_) {
        if (const int c = source_->TakeCursor(); c >= 0) {
            cursor_ = std::clamp(c, 0, static_cast<int>(rows_.size()));
            Scroll();
        }
    }
}

void PcSettingsPage::Layout() {
    row_step_ = source_ && source_->CompactRows() ? kMinRowStep : kRowStep;
    while (true) {
        rows_.clear();
        headers_.clear();
        float y[2] = {kTop, kTop};
        float lowest = kTop;
        for (int column = 0; column < 2; ++column) {
            for (size_t s = 0; s < sections_.size() && static_cast<int>(headers_.size()) < kMaxSections; ++s) {
                if (std::clamp(sections_[s].column, 0, 1) != column) {
                    continue;
                }
                headers_.emplace_back(kLabelX[column], y[column]);
                const bool compact = sections_.size() == 6;
                const float gap = compact ? 3.0f : kFirstRow;
                float row_y = y[column] - gap;
                const bool browser = Browser();
                for (size_t r = 0; r < sections_[s].rows.size() && (browser || static_cast<int>(rows_.size()) < kMaxRows); ++r) {
                    rows_.push_back({static_cast<int>(s), static_cast<int>(r), {kLabelX[column], row_y}, {kValueX[column], row_y}});
                    if (!browser) lowest = std::min(lowest, row_y);
                    row_y -= browser ? kBrowserStep : row_step_;
                }
                y[column] = row_y + (browser ? kBrowserStep : row_step_) - gap;
            }
        }
        if (lowest >= kRowFloor - 0.01f || row_step_ <= kMinRowStep) {
            break;
        }
        row_step_ = std::max(kMinRowStep, row_step_ - 0.1f);
    }
}

std::string_view PcSettingsPage::ShownNote(const PcSettingRow& row) const {
    const int shown = ShownValue(row);
    return row.ValueDisabled(shown) ? std::string_view(row.value_notes[shown]) : std::string_view(row.note);
}

const PcSettingRow* PcSettingsPage::CurrentRow() const {
    return cursor_ >= 0 && cursor_ < static_cast<int>(rows_.size()) ? &RowOf(rows_[cursor_]) : nullptr;
}

const PcSettingRow* PcSettingsPage::FindRow(std::string_view label) const {
    for (const PcSettingSection& section : sections_) {
        for (const PcSettingRow& row : section.rows) {
            if (row.label == label) {
                return &row;
            }
        }
    }
    return nullptr;
}

int PcSettingsPage::RowAt(glm::vec2 units) const {
    if (Gallery() != PcGallery::None) return GalleryHit(units);
    units.x += kCenterX;
    const int visible = VisibleRows();
    for (int i = first_; i < first_ + visible; ++i) {
        const Placed& p = rows_[i];
        const glm::vec2 at = Shown(p);
        if (units.x >= at.x - 1.0f && units.x <= p.value.x + 13.0f && std::abs(units.y - at.y) <= (Browser() ? 1.0f : row_step_ * 0.5f)) {
            return static_cast<int>(i);
        }
    }
    return -1;
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
    const int n = static_cast<int>(rows_.size()) + 1;
    if (n == 0) {
        return;
    }
    cursor_ = ((cursor_ + delta) % n + n) % n;
    confirming_ = false;
    preview_id_ = -1;
    flash_frame_ = 0.0f;
    Scroll();
    if (source_) source_->CursorMoved();
    if (game.Audio()) {
        game.Audio()->PostEvent(kCursorSound, nullptr);
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
            return;
        }
        if (confirming_) {
            confirming_ = false;
            source_->Activate(row->id);
            Refresh();
        } else {
            confirming_ = true;
        }
        flash_frame_ = 0.0f;
        if (game.Audio()) {
            game.Audio()->PostEvent(confirming_ ? kCursorSound : kChangeSound, nullptr);
        }
        return;
    }
    const int n = static_cast<int>(row->values.size());
    if (n <= 1) {
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
        game.Audio()->PostEvent(kCursorSound, nullptr);
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

PcSettingsPage::Result PcSettingsPage::Update(Game& game, const MenuInput& input, float dt) {
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
            if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
        } else if (input.back || input.close || input.accept || input.click || input.right_click) {
            const int keep = cursor_;
            source_->Back();
            Refresh();
            cursor_ = std::clamp(keep, 0, static_cast<int>(rows_.size()));
            Scroll();
            if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
        }
        return Result::None;
    }
    if (input.pointer_valid) {
        const int hovered = RowAt(input.pointer);
        if (input.pointer != last_pointer_ && hovered >= 0 && hovered != cursor_) {
            cursor_ = hovered;
            confirming_ = false;
            preview_id_ = -1;
            flash_frame_ = 0.0f;
            if (source_) source_->CursorMoved();
            if (game.Audio()) {
                game.Audio()->PostEvent(kCursorSound, nullptr);
            }
        }
        last_pointer_ = input.pointer;
        if (input.click || input.right_click) {
            if (hovered >= 0) {
                cursor_ = hovered;
                const Placed& placed = rows_[cursor_];
                const PcSettingRow& row = RowOf(placed);
                const bool decrement_arrow = !row.link && !row.action &&
                    std::abs(input.pointer.x + kCenterX - (placed.value.x - 12.0f)) <= 2.0f;
                const bool decrease = input.right_click || decrement_arrow;
                Change(game, decrease ? -1 : 1, !decrease);
            } else if (input.click && BackAt(input.pointer)) {
                if (source_ && source_->Back()) {
                    SourceChanged();
                    if (game.Audio()) {
                        game.Audio()->PostEvent(kCursorSound, nullptr);
                    }
                    return Result::None;
                }
                return Result::Back;
            }
        }
    }
    if (input.back || input.close) {
        if (!confirming_) {
            if (!input.close && source_ && source_->Back()) {
                SourceChanged();
                if (game.Audio()) {
                    game.Audio()->PostEvent(kCursorSound, nullptr);
                }
                return Result::None;
            }
            return Result::Back;
        }
        confirming_ = false;
        if (game.Audio()) {
            game.Audio()->PostEvent(kCursorSound, nullptr);
        }
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
    if (input.switch_column) SwitchColumn(game);
    if (act && Gallery() != PcGallery::None && GalleryMove(game, act)) {
        act = 0;
    }
    if (act & kRawUp) {
        Move(game, -1);
    } else if (act & kRawDown) {
        Move(game, 1);
    } else if (act & kRawLeft) {
        if (cursor_ >= static_cast<int>(rows_.size())) Move(game, -1);
        else Change(game, -1, false);
    } else if (act & kRawRight) {
        if (cursor_ >= static_cast<int>(rows_.size())) Move(game, 1);
        else Change(game, 1, false);
    }
    if (input.accept) {
        if (cursor_ == static_cast<int>(rows_.size())) {
            if (source_ && source_->Back()) {
                SourceChanged();
            } else return Result::Back;
        } else Change(game, 1, true);
    }
    return Result::None;
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
    auto text = [&](uint16_t id, std::string_view key) {
        view.State(id).text = std::string(PcText(key, language));
        view.State(id).font_type = UiFontType::PcSystem;
    };
    text(title_, source_ ? source_->Title() : "pc_title");
    text(back_text_, "pc_back");
    const bool quit_selected = cursor_ == static_cast<int>(rows_.size()) + 1;
    text(quit_text_, quit_selected && confirming_ ? "pc_quit_confirm" : "pc_quit");
    view.State(quit_text_).visible = false;
    const PcGallery gallery = Gallery();
    text(navigation_text_, source_ && source_->IsLoopBrowser() ? "pc_loop_start_hint" : Browser() || gallery != PcGallery::None ? "pc_archive_hint" : "pc_nav_columns");
    view.State(navigation_text_).font_type = UiFontType::PcSystem;
    const bool two_columns = std::any_of(sections_.begin(), sections_.end(), [](const PcSettingSection& s) { return s.column > 0; });
    view.State(navigation_text_).visible = (source_ && source_->IsLoopBrowser()) || two_columns || gallery != PcGallery::None;
    view.State(back_text_).font_type = UiFontType::PcSystem;
    view.State(quit_text_).font_type = UiFontType::PcSystem;
    view.State(back_text_).color = glm::vec4(1.0f, 1.0f, 1.0f, cursor_ == static_cast<int>(rows_.size()) ? 1.0f : 0.55f);
    view.State(quit_text_).color = glm::vec4(1.0f, 1.0f, 1.0f, quit_selected ? 1.0f : 0.55f);
    for (int i = 0; i < kMaxSections; ++i) {
        const bool used = i < static_cast<int>(headers_.size());
        view.State(section_nodes_[i].text).visible = used;
        view.State(section_nodes_[i].line).visible = used;
        if (!used) {
            continue;
        }
        view.State(section_nodes_[i].text).offset = headers_[i];
        view.State(section_nodes_[i].line).offset = headers_[i] + kLineOffset;
    }
    int header = 0;
    for (int column = 0; column < 2; ++column) {
        for (const PcSettingSection& section : sections_) {
            if (std::clamp(section.column, 0, 1) == column && header < kMaxSections) {
                text(section_nodes_[header++].text, section.title);
                if (header == 1 && ((Browser() && static_cast<int>(rows_.size()) > kBrowserRows) || gallery == PcGallery::Wall)) {
                    auto& t = view.State(section_nodes_[0].text).text;
                    if (t) *t += std::format("   {} / {}", std::min(cursor_ + 1, static_cast<int>(rows_.size())), rows_.size());
                }
            }
        }
    }
    const int visible = gallery != PcGallery::None ? 0 : VisibleRows();
    for (int i = 0; i < kMaxRows; ++i) {
        const bool used = i < visible;
        view.State(row_nodes_[i].label).visible = used;
        view.State(row_nodes_[i].group).visible = used;
        view.State(row_nodes_[i].decrease).visible = false;
        view.State(row_nodes_[i].increase).visible = false;
        if (!used) {
            continue;
        }
        const int index = first_ + i;
        Placed p = rows_[index];
        p.label = Shown(p);
        p.value.y = p.label.y;
        const PcSettingRow& row = RowOf(p);
        const bool selected = index == cursor_;
        const bool adjustable = !row.action && !row.link && row.values.size() > 1;
        for (const auto [node, delta] : {std::pair{row_nodes_[i].decrease, -1}, std::pair{row_nodes_[i].increase, 1}}) {
            auto& state = view.State(node);
            state.visible = adjustable;
            state.font_type = UiFontType::PcSystem;
            state.text = delta < 0 ? "<" : ">";
            state.offset = p.value + glm::vec2(12.0f * delta, 0.0f);
            const int at = ShownValue(row);
            const bool available = row.enabled && (row.wrap || (delta < 0 ? at > 0 : at + 1 < static_cast<int>(row.values.size())));
            state.color = glm::vec4(1.0f, 1.0f, 1.0f, available ? (selected ? 1.0f : .65f) : .25f);
        }
        text(row_nodes_[i].label, row.label);
        view.State(row_nodes_[i].label).offset = p.label;
        view.State(row_nodes_[i].label).color = glm::vec4(1.0f, 1.0f, 1.0f, row.enabled ? 1.0f : 0.35f);
        const float label_scale = Browser() || (source_ && source_->CompactRows()) ? .8f : 1.0f;
        view.State(row_nodes_[i].label).scale = glm::vec2(label_scale);
        view.State(row_nodes_[i].label).mirror_rtl = true;
        view.State(row_nodes_[i].label).text_end = (p.value.x + (Browser() ? -3.0f : -14.0f) - p.label.x) / label_scale;
        view.State(row_nodes_[i].group).offset = p.value;
        view.State(row_nodes_[i].group).color = glm::vec4(1.0f, 1.0f, 1.0f, (selected ? 1.0f : 0.65f) * (row.enabled ? 1.0f : 0.5f));
        const int shown = std::clamp(ShownValue(row), 0, std::max(0, static_cast<int>(row.values.size()) - 1));
        std::string_view value = row.values.empty() ? std::string_view() : std::string_view(row.values[shown]);
        if (row.action && selected && confirming_) {
            value = "pc_reset_confirm";
        }
        text(row_nodes_[i].value, value);
        if (row.ValueDisabled(shown)) {
            auto& color = view.State(row_nodes_[i].group).color;
            if (color) color->a *= 0.45f;
        }
    }
    const PcSettingRow* current = CurrentRow();
    view.State(bar_group_).visible = current != nullptr && gallery == PcGallery::None;
    if (current) {
        view.State(bar_group_).offset = rows_[cursor_].value;
        if (Browser()) view.State(bar_group_).offset = glm::vec2(-27.0f, Shown(rows_[cursor_]).y);
        else if (ValueLess(*current)) {
            const Placed& p = rows_[cursor_];
            const std::optional<glm::vec2> span = view.TextSpan(row_nodes_[cursor_].label);
            view.State(bar_group_).offset = glm::vec2(p.label.x + (span ? (span->x + span->y) * 0.5f : 8.0f), p.value.y);
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
    std::string_view note = current ? ShownNote(*current) : std::string_view();
    if (current && current->action && confirming_) {
        note = "pc_note_reset_confirm";
    }
    if (quit_selected) note = confirming_ ? "pc_note_quit_confirm" : "pc_note_quit";
    if (source_ && std::getenv("PT_AUDIT_PC_DESCRIPTIONS")) view.State(help_).description_samples = source_->DescriptionSamples();
    view.State(help_).text_box = glm::vec4(90.0f, 585.0f, 1190.0f, 620.0f);
    view.State(help_).font_type = UiFontType::PcSystem;
    view.State(help_).mirror_rtl = true;
    view.State(help_).text = note.empty() ? std::string() : PcNoteText(note, language);
}

}
