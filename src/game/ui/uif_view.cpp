#include "game/ui/uif_view.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "engine/core/log.h"
#include "game/ui/pc_settings.h"

#include "engine/core/strcode.h"
#include "engine/render/texture_manager.h"

namespace pt::game {
namespace {

glm::mat3 Affine(glm::vec2 translate, float angle, glm::vec2 scale) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    glm::mat3 m(1.0f);
    m[0] = glm::vec3(c * scale.x, s * scale.x, 0.0f);
    m[1] = glm::vec3(-s * scale.y, c * scale.y, 0.0f);
    m[2] = glm::vec3(translate, 1.0f);
    return m;
}

glm::vec2 Apply(const glm::mat3& m, glm::vec2 p) {
    return glm::vec2(m * glm::vec3(p, 1.0f));
}

ui::TextAlign AlignFromBox(float lo, float hi) {
    if (std::abs(lo + hi) < 1e-4f) {
        return ui::TextAlign::Center;
    }
    return std::abs(lo) < 1e-4f ? ui::TextAlign::Start : ui::TextAlign::End;
}

ui::TextAlign HorizontalAlign(const ui::UifNode& node) {
    switch ((node.text_flags >> 4) & 3) {
    case 1: return ui::TextAlign::Start;
    case 2: return ui::TextAlign::Center;
    case 3: return ui::TextAlign::End;
    default: return AlignFromBox(node.box_min.x, node.box_max.x);
    }
}

ui::TextAlign VerticalAlign(const ui::UifNode& node) {
    switch ((node.text_flags >> 7) & 3) {
    case 1: return ui::TextAlign::Start;
    case 2: return ui::TextAlign::Center;
    case 3: return ui::TextAlign::End;
    default: return AlignFromBox(-node.box_min.y, -node.box_max.y);
    }
}

}

UiCanvas UiCanvas::Fit(gpu::Extent2D target) {
    UiCanvas canvas;
    canvas.extent = glm::vec2(static_cast<float>(target.width), static_cast<float>(target.height));
    canvas.scale = std::min(canvas.extent.x / kWidth, canvas.extent.y / kHeight);
    canvas.origin = (canvas.extent - glm::vec2(kWidth, kHeight) * canvas.scale) * 0.5f;
    return canvas;
}

