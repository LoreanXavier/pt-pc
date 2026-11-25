#include "engine/core/resource_path.h"
#include "engine/platform/os.h"
#include "engine/core/ascii.h"
#include "game/ui/game_ui.h"
#include "game/lisa_date/lisa_date.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>

#include "engine/core/log.h"
#include "engine/fs/vfs.h"
#include "engine/render/renderer.h"
#include "engine/render/texture_manager.h"
#include "engine/ui/text_layout.h"
#include "game/game.h"
#include "game/survival/survival_visions.h"
#include "game/ui/pc_settings.h"
#include "game/ui/port_credits.h"
#include "game/ui/prompt_hint.h"

namespace pt::game {
namespace {

constexpr float kSaveIconDelay = 3.0f / 30.0f;

constexpr float kOfferCardW = 300.0f, kOfferCardH = 196.0f, kOfferCardGap = 26.0f, kOfferCardTop = 290.0f;

struct WeaponPiece {
    float x[4];
    float y[4];
};
constexpr WeaponPiece Box(float x0, float y0, float x1, float y1) { return {{x0, x1, x1, x0}, {y0, y0, y1, y1}}; }
struct WeaponPicture {
    float width;
    int count;
    WeaponPiece pieces[8];
};
constexpr WeaponPicture kWeaponPictures[] = {
    {30.0f, 3, {{{0.0f, 9.0f, 9.0f, 0.0f}, {3.5f, 6.0f, 13.0f, 15.5f}}, Box(9.0f, 6.0f, 27.0f, 13.0f), Box(27.0f, 7.5f, 30.0f, 11.5f)}},
    {34.0f, 5, {Box(0.0f, 4.0f, 30.0f, 9.0f), Box(8.0f, 9.0f, 24.0f, 11.0f), {{21.0f, 30.0f, 33.0f, 25.0f}, {9.0f, 9.0f, 20.0f, 20.0f}},
                Box(13.0f, 11.0f, 14.0f, 14.5f), Box(13.0f, 14.0f, 21.0f, 15.0f)}},
    {64.0f, 6, {Box(0.0f, 5.0f, 42.0f, 7.5f), Box(4.0f, 8.5f, 34.0f, 10.5f), Box(16.0f, 7.5f, 29.0f, 12.5f), Box(38.0f, 4.0f, 50.0f, 11.5f),
                {{50.0f, 64.0f, 64.0f, 50.0f}, {5.0f, 8.0f, 16.0f, 11.5f}}, Box(41.0f, 11.5f, 47.0f, 13.5f)}},
    {42.0f, 8, {Box(0.0f, 5.0f, 22.0f, 8.0f), Box(7.0f, 8.0f, 22.0f, 10.0f), Box(22.0f, 3.5f, 31.0f, 12.0f), Box(31.0f, 5.0f, 36.0f, 9.5f),
                {{34.0f, 35.0f, 38.0f, 36.5f}, {5.0f, 1.5f, 2.5f, 5.0f}}, {{30.0f, 37.0f, 42.0f, 35.0f}, {10.0f, 9.5f, 20.0f, 20.0f}},
                Box(24.0f, 12.0f, 25.0f, 15.0f), Box(24.0f, 14.5f, 31.0f, 15.5f)}},
    {66.0f, 8, {Box(0.0f, 5.0f, 40.0f, 7.0f), Box(6.0f, 8.0f, 36.0f, 9.5f), Box(27.0f, 7.0f, 40.0f, 11.0f), Box(40.0f, 4.0f, 50.0f, 11.5f),
                Box(42.0f, 11.5f, 43.0f, 15.5f), Box(42.0f, 14.5f, 50.0f, 15.5f), Box(49.0f, 11.5f, 50.0f, 15.5f),
                {{50.0f, 66.0f, 66.0f, 50.0f}, {5.0f, 8.5f, 16.0f, 11.5f}}}},
    {64.0f, 6, {Box(0.0f, 4.0f, 40.0f, 6.3f), Box(0.0f, 7.2f, 40.0f, 9.5f), Box(26.0f, 9.5f, 40.0f, 12.0f), Box(40.0f, 4.0f, 48.0f, 11.5f),
                {{48.0f, 64.0f, 64.0f, 48.0f}, {5.0f, 8.0f, 16.0f, 11.5f}}, Box(41.0f, 11.5f, 46.0f, 13.5f)}},
};

GameUi* g_active = nullptr;

constexpr const char* kSubtitleModel = "/Assets/sh/ui/ModelAsset/sys_subtitle/Scenes/UI_sys_subtitle.uif";
constexpr uint16_t kSubtitleTextNode = 2;
constexpr const char* kSubtitleFont = "sbt-sys-M";

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(pt::AsciiLower(c)); });
    return out;
}

ui::TextAlign Align(int value) {
    return value == 0 ? ui::TextAlign::Start : value == 2 ? ui::TextAlign::End : ui::TextAlign::Center;
}

}

GameUi::GameUi() = default;

GameUi::~GameUi() {
    if (g_active == this) {
        g_active = nullptr;
    }
}

GameUi* GameUi::Active() {
    return g_active;
}

bool GameUi::Init(Renderer& renderer, TextureManager& textures, Vfs& vfs) {
    renderer_ = &renderer;
    batch_ = std::make_unique<ui::UiBatch>();
    if (!batch_->Init(renderer, textures)) {
        LogError("ui: 2D pipeline setup failed");
        batch_.reset();
        return false;
    }
    if (!assets_.Init(vfs, textures)) {
        LogError("ui: UI data missing");
        return false;
    }
    subtitles_.Init(vfs);
    subtitles_.SetLanguage(0);
    other_subtitles_.Init(vfs);
    if (!menu_.Init(assets_)) {
        LogWarn("ui: option menu model missing");
    }
    demo_ui_.Init(assets_);
    other_demo_ui_.Init(assets_);
    save_icon_.Init(assets_);
    assets_.Model(kSubtitleModel);
    for (int i = 0; i < SubliminalEffect::kImageCount; ++i) {
        subliminal_textures_[i] = assets_.Texture(std::format("/Assets/sh/effect/vfx_pic/text/text_sub{:02}_alp.ftex", i + 1));
    }
    noise_texture_ = assets_.Texture("/Assets/sh/effect/vfx_pic/noise/Noise_00007.ftex");
    noise_normal_texture_ = assets_.Texture("/Assets/sh/effect/vfx_pic/noise/Noise_00007_nrm.ftex");
    ready_ = true;
    g_active = this;
    LogInfo("ui: game UI ready");
    return true;
}

bool GameUi::ScriptCommand(Game& game, std::string_view op, std::string_view text, std::span<const float> args) {
    auto arg = [&](size_t i, float fallback = 0.0f) { return i < args.size() ? args[i] : fallback; };
    GameUi* ui = Active();
    ScreenEffects& fx = game.Effects();
    if (op == "menu") {
        if (ui && !ui->MenuOpen()) {
            ui->OpenMenu(game, false, text == "pc" ? OptionsMenu::Page::Pc : OptionsMenu::Page::Original);
        }
    } else if (op == "pcset") {
        if (!ui || !ui->menu_.PcPageForScript().SetValue(text, static_cast<int>(arg(0)))) {
            LogWarn("input script: sui pcset {} found no PC settings row", text);
        }
    } else if (op == "pcfocus") {
        if (!ui || !ui->menu_.PcPageForScript().Focus(text)) {
            LogWarn("input script: sui pcfocus {} found no PC settings row", text);
        }
    } else if (op == "menukey") {
        MenuInput input;
        input.subtitle_up = text == "up";
        input.subtitle_down = text == "down";
        input.brightness_down = text == "left";
        input.brightness_up = text == "right";
        input.held_dirs = text == "up" ? kRawUp : text == "down" ? kRawDown : text == "left" ? kRawLeft : text == "right" ? kRawRight : 0u;
        input.accept = text == "accept";
        input.back = text == "back";
        input.open_pc = text == "pc";
        input.toggle_vertical = text == "vertical";
        input.toggle_horizontal = text == "horizontal";
        input.zoom_pressed = text == "zoom";
        input.zoom_released = text == "unzoom";
        input.close = text == "close";
        input.tab_prev = text == "tabprev";
        input.tab_next = text == "tabnext";
        input.donate = text == "donate";
        input.update = text == "update";
        input.crosshair = text == "crosshair";
        input.survival_promo = text == "survivalpromo";
        input.armory_tab = text == "armorytab";
        input.armory_replay = text == "armoryreplay";
        input.armory_apply_all = text == "armoryall";
        input.music = text == "musicback" ? kMenuMusicBack : text == "musicforward" ? kMenuMusicForward : text == "musicrestart" ? kMenuMusicRestart
                    : text == "musicloop" ? kMenuMusicLoop : 0u;
        if (ui) {
            ui->QueueMenuInput(input);
        }
    } else if (op == "support") {
        if (ui) ui->support_script_press_ = true;
    } else if (op == "notice") {
        if (ui) {
            ui->ShowNotice(args.empty() ? std::string(text) : std::format("{}{}{}", text, kPcNoteArgument, static_cast<int>(arg(0))));
        }
    } else if (op == "streetlight") {
        game.ToggleStreetFlashlight();
    } else if (op == "menuwheel") {
        MenuInput input;
        input.wheel = arg(0);
        if (ui) {
            ui->QueueMenuInput(input);
        }
    } else if (op == "menuclick" || op == "menupoint") {
        MenuInput input;
        input.pointer_valid = true;
        input.pointer = glm::vec2(arg(0), arg(1));
        input.click = op == "menuclick";
        if (ui) {
            ui->QueueMenuInput(input);
        }
    } else if (op == "subtitle") {
        if (ui) {
            ui->ShowSubtitle(text, arg(0));
        }
    } else if (op == "caption") {
        static const std::pair<std::string_view, uint32_t> kCaptions[] = {
            {"xmark", 0x4AF4F3D4u}, {"hello", 0x4FA7C4BDu}, {"hole", 0xF124B2D3u}, {"photo", 0xCE86DEBAu}, {"ending", 0x7D33E7DFu}, {"teaser", 0x99242989u}, {"preface", 0xD62FBBDAu}};
        for (const auto& [name, id] : kCaptions) {
            if (text == name) {
                game.ShowCaption(id);
            }
        }
    } else if (op == "subliminal") {
        fx.ShowSubliminalImage(static_cast<int>(arg(0)), arg(1) != 0.0f, arg(2) != 0.0f);
    } else if (op == "overlay") {
        fx.overlay_texture = std::string(text);
    } else if (op == "fade") {
        if (text == "in") {
            fx.CallFadeIn(arg(0, 1.0f));
        } else if (text == "strong") {
            fx.CallStrongFadeOut(arg(0, 1.0f));
        } else {
            fx.CallFadeOut(arg(0, 1.0f));
        }
    } else if (op == "fadecolor") {
        fx.SetFadeColor(static_cast<int>(arg(0)), static_cast<int>(arg(1)), static_cast<int>(arg(2)), static_cast<int>(arg(3, 255.0f)));
    } else if (op == "subs") {
        game.Options().subtitles = arg(0) != 0.0f;
        if (args.size() > 1) {
            game.Options().subtitle_language = static_cast<int>(arg(1));
        }
    } else if (op == "brightness") {
        game.Options().brightness = std::clamp(static_cast<int>(arg(0)), 0, 10);
    } else if (op == "photoactive") {
        game.Nazo().Activate(NazoId::Photo);
    } else if (op == "demoui") {
        constexpr uint64_t kTestGraph = 1;
        if (!ui) {
        } else if (text.ends_with(".fpk") || text.ends_with(".fpkd")) {
            ui->assets_.Files().LoadPackage(text);
        } else if (text.starts_with("/")) {
            ui->demo_ui_.Create("", kTestGraph, text);
        } else if (text == "start") {
            ui->demo_ui_.Start(kTestGraph);
        } else if (text == "clear") {
            ui->demo_ui_.Clear();
        } else {
            ui->demo_ui_.Text(kTestGraph, std::strtoull(std::string(text).c_str(), nullptr, 16));
        }
    } else {
        return false;
    }
    LogInfo("ui script: {} {}", op, text);
    return true;
}

void GameUi::Shutdown() {
    if (batch_) {
        batch_->Shutdown();
        batch_.reset();
    }
    ready_ = false;
    if (g_active == this) {
        g_active = nullptr;
    }
}

void GameUi::ShowSubtitle(std::string_view subtitle_id, float start_offset_seconds) {
    queued_subtitles_.emplace_back(std::string(subtitle_id), start_offset_seconds);
}

void GameUi::ClearSubtitles() {
    subtitles_.Clear();
    queued_subtitles_.clear();
    last_caption_ = 0;
    last_caption_time_ = 0.0f;
}

void GameUi::OpenPcSettings() {
    pc_request_ = true;
}

void GameUi::QueueMenuInput(const MenuInput& input) {
    MenuInput& q = queued_menu_input_;
    q.subtitle_up |= input.subtitle_up;
    q.subtitle_down |= input.subtitle_down;
    q.brightness_down |= input.brightness_down;
    q.brightness_up |= input.brightness_up;
    q.toggle_vertical |= input.toggle_vertical;
    q.toggle_horizontal |= input.toggle_horizontal;
    q.zoom_pressed |= input.zoom_pressed;
    q.zoom_released |= input.zoom_released;
    q.close |= input.close;
    q.accept |= input.accept;
    q.back |= input.back;
    q.open_pc |= input.open_pc;
    q.switch_column |= input.switch_column;
    q.tab_prev |= input.tab_prev;
    q.tab_next |= input.tab_next;
    q.tab_from_e |= input.tab_from_e;
    q.donate |= input.donate;
    q.update |= input.update;
    q.street_return |= input.street_return;
    q.crosshair |= input.crosshair;
    q.survival_promo |= input.survival_promo;
    q.held_dirs |= input.held_dirs;
    q.music |= input.music;
    q.click |= input.click;
    q.pointer_held |= input.pointer_held;
    q.right_click |= input.right_click;
    q.wheel += input.wheel;
    q.armory_tab |= input.armory_tab;
    q.armory_replay |= input.armory_replay;
    q.armory_apply_all |= input.armory_apply_all;
    if (std::abs(input.right_stick.x) > std::abs(q.right_stick.x)) q.right_stick.x = input.right_stick.x;
    if (std::abs(input.right_stick.y) > std::abs(q.right_stick.y)) q.right_stick.y = input.right_stick.y;
    if (input.pointer_valid) {
        q.pointer_valid = true;
        q.pointer = input.pointer;
    }
}

void GameUi::OpenMenu(Game& game, bool first_boot, OptionsMenu::Page page) {
    if (menu_.IsOpen()) {
        return;
    }
    menu_.Show(game, first_boot, page);
    if (game.Audio()) {
        game.Audio()->PostEvent("Pause_All", nullptr);
    }
    if (!game.Paused()) {
        game.SetPaused(true);
        pause_owner_ = true;
    }
}

uint32_t GameUi::OverlayTexture(const std::string& name) {
    auto it = overlay_cache_.find(name);
    if (it != overlay_cache_.end()) {
        return it->second;
    }
    std::string path;
    const std::string suffix = "/" + name + ".ftex";
    for (const auto& package : assets_.Files().LoadedPackages()) {
        for (const auto& entry : package->Entries()) {
            if (entry.path.ends_with(suffix)) {
                path = entry.path;
                break;
            }
        }
        if (!path.empty()) {
            break;
        }
    }
    if (path.empty()) {
        path = "/Assets/sh/effect/vfx_pic/holl" + suffix;
    }
    const uint32_t index = assets_.Texture(path);
    overlay_cache_[name] = index;
    return index;
}

void GameUi::UpdateSubliminal(Game& game, float dt, bool paused) {
    ScreenEffects& fx = game.Effects();
    const bool trigger = fx.subliminal_image >= 0 && (last_subliminal_ < 0 || fx.subliminal_time < last_subliminal_time_);
    if (trigger) {
        if (!rng_seeded_) {
            rng_.seed(ReadTsc());
            rng_seeded_ = true;
        }
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const glm::vec2 position(unit(rng_), unit(rng_));
        subliminal_.Trigger(fx.subliminal_image, fx.subliminal_flag, fx.subliminal_no_string, position);
        game.NoteArchive("noise");
        if (!fx.subliminal_no_string) game.NoteArchive(std::format("sub:{}", fx.subliminal_image + 1));
        LogInfo("ui: subliminal image {} flag {} no string {} at ({:.2f} {:.2f})", fx.subliminal_image, fx.subliminal_flag, fx.subliminal_no_string,
                position.x, position.y);
    }
    last_subliminal_ = fx.subliminal_image;
    last_subliminal_time_ = fx.subliminal_time;
    if (!paused) {
        subliminal_.Update(dt);
    }
}

void GameUi::ShowBootSplash(uint32_t texture, float seconds) {
    boot_splash_ = texture;
    boot_splash_time_ = seconds;
}

