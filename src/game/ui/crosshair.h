#pragma once

#include <glm/glm.hpp>

namespace pt::ui {
class UiBatch;
}

namespace pt::game {

struct UiCanvas;

struct CrosshairStyle {
    enum Shape : int { Ticks, Cross, Tee, Circle, CircleDot, Dot, None, ShapeCount };
    int shape = Ticks;
    int color = 0;
    float length = 7.0f;
    float thickness = 1.5f;
    float gap = 3.0f;
    bool dynamic = true;
    bool dot = true;
    bool outline = true;
    float opacity = 0.8f;
    bool hit_marker = true;
    bool behind_sights = false;
};

constexpr int kCrosshairColorCount = 7;
glm::vec3 CrosshairColor(int color);

void DrawCrosshair(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 middle, const CrosshairStyle& style, float cone, float alpha, float scale = 1.0f);
void DrawHitMarker(ui::UiBatch& batch, const UiCanvas& canvas, glm::vec2 middle, float mark, bool head, bool kill, float scale = 1.0f);

}
