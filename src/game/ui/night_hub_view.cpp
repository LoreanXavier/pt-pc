#include "game/ui/night_hub_view.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "engine/audio/audio_engine.h"
#include "engine/render/texture_manager.h"
#include "engine/ui/text_layout.h"
#include "game/game.h"
#include "game/ui/dialog_view.h"
#include "game/ui/options_menu.h"
#include "game/ui/prompt_hint.h"
#include "game/ui/ui_assets.h"

namespace pt::game {

namespace {

constexpr const char* kCursorSound = "Play_sys_cursor_01";
constexpr const char* kSelectSound = "Play_sys_change_01";
constexpr const char* kCancelSound = "Play_sys_cursor_01";

constexpr float kLeftX0 = 76.0f;
constexpr float kLeftX1 = 532.0f;
constexpr float kDividerX = 580.0f;
constexpr float kRightX0 = 626.0f;
constexpr float kRightX1 = 1204.0f;
constexpr float kHeaderY0 = 32.0f;
constexpr float kHeaderY1 = 78.0f;
constexpr float kHeaderLineY = 86.0f;
constexpr float kListTopY = 112.0f;
constexpr float kFooterLineY = 648.0f;

struct PlacedRow {
    int flat_index = 0;
    const NightHubItem* item = nullptr;
    float y0 = 0.0f;
    float y1 = 0.0f;
};

struct PlacedSection {
    std::string title_key;
    float y0 = 0.0f;
    float line_y = 0.0f;
};

struct HubLayout {
    std::vector<PlacedSection> sections;
    std::vector<PlacedRow> rows;
};

HubLayout BuildLayout(const NightHubSnapshot& data) {
    HubLayout out;
    int total_items = 0;
    for (const auto& s : data.sections) total_items += static_cast<int>(s.items.size());
    const bool dense = total_items >= 10;
    const float sec_h = dense ? 24.0f : 28.0f;
    const float row_h = dense ? 34.0f : 40.0f;
    const float row_gap = dense ? 2.0f : 4.0f;
    const float sec_gap = dense ? 14.0f : 22.0f;

    float y = kListTopY;
    int flat = 0;
    for (size_t si = 0; si < data.sections.size(); ++si) {
        const auto& sec = data.sections[si];
        if (sec.items.empty()) continue;
        if (si > 0) y += sec_gap;
        out.sections.push_back({sec.title_key, y, y + sec_h});
        y += sec_h + (dense ? 6.0f : 10.0f);
        for (const auto& item : sec.items) {
            out.rows.push_back({flat++, &item, y, y + row_h});
            y += row_h + row_gap;
        }
    }
    return out;
}

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

void DrawVGrad(ui::UiBatch& batch, const UiCanvas& canvas, float x0, float x1, float y0, float y1, glm::vec4 ct, glm::vec4 cb) {
    if (ct.a <= 0.001f && cb.a <= 0.001f) return;
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    const std::vector<ui::UiVertex> v = {
        {canvas.ToTarget({x0, y0}), {0, 0}, ct}, {canvas.ToTarget({x1, y0}), {0, 0}, ct}, {canvas.ToTarget({x1, y1}), {0, 0}, cb},
        {canvas.ToTarget({x0, y0}), {0, 0}, ct}, {canvas.ToTarget({x1, y1}), {0, 0}, cb}, {canvas.ToTarget({x0, y1}), {0, 0}, cb},
    };
    batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
}

void DrawPtGlowBar(ui::UiBatch& batch, const UiCanvas& canvas, float x0, float y0, float x1, float y1, float peak_alpha) {
    if (peak_alpha <= 0.001f) return;
    const float w = x1 - x0;
    const float a = x0 + w * 0.20f;
    const float b = x1 - w * 0.20f;
    DrawHGrad(batch, canvas, x0, a, y0, y1, {1.0f, 1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, peak_alpha});
    DrawHGrad(batch, canvas, a, b, y0, y1, {1.0f, 1.0f, 1.0f, peak_alpha}, {1.0f, 1.0f, 1.0f, peak_alpha});
    DrawHGrad(batch, canvas, b, x1, y0, y1, {1.0f, 1.0f, 1.0f, peak_alpha}, {1.0f, 1.0f, 1.0f, 0.0f});
}

void DrawPtHairline(ui::UiBatch& batch, const UiCanvas& canvas, float x0, float x1, float y, float alpha) {
    DrawHGrad(batch, canvas, x0, x1, y, y + 1.0f, {1.0f, 1.0f, 1.0f, alpha}, {1.0f, 1.0f, 1.0f, 0.0f});
}

std::string ResolveText(std::string_view key_or_text, int language) {
    if (key_or_text.starts_with("pc_")) return PcNoteText(key_or_text, language);
    return std::string(key_or_text);
}

}

void NightHubView::Reset() {
    focus_ = 0;
    hovered_ = -1;
    flash_ = 0.0f;
    repeat_timer_ = 0.0f;
    repeat_dir_ = 0;
}

NightHubView::Result NightHubView::Update(Game& game, PcSettingsSource& source, const MenuInput& input, float dt) {
    flash_ += dt * 60.0f;
    auto sound = [&](const char* ev) {
        if (game.Audio()) game.Audio()->PostEvent(ev, nullptr);
    };

    if (input.back || input.close) {
        sound(kCancelSound);
        return Result::Back;
    }

    const NightHubSnapshot data = source.NightHubData();
    const HubLayout lay = BuildLayout(data);
    const int count = static_cast<int>(lay.rows.size());
    if (count <= 0) return Result::Stay;
    focus_ = std::clamp(focus_, 0, count - 1);

    bool up = input.subtitle_up;
    bool down = input.subtitle_down;
    const uint32_t dirs = input.held_dirs & (kRawUp | kRawDown);
    if (dirs != 0 && dirs == repeat_dir_) {
        repeat_timer_ -= dt;
        if (repeat_timer_ <= 0.0f) {
            repeat_timer_ = 0.12f;
            if (dirs & kRawUp) up = true;
            if (dirs & kRawDown) down = true;
        }
    } else {
        repeat_dir_ = dirs;
        repeat_timer_ = 0.30f;
    }

    if (up) {
        focus_ = (focus_ - 1 + count) % count;
        flash_ = 0.0f;
        sound(kCursorSound);
    } else if (down) {
        focus_ = (focus_ + 1) % count;
        flash_ = 0.0f;
        sound(kCursorSound);
    }

    hovered_ = -1;
    if (input.pointer_valid) {
        const glm::vec2 p = PointerToCanvas(input.pointer);
        for (const auto& row : lay.rows) {
            if (InRect(p, kLeftX0 - 16.0f, row.y0, kLeftX1 + 12.0f, row.y1)) {
                hovered_ = row.flat_index;
                if (focus_ != row.flat_index) {
                    focus_ = row.flat_index;
                    flash_ = 0.0f;
                    sound(kCursorSound);
                }
                if (input.click && row.item) {
                    sound(kSelectSound);
                    source.Activate(row.item->id);
                    return Result::Activated;
                }
                break;
            }
        }
    }

    if (input.accept) {
        if (const NightHubItem* item = lay.rows[static_cast<size_t>(focus_)].item) {
            sound(kSelectSound);
            source.Activate(item->id);
            return Result::Activated;
        }
    }

    return Result::Stay;
}

void NightHubView::Draw(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcSettingsSource& source,
                        const PromptStyle& prompts, int language) {
    UiFont* font = assets.Font(UiFontType::PcSystem, language);
    if (!font) return;

    const NightHubSnapshot data = source.NightHubData();
    const HubLayout lay = BuildLayout(data);
    const bool rtl = language == dialog::kArabic;

    auto text_line = [&](std::string_view words, float size, float x0, float y0, float x1, float y1, ui::TextAlign align, glm::vec4 color) {
        ui::TextStyle st;
        st.font = &font->font;
        st.font_width = size;
        st.font_height = size;
        st.line_space = size * 0.25f;
        ui::TextLayout layout = ui::LayoutText(words, st, 0.0f);
        if (layout.width > (x1 - x0) && layout.width > 1.0f) {
            const float shrink = (x1 - x0) / layout.width;
            st.font_width = std::max(9.0f, size * shrink);
            st.font_height = std::max(9.0f, size * shrink);
            layout = ui::LayoutText(words, st, 0.0f);
        }
        ui::PlaceText(layout, {x0, y0}, {x1, y1}, align, align, ui::TextAlign::Center, rtl);
        DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha);
    };