void GameUi::Update(Game& game, const InputState& input, float dt) {
    if (!ready_) {
        return;
    }
    boot_splash_time_ = std::max(0.0f, boot_splash_time_ - std::min(dt, 0.1f));
    if (game.TakeSubtitleClear()) {
        ClearSubtitles();
    }
    ScreenEffects& fx = game.Effects();
    language_ = std::clamp(game.Options().subtitle_language, 0, SubtitlePlayer::kLanguageCount - 1);
    if (language_ != subtitles_.Language()) {
        subtitles_.SetLanguage(language_);
    }
    for (const Game::SubtitleRequest& request : game.TakeSubtitleRequests()) {
        subtitles_.Play(request.subtitle_id, request.offset_seconds, std::nullopt, request.sound);
    }
    if (GameAudio* audio = game.Audio()) {
        subtitles_.EndStopped([&](uint32_t sound) { return audio->IsPlaying(sound); });
    }
    for (const auto& [id, offset] : queued_subtitles_) {
        subtitles_.Play(id, offset);
    }
    queued_subtitles_.clear();
    const bool caption_start = fx.caption_id != 0 && (fx.caption_id != last_caption_ || fx.caption_time < last_caption_time_);
    const bool caption_clock = caption_start || (fx.caption_id != 0 && std::abs(fx.caption_time - last_caption_time_ - dt) > 0.001f);
    if (caption_start) {
        subtitles_.PlayKey(fx.caption_id, fx.caption_time);
    }
    last_caption_ = fx.caption_id;
    last_caption_time_ = fx.caption_time;

    menu_.SetPromptStyle(input.prompts);
    prompts_ = input.prompts;
    MenuInput menu_input = MapMenuInput(input, previous_held_, previous_dirs_);
    previous_held_ = input.held;
    previous_dirs_ = menu_input.held_dirs;
    if (input.pointer_valid && canvas_ready_) {
        const glm::vec2 virtual_px = (input.pointer - canvas_.origin) / canvas_.scale;
        menu_input.pointer_valid = true;
        menu_input.pointer = glm::vec2(virtual_px.x - UiCanvas::kWidth * 0.5f, UiCanvas::kHeight * 0.5f - virtual_px.y) / UiCanvas::kUnit;
    }
    QueueMenuInput(menu_input);
    menu_input = std::exchange(queued_menu_input_, MenuInput{});
    save_dialog_text_.clear();
    dialog_flash_ += dt * 60.0f;
    if (const auto& dialog = game.PendingSaveDialog()) {
        const bool pc_text = dialog->key.starts_with("pc_");
        if (pc_text) {
            save_dialog_text_ = std::string(PcText(dialog->key, language_));
        } else if (const ui::LangFile* system = assets_.System(language_)) {
            save_dialog_text_ = system->Text(dialog->key);
        }
        if (save_dialog_text_.empty()) {
            save_dialog_text_ = dialog->key;
        }
        if (!save_nav_open_) {
            save_nav_open_ = true;
            save_nav_.Open(0, menu_input.held_dirs);
            dialog_flash_ = 0.0f;
        } else {
            MenuInput answer = menu_input;
            answer.accept = answer.accept || input.confirm;
            if (save_nav_.Update(answer, 1, language_, pc_text) != dialog::Result::None) {
                game.CloseSaveDialog();
                save_dialog_text_.clear();
            }
        }
        menu_input = MenuInput{};
    } else {
        save_nav_open_ = false;
    }
    port_credits_time_ = game.PortCreditsActive() && game.Controller().Step() == 33 ? game.PortCreditsTime() : -1.0f;
    ending_skip_alpha_ = game.EndingSkipAlpha();
    ending_skip_progress_ = std::clamp(game.EndingSkipProgress(), 0.0f, 1.0f);
    UpdateSupport(game, input, menu_input);
    UpdateSurvivalOffer(game, menu_input);
    if (port_credits_time_ >= 0.0f && !menu_.IsOpen()) {
        if (port_credits_time_ > 0.5f && (menu_input.accept || menu_input.back || menu_input.close || menu_input.click)) {
            game.SkipPortCredits();
        }
        menu_input = MenuInput{};
        pc_request_ = false;
    }
    const bool pc_request = std::exchange(pc_request_, false);
    if (menu_suspended_) {
    } else if (!menu_.IsOpen()) {
        if (game.Controller().OptionMenuOpen()) {
            OpenMenu(game, true);
        } else if (port_credits_time_ >= 0.0f) {
        } else if (game.StreetOfferPending()) {
            OpenMenu(game, false, OptionsMenu::Page::Pc);
        } else if (std::exchange(update_question_, false)) {
            OpenMenu(game, false, OptionsMenu::Page::Pc);
        } else if (game.TakeSurvivalHubRequest() && menu_.OpenSurvivalNightFromDeath()) {
            OpenMenu(game, false, OptionsMenu::Page::Pc);
        } else if ((menu_input.close || pc_request) && !game.Status().IsSet("S_DISABLE_GAME_PAUSE")) {
            OpenMenu(game, false, pc_request ? OptionsMenu::Page::Pc : OptionsMenu::Page::Original);
        }
    } else {
        if (pc_request) {
            menu_input.open_pc = true;
        }
        if (game.TakeMenuCloseRequest()) {
            menu_.Close();
        }
        menu_.Update(game, menu_input, dt);
        if (menu_.TakeResume()) {
            if (menu_.FirstBoot() || game.Controller().OptionMenuOpen()) {
                game.Controller().CloseOptionMenu();
            }
            if (pause_owner_) {
                game.SetPaused(false);
                pause_owner_ = false;
            }
            if (menu_.TakeStreetReturn()) {
                if (game.LisaDateActive()) {
                    game.RequestLeaveLisaDate();
                } else {
                    game.LeaveStreetWalk();
                }
            }
        }
        if (menu_.TakeClosed()) {
            if (game.Audio()) {
                game.Audio()->PostEvent("Resume_All", nullptr);
            }
            game.RequestOptionsSave();
        }
    }
    if (menu_suspended_) {
        save_io_seen_ = game.SaveIoCount();
    }
    if (game.SaveIoCount() != save_io_seen_) {
        save_io_seen_ = game.SaveIoCount();
        save_icon_wait_ = kSaveIconDelay;
        save_icon_loading_ = game.SaveIoLoading();
    }
    if (save_icon_wait_ > 0.0f) {
        save_icon_wait_ -= dt;
        if (save_icon_wait_ <= 1e-4f) {
            save_icon_wait_ = 0.0f;
            save_icon_.Start(save_icon_loading_);
        }
    }
    save_icon_.Update(dt);
    const bool paused = game.Paused();
    lisa_vignette_ = game.LisaDateActive() ? 0.0f : game.LisaHintVignette();
    sound_captions_.clear();
    if (SoundCaptions& captions = game.Captions(); captions.Enabled()) {
        if (!paused) {
            captions.Update(dt, game.GetCamera().position, game.GetCamera().Forward());
        }
        for (const SoundCaptions::Line& line : captions.Lines()) {
            sound_captions_.push_back({line.rule->key, line.side, line.Alpha()});
        }
    }
    if (!paused) {
        subtitles_.Update(dt);
        if (caption_clock) {
            subtitles_.Seek(fx.caption_id, fx.caption_time);
        }
    }
    UpdateSubliminal(game, dt, paused);
    demo_ui_.Update(game, dt, paused);
    UpdateSpeedrun(game);
    static const bool survival_hud = [] {
        const char* value = std::getenv("PT_SURVIVAL_HUD");
        return !value || std::atoi(value) != 0;
    }();
    survival_hud_ = game.SurvivalActive() && survival_hud ? game.Survival()->Hud() : SurvivalHud{};
    if (!game.SurvivalActive()) {
        survival_omen_seen_ = 0;
        survival_glitch_seen_ = 0;
        survival_dark_seen_ = 0;
        accuse_primed_ = false;
        accuse_time_ = -1.0f;
    } else {
        const SurvivalHud& night = game.Survival()->Hud();
        if (night.notice_serial != survival_omen_seen_) {
            survival_omen_seen_ = night.notice_serial;
            if (night.notice_serial != 0 && !night.notice_key.empty()) {
                std::string words(PcText(night.notice_key, language_));
                if (!night.notice_extra.empty()) words += "  " + night.notice_extra;
                notice_texture_ = NoticeString(words);
                subliminal_.Trigger(notice_texture_ ? SubliminalEffect::kPortImage : 0, true, notice_texture_ == 0, glm::vec2(0.0f), night.notice_hold);
            }
        }
        if (night.dark_count != survival_dark_seen_) {
            const bool first = survival_dark_seen_ == 0;
            survival_dark_seen_ = night.dark_count;
            if (night.dark_count > 0) {
                notice_texture_ = NoticeString(std::string(PcText("pc_survival_dark", language_)) + "  " + std::to_string(night.dark_count));
                if (first || !notice_texture_) {
                    subliminal_.Trigger(notice_texture_ ? SubliminalEffect::kPortImage : 0, true, notice_texture_ == 0, glm::vec2(0.0f), 1.05f);
                } else {
                    subliminal_.Hold(1.05f);
                }
            }
        }
        if (night.glitch_serial != survival_glitch_seen_) {
            if (survival_glitch_seen_ != 0 && !paused) {
                band_glitch_ = 0.2f;
                band_seed_ = night.glitch_serial;
            }
            survival_glitch_seen_ = night.glitch_serial;
        }
        if (!paused) band_glitch_ = std::max(0.0f, band_glitch_ - dt);
        if (!accuse_primed_) {
            survival_accuse_seen_ = night.accuse_serial;
            accuse_primed_ = true;
        } else if (night.accuse_serial != survival_accuse_seen_) {
            survival_accuse_seen_ = night.accuse_serial;
            if (!night.accuse_key.empty()) {
                accuse_words_ = std::string(PcText(night.accuse_key, language_));
                accuse_texture_ = NoticeString(accuse_words_, kAccuseStrip.x, kAccuseStrip.y, 72.0f);
                accuse_time_ = 0.0f;
                band_glitch_ = 0.2f;
                band_seed_ = night.accuse_serial * 7919u;
            }
        }
        if (accuse_time_ >= 0.0f && !paused) {
            accuse_time_ += dt;
            if (accuse_time_ > kAccuseLength) accuse_time_ = -1.0f;
        }
    }
    survival_free_view_ = game.FreeView();
    crosshair_ = game.Crosshair();
    UpdateUpdateNotice(game, dt);

    subtitles_on_ = game.Options().subtitles;
    strong_subtitles_ = fx.subtitles_enabled;
    std::string hidden = fx.subtitles_visible ? std::string() : Lower(fx.subtitle_message_id);
    if (hidden != hidden_subtitle_) {
        LogInfo("ui: subtitle area control {}", hidden.empty() ? std::format("shows {}", hidden_subtitle_) : std::format("hides {}", hidden));
        hidden_subtitle_ = std::move(hidden);
    }
    subtitles_.SetListener(game.GetCamera().position);
    fade_ = fx.FadeShown();
    fade_priority_ = fx.fade_strong ? fx.fade_strong_priority : fx.fade_priority;
    overlay_ = !fx.overlay_texture.empty();
    if (overlay_) game.NoteArchive("overlay");
    if (overlay_ && fx.overlay_texture != overlay_name_) {
        overlay_name_ = fx.overlay_texture;
        overlay_texture_ = OverlayTexture(overlay_name_);
    }
}

void GameUi::DrawSubtitle(ui::UiBatch& batch, const UiCanvas& canvas) {
    subtitle_top_ = 1.0e30f;
    SubtitleView view;
    const bool shown = subtitles_on_ && subtitles_.Current(hidden_subtitle_, view);
    const std::string on_screen = shown ? view.id + " " + view.text : std::string();
    if (on_screen != logged_subtitle_) {
        logged_subtitle_ = on_screen;
        LogInfo("ui: subtitle on screen: {}", on_screen.empty() ? std::string("none") : view.id);
    }
    if (!shown) {
        return;
    }
    const int language = subtitles_.Language();
    UiFont* font = assets_.Font(UiFontType::Movie, language);
    if (!font) {
        return;
    }
    const SubtitleGeneratorSettings& generator = assets_.Generator();
    glm::vec2 size(1024.0f, 100.0f);
    glm::vec2 box_min(-0.05f, 0.0f);
    glm::vec2 box_max(0.05f, -0.1f);
    glm::vec2 origin(0.0f);
    glm::vec4 color(1.0f);
    const UiFontStyle* style = assets_.FontStyle(kSubtitleFont, language);
    if (const ui::UifModel* model = assets_.Model(kSubtitleModel)) {
        if (const ui::UifNode* node = model->FindById(kSubtitleTextNode)) {
            size = node->size;
            box_min = glm::vec2(node->box_min);
            box_max = glm::vec2(node->box_max);
            origin = glm::vec2(node->translate);
            color = node->color;
            if (const UiFontStyle* node_style = assets_.FontStyleByHash(node->font_name, language)) {
                style = node_style;
            }
        }
    }
    ui::TextStyle text_style;
    text_style.font = &font->font;
    if (style) {
        text_style.font_width = style->width;
        text_style.font_height = style->height;
        text_style.text_space = style->text_space;
        text_style.line_space = style->line_space;
    }
    text_style.text_space += generator.font_space;
    text_style.line_space += generator.line_space;
    if (subtitle_size_ > 0) {
        const float mul = subtitle_size_ == 1 ? 1.22f : 1.45f;
        text_style.font_width *= mul;
        text_style.font_height *= mul;
        text_style.line_space *= mul;
    }
    ui::NormalizeSubtitleStyle(text_style);
    const glm::vec2 anchor = origin + generator.offset;
    const glm::vec2 lo_units = anchor + glm::vec2(box_min.x * size.x, box_min.y * size.y);
    const glm::vec2 hi_units = anchor + glm::vec2(box_max.x * size.x, box_max.y * size.y);
    const glm::vec2 lo(UiCanvas::kWidth * 0.5f + std::min(lo_units.x, hi_units.x) * UiCanvas::kUnit,
                       UiCanvas::kHeight * 0.5f - std::max(lo_units.y, hi_units.y) * UiCanvas::kUnit);
    const glm::vec2 hi(UiCanvas::kWidth * 0.5f + std::max(lo_units.x, hi_units.x) * UiCanvas::kUnit,
                       UiCanvas::kHeight * 0.5f - std::min(lo_units.y, hi_units.y) * UiCanvas::kUnit);
    ui::TextLayout layout = ui::LayoutText(view.text, text_style, generator.auto_line_feed ? hi.x - lo.x : 0.0f);
    const ui::TextAlign block = generator.b_align == 3 ? Align(generator.h_align) : Align(generator.b_align);
    ui::PlaceText(layout, lo, hi, block, Align(generator.h_align), Align(generator.v_align), true);
    const glm::vec2 full = canvas.extent;
    if (const float bar = LetterboxBar(full, letterbox_); bar > 0.0f && canvas.scale > 0.0f) {
        float bottom = -1e30f;
        for (const ui::TextLine& line : layout.lines) {
            for (const ui::LaidGlyph& g : line.glyphs) bottom = std::max(bottom, g.position.y + g.size.y);
        }
        const float frame_bottom = (full.y - canvas.origin.y) / canvas.scale;
        const float margin = std::max(0.0f, frame_bottom - hi.y);
        const float limit = (full.y - bar - canvas.origin.y) / canvas.scale - margin;
        if (bottom > limit) {
            for (ui::TextLine& line : layout.lines) {
                for (ui::LaidGlyph& g : line.glyphs) g.position.y -= bottom - limit;
            }
        }
    }
    glm::vec2 sub_lo(1e30f), sub_hi(-1e30f);
    for (const ui::TextLine& line : layout.lines) {
        for (const ui::LaidGlyph& g : line.glyphs) {
            subtitle_top_ = std::min(subtitle_top_, g.position.y);
            sub_lo = glm::min(sub_lo, g.position);
            sub_hi = glm::max(sub_hi, g.position + g.size);
        }
    }
    if (subtitle_bg_ > 0 && sub_lo.x < sub_hi.x && sub_lo.y < sub_hi.y) {
        const float plate_alpha = subtitle_bg_ == 1 ? 0.48f : 0.78f;
        const glm::vec2 pad(12.0f, 6.0f);
        batch.Quad(canvas.ToTarget(sub_lo - pad), canvas.ToTarget(sub_hi + pad), {0.0f, 0.0f}, {1.0f, 1.0f},
                   glm::vec4(0.0f, 0.0f, 0.0f, plate_alpha * generator.color.a), ui::UiDrawParams::Plain(TextureManager::kWhite),
                   ui::UiShade::Solid, ui::UiBlend::Alpha);
    }
    glm::vec4 subtitle_color = color * generator.color;
    ui::UiShade shade = ui::UiShade::Border;
    const float backdrop_luminance = glm::dot(glm::vec3(fade_), glm::vec3(0.2126f, 0.7152f, 0.0722f));
    if (strong_subtitles_ && fade_.a > 0.95f && backdrop_luminance > 0.65f) {
        subtitle_color = glm::vec4(glm::vec3(0.08f), subtitle_color.a);
        shade = ui::UiShade::Text;
    }
    DrawText(batch, canvas, *font, layout, subtitle_color, ui::UiBlend::Alpha, shade);
}

void GameUi::DrawSoundCaptions(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    UiFont* font = assets_.Font(UiFontType::Movie, language_);
    if (!font || sound_captions_.empty()) {
        return;
    }
    const SubtitleGeneratorSettings& generator = assets_.Generator();
    glm::vec2 size(1024.0f, 100.0f);
    glm::vec2 box_min(-0.05f, 0.0f);
    glm::vec2 box_max(0.05f, -0.1f);
    glm::vec2 origin(0.0f);
    const UiFontStyle* style = assets_.FontStyle(kSubtitleFont, language_);
    if (const ui::UifModel* model = assets_.Model(kSubtitleModel)) {
        if (const ui::UifNode* node = model->FindById(kSubtitleTextNode)) {
            size = node->size;
            box_min = glm::vec2(node->box_min);
            box_max = glm::vec2(node->box_max);
            origin = glm::vec2(node->translate);
            if (const UiFontStyle* node_style = assets_.FontStyleByHash(node->font_name, language_)) {
                style = node_style;
            }
        }
    }
    ui::TextStyle text_style;
    text_style.font = &font->font;
    if (style) {
        text_style.font_width = style->width;
        text_style.font_height = style->height;
        text_style.text_space = style->text_space;
        text_style.line_space = style->line_space;
    }
    text_style.text_space += generator.font_space;
    text_style.line_space += generator.line_space;
    if (subtitle_size_ > 0) {
        const float mul = subtitle_size_ == 1 ? 1.22f : 1.45f;
        text_style.font_width *= mul;
        text_style.font_height *= mul;
        text_style.line_space *= mul;
    }
    ui::NormalizeSubtitleStyle(text_style);
    constexpr float kScale = 0.85f;
    text_style.font_width *= kScale;
    text_style.font_height *= kScale;
    text_style.text_space *= kScale;
    text_style.line_space *= kScale;
    const glm::vec2 anchor = origin + generator.offset;
    const glm::vec2 lo_units = anchor + glm::vec2(box_min.x * size.x, box_min.y * size.y);
    const glm::vec2 hi_units = anchor + glm::vec2(box_max.x * size.x, box_max.y * size.y);
    const float left = UiCanvas::kWidth * 0.5f + std::min(lo_units.x, hi_units.x) * UiCanvas::kUnit;
    const float right = UiCanvas::kWidth * 0.5f + std::max(lo_units.x, hi_units.x) * UiCanvas::kUnit;
    const float box_top = UiCanvas::kHeight * 0.5f - std::max(lo_units.y, hi_units.y) * UiCanvas::kUnit;
    constexpr float kGap = 8.0f;
    float bottom = std::min(box_top, subtitle_top_) - kGap;
    if (const float bar = LetterboxBar(full, letterbox_); bar > 0.0f && canvas.scale > 0.0f) {
        const float frame_bottom = (full.y - canvas.origin.y) / canvas.scale;
        const float margin = std::max(0.0f, frame_bottom - (UiCanvas::kHeight * 0.5f - std::min(lo_units.y, hi_units.y) * UiCanvas::kUnit));
        bottom = std::min(bottom, (full.y - bar - canvas.origin.y) / canvas.scale - margin - kGap);
    }
    for (size_t i = sound_captions_.size(); i-- > 0;) {
        const SoundCaptionDraw& caption = sound_captions_[i];
        std::string text(PcText(caption.key, language_));
        if (caption.side != CaptionSide::None) {
            const char* side = caption.side == CaptionSide::Left ? "pc_cap_left" : caption.side == CaptionSide::Right ? "pc_cap_right"
                               : caption.side == CaptionSide::Behind ? "pc_cap_behind" : "pc_cap_ahead";
            text += ' ';
            text += PcText(side, language_);
        }
        ui::TextLayout layout = ui::LayoutText(text, text_style, generator.auto_line_feed ? right - left : 0.0f);
        ui::PlaceText(layout, {left, bottom - layout.height}, {right, bottom}, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::End, true);
        DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, std::clamp(caption.alpha, 0.0f, 1.0f)), ui::UiBlend::Alpha, ui::UiShade::Border);
        bottom -= layout.height + kGap;
    }
}

const std::string& GameUi::NoticeCharacters() {
    if (notice_language_ != language_) {
        static constexpr const char* kNoticeKeys[] = {
            "pc_survival_task_fuse", "pc_survival_task_radio", "pc_survival_task_fragments", "pc_survival_task_lamps", "pc_survival_task_door",
            "pc_survival_dark", "pc_survival_supply", "pc_survival_task_done", "pc_survival_task_lost", "pc_survival_omen_blackout", "pc_survival_omen_fog", "pc_survival_omen_siren", "pc_survival_omen_hell", "pc_survival_omen_flank",
            "pc_survival_omen_horde", "pc_survival_omen_drought", "pc_survival_omen_hunt", "pc_survival_boss_rage", "pc_survival_boss_last",
            "pc_survival_theme_armored", "pc_survival_theme_boss_dark", "pc_survival_theme_boss_final", "pc_survival_theme_breather",
            "pc_survival_theme_crawl", "pc_survival_theme_giants", "pc_survival_theme_mothers", "pc_survival_theme_perched",
            "pc_survival_theme_procession", "pc_survival_theme_quiet", "pc_survival_theme_screamer", "pc_survival_theme_shy",
            "pc_survival_theme_splitter", "pc_survival_toll_lamps", "pc_survival_toll_fog", "pc_survival_toll_door",
            "pc_survival_toll_lisa", "pc_survival_accuse_01", "pc_survival_accuse_02", "pc_survival_accuse_03", "pc_survival_accuse_04", 
            "pc_survival_accuse_05", "pc_survival_accuse_06", "pc_survival_accuse_07", "pc_survival_accuse_08", 
            "pc_survival_accuse_09", "pc_survival_accuse_10", "pc_survival_accuse_11", "pc_survival_accuse_12", 
            "pc_survival_accuse_13", "pc_survival_accuse_14", "pc_survival_accuse_15", "pc_survival_accuse_16", 
            "pc_survival_accuse_17", "pc_survival_accuse_18", "pc_survival_accuse_19", "pc_survival_accuse_20"};
        notice_chars_ = "0123456789 /";
        for (const char* key : kNoticeKeys) notice_chars_ += PcText(key, language_);
        for (size_t b = 0; b < static_cast<size_t>(SurvivalBoon::Count); ++b) notice_chars_ += PcText(BoonSpec(static_cast<SurvivalBoon>(b)).name, language_);
        notice_language_ = language_;
    }
    return notice_chars_;
}

