#include "game/ui/ui_icons.h"

#include <glm/glm.hpp>

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/ftex.h"
#include "engine/core/log.h"
#include "engine/fs/vfs.h"
#include "engine/render/texture_manager.h"
#include "engine/ui/text_layout.h"
#include "engine/ui/ui_batch.h"
#include "game/ui/ui_assets.h"

namespace pt::game {
namespace {

constexpr const char* kAtlasPath = "/Assets/sh/ui/texture/ButtonIcon/cmn_btn_icon_a_ps4_alp";
constexpr const char* kGlowPath = "/Assets/sh/ui/ModelAsset/sys_option/Pictures/cmn_btn_icon_a_ps4_blr";
constexpr int kCell = 128;
constexpr int kSquareCellY = 128;

struct Image {
    int width = 0;
    int height = 0;
    bool srgb = false;
    std::vector<uint8_t> rgba;

    uint8_t* At(int x, int y) { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
};

bool Decode(const FtexTexture& ftex, Image& image) {
    image.width = static_cast<int>(ftex.width);
    image.height = static_cast<int>(ftex.height);
    image.srgb = ftex.Srgb();
    return DecodeFtexLevel(ftex, 0, image.rgba);
}

float SegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float t = std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

// 128 px cells: white disc r 26 at (64, 64), shadow outside it, symbol black at alpha 226 with 4 px strokes
void DrawGlyph(const Image& source, Image& out, int cell, bool circle) {
    constexpr float kClear = 24.5f;
    constexpr float kHalfStroke = 2.0f;
    constexpr float kCrossArm = 10.5f;
    constexpr float kRing = 11.5f;
    for (int y = 0; y < kCell; ++y) {
        for (int x = 0; x < kCell; ++x) {
            const glm::vec2 p(static_cast<float>(x) + 0.5f - 64.0f, static_cast<float>(y) + 0.5f - 64.0f);
            const uint8_t* s = &source.rgba[(static_cast<size_t>(kSquareCellY + y) * source.width + x) * 4];
            float rgb = s[0];
            float alpha = s[3];
            if (glm::length(p) < kClear) {
                rgb = 255.0f;
                alpha = 255.0f;
            }
            const float d = circle ? std::abs(glm::length(p) - kRing)
                                   : std::min(SegmentDistance(p, glm::vec2(-kCrossArm), glm::vec2(kCrossArm)),
                                              SegmentDistance(p, glm::vec2(-kCrossArm, kCrossArm), glm::vec2(kCrossArm, -kCrossArm)));
            const float cover = std::clamp(kHalfStroke + 0.5f - d, 0.0f, 1.0f);
            rgb *= 1.0f - cover;
            alpha = alpha * (1.0f - cover) + 226.0f * cover;
            uint8_t* o = out.At(cell * kCell + x, y);
            o[0] = o[1] = o[2] = static_cast<uint8_t>(std::lround(rgb));
            o[3] = static_cast<uint8_t>(std::lround(alpha));
        }
    }
}

std::vector<std::vector<uint8_t>> BuildMips(const Image& image) {
    std::vector<std::vector<uint8_t>> mips{image.rgba};
    int w = image.width;
    int h = image.height;
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2);
        const int nh = std::max(1, h / 2);
        const std::vector<uint8_t>& src = mips.back();
        std::vector<uint8_t> dst(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const int x0 = std::min(2 * x, w - 1);
                    const int x1 = std::min(2 * x + 1, w - 1);
                    const int y0 = std::min(2 * y, h - 1);
                    const int y1 = std::min(2 * y + 1, h - 1);
                    const int sum = src[(static_cast<size_t>(y0) * w + x0) * 4 + c] + src[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                                    src[(static_cast<size_t>(y1) * w + x0) * 4 + c] + src[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                    dst[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        mips.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return mips;
}

// Generated prompts are drawn in the atlas's cell units (128 per cell height) at twice its resolution, so they stay sharp where the
// option screen is drawn larger than the atlas (a 5 unit icon is 150 pixels high at 2160p)
constexpr int kGlyphScale = 2;
// Edge profile: the square cell's white disc with its shadow and glow, by radius; inside kProfileFrom lies the symbol
constexpr float kProfileFrom = 21.0f;
constexpr float kProfileTo = 48.0f;
constexpr float kProfileStep = 0.25f;
constexpr glm::vec2 kCellCentre{64.0f, 64.0f};
// The D-pad glyph's unused arms (rgb 75 and alpha 188 in the atlas's D-pad cells) mark the arrow keys a prompt does not use
constexpr float kDimLevel = 75.0f / 255.0f;
constexpr float kDimAlpha = 188.0f / 255.0f;
// Symbols: the atlas's strokes are 4 pixels wide; letters stand as tall as its triangle and square, their outlines grown (bold) towards
// the strokes' weight, and keycaps carry their names smaller
constexpr float kHalfStroke = 2.0f;
constexpr float kLetterCap = 22.0f;
constexpr float kLetterBold = 0.9f;
constexpr float kKeyCap = 16.0f;
constexpr float kKeyCapSingle = 19.0f;
constexpr float kKeyBold = 0.5f;
constexpr float kKeyPad = 12.0f;
constexpr float kKeyCorner = 9.0f;
// font glyph distance fields: padding around the glyph box and the farthest distance searched, font pixels
constexpr int kFieldPad = 4;
constexpr int kFieldReach = 6;

struct EdgeProfile {
    float radius = 25.5f;
    float ink = 226.0f / 255.0f;
    std::vector<float> cover;
    std::vector<float> shadow;
    std::vector<float> glow;

    // by signed distance from a body's edge in cell pixels (negative inside)
    float Sample(const std::vector<float>& table, float distance, float inside) const {
        const float r = radius + distance;
        if (r < kProfileFrom || table.empty()) {
            return inside;
        }
        const float f = (r - kProfileFrom) / kProfileStep - 0.5f;
        if (f <= 0.0f) {
            return table.front();
        }
        const size_t i = static_cast<size_t>(f);
        if (i + 1 >= table.size()) {
            return 0.0f;
        }
        const float t = f - static_cast<float>(i);
        return table[i] + (table[i + 1] - table[i]) * t;
    }
};

// The square cell (a white disc of radius ~26 around the cell centre, a black shadow around it, the symbol inside) and the glow
// under it, averaged over rings: the disc's coverage c (its colour is white over the black shadow: a = c + (1 - c) s and rgb = c / a),
// the shadow's alpha s and the glow; the symbol's alpha is the ink of generated symbols
EdgeProfile MeasureEdge(const Image& atlas, const Image& glow) {
    EdgeProfile edge;
    const size_t bins = static_cast<size_t>((kProfileTo - kProfileFrom) / kProfileStep);
    std::vector<double> sum_a(bins), sum_c(bins), sum_g(bins), count(bins);
    double ink = 0.0;
    double ink_count = 0.0;
    for (int y = 0; y < kCell; ++y) {
        for (int x = 0; x < kCell; ++x) {
            const float r = glm::length(glm::vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) - kCellCentre);
            const uint8_t* a = &atlas.rgba[(static_cast<size_t>(kSquareCellY + y) * atlas.width + x) * 4];
            const uint8_t* g = &glow.rgba[(static_cast<size_t>(kSquareCellY + y) * glow.width + x) * 4];
            const double alpha = a[3] / 255.0;
            const double rgb = a[0] / 255.0;
            if (r < 16.0f && rgb < 0.1 && alpha > 0.5) {
                ink += alpha;
                ink_count += 1.0;
            }
            if (r < kProfileFrom || r >= kProfileTo) {
                continue;
            }
            const size_t k = std::min(bins - 1, static_cast<size_t>((r - kProfileFrom) / kProfileStep));
            sum_a[k] += alpha;
            sum_c[k] += alpha * rgb;
            sum_g[k] += g[0] / 255.0;
            count[k] += 1.0;
        }
    }
    edge.cover.resize(bins);
    edge.shadow.resize(bins);
    edge.glow.resize(bins);
    for (size_t k = 0; k < bins; ++k) {
        const size_t from = count[k] > 0.0 ? k : (k > 0 ? k - 1 : k);
        const double n = std::max(1.0, count[from]);
        const double a = sum_a[from] / n;
        const double c = sum_c[from] / n;
        edge.cover[k] = static_cast<float>(c);
        edge.shadow[k] = c < 0.999 ? static_cast<float>(std::clamp((a - c) / (1.0 - c), 0.0, 1.0)) : 0.0f;
        edge.glow[k] = static_cast<float>(sum_g[from] / n);
    }
    for (size_t k = 0; k + 1 < bins; ++k) {
        if (edge.cover[k] >= 0.5f && edge.cover[k + 1] < 0.5f) {
            const float t = (edge.cover[k] - 0.5f) / std::max(1e-4f, edge.cover[k] - edge.cover[k + 1]);
            edge.radius = kProfileFrom + (static_cast<float>(k) + 0.5f + t) * kProfileStep;
            break;
        }
    }
    if (ink_count > 0.0) {
        edge.ink = static_cast<float>(ink / ink_count);
    }
    return edge;
}

struct GlyphField {
    int width = 0;
    int height = 0;
    std::vector<float> distance;

    // x, y in font pixels from the glyph box's top left
    float Sample(float x, float y) const {
        const float gx = x + static_cast<float>(kFieldPad) - 0.5f;
        const float gy = y + static_cast<float>(kFieldPad) - 0.5f;
        if (gx < 0.0f || gy < 0.0f || gx > static_cast<float>(width - 1) || gy > static_cast<float>(height - 1)) {
            return static_cast<float>(kFieldReach);
        }
        const int x0 = std::min(width - 2, static_cast<int>(gx));
        const int y0 = std::min(height - 2, static_cast<int>(gy));
        const float tx = gx - static_cast<float>(x0);
        const float ty = gy - static_cast<float>(y0);
        auto at = [&](int ix, int iy) { return distance[static_cast<size_t>(iy) * width + ix]; };
        const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
        const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
        return top + (bottom - top) * ty;
    }
};

// Signed distance from each pixel centre of a glyph's bit plane to the nearest pixel of the other state, minus half a pixel (the edge
// between them), negative inside: sampled between pixels it gives the font's 53 pixel em outlines smooth edges at any size
GlyphField BuildField(const ui::FfntFont& font, const ui::FfntGlyph& glyph) {
    GlyphField field;
    field.width = glyph.width + 2 * kFieldPad;
    field.height = glyph.height + 2 * kFieldPad;
    field.distance.assign(static_cast<size_t>(field.width) * field.height, static_cast<float>(kFieldReach));
    auto set = [&](int fx, int fy) { return font.Bit(glyph, fx - kFieldPad, fy - kFieldPad); };
    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            const bool inside = set(x, y);
            float best = static_cast<float>(kFieldReach);
            for (int dy = -kFieldReach; dy <= kFieldReach; ++dy) {
                for (int dx = -kFieldReach; dx <= kFieldReach; ++dx) {
                    const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                    if (d < best && set(x + dx, y + dy) != inside) {
                        best = d;
                    }
                }
            }
            const float edge = std::max(0.0f, best - 0.5f);
            field.distance[static_cast<size_t>(y) * field.width + x] = inside ? -edge : edge;
        }
    }
    return field;
}

float BoxDistance(glm::vec2 p, glm::vec2 centre, glm::vec2 half, float corner) {
    const glm::vec2 q = glm::abs(p - centre) - half + glm::vec2(corner);
    return glm::length(glm::max(q, glm::vec2(0.0f))) + std::min(std::max(q.x, q.y), 0.0f) - corner;
}

float Cross2(glm::vec2 a, glm::vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// signed distance to a triangle, negative inside
float TriangleDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    const glm::vec2 e0 = b - a, e1 = c - b, e2 = a - c;
    const glm::vec2 v0 = p - a, v1 = p - b, v2 = p - c;
    const glm::vec2 pq0 = v0 - e0 * std::clamp(glm::dot(v0, e0) / glm::dot(e0, e0), 0.0f, 1.0f);
    const glm::vec2 pq1 = v1 - e1 * std::clamp(glm::dot(v1, e1) / glm::dot(e1, e1), 0.0f, 1.0f);
    const glm::vec2 pq2 = v2 - e2 * std::clamp(glm::dot(v2, e2) / glm::dot(e2, e2), 0.0f, 1.0f);
    const float s = Cross2(e0, e2) > 0.0f ? 1.0f : -1.0f;
    const glm::vec2 d = glm::min(glm::min(glm::vec2(glm::dot(pq0, pq0), s * Cross2(v0, e0)), glm::vec2(glm::dot(pq1, pq1), s * Cross2(v1, e1))),
                                 glm::vec2(glm::dot(pq2, pq2), s * Cross2(v2, e2)));
    return -std::sqrt(d.x) * (d.y > 0.0f ? 1.0f : -1.0f);
}

// coverage of a line of the symbols' width, and of a filled shape, from their distances in cell pixels
float StrokeInk(float distance) {
    return std::clamp(kHalfStroke + 0.5f - distance, 0.0f, 1.0f);
}

float FillInk(float distance) {
    return std::clamp(0.5f - distance, 0.0f, 1.0f);
}

// What a generated glyph shows at a point (cell pixels from its top left): the distance to its bodies (the disc, keys or mouse; their
// edge, shadow and glow follow the atlas's disc), the body's grey level and alpha there, and the black symbol's coverage
struct Paint {
    float body = 1e9f;
    float level = 1.0f;
    float alpha = 1.0f;
    float ink = 0.0f;
};

// Text in the game's system font, black: its glyphs' distance fields placed so that the ink is centred on a point, horizontally by
// its extent and vertically by the capital height (the H), `cap` cell pixels high
class TextInk {
public:
    TextInk(const ui::FfntFont& font, std::map<uint32_t, GlyphField>& fields, std::string_view text, float cap, float bold) : bold_(bold) {
        const ui::FfntGlyph* h = font.Find('H');
        const float em = static_cast<float>(std::max(1, font.EmSize()));
        const float cap_pixels = h ? static_cast<float>(h->height) : em * 0.7f;
        ui::TextStyle style;
        style.font = &font;
        style.font_width = style.font_height = cap * em / cap_pixels;
        scale_ = style.font_height / em;
        const ui::TextLayout layout = ui::LayoutText(text, style, 0.0f);
        cap_top_ = h ? style.font_height * (static_cast<float>(font.Pad()) + h->top) / em : 0.0f;
        glm::vec2 lo(1e9f);
        glm::vec2 hi(-1e9f);
        for (const ui::TextLine& line : layout.lines) {
            for (const ui::LaidGlyph& g : line.glyphs) {
                if (!g.glyph) {
                    continue;
                }
                auto it = fields.find(g.glyph->code);
                if (it == fields.end()) {
                    it = fields.emplace(g.glyph->code, BuildField(font, *g.glyph)).first;
                }
                glyphs_.push_back({&it->second, g.position});
                for (int y = 0; y < g.glyph->height; ++y) {
                    for (int x = 0; x < g.glyph->width; ++x) {
                        if (font.Bit(*g.glyph, x, y)) {
                            lo = glm::min(lo, g.position + glm::vec2(static_cast<float>(x), static_cast<float>(y)) * scale_);
                            hi = glm::max(hi, g.position + glm::vec2(static_cast<float>(x + 1), static_cast<float>(y + 1)) * scale_);
                        }
                    }
                }
            }
        }
        width_ = hi.x > lo.x ? hi.x - lo.x : 0.0f;
        ink_left_ = lo.x;
        cap_ = cap;
    }

