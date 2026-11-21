#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/platform/input.h"
#include "engine/ui/uia.h"
#include "game/ui/archive_view.h"
#include "game/ui/armory_view.h"
#include "game/ui/dialog_view.h"
#include "game/ui/night_hub_view.h"
#include "game/ui/pc_settings_page.h"
#include "game/ui/uia_player.h"
#include "game/ui/ui_icons.h"
#include "game/ui/uif_view.h"

namespace pt::game {

class Game;

struct MenuInput {
    bool subtitle_up = false;
    bool subtitle_down = false;
    bool brightness_down = false;
    bool brightness_up = false;
    bool toggle_vertical = false;
    bool toggle_horizontal = false;
    bool zoom_pressed = false;
    bool zoom_released = false;
    bool close = false;
    bool accept = false;
    bool back = false;
    bool open_pc = false;
    bool switch_column = false;
    bool tab_prev = false;
    bool tab_next = false;
    bool tab_from_e = false;
    bool donate = false;
    bool update = false;
    bool street_return = false;
    bool crosshair = false;
    bool survival_promo = false;
    uint32_t held_dirs = 0;
    uint32_t music = 0;
    bool pointer_valid = false;
    glm::vec2 pointer{0.0f};
    bool click = false;
    bool pointer_held = false;
    bool right_click = false;
    float wheel = 0.0f;
    bool armory_tab = false;
    bool armory_replay = false;
    bool armory_apply_all = false;
    glm::vec2 right_stick{0.0f};
};

inline constexpr uint32_t kMenuMusicBack = kMusicPressBack;
inline constexpr uint32_t kMenuMusicForward = kMusicPressForward;
inline constexpr uint32_t kMenuMusicRestart = kMusicPressRestart;
inline constexpr uint32_t kMenuMusicLoop = kMusicPressLoop;

MenuInput MapMenuInput(const InputState& input, uint32_t previous_held, uint32_t previous_dirs = 0);

class OptionsMenu {
public:
    enum class State { Closed, SetIn, Active, SetOut };
    enum class Page { Original, Pc };

    bool Init(UiAssets& assets);
    void Show(Game& game, bool first_boot, Page page = Page::Original);
    void Close();
    void SetPcSource(PcSettingsSource* source) { pc_source_ = source; }
    bool OpenSurvivalNightFromDeath() { return pc_source_ && pc_source_->OpenSurvivalNightFromDeath(); }
    Page CurrentPage() const { return page_; }
    const PcSettingsPage& PcPage() const { return pc_; }
    PcSettingsPage& PcPageForScript() { return pc_; }
    void Update(Game& game, const MenuInput& input, float dt);
    void Draw(ui::UiBatch& batch, const UiCanvas& canvas);
    void AdvancePresentation(float dt);

    State CurrentState() const { return state_; }
    float ShownAlpha() const;
    bool IsOpen() const { return state_ != State::Closed; }
    bool FirstBoot() const { return first_boot_; }
    bool PictureFullScreen() const { return state_ == State::Active && page_ == Page::Pc && pc_.FullScreen(); }
    bool TakeClosed() { return std::exchange(just_closed_, false); }
    bool TakeResume() { return std::exchange(resume_, false); }
    bool TakeStreetReturn() { return std::exchange(street_return_, false); }
    int TextLanguage() const { return text_language_; }
    int Selector() const { return selector_; }
    void SetPromptStyle(const PromptStyle& style) { prompt_style_ = style; }
    PromptDevice Prompts() const { return shown_prompts_; }
    const ui::UifModel* Model() const { return model_; }
    float SelectionFlash(float frame) const;
    std::string DialogKind(int* focus) const;

private:
    struct Animation {
        const ui::UiaAnimation* main = nullptr;
        const ui::UiaAnimation* shader = nullptr;
        float speed = 1.0f;
    };

    void ApplyStatic();
    void ApplyPages();
    void DrawScreen(ui::UiBatch& batch, const UiCanvas& canvas);
    void DrawBrain(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 center, float height);
    void SwitchPage(Game& game, Page page);
    void UpdateOriginal(Game& game, const MenuInput& input, float dt);
    void OriginalClick(Game& game, glm::vec2 units, MenuInput& input);
    float TrackAlpha(std::string_view animation, float frame) const;
    void BuildEntryRow(ui::UifModel& model, int icon_atlas, int icon_glow);
    void ApplyPrompts();
    void ApplyTexts();
    int SubtitleWindowStart() const;
    void ApplySubtitleSelection();
    void Play(std::string_view name, bool loop = false);
    bool Playing(std::string_view name) const;
    void PlaySelections();
    void HoldBrightness();
    void Commit(Game& game);
    const Animation* Find(std::string_view name) const;