uint32_t GameUi::NoticeString(const std::string& words, int width, int height, float em) {
    UiFont* hand = assets_.HandFont(language_, NoticeCharacters());
    if (!hand || words.empty()) return 0;
    const std::string name = width == 1024 && height == 512 ? std::format("ui:notice:{}:{}", language_, words)
                                                           : std::format("ui:notice:{}x{}:{}:{}", width, height, language_, words);
    if (auto it = notice_textures_.find(name); it != notice_textures_.end()) return it->second;
    const int kWidth = width, kHeight = height;
    ui::TextStyle style;
    style.font = &hand->font;
    ui::NormalizeSubtitleStyle(style);
    style.font_width = style.font_height = em;
    style.line_space = em / 6.0f;
    ui::TextLayout layout = ui::LayoutText(words, style, kWidth * 0.86f);
    ui::PlaceText(layout, {0.0f, 0.0f}, {static_cast<float>(kWidth), static_cast<float>(kHeight)}, ui::TextAlign::Center, ui::TextAlign::Center,
                  ui::TextAlign::Center, true);
    std::vector<uint8_t> pixels(size_t(kWidth) * kHeight * 4, 255);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0;
    const ui::FfntFont& font = hand->font;
    for (const ui::TextLine& line : layout.lines) {
        for (const ui::LaidGlyph& g : line.glyphs) {
            if (!g.glyph || g.size.x <= 0.0f || g.size.y <= 0.0f || g.glyph->width == 0 || g.glyph->height == 0) continue;
            const ui::FfntGlyph& glyph = *g.glyph;
            auto coverage = [&](float u, float v) {
                const int x = static_cast<int>(std::floor(u)), y = static_cast<int>(std::floor(v));
                const float fx = u - static_cast<float>(x), fy = v - static_cast<float>(y);
                const float top = glm::mix(static_cast<float>(font.Coverage(glyph, x, y)), static_cast<float>(font.Coverage(glyph, x + 1, y)), fx);
                const float low = glm::mix(static_cast<float>(font.Coverage(glyph, x, y + 1)), static_cast<float>(font.Coverage(glyph, x + 1, y + 1)), fx);
                return glm::mix(top, low, fy);
            };
            const int x0 = std::max(0, static_cast<int>(std::floor(g.position.x))), x1 = std::min(kWidth, static_cast<int>(std::ceil(g.position.x + g.size.x)));
            const int y0 = std::max(0, static_cast<int>(std::floor(g.position.y))), y1 = std::min(kHeight, static_cast<int>(std::ceil(g.position.y + g.size.y)));
            const float su = static_cast<float>(glyph.width) / g.size.x, sv = static_cast<float>(glyph.height) / g.size.y;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    float ink = 0.0f;
                    for (const float sy : {0.25f, 0.75f}) {
                        for (const float sx : {0.25f, 0.75f}) {
                            ink += coverage((static_cast<float>(x) + sx - g.position.x) * su - 0.5f, (static_cast<float>(y) + sy - g.position.y) * sv - 0.5f);
                        }
                    }
                    uint8_t& alpha = pixels[(size_t(y) * kWidth + size_t(x)) * 4 + 3];
                    alpha = std::max(alpha, static_cast<uint8_t>(std::clamp(ink * 0.25f, 0.0f, 255.0f)));
                }
            }
        }
    }
    const uint32_t texture = assets_.MakeTexture(name, kWidth, kHeight, pixels);
    notice_textures_[name] = texture;
    LogInfo("ui: the notice '{}' written as a subliminal string", words);
    return texture;
}

GameUi::SubliminalStringDraw GameUi::SubliminalString() const {
    const int image = subliminal_.Image();
    if (!ready_ || subliminal_.StringValue() <= 0.0f || image < 0 || image > SubliminalEffect::kPortImage) return {};
    const uint32_t texture = image == SubliminalEffect::kPortImage ? notice_texture_ : subliminal_textures_[image];
    if (!texture) return {};
    if (image == SubliminalEffect::kPortImage && survival_hud_.notice_vision && survival_hud_.notice_age >= 3.0f && survival_hud_.notice_age < 5.0f) {
        return {};
    }
    return {texture, subliminal_.Position(), subliminal_.StringValue(), image == SubliminalEffect::kPortImage ? 0.16f : 0.1f};
}

void GameUi::DrawSubliminal(ui::UiBatch& batch, bool string_pass) {
    const glm::vec2 extent(static_cast<float>(batch.Extent().width), static_cast<float>(batch.Extent().height));
    const float across = extent.y > 0.0f ? (extent.x / extent.y) / (UiCanvas::kWidth / UiCanvas::kHeight) : 1.0f;
    const glm::vec2 uv0(0.5f - 0.5f * across, 0.0f);
    const glm::vec2 uv1(0.5f + 0.5f * across, 1.0f);
    if (string_pass) {
        if (!vr_hud_) return;
        const int image = subliminal_.Image();
        if (subliminal_.StringValue() <= 0.0f || image < 0 || image > SubliminalEffect::kPortImage) {
            return;
        }
        const uint32_t texture = image == SubliminalEffect::kPortImage ? notice_texture_ : subliminal_textures_[image];
        if (!texture) return;
        ui::UiDrawParams params = ui::UiDrawParams::Plain(texture);
        params.extra = glm::vec4(subliminal_.Position(), 0.0f, subliminal_.StringValue());
        batch.Quad(glm::vec2(0.0f), extent, uv0, uv1, glm::vec4(1.0f), params, ui::UiShade::String, ui::UiBlend::Alpha);
        return;
    }
    if (subliminal_.NoiseA() <= 0.0f && subliminal_.NoiseB() <= 0.0f) {
        return;
    }
    ui::UiDrawParams params = ui::UiDrawParams::Plain(noise_texture_);
    params.textures[1] = noise_normal_texture_;
    params.extra = glm::vec4(subliminal_.Phase(), 0.0f, subliminal_.NoiseB(), subliminal_.NoiseA());
    params.extra2 = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    params.extra2.z = across;
    batch.Quad(glm::vec2(0.0f), extent, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f), params, ui::UiShade::Noise, ui::UiBlend::Alpha);
}

float GameUi::LetterboxBar(glm::vec2 full, float aspect) {
    if (!(aspect > 0.0f) || full.y <= 0.0f || full.x / full.y >= aspect) {
        return 0.0f;
    }
    return std::floor((full.y - full.x / aspect) * 0.5f);
}

void GameUi::DrawLetterbox(ui::UiBatch& batch, glm::vec2 full, float aspect) {
    const float bar = LetterboxBar(full, aspect);
    if (bar <= 0.0f) {
        return;
    }
    const ui::UiDrawParams black = ui::UiDrawParams::Plain(TextureManager::kWhite);
    const glm::vec4 color(0.0f, 0.0f, 0.0f, 1.0f);
    batch.Quad({0.0f, 0.0f}, {full.x, bar}, {0.0f, 0.0f}, {1.0f, 1.0f}, color, black, ui::UiShade::Solid, ui::UiBlend::Alpha);
    batch.Quad({0.0f, full.y - bar}, full, {0.0f, 0.0f}, {1.0f, 1.0f}, color, black, ui::UiShade::Solid, ui::UiBlend::Alpha);
}

void GameUi::DrawSaveDialog(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    (void)full;
    dialog::View view;
    view.text = save_dialog_text_;
    view.choices = {std::string(PcText("pc_ok", language_))};
    view.focus = 0;
    view.language = language_;
    view.flash = menu_.SelectionFlash(dialog_flash_);
    dialog::Draw(batch, canvas, assets_, menu_.Model(), prompts_, view);
}

std::string GameUi::DialogKind(int* focus) const {
    *focus = 0;
    if (!save_dialog_text_.empty()) return "save";
    return menu_.DialogKind(focus);
}

bool GameUi::PointerUnits(const InputState& input, glm::vec2* units) const {
    if (!input.pointer_valid || !canvas_ready_ || canvas_.scale <= 0.0f) return false;
    const glm::vec2 virtual_px = (input.pointer - canvas_.origin) / canvas_.scale;
    *units = glm::vec2(virtual_px.x - UiCanvas::kWidth * 0.5f, UiCanvas::kHeight * 0.5f - virtual_px.y) / UiCanvas::kUnit;
    return true;
}

void GameUi::SwapTracked() {
    std::swap(subtitles_, other_subtitles_);
    std::swap(demo_ui_, other_demo_ui_);
    std::swap(save_io_seen_, other_tracked_.save_io_seen);
    std::swap(last_caption_, other_tracked_.last_caption);
    std::swap(last_caption_time_, other_tracked_.last_caption_time);
    std::swap(last_subliminal_, other_tracked_.last_subliminal);
    std::swap(last_subliminal_time_, other_tracked_.last_subliminal_time);
    std::swap(queued_subtitles_, other_tracked_.queued_subtitles);
}

void GameUi::EnterTheater(Game& theater) {
    SwapTracked();
    subtitles_.Clear();
    demo_ui_.Clear();
    queued_subtitles_.clear();
    save_io_seen_ = theater.SaveIoCount();
    last_caption_ = theater.Effects().caption_id;
    last_caption_time_ = theater.Effects().caption_time;
    last_subliminal_ = theater.Effects().subliminal_image;
    last_subliminal_time_ = theater.Effects().subliminal_time;
    save_icon_wait_ = 0.0f;
}

void GameUi::LeaveTheater(Game& game) {
    subtitles_.Clear();
    demo_ui_.Clear();
    SwapTracked();
    save_io_seen_ = game.SaveIoCount();
}

void GameUi::DrawTheaterHint(ui::UiBatch& batch, const UiCanvas& canvas) {
    UiFont* font = assets_.Font(UiFontType::PcSystem, language_);
    if (!font) return;
    auto line = [&](const std::string& text, glm::vec2 lo, glm::vec2 hi, ui::TextAlign align) {
        if (text.empty()) return;
        DrawPromptText(batch, canvas, assets_, *font, text, prompts_, 18.0f, lo, hi, align, ui::TextAlign::Center, glm::vec4(1.0f, 1.0f, 1.0f, 0.55f), true);
    };
    line(theater_title_, {30.0f, 14.0f}, {UiCanvas::kWidth - 30.0f, 40.0f}, ui::TextAlign::Start);
    line(theater_hint_, {30.0f, UiCanvas::kHeight - 64.0f}, {UiCanvas::kWidth - 30.0f, UiCanvas::kHeight - 14.0f}, ui::TextAlign::Center);
}

void GameUi::DrawEndingSkip(ui::UiBatch& batch, const UiCanvas& canvas) {
    UiFont* font = assets_.Font(UiFontType::PcSystem, language_);
    if (!font || ending_skip_alpha_ <= 0.0f) return;
    const float light = 0.2126f * fade_.r + 0.7152f * fade_.g + 0.0722f * fade_.b;
    const glm::vec3 ink = fade_.a > 0.5f && light > 0.5f ? glm::vec3(0.08f) : glm::vec3(1.0f);
    const float alpha = std::clamp(ending_skip_alpha_, 0.0f, 1.0f);
    const glm::vec2 lo(UiCanvas::kWidth - 470.0f, UiCanvas::kHeight - 74.0f);
    const glm::vec2 hi(UiCanvas::kWidth - 40.0f, UiCanvas::kHeight - 44.0f);
    DrawPromptText(batch, canvas, assets_, *font, PcText("pc_ending_skip_hold", language_), prompts_, 18.0f, lo, hi, ui::TextAlign::End, ui::TextAlign::Center,
                   glm::vec4(ink, 0.75f * alpha), true);
    const glm::vec2 bar_lo = canvas.origin + glm::vec2(lo.x + 230.0f, hi.y + 8.0f) * canvas.scale;
    const glm::vec2 bar_hi = canvas.origin + glm::vec2(hi.x, hi.y + 11.0f) * canvas.scale;
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    batch.Quad(bar_lo, bar_hi, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(ink, 0.2f * alpha), plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    if (ending_skip_progress_ > 0.0f) {
        const glm::vec2 fill_hi(bar_lo.x + (bar_hi.x - bar_lo.x) * ending_skip_progress_, bar_hi.y);
        batch.Quad(bar_lo, fill_hi, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(ink, 0.8f * alpha), plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    }
}

void GameUi::DrawPortCredits(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    const glm::vec4 paper(0.93f, 0.94f, 0.93f, 1.0f);
    batch.Quad(glm::vec2(0.0f), full, glm::vec2(0.0f), glm::vec2(1.0f), paper, ui::UiDrawParams::Plain(TextureManager::kWhite),
               ui::UiShade::Solid, ui::UiBlend::Alpha);
    const port_credits::View view = port_credits::At(port_credits_time_);
    if (support_place_ == Support::Credits) {
        const float alpha = std::clamp(port_credits_time_, 0.0f, 1.0f);
        if (UiFont* system = assets_.Font(UiFontType::PcSystem, language_)) {
            ui::TextStyle style;
            style.font = &system->font;
            style.font_width = style.font_height = 18.0f;
            const glm::vec2 lo(UiCanvas::kWidth * 0.2f, UiCanvas::kHeight - 128.0f);
            const glm::vec2 hi(UiCanvas::kWidth * 0.8f, UiCanvas::kHeight - 102.0f);
            ui::TextLayout layout = ui::LayoutTextInBox(PcText("pc_credits_thanks", language_), style, glm::vec2(0.0f), hi - lo);
            ui::PlaceText(layout, lo, hi, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Center, true);
            DrawText(batch, canvas, *system, layout, glm::vec4(glm::vec3(0.08f), 0.8f * alpha), ui::UiBlend::Alpha);
        }
        support_rect_place_ = Support::Credits;
        DrawSupportButton(batch, canvas, PcText("pc_support_port", language_), glm::vec2(UiCanvas::kWidth * 0.5f, UiCanvas::kHeight - 76.0f),
                          ui::TextAlign::Center, glm::vec3(0.08f), glm::vec3(0.82f, 0.83f, 0.82f), alpha);
    }
    UiFont* font = view.card < 0 ? nullptr : assets_.Font(UiFontType::Movie, language_);
    if (!font || view.alpha <= 0.0f) {
        return;
    }
    const port_credits::Card& card = port_credits::Cards()[view.card];
    std::string text;
    for (std::string_view line : card.lines) {
        if (!text.empty()) text += '\n';
        text += line.starts_with("pc_") ? PcText(line, language_) : line;
    }
    ui::TextStyle style;
    style.font = &font->font;
    if (const UiFontStyle* s = assets_.FontStyle(kSubtitleFont, language_)) {
        style.font_width = s->width;
        style.font_height = s->height;
        style.text_space = s->text_space;
        style.line_space = s->line_space;
    }
    style.text_space += assets_.Generator().font_space;
    style.line_space += assets_.Generator().line_space;
    ui::NormalizeSubtitleStyle(style);
    const float scale = 0.8f * card.size * view.scale;
    style.font_width *= scale;
    style.font_height *= scale;
    style.text_space *= scale;
    style.line_space *= scale;
    ui::TextLayout layout = ui::LayoutText(text, style, UiCanvas::kWidth * 0.84f * view.scale);
    const glm::vec2 center(UiCanvas::kWidth * 0.5f, UiCanvas::kHeight * 0.5f);
    ui::PlaceText(layout, center - glm::vec2(UiCanvas::kWidth, UiCanvas::kHeight) * 0.5f, center + glm::vec2(UiCanvas::kWidth, UiCanvas::kHeight) * 0.5f,
                  ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Center, true);
    DrawText(batch, canvas, *font, layout, glm::vec4(glm::vec3(0.08f), view.alpha), ui::UiBlend::Alpha, ui::UiShade::Text);
}

void GameUi::UpdateSpeedrun(const Game& game) {
    const SpeedrunTimer& run = game.Speedrun();
    speedrun_view_ = SpeedrunView{};
    if (!run.Enabled()) return;
    speedrun_view_.shown = true;
    const auto state = run.GetState();
    if (state == SpeedrunTimer::State::Idle) {
        speedrun_view_.alpha = 0.45f;
        speedrun_view_.total = SpeedrunTimer::Format(0.0);
        return;
    }
    speedrun_view_.total = SpeedrunTimer::Format(run.Shown());
    if (state == SpeedrunTimer::State::Running) {
        const FloorLevel& floor = const_cast<Game&>(game).Floor();
        speedrun_view_.segment_name = SpeedrunTimer::FloorName(floor.CurrentFloorName(), floor.LoopCount(), language_);
        speedrun_view_.segment_time = SpeedrunTimer::Format(run.ShownSegment());
    } else if (!run.Splits().empty()) {
        const SpeedrunSplit& last = run.Splits().back();
        speedrun_view_.segment_name = SpeedrunTimer::SplitName(last, language_);
        speedrun_view_.segment_time = SpeedrunTimer::Format(run.ShowsGameTime() ? last.game : last.real);
    }
    double delta = 0.0;
    if ((state == SpeedrunTimer::State::Finished || run.SinceSplit() < 5.0) && run.LastSplitDelta(delta)) {
        speedrun_view_.ahead = delta < 0.0;
        speedrun_view_.delta = (delta < 0.0 ? "-" : "+") + SpeedrunTimer::Format(std::abs(delta));
    }
}

void GameUi::DrawSpeedrun(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    UiFont* font = assets_.Font(UiFontType::Movie, language_);
    if (!font || canvas.scale <= 0.0f) return;
    ui::TextStyle base;
    base.font = &font->font;
    if (const UiFontStyle* s = assets_.FontStyle(kSubtitleFont, language_)) {
        base.font_width = s->width;
        base.font_height = s->height;
        base.text_space = s->text_space;
        base.line_space = s->line_space;
    }
    ui::NormalizeSubtitleStyle(base);
    auto scaled = [&](float k) {
        ui::TextStyle style = base;
        style.font_width *= k;
        style.font_height *= k;
        style.text_space *= k;
        return style;
    };
    auto tabular = [](std::string_view text, const ui::TextStyle& style) {
        ui::TextLayout layout = ui::LayoutText(text, style, 0.0f);
        const float pitch = ui::LayoutText("0", style, 0.0f).width;
        for (ui::TextLine& line : layout.lines) {
            std::string_view rest = text;
            float pen = line.glyphs.empty() ? 0.0f : line.glyphs.front().position.x;
            for (size_t i = 0; i < line.glyphs.size(); ++i) {
                ui::LaidGlyph& g = line.glyphs[i];
                const float next = i + 1 < line.glyphs.size() ? line.glyphs[i + 1].position.x : line.width;
                const float advance = next - g.position.x;
                const bool digit = !rest.empty() && rest.front() >= '0' && rest.front() <= '9';
                if (!rest.empty()) rest.remove_prefix(1);
                const float cell = digit ? pitch : advance;
                g.position.x = pen + (cell - advance) * 0.5f;
                pen += cell;
            }
            line.width = pen;
            layout.width = std::max(layout.width, pen);
        }
        return layout;
    };
    const float bar = LetterboxBar(full, letterbox_);
    const float top = std::max(22.0f, (bar - canvas.origin.y) / canvas.scale + 12.0f);
    const float left = 34.0f;
    const glm::vec4 white(1.0f, 1.0f, 1.0f, speedrun_view_.alpha);
    float y = top;
    auto line = [&](const std::string& name, const std::string& time, float k, glm::vec4 color) {
        if (name.empty() && time.empty()) return;
        const ui::TextStyle style = scaled(k);
        float x = left;
        float height = 0.0f;
        if (!name.empty()) {
            ui::TextLayout layout = ui::LayoutText(name, style, 0.0f);
            ui::PlaceText(layout, {x, y}, {x + 600.0f, y + layout.height}, ui::TextAlign::Start, ui::TextAlign::Start, ui::TextAlign::Start);
            DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha, ui::UiShade::Border);
            x += layout.width + 12.0f * k;
            height = layout.height;
        }
        if (!time.empty()) {
            ui::TextLayout layout = tabular(time, style);
            ui::PlaceText(layout, {x, y}, {x + 600.0f, y + layout.height}, ui::TextAlign::Start, ui::TextAlign::Start, ui::TextAlign::Start);
            DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha, ui::UiShade::Border);
            height = std::max(height, layout.height);
        }
        y += height + 2.0f;
    };
    line({}, speedrun_view_.total, 0.9f, white);
    line(speedrun_view_.segment_name, speedrun_view_.segment_time, 0.62f, glm::vec4(1.0f, 1.0f, 1.0f, 0.8f));
    line({}, speedrun_view_.delta, 0.62f, speedrun_view_.ahead ? glm::vec4(0.45f, 0.9f, 0.5f, 1.0f) : glm::vec4(1.0f, 0.5f, 0.42f, 1.0f));
    speedrun_bottom_ = y;
}

