#pragma once

#include <volk.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "engine/platform/input.h"
#include "engine/ui/ui_batch.h"
#include "game/sound_captions.h"
#include "game/ui/demo_ui.h"
#include "game/ui/options_menu.h"
#include "game/ui/dialog_view.h"
#include "game/ui/notice_queue.h"
#include "game/ui/photo_panel.h"
#include "game/ui/save_icon.h"
#include "game/ui/subliminal.h"
#include "game/ui/subtitle_player.h"
#include "game/ui/ui_assets.h"
#include "game/survival/survival.h"

namespace pt {
class Renderer;
class TextureManager;
class Vfs;
}

namespace pt::game {

class Game;
class LisaDate;

class GameUi {
public:
    static constexpr int kGrainPriority = 200;
    static constexpr int kOverlayPriority = 100;
    static constexpr int kSubliminalStringPriority = 128;
    static constexpr int kSubliminalNoisePriority = 129;
    static constexpr int kSubtitlePriority = 151;
    static constexpr int kStrongSubtitlePriority = 210;
    static constexpr int kPauseMenuPriority = 171;
    static constexpr int kLetterboxPriority = 170;
    static constexpr int kSaveDialogPriority = 20000;
    static constexpr int kSpeedrunPriority = 19000;
    static constexpr int kSurvivalPriority = 173;
    static constexpr int kUpdateNoticePriority = 172;
    static constexpr int kPortCreditsPriority = 18000;
    static constexpr int kEndingSkipPriority = 17000;
    static constexpr int kShaderScreenPriority = 1000000;
    static constexpr int kLisaVignettePriority = 150;

    GameUi();
    ~GameUi();

    bool Init(Renderer& renderer, TextureManager& textures, Vfs& vfs);
    void SetSurvivalVisionDirectory(std::string utf8_path) { vision_dir_ = std::move(utf8_path); }
    void Update(Game& game, const InputState& input, float dt);
    void ShowBootSplash(uint32_t texture, float seconds);
    bool BootSplashShown() const { return boot_splash_ != 0 && boot_splash_time_ > 0.0f; }
    void SetShaderScreen(std::string note) { shader_screen_ = std::move(note); }
    bool ShaderScreenShown() const { return !shader_screen_.empty(); }
    std::string DialogKind(int* focus) const;
    void ShowSubtitle(std::string_view subtitle_id, float start_offset_seconds);
    void ClearSubtitles();
    enum class Part { All, Below, Top };
    void Record(gpu::CommandList& cmd, gpu::ImageViewHandle target, gpu::Extent2D extent, Part part = Part::All);
    bool TopPending() const { return top_pending_; }
    void RecordPhotoMode(gpu::CommandList& cmd, gpu::Extent2D extent, const PhotoPanelView& view);
    void Shutdown();