void DrawText(ui::UiBatch& batch, const UiCanvas& canvas, const UiFont& font, const ui::TextLayout& layout, glm::vec4 color, ui::UiBlend blend,
              ui::UiShade shade) {
    ui::UiDrawParams params = ui::UiDrawParams::Plain(font.atlas);
    const float aw = static_cast<float>(font.font.AtlasWidth());
    const float ah = static_cast<float>(font.font.AtlasHeight());
    if (shade == ui::UiShade::Border) {
        params.extra = glm::vec4(4.0f / aw, 4.0f / ah, static_cast<float>(batch.Extent().height) / 1080.0f, 0.0f);
    }
    std::vector<ui::UiVertex> vertices;
    for (const ui::TextLine& line : layout.lines) {
        const glm::vec2 anchor = line.glyphs.empty() ? glm::vec2(0) : canvas.ToTarget(line.glyphs.front().position);
        const glm::vec2 snap = font.crisp ? glm::round(anchor) - anchor : glm::vec2(0);
        for (const ui::LaidGlyph& g : line.glyphs) {
            if (!g.glyph) {
                continue;
            }
            glm::vec2 p0 = canvas.ToTarget(g.position) + snap;
            glm::vec2 p1 = canvas.ToTarget(g.position + g.size) + snap;
            const float border = font.crisp ? 0.0f : 0.5f;
            glm::vec2 uv0((static_cast<float>(g.glyph->atlas_x) - border) / aw, (static_cast<float>(g.glyph->atlas_y) - border) / ah);
            glm::vec2 uv1((static_cast<float>(g.glyph->atlas_x + g.glyph->width) + border) / aw,
                          (static_cast<float>(g.glyph->atlas_y + g.glyph->height) + border) / ah);
            if (font.crisp) {
                for (int axis = 0; axis < 2; ++axis) {
                    if (p1[axis] - p0[axis] >= 1.0f) continue;
                    const float centre = std::floor((p0[axis] + p1[axis]) * 0.5f) + 0.5f;
                    const float width = p1[axis] - p0[axis];
                    const float lo = std::min(p0[axis], centre - 0.5f), hi = std::max(p1[axis], centre + 0.5f);
                    const float per_pixel = width > 0.0f ? (uv1[axis] - uv0[axis]) / width : 0.0f;
                    uv0[axis] += (lo - p0[axis]) * per_pixel;
                    uv1[axis] += (hi - p1[axis]) * per_pixel;
                    p0[axis] = lo;
                    p1[axis] = hi;
                }
            }
            glm::vec4 vertex_color = color;
            if (shade == ui::UiShade::Border) {
                vertex_color = glm::vec4(uv0, uv1);
                for (int axis = 0; axis < 2; ++axis) {
                    const float span = p1[axis] - p0[axis];
                    if (span <= 0.0f) continue;
                    const float per_pixel = (uv1[axis] - uv0[axis]) / span;
                    const float texels_per_pixel = per_pixel * (axis == 0 ? aw : ah);
                    const float pad = std::min(std::ceil(1.2f * params.extra.z) + 1.0f, texels_per_pixel > 0.0f ? 3.5f / texels_per_pixel : 0.0f);
                    p0[axis] -= pad;
                    p1[axis] += pad;
                    uv0[axis] -= pad * per_pixel;
                    uv1[axis] += pad * per_pixel;
                }
            }
            const ui::UiVertex a{p0, uv0, vertex_color};
            const ui::UiVertex b{{p1.x, p0.y}, {uv1.x, uv0.y}, vertex_color};
            const ui::UiVertex c{p1, uv1, vertex_color};
            const ui::UiVertex d{{p0.x, p1.y}, {uv0.x, uv1.y}, vertex_color};
            vertices.insert(vertices.end(), {a, b, c, a, c, d});
        }
    }
    batch.Draw(vertices, params, shade, blend);
}

void UifView::Bind(const ui::UifModel* model, UiAssets* assets) {
    model_ = model;
    assets_ = assets;
    hash_index_.clear();
    point_index_.clear();
    if (model_) {
        const auto& nodes = model_->Nodes();
        const auto& names = model_->Names();
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].id < names.size()) {
                hash_index_.emplace(static_cast<uint32_t>(names[nodes[i].id]), i);
            }
            if (nodes[i].material_name) {
                hash_index_.emplace(static_cast<uint32_t>(nodes[i].material_name), i);
            }
            for (size_t p = 0; p < nodes[i].points.size(); ++p) {
                point_index_.emplace(static_cast<uint32_t>(nodes[i].points[p].name), std::make_pair(i, p));
            }
        }
    }
    ResetStates();
}

void UifView::ClearAnimation() {
    for (UifNodeState& state : states_) {
        state.anim_translate.reset();
        state.anim_scale.reset();
        state.anim_color.reset();
        state.params.reset();
        state.anim_points.clear();
    }
}

void UifView::ResetStates() {
    states_.assign(model_ ? model_->Nodes().size() : 0, UifNodeState{});
}

UifNodeState& UifView::State(uint16_t id) {
    static UifNodeState dummy;
    const int index = model_ ? model_->IndexOfId(id) : -1;
    return index >= 0 ? states_[index] : dummy;
}

