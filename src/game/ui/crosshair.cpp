#include "game/ui/crosshair.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "engine/render/texture_manager.h"
#include "engine/ui/ui_batch.h"
#include "game/ui/uif_view.h"

namespace pt::game {

glm::vec3 CrosshairColor(int color) {
    static constexpr std::array<glm::vec3, kCrosshairColorCount> kColors = {
        glm::vec3(1.0f, 1.0f, 1.0f), glm::vec3(0.35f, 1.0f, 0.4f), glm::vec3(1.0f, 0.92f, 0.25f), glm::vec3(0.3f, 0.95f, 1.0f),
        glm::vec3(1.0f, 0.35f, 0.95f), glm::vec3(1.0f, 0.22f, 0.18f), glm::vec3(1.0f, 0.6f, 0.15f),
    };
    return kColors[static_cast<size_t>(std::clamp(color, 0, kCrosshairColorCount - 1))];
}

void DrawCrosshair(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 middle, const CrosshairStyle& style, float cone, float alpha, float scale) {
    const float a = std::clamp(alpha * style.opacity, 0.0f, 1.0f);
    if (a <= 0.0f || style.shape == CrosshairStyle::None) return;
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    const glm::vec4 color(CrosshairColor(style.color), a);
    const glm::vec4 edge(0.0f, 0.0f, 0.0f, 0.55f * a);
    const float width = std::max(0.5f, style.thickness) * scale;
    const float rim = 1.0f * scale;
    const float gap = ((style.shape == CrosshairStyle::Cross ? 0.0f : style.gap) + (style.dynamic ? cone : 0.0f)) * scale;
    const float length = std::max(0.5f, style.length) * scale;
    std::vector<ui::UiVertex> fill, outline;
    auto quad = [&](std::vector<ui::UiVertex>& v, glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, const glm::vec4& c) {
        for (const glm::vec2 p : {p0, p1, p2, p0, p2, p3}) v.push_back({canvas.ToTarget(p), {0.0f, 0.0f}, c});
    };
    auto stroke = [&](glm::vec2 from, glm::vec2 to, float w) {
        const glm::vec2 d = to - from;
        const float l = glm::length(d);
        if (l < 1e-3f) return;
        const glm::vec2 u = d / l, n(-u.y, u.x);
        const glm::vec2 h = n * (w * 0.5f);
        if (style.outline) {
            const glm::vec2 he = n * (w * 0.5f + rim), ue = u * rim;
            quad(outline, from - ue + he, to + ue + he, to + ue - he, from - ue - he, edge);
        }
        quad(fill, from + h, to + h, to - h, from - h, color);
    };
    auto dot = [&](float size) {
        const glm::vec2 h(size * 0.5f);
        if (style.outline) quad(outline, middle - h - rim, glm::vec2(middle.x + h.x + rim, middle.y - h.y - rim), middle + h + rim,
                                glm::vec2(middle.x - h.x - rim, middle.y + h.y + rim), edge);
        quad(fill, middle - h, glm::vec2(middle.x + h.x, middle.y - h.y), middle + h, glm::vec2(middle.x - h.x, middle.y + h.y), color);
    };
    auto ring = [&](float radius) {
        constexpr int kSteps = 40;
        for (int i = 0; i < kSteps; ++i) {
            const float a0 = 6.2831853f * static_cast<float>(i) / kSteps, a1 = 6.2831853f * static_cast<float>(i + 1) / kSteps;
            stroke(middle + radius * glm::vec2(std::cos(a0), std::sin(a0)), middle + radius * glm::vec2(std::cos(a1), std::sin(a1)), width);
        }
    };
    switch (style.shape) {
    case CrosshairStyle::Ticks:
    case CrosshairStyle::Tee:
    case CrosshairStyle::Cross: {
        for (const glm::vec2 d : {glm::vec2(1, 0), glm::vec2(-1, 0), glm::vec2(0, 1), glm::vec2(0, -1)}) {
            if (style.shape == CrosshairStyle::Tee && d.y < 0.0f) continue;
            stroke(middle + d * gap, middle + d * (gap + length), width);
        }
        if (style.dot) dot(std::max(2.0f * scale, width));
        break;
    }
    case CrosshairStyle::Circle:
    case CrosshairStyle::CircleDot:
        ring(std::max(2.0f * scale, gap + length * 0.5f));
        if (style.shape == CrosshairStyle::CircleDot || style.dot) dot(std::max(2.0f * scale, width * 1.3f));
        break;
    case CrosshairStyle::Dot: dot(std::max(2.0f * scale, width * 2.0f)); break;
    default: break;
    }
    if (!outline.empty()) batch.Draw(outline, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
    if (!fill.empty()) batch.Draw(fill, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
}

void DrawHitMarker(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 middle, float mark, bool head, bool kill, float scale) {
    if (mark <= 0.0f) return;
    const ui::UiDrawParams plain = ui::UiDrawParams::Plain(TextureManager::kWhite);
    const float a = std::min(1.0f, mark * 1.6f);
    const float inner = (head ? 8.0f : 7.0f) * scale;
    const float length = (kill ? 9.0f : head ? 8.0f : 6.5f) * scale;
    const glm::vec4 color = kill ? glm::vec4(0.92f, 0.16f, 0.12f, 0.95f * a) : glm::vec4(1.0f, 1.0f, 1.0f, 0.9f * a);
    std::vector<ui::UiVertex> v;
    auto stroke = [&](glm::vec2 from, glm::vec2 to, float w, const glm::vec4& c) {
        const glm::vec2 d = glm::normalize(to - from), n = glm::vec2(-d.y, d.x) * (w * 0.5f);
        for (const glm::vec2 p : {from + n, to + n, to - n, from + n, to - n, from - n}) v.push_back({canvas.ToTarget(p), {0.0f, 0.0f}, c});
    };
    for (const int pass : {0, 1}) {
        for (const glm::vec2 d : {glm::vec2(1, 1), glm::vec2(-1, 1), glm::vec2(1, -1), glm::vec2(-1, -1)}) {
            const glm::vec2 u = d * 0.70710678f;
            if (pass == 0) {
                stroke(middle + u * inner, middle + u * (inner + length), 3.2f * scale, glm::vec4(0.0f, 0.0f, 0.0f, 0.45f * a));
            } else {
                stroke(middle + u * inner, middle + u * (inner + length), 1.7f * scale, color);
            }
        }
    }
    batch.Draw(v, plain, ui::UiShade::Solid, ui::UiBlend::Alpha);
}

}
