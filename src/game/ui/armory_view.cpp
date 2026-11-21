#include "game/ui/armory_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

#include "engine/audio/audio_engine.h"
#include "engine/render/texture_manager.h"
#include "engine/ui/text_layout.h"
#include "game/game.h"
#include "game/survival/survival_profile.h"
#include "game/survival/survival_skins.h"
#include "game/ui/options_menu.h"
#include "game/ui/prompt_hint.h"
#include "game/ui/ui_assets.h"

namespace pt::game {

namespace {

constexpr const char* kCursorSound = "Play_sys_cursor_01";
constexpr const char* kSelectSound = "Play_sys_change_01";
constexpr const char* kCancelSound = "Play_sys_cursor_01";

constexpr float kTopY0 = 24.0f;
constexpr float kTopY1 = 68.0f;
constexpr float kPanelX0 = 42.0f;
constexpr float kPanelX1 = 496.0f;
constexpr float kPanelY0 = 86.0f;
constexpr float kPanelY1 = 644.0f;
constexpr float kStageX0 = 516.0f;
constexpr float kStageX1 = 1238.0f;

constexpr int kVisibleSkinCards = 6;
constexpr float kSkinListY0 = 204.0f;
constexpr float kSkinCardH = 58.0f;
constexpr float kSkinCardGap = 4.0f;

constexpr float kAnimListY0 = 148.0f;
constexpr float kAnimCardH = 78.0f;
constexpr float kAnimCardGap = 6.0f;

constexpr float kTorchListY0 = 108.0f;
constexpr float kTorchCardH = 80.0f;
constexpr float kTorchCardGap = 8.0f;

constexpr float kActionY0 = 594.0f;
constexpr float kActionY1 = 634.0f;

glm::vec2 PointerToCanvas(glm::vec2 uif) {
    return {(uif.x + 64.0f) * 10.0f, (36.0f - uif.y) * 10.0f};
}

bool InRect(glm::vec2 p, float x0, float y0, float x1, float y1) {
    return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
}

void DrawSolid(ui::UiBatch& batch, const UiCanvas& canvas, float x0, float y0, float x1, float y1, glm::vec4 color) {
    if (color.a <= 0.001f) return;
    batch.Quad(canvas.ToTarget({x0, y0}), canvas.ToTarget({x1, y1}), {0.0f, 0.0f}, {1.0f, 1.0f}, color,
               ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
}

void DrawHGrad(ui::UiBatch& batch, const UiCanvas& canvas, float x0, float x1, float y0, float y1, glm::vec4 cl, glm::vec4 cr) {
    if (cl.a <= 0.001f && cr.a <= 0.001f) return;
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    const std::vector<ui::UiVertex> v = {
        {canvas.ToTarget({x0, y0}), {0, 0}, cl}, {canvas.ToTarget({x1, y0}), {0, 0}, cr}, {canvas.ToTarget({x1, y1}), {0, 0}, cr},
        {canvas.ToTarget({x0, y0}), {0, 0}, cl}, {canvas.ToTarget({x1, y1}), {0, 0}, cr}, {canvas.ToTarget({x0, y1}), {0, 0}, cl},
    };
    batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
}

void DrawOutline(ui::UiBatch& batch, const UiCanvas& canvas, float x0, float y0, float x1, float y1, float t, glm::vec4 color) {
    DrawSolid(batch, canvas, x0, y0, x1, y0 + t, color);
    DrawSolid(batch, canvas, x0, y1 - t, x1, y1, color);
    DrawSolid(batch, canvas, x0, y0 + t, x0 + t, y1 - t, color);
    DrawSolid(batch, canvas, x1 - t, y0 + t, x1, y1 - t, color);
}

void DrawPtGlowBar(ui::UiBatch& batch, const UiCanvas& canvas, float x0, float y0, float x1, float y1, float peak_alpha) {
    if (peak_alpha <= 0.001f) return;
    const float w = x1 - x0;
    const float a = x0 + w * 0.22f;
    const float b = x1 - w * 0.22f;
    DrawHGrad(batch, canvas, x0, a, y0, y1, {1.0f, 1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, peak_alpha});
    DrawHGrad(batch, canvas, a, b, y0, y1, {1.0f, 1.0f, 1.0f, peak_alpha}, {1.0f, 1.0f, 1.0f, peak_alpha});
    DrawHGrad(batch, canvas, b, x1, y0, y1, {1.0f, 1.0f, 1.0f, peak_alpha}, {1.0f, 1.0f, 1.0f, 0.0f});
}

void DrawPtHairline(ui::UiBatch& batch, const UiCanvas& canvas, float x0, float x1, float y, float alpha) {
    DrawHGrad(batch, canvas, x0, x1, y, y + 1.0f, {1.0f, 1.0f, 1.0f, alpha}, {1.0f, 1.0f, 1.0f, 0.0f});
}

std::array<glm::vec4, 6> WeaponTabRects() {
    std::array<glm::vec4, 6> out{};
    float x = 276.0f;
    constexpr float w = 124.0f;
    constexpr float gap = 6.0f;
    for (int i = 0; i < 6; ++i) {
        out[static_cast<size_t>(i)] = {x, kTopY0 + 6.0f, x + w, kTopY1 - 6.0f};
        x += w + gap;
    }
    return out;
}

std::array<glm::vec4, 7> FilterPillRects() {
    std::array<glm::vec4, 7> out{};
    const float total_w = (kPanelX1 - kPanelX0) - 8.0f;
    const float w1 = (total_w - 3.0f * 4.0f) / 4.0f;
    float x = kPanelX0 + 4.0f;
    for (int i = 0; i < 4; ++i) {
        out[static_cast<size_t>(i)] = {x, 138.0f, x + w1, 162.0f};
        x += w1 + 4.0f;
    }
    const float w2 = (total_w - 2.0f * 4.0f) / 3.0f;
    x = kPanelX0 + 4.0f;
    for (int i = 0; i < 3; ++i) {
        out[static_cast<size_t>(4 + i)] = {x, 168.0f, x + w2, 192.0f};
        x += w2 + 4.0f;
    }
    return out;
}

std::array<glm::vec4, 3> ActionRects(bool flashlight) {
    std::array<glm::vec4, 3> out{};
    const float x0 = kPanelX0;
    const float x1 = kPanelX1;
    if (flashlight) {
        out[0] = {x0, kActionY0, x1, kActionY1};
        out[1] = out[2] = {0.0f, 0.0f, 0.0f, 0.0f};
        return out;
    }
    const float w0 = 152.0f;
    const float w1 = 148.0f;
    const float gap = 8.0f;
    out[0] = {x0, kActionY0, x0 + w0, kActionY1};
    out[1] = {x0 + w0 + gap, kActionY0, x0 + w0 + gap + w1, kActionY1};
    out[2] = {x0 + w0 + gap + w1 + gap, kActionY0, x1, kActionY1};
    return out;
}

const char* WeaponTabKey(int idx) {
    static constexpr const char* kKeys[6] = {"pc_survival_handgun", "pc_survival_shotgun", "pc_survival_magnum",
                                             "pc_survival_lever",   "pc_survival_double",  "pc_armory_torch"};
    return kKeys[std::clamp(idx, 0, 5)];
}

const char* FilterPillKey(int idx) {
    static constexpr const char* kKeys[7] = {"pc_armory_filter_all", "pc_skin_rarity_factory", "pc_skin_rarity_tactical", "pc_skin_rarity_artisan",
                                             "pc_skin_rarity_horror", "pc_skin_rarity_mythic", "pc_armory_filter_owned"};
    return kKeys[std::clamp(idx, 0, 6)];
}

const char* AnimNameKey(int idx) {
    static constexpr const char* kKeys[5] = {"pc_armory_anim_default", "pc_reload_standard", "pc_reload_tactical", "pc_reload_gunslinger",
                                             "pc_reload_telekinetic"};
    return kKeys[std::clamp(idx, 0, 4)];
}

const char* AnimDescKey(int idx) {
    static constexpr const char* kKeys[5] = {"pc_note_armory_anim_default", "pc_note_armory_anim_standard", "pc_note_armory_anim_tactical",
                                             "pc_note_armory_anim_gunslinger", "pc_note_armory_anim_telekinetic"};
    return kKeys[std::clamp(idx, 0, 4)];
}

}

void ArmoryView::Reset() {
    scroll_ = 0;
    dragging_ = false;
    repeat_timer_ = 0.0f;
    repeat_dir_ = 0;
    pulse_ = 0.0f;
    hovered_weapon_ = hovered_mode_ = hovered_filter_ = hovered_card_ = hovered_action_ = -1;
}

ArmoryView::Result ArmoryView::Update(Game& game, PcSettingsSource& source, const MenuInput& input, float dt) {
    pulse_ += dt;
    auto sound = [&](const char* ev) {
        if (game.Audio()) game.Audio()->PostEvent(ev, nullptr);
    };

    if (input.back || input.close) {
        sound(kCancelSound);
        dragging_ = false;
        return Result::Back;
    }

    ArmorySnapshot data = source.ArmoryData();
    const bool is_torch = data.weapon == 6;

    if (std::abs(input.right_stick.x) > 0.12f || std::abs(input.right_stick.y) > 0.12f) {
        source.ArmoryOrbit(input.right_stick.x * dt * 2.6f, -input.right_stick.y * dt * 1.6f);
    }

    if (!is_torch && input.armory_tab) {
        sound(kCursorSound);
        source.ArmorySelectMode(data.mode == 0 ? 1 : 0);
        scroll_ = 0;
        return Result::Stay;
    }

    if (input.tab_prev) {
        sound(kCursorSound);
        const int next = data.weapon <= 1 ? 6 : data.weapon - 1;
        source.ArmorySelectWeapon(next);
        scroll_ = 0;
        return Result::Stay;
    }
    if (input.tab_next) {
        sound(kCursorSound);
        const int next = data.weapon >= 6 ? 1 : data.weapon + 1;
        source.ArmorySelectWeapon(next);
        scroll_ = 0;
        return Result::Stay;
    }

    if (!is_torch && input.armory_replay) {
        sound(kSelectSound);
        source.ArmoryPreviewReload();
        return Result::Stay;
    }
    if (!is_torch && input.armory_apply_all) {
        sound(kSelectSound);
        source.ArmoryAction(true);
        return Result::Stay;
    }

    bool up = input.subtitle_up;
    bool down = input.subtitle_down;
    bool left = input.brightness_down;
    bool right = input.brightness_up;
    const uint32_t dirs = input.held_dirs & (kRawUp | kRawDown | kRawLeft | kRawRight);
    if (dirs != 0 && dirs == repeat_dir_) {
        repeat_timer_ -= dt;
        if (repeat_timer_ <= 0.0f) {
            repeat_timer_ = 0.11f;
            if (dirs & kRawUp) up = true;
            if (dirs & kRawDown) down = true;
            if (dirs & kRawLeft) left = true;
            if (dirs & kRawRight) right = true;
        }
    } else {
        repeat_dir_ = dirs;
        repeat_timer_ = 0.32f;
    }

    if (!is_torch && data.mode == 0 && (left || right)) {
        sound(kCursorSound);
        const int next = (data.filter + (right ? 1 : 6)) % 7;
        source.ArmorySelectFilter(next);
        scroll_ = 0;
        return Result::Stay;
    }

    auto step_list = [&](int delta) {
        if (is_torch) {
            const int next = (data.selected_torch + delta + kSurvivalTorchCount) % kSurvivalTorchCount;
            if (next != data.selected_torch) {
                sound(kCursorSound);
                source.ArmoryFocusItem(next);
            }
            return;
        }
        if (data.mode == 1) {
            const int next = (data.selected_anim + delta + 5) % 5;
            if (next != data.selected_anim) {
                sound(kCursorSound);
                source.ArmoryFocusItem(next);
            }
            return;
        }
        const int count = static_cast<int>(data.filtered_skins.size());
        if (count <= 0) return;
        int idx = 0;
        for (int i = 0; i < count; ++i) {
            if (data.filtered_skins[static_cast<size_t>(i)] == data.selected_skin) {
                idx = i;
                break;
            }
        }
        const int next_idx = (idx + delta + count) % count;
        sound(kCursorSound);
        source.ArmoryFocusItem(data.filtered_skins[static_cast<size_t>(next_idx)]);
        if (next_idx < scroll_) scroll_ = next_idx;
        if (next_idx >= scroll_ + kVisibleSkinCards) scroll_ = next_idx - kVisibleSkinCards + 1;
    };

    if (up) step_list(-1);
    if (down) step_list(1);

    if (!is_torch && data.mode == 0 && std::abs(input.wheel) > 0.01f) {
        const int count = static_cast<int>(data.filtered_skins.size());
        const int max_scroll = std::max(0, count - kVisibleSkinCards);
        scroll_ = std::clamp(scroll_ + (input.wheel > 0.0f ? -1 : 1), 0, max_scroll);
    }

    if (input.accept) {
        sound(kSelectSound);
        source.ArmoryAction(false);
        return Result::Stay;
    }

    hovered_weapon_ = hovered_mode_ = hovered_filter_ = hovered_card_ = hovered_action_ = -1;
    if (input.pointer_valid) {
        const glm::vec2 p = PointerToCanvas(input.pointer);
        if (dragging_) {
            if (input.pointer_held) {
                const glm::vec2 delta = p - drag_last_;
                drag_last_ = p;
                source.ArmoryOrbit(delta.x * 0.0085f, delta.y * 0.0055f);
            } else {
                dragging_ = false;
            }
        } else if (input.click && InRect(p, kStageX0, kPanelY0, kStageX1, kPanelY1)) {
            dragging_ = true;
            drag_last_ = p;
        }

        const auto wrects = WeaponTabRects();
        for (int i = 0; i < 6; ++i) {
            const glm::vec4 r = wrects[static_cast<size_t>(i)];
            if (InRect(p, r.x, r.y, r.z, r.w)) {
                hovered_weapon_ = i;
                if (input.click) {
                    sound(kSelectSound);
                    source.ArmorySelectWeapon(i + 1);
                    scroll_ = 0;
                    return Result::Stay;
                }
            }
        }

        if (!is_torch) {
            const float mid_x = 0.5f * (kPanelX0 + kPanelX1);
            if (InRect(p, kPanelX0, 92.0f, mid_x - 4.0f, 128.0f)) {
                hovered_mode_ = 0;
                if (input.click && data.mode != 0) {
                    sound(kSelectSound);
                    source.ArmorySelectMode(0);
                    scroll_ = 0;
                    return Result::Stay;
                }
            } else if (InRect(p, mid_x + 4.0f, 92.0f, kPanelX1, 128.0f)) {
                hovered_mode_ = 1;
                if (input.click && data.mode != 1) {
                    sound(kSelectSound);
                    source.ArmorySelectMode(1);
                    scroll_ = 0;
                    return Result::Stay;
                }
            }
        }

        if (!is_torch && data.mode == 0) {
            const auto prects = FilterPillRects();
            for (int i = 0; i < 7; ++i) {
                const glm::vec4 r = prects[static_cast<size_t>(i)];
                if (InRect(p, r.x, r.y, r.z, r.w)) {
                    hovered_filter_ = i;
                    if (input.click && data.filter != i) {
                        sound(kCursorSound);
                        source.ArmorySelectFilter(i);
                        scroll_ = 0;
                        return Result::Stay;
                    }
                }
            }
        }

        if (is_torch) {
            for (int i = 0; i < kSurvivalTorchCount; ++i) {
                const float y0 = kTorchListY0 + static_cast<float>(i) * (kTorchCardH + kTorchCardGap);
                if (InRect(p, kPanelX0, y0, kPanelX1, y0 + kTorchCardH)) {
                    hovered_card_ = i;
                    if (data.selected_torch != i) {
                        sound(kCursorSound);
                        source.ArmoryFocusItem(i);
                    }
                    if (input.click) {
                        sound(kSelectSound);
                        source.ArmoryAction(false);
                        return Result::Stay;
                    }
                }
            }
        } else if (data.mode == 1) {
            for (int i = 0; i < 5; ++i) {
                const float y0 = kAnimListY0 + static_cast<float>(i) * (kAnimCardH + kAnimCardGap);
                if (InRect(p, kPanelX0, y0, kPanelX1, y0 + kAnimCardH)) {
                    hovered_card_ = i;
                    if (data.selected_anim != i) {
                        sound(kCursorSound);
                        source.ArmoryFocusItem(i);
                    }
                    if (input.click) {
                        sound(kSelectSound);
                        source.ArmoryAction(false);
                        return Result::Stay;
                    }
                }
            }
        } else {
            const int count = static_cast<int>(data.filtered_skins.size());
            const int max_scroll = std::max(0, count - kVisibleSkinCards);
            scroll_ = std::clamp(scroll_, 0, max_scroll);
            for (int slot = 0; slot < kVisibleSkinCards && scroll_ + slot < count; ++slot) {
                const float y0 = kSkinListY0 + static_cast<float>(slot) * (kSkinCardH + kSkinCardGap);
                if (InRect(p, kPanelX0, y0, kPanelX1 - 14.0f, y0 + kSkinCardH)) {
                    const int skin_id = data.filtered_skins[static_cast<size_t>(scroll_ + slot)];
                    hovered_card_ = scroll_ + slot;
                    if (data.selected_skin != skin_id) {
                        sound(kCursorSound);
                        source.ArmoryFocusItem(skin_id);
                    }
                    if (input.click) {
                        sound(kSelectSound);
                        source.ArmoryAction(false);
                        return Result::Stay;
                    }
                }
            }
        }

        const auto arects = ActionRects(is_torch);
        for (int i = 0; i < (is_torch ? 1 : 3); ++i) {
            const glm::vec4 r = arects[static_cast<size_t>(i)];
            if (InRect(p, r.x, r.y, r.z, r.w)) {
                hovered_action_ = i;
                if (input.click) {
                    sound(kSelectSound);
                    if (i == 0) source.ArmoryAction(false);
                    else if (i == 1) source.ArmoryAction(true);
                    else source.ArmoryPreviewReload();
                    return Result::Stay;
                }
            }
        }
    } else {
        dragging_ = false;
    }

    return Result::Stay;
}

void ArmoryView::Draw(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcSettingsSource& source, const PromptStyle& prompts,
                      int language) {
    UiFont* font = assets.Font(UiFontType::PcSystem, language);
    if (!font) return;

    const ArmorySnapshot data = source.ArmoryData();
    const bool is_torch = data.weapon == 6;
    const bool rtl = language == dialog::kArabic;

    auto text_box = [&](std::string_view words, float size, float x0, float y0, float x1, float y1, ui::TextAlign align, glm::vec4 color) {
        ui::TextStyle st;
        st.font = &font->font;
        st.font_width = size;
        st.font_height = size;
        st.line_space = size * 0.22f;
        ui::TextLayout layout = ui::LayoutText(words, st, 0.0f);
        if (layout.width > (x1 - x0) && layout.width > 1.0f) {
            const float shrink = (x1 - x0) / layout.width;
            st.font_width = std::max(8.0f, size * shrink);
            st.font_height = std::max(8.0f, size * shrink);
            layout = ui::LayoutText(words, st, 0.0f);
        }
        ui::PlaceText(layout, {x0, y0}, {x1, y1}, align, align, ui::TextAlign::Center, rtl);
        DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha);
    };