    auto text_wrap = [&](std::string_view words, float size, float x0, float y0, float x1, float y1, glm::vec4 color) -> float {
        ui::TextStyle st;
        st.font = &font->font;
        st.font_width = size;
        st.font_height = size;
        st.line_space = size * 0.36f;
        ui::TextLayout layout = ui::LayoutText(words, st, x1 - x0);
        ui::PlaceText(layout, {x0, y0}, {x1, y1}, ui::TextAlign::Start, ui::TextAlign::Start, ui::TextAlign::Start, rtl);
        DrawText(batch, canvas, *font, layout, color, ui::UiBlend::Alpha);
        return y0 + layout.height;
    };

    DrawSolid(batch, canvas, 0.0f, 0.0f, UiCanvas::kWidth, UiCanvas::kHeight, {0.008f, 0.009f, 0.012f, 0.96f});

    text_line(PcText("pc_survival", language), 28.0f, kLeftX0, kHeaderY0, 620.0f, kHeaderY1, ui::TextAlign::Start, {0.98f, 0.98f, 1.0f, 0.96f});
    {
        std::string status = std::format("{}  ${}", PcText("pc_armory_credits", language), data.credits);
        if (data.best_wave > 0) {
            const std::string best = PcNoteText(std::format("pc_survival_best{}{}", kPcNoteArgument, data.best_wave), language);
            std::string best_clean = std::string(PcText("pc_survival_best", language));
            if (const size_t pos = best_clean.find("{}"); pos != std::string::npos) {
                best_clean.replace(pos, 2, std::to_string(data.best_wave));
            } else {
                best_clean = best;
            }
            status = std::format("{}    /    {}", best_clean, status);
        }
        text_line(status, 15.5f, 680.0f, kHeaderY0 + 6.0f, kRightX1, kHeaderY1, ui::TextAlign::End, {0.82f, 0.84f, 0.88f, 0.72f});
    }
    DrawPtHairline(batch, canvas, kLeftX0, kRightX1, kHeaderLineY, 0.24f);