namespace {
float SurvivalSmooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
}

void GameUi::DrawVision(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    const SurvivalHud& hud = survival_hud_;
    if (hud.vision_window < 0.0f || hud.vision_kind <= 0 || hud.vision_kind >= static_cast<int>(std::size(kSurvivalVisionNames))) return;
    const float late = std::clamp(static_cast<float>(hud.wave) / 100.0f, 0.0f, 1.0f);
    const glm::vec2 lo = -canvas.origin / canvas.scale, hi = (full - canvas.origin) / canvas.scale;
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    if (!vision_ring_) {
        constexpr int kSize = 256;
        std::vector<uint8_t> ring(size_t(kSize) * kSize * 4, 0);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / kSize * 2.0f - 1.0f, dy = (static_cast<float>(y) + 0.5f) / kSize * 2.0f - 1.0f;
                ring[(size_t(y) * kSize + x) * 4 + 3] = static_cast<uint8_t>(255.0f * SurvivalSmooth((std::sqrt(dx * dx + dy * dy) - 0.35f) / 0.75f));
            }
        }
        vision_ring_ = assets_.MakeTexture("ui:vision_ring", kSize, kSize, ring);
    }
    const float w = hud.vision_window;
    const float tunnel = SurvivalSmooth(w / 0.08f) * (1.0f - SurvivalSmooth((w - 0.85f) / 0.15f));
    if (vision_ring_) {
        batch.Quad(canvas.ToTarget(lo), canvas.ToTarget(hi), {0.0f, 0.0f}, {1.0f, 1.0f}, glm::vec4(0.0f, 0.0f, 0.0f, (0.35f + 0.2f * late) * tunnel),
                   ui::UiDrawParams::Plain(vision_ring_), ui::UiShade::Material, ui::UiBlend::Alpha);
    }
    const char* name = kSurvivalVisionNames[hud.vision_kind];
    const uint32_t card = *name && !vision_dir_.empty() ? assets_.LocalPreview(vision_dir_ + "/" + name + ".png") : 0;
    const bool photo = card == 0;
    const uint32_t picture = photo ? assets_.Texture("/Assets/sh/environ/object/shsb/label/shsb_labl001/sourceimages/shsb_labl001_p2_bsm_alp.ftex") : card;
    const glm::vec2 uv_lo = photo ? glm::vec2(0.02f, 0.64f) : glm::vec2(0.0f), uv_hi = photo ? glm::vec2(0.34f, 0.97f) : glm::vec2(1.0f);
    if (hud.vision_index < 0 || !picture) return;
    const float k = hud.vision_k;
    auto h = [&](uint32_t n) {
        uint32_t v = (hud.notice_serial * 977u + static_cast<uint32_t>(hud.vision_index) * 131u + n) * 0x9E3779B1u;
        v ^= v >> 15;
        v *= 0x2C1B3C6Du;
        v ^= v >> 12;
        return static_cast<float>(v & 0xFFFFFFu) / 16777216.0f;
    };
    const float burn = reduced_flashing_ ? 1.0f : (1.0f + (1.1f + 0.6f * late) * (1.0f - SurvivalSmooth(k / 0.14f)));
    if (!reduced_flashing_ && k < 0.14f && static_cast<int>(k * 60.0f) % 2 == 1) return;
    const int step = static_cast<int>(k * 18.0f);
    const bool glitch = h(100 + static_cast<uint32_t>(step)) < 0.2f + 0.3f * late;
    const float height = 720.0f * (0.42f + 0.18f * late + 0.1f * h(4)) * (1.0f + 0.16f * (1.0f - SurvivalSmooth(k / 0.22f)));
    const float width = height * (photo ? 0.97f : 0.75f);
    const glm::vec2 shake((h(400 + static_cast<uint32_t>(step)) - 0.5f) * (4.0f + 9.0f * late), (h(500 + static_cast<uint32_t>(step)) - 0.5f) * (3.0f + 6.0f * late));
    const glm::vec2 centre = lo + (hi - lo) * glm::vec2(0.16f + 0.68f * h(1), 0.2f + 0.6f * h(2)) + shake;
    const float angle = (h(3) - 0.5f) * (0.62f + 0.4f * late);
    const glm::vec2 ax(std::cos(angle), std::sin(angle)), ay(-std::sin(angle), std::cos(angle));
    constexpr int kRows = 12;
    auto draw = [&](glm::vec2 offset, glm::vec4 color, ui::UiBlend blend, float grow) {
        std::vector<ui::UiVertex> v;
        v.reserve(kRows * 6);
        const float hw = 0.5f * width * grow, hh = 0.5f * height * grow;
        for (int r = 0; r < kRows; ++r) {
            const float v0 = static_cast<float>(r) / kRows, v1 = static_cast<float>(r + 1) / kRows;
            const uint32_t at = static_cast<uint32_t>(step * kRows + r);
            const float tear = glitch && h(200 + at) < 0.45f ? (h(300 + at) - 0.5f) * (26.0f + 54.0f * late) : 0.0f;
            auto corner = [&](float x, float y, float u, float vv) {
                const glm::vec2 p = centre + offset + ax * (x + tear) + ay * y;
                return ui::UiVertex{canvas.ToTarget(p), {u, vv}, color};
            };
            const float y0 = -hh + 2.0f * hh * v0, y1 = -hh + 2.0f * hh * v1;
            const float t0 = glm::mix(uv_lo.y, uv_hi.y, v0), t1 = glm::mix(uv_lo.y, uv_hi.y, v1);
            const ui::UiVertex a = corner(-hw, y0, uv_lo.x, t0), b = corner(hw, y0, uv_hi.x, t0), c = corner(hw, y1, uv_hi.x, t1), d = corner(-hw, y1, uv_lo.x, t1);
            v.insert(v.end(), {a, b, c, a, c, d});
        }
        batch.Draw(v, ui::UiDrawParams::Plain(picture), ui::UiShade::Material, blend);
    };
    const float split = (3.0f + 9.0f * late) * (1.0f - 0.5f * k) + (glitch ? 6.0f : 0.0f);
    draw(glm::vec2((h(5) - 0.5f) * 80.0f, (h(6) - 0.5f) * 44.0f), glm::vec4(hud.vision_tint, 0.12f + 0.08f * late), ui::UiBlend::Additive, 1.07f);
    draw(glm::vec2(0.0f), glm::vec4(glm::min(hud.vision_tint * burn, glm::vec3(2.2f)), 1.0f), ui::UiBlend::Alpha, 1.0f);
    draw(ax * split, glm::vec4(1.0f, 0.14f, 0.1f, 0.3f), ui::UiBlend::Additive, 1.0f);
    draw(-ax * split, glm::vec4(0.1f, 0.8f, 1.0f, 0.3f), ui::UiBlend::Additive, 1.0f);
    if (const uint32_t noise = StaticTexture(); noise && glitch) {
        std::vector<ui::UiVertex> v;
        const float hw = 0.5f * width, hh = 0.5f * height;
        for (int r = 0; r < kRows; ++r) {
            const uint32_t at = static_cast<uint32_t>(step * kRows + r);
            if (h(600 + at) > 0.18f + 0.14f * late) continue;
            const float y0 = -hh + 2.0f * hh * static_cast<float>(r) / kRows, y1 = -hh + 2.0f * hh * static_cast<float>(r + 1) / kRows;
            const glm::vec2 o(h(700 + at), h(800 + at));
            const glm::vec4 c(1.0f, 1.0f, 1.0f, 0.14f + 0.12f * late);
            auto corner = [&](float x, float y, float u, float vv) { return ui::UiVertex{canvas.ToTarget(centre + ax * x + ay * y), o + glm::vec2(u, vv), c}; };
            const ui::UiVertex a = corner(-hw, y0, 0.0f, 0.0f), b = corner(hw, y0, 2.0f, 0.0f), cc = corner(hw, y1, 2.0f, 0.15f), d = corner(-hw, y1, 0.0f, 0.15f);
            v.insert(v.end(), {a, b, cc, a, cc, d});
        }
        if (!v.empty()) batch.Draw(v, ui::UiDrawParams::Plain(noise), ui::UiShade::Material, ui::UiBlend::Additive);
    }
}

uint32_t GameUi::StaticTexture() {
    if (!vision_static_) {
        constexpr int kWidth = 256, kHeight = 128;
        std::vector<uint8_t> pixels(size_t(kWidth) * kHeight * 4);
        uint32_t seed = 0x5747u;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            seed = seed * 1664525u + 1013904223u;
            const uint8_t v = static_cast<uint8_t>(seed >> 24);
            pixels[i] = pixels[i + 1] = pixels[i + 2] = v;
            pixels[i + 3] = 255;
        }
        vision_static_ = assets_.MakeTexture("ui:static", kWidth, kHeight, pixels);
    }
    return vision_static_;
}

void GameUi::DrawBands(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    const uint32_t noise = StaticTexture();
    if (band_glitch_ <= 0.0f || !noise) return;
    const glm::vec2 lo = -canvas.origin / canvas.scale, hi = (full - canvas.origin) / canvas.scale;
    const int frame = static_cast<int>((0.2f - band_glitch_) * 60.0f);
    auto h = [&](uint32_t n) {
        uint32_t v = (band_seed_ * 613u + n) * 0x9E3779B1u;
        v ^= v >> 15;
        v *= 0x2C1B3C6Du;
        v ^= v >> 12;
        return static_cast<float>(v & 0xFFFFFFu) / 16777216.0f;
    };
    const int bands = 1 + static_cast<int>(h(1) * 3.0f);
    for (int b = 0; b < bands; ++b) {
        if (h(40 + static_cast<uint32_t>(b * 16 + frame)) < 0.25f) continue;
        const float y = lo.y + (hi.y - lo.y) * (0.1f + 0.8f * h(10 + static_cast<uint32_t>(b)));
        const float height = (hi.y - lo.y) * (0.015f + 0.05f * h(20 + static_cast<uint32_t>(b)));
        const float x0 = lo.x + (hi.x - lo.x) * 0.6f * h(30 + static_cast<uint32_t>(b)), x1 = x0 + (hi.x - lo.x) * (0.35f + 0.5f * h(50 + static_cast<uint32_t>(b)));
        const glm::vec2 o(h(60 + static_cast<uint32_t>(b * 16 + frame)), h(70 + static_cast<uint32_t>(b * 16 + frame)));
        batch.Quad(canvas.ToTarget({x0, y}), canvas.ToTarget({x1, y + height}), o, o + glm::vec2(3.0f, 0.08f), glm::vec4(1.0f, 1.0f, 1.0f, 0.28f),
                   ui::UiDrawParams::Plain(noise), ui::UiShade::Material, ui::UiBlend::Additive);
        batch.Quad(canvas.ToTarget({x0, y}), canvas.ToTarget({x1, y + 1.5f}), {0.0f, 0.0f}, {1.0f, 1.0f}, glm::vec4(1.0f, 1.0f, 1.0f, 0.35f),
                   ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Additive);
    }
}

void GameUi::DrawAccusation(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    if (accuse_time_ < 0.0f) return;
    const float t = accuse_time_;
    const glm::vec2 lo = -canvas.origin / canvas.scale, hi = (full - canvas.origin) / canvas.scale;
    const bool on = reduced_flashing_ ? (t < 0.48f) : (t < 0.07f || (t >= 0.1f && t < 0.42f) || (t >= 0.47f && t < 0.5f));
    const float dark = reduced_flashing_ ? (on ? 0.62f : 0.35f * (1.0f - t / kAccuseLength)) : (on ? 0.86f : 0.5f * (1.0f - t / kAccuseLength));
    batch.Quad(canvas.ToTarget(lo), canvas.ToTarget(hi), {0.0f, 0.0f}, {1.0f, 1.0f}, glm::vec4(0.05f, 0.0f, 0.0f, dark),
               ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
    if (!on || !accuse_texture_) return;
    const uint32_t step = static_cast<uint32_t>(t * 30.0f);
    auto h = [&](uint32_t n) {
        uint32_t v = (survival_accuse_seen_ * 389u + n) * 0x9E3779B1u;
        v ^= v >> 15;
        v *= 0x2C1B3C6Du;
        v ^= v >> 12;
        return static_cast<float>(v & 0xFFFFFFu) / 16777216.0f;
    };
    float ems = 0.0f;
    for (const char ch : accuse_words_) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c & 0xC0) == 0x80) continue;
        ems += c >= 0xE3 ? 1.0f : 0.55f;
    }
    const float fit = std::clamp(0.95f / std::max(0.05f, ems * 72.0f / static_cast<float>(kAccuseStrip.x)), 1.0f, 2.8f);
    const float grow = fit * (1.0f + 0.22f * (1.0f - SurvivalSmooth(t / 0.12f)));
    const glm::vec2 centre = 0.5f * (lo + hi) + glm::vec2((h(10 + step) - 0.5f) * 22.0f, (h(20 + step) - 0.5f) * 12.0f);
    const glm::vec2 size = glm::vec2(1280.0f, 1280.0f * static_cast<float>(kAccuseStrip.y) / static_cast<float>(kAccuseStrip.x)) * grow;
    auto draw = [&](glm::vec2 offset, float scale, glm::vec4 color, ui::UiBlend blend) {
        const glm::vec2 a = centre + offset - 0.5f * size * scale;
        batch.Quad(canvas.ToTarget(a), canvas.ToTarget(a + size * scale), {0.0f, 0.0f}, {1.0f, 1.0f}, color, ui::UiDrawParams::Plain(accuse_texture_),
                   ui::UiShade::Material, blend);
    };
    const float split = 5.0f + 8.0f * (1.0f - SurvivalSmooth(t / 0.2f));
    draw(glm::vec2((h(30) - 0.5f) * 60.0f, (h(40) - 0.5f) * 30.0f), 1.12f, glm::vec4(1.0f, 0.3f, 0.25f, 0.14f), ui::UiBlend::Additive);
    draw(glm::vec2(0.0f), 1.0f, glm::vec4(1.0f, 0.93f, 0.9f, 1.0f), ui::UiBlend::Alpha);
    draw(glm::vec2(split, 0.0f), 1.0f, glm::vec4(1.0f, 0.1f, 0.06f, 0.55f), ui::UiBlend::Additive);
    draw(glm::vec2(-split, 0.0f), 1.0f, glm::vec4(0.1f, 0.75f, 1.0f, 0.35f), ui::UiBlend::Additive);
}

