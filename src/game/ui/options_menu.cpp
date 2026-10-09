#include "game/ui/options_menu.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <string>

#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "engine/core/localized_text.h"
#include "engine/fs/vfs.h"
#include "engine/ui/text_layout.h"
#include "engine/ui/uilb.h"
#include "game/game.h"
#include "game/ui/ui_icons.h"

namespace pt::game {
namespace {

constexpr const char* kModelPath = "/Assets/sh/ui/ModelAsset/sys_option/Scenes/UI_sys_option.uif";
constexpr const char* kLayoutPath = "/Assets/sh/ui/LayoutAsset/sys_option/UI_sys_option.uilb";
constexpr float kLanguageDelay = 0.5f;

constexpr uint16_t kTitle = 150;
constexpr uint16_t kBack = 115;
constexpr uint16_t kBrightnessLabel = 41;
constexpr uint16_t kTip = 48;
constexpr uint16_t kImageLeft = 61;
constexpr uint16_t kImageZoom = 69;
constexpr uint16_t kImageRight = 72;
constexpr uint16_t kSubtitleLabel = 119;
constexpr uint16_t kSubtitleTexts[8] = {121, 123, 125, 127, 129, 131, 133, 135};
constexpr uint16_t kCameraLabel = 78;
constexpr uint16_t kVerticalLabel = 93;
constexpr uint16_t kVerticalNormal = 88;
constexpr uint16_t kVerticalReversed = 86;
constexpr uint16_t kHorizontalLabel = 108;
constexpr uint16_t kHorizontalNormal = 103;
constexpr uint16_t kHorizontalReversed = 101;
constexpr uint16_t kRootNull = 1;
constexpr uint16_t kContent = 39;
constexpr uint16_t kHorizontalRow = 94;
constexpr uint16_t kSquareIcon = 104;
constexpr uint16_t kSquareGlow = 106;
// the button pictures of the rows and their glows: brightness (D-pad left and right), subtitles (D-pad up and down), vertical
// (triangle), horizontal (square), Back (OPTIONS)
constexpr uint16_t kBrightnessIcon = 44;
constexpr uint16_t kBrightnessGlow = 46;
constexpr uint16_t kSubtitleIcon = 141;
constexpr uint16_t kSubtitleGlow = 144;
constexpr uint16_t kVerticalIcon = 89;
constexpr uint16_t kVerticalGlow = 91;
constexpr uint16_t kBackIcon = 113;
constexpr uint16_t kBackGlow = 116;
constexpr std::string_view kAcceptToken = "{accept}";
constexpr uint16_t kSubtitleList = 118;
constexpr uint16_t kVerticalRow = 79;
constexpr uint16_t kGaugeGroup = 40;
constexpr glm::vec2 kEntryAt{32.0f, -26.0f};
constexpr float kStreetReturnY = -24.0f;
constexpr float kSwitchOut = 9.0f;
constexpr float kSwitchIn = 13.0f;
constexpr const char* kChangeSound = "Play_sys_change_01";
constexpr const char* kCursorSound = "Play_sys_cursor_01";
constexpr float kLeftImageLevel = 0.09f;
constexpr float kRightImageLevel = 0.01f;
constexpr float kGameFrame = 1.0f / 30.0f;

constexpr int kFirstSubtitleSelector = 4;
constexpr int kLastSubtitleSelector = kFirstSubtitleSelector + UiAssets::kLanguageCount;
constexpr const char* kLanguageKeys[] = {"op_sub_none", "op_sub_english", "op_sub_french", "op_sub_german",
    "op_sub_spanish", "op_sub_japanese", "op_sub_itlian", "op_sub_portuguese", "op_sub_turkish",
    "op_sub_chinese", "op_sub_arabic", "op_sub_russian", "op_sub_ukrainian", "op_sub_czech", "op_sub_polish"};
constexpr std::string_view kNativeLanguageNames[] = {"", "English", "Français", "Deutsch", "Español",
    "日本語", "Italiano", "Português", "Türkçe", "Chinese (Simplified)", "Arabic", "Russian", "Ukrainian", "Czech", "Polish"};
static_assert(std::size(kLanguageKeys) == UiAssets::kLanguageCount + 1);
static_assert(std::size(kNativeLanguageNames) == std::size(kLanguageKeys));

}

MenuInput MapMenuInput(const InputState& input, uint32_t previous_held) {
    MenuInput out;
    out.subtitle_up = (input.raw_pressed & kRawUp) != 0;
    out.subtitle_down = (input.raw_pressed & kRawDown) != 0;
    out.brightness_down = (input.raw_pressed & kRawLeft) != 0;
    out.brightness_up = (input.raw_pressed & kRawRight) != 0;
    out.toggle_vertical = (input.raw_pressed & kRawTriangle) != 0 || input.confirm;
    out.toggle_horizontal = (input.raw_pressed & kRawSquare) != 0 || input.cancel;
    out.zoom_pressed = (input.held & kPadZoom) != 0 && (previous_held & kPadZoom) == 0;
    out.zoom_released = (input.held & kPadZoom) == 0 && (previous_held & kPadZoom) != 0;
    out.close = input.pause;
    out.accept = (input.raw_pressed & kRawCross) != 0;
    out.back = (input.raw_pressed & kRawCircle) != 0 || input.cancel;
    out.open_pc = out.accept && !input.confirm;
    out.switch_column = (input.raw_pressed & (kRawL1 | kRawR1)) != 0;
    out.street_return = (input.raw_pressed & kRawL1) != 0 || input.house_pressed;
    out.held_dirs = input.raw_held & (kRawUp | kRawDown | kRawLeft | kRawRight);
    if (input.left_stick.y > 0.5f) {
        out.held_dirs |= kRawUp;
    } else if (input.left_stick.y < -0.5f) {
        out.held_dirs |= kRawDown;
    }
    if (input.left_stick.x > 0.5f) {
        out.held_dirs |= kRawRight;
    } else if (input.left_stick.x < -0.5f) {
        out.held_dirs |= kRawLeft;
    }
    out.click = input.click;
    out.right_click = input.right_click;
    return out;
}

bool OptionsMenu::Init(UiAssets& assets) {
    assets_ = &assets;
    const ui::UifModel* source = assets.Model(kModelPath);
    if (!source) {
        return false;
    }
    derived_ = std::make_unique<ui::UifModel>(*source);
    const bool icons = assets.BuildPcIcons();
    const int icon_atlas = icons ? derived_->AddTexture(kPcIconAtlas) : -1;
    const int icon_glow = icons ? derived_->AddTexture(kPcIconGlow) : -1;
    BuildEntryRow(*derived_, icon_atlas, icon_glow);
    ui::UifNode quit = *derived_->FindById(kBack);
    quit.parent = derived_->IndexOfId(kContent);
    quit.translate.x = -22.0f;
    quit.translate.y = -27.0f;
    quit_label_ = derived_->AddNode(std::move(quit), StrCode64("option_quit"));
    // the street walk's "Return to the house", over the quit line (a PC extra: hidden outside the walk)
    ui::UifNode street = *derived_->FindById(kBack);
    street.parent = derived_->IndexOfId(kContent);
    street.translate.x = -22.0f;
    street.translate.y = kStreetReturnY;
    street_label_ = derived_->AddNode(std::move(street), StrCode64("option_street_return"));
    // their button pictures: copies of Back's (the OPTIONS picture's node, 2 units left of its text), which ApplyPrompts fills
    // with the device's picture of R1 / L1 or the bound key as it does for every row
    auto prompt_node = [&](uint16_t id, float y, std::string_view name) {
        ui::UifNode node = *derived_->FindById(id);
        const ui::UifNode& back_text = *derived_->FindById(kBack);
        node.parent = derived_->IndexOfId(kContent);
        node.translate.x = -22.0f + (node.translate.x - back_text.translate.x);
        node.translate.y = y + (node.translate.y - back_text.translate.y);
        return derived_->AddNode(std::move(node), StrCode64(name));
    };
    quit_icon_ = prompt_node(kBackIcon, -27.0f, "option_quit_icon");
    quit_glow_ = prompt_node(kBackGlow, -27.0f, "option_quit_glow");
    street_icon_ = prompt_node(kBackIcon, kStreetReturnY, "option_street_icon");
    street_glow_ = prompt_node(kBackGlow, kStreetReturnY, "option_street_glow");
    const ui::UifNode row_template = *derived_->FindById(120);
    const ui::UifNode label_template = *derived_->FindById(121);
    for (int i = 0; i <= UiAssets::kLanguageCount; ++i) {
        ui::UifNode row = row_template;
        row.translate.x += i < 7 ? -11.0f : 11.0f;
        row.translate.y -= static_cast<float>(i < 7 ? i : i - 7) * 2.5f;
        const auto group = derived_->AddNode(std::move(row), StrCode64(std::format("option_language_row_{}", i)));
        ui::UifNode label = label_template;
        label.parent = derived_->IndexOfId(group);
        label.size.x = 210.0f;
        language_groups_.push_back(group);
        language_texts_.push_back(derived_->AddNode(std::move(label), StrCode64(std::format("option_language_text_{}", i))));
    }
    ui::UifNode bar = *derived_->FindById(80);
    bar.parent = row_template.parent;
    bar.translate = row_template.translate;
    language_bar_ = derived_->AddNode(std::move(bar), StrCode64("option_language_bar"));
    ui::UifNode bar_image = *derived_->FindById(81);
    bar_image.parent = derived_->IndexOfId(language_bar_);
    bar_image.translate.x = 0.0f;
    bar_image.translate.y = 0.0f;
    derived_->AddNode(std::move(bar_image), StrCode64("option_language_bar_image"));
    pc_.Build(*derived_, icon_atlas, icon_glow);
    prompt_nodes_ = {
        {kBrightnessIcon, kBrightnessGlow, {PromptButton::DpadLeftRight, KeyAction::PadLeft, KeyAction::PadRight}},
        {kSubtitleIcon, kSubtitleGlow, {PromptButton::DpadUpDown, KeyAction::PadUp, KeyAction::PadDown}},
        {kVerticalIcon, kVerticalGlow, {PromptButton::Triangle, KeyAction::Confirm, KeyAction::Confirm}},
        {kSquareIcon, kSquareGlow, {PromptButton::Square, KeyAction::Cancel, KeyAction::Cancel}},
        {kBackIcon, kBackGlow, {PromptButton::Options, KeyAction::Menu, KeyAction::Menu}},
        {entry_icon_, entry_glow_, {PromptButton::Cross, KeyAction::PcSettings, KeyAction::PcSettings}},
        {pc_.BackIcon(), pc_.BackGlow(), {PromptButton::Circle, KeyAction::Menu, KeyAction::Menu}},
        {quit_icon_, quit_glow_, {PromptButton::R1, KeyAction::MenuQuit, KeyAction::MenuQuit}},
        {street_icon_, street_glow_, {PromptButton::L1, KeyAction::MenuHouse, KeyAction::MenuHouse}},
    };
    root_null_hash_ = static_cast<uint32_t>(derived_->Names()[kRootNull]);
    model_ = derived_.get();
    fragment_brain_textures_ = derived_->FindById(kImageLeft)->material.textures;
    clean_brain_textures_ = derived_->FindById(kImageZoom)->material.textures;
    ui::UilbLayout layout;
    std::string error;
    if (auto data = assets.Files().ReadFile(kLayoutPath); !data || !layout.Parse(*data, &error)) {
        LogWarn("ui: option layout {}: {}", kLayoutPath, data ? error : std::string("not found"));
    }
    auto load = [&](const std::string& path) -> const ui::UiaAnimation* {
        if (path.empty()) {
            return nullptr;
        }
        auto data = assets.Files().ReadFile(path);
        auto animation = std::make_unique<ui::UiaAnimation>();
        std::string reason;
        if (!data || !animation->Parse(*data, &reason)) {
            LogWarn("ui: option animation {}: {}", path, data ? reason : std::string("not found"));
            return nullptr;
        }
        storage_.push_back(std::move(animation));
        return storage_.back().get();
    };
    for (const ui::UilbAnimation& a : layout.Animations()) {
        animations_[a.name] = {load(a.main), load(a.shader), a.speed};
    }
    if (const Animation* flash = Find("UI_sys_opt_subttl_sel_1")) {
        pc_.SetFlash(flash->main);
    }
    view_.Bind(model_, &assets);
    for (const std::string& texture : model_->Textures()) {
        assets.Texture(texture);
    }
    ApplyStatic();
    return true;
}

void OptionsMenu::BuildEntryRow(ui::UifModel& model, int icon_atlas, int icon_glow) {
    auto clone = [&](uint16_t id, uint16_t parent, std::string_view name) {
        ui::UifNode node = *model.FindById(id);
        node.parent = model.IndexOfId(parent);
        return model.AddNode(std::move(node), StrCode64(std::format("pc_settings_entry_{}", name)));
    };
    entry_group_ = clone(kHorizontalRow, kContent, "row");
    model.FindById(entry_group_)->translate = glm::vec4(kEntryAt, 0.0f, 0.0f);
    const uint16_t icon = clone(kSquareIcon, entry_group_, "icon");
    const uint16_t glow = clone(kSquareGlow, entry_group_, "glow");
    entry_icon_ = icon;
    entry_glow_ = glow;
    entry_label_ = clone(kHorizontalLabel, entry_group_, "label");
    if (icon_atlas >= 0 && icon_glow >= 0) {
        for (const auto& [id, texture] : {std::pair{icon, icon_atlas}, std::pair{glow, icon_glow}}) {
            ui::UifNode& node = *model.FindById(id);
            node.material.textures[ui::kUifBase] = texture;
            node.uvs = {{0.0f, 0.0f}, {0.0f, 1.0f}, {0.5f, 1.0f}, {0.5f, 0.0f}};
        }
    }
}

float OptionsMenu::TrackAlpha(std::string_view animation, float frame) const {
    const Animation* a = Find(animation);
    if (!a || !a->main) {
        return 1.0f;
    }
    for (const ui::UiaNode& node : a->main->Nodes()) {
        if (node.node != root_null_hash_) {
            continue;
        }
        for (const ui::UiaTrack& track : node.tracks) {
            if (track.unit == ui::UiaAnimation::kColor) {
                return ui::UiaAnimation::Sample(track, frame).a;
            }
        }
    }
    return 1.0f;
}

void OptionsMenu::SwitchPage(Game& game, Page page) {
    if (switch_frame_ >= 0.0f || page == page_ || (page == Page::Pc && !pc_source_)) {
        return;
    }
    next_page_ = page;
    const bool cancel_zoom = zoom_started_;
    CancelPhotoZoom();
    if (cancel_zoom) Play("UI_sys_opt_zoom_out");
    if (page_ == Page::Pc && page != Page::Pc && pc_source_) pc_source_->Closed();
    switch_frame_ = 0.0f;
    if (page == Page::Pc) {
        pc_.Open(pc_source_);
    }
    if (game.Audio()) {
        game.Audio()->PostEvent(page == Page::Pc ? kChangeSound : kCursorSound, nullptr);
    }
    LogInfo("ui: option menu page {}", page == Page::Pc ? "PC settings" : "original");
}

void OptionsMenu::ApplyPages() {
    float original = page_ == Page::Original ? 1.0f : 0.0f;
    float pc = page_ == Page::Pc ? 1.0f : 0.0f;
    if (switch_frame_ >= 0.0f) {
        const float out = TrackAlpha("UI_sys_opt_setout", switch_frame_);
        const float in = TrackAlpha("UI_sys_opt_setin", switch_frame_ - kSwitchOut);
        const bool fading_out = switch_frame_ < kSwitchOut;
        const float alpha = fading_out ? out : in;
        (page_ == Page::Original ? original : pc) = alpha;
    }
    view_.State(kContent).visible = original > 0.0f;
    view_.State(kContent).color_scale.a = original;
    view_.State(pc_.Root()).visible = pc > 0.0f;
    view_.State(pc_.Root()).color_scale.a = pc;
    view_.State(entry_group_).visible = pc_source_ != nullptr;
    view_.State(street_label_).visible = street_shown_;
    view_.State(street_icon_).visible = street_shown_;
    view_.State(street_glow_).visible = street_shown_;
    view_.State(entry_label_).text = std::string(PcText("pc_entry", text_language_));
    if (page_ == Page::Pc || next_page_ == Page::Pc) {
        pc_.Apply(view_, text_language_);
    }
    ApplyPrompts();
}

// The rows' button pictures follow the device used last (the input device's pick): a PlayStation pad keeps the data's pictures, the
// others get generated ones of the same size (letters on the disc, keycaps with the bound key's name, the arrow keys), right-aligned where
// the data's picture ends so a wider keycap grows away from its label. The PC page's help line shows the accept button inline.
void OptionsMenu::ApplyPrompts() {
    for (const PromptNodes& nodes : prompt_nodes_) {
        const PromptGlyph glyph = assets_->PromptPicture(nodes.prompt, prompt_style_);
        for (const uint16_t id : {nodes.icon, nodes.glow}) {
            UifNodeState& state = view_.State(id);
            const ui::UifNode* node = view_.Node(id);
            if (glyph.original || !node || node->positions.empty()) {
                state.texture.reset();
                state.uvs.reset();
                state.positions.reset();
                continue;
            }
            float x0 = node->positions.front().x;
            float x1 = x0;
            float y1 = node->positions.front().y;
            for (const glm::vec2& v : node->positions) {
                x0 = std::min(x0, v.x);
                x1 = std::max(x1, v.x);
                y1 = std::max(y1, v.y);
            }
            const float left = x1 - (x1 - x0) * glyph.width;
            std::vector<glm::vec2> positions;
            std::vector<glm::vec2> uvs;
            for (const glm::vec2& v : node->positions) {
                const bool is_left = v.x < (x0 + x1) * 0.5f;
                const bool is_top = v.y >= y1 - 1e-6f;
                positions.emplace_back(is_left ? left : v.x, v.y);
                uvs.emplace_back(is_left ? glyph.uv0.x : glyph.uv1.x, is_top ? glyph.uv0.y : glyph.uv1.y);
            }
            state.texture = derived_->AddTexture(id == nodes.icon ? glyph.icon : glyph.glow);
            state.uvs = std::move(uvs);
            state.positions = std::move(positions);
        }
    }
    UifNodeState& help = view_.State(pc_.Help());
    help.inline_picture.reset();
    if (help.text) {
        if (const size_t at = help.text->find(kAcceptToken); at != std::string::npos) {
            const PromptGlyph glyph = assets_->PromptPicture({PromptButton::Cross, KeyAction::Confirm, KeyAction::Confirm}, prompt_style_);
            help.text->replace(at, kAcceptToken.size(), "\xEE\x80\x80");
            help.inline_picture = UifInlinePicture{derived_->AddTexture(glyph.icon), derived_->AddTexture(glyph.glow), glyph.uv0, glyph.uv1,
                                                   glyph.width, glyph.body_width, glyph.body_height};
        }
    }
    if (!prompts_shown_ || shown_prompts_ != prompt_style_.device) {
        prompts_shown_ = true;
        shown_prompts_ = prompt_style_.device;
        ApplyTexts();
        if (state_ != State::Closed) {
            LogInfo("ui: option menu prompts show {} buttons", PromptDeviceName(shown_prompts_));
        }
    }
}

const OptionsMenu::Animation* OptionsMenu::Find(std::string_view name) const {
    auto it = animations_.find(StrCode64(name));
    return it != animations_.end() ? &it->second : nullptr;
}

float OptionsMenu::SelectionFlash(float frame) const {
    constexpr uint16_t kSubtitleBarGroup = 136;
    const Animation* flash = Find("UI_sys_opt_subttl_sel_1");
    if (!flash || !flash->main || !model_ || kSubtitleBarGroup >= model_->Names().size()) {
        return 1.0f;
    }
    const uint32_t hash = static_cast<uint32_t>(model_->Names()[kSubtitleBarGroup]);
    float alpha = 1.0f;
    for (const ui::UiaNode& node : flash->main->Nodes()) {
        if (node.node != hash) {
            continue;
        }
        for (const ui::UiaTrack& track : node.tracks) {
            if (track.unit == ui::UiaAnimation::kColor) {
                alpha = ui::UiaAnimation::Sample(track, frame).a;
            }
        }
    }
    return alpha;
}

void OptionsMenu::Play(std::string_view name, bool loop) {
    const Animation* a = Find(name);
    if (!a) {
        LogWarn("ui: option animation {} missing", name);
        return;
    }
    players_.Play(a->main, a->speed, loop);
    players_.Play(a->shader, a->speed, loop);
}

bool OptionsMenu::Playing(std::string_view name) const {
    const Animation* a = Find(name);
    return a && (players_.Playing(a->main) || players_.Playing(a->shader));
}

void OptionsMenu::PlaySelections() {
    Play(std::format("UI_sys_opt_subttl_sel_{}", std::clamp(selector_ - SubtitleWindowStart(), 4, 11) - 3));
    Play(vertical_ ? "UI_sys_opt_camera_a_sel_2" : "UI_sys_opt_camera_a_sel_1");
    Play(horizontal_ ? "UI_sys_opt_camera_b_sel_2" : "UI_sys_opt_camera_b_sel_1");
}

int OptionsMenu::SubtitleWindowStart() const {
    return 0;
}

void OptionsMenu::ApplySubtitleSelection() {
    for (uint16_t id = 120; id <= 136; id += 2) view_.State(id).visible = false;
    const int selected = selector_ - kFirstSubtitleSelector;
    for (size_t i = 0; i < language_groups_.size(); ++i) {
        view_.State(language_groups_[i]).color = glm::vec4(1.0f, 1.0f, 1.0f, i == selected ? 1.0f : .55f);
        view_.State(language_texts_[i]).font_type = UiFontType::PcSystem;
    }
    const auto* target = derived_->FindById(language_groups_[selected]);
    const auto* bar = derived_->FindById(language_bar_);
    view_.State(language_bar_).offset = glm::vec2(target->translate - bar->translate);
    view_.State(language_bar_).color = glm::vec4(1.0f);
}

void OptionsMenu::HoldBrightness() {
    if (const Animation* a = Find("UI_sys_option_bgrh_scle"); a && a->main) {
        players_.Hold(a->main, static_cast<float>(brightness_) * 0.1f * static_cast<float>(a->main->Frames()));
    }
}

void OptionsMenu::ApplyStatic() {
    view_.ResetStates();
    view_.State(kBack).font_type = UiFontType::System;
    view_.State(kImageLeft).color_scale = glm::vec4(kLeftImageLevel, kLeftImageLevel, kLeftImageLevel, 1.0f);
    view_.State(kImageZoom).color_scale = glm::vec4(kLeftImageLevel, kLeftImageLevel, kLeftImageLevel, 1.0f);
    view_.State(kImageRight).color_scale = glm::vec4(kRightImageLevel, kRightImageLevel, kRightImageLevel, 1.0f);
}

void OptionsMenu::ApplyTexts() {
    const ui::LangFile* lang = assets_->Options(text_language_);
    auto set = [&](uint16_t id, const char* key) {
        const auto text = localized::Menu(key, text_language_);
        view_.State(id).text = text.empty() ? (lang ? lang->Text(key) : std::string()) : std::string(text);
    };
    set(kTitle, "op_options");
    set(kBack, "op_back");
    // the quit and street lines are labelled as Back is: the button picture (ApplyPrompts) and the text
    view_.State(quit_label_).font_type = UiFontType::PcSystem;
    view_.State(quit_label_).text = std::string(PcText(quit_confirming_ ? "pc_quit_confirm" : "pc_quit", text_language_));
    view_.State(quit_label_).color = glm::vec4(1.0f, 1.0f, 1.0f, quit_focused_ || quit_confirming_ ? 1.0f : .65f);
    view_.State(street_label_).font_type = UiFontType::PcSystem;
    view_.State(street_label_).text = std::string(PcText(street_confirming_ ? "pc_street_return_confirm" : "pc_street_return", text_language_));
    view_.State(street_label_).color = glm::vec4(1.0f, 1.0f, 1.0f, street_confirming_ ? 1.0f : .65f);
    set(kBrightnessLabel, "op_brightness_setting");
    set(kTip, "op_britness_tip");
    view_.State(kTip).mirror_rtl = true;
    set(kSubtitleLabel, "op_subtitles_setting");
    for (int language_index = 0; language_index <= UiAssets::kLanguageCount; ++language_index) {
        const uint16_t id = language_texts_[language_index];
        set(id, kLanguageKeys[language_index]);
        if (language_index >= 8 && (view_.State(id).text->empty() || *view_.State(id).text == kLanguageKeys[language_index])) {
            view_.State(id).text = std::string(kNativeLanguageNames[language_index]);
        }
    }
    set(kCameraLabel, "op_camera_setting");
    set(kVerticalLabel, "op_camera_y_axis");
    set(kVerticalNormal, "op_camera_normal");
    set(kVerticalReversed, "op_camera_reversed");
    set(kHorizontalLabel, "op_camera_x_axis");
    set(kHorizontalNormal, "op_camera_normal");
    set(kHorizontalReversed, "op_camera_reversed");
}

void OptionsMenu::Show(Game& game, bool first_boot, Page page) {
    if (!model_) {
        return;
    }
    const GameOptions& options = game.Options();
    selector_ = options.subtitles ? std::clamp(options.subtitle_language, 0, UiAssets::kLanguageCount - 1) + 5 : 4;
    vertical_ = options.invert_y;
    horizontal_ = options.invert_x;
    brightness_ = std::clamp(options.brightness, 0, 10);
    text_language_ = std::clamp(options.subtitle_language, 0, UiAssets::kLanguageCount - 1);
    language_timer_ = false;
    quit_confirming_ = false;
    quit_focused_ = false;
    street_shown_ = game.StreetWalkActive();
    street_confirming_ = false;
    street_return_ = false;
    zoom_released_ = false;
    zoom_started_ = false;
    close_pending_ = false;
    resume_ = false;
    resume_sent_ = false;
    first_boot_ = first_boot;
    clock_ = 0.0f;
    switch_frame_ = -1.0f;
    page_ = next_page_ = page == Page::Pc && pc_source_ ? Page::Pc : Page::Original;
    if (page_ == Page::Pc) {
        pc_.Open(pc_source_);
    }
    players_.Clear();
    ApplyStatic();
    ApplyTexts();
    Play("UI_sys_opt_setin");
    Play("UI_sys_opt_lp_1", true);
    Play("UI_sys_opt_lp_2", true);
    PlaySelections();
    HoldBrightness();
    state_ = State::SetIn;
    ApplyPages();
    view_.ClearAnimation();
    players_.Apply(view_);
    ApplySubtitleSelection();
    LogInfo("ui: option menu opened ({}{}), selector {}, brightness {}", first_boot ? "first boot" : "pause",
            page_ == Page::Pc ? ", PC settings" : "", selector_, brightness_);
}

void OptionsMenu::CancelPhotoZoom() {
    if (const Animation* animation = Find("UI_sys_opt_zoom_in")) {
        players_.Stop(animation->main);
        players_.Stop(animation->shader);
    }
    zoom_started_ = false;
    zoom_released_ = false;
}

void OptionsMenu::Close() {
    if (state_ == State::SetIn || state_ == State::Active) {
        // the original's close (state 3) leaves zoom_in playing under the setout; a pending release is dropped, as the next open clears it
        zoom_released_ = false;
        if (page_ == Page::Pc && pc_source_) pc_source_->Closed();
        resume_ = resume_ || !resume_sent_;
        resume_sent_ = true;
        Play("UI_sys_opt_setout");
        state_ = State::SetOut;
    }
}

void OptionsMenu::Commit(Game& game) {
    GameOptions& options = game.Options();
    options.subtitles = selector_ != 4;
    if (selector_ != 4) {
        options.subtitle_language = selector_ - 5;
    }
    options.invert_y = vertical_;
    options.invert_x = horizontal_;
    options.brightness = brightness_;
}

void OptionsMenu::Update(Game& game, const MenuInput& raw_input, float dt) {
    if (state_ == State::Closed) {
        return;
    }
    MenuInput input = raw_input;
    clock_ += dt;
    while (clock_ + 1e-4f >= kGameFrame) {
        clock_ -= kGameFrame;
        pc_.Step(page_ == Page::Pc && switch_frame_ < 0.0f);
        if (switch_frame_ >= 0.0f) {
            switch_frame_ += 2.0f;
        }
    }
    if (switch_frame_ >= kSwitchOut && page_ != next_page_) {
        page_ = next_page_;
        if (page_ == Page::Original) {
            Play("UI_sys_opt_setin_s");
        }
    }
    if (switch_frame_ >= kSwitchOut + kSwitchIn) {
        switch_frame_ = -1.0f;
    }
    const bool switching = switch_frame_ >= 0.0f;
    if (state_ == State::SetIn && input.close && page_ == Page::Original && !switching) {
        close_pending_ = true;
        // 0x9208C0: the close press resumes the game at once, also while the setin plays
        resume_ = resume_ || !resume_sent_;
        resume_sent_ = true;
    }
    if (state_ == State::SetIn && !Playing("UI_sys_opt_setin")) {
        state_ = State::Active;
    }
    if (state_ == State::Active && !switching) {
        if (page_ == Page::Pc) {
            const auto result = pc_.Update(game, input, dt);
            if(game.ProgressResetPending() || game.LoopReloadPending()) { Commit(game); Close(); }
            if (result == PcSettingsPage::Result::Quit) {
                Commit(game);
                // the options changed in this menu are kept, as its close keeps them (the progress as last saved or loaded)
                game.RequestOptionsSave();
                LogInfo("ui: quit selected from PC settings");
                game.RequestQuit();
            } else if (result == PcSettingsPage::Result::Back) {
                SwitchPage(game, Page::Original);
            }
        } else {
            UpdateOriginal(game, input, dt);
        }
    }
    if (state_ == State::SetOut && !Playing("UI_sys_opt_setout")) {
        Commit(game);
        state_ = State::Closed;
        just_closed_ = true;
        LogInfo("ui: option menu closed: subtitles {} language {} invert x {} y {} brightness {}", game.Options().subtitles,
                game.Options().subtitle_language, game.Options().invert_x, game.Options().invert_y, game.Options().brightness);
    }
    ApplyPages();
    view_.ClearAnimation();
    players_.Apply(view_);
    // The collected fragment is baked into the alternate brain texture, not a separate mesh.
    // Replace both colour and glow textures; hiding only its noise animation leaves the fragment visible.
    const bool collected = (game.Nazo().PhotoWord() & 0x100) != 0;
    if (auto* left = derived_->FindById(kImageLeft)) {
        left->material.textures = collected ? clean_brain_textures_ : fragment_brain_textures_;
    }
    ApplySubtitleSelection();
}

// The street walk's "Return to the house" (gameplay.md, street walk): the first press asks, the second closes the menu, and
// GameUi then ends the walk with the ending's restart (Game::LeaveStreetWalk, step 29) as the original's restart after the credits
void OptionsMenu::StreetReturn(Game& game) {
    if (!street_confirming_) {
        street_confirming_ = true;
        quit_confirming_ = quit_focused_ = false;
        if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
        ApplyTexts();
        return;
    }
    street_confirming_ = false;
    street_return_ = true;
    if (game.Audio()) game.Audio()->PostEvent(kChangeSound, nullptr);
    LogInfo("ui: return to the house selected from Options");
    Close();
}

void OptionsMenu::UpdateOriginal(Game& game, const MenuInput& raw_input, float dt) {
    MenuInput input = raw_input;
    street_shown_ = game.StreetWalkActive();
    if (street_shown_ && input.street_return) {
        StreetReturn(game);
        return;
    }
    if (street_confirming_ && (input.accept || input.open_pc)) {
        StreetReturn(game);
        return;
    }
    if (street_confirming_ && (input.switch_column || input.back || input.close)) {
        street_confirming_ = false;
        ApplyTexts();
        if (input.back || input.close) return;
    }
    if (input.switch_column) {
        quit_focused_ = true;
        if (quit_confirming_) { Commit(game); game.RequestOptionsSave(); game.RequestQuit(); LogInfo("ui: quit selected from Options"); }
        else { quit_confirming_ = true; ApplyTexts(); }
        return;
    }
    if (quit_focused_ && (input.accept || input.open_pc)) {
        if (quit_confirming_) { Commit(game); game.RequestOptionsSave(); game.RequestQuit(); LogInfo("ui: quit selected from Options"); }
        else { quit_confirming_ = true; ApplyTexts(); }
        return;
    }
    if (input.pointer_valid && input.click) {
        OriginalClick(game, input.pointer, input);
    }
    if (input.open_pc && pc_source_) {
        Commit(game);
        SwitchPage(game, Page::Pc);
        return;
    }
    auto cursor = [&game]() {
        if (game.Audio()) {
            game.Audio()->PostEvent(kCursorSound, nullptr);
        }
    };
    if (input.subtitle_up || input.subtitle_down) {
        cursor();
        selector_ = input.subtitle_up ? (selector_ == kFirstSubtitleSelector ? kLastSubtitleSelector : selector_ - 1)
                                      : (selector_ == kLastSubtitleSelector ? kFirstSubtitleSelector : selector_ + 1);
        ApplyTexts();
        Play(std::format("UI_sys_opt_subttl_sel_{}", std::clamp(selector_ - SubtitleWindowStart(), 4, 11) - 3));
        language_timer_ = true;
        language_time_ = 0.0f;
    } else if (language_timer_) {
        language_time_ += dt;
        if (language_time_ > kLanguageDelay) {
            language_timer_ = false;
            game.Options().subtitles = selector_ != 4;
            if (selector_ != 4) {
                text_language_ = selector_ - 5;
                game.Options().subtitle_language = text_language_;
                ApplyTexts();
            }
        }
    }
    if (input.toggle_vertical) {
        cursor();
        vertical_ = !vertical_;
        game.Options().invert_y = vertical_;
        Play(vertical_ ? "UI_sys_opt_camera_a_sel_2" : "UI_sys_opt_camera_a_sel_1");
    } else if (input.toggle_horizontal) {
        cursor();
        horizontal_ = !horizontal_;
        game.Options().invert_x = horizontal_;
        Play(horizontal_ ? "UI_sys_opt_camera_b_sel_2" : "UI_sys_opt_camera_b_sel_1");
    }
    if (input.brightness_down && brightness_ > 0) {
        cursor();
        game.Options().brightness = --brightness_;
        HoldBrightness();
    } else if (input.brightness_up && brightness_ < 10) {
        cursor();
        game.Options().brightness = ++brightness_;
        HoldBrightness();
    }
    if (game.Nazo().IsPhotoOptionPending()) {
        // 0x1285F80: whether zoom_in plays is read before this frame's press; a press (re)starts zoom_in, otherwise a release sets the
        // flag +0x1BC, and the flag sends PhotoOption with zoom_out once zoom_in has ended, so a tap shorter than the zoom still counts.
        // The close button is read after this block and does not cancel it; the flag is cleared only when the menu opens (0x1285630).
        const bool zooming = Playing("UI_sys_opt_zoom_in");
        if (input.zoom_pressed) {
            zoom_started_ = true;
            Play("UI_sys_opt_zoom_in");
        } else if (input.zoom_released) {
            zoom_released_ = true;
        }
        if (zoom_released_ && !zooming) {
            Play("UI_sys_opt_zoom_out");
            game.Nazo().SetCondition("PhotoOption");
            LogInfo("ui: option menu sent PhotoOption");
            zoom_released_ = false;
            zoom_started_ = false;
        }
    }
    if (input.close || close_pending_) {
        close_pending_ = false;
        Close();
    }
}

void OptionsMenu::OriginalClick(Game& game, glm::vec2 units, MenuInput& input) {
    if (street_shown_) {
        const glm::vec2 street = view_.WorldPosition(street_label_);
        if (units.x >= street.x - 6.0f && units.x <= street.x + 22.0f && std::abs(units.y - street.y) <= 1.2f) {
            StreetReturn(game);
            return;
        }
        if (street_confirming_) {
            street_confirming_ = false;
            ApplyTexts();
        }
    }
    const glm::vec2 quit = view_.WorldPosition(quit_label_);
    if (units.x >= quit.x - 6.0f && units.x <= quit.x + 15.0f && std::abs(units.y - quit.y) <= 1.2f) {
        if (quit_confirming_) { Commit(game); game.RequestOptionsSave(); game.RequestQuit(); LogInfo("ui: quit selected from Options"); }
        else { quit_confirming_ = true; ApplyTexts(); }
        return;
    }
    if (quit_confirming_) { quit_confirming_ = false; ApplyTexts(); }
    const glm::vec2 entry = view_.WorldPosition(entry_group_);
    if (pc_source_ && units.x >= entry.x - 22.0f && units.x <= entry.x + 12.0f && std::abs(units.y - entry.y) <= 1.5f) {
        input.open_pc = true;
        return;
    }
    const glm::vec2 back = view_.WorldPosition(kBack);
    if (units.x >= back.x - 3.0f && units.x <= back.x + 10.0f && std::abs(units.y - back.y) <= 2.0f) {
        input.close = true;
        return;
    }
    const glm::vec2 rows[2] = {view_.WorldPosition(kVerticalRow), view_.WorldPosition(kHorizontalRow)};
    for (int r = 0; r < 2; ++r) {
        if (units.x < rows[r].x - 22.0f || units.x > rows[r].x + 18.0f) {
            continue;
        }
        const bool current = r == 0 ? vertical_ : horizontal_;
        bool* toggle = r == 0 ? &input.toggle_vertical : &input.toggle_horizontal;
        if (std::abs(units.y - rows[r].y) <= 1.5f) {
            *toggle = true;
            return;
        }
        if (std::abs(units.y - (rows[r].y - 3.0f)) <= 1.5f) {
            const bool reversed = units.x > rows[r].x;
            *toggle = reversed != current;
            return;
        }
    }
    for (size_t i = 0; i < language_groups_.size(); ++i) {
        const glm::vec2 at = view_.WorldPosition(language_groups_[i]);
        if (std::abs(units.x - at.x) > 10.5f || std::abs(units.y - at.y) > 1.2f) continue;
        const int target = static_cast<int>(i) + kFirstSubtitleSelector;
        if (target != selector_) {
            selector_ = target == kFirstSubtitleSelector ? kLastSubtitleSelector : target - 1;
            input.subtitle_down = true;
        }
        return;
    }
    const glm::vec2 gauge = view_.WorldPosition(kGaugeGroup) + glm::vec2(5.0f, -14.0f);
    if (std::abs(units.x - gauge.x) <= 20.0f && std::abs(units.y - gauge.y) <= 1.5f) {
        const int target = std::clamp(static_cast<int>(std::lround((units.x - (gauge.x - 19.2f)) / 38.4f * 10.0f)), 0, 10);
        if (target != brightness_) {
            const bool up = target > brightness_;
            brightness_ = up ? target - 1 : target + 1;
            (up ? input.brightness_up : input.brightness_down) = true;
        }
    }
}

void OptionsMenu::AdvancePresentation(float dt) {
    if (state_ == State::Closed) return;
    players_.Update(dt);
    view_.ClearAnimation();
    players_.Apply(view_);
    ApplyPages();
    ApplySubtitleSelection();
}

void OptionsMenu::Draw(ui::UiBatch& batch, const UiCanvas& canvas) {
    if (state_ == State::Closed || !model_) {
        return;
    }
    const bool settled = page_ == Page::Pc && state_ == State::Active && switch_frame_ < 0.0f;
    if (settled && pc_.FullScreen()) {
        // an Archive picture over the whole screen, in place of the page
        archive_view_.DrawFullScreen(batch, canvas, *assets_, pc_.Panel(), PcText(pc_.FullScreenHint(), text_language_), text_language_);
        return;
    }
    view_.Draw(batch, canvas, text_language_, 1.0f);
    if (page_ == Page::Pc && pc_.Gallery() != PcGallery::None && settled) {
        // the Museum's halls or a hall's wall (museum_layout.h); the strip takes the screen's width, the previews it shows are
        // decoded ahead
        pc_.SetWallWidth(static_cast<float>(batch.Extent().width) / std::max(canvas.scale, 0.001f));
        assets_->PreloadPreviews(pc_.PreviewFiles());
        archive_view_.DrawMuseum(batch, canvas, *assets_, pc_, text_language_);
    }
    if (page_ == Page::Pc && pc_.Browser()) {
        // all the loop previews are decoded on a worker as soon as the page shows (UiAssets::PreloadPreviews), so moving
        // through the entries shows each at once
        if (pc_.IsLoopBrowser()) assets_->PreloadPreviews(pc_.PreviewFiles());
        // the loop browser's preview, the Archive's picture or transcript, right of the rows
        const PcPanel panel = pc_.Panel();
        const bool text = !panel.lines.empty();
        archive_view_.DrawPanel(batch, canvas, *assets_, panel, text ? glm::vec2(650.0f, 160.0f) : glm::vec2(650.0f, 170.0f),
                                text ? glm::vec2(1190.0f, 560.0f) : glm::vec2(1190.0f, 474.0f), text_language_);
    }
    // the page's credit line: the back text's font style, smaller and muted, right aligned in the bottom right corner, while
    // the page is shown (not during its switch)
    std::string_view credit = page_ == Page::Pc && state_ == State::Active && switch_frame_ < 0.0f ? pc_.Credit() : std::string_view();
    // a text key with arguments (the version line) is shown in the menu's language
    std::string credit_text;
    if (credit.starts_with("pc_")) {
        credit_text = PcNoteText(credit, text_language_);
        credit = credit_text;
    }
    const ui::UifNode* back = model_->FindById(kBack);
    UiFont* font = credit.empty() || !back ? nullptr : assets_->Font(UiFontType::PcSystem, text_language_);
    if (font) {
        constexpr float kCreditScale = 0.7f;
        ui::TextStyle style;
        style.font = &font->font;
        if (const UiFontStyle* node_style = assets_->FontStyleByHash(back->font_name, text_language_)) {
            style.font_width = node_style->width;
            style.font_height = node_style->height;
            style.text_space = node_style->text_space;
            style.line_space = node_style->line_space;
        }
        style.font_width *= kCreditScale;
        style.font_height *= kCreditScale;
        style.text_space *= kCreditScale;
        style.line_space *= kCreditScale;
        ui::TextLayout layout = ui::LayoutText(credit, style, 0.0f);
        ui::PlaceText(layout, {UiCanvas::kWidth - 420.0f, UiCanvas::kHeight - 34.0f}, {UiCanvas::kWidth - 30.0f, UiCanvas::kHeight - 14.0f},
                      ui::TextAlign::End, ui::TextAlign::End, ui::TextAlign::Center);
        DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, 0.32f), ui::UiBlend::Alpha);
    }
}

}
