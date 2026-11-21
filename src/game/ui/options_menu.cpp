#include "game/ui/options_menu.h"

#include <algorithm>
#include <cmath>
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
#include "game/ui/prompt_hint.h"
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
constexpr uint16_t kBrightnessIcon = 44;
constexpr uint16_t kBrightnessGlow = 46;
constexpr uint16_t kSubtitleIcon = 141;
constexpr uint16_t kSubtitleGlow = 144;
constexpr uint16_t kVerticalIcon = 89;
constexpr uint16_t kVerticalGlow = 91;
constexpr uint16_t kBackIcon = 113;
constexpr uint16_t kBackGlow = 116;
constexpr uint16_t kSubtitleList = 118;
constexpr uint16_t kVerticalRow = 79;
constexpr uint16_t kGaugeGroup = 40;
constexpr glm::vec2 kEntryAt{32.0f, -26.0f};
constexpr glm::vec2 kButtonBarShift{3.0f, 0.0f};
constexpr glm::vec2 kEntryBarShift{-15.5f, 0.0f};
constexpr float kStreetReturnY = -24.0f;
constexpr float kCrosshairY = -21.0f;
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

MenuInput MapMenuInput(const InputState& input, uint32_t previous_held, uint32_t previous_dirs) {
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
    out.tab_prev = (input.menu_keys_pressed & kMenuKeyTabPrev) != 0;
    out.tab_next = (input.menu_keys_pressed & kMenuKeyTabNext) != 0;
    out.tab_from_e = (input.menu_keys_pressed & kMenuKeyFromE) != 0;
    out.donate = (input.menu_keys_pressed & kMenuKeyDonate) != 0;
    out.update = (input.menu_keys_pressed & kMenuKeyUpdate) != 0;
    out.street_return = (input.raw_pressed & kRawL1) != 0 || input.house_pressed;
    out.crosshair = (input.raw_pressed & kRawL3) != 0 || (input.menu_keys_pressed & kMenuKeyCrosshair) != 0;
    out.survival_promo = (input.raw_pressed & kRawR2) != 0 || (input.menu_keys_pressed & kMenuKeySurvival) != 0;
    out.held_dirs = input.raw_held & (kRawUp | kRawDown | kRawLeft | kRawRight);
    const auto stick = [&](float v, uint32_t positive, uint32_t negative) {
        const float on = 0.5f;
        const float off = 0.35f;
        if (v > on || (v > off && (previous_dirs & positive))) out.held_dirs |= positive;
        else if (v < -on || (v < -off && (previous_dirs & negative))) out.held_dirs |= negative;
    };
    stick(input.left_stick.y, kRawUp, kRawDown);
    stick(input.left_stick.x, kRawRight, kRawLeft);
    out.click = input.click;
    out.pointer_held = input.click_held;
    out.right_click = input.right_click;
    out.wheel = input.wheel;
    out.music = input.music_pressed;
    out.right_stick = input.right_stick;
    out.armory_tab = (input.menu_keys_pressed & kMenuKeyArmoryTab) != 0 || (input.raw_pressed & (kRawL2 | kRawR2)) != 0;
    out.armory_replay = (input.menu_keys_pressed & kMenuKeyArmoryReplay) != 0 || (input.raw_pressed & kRawSquare) != 0;
    out.armory_apply_all = (input.menu_keys_pressed & kMenuKeyArmoryApplyAll) != 0 || (input.raw_pressed & kRawTriangle) != 0;
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
    if (const ui::UifNode* bar81 = derived_->FindById(81); bar81 && !bar81->positions.empty()) {
        float lo = bar81->positions.front().x, hi = lo;
        for (const glm::vec2& v : bar81->positions) {
            lo = std::min(lo, v.x);
            hi = std::max(hi, v.x);
        }
        bar_half_ = std::max(0.5f, (hi - lo) * bar81->size.x * 0.5f);
    }
    auto button_bar = [&](uint16_t parent_id, glm::vec2 pos, std::string_view name) {
        ui::UifNode node = *derived_->FindById(80);
        node.parent = derived_->IndexOfId(parent_id);
        node.translate = glm::vec4(pos, 0.0f, 0.0f);
        const uint16_t id = derived_->AddNode(std::move(node), StrCode64(std::format("option_{}_bar", name)));
        ui::UifNode image = *derived_->FindById(81);
        image.parent = derived_->IndexOfId(id);
        image.translate.x = 0.0f;
        image.translate.y = 0.0f;
        derived_->AddNode(std::move(image), StrCode64(std::format("option_{}_bar_image", name)));
        return id;
    };
    quit_bar_ = button_bar(kContent, {-22.0f + kButtonBarShift.x, -27.0f}, "quit");
    street_bar_ = button_bar(kContent, {-22.0f + kButtonBarShift.x, kStreetReturnY}, "street");
    crosshair_bar_ = button_bar(kContent, {-22.0f + kButtonBarShift.x, kCrosshairY}, "crosshair");
    auto in_content = [&](uint16_t id) -> glm::vec2 {
        glm::vec2 pos(0.0f);
        int idx = derived_->IndexOfId(id);
        const int content_idx = derived_->IndexOfId(kContent);
        while (idx >= 0 && idx != content_idx) {
            const ui::UifNode& n = derived_->Nodes()[idx];
            pos += glm::vec2(n.translate);
            idx = n.parent;
        }
        return pos;
    };
    const glm::vec2 sub_pos = in_content(kSubtitleLabel);
    const glm::vec2 title_pos = in_content(kTitle);
    const float promo_x = sub_pos.x;
    const float promo_title_y = title_pos.y + 3.2f;
    const float promo_action_y = promo_title_y - 2.7f;
    promo_bar_ = button_bar(kContent, {promo_x + kButtonBarShift.x, promo_action_y}, "promo");
    BuildEntryRow(*derived_, icon_atlas, icon_glow);
    ui::UifNode quit = *derived_->FindById(kBack);
    quit.parent = derived_->IndexOfId(kContent);
    quit.translate.x = -22.0f;
    quit.translate.y = -27.0f;
    quit_label_ = derived_->AddNode(std::move(quit), StrCode64("option_quit"));
    ui::UifNode street = *derived_->FindById(kBack);
    street.parent = derived_->IndexOfId(kContent);
    street.translate.x = -22.0f;
    street.translate.y = kStreetReturnY;
    street_label_ = derived_->AddNode(std::move(street), StrCode64("option_street_return"));
    ui::UifNode crosshair = *derived_->FindById(kBack);
    crosshair.parent = derived_->IndexOfId(kContent);
    crosshair.translate.x = -22.0f;
    crosshair.translate.y = kCrosshairY;
    crosshair_label_ = derived_->AddNode(std::move(crosshair), StrCode64("option_crosshair"));
    ui::UifNode promo_title = *derived_->FindById(kBrightnessLabel);
    promo_title.parent = derived_->IndexOfId(kContent);
    promo_title.translate = glm::vec4(promo_x, promo_title_y, 0.0f, 0.0f);
    promo_title.size.x = std::max(promo_title.size.x, 420.0f);
    promo_title_ = derived_->AddNode(std::move(promo_title), StrCode64("option_promo_title"));
    ui::UifNode promo_line = *derived_->FindById(74);
    promo_line.parent = derived_->IndexOfId(kContent);
    promo_line.translate = glm::vec4(promo_x + 16.0f, promo_title_y - 1.0f, 0.0f, 0.0f);
    promo_line_ = derived_->AddNode(std::move(promo_line), StrCode64("option_promo_line"));
    ui::UifNode promo_action = *derived_->FindById(kBack);
    promo_action.parent = derived_->IndexOfId(kContent);
    promo_action.translate = glm::vec4(promo_x, promo_action_y, 0.0f, 0.0f);
    promo_action.size.x = std::max(promo_action.size.x, 360.0f);
    promo_action_ = derived_->AddNode(std::move(promo_action), StrCode64("option_promo_action"));
    auto prompt_node_at = [&](uint16_t id, float x, float y, std::string_view name) {
        ui::UifNode node = *derived_->FindById(id);
        const ui::UifNode& back_text = *derived_->FindById(kBack);
        node.parent = derived_->IndexOfId(kContent);
        node.translate.x = x + (node.translate.x - back_text.translate.x);
        node.translate.y = y + (node.translate.y - back_text.translate.y);
        return derived_->AddNode(std::move(node), StrCode64(name));
    };
    quit_icon_ = prompt_node_at(kBackIcon, -22.0f, -27.0f, "option_quit_icon");
    quit_glow_ = prompt_node_at(kBackGlow, -22.0f, -27.0f, "option_quit_glow");
    street_icon_ = prompt_node_at(kBackIcon, -22.0f, kStreetReturnY, "option_street_icon");
    street_glow_ = prompt_node_at(kBackGlow, -22.0f, kStreetReturnY, "option_street_glow");
    crosshair_icon_ = prompt_node_at(kBackIcon, -22.0f, kCrosshairY, "option_crosshair_icon");
    crosshair_glow_ = prompt_node_at(kBackGlow, -22.0f, kCrosshairY, "option_crosshair_glow");
    promo_icon_ = prompt_node_at(kBackIcon, promo_x, promo_action_y, "option_promo_icon");
    promo_glow_ = prompt_node_at(kBackGlow, promo_x, promo_action_y, "option_promo_glow");
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
    const ui::UifNode& entry_text_node = *derived_->FindById(entry_label_);
    entry_bar_ = button_bar(entry_group_, glm::vec2(entry_text_node.translate) + kEntryBarShift, "entry");
    const ui::UifNode& back_text_node = *derived_->FindById(kBack);
    back_bar_ = button_bar(kContent, glm::vec2(back_text_node.translate) + kButtonBarShift, "back");
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
        {crosshair_icon_, crosshair_glow_, {PromptButton::L3, KeyAction::MenuCrosshair, KeyAction::MenuCrosshair}},
        {promo_icon_, promo_glow_, {PromptButton::R2, KeyAction::MenuSurvival, KeyAction::MenuSurvival}},
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
    LogDebug("ui: options page switch to {}", page == Page::Pc ? "PC" : "original");
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
    view_.State(pc_.Root()).visible = pc > 0.0f && !pc_.IsQuestion() && !pc_.ScreenShown() &&
                                      !(pc_source_ && (pc_source_->IsArmory() || pc_source_->IsNightHub()));
    const bool see_through = page_ == Page::Pc && pc_.SeeThrough();
    for (const uint16_t id : {uint16_t{2}, uint16_t{37}, uint16_t{155}, uint16_t{158}}) {
        if (see_through) {
            view_.State(id).color_scale.a = 0.0f;
        } else if (see_through_) {
            view_.State(id).color_scale.a = 1.0f;
        }
    }
    see_through_ = see_through;
    view_.State(pc_.Root()).color_scale.a = pc;
    view_.State(entry_group_).visible = pc_source_ != nullptr;
    view_.State(street_label_).visible = street_shown_;
    view_.State(street_icon_).visible = street_shown_;
    view_.State(street_glow_).visible = street_shown_;
    view_.State(crosshair_label_).visible = crosshair_shown_;
    view_.State(crosshair_icon_).visible = crosshair_shown_;
    view_.State(crosshair_glow_).visible = crosshair_shown_;
    view_.State(promo_title_).visible = false;
    view_.State(promo_line_).visible = false;
    view_.State(promo_action_).visible = false;
    view_.State(promo_icon_).visible = false;
    view_.State(promo_glow_).visible = false;
    view_.State(entry_label_).text = std::string(PcText("pc_entry", text_language_));
    if (page_ == Page::Pc || next_page_ == Page::Pc) {
        pc_.SetMeasure(view_, text_language_);
        pc_.Apply(view_, text_language_);
    }
    ApplyPrompts();
}

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
    for (const uint16_t id : {pc_.Help(), pc_.NavigationText(), pc_.TabPrevText(), pc_.TabNextText(), pc_.DonateText(), pc_.UpdateText()}) {
        UifNodeState& help = view_.State(id);
        help.inline_pictures.clear();
        if (help.text && HasPromptTokens(*help.text)) {
            const PromptLine line = ExpandPromptHint(*help.text, prompt_style_);
            help.text = line.text;
            for (const Prompt& prompt : line.pictures) {
                const PromptGlyph glyph = assets_->PromptPicture(prompt, prompt_style_);
                help.inline_pictures.push_back(UifInlinePicture{derived_->AddTexture(glyph.icon), derived_->AddTexture(glyph.glow), glyph.uv0, glyph.uv1,
                                                                glyph.width, glyph.body_width, glyph.body_height});
            }
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

std::string OptionsMenu::DialogKind(int* focus) const {
    *focus = 0;
    if (state_ != State::Active) return "none";
    if (page_ == Page::Pc) {
        if (pc_.IsQuestion()) {
            *focus = pc_.Cursor();
            return "question";
        }
        if (pc_.Confirming()) {
            *focus = pc_.DialogFocus();
            return "confirm";
        }
        return "none";
    }
    if (quit_confirming_ || street_confirming_) {
        *focus = ask_nav_.Focus();
        return quit_confirming_ ? "quit" : "street";
    }
    return "none";
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
    view_.State(quit_label_).font_type = UiFontType::PcSystem;
    view_.State(quit_label_).text = std::string(PcText("pc_quit", text_language_));
    view_.State(quit_label_).color = glm::vec4(1.0f, 1.0f, 1.0f, quit_focused_ || quit_confirming_ ? 1.0f : .65f);
    view_.State(street_label_).font_type = UiFontType::PcSystem;
    view_.State(street_label_).text = std::string(PcText(leave_date_ ? "pc_lisa_date_leave" : "pc_street_return", text_language_));
    view_.State(street_label_).color = glm::vec4(1.0f, 1.0f, 1.0f, street_confirming_ ? 1.0f : .65f);
    view_.State(crosshair_label_).font_type = UiFontType::PcSystem;
    view_.State(crosshair_label_).text = std::string(PcText("pc_crosshair", text_language_));
    view_.State(crosshair_label_).color = glm::vec4(1.0f, 1.0f, 1.0f, .65f);
    view_.State(promo_title_).font_type = UiFontType::PcSystem;
    view_.State(promo_title_).text = std::string(PcText("pc_survival_promo_tag", text_language_)) + ": " + std::string(PcText("pc_survival", text_language_));
    view_.State(promo_title_).color = glm::vec4(1.0f, 1.0f, 1.0f, 0.95f);
    view_.State(promo_action_).font_type = UiFontType::PcSystem;
    view_.State(promo_action_).text = std::string(PcText("pc_survival_promo_play", text_language_));
    view_.State(promo_action_).color = glm::vec4(1.0f, 1.0f, 1.0f, 0.85f);
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
    if (pc_source_) pc_source_->MenuShown();
    quit_confirming_ = false;
    quit_focused_ = false;
    leave_date_ = game.LisaDateActive();
    street_shown_ = game.StreetWalkActive() || leave_date_;
    crosshair_shown_ = game.SurvivalActive() && !leave_date_;
    survival_promo_shown_ = !first_boot && pc_source_ && pc_source_->SurvivalPromoDue() && !game.SurvivalActive() && !leave_date_;
    survival_promo_seen_ = false;
    survival_locked_confirming_ = false;
    promo_toast_t_ = 0.0f;
    armory_view_.Reset();
    night_hub_view_.Reset();
    street_confirming_ = false;
    street_return_ = false;
    float zoom_frame[2] = {-1.0f, -1.0f};
    const Animation* zoom_in = Find("UI_sys_opt_zoom_in");
    if (zoom_in && zoom_started_) {
        zoom_frame[0] = players_.PlayingFrame(zoom_in->main);
        zoom_frame[1] = players_.PlayingFrame(zoom_in->shader);
    }
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
    if (zoom_in && zoom_frame[0] >= 0.0f) players_.Hold(zoom_in->main, zoom_frame[0]);
    if (zoom_in && zoom_frame[1] >= 0.0f) players_.Hold(zoom_in->shader, zoom_frame[1]);
    if (page_ == Page::Pc) CancelPhotoZoom();
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
        if (survival_promo_seen_ && pc_source_) {
            pc_source_->DismissSurvivalPromo();
            survival_promo_shown_ = false;
            survival_promo_seen_ = false;
        }
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
    {
        const float target = state_ == State::SetIn || state_ == State::Active ? 1.0f : 0.0f;
        const float step = dt / 0.25f;
        pillar_alpha_ = target > pillar_alpha_ ? std::min(target, pillar_alpha_ + step) : std::max(target, pillar_alpha_ - step);
    }
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
    if (state_ == State::SetIn && (!Playing("UI_sys_opt_setin") || ShownAlpha() >= 0.999f)) {
        state_ = State::Active;
    }
    const bool taking_input = (state_ == State::Active || state_ == State::SetIn) && !switching;
    if (taking_input) {
        if (page_ == Page::Pc) {
            if (pc_source_ && pc_source_->IsArmory()) {
                const auto ares = armory_view_.Update(game, *pc_source_, input, dt);
                if (ares == ArmoryView::Result::Back) {
                    pc_source_->Back();
                    pc_.Refresh(false);
                    armory_view_.Reset();
                }
            } else if (pc_source_ && pc_source_->IsNightHub()) {
                const auto nres = night_hub_view_.Update(game, *pc_source_, input, dt);
                if (nres == NightHubView::Result::Activated) {
                    if (game.ProgressResetPending() || game.LoopReloadPending() || game.TakeMenuCloseRequest()) {
                        Commit(game);
                        Close();
                    } else {
                        pc_.Refresh(false);
                    }
                } else if (nres == NightHubView::Result::Back) {
                    const bool stayed = pc_source_->Back();
                    if (pc_source_->TakeCloseToGame()) {
                        Commit(game);
                        Close();
                    } else if (!stayed || pc_source_->TakeReturnToOptions()) {
                        SwitchPage(game, Page::Original);
                    } else {
                        pc_.Refresh(false);
                    }
                    night_hub_view_.Reset();
                }
            } else {
                const auto result = pc_.Update(game, input, dt);
                if(game.ProgressResetPending() || game.LoopReloadPending()) { Commit(game); Close(); }
                if (result == PcSettingsPage::Result::Quit) {
                    Commit(game);
                    game.RequestOptionsSave();
                    LogInfo("ui: quit selected from PC settings");
                    game.RequestQuit();
                } else if (result == PcSettingsPage::Result::Back || (pc_source_ && pc_source_->TakeReturnToOptions())) {
                    SwitchPage(game, Page::Original);
                }
            }
        } else {
            UpdateOriginal(game, input, dt);
        }
    }
    {
        const bool shown = page_ == Page::Original && taking_input && !quit_confirming_ && !street_confirming_ && !survival_locked_confirming_;
        if (!shown) hovered_ = Button::None;
        if (shown && survival_promo_shown_) {
            promo_toast_t_ = std::min(1.0f, promo_toast_t_ + dt * 2.5f);
            if (state_ == State::Active) survival_promo_seen_ = true;
        }
        const float ramp = std::clamp(hover_frame_ / 6.0f, 0.0f, 1.0f);
        const float flash = std::max(SelectionFlash(hover_frame_), ramp * ramp * (3.0f - 2.0f * ramp));
        for (const auto& [bar, label, button, allowed] : {
                 std::tuple{entry_bar_, entry_label_, Button::PcSettings, pc_source_ != nullptr},
                 std::tuple{back_bar_, kBack, Button::Back, true},
                 std::tuple{quit_bar_, quit_label_, Button::Quit, true},
                 std::tuple{street_bar_, street_label_, Button::StreetReturn, street_shown_},
                 std::tuple{crosshair_bar_, crosshair_label_, Button::Crosshair, crosshair_shown_},
                 std::tuple{promo_bar_, promo_action_, Button::SurvivalPromo, false},
             }) {
            const bool hovered = shown && allowed && hovered_ == button;
            auto& st = view_.State(bar);
            st.visible = hovered;
            st.color = glm::vec4(1.0f, 1.0f, 1.0f, flash);
            if (hovered) {
                const ui::UifNode* bar_node = derived_->FindById(bar);
                const ui::UifNode* label_node = derived_->FindById(label);
                if (bar_node && label_node) {
                    std::optional<glm::vec2> span = view_.TextSpan(label);
                    if (!span) {
                        const auto& lstate = view_.State(label);
                        const std::string_view txt = lstate.text ? std::string_view(*lstate.text) : std::string_view();
                        const UiFontType ftype = lstate.font_type.value_or(UiFontType::PcSystem);
                        if (!txt.empty()) {
                            if (const auto m = view_.MeasureText(label, txt, text_language_, ftype)) {
                                const float w_units = m->x / UiCanvas::kUnit;
                                span = button == Button::PcSettings ? glm::vec2(-w_units, 0.0f) : glm::vec2(0.0f, w_units);
                            }
                        }
                    }
                    if (span) {
                        const float left_edge = label_node->translate.x + span->x - (button == Button::PcSettings ? 3.0f : 3.2f);
                        const float right_edge = label_node->translate.x + span->y;
                        const float mid_x = 0.5f * (left_edge + right_edge);
                        const float span_w = std::max(0.0f, right_edge - left_edge);
                        st.offset = glm::vec2(mid_x - bar_node->translate.x, 0.0f);
                        const float want_half = std::max(bar_half_ * 0.85f, span_w * 0.5f + 5.5f);
                        st.scale = glm::vec2(want_half / bar_half_, 1.0f);
                    }
                }
            }
        }
        view_.State(quit_label_).color = glm::vec4(1.0f, 1.0f, 1.0f, quit_focused_ || quit_confirming_ || (shown && hovered_ == Button::Quit) ? 1.0f : .65f);
        view_.State(street_label_).color = glm::vec4(1.0f, 1.0f, 1.0f, street_confirming_ || (shown && hovered_ == Button::StreetReturn) ? 1.0f : .65f);
        view_.State(crosshair_label_).color = glm::vec4(1.0f, 1.0f, 1.0f, (shown && hovered_ == Button::Crosshair) ? 1.0f : .65f);
        view_.State(promo_action_).color = glm::vec4(1.0f, 1.0f, 1.0f, (shown && hovered_ == Button::SurvivalPromo) ? 1.0f : 0.88f);
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
    const bool collected = (game.Nazo().PhotoWord() & 0x100) != 0;
    if (auto* left = derived_->FindById(kImageLeft)) {
        left->material.textures = collected ? clean_brain_textures_ : fragment_brain_textures_;
    }
    ApplySubtitleSelection();
}

void OptionsMenu::OpenAsk(Game& game, bool quit, uint32_t held) {
    quit_confirming_ = quit;
    quit_focused_ = quit;
    street_confirming_ = !quit;
    ask_nav_.Open(0, held);
    ask_flash_ = 0.0f;
    if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
    ApplyTexts();
}

void OptionsMenu::UpdateAsk(Game& game, const MenuInput& input, float dt) {
    ask_flash_ += dt * 60.0f;
    if (survival_locked_confirming_) {
        const dialog::Result result = ask_nav_.Update(input, 1, text_language_, true);
        if (result != dialog::Result::None) {
            survival_locked_confirming_ = false;
            if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
        }
        return;
    }
    const bool quit = quit_confirming_;
    const dialog::Result result = ask_nav_.Update(input, 2, text_language_, true);
    if (ask_nav_.Moved()) {
        ask_flash_ = 0.0f;
        if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
    }
    const bool yes = result == dialog::Result::Chosen && ask_nav_.Chosen() == 0;
    if (result == dialog::Result::None) return;
    if (!yes) {
        quit_confirming_ = street_confirming_ = quit_focused_ = false;
        if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
        ApplyTexts();
        return;
    }
    if (quit) {
        Commit(game);
        game.RequestOptionsSave();
        game.RequestQuit();
        LogInfo("ui: quit selected from Options");
        return;
    }
    street_confirming_ = false;
    street_return_ = true;
    if (game.Audio()) game.Audio()->PostEvent(kChangeSound, nullptr);
    LogInfo("ui: {} selected from Options", leave_date_ ? "leave the date" : "return to the house");
    Close();
}

void OptionsMenu::UpdateOriginal(Game& game, const MenuInput& raw_input, float dt) {
    MenuInput input = raw_input;
    leave_date_ = game.LisaDateActive();
    street_shown_ = game.StreetWalkActive() || leave_date_;
    crosshair_shown_ = game.SurvivalActive() && !leave_date_;
    if (quit_confirming_ || street_confirming_ || survival_locked_confirming_) {
        UpdateAsk(game, input, dt);
        return;
    }
    if (input.pointer_valid) {
        const Button over = ButtonAt(input.pointer);
        if (over != hovered_) {
            hovered_ = over;
            hover_frame_ = 0.0f;
            if (over != Button::None && game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
        }
    }
    hover_frame_ += dt * 60.0f;
    if (street_shown_ && input.street_return) {
        OpenAsk(game, false, input.held_dirs);
        return;
    }
    if (crosshair_shown_ && input.crosshair) {
        OpenCrosshair(game);
        return;
    }
    if (survival_promo_shown_ && input.survival_promo) {
        OpenSurvivalPromo(game);
        return;
    }
    if (input.switch_column || (quit_focused_ && (input.accept || input.open_pc))) {
        OpenAsk(game, true, input.held_dirs);
        return;
    }
    if (input.pointer_valid && input.click) {
        OriginalClick(game, input.pointer, input);
    }
    if (input.open_pc && pc_source_) {
        Commit(game);
        if (language_timer_) {
            language_timer_ = false;
            if (selector_ != 4) {
                text_language_ = selector_ - 5;
                ApplyTexts();
            }
        }
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
    if (game.Nazo().IsPhotoOptionPending() && !Playing("UI_sys_opt_setin")) {
        const bool zooming = Playing("UI_sys_opt_zoom_in");
        if (zoom_started_) zoom_elapsed_ += dt;
        if (input.zoom_pressed) {
            zoom_started_ = true;
            zoom_elapsed_ = 0.0f;
            Play("UI_sys_opt_zoom_in");
            LogInfo("ui: option menu zoom pressed, zoom_in {}", zooming ? "restarted" : "started");
        } else if (input.zoom_released) {
            zoom_released_ = true;
            LogInfo("ui: option menu zoom released{}", zooming ? ", PhotoOption waits for the zoom_in" : "");
        }
        if (zoom_released_ && !zooming) {
            Play("UI_sys_opt_zoom_out");
            game.Nazo().SetCondition("PhotoOption");
            LogInfo("ui: option menu sent PhotoOption, {:.2f} s after the zoom press", zoom_elapsed_);
            zoom_released_ = false;
            zoom_started_ = false;
        }
    }
    if (input.close) {
        Close();
    }
}

void OptionsMenu::OpenCrosshair(Game& game) {
    if (!pc_source_ || !pc_source_->OpenCrosshair()) return;
    Commit(game);
    LogInfo("ui: crosshair selected from Options");
    SwitchPage(game, Page::Pc);
}

void OptionsMenu::OpenSurvivalPromo(Game& game) {
    if (!pc_source_) return;
    pc_source_->DismissSurvivalPromo();
    survival_promo_shown_ = false;
    survival_promo_seen_ = false;
    hovered_ = Button::None;
    if (!pc_source_->SurvivalUnlocked()) {
        survival_locked_confirming_ = true;
        ask_nav_.Open(0, 0);
        ask_flash_ = 0.0f;
        if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
        LogInfo("ui: survival night promo blocked (NG+4 required)");
        return;
    }
    if (!pc_source_->OpenSurvivalNight()) return;
    Commit(game);
    LogInfo("ui: survival night promo accepted from Options");
    SwitchPage(game, Page::Pc);
}

OptionsMenu::Button OptionsMenu::ButtonAt(glm::vec2 units) const {
    if (survival_promo_shown_ && !quit_confirming_ && !street_confirming_ && !survival_locked_confirming_) {
        const glm::vec2 px(640.0f + units.x * UiCanvas::kUnit, 360.0f - units.y * UiCanvas::kUnit);
        if (px.x >= 884.0f && px.x <= 1246.0f && px.y >= 22.0f && px.y <= 106.0f) {
            if (px.x >= 1212.0f && px.y <= 54.0f) return Button::SurvivalPromoClose;
            return Button::SurvivalPromo;
        }
    }
    if (crosshair_shown_) {
        const glm::vec2 line = view_.WorldPosition(crosshair_label_);
        if (units.x >= line.x - 6.0f && units.x <= line.x + 15.0f && std::abs(units.y - line.y) <= 1.2f) return Button::Crosshair;
    }
    if (street_shown_) {
        const glm::vec2 street = view_.WorldPosition(street_label_);
        if (units.x >= street.x - 6.0f && units.x <= street.x + 22.0f && std::abs(units.y - street.y) <= 1.2f) return Button::StreetReturn;
    }
    const glm::vec2 quit = view_.WorldPosition(quit_label_);
    if (units.x >= quit.x - 6.0f && units.x <= quit.x + 15.0f && std::abs(units.y - quit.y) <= 1.2f) return Button::Quit;
    const glm::vec2 entry = view_.WorldPosition(entry_group_);
    if (pc_source_ && units.x >= entry.x - 22.0f && units.x <= entry.x + 12.0f && std::abs(units.y - entry.y) <= 1.5f) return Button::PcSettings;
    const glm::vec2 back = view_.WorldPosition(kBack);
    if (units.x >= back.x - 3.0f && units.x <= back.x + 10.0f && std::abs(units.y - back.y) <= 2.0f) return Button::Back;
    return Button::None;
}

void OptionsMenu::OriginalClick(Game& game, glm::vec2 units, MenuInput& input) {
    if (survival_promo_shown_) {
        const Button b = ButtonAt(units);
        if (b == Button::SurvivalPromoClose) {
            if (pc_source_) pc_source_->DismissSurvivalPromo();
            survival_promo_shown_ = false;
            survival_promo_seen_ = false;
            hovered_ = Button::None;
            if (game.Audio()) game.Audio()->PostEvent(kCursorSound, nullptr);
            return;
        }
        if (b == Button::SurvivalPromo) {
            OpenSurvivalPromo(game);
            return;
        }
    }
    if (crosshair_shown_) {
        const glm::vec2 line = view_.WorldPosition(crosshair_label_);
        if (units.x >= line.x - 6.0f && units.x <= line.x + 15.0f && std::abs(units.y - line.y) <= 1.2f) {
            OpenCrosshair(game);
            return;
        }
    }
    if (street_shown_) {
        const glm::vec2 street = view_.WorldPosition(street_label_);
        if (units.x >= street.x - 6.0f && units.x <= street.x + 22.0f && std::abs(units.y - street.y) <= 1.2f) {
            OpenAsk(game, false, input.held_dirs);
            return;
        }
    }
    const glm::vec2 quit = view_.WorldPosition(quit_label_);
    if (units.x >= quit.x - 6.0f && units.x <= quit.x + 15.0f && std::abs(units.y - quit.y) <= 1.2f) {
        OpenAsk(game, true, input.held_dirs);
        return;
    }
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

void OptionsMenu::DrawScreen(ui::UiBatch& batch, const UiCanvas& canvas) {
    const PcScreen screen = pc_.Screen();
    const glm::vec2 full(static_cast<float>(batch.Extent().width), static_cast<float>(batch.Extent().height));
    const auto plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    constexpr float kStripTop = 0.76f * UiCanvas::kHeight;
    if (screen.scene) {
        const float top = canvas.ToTarget({0.0f, kStripTop}).y;
        batch.Quad(glm::vec2(0.0f, top), full, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), plain, ui::UiShade::Solid,
                   ui::UiBlend::Alpha);
    } else {
        batch.Quad(glm::vec2(0.0f), full, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), plain, ui::UiShade::Solid,
                   ui::UiBlend::Alpha);
    }
    if (screen.pattern == 4) {
        DrawBrain(batch, canvas, {440.0f, 270.0f}, 250.0f);
        DrawBrain(batch, canvas, {840.0f, 270.0f}, 250.0f);
    } else if (screen.pattern == 2) {
        DrawBrain(batch, canvas, {640.0f, 245.0f}, 300.0f);
    } else if (screen.pattern != 0) {
        DrawBrain(batch, canvas, {640.0f, 0.38f * UiCanvas::kHeight}, screen.pattern == 1 ? 270.0f : 320.0f);
    }
    UiFont* font = assets_->Font(UiFontType::PcSystem, text_language_);
    if (!font) return;
    const auto text = [&](std::string_view key) {
        return key.find(kPcNoteArgument) != std::string_view::npos ? PcNoteText(key, text_language_) : std::string(PcText(key, text_language_));
    };
    const auto style = [&](float size) {
        ui::TextStyle s;
        s.font = &font->font;
        s.font_width = size;
        s.font_height = size;
        s.line_space = size * 0.25f;
        return s;
    };
    const bool rtl = text_language_ == dialog::kArabic;
    const auto line = [&](const std::string& words, float size, float y0, float y1, glm::vec4 color) {
        ui::TextLayout layout = ui::LayoutText(words, style(size), UiCanvas::kWidth - 240.0f);
        ui::PlaceText(layout, {120.0f, y0}, {UiCanvas::kWidth - 120.0f, y1}, ui::TextAlign::Center, ui::TextAlign::Center, ui::TextAlign::Center, rtl);
        DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha);
    };
    line(text(screen.header), 16.0f, kStripTop + 6.0f, kStripTop + 28.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.55f));
    const float middle = UiCanvas::kWidth * 0.5f;
    {
        ui::TextLayout name = ui::LayoutText(text(screen.label), style(24.0f), 0.0f);
        ui::PlaceText(name, {120.0f, kStripTop + 30.0f}, {middle - 30.0f, kStripTop + 62.0f}, ui::TextAlign::End, ui::TextAlign::End,
                      ui::TextAlign::Center);
        DrawText(batch, canvas, *font, name, glm::vec4(1.0f), ui::UiBlend::Alpha);
        ui::TextLayout value = ui::LayoutText(text(screen.value), style(24.0f), 0.0f);
        ui::PlaceText(value, {middle + 60.0f, kStripTop + 30.0f}, {middle + 300.0f, kStripTop + 62.0f}, ui::TextAlign::Center,
                      ui::TextAlign::Center, ui::TextAlign::Center);
        DrawText(batch, canvas, *font, value, glm::vec4(1.0f), ui::UiBlend::Alpha);
        for (const bool right : {false, true}) {
            ui::TextLayout arrow = ui::LayoutText(right ? ">" : "<", style(24.0f), 0.0f);
            const float x = right ? middle + 320.0f : middle + 20.0f;
            ui::PlaceText(arrow, {x, kStripTop + 30.0f}, {x + 20.0f, kStripTop + 62.0f}, ui::TextAlign::Center, ui::TextAlign::Center,
                          ui::TextAlign::Center);
            DrawText(batch, canvas, *font, arrow, glm::vec4(1.0f, 1.0f, 1.0f, (right ? screen.can_right : screen.can_left) ? 0.9f : 0.25f),
                     ui::UiBlend::Alpha);
        }
    }
    line(text(screen.note), 17.0f, kStripTop + 66.0f, kStripTop + 128.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.8f));
    DrawPromptText(batch, canvas, *assets_, *font, text(screen.hint), prompt_style_, 16.0f, {120.0f, UiCanvas::kHeight - 36.0f},
                   {UiCanvas::kWidth - 120.0f, UiCanvas::kHeight - 10.0f}, ui::TextAlign::Center, ui::TextAlign::Center,
                   glm::vec4(1.0f, 1.0f, 1.0f, 0.6f), true);
}

void OptionsMenu::DrawBrain(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 center, float height) {
    const ui::UifNode* node = derived_ ? derived_->FindById(kImageLeft) : nullptr;
    if (!node || node->positions.empty()) return;
    std::array<uint32_t, 4> textures{TextureManager::kWhite, TextureManager::kWhite, TextureManager::kWhite, TextureManager::kWhite};
    for (int slot = 0; slot < 4; ++slot) {
        const int t = clean_brain_textures_[slot];
        if (t >= 0 && t < static_cast<int>(derived_->Textures().size())) {
            textures[slot] = assets_->Texture(derived_->Textures()[t]);
            textures[slot] |= assets_->TextureAddressBits(textures[slot]);
        }
    }
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const glm::vec2& p : node->positions) {
        lo = glm::min(lo, p * node->size);
        hi = glm::max(hi, p * node->size);
    }
    const glm::vec2 extent = glm::max(hi - lo, glm::vec2(1e-6f));
    const float scale = height / extent.y;
    std::vector<ui::UiVertex> vertices;
    for (const uint16_t i : node->indices) {
        if (i >= node->positions.size() || i >= node->uvs.size()) continue;
        const glm::vec2 local = (node->positions[i] * node->size - (lo + hi) * 0.5f) * scale;
        vertices.push_back({canvas.ToTarget(center + glm::vec2(local.x, -local.y)), node->uvs[i], glm::vec4(1.0f)});
    }
    batch.Draw(vertices, ui::UiDrawParams::Plain(textures[ui::kUifBase]), ui::UiShade::Material, ui::UiBlend::Alpha);
}

