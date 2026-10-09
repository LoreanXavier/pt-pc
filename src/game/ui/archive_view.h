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

// The pictures and texts of the PC page's panel (PcPanel): the loop browser's previews, the Archive's textures, subliminal strings,
// photo pieces and transcripts, in the panel right of a browser page's rows or over the whole screen. Everything is read from the
// game's data when first shown (UiAssets textures, the pieces' .fmdl and the hallway's data set for the frame)
class ArchiveView {
public:
    // the panel inside lo..hi (virtual pixels of the 1280 x 720 canvas)
    void DrawPanel(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, int language);
    // the panel's picture over the whole screen, with its title and the hint line
    void DrawFullScreen(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, std::string_view hint, int language);
    // the Museum's page (museum_layout.h): the halls' doorways, or a hall's wall with the cursor's exhibit in the spotlight
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
    // the picture fitted into lo..hi (target pixels); false when there is none
    // `gain` lifts a picture shot in the theater (the Museum's thumbnails, dark as the hallway is)
    bool DrawPicture(ui::UiBatch& batch, UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, float gain = 1.0f);
    // the picture's own shape fitted into lo..hi (target pixels); false when the panel has none
    bool FitPicture(UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, glm::vec2& out_lo, glm::vec2& out_hi);
    // a frame on the wall (canvas pixels): its line, the mat inside, and the picture (its own shape, framed as it is), the
    // exhibit's name when there is no picture yet; `selected` lights the frame, `locked` keeps it empty and dim
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