    DrawSolid(batch, canvas, 0.0f, 0.0f, kPanelX1 + 12.0f, UiCanvas::kHeight, {0.01f, 0.012f, 0.016f, 0.78f});
    DrawHGrad(batch, canvas, kPanelX1 + 12.0f, kPanelX1 + 110.0f, 0.0f, UiCanvas::kHeight, {0.01f, 0.012f, 0.016f, 0.78f}, {0.01f, 0.012f, 0.016f, 0.0f});
    DrawSolid(batch, canvas, 0.0f, 0.0f, UiCanvas::kWidth, 74.0f, {0.01f, 0.012f, 0.016f, 0.72f});
    DrawPtHairline(batch, canvas, 42.0f, 1238.0f, 74.0f, 0.24f);
    DrawSolid(batch, canvas, 0.0f, 654.0f, UiCanvas::kWidth, UiCanvas::kHeight, {0.01f, 0.012f, 0.016f, 0.76f});
    DrawPtHairline(batch, canvas, 42.0f, 1238.0f, 654.0f, 0.20f);

    text_box(PcText("pc_armory_title", language), 22.0f, 42.0f, kTopY0, 264.0f, kTopY1, ui::TextAlign::Start, {0.96f, 0.96f, 0.98f, 0.96f});

    const auto wrects = WeaponTabRects();
    for (int i = 0; i < 6; ++i) {
        const glm::vec4 r = wrects[static_cast<size_t>(i)];
        const bool active = data.weapon == (i + 1);
        const bool hov = hovered_weapon_ == i;
        if (active) {
            DrawPtGlowBar(batch, canvas, r.x - 4.0f, r.y + 1.0f, r.z + 4.0f, r.w - 1.0f, 0.24f);
        } else if (hov) {
            DrawPtGlowBar(batch, canvas, r.x - 4.0f, r.y + 1.0f, r.z + 4.0f, r.w - 1.0f, 0.11f);
        }
        text_box(PcText(WeaponTabKey(i), language), 15.0f, r.x + 4.0f, r.y, r.z - 4.0f, r.w, ui::TextAlign::Center,
                 active ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) : glm::vec4(0.76f, 0.78f, 0.80f, hov ? 0.92f : 0.56f));
    }

