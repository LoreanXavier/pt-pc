#include "game/ui/game_ui.h"

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
#include "game/ui/pc_settings.h"
#include "game/ui/port_credits.h"

namespace pt::game {
namespace {

constexpr float kSaveIconDelay = 3.0f / 30.0f;

GameUi* g_active = nullptr;

constexpr const char* kSubtitleModel = "/Assets/sh/ui/ModelAsset/sys_subtitle/Scenes/UI_sys_subtitle.uif";
constexpr uint16_t kSubtitleTextNode = 2;
constexpr const char* kSubtitleFont = "sbt-sys-M";

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
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

// a loop transition drops the previous loop's speech: nothing of its subtitles or caption carries into the next loop
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
    q.street_return |= input.street_return;
    q.held_dirs |= input.held_dirs;
    q.click |= input.click;
    q.right_click |= input.right_click;
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
        // the Archive's pictures (archive.h): the flash's noise, and its string unless the trigger has none
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

void GameUi::Update(Game& game, const InputState& input, float dt) {
    if (!ready_) {
        return;
    }
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
    MenuInput menu_input = MapMenuInput(input, previous_held_);
    previous_held_ = input.held;
    if (input.pointer_valid && canvas_ready_) {
        const glm::vec2 virtual_px = (input.pointer - canvas_.origin) / canvas_.scale;
        menu_input.pointer_valid = true;
        menu_input.pointer = glm::vec2(virtual_px.x - UiCanvas::kWidth * 0.5f, UiCanvas::kHeight * 0.5f - virtual_px.y) / UiCanvas::kUnit;
    }
    QueueMenuInput(menu_input);
    menu_input = std::exchange(queued_menu_input_, MenuInput{});
    // the save request's dialog (0x9476D0 states 0xD to 0x14) takes the pad as the PS4's system dialog does; its button closes it
    save_dialog_text_.clear();
    if (const auto& dialog = game.PendingSaveDialog()) {
        if (dialog->key.starts_with("pc_")) {
            save_dialog_text_ = std::string(PcText(dialog->key, language_));
        } else if (const ui::LangFile* system = assets_.System(language_)) {
            save_dialog_text_ = system->Text(dialog->key);
        }
        if (save_dialog_text_.empty()) {
            save_dialog_text_ = dialog->key;
        }
        if (menu_input.accept || menu_input.click || input.confirm) {
            game.CloseSaveDialog();
            save_dialog_text_.clear();
        }
        menu_input = MenuInput{};
    }
    // the port's credits page (Game::StartPortCredits): a confirm or back press ends it, and no menu opens over it; the first half
    // second leaves out a press that was meant for the ending
    port_credits_time_ = game.PortCreditsActive() && game.Controller().Step() == 33 ? game.PortCreditsTime() : -1.0f;
    if (port_credits_time_ >= 0.0f && !menu_.IsOpen()) {
        if (port_credits_time_ > 0.5f && (menu_input.accept || menu_input.back || menu_input.close || menu_input.click)) {
            game.SkipPortCredits();
        }
        menu_input = MenuInput{};
        pc_request_ = false;
    }
    const bool pc_request = std::exchange(pc_request_, false);
    if (menu_suspended_) {
        // the theater runs: the menu waits under it (main.cpp)
    } else if (!menu_.IsOpen()) {
        if (game.Controller().OptionMenuOpen()) {
            OpenMenu(game, true);
        } else if (port_credits_time_ >= 0.0f) {
            // the credits page runs out (or is skipped) before the street question opens
        } else if (game.StreetOfferPending()) {
            // the end of the credits asks on the PC page (PcSettings street page)
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
            // 0x9208C0: the close press unpauses the world at once (0x52ADF0), the setout plays over the running game
            if (menu_.FirstBoot() || game.Controller().OptionMenuOpen()) {
                game.Controller().CloseOptionMenu();
            }
            if (pause_owner_) {
                game.SetPaused(false);
                pause_owner_ = false;
            }
            if (menu_.TakeStreetReturn()) {
                game.LeaveStreetWalk();
            }
        }
        if (menu_.TakeClosed()) {
            // 0x1285F80 state 3: Resume_All once the setout is done
            if (game.Audio()) {
                game.Audio()->PostEvent("Resume_All", nullptr);
            }
            game.RequestOptionsSave();
        }
    }
    // the save job's SaveUiDisp and LoadUiDisp start the icon (0x1284F40) three 30 Hz frames after the request: the job collects in
    // the frame after it, writes (and posts SaveUiDisp) in the next, and the icon takes the message in the third; menu_detail_rb's
    // icon follows the menu's close by that much more than the port's did. The UI runs while the world is paused.
    if (menu_suspended_) {
        // the theater's own session loads no save of the player's: no save icon for it
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
    if (!paused) {
        subtitles_.Update(dt);
        if (caption_clock) {
            subtitles_.Seek(fx.caption_id, fx.caption_time);
        }
    }
    UpdateSubliminal(game, dt, paused);
    demo_ui_.Update(game, dt, paused);
    UpdateSpeedrun(game);
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
    SubtitleView view;
    if (!subtitles_on_ || !subtitles_.Current(hidden_subtitle_, view)) {
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
    // under the PC letterbox the lines move up into the picture: their bottom (the glyph boxes, in the canvas' 1280x720) at least
    // the subtitle box's own distance from the frame's bottom above the lower bar
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
    // the subtitle nodes (0x854A30, EvSubtitlesNode and EvControlSubtitlesNode) draw their text with Draw2D_Border
    glm::vec4 subtitle_color = color * generator.color;
    ui::UiShade shade = ui::UiShade::Border;
    // Strong subtitles draw above the ending's flat pale fade; white fill disappears into that background.
    const float backdrop_luminance = glm::dot(glm::vec3(fade_), glm::vec3(0.2126f, 0.7152f, 0.0722f));
    if (strong_subtitles_ && fade_.a > 0.95f && backdrop_luminance > 0.65f) {
        subtitle_color = glm::vec4(glm::vec3(0.08f), subtitle_color.a);
        shade = ui::UiShade::Text;
    }
    DrawText(batch, canvas, *font, layout, subtitle_color, ui::UiBlend::Alpha, shade);
}

void GameUi::DrawSubliminal(ui::UiBatch& batch, bool string_pass) {
    const glm::vec2 extent(static_cast<float>(batch.Extent().width), static_cast<float>(batch.Extent().height));
    // the original's full-screen passes span 0 to 1 across its 16:9 frame; a wider (or narrower) window shows more (or less) of the
    // same texels beside it instead of stretching them
    const float across = extent.y > 0.0f ? (extent.x / extent.y) / (UiCanvas::kWidth / UiCanvas::kHeight) : 1.0f;
    const glm::vec2 uv0(0.5f - 0.5f * across, 0.0f);
    const glm::vec2 uv1(0.5f + 0.5f * across, 1.0f);
    if (string_pass) {
        const int image = subliminal_.Image();
        if (subliminal_.StringValue() <= 0.0f || image < 0 || image >= SubliminalEffect::kImageCount) {
            return;
        }
        ui::UiDrawParams params = ui::UiDrawParams::Plain(subliminal_textures_[image]);
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
    const VkFormat scene_format = renderer_ ? renderer_->SceneColorFormat() : Renderer::kSceneColorFormat;
    const bool hdr_scene = scene_format == VK_FORMAT_R16G16B16A16_SFLOAT || scene_format == VK_FORMAT_R32G32B32A32_SFLOAT;
    params.extra2 = hdr_scene ? glm::vec4(renderer_ ? renderer_->exposure : 1.0f, 2.2f, 0.0f, 0.0f) : glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    // the noise's own textures widen as the string's; the scene under it is read at the pixel (ShadeNoise, extra2.z)
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
    batch.Quad(glm::vec2(0.0f), full, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.75f),
               ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
    // the subtitles' font and style: the system dialog of the PS4 has a font of its own that the game data does not hold
    UiFont* font = assets_.Font(UiFontType::Movie, language_);
    if (!font) {
        return;
    }
    ui::TextStyle style;
    style.font = &font->font;
    if (const UiFontStyle* s = assets_.FontStyle(kSubtitleFont, language_)) {
        style.font_width = s->width;
        style.font_height = s->height;
        style.text_space = s->text_space;
        style.line_space = s->line_space;
    }
    // the subtitle font's style with the generator's spacing (DrawSubtitle)
    style.text_space += assets_.Generator().font_space;
    style.line_space += assets_.Generator().line_space;
    ui::NormalizeSubtitleStyle(style);
    const std::string text = save_dialog_text_ + "\n\n" + std::string(PcText("pc_ok", language_));
    ui::TextLayout layout = ui::LayoutText(text, style, UiCanvas::kWidth * 0.6f);
    const glm::vec2 lo(UiCanvas::kWidth * 0.2f, UiCanvas::kHeight * 0.3f);
    const glm::vec2 hi(UiCanvas::kWidth * 0.8f, UiCanvas::kHeight * 0.7f);
    ui::PlaceText(layout, lo, hi, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Center);
    DrawText(batch, canvas, *font, layout, glm::vec4(1.0f), ui::UiBlend::Alpha, ui::UiShade::Text);
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
    // what the player's game did meanwhile is none of its own: it did not run
    save_io_seen_ = game.SaveIoCount();
}

// the theater's line: what plays at the top left, how to leave at the bottom, small and muted as the page's credit line
void GameUi::DrawTheaterHint(ui::UiBatch& batch, const UiCanvas& canvas) {
    UiFont* font = assets_.Font(UiFontType::PcSystem, language_);
    if (!font) return;
    auto line = [&](const std::string& text, glm::vec2 lo, glm::vec2 hi, ui::TextAlign align) {
        if (text.empty()) return;
        ui::TextStyle style;
        style.font = &font->font;
        style.font_width = style.font_height = 18.0f;
        ui::TextLayout layout = ui::LayoutText(text, style, hi.x - lo.x);
        ui::PlaceText(layout, lo, hi, align, align, ui::TextAlign::Center, true);
        DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, 0.55f), ui::UiBlend::Alpha);
    };
    line(theater_title_, {30.0f, 14.0f}, {UiCanvas::kWidth - 30.0f, 40.0f}, ui::TextAlign::Start);
    line(theater_hint_, {30.0f, UiCanvas::kHeight - 40.0f}, {UiCanvas::kWidth - 30.0f, UiCanvas::kHeight - 14.0f}, ui::TextAlign::Center);
}

// The port's credits page (port_credits.h): the ending's off-white fade over the whole screen and the current card in the
// subtitle font, dark as the original's staff names, centred, faded and grown by the card's setin
void GameUi::DrawPortCredits(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    const glm::vec4 paper(0.93f, 0.94f, 0.93f, 1.0f);
    batch.Quad(glm::vec2(0.0f), full, glm::vec2(0.0f), glm::vec2(1.0f), paper, ui::UiDrawParams::Plain(TextureManager::kWhite),
               ui::UiShade::Solid, ui::UiBlend::Alpha);
    const port_credits::View view = port_credits::At(port_credits_time_);
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
    // the notices at about the teaser note's size at the card's start; the wrap width grows with the text, so the lines stay
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

// The speedrun overlay (docs/gameplay.md, speedrun mode): the run's time in the picked clock, the current loop and its time,
// and for 5 s after a split (and at the finish) the run against the record at that split
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
    // tabular digits: each digit takes the width of a 0, so the time does not jitter as it counts (times only: ASCII, one glyph
    // per character)
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
    // the picture's top left corner: inside the 16:9 canvas (on an ultrawide screen it stays with the rest of the UI) and
    // below the PC letterbox's top bar
    const float bar = LetterboxBar(full, letterbox_);
    const float top = std::max(22.0f, (bar - canvas.origin.y) / canvas.scale + 12.0f);
    const float left = 34.0f;
    const glm::vec4 white(1.0f, 1.0f, 1.0f, speedrun_view_.alpha);
    float y = top;
    // one line: an optional name, then a time; returns nothing, moves y down
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
}

void GameUi::ShowUpdateNotice(std::string note) {
    if (!update_notice_.empty() || note.empty()) return;
    update_notice_ = std::move(note);
    update_notice_time_ = -1.0f;
}

// The notice's clock runs only in the game proper (controller step 15: not the boot, the first start's option screen and preface,
// the game over, the ending and its credits), with no demo in charge of the camera (the kill, the openings), outside the Archive's
// theater, under no full fade, and not while held (the photo mode draws in the UI's place, VR has no place for it); the pause
// menu and the PC settings page over the game count as the game. Elsewhere it waits, and it is never lost.
void GameUi::UpdateUpdateNotice(Game& game, float dt) {
    if (update_notice_.empty() || update_notice_time_ >= kUpdateNoticeLength) return;
    const bool allowed = game.Controller().Step() == 15 && !game.Demos().HasActiveCamera() && !menu_suspended_ && !update_notice_held_ &&
                         fade_.a < 0.5f && !(menu_.IsOpen() && menu_.FirstBoot());
    if (!allowed) return;
    if (update_notice_time_ < 0.0f) {
        update_notice_time_ = 0.0f;
        LogInfo("update: notice shown");
    } else {
        update_notice_time_ += dt;
    }
}

// one line in the PC system font, small and muted as the theater's line and the settings page's corner line, centred at the top
// of the picture (the subtitles and the prompts are at the bottom, the speedrun timer at the top left), below the PC letterbox's
// top bar; a quiet fade in and out, nothing else moves
void GameUi::DrawUpdateNotice(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 full) {
    const float t = update_notice_time_;
    if (t < 0.0f || t >= kUpdateNoticeLength || canvas.scale <= 0.0f) return;
    UiFont* font = assets_.Font(UiFontType::PcSystem, language_);
    if (!font) return;
    const std::string text = PcNoteText(update_notice_, language_);
    if (text.empty()) return;
    const float fade = t < kUpdateNoticeFadeIn                       ? t / kUpdateNoticeFadeIn
                       : t > kUpdateNoticeFadeIn + kUpdateNoticeHold ? (kUpdateNoticeLength - t) / kUpdateNoticeFadeOut
                                                                     : 1.0f;
    ui::TextStyle style;
    style.font = &font->font;
    style.font_width = style.font_height = 18.0f;
    const float bar = LetterboxBar(full, letterbox_);
    const float top = std::max(14.0f, (bar - canvas.origin.y) / canvas.scale + 12.0f);
    const glm::vec2 lo(UiCanvas::kWidth * 0.15f, top);
    const glm::vec2 hi(UiCanvas::kWidth * 0.85f, top + 26.0f);
    ui::TextLayout layout = ui::LayoutText(text, style, hi.x - lo.x);
    ui::PlaceText(layout, lo, hi, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Start, true);
    DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, 0.55f * std::clamp(fade, 0.0f, 1.0f)), ui::UiBlend::Alpha);
}

void GameUi::Record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    (void)target;
    if (!ready_ || !batch_) {
        return;
    }
    ui::UiBatch& batch = *batch_;
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
        // the peephole's hole (holl_002_alp, opaque black at its edges) covers the original's 16:9 frame: drawn over the canvas'
        // 16:9 area, its round hole stays round in a wider or narrower window, and the frame around that area is black
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
    layers.push_back({kSubliminalStringPriority, [&] { DrawSubliminal(batch, true); }});
    layers.push_back({kSubliminalNoisePriority, [&] { DrawSubliminal(batch, false); }});
    layers.push_back({strong_subtitles_ ? kStrongSubtitlePriority : kSubtitlePriority, [&] { DrawSubtitle(batch, canvas); }});
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
    if (!theater_title_.empty() || !theater_hint_.empty()) {
        layers.push_back({SaveIcon::kPriority - 1, [&] { DrawTheaterHint(batch, canvas); }});
    }
    if (demo_ui_.Active()) {
        layers.push_back({DemoUi::kPriority, [&] { demo_ui_.Draw(batch, canvas, language_); }});
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
    if (speedrun_view_.shown && !menu_.IsOpen()) {
        layers.push_back({kSpeedrunPriority, [&] { DrawSpeedrun(batch, canvas, full); }});
    }
    if (UpdateNoticeShown() && !vr_hud_) {
        layers.push_back({kUpdateNoticePriority, [&] { DrawUpdateNotice(batch, canvas, full); }});
    }
    std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) { return a.priority < b.priority; });
    for (const Layer& layer : layers) {
        layer.draw();
    }
    batch.Record(cmd, extent);
}