void GameUi::DrawSurvival(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    const SurvivalHud& hud = survival_hud_;
    UiFont* font = assets_.Font(UiFontType::Movie, language_);
    if (!font || canvas.scale <= 0.0f) return;
    ui::TextStyle base;
    base.font = &font->font;
    if (const UiFontStyle* s = assets_.FontStyle(kSubtitleFont, language_)) {
        base.font_width = s->width;
        base.font_height = s->height;
        base.text_space = s->text_space;
        base.line_space = s->line_space;
    }
    ui::NormalizeSubtitleStyle(base);
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    auto rect = [&](glm::vec2 lo, glm::vec2 hi, glm::vec4 color) {
        batch.Quad(canvas.ToTarget(lo), canvas.ToTarget(hi), {0.0f, 0.0f}, {1.0f, 1.0f}, color, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    auto quad = [&](glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, glm::vec4 color) {
        const std::vector<ui::UiVertex> v = {{canvas.ToTarget(a), {0, 0}, color}, {canvas.ToTarget(b), {0, 0}, color}, {canvas.ToTarget(c), {0, 0}, color},
                                             {canvas.ToTarget(a), {0, 0}, color}, {canvas.ToTarget(c), {0, 0}, color}, {canvas.ToTarget(d), {0, 0}, color}};
        batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    auto line = [&](glm::vec2 a, glm::vec2 b, float width, glm::vec4 color) {
        const glm::vec2 d = b - a;
        const float l = glm::length(d);
        if (l < 1e-3f) return;
        const glm::vec2 n = glm::vec2(-d.y, d.x) / l * (width * 0.5f);
        quad(a + n, b + n, b - n, a - n, color);
    };
    auto words = [&](std::string_view key, int value) {
        std::string text_line(PcText(key, language_));
        if (const size_t at = text_line.find("{}"); at != std::string::npos) text_line.replace(at, 2, std::to_string(value));
        return text_line;
    };
    auto text = [&](const std::string& s, glm::vec2 at, float k, glm::vec4 color, ui::TextAlign align) {
        if (s.empty() || color.a <= 0.0f) return 0.0f;
        ui::TextStyle style = base;
        style.font_width *= k;
        style.font_height *= k;
        style.text_space *= k;
        ui::TextLayout layout = ui::LayoutText(s, style, 0.0f);
        const float x = align == ui::TextAlign::Center ? at.x - layout.width * 0.5f : align == ui::TextAlign::End ? at.x - layout.width : at.x;
        ui::PlaceText(layout, {x, at.y}, {x + layout.width, at.y + layout.height}, ui::TextAlign::Start, ui::TextAlign::Start, ui::TextAlign::Start);
        DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha, ui::UiShade::Border);
        return layout.height;
    };
    const float bar = LetterboxBar(full, letterbox_);
    const float top = std::max(22.0f, (bar - canvas.origin.y) / canvas.scale + 12.0f);
    const float bottom = std::min(UiCanvas::kHeight - 22.0f, UiCanvas::kHeight - (bar - canvas.origin.y) / canvas.scale - 12.0f);
    const glm::vec2 screen_lo = -canvas.origin / canvas.scale;
    const glm::vec2 screen_hi = (full - canvas.origin) / canvas.scale;
    const glm::vec2 middle(640.0f, 360.0f);
    const glm::vec4 white(1.0f, 1.0f, 1.0f, 0.92f);
    const glm::vec4 dim(1.0f, 1.0f, 1.0f, 0.6f);
    DrawBands(batch, canvas, full);
    DrawVision(batch, canvas, full);
    const glm::vec4 red(0.92f, 0.16f, 0.12f, 0.95f);

    ++survival_pulse_;
    const float since_beat = std::max(0.0f, hud.run_seconds - hud.last_beat);
    const float beat = hud.heart_rate > 0.0f ? std::exp(-since_beat * 6.0f) : 0.0f;
    const float low = !hud.dead ? hud.low_health * (0.35f + 0.65f * beat) : 0.0f;
    const float edge = std::clamp(hud.hurt * 0.75f + low * 0.85f + (hud.dead ? 0.6f : 0.0f), 0.0f, 1.0f);
    if (edge > 0.0f) {
        constexpr int kSegments = 48;
        constexpr float kRadius[] = {0.62f, 0.85f, 1.0f, 1.6f};
        constexpr float kShare[] = {0.0f, 0.45f, 0.85f, 1.0f};
        const glm::vec2 c = full * 0.5f;
        const glm::vec2 half = c * 1.4143f;
        std::vector<ui::UiVertex> v;
        v.reserve(kSegments * 6 * 3);
        for (int ring = 0; ring + 1 < static_cast<int>(std::size(kRadius)); ++ring) {
            const glm::vec4 inner(0.55f, 0.0f, 0.0f, edge * kShare[ring]);
            const glm::vec4 outer(0.55f, 0.0f, 0.0f, edge * kShare[ring + 1]);
            for (int i = 0; i < kSegments; ++i) {
                const float a0 = 6.2831853f * static_cast<float>(i) / kSegments;
                const float a1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
                const glm::vec2 d0(std::cos(a0), std::sin(a0)), d1(std::cos(a1), std::sin(a1));
                v.push_back({c + half * d0 * kRadius[ring], {0, 0}, inner});
                v.push_back({c + half * d0 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d1 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d0 * kRadius[ring], {0, 0}, inner});
                v.push_back({c + half * d1 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d1 * kRadius[ring], {0, 0}, inner});
            }
        }
        batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    }

    if (const float heal_edge = !hud.dead ? SurvivalSmooth(hud.heal) * 0.42f : 0.0f; heal_edge > 0.005f) {
        constexpr int kSegments = 48;
        constexpr float kRadius[] = {0.62f, 0.86f, 1.0f, 1.6f};
        constexpr float kShare[] = {0.0f, 0.42f, 0.82f, 1.0f};
        const glm::vec2 c = full * 0.5f;
        const glm::vec2 half = c * 1.4143f;
        std::vector<ui::UiVertex> v;
        v.reserve(kSegments * 6 * 3);
        for (int ring = 0; ring + 1 < static_cast<int>(std::size(kRadius)); ++ring) {
            const glm::vec4 inner(0.08f, 0.46f, 0.22f, heal_edge * kShare[ring]);
            const glm::vec4 outer(0.06f, 0.38f, 0.18f, heal_edge * kShare[ring + 1]);
            for (int i = 0; i < kSegments; ++i) {
                const float a0 = 6.2831853f * static_cast<float>(i) / kSegments;
                const float a1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
                const glm::vec2 d0(std::cos(a0), std::sin(a0)), d1(std::cos(a1), std::sin(a1));
                v.push_back({c + half * d0 * kRadius[ring], {0, 0}, inner});
                v.push_back({c + half * d0 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d1 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d0 * kRadius[ring], {0, 0}, inner});
                v.push_back({c + half * d1 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d1 * kRadius[ring], {0, 0}, inner});
            }
        }
        batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    }

    if (const float dread = SurvivalSmooth((hud.fear - 0.3f) / 0.7f) * (0.75f + 0.25f * beat); dread > 0.0f && !hud.dead) {
        constexpr int kSegments = 48;
        constexpr float kRadius[] = {0.5f, 0.8f, 1.0f, 1.6f};
        constexpr float kShare[] = {0.0f, 0.55f, 0.9f, 1.0f};
        const glm::vec2 c = full * 0.5f;
        const glm::vec2 half = c * 1.4143f;
        std::vector<ui::UiVertex> v;
        v.reserve(kSegments * 6 * 3);
        for (int ring = 0; ring + 1 < static_cast<int>(std::size(kRadius)); ++ring) {
            const glm::vec4 inner(0.0f, 0.0f, 0.012f, 0.8f * dread * kShare[ring]);
            const glm::vec4 outer(0.0f, 0.0f, 0.012f, 0.8f * dread * kShare[ring + 1]);
            for (int i = 0; i < kSegments; ++i) {
                const float a0 = 6.2831853f * static_cast<float>(i) / kSegments;
                const float a1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
                const glm::vec2 d0(std::cos(a0), std::sin(a0)), d1(std::cos(a1), std::sin(a1));
                v.push_back({c + half * d0 * kRadius[ring], {0, 0}, inner});
                v.push_back({c + half * d0 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d1 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d0 * kRadius[ring], {0, 0}, inner});
                v.push_back({c + half * d1 * kRadius[ring + 1], {0, 0}, outer});
                v.push_back({c + half * d1 * kRadius[ring], {0, 0}, inner});
            }
        }
        batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    }

    if (hud.dead) {
        const float t = hud.dead_time;
        const float under = hud.caught_end ? 1.0f : 0.9f * SurvivalSmooth((t - 0.5f) / 1.4f);
        const glm::vec3 shade = hud.caught_end ? glm::vec3(0.0f) : glm::vec3(0.035f, 0.004f, 0.004f);
        batch.Quad({0.0f, 0.0f}, full, {0.0f, 0.0f}, {1.0f, 1.0f}, glm::vec4(shade, under), plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
        const float shown = hud.caught_end ? SurvivalSmooth((t - 0.15f) / 0.6f) : SurvivalSmooth((t - 0.9f) / 0.9f);
        if (shown <= 0.0f) return;
        {
            const float lo = 110.0f, hi = 630.0f, mid = 0.5f * (lo + hi);
            const glm::vec4 clear(0.0f, 0.0f, 0.0f, 0.0f), dark(0.0f, 0.0f, 0.0f, 0.55f * shown);
            const auto band = [&](float y0, float y1, const glm::vec4& c0, const glm::vec4& c1) {
                const std::vector<ui::UiVertex> v = {{canvas.ToTarget({screen_lo.x, y0}), {0, 0}, c0}, {canvas.ToTarget({screen_hi.x, y0}), {0, 0}, c0},
                                                     {canvas.ToTarget({screen_hi.x, y1}), {0, 0}, c1}, {canvas.ToTarget({screen_lo.x, y0}), {0, 0}, c0},
                                                     {canvas.ToTarget({screen_hi.x, y1}), {0, 0}, c1}, {canvas.ToTarget({screen_lo.x, y1}), {0, 0}, c1}};
                batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
            };
            band(lo, mid, clear, dark);
            band(mid, hi, dark, clear);
        }
        const glm::vec4 blood(0.9f, 0.18f, 0.14f, shown);
        const glm::vec4 w(1.0f, 1.0f, 1.0f, 0.9f * shown);
        const glm::vec4 soft(1.0f, 1.0f, 1.0f, 0.6f * shown);
        float y = 150.0f + 10.0f * (1.0f - shown);
        static constexpr const char* kLevels[] = {"pc_survival_easy", "pc_survival_normal", "pc_survival_hard", "pc_survival_nightmare", "pc_survival_insanity"};
        {
            std::string over = words(kLevels[static_cast<size_t>(hud.difficulty)], 0);
            if (hud.daily && hud.daily_mutator >= 0) {
                over += " - " + words(DailyMutatorById(hud.daily_mutator).name_key, 0);
            }
            if (const int night = SurvivalNightOf(hud.wave); night > 1) over += " - " + words("pc_survival_night", night);
            y += text(over, {640.0f, y}, 0.7f, soft, ui::TextAlign::Center) + 8.0f;
        }
        y += text(words("pc_survival_dead", SurvivalWaveInNight(hud.wave)), {640.0f, y}, 1.5f, blood, ui::TextAlign::Center) + 10.0f;
        static constexpr const char* kCauses[] = {"pc_survival_cause_doll", "pc_survival_cause_crawler", "pc_survival_cause_stretched", "pc_survival_cause_mother",
                                                  "pc_survival_cause_lisa",  "pc_survival_cause_giant",   "pc_survival_cause_dark"};
        if (hud.cause >= 0 && hud.cause < static_cast<int>(std::size(kCauses))) {
            y += text(words(kCauses[static_cast<size_t>(hud.cause)], 0), {640.0f, y}, 0.78f, glm::vec4(1.0f, 0.86f, 0.82f, 0.85f * shown), ui::TextAlign::Center);
        }
        y += 18.0f;
        line({380.0f, y}, {900.0f, y}, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.3f * shown));
        y += 16.0f;
        {
            const int seconds = static_cast<int>(hud.end_seconds);
            std::string time_line(PcText("pc_survival_time", language_));
            if (const size_t at = time_line.find("{}"); at != std::string::npos) {
                time_line.replace(at, 2, std::format("{}:{:02}", seconds / 60, seconds % 60));
            }
            const std::string cells[] = {words("pc_survival_kills", hud.end_kills),         words("pc_survival_headshots", hud.end_headshots),
                                         words("pc_survival_accuracy", hud.end_accuracy),   words("pc_survival_score_total", hud.end_score),
                                         words("pc_survival_best_chain", hud.end_chain),     time_line};
            float row_h = 0.0f;
            for (size_t i = 0; i < std::size(cells); ++i) {
                const float k = hud.caught_end ? SurvivalSmooth((t - 0.35f - 0.06f * static_cast<float>(i)) / 0.4f)
                                               : SurvivalSmooth((t - 1.2f - 0.08f * static_cast<float>(i)) / 0.5f);
                const float x = i % 2 == 0 ? 500.0f : 780.0f;
                const float h = text(cells[i], {x, y + row_h * static_cast<float>(i / 2) + 4.0f * (1.0f - k)}, 0.8f, glm::vec4(1.0f, 1.0f, 1.0f, 0.9f * k),
                                     ui::TextAlign::Center);
                row_h = std::max(row_h, h + 12.0f);
            }
            y += row_h * 3.0f + 8.0f;
        }
        line({380.0f, y}, {900.0f, y}, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.3f * shown));
        y += 16.0f;
        const bool best_later = hud.best_wave > kSurvivalWaves;
        y += text(words(best_later ? "pc_survival_best_night" : "pc_survival_best", best_later ? SurvivalNightOf(hud.best_wave) : hud.best_wave),
                  {640.0f, y}, 0.82f, w, ui::TextAlign::Center) +
             6.0f;
        if (hud.end_new_best) {
            y += text(words("pc_survival_new_best", 0), {640.0f, y}, 0.78f, glm::vec4(1.0f, 0.84f, 0.5f, 0.9f * shown), ui::TextAlign::Center) + 6.0f;
        }
        if (hud.end_credits_earned > 0 || hud.end_credits_total > 0) {
            std::string payout = std::format("+${}", hud.end_credits_earned);
            if (hud.end_daily_bonus > 0) {
                payout += std::format("  (+${})", hud.end_daily_bonus);
            }
            const glm::vec4 gold(0.95f, 0.82f, 0.42f, 0.95f * shown);
            y += text(payout, {640.0f, y}, 0.82f, gold, ui::TextAlign::Center) + 4.0f;
            y += text(std::format("{}: ${}", PcText("pc_armory_credits", language_), hud.end_credits_total), {640.0f, y}, 0.68f,
                      glm::vec4(1.0f, 1.0f, 1.0f, 0.72f * shown), ui::TextAlign::Center) +
                 6.0f;
        }
        y += 22.0f;
        if (hud.can_retry) {
            const float blink = (0.86f + 0.14f * std::sin((t - 2.5f) * 2.6f + 1.5708f)) * std::max(shown, 0.92f);
            DrawPromptText(batch, canvas, assets_, *font, PcText("pc_survival_death_prompts", language_), prompts_, 22.0f,
                           {180.0f, y - 6.0f}, {1100.0f, y + 36.0f}, ui::TextAlign::Center, ui::TextAlign::Center,
                           glm::vec4(1.0f, 1.0f, 1.0f, blink), true);
        }
        return;
    }

    {
        const float aspect = full.y > 0.0f ? full.x / full.y : 16.0f / 9.0f;
        const float t = std::tan(hud.fov_y * 0.5f);
        std::vector<glm::vec3> marks = hud.task_marks;
        if (hud.supply_on) marks.push_back(hud.supply_at);
        for (const glm::vec3& mark : marks) {
            glm::vec3 v(hud.view * glm::vec4(mark, 1.0f));
            const float distance = glm::length(mark - hud.eye);
            const bool behind = v.z > -0.1f;
            glm::vec2 ndc = behind ? glm::vec2(v.x, v.y) * (v.z > 0.0f ? -1.0f : 1.0f) * 1e3f : glm::vec2(v.x / (-v.z * t * aspect), v.y / (-v.z * t));
            const bool edge_held = behind || std::abs(ndc.x) > 0.92f || std::abs(ndc.y) > 0.88f;
            if (behind) ndc = glm::vec2(v.x >= 0.0f ? 1.0f : -1.0f, 0.0f) * 2.0f + glm::vec2(0.0f, -0.3f);
            if (edge_held) ndc /= std::max(1.0f, std::max(std::abs(ndc.x) / 0.92f, std::abs(ndc.y) / 0.88f));
            const glm::vec2 px((ndc.x * 0.5f + 0.5f) * full.x, (0.5f - ndc.y * 0.5f) * full.y);
            const glm::vec2 at = (px - canvas.origin) / canvas.scale;
            const float alpha = 0.9f * std::clamp((distance - 1.0f) / 1.5f, 0.0f, 1.0f) * (0.75f + 0.25f * std::sin(static_cast<float>(survival_pulse_) * 0.12f));
            if (alpha <= 0.01f) continue;
            const glm::vec3 gold(1.0f, 0.84f, 0.5f);
            for (const float r : {9.0f, 4.0f}) {
                const glm::vec2 corner[4] = {at + glm::vec2(0.0f, -r), at + glm::vec2(r, 0.0f), at + glm::vec2(0.0f, r), at + glm::vec2(-r, 0.0f)};
                for (int c = 0; c < 4; ++c) {
                    line(corner[c], corner[(c + 1) % 4], 2.8f, glm::vec4(0.0f, 0.0f, 0.0f, 0.4f * alpha));
                    line(corner[c], corner[(c + 1) % 4], 1.3f, glm::vec4(gold, alpha));
                }
            }
            text(words("pc_survival_metres", static_cast<int>(std::lround(distance))), at + glm::vec2(0.0f, 12.0f), 0.42f, glm::vec4(gold, alpha * 0.9f),
                 ui::TextAlign::Center);
        }
    }

    const std::string coming = SurvivalNightOf(hud.wave + 1) > SurvivalNightOf(std::max(hud.wave, 1))
                                   ? words("pc_survival_night", SurvivalNightOf(hud.wave + 1))
                                   : words("pc_survival_next_wave", SurvivalWaveInNight(hud.wave + 1));
    float task_top = top + 50.0f;
    if (hud.phase == SurvivalPhase::Wave && hud.wave > 0) {
        float at = top;
        if (const int night = SurvivalNightOf(hud.wave); night > 1) {
            at += text(words("pc_survival_night", night), {640.0f, top}, 0.52f, dim, ui::TextAlign::Center) + 4.0f;
        }
        const float h = text(words("pc_survival_wave", SurvivalWaveInNight(hud.wave)), {640.0f, at}, 0.68f, white, ui::TextAlign::Center);
        float end = at + h;
        if (hud.enemies_left > 0) {
            end += 8.0f + text(words("pc_survival_left", hud.false_left >= 0 ? hud.false_left : hud.enemies_left), {640.0f, at + h + 8.0f}, 0.48f, dim,
                               ui::TextAlign::Center);
        }
        task_top = std::max(task_top, end + 4.0f);
    } else if (hud.phase == SurvivalPhase::Countdown || hud.phase == SurvivalPhase::Break) {
        if (hud.countdown <= 5.0f && hud.countdown > 0.0f) {
            const int n = static_cast<int>(std::ceil(hud.countdown));
            const float frac = hud.countdown - std::floor(hud.countdown);
            const float pop = 1.0f + 0.35f * SurvivalSmooth((frac - 0.75f) / 0.25f);
            const float alpha = std::min(1.0f, frac * 3.0f + 0.2f);
            text(coming, {640.0f, 150.0f}, 0.62f, glm::vec4(1.0f, 1.0f, 1.0f, 0.75f), ui::TextAlign::Center);
            text(std::to_string(n), {640.0f, 178.0f - 18.0f * (pop - 1.0f)}, 2.2f * pop, glm::vec4(1.0f, 1.0f, 1.0f, alpha * 0.95f), ui::TextAlign::Center);
        } else if (hud.wave > 0) {
            text(coming, {640.0f, top}, 0.55f, dim, ui::TextAlign::Center);
            text(std::to_string(static_cast<int>(std::ceil(hud.countdown))), {640.0f, top + 20.0f}, 0.62f, white, ui::TextAlign::Center);
        }
    }

    if (!hud.task_key.empty() && hud.task_age >= 0.0f && !hud.dead && hud.task_shown > 0.0f) {
        const bool over = hud.task_done || hud.task_lost;
        const float age = hud.task_age;
        if (!over || age < 4.5f) {
            const float notice = over ? 1.0f - SurvivalSmooth((age - 3.5f) / 1.0f) : 1.0f - SurvivalSmooth((age - 5.0f) / 0.6f);
            const float appear = SurvivalSmooth(age / 0.35f);
            const float y = task_top - 6.0f * (1.0f - appear);
            std::string task_line = words(over ? (hud.task_done ? "pc_survival_task_done" : "pc_survival_task_lost") : hud.task_key, 0);
            if (!over && hud.task_need > 0) {
                task_line += hud.task_need == 100 ? "  " + std::to_string(hud.task_count) + " %"
                                             : "  " + std::to_string(hud.task_count) + " / " + std::to_string(hud.task_need);
            }
            const glm::vec3 tone = hud.task_lost ? glm::vec3(0.86f, 0.5f, 0.45f) : over ? glm::vec3(1.0f, 0.88f, 0.62f) : glm::vec3(0.94f, 0.92f, 0.88f);
            text(task_line, {640.0f, y}, 0.48f, glm::vec4(tone, hud.task_shown * appear * (over ? notice : 0.85f)), ui::TextAlign::Center);
        }
    }
    if (!hud.use_key.empty() && !hud.dead) {
        const bool keyboard = prompts_.device == PromptDevice::Keyboard;
        const glm::vec2 key(560.0f, 470.0f);
        if (keyboard) {
            rect(key - glm::vec2(11.0f), key + glm::vec2(11.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.9f));
            text("E", key - glm::vec2(0.0f, 10.0f), 0.5f, glm::vec4(0.05f, 0.05f, 0.05f, 1.0f), ui::TextAlign::Center);
        } else {
            for (int i = 0; i < 20; ++i) {
                const float a0 = 6.2831853f * static_cast<float>(i) / 20.0f, a1 = 6.2831853f * static_cast<float>(i + 1) / 20.0f;
                line(key + 11.0f * glm::vec2(std::cos(a0), std::sin(a0)), key + 11.0f * glm::vec2(std::cos(a1), std::sin(a1)), 1.6f, glm::vec4(1.0f, 1.0f, 1.0f, 0.9f));
            }
            line(key + glm::vec2(-5.0f, -5.0f), key + glm::vec2(5.0f, 5.0f), 2.0f, glm::vec4(0.55f, 0.75f, 1.0f, 1.0f));
            line(key + glm::vec2(-5.0f, 5.0f), key + glm::vec2(5.0f, -5.0f), 2.0f, glm::vec4(0.55f, 0.75f, 1.0f, 1.0f));
        }
        text(words(hud.use_key, 0), key + glm::vec2(22.0f, -10.0f), 0.5f, glm::vec4(1.0f, 1.0f, 1.0f, 0.92f), ui::TextAlign::Start);
        rect(key + glm::vec2(22.0f, 14.0f), key + glm::vec2(182.0f, 15.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.2f));
        rect(key + glm::vec2(22.0f, 14.0f), key + glm::vec2(22.0f + 160.0f * hud.use_progress, 15.0f), glm::vec4(1.0f, 0.84f, 0.5f, 0.95f));
    }

    {
        const glm::vec2 lo(40.0f, bottom - 26.0f);
        const float width = 190.0f, height = 18.0f + 10.0f * hud.low_health;
        const float shown = std::max({0.4f, 1.0f - hud.health * 0.6f, hud.hurt, hud.heal});
        const glm::vec3 base_color = glm::mix(glm::vec3(0.9f, 0.9f, 0.86f), glm::vec3(0.95f, 0.14f, 0.1f), SurvivalSmooth(hud.low_health * 1.4f));
        const glm::vec3 color = glm::mix(base_color, glm::vec3(0.28f, 0.88f, 0.46f), SurvivalSmooth(hud.heal));
        auto ecg = [](float s) {
            if (s < 0.0f || s > 0.6f) return 0.0f;
            const auto bump = [](float x, float at, float w) { const float d = (x - at) / w; return std::exp(-d * d); };
            return 0.12f * bump(s, 0.07f, 0.026f) - 0.18f * bump(s, 0.132f, 0.007f) + 1.0f * bump(s, 0.15f, 0.0088f) - 0.32f * bump(s, 0.172f, 0.0088f) +
                   0.22f * bump(s, 0.317f, 0.044f);
        };
        constexpr int kSamples = 480;
        constexpr float kWindow = 2.4f;
        constexpr float kStep = kWindow / kSamples;
        const float now = std::floor(hud.run_seconds / kStep) * kStep;
        glm::vec2 last(0.0f);
        for (int i = 0; i <= kSamples; ++i) {
            const float x = static_cast<float>(i) / kSamples;
            const float t = now - static_cast<float>(kSamples - i) * kStep;
            float value = 0.0f;
            if (hud.heart_rate > 0.0f || hud.dead) {
                for (const float b : hud.beats) value += ecg(t - b);
            }
            const glm::vec2 p = lo + glm::vec2(x * width, -value * height);
            if (i > 0) line(last, p, 1.4f, glm::vec4(color, shown * (0.25f + 0.75f * x)));
            last = p;
        }
        rect(last - glm::vec2(1.5f), last + glm::vec2(1.5f), glm::vec4(color, shown));
        if (hud.stamina < 0.999f) {
            const glm::vec2 s = lo + glm::vec2(0.0f, 12.0f);
            rect(s, s + glm::vec2(width, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.18f));
            rect(s, s + glm::vec2(width * hud.stamina, 1.0f), hud.winded ? glm::vec4(0.95f, 0.45f, 0.3f, 0.8f) : glm::vec4(0.9f, 0.9f, 0.86f, 0.7f));
        }
    }

    {
        const float right = UiCanvas::kWidth - 40.0f;
        const float a = hud.ammo_shown;
        static constexpr const char* kNames[] = {"pc_survival_flashlight", "pc_survival_handgun", "pc_survival_shotgun", "pc_survival_magnum",
                                                 "pc_survival_lever", "pc_survival_double"};
        text(words(kNames[static_cast<size_t>(hud.weapon)], 0), {right, bottom - 74.0f}, 0.48f,
             glm::vec4(1.0f, 1.0f, 1.0f, 0.45f + 0.4f * hud.weapon_name_shown), ui::TextAlign::End);
        if (!hud.dead) {
            constexpr float kGap = 16.0f;
            constexpr float kSize = 1.15f;
            const float shown = 0.6f + 0.4f * hud.weapon_name_shown;
            const float row = bottom - (hud.flares > 0 || hud.pipe >= 0.0f ? 154.0f : 134.0f);
            float total = 0.0f;
            int count = 0;
            for (size_t i = 0; i < hud.owned.size(); ++i) {
                if (!hud.owned[i]) continue;
                total += kWeaponPictures[i].width * kSize + (count ? kGap : 0.0f);
                ++count;
            }
            float x = right - total;
            const float row_left = x;
            for (size_t i = 0; i < hud.owned.size(); ++i) {
                if (!hud.owned[i]) continue;
                const WeaponPicture& picture = kWeaponPictures[i];
                const bool in_hand = static_cast<size_t>(hud.weapon) == i;
                const glm::vec4 ink(1.0f, 1.0f, 1.0f, (in_hand ? 0.95f : 0.38f) * shown);
                for (int p = 0; p < picture.count; ++p) {
                    const WeaponPiece& piece = picture.pieces[p];
                    quad({x + piece.x[0] * kSize, row + piece.y[0] * kSize}, {x + piece.x[1] * kSize, row + piece.y[1] * kSize},
                         {x + piece.x[2] * kSize, row + piece.y[2] * kSize}, {x + piece.x[3] * kSize, row + piece.y[3] * kSize}, ink);
                }
                if (in_hand) rect({x, row + 26.0f}, {x + picture.width * kSize, row + 27.0f}, glm::vec4(1.0f, 1.0f, 1.0f, 0.8f * shown));
                if (prompts_.device == PromptDevice::Keyboard && count > 1) {
                    static constexpr const char* kSlotLabel[] = {"6", "1", "2", "3", "4", "5"};
                    text(kSlotLabel[i], {x + picture.width * kSize * 0.5f, row + 29.0f}, 0.40f,
                         glm::vec4(1.0f, 1.0f, 1.0f, (in_hand ? 0.85f : 0.40f) * shown), ui::TextAlign::Center);
                }
                x += picture.width * kSize + kGap;
            }
            if (UiFont* key_font = count > 1 ? assets_.Font(UiFontType::PcSystem, language_) : nullptr) {
                auto draw_prompt = [&](std::string_view token, float size, const glm::vec2& lo, const glm::vec2& hi, float alpha) {
                    const PromptLine prompt = ExpandPromptHint(token, prompts_);
                    ui::TextStyle key_style;
                    key_style.font = &key_font->font;
                    key_style.font_width = key_style.font_height = size;
                    key_style.inline_advances = PromptAdvances(assets_, prompt, prompts_, size);
                    key_style.inline_advance = key_style.inline_advances.empty() ? 0.0f : key_style.inline_advances.front();
                    ui::TextLayout key_layout = ui::LayoutText(prompt.text, key_style, 140.0f);
                    ui::PlaceText(key_layout, lo, hi, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Center, true);
                    DrawText(batch, canvas, *key_font, key_layout, glm::vec4(1.0f, 1.0f, 1.0f, alpha), ui::UiBlend::Alpha);
                    DrawPromptPictures(batch, canvas, assets_, *key_font, key_layout, prompt, prompts_, alpha);
                };
                constexpr float kKey = 17.0f;
                const PromptLine swap_prompt = ExpandPromptHint("{swap}", prompts_);
                ui::TextStyle measure_style;
                measure_style.font = &key_font->font;
                measure_style.font_width = measure_style.font_height = kKey;
                measure_style.inline_advances = PromptAdvances(assets_, swap_prompt, prompts_, kKey);
                measure_style.inline_advance = measure_style.inline_advances.empty() ? 0.0f : measure_style.inline_advances.front();
                const float swap_w = ui::LayoutText(swap_prompt.text, measure_style, 120.0f).width;
                draw_prompt("{swap}", kKey, {row_left - 14.0f - swap_w, row}, {row_left - 14.0f, row + 23.0f}, 0.8f * shown);
                if (prompts_.device != PromptDevice::Keyboard) {
                    draw_prompt("{leftright}", 16.0f, {row_left, row + 29.0f}, {right, row + 47.0f}, 0.72f * shown);
                }
            }
        }
        if (hud.weapon == SurvivalWeapon::Flashlight) {
            const float width = 120.0f;
            rect({right - width, bottom - 10.0f}, {right, bottom - 9.0f}, glm::vec4(1.0f, 1.0f, 1.0f, 0.2f));
            rect({right - width, bottom - 10.0f}, {right - width + width * hud.burst_ready, bottom - 9.0f},
                 glm::vec4(0.85f, 0.92f, 1.0f, hud.burst_ready >= 1.0f ? 0.9f : 0.5f));
        } else {
            const bool empty = hud.clip == 0;
            const glm::vec4 color = empty ? glm::vec4(0.95f, 0.22f, 0.16f, 0.95f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.92f * a);
            const std::string reserve = std::to_string(hud.reserve);
            ui::TextStyle small = base;
            small.font_width *= 0.52f;
            small.font_height *= 0.52f;
            small.text_space *= 0.52f;
            const float reserve_w = ui::LayoutText(reserve, small, 0.0f).width;
            const float y = bottom - 50.0f;
            constexpr float kStackW = 13.0f;
            const float spare_left = right - reserve_w - 5.0f - kStackW;
            const float h = text(std::to_string(hud.clip), {spare_left - 14.0f, y}, 0.95f, color, ui::TextAlign::End);
            const glm::vec4 spare_color = hud.reserve == 0 ? glm::vec4(0.95f, 0.3f, 0.24f, 0.8f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.6f * a);
            const float spare_base = y + h * 0.9f;
            for (int i = 0; i < 3; ++i) {
                const float x0 = spare_left + static_cast<float>(i) * 5.0f;
                const glm::vec4 brass(spare_color.r, spare_color.g * 0.92f, spare_color.b * 0.75f, spare_color.a);
                rect({x0, spare_base - 9.0f}, {x0 + 3.0f, spare_base}, brass);
                rect({x0 + 0.5f, spare_base - 12.0f}, {x0 + 2.5f, spare_base - 9.0f}, spare_color);
            }
            text(reserve, {right, y + h * 0.42f}, 0.52f, spare_color, ui::TextAlign::End);
            const float tick_h = hud.weapon == SurvivalWeapon::Handgun ? 7.0f : 9.0f;
            const float step = hud.weapon == SurvivalWeapon::Handgun ? 4.0f : 7.0f;
            const float base_y = bottom - 9.0f;
            for (int i = 0; i < hud.clip_size; ++i) {
                const float x = right - static_cast<float>(i) * step - 1.0f;
                const bool full_round = i < hud.clip;
                line({x, base_y - 2.0f}, {x, base_y - 2.0f - tick_h}, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, (full_round ? 0.85f : 0.15f) * std::max(a, empty ? 1.0f : 0.0f)));
            }
            const float width = static_cast<float>(hud.clip_size - 1) * step + 2.0f;
            rect({right - width, base_y}, {right, base_y + 1.0f}, glm::vec4(1.0f, 1.0f, 1.0f, 0.2f * a));
            if (hud.reloading) rect({right - width, base_y}, {right - width + width * hud.reload_progress, base_y + 1.0f}, glm::vec4(1.0f, 1.0f, 1.0f, 0.85f));
            if (empty && hud.reserve > 0 && !hud.reloading) {
                const float blink = 0.5f + 0.5f * std::sin(static_cast<float>(survival_pulse_) * 0.25f);
                text(words("pc_survival_reload_hint", 0), {right, bottom - 92.0f}, 0.45f,
                     glm::vec4(0.95f, 0.4f, 0.32f, blink), ui::TextAlign::End);
            }
        }
    }

    if (!hud.dead && (hud.flares > 0 || hud.pipe >= 0.0f)) {
        float x = UiCanvas::kWidth - 40.0f;
        const float y = bottom - 96.0f;
        for (int i = 0; i < hud.flares; ++i) {
            rect({x - 3.0f, y - 12.0f}, {x, y}, glm::vec4(0.75f, 0.12f, 0.1f, 0.85f));
            rect({x - 3.0f, y - 14.0f}, {x, y - 12.0f}, glm::vec4(1.0f, 0.6f, 0.45f, 0.9f));
            x -= 7.0f;
        }
        if (hud.pipe >= 0.0f) {
            if (hud.flares > 0) x -= 8.0f;
            const float w = 40.0f;
            rect({x - w, y - 7.0f}, {x, y - 4.0f}, glm::vec4(0.55f, 0.36f, 0.26f, 0.85f));
            rect({x - 4.0f, y - 11.0f}, {x - 1.0f, y - 4.0f}, glm::vec4(0.55f, 0.36f, 0.26f, 0.85f));
            rect({x - w, y}, {x, y + 1.0f}, glm::vec4(1.0f, 1.0f, 1.0f, 0.2f));
            rect({x - w, y}, {x - w + w * hud.pipe, y + 1.0f}, hud.pipe < 0.25f ? glm::vec4(0.95f, 0.35f, 0.26f, 0.9f) : glm::vec4(0.9f, 0.9f, 0.86f, 0.75f));
        }
    }

    const float sighted = crosshair_.behind_sights ? 0.0f : hud.first_person ? hud.aim : hud.sights;
    if (!survival_free_view_ && sighted < 0.99f && hud.offer_count == 0) {
        const float t = std::tan(std::max(0.2f, hud.fov_y) * 0.5f);
        const float cone = std::clamp(std::tan(hud.spread) / t * 360.0f, 0.0f, 60.0f);
        DrawCrosshair(batch, canvas, middle, crosshair_, cone, (1.0f - 0.45f * hud.aim) * (1.0f - sighted));
    }

    if (!survival_free_view_ && crosshair_.hit_marker) DrawHitMarker(batch, canvas, middle, hud.hit_mark, hud.hit_mark_head, hud.hit_mark_kill);

    if (hud.hurt_arc > 0.0f && !survival_free_view_) {
        const float radius = 150.0f;
        const float a = hud.hurt_angle;
        constexpr int kSteps = 12;
        for (int i = 0; i < kSteps; ++i) {
            const float a0 = a - 0.32f + 0.64f * static_cast<float>(i) / kSteps, a1 = a - 0.32f + 0.64f * static_cast<float>(i + 1) / kSteps;
            const float fade = std::sin(3.14159265f * (static_cast<float>(i) + 0.5f) / kSteps);
            const glm::vec2 p0 = middle + glm::vec2(std::sin(a0), -std::cos(a0)) * radius, p1 = middle + glm::vec2(std::sin(a1), -std::cos(a1)) * radius;
            line(p0, p1, 6.0f, glm::vec4(0.9f, 0.08f, 0.06f, 0.8f * hud.hurt_arc * fade));
        }
    }

    if (hud.banner > 0.0f && !hud.banner_key.empty() && hud.offer_count == 0) {
        const float alpha = std::min(1.0f, hud.banner * 1.5f);
        const bool lisa = hud.banner_key == "pc_survival_lisa";
        const float h = text(words(hud.banner_key, hud.banner_value), {640.0f, 232.0f}, 1.1f,
                             lisa ? glm::vec4(0.95f, 0.3f, 0.5f, alpha) : glm::vec4(1.0f, 1.0f, 1.0f, alpha * 0.95f), ui::TextAlign::Center);
        if (hud.banner_key == "pc_survival_title") {
            static constexpr const char* kLevels[] = {"pc_survival_easy", "pc_survival_normal", "pc_survival_hard", "pc_survival_nightmare", "pc_survival_insanity"};
            std::string sub = words(hud.daily ? "pc_survival_daily" : kLevels[static_cast<size_t>(hud.difficulty)], 0);
            if (hud.daily && hud.daily_mutator >= 0) {
                sub += " - " + words(DailyMutatorById(hud.daily_mutator).name_key, 0);
            }
            text(sub, {640.0f, 236.0f + h}, 0.62f, glm::vec4(1.0f, 1.0f, 1.0f, alpha * 0.68f), ui::TextAlign::Center);
        }
    }
    if (hud.omen > 0.0f && !hud.omen_key.empty() && hud.offer_count == 0 && !assets_.HandFont(language_, NoticeCharacters())) {
        text(words(hud.omen_key, 0), {640.0f, 292.0f}, 0.62f, glm::vec4(0.88f, 0.8f, 0.7f, 0.85f * std::min(1.0f, hud.omen * 1.2f)),
             ui::TextAlign::Center);
    }
    if (hud.offer_count > 0) {
        const bool keyboard = prompts_.device == PromptDevice::Keyboard;
        const float appear = SurvivalSmooth(hud.offer_age / 0.3f);
        const float head = hud.offer_taken < 0 ? appear : appear * (1.0f - SurvivalSmooth(hud.taken_age / 0.5f));
        rect(screen_lo, screen_hi, glm::vec4(0.0f, 0.0f, 0.0f, 0.5f * head));
        constexpr float kW = kOfferCardW, kH = kOfferCardH, kGap = kOfferCardGap;
        const float total = static_cast<float>(hud.offer_count) * kW + static_cast<float>(hud.offer_count - 1) * kGap;
        const float x0 = 640.0f - total * 0.5f;
        const float y0 = kOfferCardTop + 12.0f * (1.0f - appear);
        text(words("pc_survival_offer_title", 0), {640.0f, y0 - 56.0f}, 0.8f, glm::vec4(0.94f, 0.88f, 0.8f, 0.95f * head), ui::TextAlign::Center);
        static constexpr const char* kRoman[] = {"", "I", "II", "III", "IV"};
        for (int i = 0; i < hud.offer_count; ++i) {
            const bool taken = hud.offer_taken == i;
            const float alpha = appear * (hud.offer_taken < 0 ? 1.0f : taken ? 1.0f - SurvivalSmooth((hud.taken_age - 0.7f) / 0.5f)
                                                                             : 1.0f - SurvivalSmooth(hud.taken_age / 0.3f));
            if (alpha <= 0.01f) continue;
            const float x = x0 + static_cast<float>(i) * (kW + kGap), cx = x + 0.5f * kW;
            const glm::vec2 lo(x, y0), hi(x + kW, y0 + kH);
            const glm::vec3 edge = taken ? glm::vec3(1.0f, 0.82f, 0.45f) : glm::vec3(1.0f);
            const bool hover = hud.offer_taken < 0 && offer_hover_ == i;
            rect(lo, hi, glm::vec4(0.03f, 0.03f, 0.03f, 0.8f * alpha));
            if (taken) rect(lo, hi, glm::vec4(1.0f, 0.8f, 0.45f, 0.14f * alpha));
            if (hover) rect(lo, hi, glm::vec4(1.0f, 1.0f, 1.0f, 0.09f * alpha));
            const glm::vec4 border(edge, (taken || hover ? 0.95f : 0.5f) * alpha);
            line(lo, {hi.x, lo.y}, 1.5f, border);
            line({hi.x, lo.y}, hi, 1.5f, border);
            line(hi, {lo.x, hi.y}, 1.5f, border);
            line({lo.x, hi.y}, lo, 1.5f, border);
            const glm::vec2 key(cx, y0 + 28.0f);
            if (keyboard) {
                rect(key - glm::vec2(17.0f), key + glm::vec2(17.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.92f * alpha));
                ui::TextStyle digit_style = base;
                digit_style.font_width *= 0.82f;
                digit_style.font_height *= 0.82f;
                const float digit_h = ui::LayoutText(std::to_string(i + 1), digit_style, 0.0f).height;
                text(std::to_string(i + 1), key - glm::vec2(0.0f, 0.44f * digit_h), 0.82f, glm::vec4(0.02f, 0.02f, 0.02f, alpha), ui::TextAlign::Center);
            } else {
                const glm::vec2 d = i == 0 ? glm::vec2(-1.0f, 0.0f) : i == 1 ? glm::vec2(0.0f, -1.0f) : glm::vec2(1.0f, 0.0f);
                const glm::vec2 n(-d.y, d.x);
                quad(key + d * 14.0f, key - d * 10.0f + n * 12.0f, key - d * 10.0f - n * 12.0f, key + d * 14.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.92f * alpha));
            }
            const SurvivalBoon boon = static_cast<SurvivalBoon>(hud.offer[static_cast<size_t>(i)]);
            const SurvivalBoonSpec& spec = BoonSpec(boon);
            const int rank = hud.offer_rank[static_cast<size_t>(i)];
            if (spec.ranks > 1 && rank >= 2 && rank <= 4) {
                text(kRoman[rank], {cx + 30.0f, y0 + 16.0f}, 0.55f, glm::vec4(1.0f, 0.82f, 0.5f, 0.85f * alpha), ui::TextAlign::Start);
            }
            const std::string name = words(spec.name, 0);
            const glm::vec4 ink = taken ? glm::vec4(1.0f, 0.86f, 0.62f, alpha) : glm::vec4(1.0f, 0.97f, 0.93f, alpha);
            float name_h = 0.0f;
            if (const uint32_t hand = NoticeString(name, kBoonStrip.x, kBoonStrip.y, 40.0f)) {
                float ems = 0.0f;
                for (const char ch : name) {
                    const unsigned char c = static_cast<unsigned char>(ch);
                    if ((c & 0xC0) == 0x80) continue;
                    ems += c >= 0xE3 ? 1.0f : 0.55f;
                }
                const float width = std::min(560.0f, (kW - 24.0f) * static_cast<float>(kBoonStrip.x) / std::max(1.0f, ems * 40.0f));
                const float height = width * static_cast<float>(kBoonStrip.y) / static_cast<float>(kBoonStrip.x);
                const glm::vec2 corner(cx - 0.5f * width, y0 + 50.0f);
                batch.Quad(canvas.ToTarget(corner), canvas.ToTarget(corner + glm::vec2(width, height)), {0.0f, 0.0f}, {1.0f, 1.0f}, ink, ui::UiDrawParams::Plain(hand),
                           ui::UiShade::Material, ui::UiBlend::Alpha);
                name_h = height * 0.78f;
            } else {
                name_h = text(name, {cx, y0 + 58.0f}, 0.7f, ink, ui::TextAlign::Center) + 14.0f;
            }
            ui::TextStyle small = base;
            small.font_width *= 0.62f;
            small.font_height *= 0.62f;
            small.text_space *= 0.62f;
            const float note_w = kW - 28.0f;
            ui::TextLayout note = ui::LayoutText(words(std::string(spec.name) + "_note", 0), small, note_w);
            const float note_y = y0 + 50.0f + name_h;
            ui::PlaceText(note, {cx - 0.5f * note_w, note_y}, {cx + 0.5f * note_w, hi.y - 10.0f}, ui::TextAlign::Center, ui::TextAlign::Start,
                          ui::TextAlign::Center);
            DrawText(batch, canvas, *font, note, glm::vec4(0.9f, 0.9f, 0.88f, 0.92f * alpha), ui::UiBlend::Alpha, ui::UiShade::Border);
        }
    }
    if (hud.kept > 0.0f) {
        text(words("pc_survival_kept", hud.kept_wave), {640.0f, 560.0f}, 0.5f, glm::vec4(0.85f, 0.9f, 1.0f, std::min(1.0f, hud.kept * 1.5f) * 0.85f),
             ui::TextAlign::Center);
    }
    if (hud.pickup_note > 0.0f && !hud.pickup_key.empty()) {
        const std::string line = words(hud.pickup_key, hud.pickup_value);
        const glm::vec4 color(1.0f, 0.92f, 0.75f, std::min(1.0f, hud.pickup_note * 1.5f));
        if (HasPromptTokens(line)) {
            DrawPromptText(batch, canvas, assets_, *font, line, prompts_, 22.0f, {240.0f, 506.0f}, {1040.0f, 546.0f}, ui::TextAlign::Center, ui::TextAlign::Center, color, true);
        } else {
            text(line, {640.0f, 520.0f}, 0.8f, color, ui::TextAlign::Center);
        }
    }
    (void)screen_lo;
    (void)screen_hi;
}