const UifView::World& UifView::Resolve(int index, std::vector<World>& cache, float root_alpha, glm::vec2 root_offset) const {
    World& world = cache[index];
    if (world.done) {
        return world;
    }
    const ui::UifNode& node = model_->Nodes()[index];
    const UifNodeState& state = states_[index];
    World parent;
    if (node.parent >= 0 && node.parent != index) {
        parent = Resolve(node.parent, cache, root_alpha, root_offset);
    } else {
        parent.transform = Affine(root_offset, 0.0f, glm::vec2(1.0f));
        parent.color = glm::vec4(1.0f, 1.0f, 1.0f, root_alpha);
    }
    const float angle = 2.0f * std::atan2(node.rotation.z, node.rotation.w);
    const glm::vec3 base_translate = state.anim_translate.value_or(glm::vec3(node.translate));
    const glm::vec2 translate = glm::vec2(base_translate) + state.offset;
    const glm::vec2 node_scale = state.anim_scale ? glm::vec2(*state.anim_scale) : node.scale;
    const glm::vec2 scale = node.type == ui::UifNodeType::Root ? glm::vec2(1.0f) : node_scale * state.scale;
    world.transform = parent.transform * Affine(node.type == ui::UifNodeType::Root ? glm::vec2(0.0f) : translate, angle, scale);
    glm::vec4 own = state.color.value_or(node.color);
    if (state.anim_color) {
        own = glm::vec4(glm::vec3(own) * glm::vec3(*state.anim_color), state.anim_color->a);
    }
    const glm::vec4 color = node.type == ui::UifNodeType::Root ? glm::vec4(1.0f) : own * state.color_scale;
    world.color = parent.color * color;
    world.visible = parent.visible && state.visible.value_or(true);
    world.done = true;
    return world;
}

void UifView::DrawInlinePictures(ui::UiBatch& batch, const UiCanvas& canvas, const UiFont& font,
                                 const ui::TextLayout& layout, const std::vector<UifInlinePicture>& pictures, float font_height, glm::vec4 color) const {
    const auto& textures = model_->Textures();
    auto texture = [&](int index) {
        if (index < 0 || index >= static_cast<int>(textures.size())) {
            return TextureManager::kWhite;
        }
        const uint32_t t = assets_->Texture(textures[index]);
        return t | assets_->TextureAddressBits(t);
    };
    const float em = static_cast<float>(std::max(1, font.font.EmSize()));
    const ui::FfntGlyph* cap = font.font.Find('H');
    const float cap_middle = cap ? (static_cast<float>(font.font.Pad()) + cap->top + cap->height * 0.5f) / em : 0.5f * font.font.LineFactor();
    for (const ui::TextLine& line : layout.lines) {
        for (const ui::LaidGlyph& g : line.glyphs) {
            if (g.glyph) {
                continue;
            }
            if (g.picture < 0 || static_cast<size_t>(g.picture) >= pictures.size()) {
                continue;
            }
            const UifInlinePicture& picture = pictures[static_cast<size_t>(g.picture)];
            if (picture.texture < 0) {
                continue;
            }
            const float height = font_height / std::max(0.05f, picture.body_height);
            const glm::vec2 centre(g.position.x + g.size.x * 0.5f, g.position.y + font_height * cap_middle);
            const glm::vec2 half(height * picture.width * 0.5f, height * 0.5f);
            const glm::vec2 p0 = canvas.ToTarget(centre - half);
            const glm::vec2 p1 = canvas.ToTarget(centre + half);
            if (picture.glow >= 0) {
                batch.Quad(p0, p1, picture.uv0, picture.uv1, glm::vec4(1.0f, 1.0f, 1.0f, 0.4f * color.a), ui::UiDrawParams::Plain(texture(picture.glow)),
                           ui::UiShade::Material, ui::UiBlend::Additive);
            }
            batch.Quad(p0, p1, picture.uv0, picture.uv1, glm::vec4(1.0f, 1.0f, 1.0f, color.a), ui::UiDrawParams::Plain(texture(picture.texture)),
                       ui::UiShade::Material, ui::UiBlend::Alpha);
        }
    }
}

glm::vec2 UifView::WorldPosition(uint16_t id) const {
    if (!model_) {
        return glm::vec2(0.0f);
    }
    std::vector<World> cache(model_->Nodes().size());
    const int index = model_->IndexOfId(id);
    return index >= 0 ? Apply(Resolve(index, cache, 1.0f, glm::vec2(0.0f)).transform, glm::vec2(0.0f)) : glm::vec2(0.0f);
}

std::optional<glm::vec2> UifView::TextSpan(uint16_t id) const {
    const int index = model_ ? model_->IndexOfId(id) : -1;
    return index >= 0 && index < static_cast<int>(text_spans_.size()) ? text_spans_[index] : std::nullopt;
}

std::optional<glm::vec4> UifView::TextRect(uint16_t id) const {
    const int index = model_ ? model_->IndexOfId(id) : -1;
    return index >= 0 && index < static_cast<int>(text_rects_.size()) ? text_rects_[index] : std::nullopt;
}