namespace {

// The photo mode's panel, in the PC settings page's units and nodes (pc_settings_page.cpp): its texts take the option screen's
// font styles and colours, its header lines and selection bar are the screen's own meshes, all at kPhotoScale
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
constexpr float kPhotoTitleY = 30.0f;
constexpr float kPhotoTop = 25.5f;
constexpr float kPhotoRowStep = 3.0f * kPhotoScale;
constexpr float kPhotoSectionGap = 3.5f * kPhotoScale;
constexpr float kPhotoPanelRight = -14.0f;
constexpr float kPhotoFade = 8.0f;

glm::vec2 VirtualPx(glm::vec2 units) {
    return {UiCanvas::kWidth * 0.5f + units.x * UiCanvas::kUnit, UiCanvas::kHeight * 0.5f - units.y * UiCanvas::kUnit};
}

// a node's own scale (a root's is 1)
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

    // the panel's ground: black from the screen's left edge, fading out to its right
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
    // a line of a node's font style at the node's scale under the page root (times parent), placed in [x0, x1] at height y as the
    // node's text box places it; shrunk to the width when it is wider
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
    // a wrapped text in a box (units), shrunk until it fits, as the PC page's description
    auto box_text = [&](uint16_t id, std::string_view value, glm::vec2 lo, glm::vec2 hi, glm::vec4 color) {
        const ui::UifNode* node = model->FindById(id);
        if (!node || !font || value.empty()) return;
        const ui::TextStyle text_style =
            NodeTextStyle(*font, assets_.FontStyleByHash(node->font_name, language), root_scale * OwnScale(*node) * kPhotoScale);
        const ui::TextLayout layout = ui::LayoutTextInBox(value, text_style, VirtualPx({lo.x, hi.y}), VirtualPx({hi.x, lo.y}), true);
        DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha);
    };
    // one of the screen's meshes at a point of the page (units), at its scale under the page root (times parent)
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
    float y = kPhotoTop;
    bool first = true;
    for (const PhotoPanelRow& row : view.rows) {
        if (row.section) {
            if (!first) y -= kPhotoSectionGap - kPhotoRowStep;
            text(kOptionHeaderText, glm::vec2(1.0f), row.label, kPhotoLabelX, kPhotoValueX + kPhotoArrow + 2.0f, y, ui::TextAlign::Start,
                 root_color * node_color(kOptionHeaderText));
            mesh(kOptionHeaderLine, glm::vec2(1.0f), glm::vec2(kPhotoLabelX, y) + glm::vec2(16.0f, -1.0f) * kPhotoScale, root_color);
            y -= kPhotoRowStep;
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
        y -= kPhotoRowStep;
    }
    const glm::vec4 tip_color = root_color * node_color(kOptionTipText);
    const float note_top = y - 0.6f;
    box_text(kOptionTipText, view.note, {kPhotoLabelX, note_top - 5.4f}, {text_right, note_top}, tip_color);
    box_text(kOptionTipText, view.hint, {kPhotoLabelX, note_top - 11.0f}, {text_right, note_top - 6.2f},
             tip_color * glm::vec4(1.0f, 1.0f, 1.0f, 0.6f));
}

void GameUi::RecordPhotoMode(VkCommandBuffer cmd, VkExtent2D extent, const PhotoPanelView& view) {
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
    // the letterbox: black bars over and under the frame (they are part of the photo)
    const glm::vec2 lo(view.crop.x*full.x,view.crop.y*full.y);
    const glm::vec2 hi((view.crop.x+view.crop.width)*full.x,(view.crop.y+view.crop.height)*full.y);
    const auto black=ui::UiDrawParams::Plain(TextureManager::kWhite);
    const glm::vec4 color(0,0,0,1);
    auto matte=[&](glm::vec2 a,glm::vec2 b){ if(b.x>a.x && b.y>a.y) batch.Quad(a,b,{0,0},{1,1},color,black,ui::UiShade::Solid,ui::UiBlend::Alpha); };
    matte({0,0},{full.x,lo.y}); matte({0,hi.y},full);
    matte({0,lo.y},{lo.x,hi.y}); matte({hi.x,lo.y},{full.x,hi.y});
    if (view.panel) {
        DrawPhotoPanel(batch, canvas, view);
    }
    batch.Record(cmd, extent);
}

}