void GameUi::DrawFpsCounter(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    UiFont* font = assets_.Font(UiFontType::Movie, language_);
    if (!font || canvas.scale <= 0.0f || fps_rendered_ <= 0) return;
    ui::TextStyle style;
    style.font = &font->font;
    if (const UiFontStyle* s = assets_.FontStyle(kSubtitleFont, language_)) {
        style.font_width = s->width;
        style.font_height = s->height;
        style.text_space = s->text_space;
        style.line_space = s->line_space;
    }
    ui::NormalizeSubtitleStyle(style);
    constexpr float kScale = 0.75f;
    style.font_width *= kScale;
    style.font_height *= kScale;
    style.text_space *= kScale;
    const float pitch = ui::LayoutText("0", style, 0.0f).width;
    const float bar = LetterboxBar(full, letterbox_);
    const float top = std::max(22.0f, (bar - canvas.origin.y) / canvas.scale + 12.0f);
    float right = UiCanvas::kWidth - 34.0f;
    struct Part {
        std::string text;
        glm::vec4 color;
        bool digits;
    };
    std::vector<Part> parts;
    parts.push_back({" FPS", glm::vec4(1.0f, 1.0f, 1.0f, 0.55f), false});
    const bool generated = fps_presented_ > fps_rendered_;
    parts.push_back({std::to_string(generated ? fps_presented_ : fps_rendered_), glm::vec4(1.0f, 1.0f, 1.0f, 0.9f), true});
    if (generated) {
        parts.push_back({"/", glm::vec4(1.0f, 1.0f, 1.0f, 0.45f), false});
        parts.push_back({std::to_string(fps_rendered_), glm::vec4(1.0f, 1.0f, 1.0f, 0.6f), true});
    }
    const float left_limit = UiCanvas::kWidth * 0.86f;
    for (const Part& part : parts) {
        ui::TextLayout layout = ui::LayoutText(part.text, style, 0.0f);
        const float width = part.digits ? pitch * static_cast<float>(std::max<size_t>(part.text.size(), 2)) : layout.width;
        const float x = std::max(left_limit, right - width);
        ui::PlaceText(layout, {x, top}, {x + width, top + layout.height}, ui::TextAlign::End, ui::TextAlign::Start, ui::TextAlign::End);
        DrawText(batch, canvas, *font, layout, part.color, ui::UiBlend::Alpha, ui::UiShade::Border);
        right = x;
    }
}

