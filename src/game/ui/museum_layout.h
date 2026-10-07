#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace pt::game {

struct MuseumLayout {
    struct Cell {
        glm::vec2 lo{0.0f};
        glm::vec2 hi{0.0f};
        bool Contains(glm::vec2 p) const { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; }
    };

    static constexpr int kHallColumns = 3;
    static constexpr float kHallWidth = 340.0f;
    static constexpr float kHallGap = 40.0f;
    static constexpr float kHallPicture = 140.0f;
    static constexpr float kHallPlaque = 44.0f;
    static constexpr float kHallTop = 158.0f;
    static constexpr float kHallRowGap = 18.0f;
    static constexpr float kHallTextY = 564.0f;
    static constexpr float kHallTextHeight = 22.0f;

    static constexpr float kThumbWidth = 128.0f;
    static constexpr float kThumbHeight = 72.0f;
    static constexpr float kThumbGap = 12.0f;
    static constexpr float kStripTop = 503.0f;
    static constexpr float kCanvasWidth = 1280.0f;
    static constexpr float kCanvasHeight = 720.0f;
    static constexpr Cell kSpotlight{{391.0f, 156.0f}, {889.0f, 436.0f}};
    static constexpr float kPlaqueTitleY = 446.0f;
    static constexpr float kPlaqueCaptionY = 472.0f;

    static Cell HallCell(int i) {
        const int column = i % kHallColumns;
        const int row = i / kHallColumns;
        const float x = (kCanvasWidth - (kHallColumns * kHallWidth + (kHallColumns - 1) * kHallGap)) * 0.5f + column * (kHallWidth + kHallGap);
        const float y = kHallTop + row * (kHallPicture + kHallPlaque + kHallRowGap);
        return {{x, y}, {x + kHallWidth, y + kHallPicture + kHallPlaque}};
    }
    static Cell HallPicture(int i) {
        Cell cell = HallCell(i);
        cell.hi.y = cell.lo.y + kHallPicture;
        return cell;
    }
    static Cell HallText(int index) {
        const float y = kHallTextY + index * kHallTextHeight;
        return {{kCanvasWidth * 0.5f - 300.0f, y - kHallTextHeight * 0.5f}, {kCanvasWidth * 0.5f + 300.0f, y + kHallTextHeight * 0.5f}};
    }

    static int WallVisible(float width) {
        const float room = std::max(width, kCanvasWidth) - 180.0f;
        return std::clamp(static_cast<int>(std::floor((room + kThumbGap) / (kThumbWidth + kThumbGap))), 4, 16);
    }
    static Cell WallCell(int slot, int visible) {
        const float total = visible * kThumbWidth + (visible - 1) * kThumbGap;
        const float x = kCanvasWidth * 0.5f - total * 0.5f + slot * (kThumbWidth + kThumbGap);
        return {{x, kStripTop}, {x + kThumbWidth, kStripTop + kThumbHeight}};
    }
};

}