    const std::string cred_lbl = std::format("{}", PcText("pc_armory_credits", language));
    const std::string cred_val = std::format("${}", data.credits);
    text_box(cred_lbl, 13.5f, 1068.0f, kTopY0 + 8.0f, 1166.0f, kTopY1 - 6.0f, ui::TextAlign::End, {0.72f, 0.74f, 0.78f, 0.68f});
    text_box(cred_val, 18.0f, 1170.0f, kTopY0 + 6.0f, 1238.0f, kTopY1 - 6.0f, ui::TextAlign::End, {0.98f, 0.98f, 1.0f, 0.96f});

    if (!is_torch) {
        const float mid_x = 0.5f * (kPanelX0 + kPanelX1);
        for (int m = 0; m < 2; ++m) {
            const float x0 = m == 0 ? kPanelX0 : mid_x + 4.0f;
            const float x1 = m == 0 ? mid_x - 4.0f : kPanelX1;
            const bool active = data.mode == m;
            const bool hov = hovered_mode_ == m;
            if (active) {
                DrawPtGlowBar(batch, canvas, x0, 94.0f, x1, 126.0f, 0.22f);
            } else if (hov) {
                DrawPtGlowBar(batch, canvas, x0, 94.0f, x1, 126.0f, 0.10f);
            }
            const char* key = m == 0 ? "pc_armory_tab_skins" : "pc_armory_tab_animations";
            text_box(PcText(key, language), 15.5f, x0 + 6.0f, 94.0f, x1 - 6.0f, 126.0f, ui::TextAlign::Center,
                     active ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) : glm::vec4(0.76f, 0.78f, 0.80f, hov ? 0.90f : 0.54f));
        }
        DrawPtHairline(batch, canvas, kPanelX0, kPanelX1, 130.0f, 0.18f);
    }

    bool primary_can = false;
    std::string primary_label;
    std::string stage_caption;

    if (is_torch) {
        const SurvivalTorchDef& cur_t = SurvivalTorch(data.selected_torch);
        const bool cur_owned = (data.owned_torches & (1u << static_cast<unsigned>(cur_t.id))) != 0u || cur_t.id == 0;
        const bool cur_eq = data.equipped_torch == cur_t.id;
        primary_can = cur_eq ? false : (cur_owned || data.credits >= cur_t.price);
        primary_label = cur_eq ? std::string(PcText("pc_armory_equipped", language))
                        : cur_owned ? std::string(PcText("pc_armory_equip", language))
                                    : std::format("{} (${})", PcText("pc_armory_buy", language), cur_t.price);
        stage_caption = std::format("{}  /  {}", PcText("pc_armory_torch", language), PcText(cur_t.name_key, language));

        for (int i = 0; i < kSurvivalTorchCount; ++i) {
            const SurvivalTorchDef& td = SurvivalTorch(i);
            const float y0 = kTorchListY0 + static_cast<float>(i) * (kTorchCardH + kTorchCardGap);
            const float y1 = y0 + kTorchCardH;
            const bool sel = data.selected_torch == i;
            const bool eq = data.equipped_torch == i;
            const bool own = (data.owned_torches & (1u << static_cast<unsigned>(i))) != 0u || i == 0;
            const bool hov = hovered_card_ == i;

            if (sel) {
                DrawPtGlowBar(batch, canvas, kPanelX0 - 4.0f, y0 + 2.0f, kPanelX1 + 4.0f, y1 - 2.0f, 0.24f);
            } else if (hov) {
                DrawPtGlowBar(batch, canvas, kPanelX0 - 4.0f, y0 + 2.0f, kPanelX1 + 4.0f, y1 - 2.0f, 0.11f);
            }
            DrawPtHairline(batch, canvas, kPanelX0, kPanelX1, y1 + 3.0f, 0.10f);

            const float max_c = std::max({td.tint.r, td.tint.g, td.tint.b, 1.0f});
            const glm::vec3 sw = td.tint / max_c;
            DrawSolid(batch, canvas, kPanelX0 + 12.0f, y0 + 22.0f, kPanelX0 + 48.0f, y1 - 22.0f, {sw.r, sw.g, sw.b, 0.95f});
            DrawOutline(batch, canvas, kPanelX0 + 12.0f, y0 + 22.0f, kPanelX0 + 48.0f, y1 - 22.0f, 1.0f, {1.0f, 1.0f, 1.0f, sel ? 0.65f : 0.28f});

            text_box(PcText(td.name_key, language), 17.5f, kPanelX0 + 62.0f, y0, kPanelX1 - 122.0f, y1, ui::TextAlign::Start,
                     {1.0f, 1.0f, 1.0f, sel ? 1.0f : 0.78f});
            const std::string badge = eq ? std::string(PcText("pc_armory_equipped", language))
                                      : own ? std::string(PcText("pc_armory_status_owned", language))
                                            : std::format("${}", td.price);
            const glm::vec4 bcol = eq ? glm::vec4(1.0f, 1.0f, 1.0f, 0.98f)
                                   : own ? glm::vec4(0.80f, 0.82f, 0.85f, 0.74f)
                                         : (data.credits >= td.price ? glm::vec4(0.95f, 0.95f, 0.97f, 0.90f) : glm::vec4(0.76f, 0.44f, 0.44f, 0.78f));
            if (eq) {
                const float cy = 0.5f * (y0 + y1);
                DrawSolid(batch, canvas, kPanelX1 - 10.0f, cy - 2.5f, kPanelX1 - 5.0f, cy + 2.5f, {1.0f, 1.0f, 1.0f, 0.92f});
            }
            text_box(badge, 14.0f, kPanelX1 - 118.0f, y0, kPanelX1 - (eq ? 18.0f : 8.0f), y1, ui::TextAlign::End, bcol);
        }
    } else if (data.mode == 1) {
        const int sel_a = std::clamp(data.selected_anim, 0, 4);
        const bool is_def = sel_a == 0;
        const int style_idx = sel_a - 1;
        const bool cur_owned = is_def || ((data.owned_reloads & (1u << static_cast<unsigned>(style_idx))) != 0u);
        const bool cur_eq = data.equipped_anim == sel_a;
        const int price = is_def ? 0 : SurvivalProfile::ReloadPrice(style_idx);
        primary_can = cur_eq ? false : (cur_owned || data.credits >= price);
        primary_label = cur_eq ? std::string(PcText("pc_armory_equipped", language))
                        : cur_owned ? std::string(PcText("pc_armory_equip", language))
                                    : std::format("{} (${})", PcText("pc_armory_buy", language), price);
        stage_caption = std::format("{}  /  {}", PcText(WeaponTabKey(data.weapon - 1), language), PcText(AnimNameKey(sel_a), language));

        for (int i = 0; i < 5; ++i) {
            const float y0 = kAnimListY0 + static_cast<float>(i) * (kAnimCardH + kAnimCardGap);
            const float y1 = y0 + kAnimCardH;
            const bool sel = sel_a == i;
            const bool eq = data.equipped_anim == i;
            const bool own = i == 0 || ((data.owned_reloads & (1u << static_cast<unsigned>(i - 1))) != 0u);
            const int p_i = i == 0 ? 0 : SurvivalProfile::ReloadPrice(i - 1);
            const bool hov = hovered_card_ == i;

            if (sel) {
                DrawPtGlowBar(batch, canvas, kPanelX0 - 4.0f, y0 + 2.0f, kPanelX1 + 4.0f, y1 - 2.0f, 0.24f);
            } else if (hov) {
                DrawPtGlowBar(batch, canvas, kPanelX0 - 4.0f, y0 + 2.0f, kPanelX1 + 4.0f, y1 - 2.0f, 0.11f);
            }
            DrawPtHairline(batch, canvas, kPanelX0, kPanelX1, y1 + 2.0f, 0.10f);

            text_box(PcText(AnimNameKey(i), language), 16.5f, kPanelX0 + 12.0f, y0 + 8.0f, kPanelX1 - 122.0f, y0 + 36.0f, ui::TextAlign::Start,
                     {1.0f, 1.0f, 1.0f, sel ? 1.0f : 0.80f});
            text_box(PcText(AnimDescKey(i), language), 12.5f, kPanelX0 + 12.0f, y0 + 38.0f, kPanelX1 - 12.0f, y1 - 8.0f, ui::TextAlign::Start,
                     {0.76f, 0.78f, 0.82f, sel ? 0.86f : 0.56f});

            const std::string badge = eq ? std::string(PcText("pc_armory_equipped", language))
                                      : own ? std::string(PcText("pc_armory_status_owned", language))
                                            : std::format("${}", p_i);
            const glm::vec4 bcol = eq ? glm::vec4(1.0f, 1.0f, 1.0f, 0.98f)
                                   : own ? glm::vec4(0.80f, 0.82f, 0.85f, 0.74f)
                                         : (data.credits >= p_i ? glm::vec4(0.95f, 0.95f, 0.97f, 0.90f) : glm::vec4(0.76f, 0.44f, 0.44f, 0.78f));
            if (eq) {
                DrawSolid(batch, canvas, kPanelX1 - 10.0f, y0 + 20.0f, kPanelX1 - 5.0f, y0 + 25.0f, {1.0f, 1.0f, 1.0f, 0.92f});
            }
            text_box(badge, 14.0f, kPanelX1 - 118.0f, y0 + 8.0f, kPanelX1 - (eq ? 18.0f : 8.0f), y0 + 36.0f, ui::TextAlign::End, bcol);
        }
    } else {
        const auto prects = FilterPillRects();
        for (int i = 0; i < 7; ++i) {
            const glm::vec4 r = prects[static_cast<size_t>(i)];
            const bool active = data.filter == i;
            const bool hov = hovered_filter_ == i;
            if (active) {
                DrawPtGlowBar(batch, canvas, r.x, r.y + 1.0f, r.z, r.w - 1.0f, 0.20f);
                DrawHGrad(batch, canvas, r.x + 8.0f, r.z - 8.0f, r.w - 1.0f, r.w, {1.0f, 1.0f, 1.0f, 0.55f}, {1.0f, 1.0f, 1.0f, 0.15f});
            } else if (hov) {
                DrawPtGlowBar(batch, canvas, r.x, r.y + 1.0f, r.z, r.w - 1.0f, 0.10f);
            }
            text_box(PcText(FilterPillKey(i), language), 13.0f, r.x + 4.0f, r.y, r.z - 4.0f, r.w, ui::TextAlign::Center,
                     active ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) : glm::vec4(0.76f, 0.78f, 0.80f, hov ? 0.88f : 0.54f));
        }
        DrawPtHairline(batch, canvas, kPanelX0, kPanelX1, 198.0f, 0.16f);

        const int count = static_cast<int>(data.filtered_skins.size());
        int sel_idx = 0;
        for (int i = 0; i < count; ++i) {
            if (data.filtered_skins[static_cast<size_t>(i)] == data.selected_skin) {
                sel_idx = i;
                break;
            }
        }
        const int max_scroll = std::max(0, count - kVisibleSkinCards);
        scroll_ = std::clamp(scroll_, 0, max_scroll);
        if (sel_idx < scroll_) scroll_ = sel_idx;
        if (sel_idx >= scroll_ + kVisibleSkinCards) scroll_ = sel_idx - kVisibleSkinCards + 1;

        for (int slot = 0; slot < kVisibleSkinCards && scroll_ + slot < count; ++slot) {
            const int list_i = scroll_ + slot;
            const int skin_id = data.filtered_skins[static_cast<size_t>(list_i)];
            const SurvivalSkinDef& sk = SurvivalSkin(skin_id);
            const float y0 = kSkinListY0 + static_cast<float>(slot) * (kSkinCardH + kSkinCardGap);
            const float y1 = y0 + kSkinCardH;
            const bool sel = skin_id == data.selected_skin;
            const bool eq = skin_id == data.equipped_skin;
            const bool own = skin_id == 0 || ((data.owned_skins & (1ull << static_cast<unsigned>(skin_id))) != 0ull);
            const bool hov = hovered_card_ == list_i;
            const float row_x1 = kPanelX1 - 14.0f;

            if (sel) {
                DrawPtGlowBar(batch, canvas, kPanelX0 - 4.0f, y0 + 2.0f, row_x1 + 2.0f, y1 - 2.0f, 0.24f);
            } else if (hov) {
                DrawPtGlowBar(batch, canvas, kPanelX0 - 4.0f, y0 + 2.0f, row_x1 + 2.0f, y1 - 2.0f, 0.11f);
            }
            DrawPtHairline(batch, canvas, kPanelX0, row_x1, y1 + 1.5f, 0.08f);

            const float sx0 = kPanelX0 + 8.0f;
            const float sy0 = y0 + 12.0f;
            const float sx1 = sx0 + 30.0f;
            const float sy1 = y1 - 12.0f;
            const float sym = 0.5f * (sy0 + sy1);
            const glm::vec3 acc_col = sk.rarity == SkinRarity::Mythic ? MythicLiveColor(sk.id, pulse_) : sk.accent;
            DrawSolid(batch, canvas, sx0, sy0, sx1, sym, {sk.metal_a.r, sk.metal_a.g, sk.metal_a.b, 1.0f});
            DrawSolid(batch, canvas, sx0, sym, sx1, sy1, {acc_col.r, acc_col.g, acc_col.b, 1.0f});
            DrawOutline(batch, canvas, sx0, sy0, sx1, sy1, 1.0f, {1.0f, 1.0f, 1.0f, sel ? 0.55f : 0.22f});

            text_box(sk.name, 15.5f, sx1 + 12.0f, y0 + 6.0f, row_x1 - 104.0f, y0 + 29.0f, ui::TextAlign::Start,
                     {1.0f, 1.0f, 1.0f, sel ? 1.0f : 0.80f});
            const std::string sub = std::format("{}  /  {}", PcText(SkinRarityKey(sk.rarity), language), PcText(ReloadStyleKey(sk.reload), language));
            text_box(sub, 11.5f, sx1 + 12.0f, y0 + 30.0f, row_x1 - 104.0f, y1 - 6.0f, ui::TextAlign::Start,
                     {0.76f, 0.78f, 0.82f, sel ? 0.84f : 0.52f});

            std::string badge;
            glm::vec4 bcol{0.82f, 0.84f, 0.86f, 0.85f};
            if (eq) {
                badge = std::string(PcText("pc_armory_equipped", language));
                bcol = {1.0f, 1.0f, 1.0f, 0.98f};
            } else if (own) {
                badge = std::string(PcText("pc_armory_status_owned", language));
                bcol = {0.80f, 0.82f, 0.86f, 0.72f};
            } else if (sk.price > 0) {
                badge = std::format("${}", sk.price);
                bcol = data.credits >= sk.price ? glm::vec4(0.96f, 0.96f, 0.98f, 0.92f) : glm::vec4(0.76f, 0.44f, 0.44f, 0.76f);
            } else if (sk.daily_wins_need > 0 && data.daily_wins < sk.daily_wins_need) {
                badge = std::format("{}d", sk.daily_wins_need);
                bcol = {0.72f, 0.74f, 0.78f, 0.68f};
            } else if (sk.milestone_wave > 0 && data.best_wave < sk.milestone_wave) {
                badge = std::format("W{}", sk.milestone_wave);
                bcol = {0.72f, 0.74f, 0.78f, 0.68f};
            }
            if (eq) {
                const float cy = 0.5f * (y0 + y1);
                DrawSolid(batch, canvas, row_x1 - 8.0f, cy - 2.0f, row_x1 - 4.0f, cy + 2.0f, {1.0f, 1.0f, 1.0f, 0.92f});
            }
            text_box(badge, 13.5f, row_x1 - 102.0f, y0, row_x1 - (eq ? 14.0f : 4.0f), y1, ui::TextAlign::End, bcol);
        }

        if (count > kVisibleSkinCards) {
            const float tx0 = kPanelX1 - 6.0f;
            const float tx1 = kPanelX1 - 2.0f;
            const float ty0 = kSkinListY0 + 2.0f;
            const float ty1 = kSkinListY0 + static_cast<float>(kVisibleSkinCards) * (kSkinCardH + kSkinCardGap) - kSkinCardGap - 2.0f;
            DrawSolid(batch, canvas, tx0, ty0, tx1, ty1, {1.0f, 1.0f, 1.0f, 0.10f});
            const float frac = static_cast<float>(kVisibleSkinCards) / static_cast<float>(count);
            const float pos = static_cast<float>(scroll_) / static_cast<float>(std::max(1, max_scroll));
            const float th = std::max(24.0f, (ty1 - ty0) * frac);
            const float sy = ty0 + ((ty1 - ty0) - th) * pos;
            DrawSolid(batch, canvas, tx0, sy, tx1, sy + th, {1.0f, 1.0f, 1.0f, 0.58f});
        }

        const SurvivalSkinDef& cur_sk = SurvivalSkin(data.selected_skin);
        const bool cur_own = cur_sk.id == 0 || ((data.owned_skins & (1ull << static_cast<unsigned>(cur_sk.id))) != 0ull);
        const bool cur_eq = data.equipped_skin == cur_sk.id;
        primary_can = cur_eq ? false : (cur_own || (cur_sk.price > 0 && data.credits >= cur_sk.price));
        primary_label = cur_eq ? std::string(PcText("pc_armory_equipped", language))
                        : cur_own ? std::string(PcText("pc_armory_equip", language))
                        : cur_sk.price > 0 ? std::format("{} (${})", PcText("pc_armory_buy", language), cur_sk.price)
                                           : std::string(PcText("pc_armory_locked", language));
        stage_caption = std::format("{}  /  {}  /  {}", PcText(WeaponTabKey(data.weapon - 1), language), cur_sk.name,
                                    PcText(ReloadStyleKey(cur_sk.reload), language));
    }

    DrawPtHairline(batch, canvas, kPanelX0, kPanelX1, kActionY0 - 10.0f, 0.18f);
    const auto arects = ActionRects(is_torch);
    for (int i = 0; i < (is_torch ? 1 : 3); ++i) {
        const glm::vec4 r = arects[static_cast<size_t>(i)];
        const bool hov = hovered_action_ == i;
        const bool enabled = i == 0 ? primary_can : true;
        const std::string lbl = i == 0 ? primary_label
                                : i == 1 ? std::string(PcText("pc_armory_apply_all", language))
                                         : std::string(PcText("pc_armory_preview_reload", language));
        DrawSolid(batch, canvas, r.x, r.y, r.z, r.w, {1.0f, 1.0f, 1.0f, enabled ? (hov ? 0.12f : 0.04f) : 0.015f});
        if (enabled && (hov || i == 0)) {
            DrawPtGlowBar(batch, canvas, r.x + 2.0f, r.y + 2.0f, r.z - 2.0f, r.w - 2.0f, hov ? 0.28f : 0.14f);
        }
        DrawOutline(batch, canvas, r.x, r.y, r.z, r.w, hov && enabled ? 1.5f : 1.0f,
                    {1.0f, 1.0f, 1.0f, enabled ? (hov ? 0.78f : (i == 0 ? 0.42f : 0.22f)) : 0.10f});
        text_box(lbl, 13.5f, r.x + 6.0f, r.y, r.z - 6.0f, r.w, ui::TextAlign::Center,
                 enabled ? glm::vec4(1.0f, 1.0f, 1.0f, hov ? 1.0f : 0.90f) : glm::vec4(0.62f, 0.64f, 0.68f, 0.46f));
    }

    text_box(stage_caption, 15.0f, kStageX0 + 10.0f, 608.0f, kStageX1, 638.0f, ui::TextAlign::End, {0.94f, 0.95f, 0.97f, 0.84f});
    if (data.playing_reload && !is_torch) {
        const float glow = 0.55f + 0.45f * std::sin(pulse_ * 6.0f);
        const std::string live = std::format("{}...", PcText(ReloadStyleKey(static_cast<ReloadStyle>(std::clamp(data.playing_style, 0, 3))), language));
        text_box(live, 13.5f, kStageX0 + 10.0f, 582.0f, kStageX1, 606.0f, ui::TextAlign::End, {1.0f, 1.0f, 1.0f, 0.86f * glow});
    }

    const std::string hint = is_torch
        ? std::format("{{tabprev}} {{tabnext}} {}    {{armoryorbit}} {}    {{back}} {}",
                      PcText("pc_armory_weapon", language), PcText("pc_armory_rotate", language), PcText("pc_back", language))
        : std::format("{{tabprev}} {{tabnext}} {}    {{armorytab}} {}    {{reload}} {}    {{armoryall}} {}    {{armoryorbit}} {}    {{back}} {}",
                      PcText("pc_armory_weapon", language), PcText("pc_armory_tab_animations", language),
                      PcText("pc_armory_preview_reload", language), PcText("pc_armory_apply_all", language),
                      PcText("pc_armory_rotate", language), PcText("pc_back", language));
    DrawPromptText(batch, canvas, assets, *font, hint, prompts, 15.0f, {20.0f, 660.0f}, {1260.0f, 710.0f},
                   ui::TextAlign::Center, ui::TextAlign::Center, {0.90f, 0.92f, 0.95f, 0.88f}, true);
}

}
