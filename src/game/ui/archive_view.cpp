#include "game/ui/archive_view.h"

#include <algorithm>
#include <cstring>

#include "engine/assets/fmdl.h"
#include "engine/assets/ftex.h"
#include "engine/core/log.h"
#include "engine/data/fox2.h"
#include "engine/fs/vfs.h"
#include "engine/render/texture_manager.h"
#include "engine/ui/text_layout.h"
#include "game/stage_data.h"
#include "game/ui/museum_layout.h"
#include "game/ui/pc_settings_page.h"
#include "game/ui/ui_assets.h"
#include "game/ui/uif_view.h"

namespace pt::game {
namespace {

// the hallway's data set, where the frame on the wall holds the photo pieces (gameplay.md 6.4)
constexpr const char* kHallwayData = "/Assets/sh/level/promotion/pt_2014/hallway/pt14_hallway.fpkd";
constexpr std::string_view kNazoGroup = "pt14_hallway_nazo|shsb_labl001_mapc";

glm::vec2 Fit(glm::vec2 size, glm::vec2 lo, glm::vec2 hi, glm::vec2& origin) {
    const glm::vec2 room = hi - lo;
    const float scale = size.x > 0.0f && size.y > 0.0f ? std::min(room.x / size.x, room.y / size.y) : 1.0f;
    const glm::vec2 shown = size * scale;
    origin = lo + (room - shown) * 0.5f;
    return shown;
}

ui::TextStyle PanelStyle(const UiFont& font, float size) {
    ui::TextStyle style;
    style.font = &font.font;
    style.font_width = size;
    style.font_height = size;
    style.text_space = 0.0f;
    style.line_space = size * 0.25f;
    return style;
}

}

glm::vec2 ArchiveView::TextureSize(UiAssets& assets, const std::string& path) {
    if (auto it = sizes_.find(path); it != sizes_.end()) return it->second;
    glm::vec2 size(16.0f, 9.0f);
    if (auto header = ReadTextureFile(assets.Files().Textures(), FtexStem(path) + ".ftex"); header && header->size() >= 0x0E) {
        uint16_t w = 0;
        uint16_t h = 0;
        std::memcpy(&w, header->data() + 0x0A, 2);
        std::memcpy(&h, header->data() + 0x0C, 2);
        if (w > 0 && h > 0) size = glm::vec2(w, h);
    }
    sizes_[path] = size;
    return size;
}

bool ArchiveView::LoadPiece(UiAssets& assets, const std::string& model, const glm::mat4& to_plane, Piece& out) {
    const auto bytes = assets.Files().ReadFile(model);
    FmdlModel fmdl;
    if (!bytes || !LoadFmdl(*bytes, model, fmdl) || fmdl.mesh.vertices.empty()) {
        LogWarn("archive: photo piece {} unreadable", model);
        return false;
    }
    const std::string base = fmdl.materials.empty() ? std::string() : fmdl.materials.front().base_color;
    out.texture = base.empty() ? TextureManager::kWhite : assets.Texture(base + ".ftex");
    for (const Vertex& v : fmdl.mesh.vertices) {
        const glm::vec4 p = to_plane * glm::vec4(v.position, 1.0f);
        out.positions.emplace_back(p.x, p.y);
        out.uvs.push_back(v.uv0);
    }
    for (const SubMesh& sub : fmdl.mesh.submeshes) {
        for (uint32_t i = 0; i < sub.index_count; ++i) {
            out.indices.push_back(fmdl.mesh.indices[sub.first_index + i] + static_cast<uint32_t>(sub.vertex_offset));
        }
    }
    if (fmdl.mesh.submeshes.empty()) out.indices = fmdl.mesh.indices;
    return true;
}

// a piece alone (its model's own plane), or "complete": the pieces the frame holds, placed as the hallway's data places them, in
// the plane of the frame's first piece
const ArchiveView::Photo& ArchiveView::LoadPhoto(UiAssets& assets, const std::string& name) {
    Photo& photo = photos_[name];
    if (photo.loaded) return photo;
    photo.loaded = true;
    if (name != "complete") {
        Piece piece;
        if (LoadPiece(assets, name, glm::mat4(1.0f), piece)) photo.pieces.push_back(std::move(piece));
    } else if (auto package = assets.Files().LoadPackage(kHallwayData)) {
        std::vector<std::pair<std::string, glm::mat4>> placed;
        for (const auto& entry : package->Entries()) {
            if (!entry.path.ends_with(".fox2")) continue;
            auto file = std::make_shared<fox2::DataSetFile>();
            const auto data = package->Read(entry);
            if (!file->Load(entry.path, data)) continue;
            const auto stage = BuildStageData(file, kHallwayData);
            for (const StaticModelPlacement& m : stage->static_models) {
                // the frame's pieces: the two it starts with and the six the player brings (mapc003 to mapc008, "_frame")
                const size_t at = m.name.find(kNazoGroup);
                if (at == std::string::npos) continue;
                if (m.name.ends_with("_frame") || m.name.ends_with("mapc001_0000") || m.name.ends_with("mapc002_0000")) {
                    placed.emplace_back(m.model_file, m.world);
                    LogInfo("archive: frame piece {} ({})", m.name, m.model_file);
                }
            }
        }
        if (!placed.empty()) {
            const glm::mat4 to_plane = glm::inverse(placed.front().second);
            for (const auto& [model, world] : placed) {
                Piece piece;
                if (LoadPiece(assets, model, to_plane * world, piece)) photo.pieces.push_back(std::move(piece));
            }
        }
        LogInfo("archive: the frame's photo, {} pieces", photo.pieces.size());
    }
    bool first = true;
    for (const Piece& piece : photo.pieces) {
        for (const glm::vec2& p : piece.positions) {
            photo.lo = first ? p : glm::min(photo.lo, p);
            photo.hi = first ? p : glm::max(photo.hi, p);
            first = false;
        }
    }
    return photo;
}

bool ArchiveView::DrawPicture(ui::UiBatch& batch, UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, float gain) {
    if (!panel.photo.empty()) {
        const Photo& photo = LoadPhoto(assets, panel.photo);
        if (photo.pieces.empty()) return false;
        glm::vec2 origin;
        const glm::vec2 extent = photo.hi - photo.lo;
        const glm::vec2 shown = Fit(extent, lo, hi, origin);
        const float scale = extent.x > 0.0f ? shown.x / extent.x : 1.0f;
        for (const Piece& piece : photo.pieces) {
            std::vector<ui::UiVertex> vertices;
            for (const uint32_t i : piece.indices) {
                if (i >= piece.positions.size()) continue;
                const glm::vec2 p = piece.positions[i];
                vertices.push_back({origin + glm::vec2(p.x - photo.lo.x, photo.hi.y - p.y) * scale, piece.uvs[i], glm::vec4(1.0f)});
            }
            batch.Draw(vertices, ui::UiDrawParams::Plain(piece.texture), ui::UiShade::Material, ui::UiBlend::Alpha);
        }
        return true;
    }
    uint32_t texture = 0;
    glm::vec2 size(16.0f, 9.0f);
    if (!panel.texture.empty()) {
        texture = assets.Texture(panel.texture);
        size = TextureSize(assets, panel.texture);
    } else if (!panel.file.empty()) {
        texture = assets.LocalPreview(panel.file);
    } else if (!panel.thumbnail.empty()) {
        texture = assets.LocalPreview(panel.thumbnail);
    }
    if (!texture) return false;
    glm::vec2 origin;
    const glm::vec2 shown = Fit(size, lo, hi, origin);
    if (panel.string) {
        // the subliminal service's string pass draws the texture's alpha in a tenth of the colour (ShadeString), dark text over the
        // picture; here over black, at ten times the colour, so it reads white
        ui::UiDrawParams params = ui::UiDrawParams::Plain(texture);
        params.extra = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        batch.Quad(origin, origin + shown, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(10.0f, 10.0f, 10.0f, 1.0f), params, ui::UiShade::String,
                   ui::UiBlend::Alpha);
    } else {
        const float lift = panel.thumbnail.empty() ? 1.0f : gain;
        batch.Quad(origin, origin + shown, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(lift, lift, lift, 1.0f), ui::UiDrawParams::Plain(texture),
                   ui::UiShade::Material, ui::UiBlend::Alpha);
    }
    return true;
}

bool ArchiveView::FitPicture(UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi, glm::vec2& out_lo, glm::vec2& out_hi) {
    glm::vec2 size(16.0f, 9.0f);
    if (!panel.photo.empty()) {
        const Photo& photo = LoadPhoto(assets, panel.photo);
        if (photo.pieces.empty()) return false;
        size = photo.hi - photo.lo;
    } else if (!panel.texture.empty()) {
        size = TextureSize(assets, panel.texture);
    } else if (panel.file.empty() && panel.thumbnail.empty()) {
        return false;
    }
    glm::vec2 origin;
    const glm::vec2 shown = Fit(size, lo, hi, origin);
    out_lo = origin;
    out_hi = origin + shown;
    return true;
}

void ArchiveView::DrawLine(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, std::string_view text, float size, glm::vec2 lo,
                           glm::vec2 hi, ui::TextAlign align, float alpha, int language) {
    UiFont* font = assets.Font(UiFontType::PcSystem, language);
    if (!font || text.empty()) return;
    ui::TextLayout layout = ui::LayoutText(text, PanelStyle(*font, size), hi.x - lo.x);
    // what does not fit the box keeps its first lines (a long name in a small plaque)
    const int fit = std::max(1, static_cast<int>((hi.y - lo.y) / std::max(layout.line_pitch, 1.0f)));
    if (static_cast<int>(layout.lines.size()) > fit) layout.lines.resize(static_cast<size_t>(fit));
    ui::PlaceText(layout, lo, hi, align, align, ui::TextAlign::Center, true);
    DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, alpha), ui::UiBlend::Alpha);
}