void GameUi::ShowUpdateNotice(std::string note) {
    if (update_notice_given_ || note.empty()) return;
    update_notice_given_ = true;
    ShowNotice(std::move(note));
}

void GameUi::ShowNotice(std::string note) {
    if (note.empty()) return;
    if (notices_.Push(note)) LogInfo("notice: queued {}", note);
}

void GameUi::UpdateUpdateNotice(Game& game, float dt) {
    const bool allowed = game.Controller().Step() == 15 && !game.Demos().HasActiveCamera() && !menu_suspended_ && !update_notice_held_ &&
                         !lisa_date_ && fade_.a < 0.5f && !(menu_.IsOpen() && (menu_.FirstBoot() || menu_.PictureFullScreen())) && !ShaderScreenShown() &&
                         save_dialog_text_.empty();
    const bool was_done = notices_.Done();
    const std::string before = notices_.Current();
    const float seconds = notices_.Time();
    notices_.Update(dt, allowed);
    if (notices_.Visible() && notices_.Time() <= 0.0f) LogInfo("notice: shown {}", notices_.Current());
    if (notices_.Restarted()) LogInfo("notice: taken away {} after {:.1f} s, too soon to be read, it starts again", before, seconds);
    if (notices_.Done() && !was_done) {
        if (notices_.Cut()) LogInfo("notice: cut {} after {:.1f} s (taken away by a demo, a fade, a dialog or the photo mode, not shown again)", before, notices_.Time());
        else LogInfo("notice: ended {} after {:.1f} s", before, notices_.Time());
    }
}

void GameUi::DrawUpdateNotice(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    if (!notices_.Visible() || canvas.scale <= 0.0f) return;
    UiFont* font = assets_.Font(UiFontType::PcSystem, language_);
    if (!font) return;
    const std::string text = PcNoteText(notices_.Current(), language_);
    if (text.empty()) return;
    const float fade = notices_.Alpha();
    ui::TextStyle style;
    style.font = &font->font;
    style.font_width = style.font_height = 18.0f;
    const float bar = LetterboxBar(full, letterbox_);
    float top = std::max(14.0f, (bar - canvas.origin.y) / canvas.scale + 12.0f);
    if (speedrun_view_.shown && !menu_.IsOpen()) top = std::max(top, speedrun_bottom_ + 8.0f);
    const glm::vec2 lo(UiCanvas::kWidth * 0.15f, top);
    const glm::vec2 hi(UiCanvas::kWidth * 0.85f, top + 26.0f);
    ui::TextLayout layout = ui::LayoutTextInBox(text, style, glm::vec2(0.0f), hi - lo);
    ui::PlaceText(layout, lo, hi, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Start, true);
    DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, 0.55f * fade), ui::UiBlend::Alpha);
    if (notices_.Current() == kUpdatedNotice && support_place_ == Support::Notice) {
        support_rect_place_ = Support::Notice;
        DrawSupportButton(batch, canvas, PcText("pc_support_port", language_), glm::vec2(UiCanvas::kWidth * 0.5f, hi.y + 24.0f), ui::TextAlign::Center,
                          glm::vec3(1.0f), glm::vec3(0.0f), fade);
    }
}

std::string_view GameUi::SupportPlace() const {
    switch (support_place_) {
        case Support::Menu: return "menu";
        case Support::Notice: return "notice";
        case Support::Credits: return "credits";
        default: return "none";
    }
}

void GameUi::UpdateSurvivalOffer(Game& game, MenuInput& menu_input) {
    survival_choosing_ = game.SurvivalChoosing() && !menu_.IsOpen();
    offer_hover_ = -1;
    if (!survival_choosing_) {
        offer_pointer_ = glm::vec2(-1.0f);
        return;
    }
    const SurvivalHud& hud = game.Survival()->Hud();
    if (hud.offer_count <= 0 || hud.offer_taken >= 0) return;
    if (menu_input.pointer_valid) {
        offer_pointer_ = glm::vec2(UiCanvas::kWidth * 0.5f + menu_input.pointer.x * UiCanvas::kUnit, UiCanvas::kHeight * 0.5f - menu_input.pointer.y * UiCanvas::kUnit);
    }
    const glm::vec2 px = offer_pointer_;
    const float total = static_cast<float>(hud.offer_count) * kOfferCardW + static_cast<float>(hud.offer_count - 1) * kOfferCardGap;
    const float x0 = 640.0f - total * 0.5f;
    for (int i = 0; i < hud.offer_count; ++i) {
        const float x = x0 + static_cast<float>(i) * (kOfferCardW + kOfferCardGap);
        if (px.x >= x && px.x <= x + kOfferCardW && px.y >= kOfferCardTop && px.y <= kOfferCardTop + kOfferCardH) offer_hover_ = i;
    }
    if (offer_hover_ >= 0 && menu_input.click) {
        game.ChooseSurvivalOffer(offer_hover_);
        menu_input.click = false;
    }
}

void GameUi::UpdateSupport(Game& game, const InputState& input, MenuInput& menu_input) {
    (void)game;
    Support place = Support::None;
    int focus = 0;
    const bool menu_shown = menu_.CurrentState() == OptionsMenu::State::Active || menu_.CurrentState() == OptionsMenu::State::SetIn;
    const bool menu_original = menu_.IsOpen() && !menu_suspended_ && menu_shown && menu_.CurrentPage() == OptionsMenu::Page::Original &&
                               menu_.DialogKind(&focus) == "none";
    if (support_prompts_ && menu_original) {
        place = Support::Menu;
    } else if (support_prompts_ && port_credits_time_ >= 0.0f && !menu_.IsOpen()) {
        place = Support::Credits;
    } else if (SupportNoticeShown()) {
        place = Support::Notice;
    }
    if (place != support_place_) {
        support_place_ = place;
        LogInfo("support: button {}", SupportPlace());
    }
    const bool pressed = std::exchange(support_script_press_, false) || input.support_pressed;
    support_hover_ = false;
    if (place == Support::None) return;
    bool clicked = false;
    if (menu_input.pointer_valid && support_rect_place_ == place && support_rect_.z > support_rect_.x) {
        const glm::vec2 px(UiCanvas::kWidth * 0.5f + menu_input.pointer.x * UiCanvas::kUnit, UiCanvas::kHeight * 0.5f - menu_input.pointer.y * UiCanvas::kUnit);
        support_hover_ = px.x >= support_rect_.x - 3.0f && px.x <= support_rect_.z + 3.0f && px.y >= support_rect_.y - 3.0f && px.y <= support_rect_.w + 3.0f;
        clicked = support_hover_ && menu_input.click;
    }
    if (pressed || clicked) {
        support_request_ = std::string(SupportPlace());
        LogInfo("support: Support on Patreon used ({}, {})", SupportPlace(), clicked ? "click" : "key");
        if (clicked) menu_input.click = false;
    }
}

void GameUi::DrawSupportButton(ui::UiBatch& batch, const UiCanvas& canvas, std::string_view label, glm::vec2 anchor, ui::TextAlign align,
                               glm::vec3 ink, glm::vec3 plate, float alpha, bool framed) {
    UiFont* font = assets_.Font(UiFontType::PcSystem, language_);
    if (!font || alpha <= 0.0f || canvas.scale <= 0.0f || label.empty()) return;
    const float kSize = framed ? 17.0f : 20.0f;
    const PromptLine line = ExpandPromptHint("{support} " + std::string(label), prompts_);
    ui::TextStyle style;
    style.font = &font->font;
    style.font_width = style.font_height = kSize;
    style.inline_advances = PromptAdvances(assets_, line, prompts_, kSize);
    style.inline_advance = style.inline_advances.empty() ? 0.0f : style.inline_advances.front();
    ui::TextLayout layout = ui::LayoutText(line.text, style, UiCanvas::kWidth * 0.6f);
    const glm::vec2 pad(12.0f, 6.0f);
    const glm::vec2 size(std::round(layout.width + 2.0f * pad.x), std::round(layout.height + 2.0f * pad.y));
    const glm::vec2 lo = glm::round(align == ui::TextAlign::End ? glm::vec2(anchor.x - size.x, anchor.y - 0.5f * size.y) : anchor - 0.5f * size);
    const glm::vec2 hi = lo + size;
    const float hover = support_hover_ ? 1.0f : 0.0f;
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    const auto rect = [&](glm::vec2 a, glm::vec2 b, glm::vec4 color) {
        batch.Quad(canvas.ToTarget(a), canvas.ToTarget(b), glm::vec2(0.0f), glm::vec2(1.0f), color, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    if (framed) {
        rect(lo, hi, glm::vec4(plate, (0.42f + 0.18f * hover) * alpha));
        const glm::vec4 edge(ink, (0.32f + 0.4f * hover) * alpha);
        rect(lo, glm::vec2(hi.x, lo.y + 1.0f), edge);
        rect(glm::vec2(lo.x, hi.y - 1.0f), hi, edge);
        rect(glm::vec2(lo.x, lo.y + 1.0f), glm::vec2(lo.x + 1.0f, hi.y - 1.0f), edge);
        rect(glm::vec2(hi.x - 1.0f, lo.y + 1.0f), glm::vec2(hi.x, hi.y - 1.0f), edge);
    }
    ui::PlaceText(layout, lo + pad, hi - pad, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Center, true);
    DrawText(batch, canvas, *font, layout, glm::vec4(ink, (0.8f + 0.2f * hover) * alpha), ui::UiBlend::Alpha);
    DrawPromptPictures(batch, canvas, assets_, *font, layout, line, prompts_, framed ? alpha : alpha * (0.8f + 0.2f * hover));
    support_rect_ = glm::vec4(lo, hi);
}

void GameUi::DrawLisaVignette(ui::UiBatch& batch, glm::vec2 full, float strength) {
    constexpr int kSegments = 64;
    constexpr float kRadius[] = {0.52f, 0.72f, 0.88f, 1.0f, 1.6f};
    constexpr float kShare[] = {0.0f, 0.22f, 0.58f, 0.85f, 1.0f};
    const glm::vec2 middle = full * 0.5f;
    const glm::vec2 half = middle * 1.4143f;
    const glm::vec3 pink(1.0f, 0.42f, 0.66f);
    std::vector<ui::UiVertex> v;
    v.reserve(kSegments * 6 * 4);
    for (int ring = 0; ring + 1 < static_cast<int>(std::size(kRadius)); ++ring) {
        const glm::vec4 inner(pink, std::clamp(strength * kShare[ring], 0.0f, 1.0f));
        const glm::vec4 outer(pink, std::clamp(strength * kShare[ring + 1], 0.0f, 1.0f));
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = 6.2831853f * static_cast<float>(i) / kSegments;
            const float a1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
            const glm::vec2 d0(std::cos(a0), std::sin(a0));
            const glm::vec2 d1(std::cos(a1), std::sin(a1));
            const glm::vec2 p00 = middle + half * d0 * kRadius[ring];
            const glm::vec2 p01 = middle + half * d1 * kRadius[ring];
            const glm::vec2 p10 = middle + half * d0 * kRadius[ring + 1];
            const glm::vec2 p11 = middle + half * d1 * kRadius[ring + 1];
            v.push_back({p00, {0, 0}, inner});
            v.push_back({p10, {0, 0}, outer});
            v.push_back({p11, {0, 0}, outer});
            v.push_back({p00, {0, 0}, inner});
            v.push_back({p11, {0, 0}, outer});
            v.push_back({p01, {0, 0}, inner});
        }
    }
    batch.Draw(v, ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
}

void GameUi::Record(gpu::CommandList& cmd, gpu::ImageViewHandle target, gpu::Extent2D extent, Part part) {
    (void)target;
    if (!ready_ || !batch_) {
        top_pending_ = false;
        return;
    }
    ui::UiBatch& batch = *batch_;
    if (part == Part::Top) {
        if (top_pending_) {
            batch.RecordTop(cmd, extent);
        }
        return;
    }
    batch.Begin(extent);
    batch.SetCoverageAlpha(vr_hud_);
    const UiCanvas canvas = UiCanvas::Fit(extent);
    canvas_ = canvas;
    canvas_ready_ = true;
    batch.SetScreenArea(canvas.origin, UiCanvas::kHeight * canvas.scale);
    const glm::vec2 full(static_cast<float>(extent.width), static_cast<float>(extent.height));
    struct Layer {
        int priority;
        std::function<void()> draw;
    };
    std::vector<Layer> layers;
    if (overlay_) {
        layers.push_back({kOverlayPriority, [&] {
                              const glm::vec2 lo = canvas.origin;
                              const glm::vec2 hi = canvas.origin + glm::vec2(UiCanvas::kWidth, UiCanvas::kHeight) * canvas.scale;
                              batch.Quad(lo, hi, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f), ui::UiDrawParams::Plain(overlay_texture_),
                                         ui::UiShade::Material, ui::UiBlend::Alpha);
                              const ui::UiDrawParams black = ui::UiDrawParams::Plain(TextureManager::kWhite);
                              const glm::vec4 color(0.0f, 0.0f, 0.0f, 1.0f);
                              const glm::vec2 margins[4][2] = {{{0.0f, 0.0f}, {lo.x, full.y}}, {{hi.x, 0.0f}, full},
                                                               {{lo.x, 0.0f}, {hi.x, lo.y}}, {{lo.x, hi.y}, {hi.x, full.y}}};
                              for (const auto& [a, b] : margins) {
                                  if (b.x > a.x && b.y > a.y) {
                                      batch.Quad(a, b, glm::vec2(0.0f), glm::vec2(1.0f), color, black, ui::UiShade::Solid, ui::UiBlend::Alpha);
                                  }
                              }
                          }});
    }
    if (!survival_hud_.active || (!survival_hud_.caught && !survival_hud_.dead)) {
        layers.push_back({kSubliminalStringPriority, [&] { DrawSubliminal(batch, true); }});
        layers.push_back({kSubliminalNoisePriority, [&] { DrawSubliminal(batch, false); }});
    }
    layers.push_back({strong_subtitles_ ? kStrongSubtitlePriority : kSubtitlePriority, [&] { DrawSubtitle(batch, canvas); }});
    if (!sound_captions_.empty() && !vr_hud_) {
        layers.push_back({strong_subtitles_ ? kStrongSubtitlePriority : kSubtitlePriority, [&] { DrawSoundCaptions(batch, canvas, full); }});
    }
    if (lisa_vignette_ > 0.0f && !vr_hud_) {
        layers.push_back({kLisaVignettePriority, [&] { DrawLisaVignette(batch, full, lisa_vignette_); }});
    }
    if (LetterboxBar(full, letterbox_) > 0.0f && !vr_hud_) {
        layers.push_back({kLetterboxPriority, [&] { DrawLetterbox(batch, full, letterbox_); }});
    }
    if (fade_.a > 0.0f && !vr_hud_) {
        layers.push_back({fade_priority_, [&] {
                              batch.Quad(glm::vec2(0.0f), full, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(glm::vec3(fade_), std::clamp(fade_.a, 0.0f, 1.0f)),
                                         ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
                          }});
    }
    if (menu_.IsOpen() && !menu_suspended_) {
        layers.push_back({kPauseMenuPriority, [&] { menu_.Draw(batch, canvas); }});
    }
    if (support_place_ == Support::Menu && menu_.IsOpen() && !menu_suspended_ && !vr_hud_) {
        layers.push_back({kPauseMenuPriority, [&] {
                              support_rect_place_ = Support::Menu;
                              DrawSupportButton(batch, canvas, PcText("pc_support_patreon", language_), glm::vec2(1198.0f, 670.0f),
                                                ui::TextAlign::End, glm::vec3(1.0f), glm::vec3(0.0f), menu_.ShownAlpha(), false);
                          }});
    }
    if (!theater_title_.empty() || !theater_hint_.empty()) {
        layers.push_back({SaveIcon::kPriority - 1, [&] { DrawTheaterHint(batch, canvas); }});
    }
    if (demo_ui_.Active()) {
        layers.push_back({DemoUi::kPriority, [&] {
                              if (const glm::vec4 color = demo_ui_.BugPillarColor(); color.a > 0.0f) {
                                  const float left = canvas.origin.x;
                                  const float right = canvas.origin.x + UiCanvas::kWidth * canvas.scale;
                                  const auto plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
                                  if (left > 0.5f) batch.Quad(glm::vec2(0.0f), glm::vec2(left, full.y), glm::vec2(0.0f), glm::vec2(1.0f), color, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
                                  if (full.x - right > 0.5f) batch.Quad(glm::vec2(right, 0.0f), full, glm::vec2(0.0f), glm::vec2(1.0f), color, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
                              }
                              demo_ui_.Draw(batch, canvas, language_);
                          }});
    }
    if (boot_splash_ != 0 && boot_splash_time_ > 0.0f) {
        layers.push_back({SaveIcon::kPriority - 2, [&] {
                              const glm::vec2 lo = canvas.origin;
                              const glm::vec2 hi = canvas.origin + glm::vec2(UiCanvas::kWidth, UiCanvas::kHeight) * canvas.scale;
                              const float alpha = std::clamp(boot_splash_time_ / 0.6f, 0.0f, 1.0f);
                              batch.Quad(lo, hi, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, alpha),
                                         ui::UiDrawParams::Plain(boot_splash_), ui::UiShade::Material, ui::UiBlend::Alpha);
                          }});
    }
    if (!shader_screen_.empty() && canvas.scale > 0.0f) {
        layers.push_back({kShaderScreenPriority, [&] {
                              batch.Quad(glm::vec2(0.0f), full, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
                                         ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
                              UiFont* font = assets_.Font(UiFontType::PcSystem, language_);
                              const std::string text = font ? PcNoteText(shader_screen_, language_) : std::string();
                              if (text.empty()) return;
                              ui::TextStyle style;
                              style.font = &font->font;
                              style.font_width = style.font_height = 18.0f;
                              const glm::vec2 lo(UiCanvas::kWidth * 0.15f, UiCanvas::kHeight * 0.5f - 13.0f);
                              const glm::vec2 hi(UiCanvas::kWidth * 0.85f, UiCanvas::kHeight * 0.5f + 13.0f);
                              ui::TextLayout layout = ui::LayoutText(text, style, hi.x - lo.x);
                              ui::PlaceText(layout, lo, hi, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Start, true);
                              DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, 0.75f), ui::UiBlend::Alpha);
                          }});
    }
    if (save_icon_.Visible()) {
        layers.push_back({SaveIcon::kPriority, [&] { save_icon_.Draw(batch, canvas); }});
    }
    if (!save_dialog_text_.empty()) {
        layers.push_back({kSaveDialogPriority, [&] { DrawSaveDialog(batch, canvas, full); }});
    }
    if (port_credits_time_ >= 0.0f && !menu_.IsOpen()) {
        layers.push_back({kPortCreditsPriority, [&] { DrawPortCredits(batch, canvas, full); }});
    }
    if (ending_skip_alpha_ > 0.0f && !vr_hud_) {
        layers.push_back({kEndingSkipPriority, [&] { DrawEndingSkip(batch, canvas); }});
    }
    if (speedrun_view_.shown && !menu_.IsOpen()) {
        layers.push_back({kSpeedrunPriority, [&] { DrawSpeedrun(batch, canvas, full); }});
    }
    if (fps_shown_ && !menu_.IsOpen() && !vr_hud_) {
        layers.push_back({kSpeedrunPriority, [&] { DrawFpsCounter(batch, canvas, full); }});
    }
    if (survival_hud_.active && !survival_hud_.caught && !menu_.IsOpen()) {
        layers.push_back({kSurvivalPriority, [&] {
            DrawSurvival(batch, canvas, full);
            DrawAccusation(batch, canvas, full);
        }});
    }
    if (UpdateNoticeShown() && !vr_hud_) {
        layers.push_back({kUpdateNoticePriority, [&] { DrawUpdateNotice(batch, canvas, full); }});
    }
    if (lisa_date_) {
        layers.push_back({LisaDate::kUiPriority, [&] {
                              ui::UiDrawParams noise = ui::UiDrawParams::Plain(noise_texture_);
                              noise.textures[1] = noise_normal_texture_;
                              noise.extra2 = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
                              noise.extra2.z = full.y > 0.0f ? (full.x / full.y) / (16.0f / 9.0f) : 1.0f;
                              lisa_date_->DrawUi(batch, canvas, full, noise);
                          }});
    }
    std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) { return a.priority < b.priority; });
    for (const Layer& layer : layers) {
        if (part == Part::Below && layer.priority >= kGrainPriority && !batch.HasTop()) {
            batch.MarkTop();
        }
        layer.draw();
    }
    if (part == Part::Below) {
        top_pending_ = batch.HasTop();
        batch.RecordBelow(cmd, extent);
    } else {
        batch.Record(cmd, extent);
    }
}