float OptionsMenu::ShownAlpha() const {
    if (state_ == State::Active) return 1.0f;
    if (state_ != State::SetIn) return 0.0f;
    const Animation* in = Find("UI_sys_opt_setin");
    const float frame = in ? players_.PlayingFrame(in->main) : -1.0f;
    return frame >= 0.0f ? TrackAlpha("UI_sys_opt_setin", frame) : 1.0f;
}

void OptionsMenu::DrawCrosshairPreview(ui::UiBatch& batch, const UiCanvas& canvas, const CrosshairStyle& style) {
    preview_clock_ += 1.0f / 60.0f;
    const glm::vec2 lo(650.0f, 170.0f), hi(1190.0f, 474.0f);
    const float mid_x = 0.5f * (lo.x + hi.x);
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    auto gradient = [&](glm::vec2 a, glm::vec2 b, glm::vec4 top, glm::vec4 bottom) {
        const std::vector<ui::UiVertex> v = {{canvas.ToTarget(a), {0, 0}, top}, {canvas.ToTarget({b.x, a.y}), {0, 0}, top}, {canvas.ToTarget(b), {0, 0}, bottom},
                                             {canvas.ToTarget(a), {0, 0}, top}, {canvas.ToTarget(b), {0, 0}, bottom}, {canvas.ToTarget({a.x, b.y}), {0, 0}, bottom}};
        batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    gradient(lo, {mid_x, hi.y}, glm::vec4(0.02f, 0.025f, 0.03f, 0.96f), glm::vec4(0.05f, 0.055f, 0.06f, 0.96f));
    gradient({mid_x, lo.y}, hi, glm::vec4(0.62f, 0.55f, 0.42f, 0.96f), glm::vec4(0.28f, 0.26f, 0.22f, 0.96f));
    const glm::vec4 frame(1.0f, 1.0f, 1.0f, 0.25f);
    gradient(lo, {hi.x, lo.y + 1.0f}, frame, frame);
    gradient({lo.x, hi.y - 1.0f}, hi, frame, frame);
    gradient(lo, {lo.x + 1.0f, hi.y}, frame, frame);
    gradient({hi.x - 1.0f, lo.y}, hi, frame, frame);
    const float cone = 5.0f + 5.0f * std::sin(preview_clock_ * 2.2f);
    const float cycle = std::fmod(preview_clock_, 2.4f);
    const int kind = static_cast<int>(preview_clock_ / 2.4f) % 3;
    const float mark = style.hit_marker && cycle < 0.45f ? 1.0f - cycle / 0.45f : 0.0f;
    for (const float x : {0.5f * (lo.x + mid_x), 0.5f * (mid_x + hi.x)}) {
        const glm::vec2 big(x, lo.y + 120.0f), small(x, hi.y - 52.0f);
        DrawCrosshair(batch, canvas, big, style, cone, 1.0f, 3.0f);
        DrawHitMarker(batch, canvas, big, mark, kind == 1, kind == 2, 3.0f);
        DrawCrosshair(batch, canvas, small, style, cone, 1.0f, 1.0f);
        DrawHitMarker(batch, canvas, small, mark, kind == 1, kind == 2, 1.0f);
    }
}

void OptionsMenu::DrawSurvivalToast(ui::UiBatch& batch, const UiCanvas& canvas) {
    UiFont* font = assets_->Font(UiFontType::PcSystem, text_language_);
    if (!font) return;

    const float t = std::clamp(promo_toast_t_, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    const float alpha = ease * ShownAlpha();
    if (alpha <= 0.01f) return;

    const float slide_x = (1.0f - ease) * 26.0f;
    const glm::vec2 lo(884.0f + slide_x, 22.0f);
    const glm::vec2 hi(1246.0f + slide_x, 106.0f);
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);

    auto solid = [&](glm::vec2 a, glm::vec2 b, glm::vec4 c) {
        c.a *= alpha;
        if (c.a <= 0.001f) return;
        batch.Quad(canvas.ToTarget(a), canvas.ToTarget(b), {0.0f, 0.0f}, {1.0f, 1.0f}, c, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    auto hgrad = [&](float x0, float x1, float y0, float y1, glm::vec4 cl, glm::vec4 cr) {
        cl.a *= alpha;
        cr.a *= alpha;
        const std::vector<ui::UiVertex> v = {
            {canvas.ToTarget({x0, y0}), {0, 0}, cl}, {canvas.ToTarget({x1, y0}), {0, 0}, cr}, {canvas.ToTarget({x1, y1}), {0, 0}, cr},
            {canvas.ToTarget({x0, y0}), {0, 0}, cl}, {canvas.ToTarget({x1, y1}), {0, 0}, cr}, {canvas.ToTarget({x0, y1}), {0, 0}, cl},
        };
        batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    auto frame = [&](glm::vec2 a, glm::vec2 b, float w, glm::vec4 c) {
        solid(a, {b.x, a.y + w}, c);
        solid({a.x, b.y - w}, b, c);
        solid({a.x, a.y + w}, {a.x + w, b.y - w}, c);
        solid({b.x - w, a.y + w}, {b.x, b.y - w}, c);
    };

    const bool hov_card = hovered_ == Button::SurvivalPromo;
    const bool hov_close = hovered_ == Button::SurvivalPromoClose;
    const float breath = 0.5f + 0.5f * std::sin(clock_ * 2.4f);

    solid(lo, hi, {0.02f, 0.024f, 0.03f, 0.90f});
    frame(lo, hi, 1.5f, {1.0f, 1.0f, 1.0f, hov_card ? 0.56f : 0.26f});
    const float mid_x = 0.5f * (lo.x + hi.x);
    hgrad(lo.x + 12.0f, mid_x, lo.y, lo.y + 2.0f, {1.0f, 1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 0.36f + 0.14f * breath});
    hgrad(mid_x, hi.x - 12.0f, lo.y, lo.y + 2.0f, {1.0f, 1.0f, 1.0f, 0.36f + 0.14f * breath}, {1.0f, 1.0f, 1.0f, 0.0f});

    solid({lo.x + 15.0f, lo.y + 15.0f}, {lo.x + 20.0f, lo.y + 20.0f}, {1.0f, 1.0f, 1.0f, 0.55f + 0.45f * breath});
    const std::string header = std::string(PcText("pc_survival_promo_tag", text_language_)) + ": " + std::string(PcText("pc_survival", text_language_));
    {
        ui::TextStyle st;
        st.font = &font->font;
        st.font_width = 14.5f;
        st.font_height = 14.5f;
        ui::TextLayout layout = ui::LayoutText(header, st, 0.0f);
        const float max_w = (hi.x - 40.0f) - (lo.x + 28.0f);
        if (layout.width > max_w && layout.width > 1.0f) {
            const float s = max_w / layout.width;
            st.font_width = std::max(10.0f, 14.5f * s);
            st.font_height = std::max(10.0f, 14.5f * s);
            layout = ui::LayoutText(header, st, 0.0f);
        }
        ui::PlaceText(layout, {lo.x + 28.0f, lo.y + 6.0f}, {hi.x - 38.0f, lo.y + 30.0f}, ui::TextAlign::Start, ui::TextAlign::Start,
                      ui::TextAlign::Center, text_language_ == dialog::kArabic);
        DrawText(batch, canvas, *font, layout, {1.0f, 1.0f, 1.0f, 0.72f * alpha}, ui::UiBlend::Alpha);
    }

    {
        ui::TextStyle st;
        st.font = &font->font;
        st.font_width = 13.5f;
        st.font_height = 13.5f;
        ui::TextLayout xlay = ui::LayoutText("x", st, 0.0f);
        ui::PlaceText(xlay, {hi.x - 30.0f, lo.y + 5.0f}, {hi.x - 8.0f, lo.y + 29.0f}, ui::TextAlign::Center, ui::TextAlign::Center,
                      ui::TextAlign::Center, false);
        DrawText(batch, canvas, *font, xlay, {1.0f, 1.0f, 1.0f, (hov_close ? 0.95f : 0.42f) * alpha}, ui::UiBlend::Alpha);
    }

    hgrad(lo.x + 14.0f, hi.x - 14.0f, lo.y + 34.0f, lo.y + 35.0f, {1.0f, 1.0f, 1.0f, 0.24f}, {1.0f, 1.0f, 1.0f, 0.0f});

    const glm::vec2 plo(lo.x + 12.0f, lo.y + 41.0f);
    const glm::vec2 phi(hi.x - 12.0f, hi.y - 10.0f);
    solid(plo, phi, {1.0f, 1.0f, 1.0f, hov_card ? 0.12f : 0.04f});
    const float glow = hov_card ? std::max(SelectionFlash(hover_frame_), 0.92f) : (0.34f + 0.14f * breath);
    const float pw = phi.x - plo.x;
    const float gx0 = plo.x + 2.0f, gx1 = plo.x + pw * 0.24f, gx2 = phi.x - pw * 0.24f, gx3 = phi.x - 2.0f;
    hgrad(gx0, gx1, plo.y + 2.0f, phi.y - 2.0f, {1.0f, 1.0f, 1.0f, 0.05f * glow}, {1.0f, 1.0f, 1.0f, 0.26f * glow});
    hgrad(gx1, gx2, plo.y + 2.0f, phi.y - 2.0f, {1.0f, 1.0f, 1.0f, 0.26f * glow}, {1.0f, 1.0f, 1.0f, 0.26f * glow});
    hgrad(gx2, gx3, plo.y + 2.0f, phi.y - 2.0f, {1.0f, 1.0f, 1.0f, 0.26f * glow}, {1.0f, 1.0f, 1.0f, 0.05f * glow});
    frame(plo, phi, hov_card ? 1.5f : 1.0f, {1.0f, 1.0f, 1.0f, hov_card ? 0.88f : 0.28f});

    const std::string action = "{survival}  " + std::string(PcText("pc_survival_promo_yes", text_language_));
    DrawPromptText(batch, canvas, *assets_, *font, action, prompt_style_, 16.5f, plo + glm::vec2(10.0f, 0.0f), phi - glm::vec2(10.0f, 0.0f),
                   ui::TextAlign::Center, ui::TextAlign::Center, {1.0f, 1.0f, 1.0f, (hov_card ? 1.0f : 0.92f) * alpha}, true);
}

void OptionsMenu::DrawPcDropdown(ui::UiBatch& batch, const UiCanvas& canvas, const PcDropdownView& dd) {
    UiFont* font = assets_->Font(UiFontType::PcSystem, text_language_);
    if (!font) return;

    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    auto solid = [&](glm::vec2 a, glm::vec2 b, glm::vec4 c) {
        if (c.a <= 0.001f) return;
        batch.Quad(canvas.ToTarget(a), canvas.ToTarget(b), {0.0f, 0.0f}, {1.0f, 1.0f}, c, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    auto hgrad = [&](float x0, float x1, float y0, float y1, glm::vec4 cl, glm::vec4 cr) {
        const std::vector<ui::UiVertex> v = {
            {canvas.ToTarget({x0, y0}), {0, 0}, cl}, {canvas.ToTarget({x1, y0}), {0, 0}, cr}, {canvas.ToTarget({x1, y1}), {0, 0}, cr},
            {canvas.ToTarget({x0, y0}), {0, 0}, cl}, {canvas.ToTarget({x1, y1}), {0, 0}, cr}, {canvas.ToTarget({x0, y1}), {0, 0}, cl},
        };
        batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    auto frame = [&](glm::vec2 a, glm::vec2 b, float w, glm::vec4 c) {
        solid(a, {b.x, a.y + w}, c);
        solid({a.x, b.y - w}, b, c);
        solid({a.x, a.y + w}, {a.x + w, b.y - w}, c);
        solid({b.x - w, a.y + w}, {b.x, b.y - w}, c);
    };

    solid(dd.lo - glm::vec2(6.0f), dd.hi + glm::vec2(6.0f), {0.0f, 0.0f, 0.0f, 0.55f});
    solid(dd.lo, dd.hi, {0.02f, 0.024f, 0.03f, 0.96f});
    frame(dd.lo, dd.hi, 1.5f, {1.0f, 1.0f, 1.0f, 0.36f});

    const int n = static_cast<int>(dd.items.size());
    const bool scrollable = n > dd.visible;
    const float list_top = dd.lo.y + 8.0f;
    const float right_pad = scrollable ? 16.0f : 8.0f;
    const float glow_a = std::max(SelectionFlash(dd.flash), 0.85f);

    for (int slot = 0; slot < dd.visible && dd.scroll + slot < n; ++slot) {
        const int idx = dd.scroll + slot;
        const float y0 = list_top + static_cast<float>(slot) * dd.row_height;
        const float y1 = y0 + dd.row_height;
        const bool sel = idx == dd.cursor;
        const bool act = idx == dd.active;

        if (sel) {
            const float x0 = dd.lo.x + 6.0f;
            const float x3 = dd.hi.x - right_pad;
            const float w = x3 - x0;
            const float x1 = x0 + w * 0.22f;
            const float x2 = x3 - w * 0.22f;
            hgrad(x0, x1, y0 + 2.0f, y1 - 2.0f, {1.0f, 1.0f, 1.0f, 0.05f * glow_a}, {1.0f, 1.0f, 1.0f, 0.28f * glow_a});
            hgrad(x1, x2, y0 + 2.0f, y1 - 2.0f, {1.0f, 1.0f, 1.0f, 0.28f * glow_a}, {1.0f, 1.0f, 1.0f, 0.28f * glow_a});
            hgrad(x2, x3, y0 + 2.0f, y1 - 2.0f, {1.0f, 1.0f, 1.0f, 0.28f * glow_a}, {1.0f, 1.0f, 1.0f, 0.05f * glow_a});
        }
        if (act) {
            const float cy = 0.5f * (y0 + y1);
            solid({dd.lo.x + 16.0f, cy - 2.5f}, {dd.lo.x + 21.0f, cy + 2.5f}, {1.0f, 1.0f, 1.0f, sel ? 1.0f : 0.78f});
        }
        ui::TextStyle st;
        st.font = &font->font;
        st.font_width = 16.5f;
        st.font_height = 16.5f;
        ui::TextLayout layout = ui::LayoutText(dd.items[static_cast<size_t>(idx)], st, 0.0f);
        ui::PlaceText(layout, {dd.lo.x + 26.0f, y0}, {dd.hi.x - right_pad - 6.0f, y1}, ui::TextAlign::Center, ui::TextAlign::Center,
                      ui::TextAlign::Center, false);
        DrawText(batch, canvas, *font, layout, {1.0f, 1.0f, 1.0f, sel ? 1.0f : act ? 0.92f : 0.65f}, ui::UiBlend::Alpha);
    }

    if (scrollable) {
        const float tx0 = dd.hi.x - 10.0f;
        const float tx1 = dd.hi.x - 6.0f;
        const float ty0 = list_top + 2.0f;
        const float ty1 = list_top + static_cast<float>(dd.visible) * dd.row_height - 2.0f;
        solid({tx0, ty0}, {tx1, ty1}, {1.0f, 1.0f, 1.0f, 0.12f});
        const float frac = static_cast<float>(dd.visible) / static_cast<float>(n);
        const float pos = static_cast<float>(dd.scroll) / static_cast<float>(std::max(1, n - dd.visible));
        const float th = std::max(20.0f, (ty1 - ty0) * frac);
        const float sy = ty0 + ((ty1 - ty0) - th) * pos;
        solid({tx0, sy}, {tx1, sy + th}, {1.0f, 1.0f, 1.0f, 0.68f});
    }

    const float foot_y0 = dd.hi.y - 30.0f;
    solid({dd.lo.x + 10.0f, foot_y0}, {dd.hi.x - 10.0f, foot_y0 + 1.0f}, {1.0f, 1.0f, 1.0f, 0.16f});
    const std::string foot = std::format("{{accept}} {}   {{back}} {}", PcText("pc_ok", text_language_), PcText("pc_back", text_language_));
    DrawPromptText(batch, canvas, *assets_, *font, foot, prompt_style_, 13.5f, {dd.lo.x + 10.0f, foot_y0 + 2.0f}, {dd.hi.x - 10.0f, dd.hi.y - 2.0f},
                   ui::TextAlign::Center, ui::TextAlign::Center, {1.0f, 1.0f, 1.0f, 0.72f}, true);
}

void OptionsMenu::Draw(ui::UiBatch& batch, const UiCanvas& canvas) {
    if (state_ == State::Closed || !model_) {
        return;
    }
    const bool settled = page_ == Page::Pc && state_ == State::Active && switch_frame_ < 0.0f;
    if (settled && pc_.ScreenShown()) {
        DrawScreen(batch, canvas);
        return;
    }
    if (settled && pc_.FullScreen()) {
        archive_view_.DrawFullScreen(batch, canvas, *assets_, pc_.Panel(), PcText(pc_.FullScreenHint(), text_language_), prompt_style_, text_language_);
        return;
    }
    if (page_ == Page::Pc && pc_source_ && pc_source_->IsArmory() && switch_frame_ < 0.0f) {
        armory_view_.Draw(batch, canvas, *assets_, *pc_source_, prompt_style_, text_language_);
        return;
    }
    if (page_ == Page::Pc && pc_source_ && pc_source_->IsNightHub() && switch_frame_ < 0.0f) {
        night_hub_view_.Draw(batch, canvas, *assets_, *pc_source_, prompt_style_, text_language_);
        return;
    }
    if (pillar_alpha_ > 0.0f && !see_through_) {
        const float left = canvas.origin.x;
        const float right = canvas.origin.x + UiCanvas::kWidth * canvas.scale;
        const float width = static_cast<float>(batch.Extent().width);
        const float height = static_cast<float>(batch.Extent().height);
        const glm::vec4 black(0.0f, 0.0f, 0.0f, pillar_alpha_);
        const auto plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
        if (left > 0.5f) batch.Quad(glm::vec2(0.0f), glm::vec2(left, height), glm::vec2(0.0f), glm::vec2(1.0f), black, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
        if (width - right > 0.5f) batch.Quad(glm::vec2(right, 0.0f), glm::vec2(width, height), glm::vec2(0.0f), glm::vec2(1.0f), black, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    }
    view_.Draw(batch, canvas, text_language_, 1.0f);
    if (page_ == Page::Original && survival_promo_shown_ && !quit_confirming_ && !street_confirming_ && !survival_locked_confirming_ && switch_frame_ < 0.0f) {
        DrawSurvivalToast(batch, canvas);
    }
    if (settled) pc_.LogLayout(view_);
    if (page_ == Page::Pc && pc_.Gallery() != PcGallery::None && settled) {
        pc_.SetWallWidth(static_cast<float>(batch.Extent().width) / std::max(canvas.scale, 0.001f));
        assets_->PreloadPreviews(pc_.PreviewFiles());
        archive_view_.DrawMuseum(batch, canvas, *assets_, pc_, text_language_);
    }
    if (page_ == Page::Pc && pc_.Browser()) {
        if (pc_.IsLoopBrowser()) assets_->PreloadPreviews(pc_.PreviewFiles());
        const PcPanel panel = pc_.Panel();
        const bool text = !panel.lines.empty();
        if (panel.crosshair) {
            DrawCrosshairPreview(batch, canvas, *panel.crosshair);
        } else {
            archive_view_.DrawPanel(batch, canvas, *assets_, panel, text ? glm::vec2(650.0f, 160.0f) : glm::vec2(650.0f, 170.0f),
                                    text ? glm::vec2(1190.0f, 560.0f) : glm::vec2(1190.0f, 474.0f), text_language_);
        }
    }
    if (page_ == Page::Pc && (state_ == State::Active || state_ == State::SetIn) && switch_frame_ < 0.0f) {
        PcDropdownView dd;
        if (pc_.DropdownView(dd)) {
            DrawPcDropdown(batch, canvas, dd);
        }
    }
    if ((state_ == State::Active || state_ == State::SetIn) && switch_frame_ < 0.0f) {
        dialog::View view;
        bool shown = false;
        if (page_ == Page::Pc) {
            shown = pc_.DialogView(view, text_language_);
            view.flash = SelectionFlash(pc_.FlashFrame());
        } else if (survival_locked_confirming_) {
            view.text = std::string(PcText("pc_survival_locked_ng4", text_language_));
            view.choices = {std::string(PcText("pc_ok", text_language_))};
            view.focus = 0;
            view.language = text_language_;
            view.flash = SelectionFlash(ask_flash_);
            shown = true;
        } else if (quit_confirming_ || street_confirming_) {
            view.text = std::string(PcText(quit_confirming_ ? "pc_quit_confirm" : leave_date_ ? "pc_lisa_date_leave_confirm" : "pc_street_return_confirm",
                                           text_language_));
            view.choices = {std::string(PcText("pc_save_move_yes", text_language_)), std::string(PcText("pc_save_move_no", text_language_))};
            view.focus = ask_nav_.Focus();
            view.language = text_language_;
            view.flash = SelectionFlash(ask_flash_);
            shown = true;
        }
        if (state_ == State::SetIn) {
            const Animation* in = Find("UI_sys_opt_setin");
            const float frame = in ? players_.PlayingFrame(in->main) : -1.0f;
            view.alpha = frame >= 0.0f ? TrackAlpha("UI_sys_opt_setin", frame) : 1.0f;
        }
        if (shown) dialog::Draw(batch, canvas, *assets_, model_, prompt_style_, view);
    }
    std::string_view credit = page_ == Page::Pc && state_ == State::Active && switch_frame_ < 0.0f ? pc_.Credit() : std::string_view();
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