void ArchiveView::DrawFrame(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, std::string_view name, glm::vec2 lo,
                            glm::vec2 hi, float inset, bool selected, bool locked, int language) {
    const auto solid = [&](glm::vec2 a, glm::vec2 b, glm::vec4 color) {
        batch.Quad(a, b, glm::vec2(0.0f), glm::vec2(1.0f), color, ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
    };
    // the picture's own shape inside the cell, or the whole cell when there is none
    glm::vec2 a = canvas.ToTarget(lo + glm::vec2(inset));
    glm::vec2 b = canvas.ToTarget(hi - glm::vec2(inset));
    glm::vec2 pa = a;
    glm::vec2 pb = b;
    const bool picture = !locked && FitPicture(assets, panel, a, b, pa, pb);
    if (picture) {
        a = pa - glm::vec2(inset * canvas.scale);
        b = pb + glm::vec2(inset * canvas.scale);
    }
    const float line = std::max(1.0f, std::round((selected ? 2.0f : 1.0f) * canvas.scale));
    const float edge = selected ? 0.95f : locked ? 0.16f : 0.38f;
    // the mat: black behind a picture (a string reads white on it), the wall's dark behind an empty frame
    solid(a, b, glm::vec4(0.0f, 0.0f, 0.0f, picture ? 0.9f : 0.55f));
    solid({a.x - line, a.y - line}, {b.x + line, a.y}, glm::vec4(1.0f, 1.0f, 1.0f, edge));
    solid({a.x - line, b.y}, {b.x + line, b.y + line}, glm::vec4(1.0f, 1.0f, 1.0f, edge));
    solid({a.x - line, a.y}, {a.x, b.y}, glm::vec4(1.0f, 1.0f, 1.0f, edge));
    solid({b.x, a.y}, {b.x + line, b.y}, glm::vec4(1.0f, 1.0f, 1.0f, edge));
    // a theater shot is lit as an exhibit is, the hallway's dark lifted; one not decoded yet leaves the name in the frame
    const bool drawn = picture && DrawPicture(batch, assets, panel, pa, pb, panel.lift);
    if (drawn) {
        if (!selected) solid(pa, pb, glm::vec4(0.0f, 0.0f, 0.0f, 0.22f));
    } else if (!locked && !name.empty()) {
        // no picture yet (its capture still running, or its file still being decoded): the name in the frame
        DrawLine(batch, canvas, assets, name, 15.0f, lo + glm::vec2(inset + 8.0f), hi - glm::vec2(inset + 8.0f), ui::TextAlign::Center,
                 selected ? 0.8f : 0.45f, language);
    }
}

void ArchiveView::DrawMuseum(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcSettingsPage& page, int language) {
    const int n = page.RowCount();
    const int cells = page.CellCount();
    const int cursor = page.Cursor();
    using L = MuseumLayout;
    if (page.Gallery() == PcGallery::Halls) {
        for (int i = 0; i < n; ++i) {
            const PcSettingRow* row = page.RowAtIndex(i);
            if (!row) continue;
            const bool selected = i == cursor;
            if (i < cells) {
                const L::Cell picture = L::HallPicture(i);
                const PcPanel panel = page.PanelOf(i);
                DrawFrame(batch, canvas, assets, panel, PcText(row->label, language), picture.lo, picture.hi, 4.0f, selected, !row->enabled || panel.locked,
                          language);
                // the plaque: the hall's name, and how many of its exhibits are open
                const L::Cell cell = L::HallCell(i);
                const float alpha = row->enabled ? (selected ? 1.0f : 0.7f) : 0.35f;
                DrawLine(batch, canvas, assets, PcText(row->label, language), 17.0f, {cell.lo.x + 4.0f, picture.hi.y + 4.0f},
                         {cell.hi.x - 4.0f, picture.hi.y + 26.0f}, ui::TextAlign::Center, alpha, language);
                if (!row->values.empty()) {
                    DrawLine(batch, canvas, assets, row->values.front(), 13.0f, {cell.lo.x + 4.0f, picture.hi.y + 26.0f},
                             {cell.hi.x - 4.0f, picture.hi.y + 42.0f}, ui::TextAlign::Center, alpha * 0.55f, language);
                }
            } else {
                const L::Cell cell = L::HallText(i - cells);
                std::string_view label = row->label;
                if (row->action && selected && page.Confirming()) label = "pc_reset_confirm";
                DrawLine(batch, canvas, assets, PcText(label, language), 17.0f, cell.lo, cell.hi, ui::TextAlign::Center,
                         row->enabled ? (selected ? 1.0f : 0.55f) : 0.3f, language);
            }
        }
        return;
    }
    // the wall: the strip (a short one centred on its own frames), then the spotlight and its plaque
    const int slots = std::min(n, page.WallVisible());
    const int visible = std::min(n - page.First(), slots);
    for (int slot = 0; slot < visible; ++slot) {
        const int index = page.First() + slot;
        const PcSettingRow* row = page.RowAtIndex(index);
        if (!row) continue;
        L::Cell cell = L::WallCell(slot, slots);
        const bool selected = index == cursor;
        if (selected) {
            cell.lo -= glm::vec2(3.0f);
            cell.hi += glm::vec2(3.0f);
        }
        const PcPanel panel = page.PanelOf(index);
        DrawFrame(batch, canvas, assets, panel, row->label, cell.lo, cell.hi, 3.0f, selected, !row->enabled || panel.locked, language);
    }
    // the back button keeps the last exhibit in the spotlight
    const int shown = std::min(cursor, n - 1);
    const PcSettingRow* current = page.RowAtIndex(shown);
    if (!current) return;
    const PcPanel panel = page.PanelOf(shown);
    const bool locked = !current->enabled || panel.locked;
    if (!panel.lines.empty() && !locked) {
        // a voice: its transcript on a sheet in the frame, the spoken line lit
        const glm::vec2 a = canvas.ToTarget(L::kSpotlight.lo);
        const glm::vec2 b = canvas.ToTarget(L::kSpotlight.hi);
        PcPanel sheet;
        DrawFrame(batch, canvas, assets, sheet, {}, L::kSpotlight.lo, L::kSpotlight.hi, 0.0f, true, false, language);
        batch.Quad(a, b, glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.5f), ui::UiDrawParams::Plain(TextureManager::kWhite),
                   ui::UiShade::Solid, ui::UiBlend::Alpha);
        DrawLines(batch, canvas, assets, panel, L::kSpotlight.lo + glm::vec2(22.0f, 18.0f), L::kSpotlight.hi - glm::vec2(22.0f, 14.0f), language);
    } else {
        DrawFrame(batch, canvas, assets, panel, current->label, L::kSpotlight.lo, L::kSpotlight.hi, 8.0f, true, locked, language);
    }
    DrawLine(batch, canvas, assets, current->label, 21.0f, {200.0f, L::kPlaqueTitleY}, {1080.0f, L::kPlaqueTitleY + 26.0f}, ui::TextAlign::Center,
             locked ? 0.5f : 0.95f, language);
    if (!locked && !panel.caption.empty()) {
        DrawLine(batch, canvas, assets, panel.caption, 14.0f, {200.0f, L::kPlaqueCaptionY}, {1080.0f, L::kPlaqueCaptionY + 20.0f}, ui::TextAlign::Center,
                 0.42f, language);
    }
}

