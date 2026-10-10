#pragma once

#include <volk.h>

#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "engine/platform/input.h"
#include "engine/ui/ui_batch.h"
#include "game/ui/demo_ui.h"
#include "game/ui/options_menu.h"
#include "game/ui/photo_panel.h"
#include "game/ui/save_icon.h"
#include "game/ui/subliminal.h"
#include "game/ui/subtitle_player.h"
#include "game/ui/ui_assets.h"

namespace pt {
class Renderer;
class TextureManager;
class Vfs;
}

namespace pt::game {

class Game;

class GameUi {
public:
    static constexpr int kOverlayPriority = 100;
    static constexpr int kSubliminalStringPriority = 128;
    static constexpr int kSubliminalNoisePriority = 129;
    static constexpr int kSubtitlePriority = 151;
    static constexpr int kStrongSubtitlePriority = 210;
    static constexpr int kPauseMenuPriority = 171;
    // the PC letterbox (pt.ini [display] letterbox): over the game's picture, its overlays and the subtitles (which move into the
    // picture), under the menus, the demos' UI, the fades and the save icon
    static constexpr int kLetterboxPriority = 170;
    // the save request's dialogs stand for the PS4's system message dialog, over everything the game draws
    static constexpr int kSaveDialogPriority = 20000;
    // the speedrun timer (PC extra): over the game's fades and the demos' UI, under the save dialog
    static constexpr int kSpeedrunPriority = 19000;
    // the update notice (PC, docs/updates.md): over the subtitles and the pause menu (its page's dimmed picture would hide it)
    static constexpr int kUpdateNoticePriority = 172;
    // the port's credits page after the ending's credits (port_credits.h): over the fades and the demos' UI, under the menus that
    // follow it, the speedrun timer and the save dialog
    static constexpr int kPortCreditsPriority = 18000;

    GameUi();
    ~GameUi();

    bool Init(Renderer& renderer, TextureManager& textures, Vfs& vfs);
    void Update(Game& game, const InputState& input, float dt);
    // PC addition: the PS4 game's title picture (sce_sys/pic1.png) over the start's loading screen for `seconds`, under the
    // loading icon, fading out at its end
    void ShowBootSplash(uint32_t texture, float seconds);
    bool BootSplashShown() const { return boot_splash_ != 0 && boot_splash_time_ > 0.0f; }
    void ShowSubtitle(std::string_view subtitle_id, float start_offset_seconds);
    void ClearSubtitles();
    void Record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);
    // the photo mode's overlay in place of the game's UI: the letterbox bars and the side panel, drawn as the PC settings page
    void RecordPhotoMode(VkCommandBuffer cmd, VkExtent2D extent, const PhotoPanelView& view);
    void Shutdown();