    const float div_mid = 0.5f * (kListTopY + kFooterLineY);
    DrawVGrad(batch, canvas, kDividerX, kDividerX + 1.0f, kListTopY, div_mid, {1.0f, 1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 0.16f});
    DrawVGrad(batch, canvas, kDividerX, kDividerX + 1.0f, div_mid, kFooterLineY - 12.0f, {1.0f, 1.0f, 1.0f, 0.16f}, {1.0f, 1.0f, 1.0f, 0.0f});

    for (const auto& sec : lay.sections) {
        text_line(PcText(sec.title_key, language), 15.0f, kLeftX0, sec.y0, kLeftX1, sec.line_y - 2.0f, ui::TextAlign::Start,
                  {0.78f, 0.80f, 0.84f, 0.64f});
        DrawPtHairline(batch, canvas, kLeftX0, kLeftX1, sec.line_y, 0.20f);
    }

    const float ramp = std::clamp(flash_ / 6.0f, 0.0f, 1.0f);
    const float glow_peak = 0.18f + 0.08f * (1.0f - ramp * ramp * (3.0f - 2.0f * ramp));
    const int count = static_cast<int>(lay.rows.size());
    const int clamped_focus = count > 0 ? std::clamp(focus_, 0, count - 1) : -1;

    for (const auto& row : lay.rows) {
        if (!row.item) continue;
        const bool sel = row.flat_index == clamped_focus;
        const bool hov = row.flat_index == hovered_;
        if (sel) {
            DrawPtGlowBar(batch, canvas, kLeftX0 - 18.0f, row.y0 + 2.0f, kLeftX1 + 14.0f, row.y1 - 2.0f, glow_peak);
        } else if (hov) {
            DrawPtGlowBar(batch, canvas, kLeftX0 - 18.0f, row.y0 + 2.0f, kLeftX1 + 14.0f, row.y1 - 2.0f, 0.10f);
        }
        const bool has_tag = !row.item->tag.empty();
        const float label_x1 = has_tag ? (kLeftX1 - 134.0f) : (kLeftX1 - 8.0f);
        text_line(ResolveText(row.item->label_key, language), 19.0f, kLeftX0 + 8.0f, row.y0, label_x1, row.y1, ui::TextAlign::Start,
                  {1.0f, 1.0f, 1.0f, sel ? 1.0f : 0.72f});
        if (has_tag) {
            const std::string tag_str = row.item->tag_is_key ? ResolveText(row.item->tag, language) : row.item->tag;
            text_line(tag_str, 14.5f, kLeftX1 - 128.0f, row.y0, kLeftX1 - 8.0f, row.y1, ui::TextAlign::End,
                      {0.92f, 0.94f, 0.96f, sel ? 0.94f : 0.48f});
        }
    }