void ArchiveView::DrawLines(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi,
                            int language) {
    UiFont* font = assets.Font(UiFontType::PcSystem, language);
    if (!font || panel.lines.empty()) return;
    const ui::TextStyle style = PanelStyle(*font, 19.0f);
    const float width = hi.x - lo.x;
    std::vector<ui::TextLayout> layouts;
    std::vector<float> heights;
    for (const std::string& line : panel.lines) {
        layouts.push_back(ui::LayoutText(line, style, width));
        heights.push_back(std::max(1, static_cast<int>(layouts.back().lines.size())) * layouts.back().line_pitch + 8.0f);
    }
    // the spoken line stays in view: the window starts so that it and the lines before it fit
    size_t first = 0;
    const int current = std::clamp(panel.current_line, 0, static_cast<int>(panel.lines.size()) - 1);
    float above = 0.0f;
    for (int i = current; i >= 0; --i) {
        above += heights[i];
        if (above > (hi.y - lo.y) * 0.6f) {
            first = static_cast<size_t>(i + 1);
            break;
        }
    }
    first = std::min(first, static_cast<size_t>(current));
    float y = lo.y;
    for (size_t i = first; i < layouts.size() && y + heights[i] - 8.0f <= hi.y; ++i) {
        ui::PlaceText(layouts[i], {lo.x, y}, {hi.x, y + heights[i]}, ui::TextAlign::Start, ui::TextAlign::Start, ui::TextAlign::Start, true);
        const bool spoken = panel.current_line < 0 || static_cast<int>(i) == panel.current_line;
        DrawText(batch, canvas, *font, layouts[i], glm::vec4(1.0f, 1.0f, 1.0f, spoken ? 1.0f : 0.45f), ui::UiBlend::Alpha);
        y += heights[i];
    }
}