    static GameUi* Active();
    static bool ScriptCommand(Game& game, std::string_view op, std::string_view text, std::span<const float> args);
    bool MenuOpen() const { return menu_.IsOpen(); }
    // a subtitle or a caption is on screen or queued (input script `expect speech`)
    bool SpeechShown() const { return subtitles_.Active() || !queued_subtitles_.empty() || last_caption_ != 0; }
    void OpenMenu(Game& game, bool first_boot, OptionsMenu::Page page = OptionsMenu::Page::Original);
    void OpenPcSettings();
    void CloseMenu() { menu_.Close(); }
    void SetPcSettings(PcSettingsSource* source) { menu_.SetPcSource(source); }
    const OptionsMenu& Menu() const { return menu_; }
    void AdvancePresentation(float dt) { menu_.AdvancePresentation(dt); }
    void SetViewExtent(VkExtent2D extent) {
        canvas_ = UiCanvas::Fit(extent);
        canvas_ready_ = true;
    }
    void QueueMenuInput(const MenuInput& input);
    void ShowCaptionKey(uint32_t key, float offset_seconds) { subtitles_.PlayKey(key, offset_seconds); }
    // the frame's aspect ratio between the PC letterbox's bars (LetterboxAspect), 0 for none
    void SetLetterbox(float aspect) { letterbox_ = aspect; }
    // VR's HUD (docs/vr.md): Record draws on the transparent HUD image, so the full screen fade (VR puts it on the eye images) and
    // the letterbox's bars stay out, and the UI's coverage goes in alpha
    void SetVrHud(bool enabled) { vr_hud_ = enabled; }
    SubtitlePlayer& Subtitles() { return subtitles_; }
    DemoUi& DemoGraphs() { return demo_ui_; }
    // the Archive's theater (archive_theater.h): the menu that opened it stays open but is neither drawn nor driven, the UI follows
    // the theater's game (Update), and a line at the bottom names what plays and how to leave
    void SetMenuSuspended(bool suspended) { menu_suspended_ = suspended; }
    bool MenuSuspended() const { return menu_suspended_; }
    void SetTheaterHint(std::string title, std::string hint) {
        theater_title_ = std::move(title);
        theater_hint_ = std::move(hint);
    }
    // the theater began or ended: the player's subtitles, demo graphs and what the UI tracked of the player's game are put aside
    // whole while the theater's own run, and come back as they were
    void EnterTheater(Game& theater);
    void LeaveTheater(Game& game);
    // a newer release (docs/updates.md): one quiet line at the top of the picture, once a run, when the player is in the game
    // (controller step 15, no demo camera, no theater) or in the pause menu; `note` is a PC text key with its arguments
    // (PcNoteText), translated when drawn. It waits while held (the photo mode, VR).
    void ShowUpdateNotice(std::string note);
    void HoldUpdateNotice(bool held) { update_notice_held_ = held; }
    bool UpdateNoticeShown() const { return update_notice_time_ >= 0.0f && update_notice_time_ < kUpdateNoticeLength; }

private:
    void UpdateSubliminal(Game& game, float dt, bool paused);
    void DrawSubtitle(ui::UiBatch& batch, const UiCanvas& canvas);
    void DrawSubliminal(ui::UiBatch& batch, bool string_pass);
    uint32_t OverlayTexture(const std::string& name);
    void DrawPhotoPanel(ui::UiBatch& batch, const UiCanvas& canvas, const PhotoPanelView& view);
    // the height of each black bar over and under a frame of this size for a letterbox aspect (0 when the frame is not taller)
    static float LetterboxBar(glm::vec2 full, float aspect);
    static void DrawLetterbox(ui::UiBatch& batch, glm::vec2 full, float aspect);
    void DrawSaveDialog(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void DrawTheaterHint(ui::UiBatch& batch, const UiCanvas& canvas);
    void DrawPortCredits(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void UpdateSpeedrun(const Game& game);
    void DrawSpeedrun(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void UpdateUpdateNotice(Game& game, float dt);
    void DrawUpdateNotice(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);

    Renderer* renderer_ = nullptr;
    std::unique_ptr<ui::UiBatch> batch_;
    UiAssets assets_;
    SubtitlePlayer subtitles_;
    SubliminalEffect subliminal_;
    OptionsMenu menu_;
    DemoUi demo_ui_;
    SaveIcon save_icon_;
    uint32_t boot_splash_ = 0;
    float boot_splash_time_ = 0.0f;
    uint32_t save_io_seen_ = 0;
    float save_icon_wait_ = 0.0f;
    bool save_icon_loading_ = false;
    bool ready_ = false;
    // the subliminal text positions (the Fox generator in the eboot, seeded once per boot from the time): seeded at the first use
    std::mt19937 rng_;
    bool rng_seeded_ = false;

    std::vector<std::pair<std::string, float>> queued_subtitles_;
    MenuInput queued_menu_input_;
    uint32_t previous_held_ = 0;
    bool pause_owner_ = false;

    int language_ = 0;
    bool subtitles_on_ = false;
    bool strong_subtitles_ = false;
    std::string hidden_subtitle_;
    std::string logged_subtitle_;
    uint32_t last_caption_ = 0;
    float last_caption_time_ = 0.0f;
    int last_subliminal_ = -1;
    float last_subliminal_time_ = 0.0f;

    glm::vec4 fade_{0.0f};
    int fade_priority_ = 192;
    std::string overlay_name_;
    uint32_t overlay_texture_ = 0;
    bool overlay_ = false;
    std::unordered_map<std::string, uint32_t> overlay_cache_;
    uint32_t subliminal_textures_[SubliminalEffect::kImageCount] = {};
    uint32_t noise_texture_ = 0;
    uint32_t noise_normal_texture_ = 0;
    UiCanvas canvas_;
    bool canvas_ready_ = false;
    bool pc_request_ = false;
    float letterbox_ = 0.0f;
    bool vr_hud_ = false;
    std::string save_dialog_text_;
    bool menu_suspended_ = false;
    // the side not shown: the theater's while the player's game runs, the player's while the theater does (EnterTheater)
    SubtitlePlayer other_subtitles_;
    DemoUi other_demo_ui_;
    struct Tracked {
        uint32_t save_io_seen = 0;
        uint32_t last_caption = 0;
        float last_caption_time = 0.0f;
        int last_subliminal = -1;
        float last_subliminal_time = 0.0f;
        std::vector<std::pair<std::string, float>> queued_subtitles;
    };
    Tracked other_tracked_;
    void SwapTracked();
    std::string theater_title_;
    std::string theater_hint_;
    // the port's credits page time while controller step 33 shows it, else negative
    float port_credits_time_ = -1.0f;
    // the speedrun overlay's lines (UpdateSpeedrun), drawn in the picture's top left corner
    struct SpeedrunView {
        bool shown = false;
        float alpha = 1.0f;
        std::string total;
        std::string segment_name;
        std::string segment_time;
        std::string delta;
        bool ahead = false;
    } speedrun_view_;
    // the update notice: fade in, hold, fade out (seconds); its time runs only while it may show, -1 before it starts
    static constexpr float kUpdateNoticeFadeIn = 0.5f;
    static constexpr float kUpdateNoticeHold = 6.0f;
    static constexpr float kUpdateNoticeFadeOut = 1.0f;
    static constexpr float kUpdateNoticeLength = kUpdateNoticeFadeIn + kUpdateNoticeHold + kUpdateNoticeFadeOut;
    std::string update_notice_;
    float update_notice_time_ = -1.0f;
    bool update_notice_held_ = false;
};

}
