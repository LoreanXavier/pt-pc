#pragma once

#include <glm/glm.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "engine/ui/text_layout.h"
#include "engine/ui/ui_batch.h"
#include "game/ui/pc_settings.h"

namespace pt::game {

class UiAssets;
class PcSettingsPage;
struct UiCanvas;

class ArchiveView {
public:
    void DrawPanel(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, int language);
    void DrawFullScreen(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, std::string_view hint, int language);
    void DrawMuseum(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcSettingsPage& page, int language);

private:
    struct Piece {
        uint32_t texture = 0;
        std::vector<glm::vec2> positions;
        std::vector<glm::vec2> uvs;
        std::vector<uint32_t> indices;
    };
    struct Photo {
        bool loaded = false;
        std::vector<Piece> pieces;
        glm::vec2 lo{0.0f};
        glm::vec2 hi{0.0f};
    };

    const Photo& LoadPhoto(UiAssets& assets, const std::string& name);
    bool LoadPiece(UiAssets& assets, const std::string& model, const glm::mat4& to_plane, Piece& out);
    bool DrawPicture(ui::UiBatch& batch, UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, float gain = 1.0f);
    bool FitPicture(UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, glm::vec2& out_lo, glm::vec2& out_hi);
    void DrawFrame(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, std::string_view name, glm::vec2 lo,
                   glm::vec2 hi, float inset, bool selected, bool locked, int language);
    void DrawLine(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, std::string_view text, float size, glm::vec2 lo, glm::vec2 hi,
                  ui::TextAlign align, float alpha, int language);
    void DrawLines(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, int language);
    glm::vec2 TextureSize(UiAssets& assets, const std::string& path);

    std::map<std::string, Photo> photos_;
    std::map<std::string, glm::vec2> sizes_;
};

}