void ArchiveView::DrawPanel(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, glm::vec2 lo, glm::vec2 hi,
                            int language) {
    if (panel.Empty()) return;
    const glm::vec2 a = canvas.ToTarget(lo);
    const glm::vec2 b = canvas.ToTarget(hi);
    if (!panel.texture.empty() || !panel.photo.empty()) {
        // a backdrop the pictures read on (a string is drawn white, an overlay sprite is black where it covers)
        const glm::vec4 ground = panel.string ? glm::vec4(0.0f, 0.0f, 0.0f, 0.85f) : glm::vec4(0.16f, 0.16f, 0.16f, 0.9f);
        batch.Quad(a, b, glm::vec2(0.0f), glm::vec2(1.0f), ground, ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid,
                   ui::UiBlend::Alpha);
        const glm::vec2 inset = (b - a) * 0.03f;
        DrawPicture(batch, assets, panel, a + inset, b - inset);
    } else if (!panel.file.empty()) {
        DrawPicture(batch, assets, panel, a, b);
    }
    if (!panel.lines.empty()) {
        DrawLines(batch, canvas, assets, panel, lo, hi, language);
    }
}

void ArchiveView::DrawFullScreen(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcPanel& panel, std::string_view hint,
                                 int language) {
    const glm::vec2 full(static_cast<float>(batch.Extent().width), static_cast<float>(batch.Extent().height));
    const glm::vec4 ground = panel.string ? glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) : glm::vec4(0.08f, 0.08f, 0.08f, 1.0f);
    batch.Quad(glm::vec2(0.0f), full, glm::vec2(0.0f), glm::vec2(1.0f), ground,
               ui::UiDrawParams::Plain(TextureManager::kWhite), ui::UiShade::Solid, ui::UiBlend::Alpha);
    // a subliminal string spans the original's 16:9 frame (GameUi::DrawSubliminal); other pictures keep their own shape
    const glm::vec2 lo = canvas.ToTarget({40.0f, 60.0f});
    const glm::vec2 hi = canvas.ToTarget({UiCanvas::kWidth - 40.0f, UiCanvas::kHeight - 70.0f});
    DrawPicture(batch, assets, panel, panel.string ? canvas.ToTarget({0.0f, 0.0f}) : lo,
                panel.string ? canvas.ToTarget({UiCanvas::kWidth, UiCanvas::kHeight}) : hi);
    UiFont* font = assets.Font(UiFontType::PcSystem, language);
    if (!font) return;
    if (!panel.title.empty()) {
        ui::TextLayout layout = ui::LayoutText(panel.title, PanelStyle(*font, 24.0f), 0.0f);
        ui::PlaceText(layout, {40.0f, 18.0f}, {UiCanvas::kWidth - 40.0f, 50.0f}, ui::TextAlign::Start, ui::TextAlign::Start, ui::TextAlign::Center, true);
        DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, 0.85f), ui::UiBlend::Alpha);
    }
    if (!hint.empty()) {
        ui::TextLayout layout = ui::LayoutText(hint, PanelStyle(*font, 18.0f), UiCanvas::kWidth - 80.0f);
        ui::PlaceText(layout, {40.0f, UiCanvas::kHeight - 50.0f}, {UiCanvas::kWidth - 40.0f, UiCanvas::kHeight - 16.0f}, ui::TextAlign::Center,
                      ui::TextAlign::Center, ui::TextAlign::Center, true);
        DrawText(batch, canvas, *font, layout, glm::vec4(1.0f, 1.0f, 1.0f, 0.6f), ui::UiBlend::Alpha);
    }
}

}
