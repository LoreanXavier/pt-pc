#include "game/ui/dialog_view.h"

#include <algorithm>
#include <array>
#include <limits>

#include "engine/render/texture_manager.h"
#include "engine/ui/text_layout.h"
#include "engine/ui/uif.h"
#include "game/ui/options_menu.h"
#include "game/ui/prompt_hint.h"
#include "game/ui/ui_assets.h"
#include "game/ui/uif_view.h"

namespace pt::game::dialog {
namespace {

constexpr uint16_t kBarNode = 81;
constexpr float kPlateHeight = 64.0f;
constexpr float kPlateGap = 40.0f;
constexpr float kSingleWidth = 300.0f;
constexpr float kPairWidth = 380.0f;
constexpr float kTextSize = 26.0f;
constexpr float kLabelSize = 28.0f;
constexpr float kBackSize = 20.0f;

constexpr Cell kPanel{{190.0f, 200.0f}, {1090.0f, 520.0f}};
constexpr float kTextTop = 226.0f;
constexpr float kTextBottom = 372.0f;
constexpr float kPlateTop = 392.0f;
constexpr float kFooterTop = 472.0f;
constexpr float kFooterBottom = 502.0f;
constexpr float kInset = 40.0f;

}

Cell Panel() { return kPanel; }

glm::vec2 UnitsToCanvas(glm::vec2 units) {
    return {UiCanvas::kWidth * 0.5f + units.x * UiCanvas::kUnit, UiCanvas::kHeight * 0.5f - units.y * UiCanvas::kUnit};
}

Cell Plate(int index, int count, int language) {
    count = std::max(count, 1);
    const float room = kPanel.hi.x - kPanel.lo.x - 2.0f * kInset;
    const float width = count == 1 ? kSingleWidth : std::min(kPairWidth, (room - kPlateGap * static_cast<float>(count - 1)) / static_cast<float>(count));
    const float total = width * static_cast<float>(count) + kPlateGap * static_cast<float>(count - 1);
    const int slot = Rtl(language) ? count - 1 - index : index;
    const float x = UiCanvas::kWidth * 0.5f - total * 0.5f + static_cast<float>(slot) * (width + kPlateGap);
    return {{x, kPlateTop}, {x + width, kPlateTop + kPlateHeight}};
}

void Nav::Open(int focus, uint32_t held) {
    focus_ = std::max(focus, 0);
    chosen_ = -1;
    moved_ = false;
    held_ = held;
    last_pointer_ = glm::vec2(-1.0f);
    fresh_ = true;
}

Result Nav::Update(const MenuInput& input, int count, int language, bool can_cancel) {
    moved_ = false;
    chosen_ = -1;
    count = std::max(count, 1);
    focus_ = std::clamp(focus_, 0, count - 1);
    const uint32_t pressed = input.held_dirs & ~held_;
    held_ = input.held_dirs;
    int step = 0;
    if (pressed & kRawUp) {
        step = -1;
    } else if (pressed & kRawDown) {
        step = 1;
    } else if (pressed & kRawLeft) {
        step = Rtl(language) ? 1 : -1;
    } else if (pressed & kRawRight) {
        step = Rtl(language) ? -1 : 1;
    }
    if (step != 0) {
        const int next = std::clamp(focus_ + step, 0, count - 1);
        moved_ = next != focus_;
        focus_ = next;
    }
    int hovered = -1;
    if (input.pointer_valid) {
        if (fresh_) {
            last_pointer_ = input.pointer;
            fresh_ = false;
        }
        const glm::vec2 at = UnitsToCanvas(input.pointer);
        for (int i = 0; i < count; ++i) {
            if (Plate(i, count, language).Contains(at)) hovered = i;
        }
        if (input.pointer != last_pointer_ && hovered >= 0 && hovered != focus_) {
            focus_ = hovered;
            moved_ = true;
        }
        last_pointer_ = input.pointer;
    }
    if (input.click && hovered >= 0) {
        focus_ = hovered;
        chosen_ = hovered;
        return Result::Chosen;
    }
    if (input.accept) {
        chosen_ = focus_;
        return Result::Chosen;
    }
    if (can_cancel && (input.back || input.close || input.right_click)) return Result::Cancelled;
    return Result::None;
}

void Draw(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const ui::UifModel* model, const PromptStyle& prompts, const View& view) {
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    const float fade = std::clamp(view.alpha, 0.0f, 1.0f);
    auto solid = [&](glm::vec2 lo, glm::vec2 hi, glm::vec4 color) {
        color.a *= fade;
        batch.Quad(canvas.ToTarget(lo), canvas.ToTarget(hi), glm::vec2(0.0f), glm::vec2(1.0f), color, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    auto frame = [&](const Cell& c, float width, glm::vec4 color) {
        solid(c.lo, {c.hi.x, c.lo.y + width}, color);
        solid({c.lo.x, c.hi.y - width}, c.hi, color);
        solid({c.lo.x, c.lo.y + width}, {c.lo.x + width, c.hi.y - width}, color);
        solid({c.hi.x - width, c.lo.y + width}, {c.hi.x, c.hi.y - width}, color);
    };
    if (view.dim > 0.0f) {
        batch.Quad(glm::vec2(0.0f), glm::vec2(static_cast<float>(batch.Extent().width), static_cast<float>(batch.Extent().height)), glm::vec2(0.0f),
                   glm::vec2(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, view.dim * fade), plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    }
    solid(kPanel.lo, kPanel.hi, glm::vec4(0.0f, 0.0f, 0.0f, 0.97f));
    frame(kPanel, 2.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.28f));
    std::string every = view.text + view.back_label;
    for (const std::string& choice : view.choices) every += choice;
    UiFont* font = assets.FontCovering(UiFontType::PcSystem, view.language, every);
    if (!font) return;

    DrawPromptText(batch, canvas, assets, *font, view.text, prompts, kTextSize, {kPanel.lo.x + kInset, kTextTop}, {kPanel.hi.x - kInset, kTextBottom},
                   ui::TextAlign::Center, ui::TextAlign::Center, glm::vec4(1.0f, 1.0f, 1.0f, fade), true, 6.0f);

    auto highlight = [&](const Cell& plate, float alpha) {
        const glm::vec2 lo = plate.lo + glm::vec2(3.0f);
        const glm::vec2 hi = plate.hi - glm::vec2(3.0f);
        const float w = hi.x - lo.x;
        const float x0 = lo.x, x1 = lo.x + w * 0.22f, x2 = hi.x - w * 0.22f, x3 = hi.x;
        const glm::vec4 edge(1.0f, 1.0f, 1.0f, 0.08f * alpha);
        const glm::vec4 core(1.0f, 1.0f, 1.0f, 0.34f * alpha);
        auto strip = [&](float a, float b, const glm::vec4& ca, const glm::vec4& cb) {
            const std::vector<ui::UiVertex> v = {
                {canvas.ToTarget({a, lo.y}), {0, 0}, ca}, {canvas.ToTarget({b, lo.y}), {0, 0}, cb}, {canvas.ToTarget({b, hi.y}), {0, 0}, cb},
                {canvas.ToTarget({a, lo.y}), {0, 0}, ca}, {canvas.ToTarget({b, hi.y}), {0, 0}, cb}, {canvas.ToTarget({a, hi.y}), {0, 0}, ca},
            };
            batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
        };
        strip(x0, x1, edge, core);
        strip(x1, x2, core, core);
        strip(x2, x3, core, edge);
        (void)model;
    };

    const int count = static_cast<int>(view.choices.size());
    for (int i = 0; i < count; ++i) {
        const Cell plate = Plate(i, count, view.language);
        const bool focused = i == view.focus;
        solid(plate.lo, plate.hi, glm::vec4(1.0f, 1.0f, 1.0f, focused ? 0.12f : 0.04f));
        if (focused) highlight(plate, std::clamp(view.flash, 0.0f, 1.0f) * fade);
        frame(plate, focused ? 3.0f : 2.0f, glm::vec4(1.0f, 1.0f, 1.0f, focused ? 0.95f : 0.35f));
        const std::string label = focused ? "{accept} " + view.choices[static_cast<size_t>(i)] : view.choices[static_cast<size_t>(i)];
        DrawPromptText(batch, canvas, assets, *font, label, prompts, kLabelSize, plate.lo + glm::vec2(16.0f, 0.0f), plate.hi - glm::vec2(16.0f, 0.0f),
                       ui::TextAlign::Center, ui::TextAlign::Center, glm::vec4(1.0f, 1.0f, 1.0f, (focused ? 1.0f : 0.65f) * fade), true);
    }
    if (!view.back_label.empty()) {
        DrawPromptText(batch, canvas, assets, *font, "{back}: " + view.back_label, prompts, kBackSize, {kPanel.lo.x + kInset, kFooterTop},
                       {kPanel.hi.x - kInset, kFooterBottom}, ui::TextAlign::Center, ui::TextAlign::Center, glm::vec4(1.0f, 1.0f, 1.0f, 0.6f * fade), true);
    }
}

}