    if (clamped_focus >= 0 && lay.rows[static_cast<size_t>(clamped_focus)].item) {
        const NightHubItem& cur = *lay.rows[static_cast<size_t>(clamped_focus)].item;
        float ry = 148.0f;

        const bool has_tag = !cur.tag.empty();
        const float title_x1 = has_tag ? (kRightX1 - 160.0f) : kRightX1;
        text_line(ResolveText(cur.label_key, language), 26.0f, kRightX0, ry, title_x1, ry + 38.0f, ui::TextAlign::Start,
                  {1.0f, 1.0f, 1.0f, 0.98f});
        if (has_tag) {
            const std::string tag_str = cur.tag_is_key ? ResolveText(cur.tag, language) : cur.tag;
            text_line(tag_str, 17.0f, kRightX1 - 154.0f, ry + 4.0f, kRightX1, ry + 38.0f, ui::TextAlign::End,
                      {0.90f, 0.92f, 0.95f, 0.78f});
        }
        ry += 48.0f;
        DrawPtHairline(batch, canvas, kRightX0, kRightX1, ry, 0.28f);
        ry += 24.0f;

        if (!cur.note.empty()) {
            const std::string desc = ResolveText(cur.note, language);
            ry = text_wrap(desc, 17.5f, kRightX0, ry, kRightX1 - 12.0f, ry + 180.0f, {0.88f, 0.90f, 0.93f, 0.86f});
            ry += 18.0f;
        }

        if (!cur.detail_sub.empty()) {
            const std::string sub = ResolveText(cur.detail_sub, language);
            ry = text_wrap(sub, 15.5f, kRightX0, ry, kRightX1 - 12.0f, ry + 120.0f, {0.78f, 0.80f, 0.84f, 0.70f});
            ry += 18.0f;
        }

        if (!cur.stats.empty()) {
            ry = std::max(ry + 8.0f, 340.0f);
            for (const auto& st : cur.stats) {
                if (ry > kFooterLineY - 56.0f) break;
                DrawPtHairline(batch, canvas, kRightX0, kRightX1, ry, 0.14f);
                const float sy0 = ry + 6.0f;
                const float sy1 = sy0 + 34.0f;
                text_line(ResolveText(st.label_key, language), 15.0f, kRightX0, sy0, kRightX0 + 280.0f, sy1, ui::TextAlign::Start,
                          {0.74f, 0.76f, 0.80f, 0.66f});
                const std::string val = st.value_is_key ? ResolveText(st.value, language) : st.value;
                text_line(val, 16.5f, kRightX0 + 286.0f, sy0, kRightX1, sy1, ui::TextAlign::End, {0.98f, 0.98f, 1.0f, 0.94f});
                ry = sy1 + 4.0f;
            }
            DrawPtHairline(batch, canvas, kRightX0, kRightX1, ry, 0.14f);
        }
    }

    DrawPtHairline(batch, canvas, kLeftX0, kRightX1, kFooterLineY, 0.20f);
    const std::string hint = std::format("{{accept}} {}      {{back}} {}", PcText("pc_ok", language), PcText("pc_back", language));
    DrawPromptText(batch, canvas, assets, *font, hint, prompts, 15.5f, {kLeftX0, kFooterLineY + 8.0f}, {kRightX1, UiCanvas::kHeight - 10.0f},
                   ui::TextAlign::Center, ui::TextAlign::Center, {0.88f, 0.90f, 0.94f, 0.86f}, true);
}

}