namespace {

constexpr uint16_t kOptionContent = 39;
constexpr uint16_t kOptionHeaderText = 41;
constexpr uint16_t kOptionTipText = 48;
constexpr uint16_t kOptionHeaderLine = 74;
constexpr uint16_t kOptionBarGroup = 80;
constexpr uint16_t kOptionBar = 81;
constexpr uint16_t kOptionEntryGroup = 120;
constexpr uint16_t kOptionEntryText = 121;
constexpr uint16_t kOptionTitle = 150;
constexpr float kPhotoScale = 0.8f;
constexpr float kPhotoLabelX = -58.0f;
constexpr float kPhotoValueX = kPhotoLabelX + 32.0f * kPhotoScale;
constexpr float kPhotoArrow = 12.0f * kPhotoScale;
constexpr float kPhotoTitleY = 32.0f;
constexpr float kPhotoTop = 27.5f;
constexpr float kPhotoBottom = -35.0f;
constexpr float kPhotoTipRoom = 11.6f;
constexpr float kPhotoRowStep = 3.0f * kPhotoScale;
constexpr float kPhotoSectionGap = 3.5f * kPhotoScale;
constexpr float kPhotoPanelRight = -14.0f;
constexpr float kPhotoFade = 8.0f;

glm::vec2 VirtualPx(glm::vec2 units) {
    return {UiCanvas::kWidth * 0.5f + units.x * UiCanvas::kUnit, UiCanvas::kHeight * 0.5f - units.y * UiCanvas::kUnit};
}

glm::vec2 OwnScale(const ui::UifNode& node) {
    return node.type == ui::UifNodeType::Root ? glm::vec2(1.0f) : glm::abs(node.scale);
}

ui::TextAlign NodeVerticalAlign(const ui::UifNode& node) {
    switch ((node.text_flags >> 7) & 3) {
    case 1: return ui::TextAlign::Start;
    case 2: return ui::TextAlign::Center;
    case 3: return ui::TextAlign::End;
    default: {
        const float lo = -node.box_min.y;
        const float hi = -node.box_max.y;
        if (std::abs(lo + hi) < 1e-4f) return ui::TextAlign::Center;
        return std::abs(lo) < 1e-4f ? ui::TextAlign::Start : ui::TextAlign::End;
    }
    }
}

ui::TextStyle NodeTextStyle(const UiFont& font, const UiFontStyle* style, glm::vec2 scale) {
    ui::TextStyle text_style;
    text_style.font = &font.font;
    if (style) {
        text_style.font_width = style->width;
        text_style.font_height = style->height;
        text_style.text_space = style->text_space;
        text_style.line_space = style->line_space;
    }
    const float s = std::sqrt(scale.x * scale.y);
    text_style.font_width *= s;
    text_style.font_height *= s;
    text_style.text_space *= s;
    text_style.line_space *= s;
    return text_style;
}

}

void GameUi::DrawPhotoPanel(ui::UiBatch& batch, const UiCanvas& canvas, const PhotoPanelView& view) {
    const ui::UifModel* model = menu_.Model();
    const ui::UifNode* content = model ? model->FindById(kOptionContent) : nullptr;
    if (!model || !content) {
        return;
    }
    const int language = std::clamp(view.language, 0, UiAssets::kLanguageCount - 1);
    const glm::vec2 root_scale = OwnScale(*content);
    const glm::vec4 root_color = content->color;
    const float bottom = static_cast<float>(batch.Extent().height);

    {
        const float solid = canvas.FromUnits({kPhotoPanelRight - kPhotoFade, 0.0f}).x;
        const float clear = canvas.FromUnits({kPhotoPanelRight, 0.0f}).x;
        const glm::vec4 dark(0.0f, 0.0f, 0.0f, 0.72f);
        const glm::vec4 none(0.0f);
        const ui::UiVertex v[] = {
            {{0.0f, 0.0f}, {0.0f, 0.0f}, dark}, {{solid, 0.0f}, {1.0f, 0.0f}, dark}, {{solid, bottom}, {1.0f, 1.0f}, dark},
            {{0.0f, 0.0f}, {0.0f, 0.0f}, dark}, {{solid, bottom}, {1.0f, 1.0f}, dark}, {{0.0f, bottom}, {0.0f, 1.0f}, dark},
            {{solid, 0.0f}, {0.0f, 0.0f}, dark}, {{clear, 0.0f}, {1.0f, 0.0f}, none}, {{clear, bottom}, {1.0f, 1.0f}, none},
            {{solid, 0.0f}, {0.0f, 0.0f}, dark}, {{clear, bottom}, {1.0f, 1.0f}, none}, {{solid, bottom}, {0.0f, 1.0f}, dark},
        };
        batch.Draw(v, ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
    }

    UiFont* font = assets_.Font(UiFontType::PcSystem, language);
    auto text = [&](uint16_t id, glm::vec2 parent, std::string_view value, float x0, float x1, float y, ui::TextAlign h, glm::vec4 color,
                    bool mirror = false) {
        const ui::UifNode* node = model->FindById(id);
        if (!node || !font || value.empty()) return;
        const glm::vec2 scale = root_scale * parent * OwnScale(*node) * kPhotoScale;
        ui::TextStyle text_style = NodeTextStyle(*font, assets_.FontStyleByHash(node->font_name, language), scale);
        const float y0 = node->box_min.y * node->size.y * scale.y;
        const float y1 = node->box_max.y * node->size.y * scale.y;
        const glm::vec2 a = VirtualPx({x0, y + std::max(y0, y1)});
        const glm::vec2 b = VirtualPx({x1, y + std::min(y0, y1)});
        ui::TextLayout layout = ui::LayoutText(value, text_style, 0.0f);
        const float room = b.x - a.x;
        if (layout.width > room && layout.width > 0.0f) {
            const float fit = room / layout.width;
            text_style.font_width *= fit;
            text_style.font_height *= fit;
            text_style.text_space *= fit;
            text_style.line_space *= fit;
            layout = ui::LayoutText(value, text_style, 0.0f);
        }
        ui::PlaceText(layout, a, b, h, h, NodeVerticalAlign(*node), mirror);
        DrawText(batch, canvas, *font, layout, color, node->Additive() ? ui::UiBlend::Additive : ui::UiBlend::Alpha);
    };
    auto box_text = [&](uint16_t id, std::string_view value, glm::vec2 lo, glm::vec2 hi, glm::vec4 color) {
        const ui::UifNode* node = model->FindById(id);
        if (!node || !font || value.empty()) return;
        ui::TextStyle text_style =
            NodeTextStyle(*font, assets_.FontStyleByHash(node->font_name, language), root_scale * OwnScale(*node) * kPhotoScale);
        const PromptLine line = ExpandPromptHint(value, prompts_);
        text_style.inline_advances = PromptAdvances(assets_, line, prompts_, text_style.font_height);
        text_style.inline_advance = text_style.inline_advances.empty() ? 0.0f : text_style.inline_advances.front();
        const ui::TextLayout layout = ui::LayoutTextInBox(line.text, text_style, VirtualPx({lo.x, hi.y}), VirtualPx({hi.x, lo.y}), true);
        DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha);
        DrawPromptPictures(batch, canvas, assets_, *font, layout, line, prompts_, color.a);
    };
    auto mesh = [&](uint16_t id, glm::vec2 parent, glm::vec2 at, glm::vec4 color) {
        const ui::UifNode* node = model->FindById(id);
        if (!node || node->type != ui::UifNodeType::Mesh) return;
        std::array<uint32_t, 4> textures{TextureManager::kWhite, TextureManager::kWhite, TextureManager::kWhite, TextureManager::kWhite};
        for (int slot = 0; slot < 4; ++slot) {
            const int t = node->material.textures[slot];
            if (t >= 0 && t < static_cast<int>(model->Textures().size())) {
                textures[slot] = assets_.Texture(model->Textures()[t]);
                textures[slot] |= assets_.TextureAddressBits(textures[slot]);
            }
        }
        const glm::vec2 scale = root_scale * parent * OwnScale(*node) * kPhotoScale;
        std::vector<ui::UiVertex> vertices;
        for (const uint16_t i : node->indices) {
            if (i >= node->positions.size() || i >= node->uvs.size()) continue;
            vertices.push_back({canvas.FromUnits(at + node->positions[i] * node->size * scale), node->uvs[i], color * node->color});
        }
        batch.Draw(vertices, ui::UiDrawParams::FromMaterial(textures, node->material.params), ui::UiShade::Material,
                   node->Additive() ? ui::UiBlend::Additive : ui::UiBlend::Alpha);
    };
    auto tint = [&](float alpha) { return root_color * glm::vec4(1.0f, 1.0f, 1.0f, alpha); };
    auto node_color = [&](uint16_t id) {
        const ui::UifNode* node = model->FindById(id);
        return node ? node->color : glm::vec4(1.0f);
    };
    const ui::UifNode* entry_group = model->FindById(kOptionEntryGroup);
    const ui::UifNode* bar_group = model->FindById(kOptionBarGroup);
    const glm::vec2 group_scale = entry_group ? OwnScale(*entry_group) : glm::vec2(1.0f);
    const float text_right = kPhotoPanelRight - kPhotoFade;

    text(kOptionTitle, glm::vec2(1.0f), PcText("pc_photo_title", language), kPhotoLabelX, text_right, kPhotoTitleY, ui::TextAlign::Start,
         root_color * node_color(kOptionTitle));
    int sections = 0;
    for (const PhotoPanelRow& row : view.rows) sections += row.section ? 1 : 0;
    const int steps = static_cast<int>(view.rows.size());
    const float room = kPhotoTop - (kPhotoBottom + kPhotoTipRoom) - std::max(0, sections - 1) * (kPhotoSectionGap - kPhotoRowStep);
    const float row_step = steps > 0 ? std::min(kPhotoRowStep, room / static_cast<float>(steps)) : kPhotoRowStep;
    float y = kPhotoTop;
    bool first = true;
    for (const PhotoPanelRow& row : view.rows) {
        if (row.section) {
            if (!first) y -= kPhotoSectionGap - kPhotoRowStep;
            text(kOptionHeaderText, glm::vec2(1.0f), row.label, kPhotoLabelX, kPhotoValueX + kPhotoArrow + 2.0f, y, ui::TextAlign::Start,
                 root_color * node_color(kOptionHeaderText));
            mesh(kOptionHeaderLine, glm::vec2(1.0f), glm::vec2(kPhotoLabelX, y) + glm::vec2(16.0f, -1.0f) * kPhotoScale, root_color);
            y -= row_step;
            first = false;
            continue;
        }
        first = false;
        text(kOptionHeaderText, glm::vec2(1.0f), row.label, kPhotoLabelX, kPhotoValueX - kPhotoArrow - 1.0f, y, ui::TextAlign::Start, tint(1.0f),
             true);
        if (row.selected) {
            mesh(kOptionBar, bar_group ? OwnScale(*bar_group) : glm::vec2(1.0f), glm::vec2(kPhotoValueX, y),
                 tint(menu_.SelectionFlash(view.flash_frame)));
        }
        text(kOptionEntryText, group_scale, row.value, kPhotoValueX - kPhotoArrow + 1.5f, kPhotoValueX + kPhotoArrow - 1.5f, y,
             ui::TextAlign::Center, tint(row.selected ? 1.0f : 0.65f) * node_color(kOptionEntryText));
        if (row.adjustable) {
            for (const int delta : {-1, 1}) {
                const bool available = delta < 0 ? row.can_decrease : row.can_increase;
                const float x = kPhotoValueX + kPhotoArrow * static_cast<float>(delta);
                text(kOptionEntryText, glm::vec2(1.0f), delta < 0 ? "<" : ">", x - 1.5f, x + 1.5f, y, ui::TextAlign::Center,
                     tint(available ? (row.selected ? 1.0f : 0.65f) : 0.25f));
            }
        }
        y -= row_step;
    }
    const glm::vec4 tip_color = root_color * node_color(kOptionTipText);
    const float note_top = y - 0.6f;
    box_text(kOptionTipText, view.note, {kPhotoLabelX, note_top - 5.4f}, {text_right, note_top}, tip_color);
    box_text(kOptionTipText, view.hint, {kPhotoLabelX, std::max(note_top - 11.0f, kPhotoBottom)}, {text_right, note_top - 6.2f},
             tip_color * glm::vec4(1.0f, 1.0f, 1.0f, 0.6f));
}

void GameUi::RecordPhotoMode(gpu::CommandList& cmd, gpu::Extent2D extent, const PhotoPanelView& view) {
    if (!ready_ || !batch_) {
        return;
    }
    ui::UiBatch& batch = *batch_;
    batch.Begin(extent);
    batch.SetCoverageAlpha(false);
    const UiCanvas canvas = UiCanvas::Fit(extent);
    canvas_ = canvas;
    canvas_ready_ = true;
    batch.SetScreenArea(canvas.origin, UiCanvas::kHeight * canvas.scale);
    const glm::vec2 full(static_cast<float>(extent.width), static_cast<float>(extent.height));
    const glm::vec2 lo(view.crop.x*full.x,view.crop.y*full.y);
    const glm::vec2 hi((view.crop.x+view.crop.width)*full.x,(view.crop.y+view.crop.height)*full.y);
    const auto black=ui::UiDrawParams::Plain(TextureManager::kWhite);
    const glm::vec4 color(0,0,0,1);
    auto matte=[&](glm::vec2 a,glm::vec2 b){ if(b.x>a.x && b.y>a.y) batch.Quad(a,b,{0,0},{1,1},color,black,ui::UiShade::Solid,ui::UiBlend::Alpha); };
    matte({0,0},{full.x,lo.y}); matte({0,hi.y},full);
    matte({0,lo.y},{lo.x,hi.y}); matte({hi.x,lo.y},{full.x,hi.y});
    if (const std::vector<float> lines = PhotoGridLines(view.grid); !lines.empty() && hi.x > lo.x && hi.y > lo.y) {
        const float width = std::max(1.0f, std::round(full.y / 720.0f));
        const glm::vec4 light(1.0f, 1.0f, 1.0f, 0.55f);
        const glm::vec4 dark(0.0f, 0.0f, 0.0f, 0.35f);
        auto line = [&](glm::vec2 a, glm::vec2 b, glm::vec4 c) { batch.Quad(a, b, {0, 0}, {1, 1}, c, black, ui::UiShade::Solid, ui::UiBlend::Alpha); };
        for (const float f : lines) {
            const float x = std::round(lo.x + (hi.x - lo.x) * f - width * 0.5f);
            const float y = std::round(lo.y + (hi.y - lo.y) * f - width * 0.5f);
            line({x + width, lo.y}, {x + 2.0f * width, hi.y}, dark);
            line({lo.x, y + width}, {hi.x, y + 2.0f * width}, dark);
            line({x, lo.y}, {x + width, hi.y}, light);
            line({lo.x, y}, {hi.x, y + width}, light);
        }
    }
    if (view.panel) {
        DrawPhotoPanel(batch, canvas, view);
    }
    batch.Record(cmd, extent);
}

}