    UiAssets* assets_ = nullptr;
    const ui::UifModel* model_ = nullptr;
    std::unique_ptr<ui::UifModel> derived_;
    PcSettingsPage pc_;
    ArchiveView archive_view_;
    PcSettingsSource* pc_source_ = nullptr;
    Page page_ = Page::Original;
    Page next_page_ = Page::Original;
    float switch_frame_ = -1.0f;
    uint16_t entry_group_ = 0;
    std::vector<uint16_t> language_groups_;
    std::vector<uint16_t> language_texts_;
    uint16_t language_bar_ = 0;
    uint16_t entry_label_ = 0;
    uint16_t quit_label_ = 0;
    bool quit_confirming_ = false;
    bool quit_focused_ = false;
    dialog::Nav ask_nav_;
    float ask_flash_ = 0.0f;
    uint16_t street_label_ = 0;
    uint16_t street_icon_ = 0;
    uint16_t street_glow_ = 0;
    uint16_t crosshair_label_ = 0;
    uint16_t crosshair_icon_ = 0;
    uint16_t crosshair_glow_ = 0;
    bool crosshair_shown_ = false;
    uint16_t quit_icon_ = 0;
    uint16_t quit_glow_ = 0;
    bool street_shown_ = false;
    bool street_confirming_ = false;
    bool street_return_ = false;
    bool leave_date_ = false;
    void OpenAsk(Game& game, bool quit, uint32_t held);
    void UpdateAsk(Game& game, const MenuInput& input, float dt);
    uint16_t entry_icon_ = 0;
    uint16_t entry_glow_ = 0;
    enum class Button { None, PcSettings, Back, Quit, StreetReturn, Crosshair, SurvivalPromo, SurvivalPromoClose };
    void OpenCrosshair(Game& game);
    void OpenSurvivalPromo(Game& game);
    void DrawSurvivalToast(ui::UiBatch& batch, const UiCanvas& canvas);
    void DrawPcDropdown(ui::UiBatch& batch, const UiCanvas& canvas, const PcDropdownView& dd);
    Button ButtonAt(glm::vec2 units) const;
    uint16_t entry_bar_ = 0;
    uint16_t back_bar_ = 0;
    uint16_t quit_bar_ = 0;
    uint16_t street_bar_ = 0;
    uint16_t crosshair_bar_ = 0;
    uint16_t promo_title_ = 0;
    uint16_t promo_line_ = 0;
    uint16_t promo_action_ = 0;
    uint16_t promo_icon_ = 0;
    uint16_t promo_glow_ = 0;
    uint16_t promo_bar_ = 0;
    float bar_half_ = 10.0f;
    bool survival_promo_shown_ = false;
    bool survival_promo_seen_ = false;
    bool survival_locked_confirming_ = false;
    float promo_toast_t_ = 0.0f;
    Button hovered_ = Button::None;
    float hover_frame_ = 0.0f;
    struct PromptNodes {
        uint16_t icon = 0;
        uint16_t glow = 0;
        Prompt prompt;
    };
    std::vector<PromptNodes> prompt_nodes_;
    PromptStyle prompt_style_;
    PromptDevice shown_prompts_ = PromptDevice::Keyboard;
    bool prompts_shown_ = false;
    uint32_t root_null_hash_ = 0;
    UifView view_;
    UiaPlayers players_;
    std::unordered_map<uint64_t, Animation> animations_;
    std::vector<std::unique_ptr<ui::UiaAnimation>> storage_;
    State state_ = State::Closed;
    bool first_boot_ = false;
    bool just_closed_ = false;
    int selector_ = 4;
    bool vertical_ = false;
    bool horizontal_ = false;
    int brightness_ = 5;
    int text_language_ = 0;
    bool language_timer_ = false;
    float language_time_ = 0.0f;
    void CancelPhotoZoom();
    bool zoom_released_ = false;
    bool zoom_started_ = false;
    float zoom_elapsed_ = 0.0f;
    decltype(ui::UifMaterial::textures) fragment_brain_textures_;
    decltype(ui::UifMaterial::textures) clean_brain_textures_;
    bool resume_ = false;
    bool resume_sent_ = false;
    float clock_ = 0.0f;
    float pillar_alpha_ = 0.0f;
    ArmoryView armory_view_;
    NightHubView night_hub_view_;
    bool see_through_ = false;
    void DrawCrosshairPreview(ui::UiBatch& batch, const UiCanvas& canvas, const CrosshairStyle& style);
    float preview_clock_ = 0.0f;
};

}