    float Width() const { return width_; }

    void Centre(glm::vec2 centre) { origin_ = glm::vec2(centre.x - width_ * 0.5f - ink_left_, centre.y - cap_ * 0.5f - cap_top_); }

    float Ink(glm::vec2 p) const {
        float ink = 0.0f;
        for (const Placed& g : glyphs_) {
            const glm::vec2 local = (p - origin_ - g.position) / scale_;
            const float d = g.field->Sample(local.x, local.y) * scale_;
            ink = std::max(ink, FillInk(d - bold_));
        }
        return ink;
    }

private:
    struct Placed {
        const GlyphField* field;
        glm::vec2 position;
    };
    std::vector<Placed> glyphs_;
    glm::vec2 origin_{0.0f};
    float scale_ = 1.0f;
    float width_ = 0.0f;
    float ink_left_ = 0.0f;
    float cap_top_ = 0.0f;
    float cap_ = 0.0f;
    float bold_ = 0.0f;
};

// Draws a glyph cell `width` cell pixels wide (128 high) at kGlyphScale: the picture as the atlas's cells are (the body white or grey
// over its shadow, the ink black at the symbols' alpha) and its glow as the glow texture's cells are (grey levels, opaque)
void Rasterize(const EdgeProfile& edge, float width, const std::function<Paint(glm::vec2)>& paint, Image& icon, Image& glow) {
    const int w = static_cast<int>(std::lround(width)) * kGlyphScale;
    const int h = kCell * kGlyphScale;
    icon = Image{w, h, icon.srgb, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
    glow = Image{w, h, glow.srgb, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const glm::vec2 p((static_cast<float>(x) + 0.5f) / kGlyphScale, (static_cast<float>(y) + 0.5f) / kGlyphScale);
            const Paint s = paint(p);
            const float cover = edge.Sample(edge.cover, s.body, 1.0f);
            const float shadow = edge.Sample(edge.shadow, s.body, 0.0f);
            const float body_alpha = s.alpha * (1.0f - s.ink) + edge.ink * s.ink;
            const float body_colour = s.level * s.alpha * (1.0f - s.ink);
            const float alpha = cover * body_alpha + (1.0f - cover) * shadow;
            const float colour = alpha > 1e-4f ? cover * body_colour / alpha : 0.0f;
            uint8_t* o = icon.At(x, y);
            o[0] = o[1] = o[2] = static_cast<uint8_t>(std::lround(std::clamp(colour, 0.0f, 1.0f) * 255.0f));
            o[3] = static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
            uint8_t* g = glow.At(x, y);
            g[0] = g[1] = g[2] = static_cast<uint8_t>(std::lround(std::clamp(edge.Sample(edge.glow, s.body, 1.0f), 0.0f, 1.0f) * 255.0f));
            g[3] = 255;
        }
    }
}

bool IsArrow(uint16_t scancode) {
    return scancode == SDL_SCANCODE_UP || scancode == SDL_SCANCODE_DOWN || scancode == SDL_SCANCODE_LEFT || scancode == SDL_SCANCODE_RIGHT;
}

uint8_t ArrowBit(uint16_t scancode) {
    return scancode == SDL_SCANCODE_UP ? 1 : scancode == SDL_SCANCODE_DOWN ? 2 : scancode == SDL_SCANCODE_LEFT ? 4 : 8;
}

}

struct UiAssets::PromptArt {
    EdgeProfile edge;
    bool icon_srgb = false;
    bool glow_srgb = false;
    std::map<uint32_t, GlyphField> fields;
    std::map<std::string, PromptGlyph> glyphs;
};

uint32_t UiAssets::UploadImage(const std::string& name, int width, int height, bool srgb, const std::vector<uint8_t>& rgba) {
    const Image image{width, height, srgb, rgba};
    const std::vector<std::vector<uint8_t>> levels = BuildMips(image);
    std::vector<TextureMip> mips;
    int w = width;
    int h = height;
    for (const auto& level : levels) {
        mips.push_back({static_cast<uint32_t>(w), static_cast<uint32_t>(h), level});
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    const uint32_t index = textures_->Create(name, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, mips);
    texture_cache_[name] = index;
    texture_address_[index] = ui::kUiClampU | ui::kUiClampV;
    return index;
}

bool UiAssets::BuildPcIcons() {
    if (pc_icons_built_) {
        return pc_icons_ready_;
    }
    pc_icons_built_ = true;
    FtexTexture atlas_ftex;
    FtexTexture glow_ftex;
    Image atlas;
    Image glow;
    if (!LoadFtex(vfs_->Textures(), kAtlasPath, atlas_ftex) || !Decode(atlas_ftex, atlas) || !LoadFtex(vfs_->Textures(), kGlowPath, glow_ftex) ||
        !Decode(glow_ftex, glow) || atlas.width < kCell || atlas.height < 2 * kCell || glow.width < kCell || glow.height < 2 * kCell) {
        LogWarn("ui: PC page button icons failed (formats {} and {})", atlas_ftex.pixel_format, glow_ftex.pixel_format);
        return false;
    }
    Image icons{2 * kCell, kCell, atlas.srgb, std::vector<uint8_t>(static_cast<size_t>(2 * kCell) * kCell * 4)};
    DrawGlyph(atlas, icons, 0, false);
    DrawGlyph(atlas, icons, 1, true);
    Image halos{2 * kCell, kCell, glow.srgb, std::vector<uint8_t>(static_cast<size_t>(2 * kCell) * kCell * 4)};
    for (int cell = 0; cell < 2; ++cell) {
        for (int y = 0; y < kCell; ++y) {
            std::copy_n(&glow.rgba[(static_cast<size_t>(kSquareCellY + y) * glow.width) * 4], kCell * 4, halos.At(cell * kCell, y));
        }
    }
    UploadImage(kPcIconAtlas, icons.width, icons.height, icons.srgb, icons.rgba);
    UploadImage(kPcIconGlow, halos.width, halos.height, halos.srgb, halos.rgba);
    auto art = std::make_shared<PromptArt>();
    art->edge = MeasureEdge(atlas, glow);
    art->icon_srgb = atlas.srgb;
    art->glow_srgb = glow.srgb;
    prompt_art_ = std::move(art);
    pc_icons_ready_ = true;
    LogInfo("ui: PC page button icons from {} (format {}), glow format {}, prompt edge disc {:.2f} ink {:.3f}", kAtlasPath,
            atlas_ftex.pixel_format, glow_ftex.pixel_format, prompt_art_->edge.radius, prompt_art_->edge.ink);
    return true;
}

// Generated glyphs by id: "letter:A" (a letter on the disc), "menu" (the Xbox menu button's three lines on the disc), "plus", "key:Esc"
// (a keycap with a key's name), "arrows:<mask>" (the arrow keys, those of the mask (1 up, 2 down, 4 left, 8 right) lit and the others
// dim as the D-pad glyph's unused arms), "mouse:<button>" (a mouse with that button (1 left, 2 middle, 3 right) filled)
const PromptGlyph& UiAssets::GeneratedGlyph(const std::string& id) {
    PromptArt& art = *prompt_art_;
    if (auto it = art.glyphs.find(id); it != art.glyphs.end()) {
        return it->second;
    }
    const EdgeProfile& edge = art.edge;
    const UiFont* font = Font(UiFontType::System, 0);
    const float r = edge.radius;
    float width = static_cast<float>(kCell);
    glm::vec2 body(2.0f * r);
    std::function<Paint(glm::vec2)> paint;
    std::optional<TextInk> text;
    auto disc = [&](glm::vec2 p) { return glm::length(p - kCellCentre) - r; };
    if (id.starts_with("letter:") && font) {
        text.emplace(font->font, art.fields, std::string_view(id).substr(7), kLetterCap, kLetterBold);
        text->Centre(kCellCentre);
        paint = [&](glm::vec2 p) { return Paint{disc(p), 1.0f, 1.0f, text->Ink(p)}; };
    } else if (id == "menu") {
        paint = [&](glm::vec2 p) {
            float ink = 0.0f;
            for (const float dy : {-7.0f, 0.0f, 7.0f}) {
                ink = std::max(ink, StrokeInk(SegmentDistance(p, kCellCentre + glm::vec2(-9.0f, dy), kCellCentre + glm::vec2(9.0f, dy))));
            }
            return Paint{disc(p), 1.0f, 1.0f, ink};
        };
    } else if (id == "plus") {
        paint = [&](glm::vec2 p) {
            const float d = std::min(SegmentDistance(p, kCellCentre - glm::vec2(10.0f, 0.0f), kCellCentre + glm::vec2(10.0f, 0.0f)),
                                     SegmentDistance(p, kCellCentre - glm::vec2(0.0f, 10.0f), kCellCentre + glm::vec2(0.0f, 10.0f)));
            return Paint{disc(p), 1.0f, 1.0f, StrokeInk(d)};
        };
    } else if (id.starts_with("key:") && font) {
        const std::string_view name = std::string_view(id).substr(4);
        const bool single = ui::DecodeUtf8(name).size() == 1;
        text.emplace(font->font, art.fields, name, single ? kKeyCapSingle : kKeyCap, kKeyBold);
        body = glm::vec2(std::max(2.0f * r, text->Width() + 2.0f * kKeyPad), 2.0f * r);
        width = std::ceil(body.x + 2.0f * (kCellCentre.x - r));
        const glm::vec2 centre(width * 0.5f, kCellCentre.y);
        text->Centre(centre);
        paint = [&, centre](glm::vec2 p) { return Paint{BoxDistance(p, centre, body * 0.5f, kKeyCorner), 1.0f, 1.0f, text->Ink(p)}; };
    } else if (id.starts_with("steam:face:") && font) {
        const std::string_view name = std::string_view(id).substr(11);
        text.emplace(font->font, art.fields, name, kLetterCap, kLetterBold);
        text->Centre(kCellCentre);
        paint = [&, centre = kCellCentre](glm::vec2 p) {
            return Paint{BoxDistance(p, centre, glm::vec2(r * 0.78f), r * 0.24f), 1.0f, 1.0f, text->Ink(p)};
        };
    } else if (id == "steam:menu") {
        const glm::vec2 half(r * 0.9f, r * 0.72f);
        body = half * 2.0f;
        paint = [&, half](glm::vec2 p) {
            float ink = 0.0f;
            for (const float dy : {-0.25f * r, 0.0f, 0.25f * r}) {
                ink = std::max(ink, StrokeInk(SegmentDistance(p, kCellCentre + glm::vec2(-0.42f * r, dy),
                                                                   kCellCentre + glm::vec2(0.42f * r, dy))));
            }
            return Paint{BoxDistance(p, kCellCentre, half, 0.2f * r), 1.0f, 1.0f, ink};
        };
    } else if ((id.starts_with("bumper:") || id.starts_with("steam:bumper:")) && font) {
        // a shoulder button: a flat pill with its name, wider than a face button's disc
        const size_t prefix = id.starts_with("steam:bumper:") ? 13 : 7;
        const std::string_view name = std::string_view(id).substr(prefix);
        text.emplace(font->font, art.fields, name, kKeyCap, kKeyBold);
        body = glm::vec2(std::max(2.6f * r, text->Width() + 2.0f * kKeyPad), 1.56f * r);
        width = std::ceil(body.x + 2.0f * (kCellCentre.x - r));
        const glm::vec2 centre(width * 0.5f, kCellCentre.y);
        text->Centre(centre);
        paint = [&, centre](glm::vec2 p) { return Paint{BoxDistance(p, centre, body * 0.5f, body.y * 0.5f), 1.0f, 1.0f, text->Ink(p)}; };
    } else if (id.starts_with("arrows:")) {
        const int mask = std::atoi(id.c_str() + 7);
        constexpr float kKey = 21.0f;
        constexpr float kGap = 3.0f;
        constexpr float kStep = kKey + kGap;
        struct Key {
            glm::vec2 centre;
            glm::vec2 direction;
            bool lit;
        };
        const Key keys[4] = {{kCellCentre + glm::vec2(0.0f, -kStep * 0.5f), {0.0f, -1.0f}, (mask & 1) != 0},
                             {kCellCentre + glm::vec2(0.0f, kStep * 0.5f), {0.0f, 1.0f}, (mask & 2) != 0},
                             {kCellCentre + glm::vec2(-kStep, kStep * 0.5f), {-1.0f, 0.0f}, (mask & 4) != 0},
                             {kCellCentre + glm::vec2(kStep, kStep * 0.5f), {1.0f, 0.0f}, (mask & 8) != 0}};
        body = glm::vec2(3.0f * kKey + 2.0f * kGap, 2.0f * kKey + kGap);
        paint = [keys](glm::vec2 p) {
            Paint out;
            float nearest = 1e9f;
            for (const Key& key : keys) {
                const float d = BoxDistance(p, key.centre, glm::vec2(kKey * 0.5f), 4.5f);
                out.body = std::min(out.body, d);
                if (d < nearest) {
                    nearest = d;
                    out.level = key.lit ? 1.0f : kDimLevel;
                    out.alpha = key.lit ? 1.0f : kDimAlpha;
                    if (key.lit) {
                        const glm::vec2 side(-key.direction.y, key.direction.x);
                        const glm::vec2 tip = key.centre + key.direction * 4.5f;
                        const glm::vec2 base = key.centre - key.direction * 3.0f;
                        out.ink = FillInk(TriangleDistance(p, tip, base + side * 5.0f, base - side * 5.0f));
                    } else {
                        out.ink = 0.0f;
                    }
                }
            }
            return out;
        };
    } else if (id.starts_with("mouse:")) {
        const int button = std::atoi(id.c_str() + 6);
        constexpr glm::vec2 kHalf{15.0f, 22.0f};
        constexpr float kSplit = 16.0f;
        body = kHalf * 2.0f;
        paint = [button, kHalf](glm::vec2 p) {
            const float shell = BoxDistance(p, kCellCentre, kHalf, 14.0f);
            const float top = kCellCentre.y - kHalf.y;
            const float split_y = top + kSplit;
            float ink = std::max(StrokeInk(SegmentDistance(p, {kCellCentre.x, top}, {kCellCentre.x, split_y}) + 0.5f),
                                 StrokeInk(SegmentDistance(p, {kCellCentre.x - kHalf.x, split_y}, {kCellCentre.x + kHalf.x, split_y}) + 0.5f));
            if (button == 2) {
                ink = std::max(ink, FillInk(BoxDistance(p, {kCellCentre.x, top + 8.0f}, {2.5f, 5.0f}, 2.5f)));
            } else {
                // the pressed button: the shell's inside above the split, on its side of the middle line
                const float side = button == 1 ? p.x - (kCellCentre.x - 1.0f) : (kCellCentre.x + 1.0f) - p.x;
                ink = std::max(ink, FillInk(std::max({shell + 1.0f, p.y - split_y + 1.0f, side})));
            }
            return Paint{shell, 1.0f, 1.0f, ink};
        };
    }
    PromptGlyph glyph;
    if (!paint) {
        return art.glyphs.emplace(id, glyph).first->second;
    }
    Image icon{0, 0, art.icon_srgb, {}};
    Image glow{0, 0, art.glow_srgb, {}};
    Rasterize(edge, width, paint, icon, glow);
    glyph.icon = "ui:prompt:" + id;
    glyph.glow = glyph.icon + ":glow";
    UploadImage(glyph.icon, icon.width, icon.height, icon.srgb, icon.rgba);
    UploadImage(glyph.glow, glow.width, glow.height, glow.srgb, glow.rgba);
    glyph.width = width / static_cast<float>(kCell);
    glyph.body_width = body.x / static_cast<float>(kCell);
    glyph.body_height = body.y / static_cast<float>(kCell);
    LogInfo("ui: button prompt {} drawn ({} x {} pixels)", id, icon.width, icon.height);
    return art.glyphs.emplace(id, glyph).first->second;
}

std::string SteamPromptGlyphName(const Prompt& prompt, const PromptStyle& style) {
    if (style.device != PromptDevice::Steam) {
        return {};
    }
    switch (prompt.button) {
        case PromptButton::Cross:
        case PromptButton::Circle:
        case PromptButton::Square:
        case PromptButton::Triangle: {
            const size_t face = prompt.button == PromptButton::Cross ? 0 : prompt.button == PromptButton::Circle ? 1
                                : prompt.button == PromptButton::Square ? 2 : 3;
            constexpr char kDefaultFaces[] = {'A', 'B', 'X', 'Y'};
            const char letter = style.faces[face] ? style.faces[face] : kDefaultFaces[face];
            return std::format("steam:face:{}", letter);
        }
        case PromptButton::Options: return "steam:menu";
        case PromptButton::L1: return "steam:bumper:L1";
        case PromptButton::R1: return "steam:bumper:R1";
        default: return {};
    }
}

PromptGlyph UiAssets::PromptPicture(const Prompt& prompt, const PromptStyle& style) {
    const PromptDevice device = style.device;
    const bool built = BuildPcIcons();
    // the option screen's own pictures: cells of cmn_btn_icon_a_ps4_alp (triangle, square, the two D-pads, OPTIONS) and the cross and
    // circle drawn from its square
    auto atlas = [](glm::vec2 uv, float body_w, float body_h) {
        PromptGlyph g;
        g.icon = std::string(kAtlasPath) + ".ftex";
        g.glow = std::string(kGlowPath) + ".ftex";
        g.uv0 = uv;
        g.uv1 = uv + glm::vec2(0.125f, 0.25f);
        g.body_width = body_w;
        g.body_height = body_h;
        g.original = true;
        return g;
    };
    const float disc = built ? 2.0f * prompt_art_->edge.radius / static_cast<float>(kCell) : 0.4f;
    auto pc = [&](bool circle) {
        PromptGlyph g;
        g.icon = kPcIconAtlas;
        g.glow = kPcIconGlow;
        g.uv0 = glm::vec2(circle ? 0.5f : 0.0f, 0.0f);
        g.uv1 = glm::vec2(circle ? 1.0f : 0.5f, 1.0f);
        g.body_width = g.body_height = disc;
        g.original = true;
        return g;
    };
    PromptGlyph original;
    switch (prompt.button) {
        case PromptButton::Cross: original = pc(false); break;
        case PromptButton::Circle: original = pc(true); break;
        case PromptButton::Square: original = atlas({0.0f, 0.25f}, disc, disc); break;
        case PromptButton::Triangle: original = atlas({0.0f, 0.0f}, disc, disc); break;
        case PromptButton::Options: original = atlas({0.625f, 0.5f}, 0.72f, 0.5f); break;
        case PromptButton::DpadUpDown: original = atlas({0.5f, 0.5f}, 0.45f, 0.45f); break;
        case PromptButton::DpadLeftRight: original = atlas({0.5f, 0.25f}, 0.45f, 0.45f); break;
        // the screen's atlas has no shoulder buttons: the OPTIONS cell stands in only when the generated pictures cannot be built
        case PromptButton::L1:
        case PromptButton::R1: original = atlas({0.625f, 0.5f}, 0.72f, 0.5f); break;
    }
    const bool shoulder = prompt.button == PromptButton::L1 || prompt.button == PromptButton::R1;
    if (built && device == PromptDevice::Steam) {
        const std::string name = SteamPromptGlyphName(prompt, style);
        if (!name.empty()) {
            return GeneratedGlyph(name);
        }
    }
    if (built && shoulder && device != PromptDevice::Keyboard) {
        // the shoulder button by the pad's own name: L1 and R1 on PlayStation pads, LB and RB on Xbox pads, L and R on Nintendo pads
        const bool left = prompt.button == PromptButton::L1;
        const char* name = device == PromptDevice::PlayStation ? (left ? "L1" : "R1") : device == PromptDevice::Nintendo ? (left ? "L" : "R")
                                                                                                                      : (left ? "LB" : "RB");
        return GeneratedGlyph(std::string("bumper:") + name);
    }
    if (!built || device == PromptDevice::PlayStation) {
        return original;
    }
    const bool dpad = prompt.button == PromptButton::DpadUpDown || prompt.button == PromptButton::DpadLeftRight;
    if (device != PromptDevice::Keyboard) {
        if (dpad) {
            return original;
        }
        // by position, with the letter SDL gives that position on the pad (south: A on Xbox pads, B on Nintendo pads; the Xbox letters
        // where SDL has none), and the start button as the Xbox menu button or the Nintendo plus
        const int face = prompt.button == PromptButton::Cross ? 0 : prompt.button == PromptButton::Circle ? 1 : prompt.button == PromptButton::Square ? 2
                         : prompt.button == PromptButton::Triangle ? 3 : -1;
        if (face < 0) {
            return GeneratedGlyph(device == PromptDevice::Nintendo ? "plus" : "menu");
        }
        constexpr char kXbox[4] = {'A', 'B', 'X', 'Y'};
        const char letter = style.faces[static_cast<size_t>(face)] ? style.faces[static_cast<size_t>(face)] : kXbox[face];
        return GeneratedGlyph(std::string("letter:") + letter);
    }
    // keyboard and mouse: the keys the action is bound to, from the bindings the input reads
    if (dpad) {
        const KeyBinding* a = FirstBinding(prompt.key, true);
        const KeyBinding* b = FirstBinding(prompt.key2, true);
        if (a && b && IsArrow(a->scancode) && IsArrow(b->scancode)) {
            return GeneratedGlyph(std::format("arrows:{}", ArrowBit(a->scancode) | ArrowBit(b->scancode)));
        }
        if (a && b) {
            return GeneratedGlyph("key:" + KeyBindingName(*a) + " " + KeyBindingName(*b));
        }
        return original;
    }
    const KeyBinding* binding = FirstBinding(prompt.key);
    if (!binding) {
        return original;
    }
    if (binding->mouse) {
        return GeneratedGlyph(std::format("mouse:{}", binding->mouse));
    }
    return GeneratedGlyph("key:" + KeyBindingName(*binding));
}

}