std::optional<glm::vec4> UifView::MeasureText(uint16_t id, std::string_view text, int language, UiFontType type, float scale) const {
    const int index = model_ && assets_ ? model_->IndexOfId(id) : -1;
    if (index < 0) return std::nullopt;
    const ui::UifNode& node = model_->Nodes()[index];
    UiFont* font = assets_->Font(type, language);
    if (!font) return std::nullopt;
    std::vector<World> cache(model_->Nodes().size());
    const World& world = Resolve(index, cache, 1.0f, glm::vec2(0.0f));
    const glm::vec2 own = states_[index].scale;
    const float text_scale = std::sqrt(std::abs(glm::determinant(glm::mat2(world.transform)))) / std::max(1e-4f, std::sqrt(std::abs(own.x * own.y))) * scale;
    ui::TextStyle style;
    style.font = &font->font;
    if (const UiFontStyle* s = assets_->FontStyleByHash(node.font_name, language)) {
        style.font_width = s->width;
        style.font_height = s->height;
        style.text_space = s->text_space;
        style.line_space = s->line_space;
    }
    style.font_width *= text_scale;
    style.font_height *= text_scale;
    style.text_space *= text_scale;
    const ui::TextLayout layout = ui::LayoutText(text, style, 0.0f);
    float top = 1e9f;
    float bottom = -1e9f;
    for (const ui::TextLine& line : layout.lines) {
        for (const ui::LaidGlyph& g : line.glyphs) {
            top = std::min(top, g.position.y);
            bottom = std::max(bottom, g.position.y + g.size.y);
        }
    }
    return glm::vec4(layout.width, layout.line_height, top, bottom);
}