    static GameUi* Active();
    static bool ScriptCommand(Game& game, std::string_view op, std::string_view text, std::span<const float> args);
    bool MenuOpen() const { return menu_.IsOpen(); }
    bool SurvivalChoosing() const { return survival_choosing_; }
    bool SpeechShown() const { return subtitles_.Active() || !queued_subtitles_.empty() || last_caption_ != 0; }
    void OpenMenu(Game& game, bool first_boot, OptionsMenu::Page page = OptionsMenu::Page::Original);
    void OpenPcSettings();
    void CloseMenu() { menu_.Close(); }
    void SetPcSettings(PcSettingsSource* source) { menu_.SetPcSource(source); }
    const OptionsMenu& Menu() const { return menu_; }
    void AdvancePresentation(float dt) { menu_.AdvancePresentation(dt); }
    void SetViewExtent(gpu::Extent2D extent) {
        canvas_ = UiCanvas::Fit(extent);
        canvas_ready_ = true;
    }
    void QueueMenuInput(const MenuInput& input);
    void ShowCaptionKey(uint32_t key, float offset_seconds) { subtitles_.PlayKey(key, offset_seconds); }
    void SetLetterbox(float aspect) { letterbox_ = aspect; }
    void SetAccessibility(int subtitle_size, int subtitle_bg, bool reduced_flashing) {
        subtitle_size_ = subtitle_size;
        subtitle_bg_ = subtitle_bg;
        reduced_flashing_ = reduced_flashing;
    }
    void SetVrHud(bool enabled) { vr_hud_ = enabled; }
    SubtitlePlayer& Subtitles() { return subtitles_; }
    DemoUi& DemoGraphs() { return demo_ui_; }
    void SetMenuSuspended(bool suspended) { menu_suspended_ = suspended; }
    bool MenuSuspended() const { return menu_suspended_; }
    void SetTheaterHint(std::string title, std::string hint) {
        theater_title_ = std::move(title);
        theater_hint_ = std::move(hint);
    }
    void EnterTheater(Game& theater);
    void LeaveTheater(Game& game);
    void ShowUpdateNotice(std::string note);
    void AskUpdate() { update_question_ = true; }
    void SetFpsCounter(bool shown, int rendered, int presented) {
        fps_shown_ = shown;
        fps_rendered_ = rendered;
        fps_presented_ = presented;
    }
    void ShowNotice(std::string note);
    void HoldUpdateNotice(bool held) { update_notice_held_ = held; }
    bool UpdateNoticeShown() const { return notices_.Visible(); }
    static constexpr std::string_view kUpdatedNotice = "pc_notice_updated_support";
    void SetSupportPrompts(bool shown) { support_prompts_ = shown; }
    bool SupportNoticeShown() const { return notices_.Visible() && notices_.Current() == kUpdatedNotice; }
    bool TakeSupportRequest(std::string* from) {
        if (support_request_.empty()) return false;
        if (from) *from = support_request_;
        support_request_.clear();
        return true;
    }
    std::string_view SupportPlace() const;
    void SetLisaDate(LisaDate* date) { lisa_date_ = date; }
    UiAssets& Assets() { return assets_; }

private:
    void UpdateSubliminal(Game& game, float dt, bool paused);
    void DrawSubtitle(ui::UiBatch& batch, const UiCanvas& canvas);
    void DrawSoundCaptions(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void DrawSubliminal(ui::UiBatch& batch, bool string_pass);
    uint32_t OverlayTexture(const std::string& name);
    void DrawPhotoPanel(ui::UiBatch& batch, const UiCanvas& canvas, const PhotoPanelView& view);
    static float LetterboxBar(glm::vec2 full, float aspect);
    static void DrawLetterbox(ui::UiBatch& batch, glm::vec2 full, float aspect);
    static void DrawLisaVignette(ui::UiBatch& batch, glm::vec2 full, float strength);
    void DrawSaveDialog(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    bool PointerUnits(const InputState& input, glm::vec2* units) const;
    void DrawTheaterHint(ui::UiBatch& batch, const UiCanvas& canvas);
    void DrawPortCredits(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void DrawEndingSkip(ui::UiBatch& batch, const UiCanvas& canvas);
    void UpdateSpeedrun(const Game& game);
    void DrawSpeedrun(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void DrawFpsCounter(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void DrawSurvival(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void UpdateUpdateNotice(Game& game, float dt);
    void DrawUpdateNotice(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    void DrawSupportButton(ui::UiBatch& batch, const UiCanvas& canvas, std::string_view label, glm::vec2 anchor, ui::TextAlign align,
                           glm::vec3 ink, glm::vec3 plate, float alpha, bool framed = true);
    void UpdateSupport(Game& game, const InputState& input, MenuInput& menu_input);

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
    std::string shader_screen_;
    dialog::Nav save_nav_;
    bool save_nav_open_ = false;
    float dialog_flash_ = 0.0f;
    uint32_t save_io_seen_ = 0;
    float save_icon_wait_ = 0.0f;
    bool save_icon_loading_ = false;
    bool ready_ = false;
    std::mt19937 rng_;
    bool rng_seeded_ = false;

    std::vector<std::pair<std::string, float>> queued_subtitles_;
    MenuInput queued_menu_input_;
    uint32_t previous_held_ = 0;
    uint32_t previous_dirs_ = 0;
    bool pause_owner_ = false;

    int language_ = 0;
    bool subtitles_on_ = false;
    bool strong_subtitles_ = false;
    std::string hidden_subtitle_;
    std::string logged_subtitle_;
    struct SoundCaptionDraw {
        std::string key;
        CaptionSide side = CaptionSide::None;
        float alpha = 1.0f;
    };
    std::vector<SoundCaptionDraw> sound_captions_;
    float subtitle_top_ = 1.0e30f;
    std::string logged_sound_captions_;
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
public:
    struct SubliminalStringDraw {
        uint32_t texture = 0;
        glm::vec2 position{0.0f};
        float value = 0.0f;
        float ink = 0.1f;
    };
    SubliminalStringDraw SubliminalString() const;
private:
    uint32_t noise_texture_ = 0;
    uint32_t noise_normal_texture_ = 0;
    UiCanvas canvas_;
    bool canvas_ready_ = false;
    bool pc_request_ = false;
    bool fps_shown_ = false;
    int fps_rendered_ = 0;
    int fps_presented_ = 0;
    bool update_question_ = false;
    float letterbox_ = 0.0f;
    int subtitle_size_ = 0;
    int subtitle_bg_ = 0;
    bool reduced_flashing_ = false;
    bool vr_hud_ = false;
    bool top_pending_ = false;
    std::string save_dialog_text_;
    bool menu_suspended_ = false;
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
    PromptStyle prompts_;
    float port_credits_time_ = -1.0f;
    float ending_skip_alpha_ = 0.0f;
    float ending_skip_progress_ = 0.0f;
    struct SpeedrunView {
        bool shown = false;
        float alpha = 1.0f;
        std::string total;
        std::string segment_name;
        std::string segment_time;
        std::string delta;
        bool ahead = false;
    } speedrun_view_;
    SurvivalHud survival_hud_;
    CrosshairStyle crosshair_;
    uint32_t survival_pulse_ = 0;
    bool survival_choosing_ = false;
    int offer_hover_ = -1;
    glm::vec2 offer_pointer_{-1.0f};
    void UpdateSurvivalOffer(Game& game, MenuInput& menu_input);
    bool survival_free_view_ = false;
    uint32_t survival_omen_seen_ = 0;
    uint32_t survival_glitch_seen_ = 0;
    int survival_dark_seen_ = 0;
    bool accuse_primed_ = false;
    uint32_t survival_accuse_seen_ = 0;
    uint32_t accuse_texture_ = 0;
    std::string accuse_words_;
    float accuse_time_ = -1.0f;
    static constexpr float kAccuseLength = 0.55f;
    static constexpr glm::ivec2 kAccuseStrip{2048, 320};
    static constexpr glm::ivec2 kBoonStrip{1024, 128};
    void DrawAccusation(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    uint32_t notice_texture_ = 0;
    std::unordered_map<std::string, uint32_t> notice_textures_;
    std::string notice_chars_;
    int notice_language_ = -1;
    const std::string& NoticeCharacters();
    uint32_t NoticeString(const std::string& words, int width = 1024, int height = 512, float em = 36.0f);
    void DrawVision(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    uint32_t vision_ring_ = 0;
    std::string vision_dir_;
    uint32_t vision_static_ = 0;
    float band_glitch_ = 0.0f;
    uint32_t band_seed_ = 0;
    void DrawBands(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full);
    uint32_t StaticTexture();
    NoticeQueue notices_;
    bool update_notice_given_ = false;
    float speedrun_bottom_ = 0.0f;
    bool update_notice_held_ = false;
    LisaDate* lisa_date_ = nullptr;
    float lisa_vignette_ = 0.0f;
    enum class Support { None, Menu, Notice, Credits };
    Support support_place_ = Support::None;
    bool support_prompts_ = true;
    glm::vec4 support_rect_{0.0f};
    Support support_rect_place_ = Support::None;
    bool support_hover_ = false;
    std::string support_request_;
    bool support_script_press_ = false;
};

}