void UifView::Draw(ui::UiBatch& batch, const UiCanvas& canvas, int language, float root_alpha, glm::vec2 root_offset) {
    if (!model_ || !assets_) {
        return;
    }
    const auto& nodes = model_->Nodes();
    text_spans_.resize(nodes.size());
    text_rects_.assign(nodes.size(), std::nullopt);
    std::vector<World> cache(nodes.size());
    std::vector<int> order;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].type == ui::UifNodeType::Mesh || nodes[i].type == ui::UifNodeType::Text) {
            order.push_back(static_cast<int>(i));
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return nodes[a].priority < nodes[b].priority; });
    for (const int index : order) {
        const ui::UifNode& node = nodes[index];
        const World& world = Resolve(index, cache, root_alpha, root_offset);
        if (!world.visible || world.color.a <= 0.001f) {
            continue;
        }
        const UifNodeState& state = states_[index];
        const ui::UiBlend blend = node.Additive() ? ui::UiBlend::Additive : ui::UiBlend::Alpha;
        if (node.type == ui::UifNodeType::Mesh) {
            std::array<uint32_t, 4> textures{TextureManager::kWhite, TextureManager::kWhite, TextureManager::kWhite, TextureManager::kWhite};
            for (int slot = 0; slot < 4; ++slot) {
                const int t = slot == ui::kUifBase && state.texture ? *state.texture : node.material.textures[slot];
                if (t >= 0 && t < static_cast<int>(model_->Textures().size())) {
                    textures[slot] = assets_->Texture(model_->Textures()[t]);
                    textures[slot] |= assets_->TextureAddressBits(textures[slot]);
                }
            }
            const ui::UiDrawParams params = ui::UiDrawParams::FromMaterial(textures, state.params.value_or(node.material.params));
            const std::vector<glm::vec2>& base = state.positions ? *state.positions : node.positions;
            std::vector<glm::vec2> deformed;
            if (!state.anim_points.empty()) {
                deformed = base;
                for (const auto& [p, target] : state.anim_points) {
                    const ui::UifPoint& point = node.points[p];
                    for (const uint16_t v : point.vertices) {
                        if (v < deformed.size()) {
                            deformed[v] = base[v] + (target - point.position);
                        }
                    }
                }
            }
            const std::vector<glm::vec2>& positions = deformed.empty() ? base : deformed;
            const std::vector<glm::vec2>& uvs = state.uvs && state.uvs->size() == node.uvs.size() ? *state.uvs : node.uvs;
            std::vector<ui::UiVertex> vertices;
            for (const uint16_t i : node.indices) {
                if (i >= positions.size() || i >= uvs.size()) {
                    continue;
                }
                const glm::vec2 local = positions[i] * node.size;
                vertices.push_back({canvas.FromUnits(Apply(world.transform, local)), uvs[i], world.color});
            }
            batch.Draw(vertices, params, ui::UiShade::Material, blend);
        } else if (state.text && !state.text->empty()) {
            const int font_language = state.font_language.value_or(language);
            const UiFontStyle* style = assets_->FontStyleByHash(node.font_name, font_language);
            const UiFontType type = state.font_type.value_or(style && style->name.starts_with("sbt") ? UiFontType::Movie : UiFontType::System);
            UiFont* font = assets_->Font(type, font_language);
            if (!font) {
                continue;
            }
            const float text_scale = std::sqrt(std::abs(glm::determinant(glm::mat2(world.transform))));
            ui::TextStyle text_style;
            text_style.font = &font->font;
            if (style) {
                text_style.font_width = style->width;
                text_style.font_height = style->height;
                text_style.text_space = style->text_space;
                text_style.line_space = style->line_space;
            }
            text_style.font_width *= text_scale;
            text_style.font_height *= text_scale;
            text_style.text_space *= text_scale;
            text_style.line_space *= text_scale;
            if (state.text_line_pitch) text_style.line_space = *state.text_line_pitch - text_style.font_height * font->font.LineFactor();
            const std::vector<UifInlinePicture>& pictures = state.inline_pictures;
            for (const UifInlinePicture& picture : pictures) {
                text_style.inline_advances.push_back(text_style.font_height / std::max(0.05f, picture.body_height) * picture.body_width + 0.5f * text_style.font_height);
            }
            if (!pictures.empty()) text_style.inline_advance = text_style.inline_advances.front();
            const glm::vec2 a = canvas.FromUnits(Apply(world.transform, glm::vec2(node.box_min) * node.size));
            const glm::vec2 b = canvas.FromUnits(Apply(world.transform, glm::vec2(node.box_max) * node.size));
            const glm::vec2 lo = (glm::min(a, b) - canvas.origin) / canvas.scale;
            glm::vec2 hi = (glm::max(a, b) - canvas.origin) / canvas.scale;
            const float wrap = hi.x - lo.x;
            ui::TextLayout layout = ui::LayoutText(*state.text, text_style, wrap);
            if (state.text_end) {
                const float end = (canvas.FromUnits(Apply(world.transform, glm::vec2(*state.text_end, 0.0f))).x - canvas.origin.x) / canvas.scale;
                const float room = end - lo.x;
                if (room > 0.0f && layout.width > room) {
                    const float fit = room / layout.width;
                    text_style.font_width *= fit;
                    text_style.font_height *= fit;
                    text_style.text_space *= fit;
                    text_style.line_space *= fit;
                    text_style.inline_advance *= fit;
                    for (float& advance : text_style.inline_advances) advance *= fit;
                    layout = ui::LayoutText(*state.text, text_style, wrap);
                }
                if (layout.rtl && state.mirror_rtl) hi.x = end;
            }
            if (state.text_fit) {
                const float room = *state.text_fit;
                if (room > 0.0f && layout.width > room) {
                    const float fit = room / layout.width;
                    text_style.font_width *= fit;
                    text_style.font_height *= fit;
                    text_style.text_space *= fit;
                    text_style.line_space *= fit;
                    text_style.inline_advance *= fit;
                    for (float& advance : text_style.inline_advances) advance *= fit;
                    layout = ui::LayoutText(*state.text, text_style, wrap);
                }
            }
            const ui::TextAlign h = HorizontalAlign(node);
            const ui::TextAlign v = VerticalAlign(node);
            if (state.text_box) {
                const auto box = *state.text_box;
                layout = ui::LayoutTextInBox(*state.text, text_style, {box.x, box.y}, {box.z, box.w}, state.mirror_rtl);
                if (!descriptions_audited_ && !state.description_samples.empty() && std::getenv("PT_AUDIT_PC_DESCRIPTIONS")) {
                    descriptions_audited_ = true;
                    int count = 0, failures = 0, original_overflows = 0;
                    auto keys = PcDescriptionKeys();
                    for (const auto& sample : state.description_samples) keys.push_back(sample);
                    keys.push_back("FSR: amd_fidelityfx_vk.dll is missing next to pt.exe. DLSS: nvngx_dlss.dll is missing next to pt.exe. XeSS: libxess.dll is missing next to pt.exe.");
                    keys.push_back("DLSS. Render resolution: 1706 x 960. Output resolution: 2560 x 1440.");
                    for (int lang = 0; lang < UiAssets::kLanguageCount; ++lang) {
                        auto* audit_font = assets_->Font(UiFontType::PcSystem, lang);
                        const auto* audit_style = assets_->FontStyleByHash(node.font_name, lang);
                        if (!audit_font) { ++failures; continue; }
                        auto metrics = text_style;
                        metrics.font = &audit_font->font;
                        if (audit_style) {
                            metrics.font_width = audit_style->width * text_scale;
                            metrics.font_height = audit_style->height * text_scale;
                            metrics.text_space = audit_style->text_space * text_scale;
                            metrics.line_space = audit_style->line_space * text_scale;
                        }
                        for (auto key : keys) {
                            const auto translated = PcNoteText(key, lang);
                            auto raw = ui::LayoutText(translated, metrics, box.z - box.x);
                            const bool overflow = raw.height > box.w - box.y || raw.width > box.z - box.x;
                            original_overflows += overflow;
                            auto fitted = ui::LayoutTextInBox(translated, metrics, {box.x, box.y}, {box.z, box.w});
                            bool fits = !translated.empty() && !fitted.lines.empty();
                            for (const auto& line : fitted.lines) for (const auto& glyph : line.glyphs)
                                fits &= glyph.position.x >= box.x - .01f && glyph.position.y >= box.y - .01f &&
                                    glyph.position.x + glyph.size.x <= box.z + .01f && glyph.position.y + glyph.size.y <= box.w + .01f;
                            failures += !fits;
                            ++count;
                            LogInfo("pc-description-audit: lang {} key {} raw-overflow {} fits {} lines {} height {}", lang, key, overflow, fits, fitted.lines.size(), fitted.height);
                        }
                    }
                    LogInfo("pc-description-audit: {} cases, {} original overflows, {} failures", count, original_overflows, failures);
                }
            } else {
                ui::PlaceText(layout, lo, hi, h, h, v, state.mirror_rtl);
                float left = 1e9f;
                float right = -1e9f;
                for (const ui::TextLine& line : layout.lines) {
                    for (const ui::LaidGlyph& g : line.glyphs) {
                        left = std::min(left, g.position.x);
                        right = std::max(right, g.position.x + g.size.x);
                    }
                }
                const float origin = (canvas.FromUnits(Apply(world.transform, glm::vec2(0.0f))).x - canvas.origin.x) / canvas.scale;
                const float units = UiCanvas::kUnit * std::max(1e-4f, glm::length(glm::vec2(world.transform[0])));
                text_spans_[index] = left <= right ? std::optional<glm::vec2>(glm::vec2(left - origin, right - origin) / units) : std::nullopt;
            }
            {
                glm::vec4 rect(1e9f, 1e9f, -1e9f, -1e9f);
                for (const ui::TextLine& line : layout.lines) {
                    for (const ui::LaidGlyph& g : line.glyphs) {
                        rect = glm::vec4(glm::min(glm::vec2(rect), g.position), glm::max(glm::vec2(rect.z, rect.w), g.position + g.size));
                    }
                }
                if (rect.x <= rect.z) text_rects_[index] = rect;
            }
            DrawText(batch, canvas, *font, layout, world.color, blend);
            if (!pictures.empty()) {
                const float shown = layout.line_height / std::max(0.01f, font->font.LineFactor());
                DrawInlinePictures(batch, canvas, *font, layout, pictures, shown > 0.0f ? shown : text_style.font_height, world.color);
            }
        }
    }
}

}
