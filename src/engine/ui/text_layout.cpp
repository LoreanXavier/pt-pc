#include "engine/ui/text_layout.h"

#include <algorithm>
#include <string_view>

namespace pt::ui {
namespace {

bool StrongRtl(uint32_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

bool Cjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF);
}

bool NoLineStart(uint32_t c) {
    constexpr std::u32string_view kClosing = U"\uFF0C\u3002\u3001\uFF1B\uFF1A\uFF01\uFF1F\uFF09\u300D\u300F\u300B\u3009\u3011\u3015\u2026\u00B7%,.;:!?)]}";
    return kClosing.find(static_cast<char32_t>(c)) != std::u32string_view::npos;
}

bool NoLineEnd(uint32_t c) {
    constexpr std::u32string_view kOpening = U"\uFF08\u300C\u300E\u300A\u3008\u3010\u3014([{";
    return kOpening.find(static_cast<char32_t>(c)) != std::u32string_view::npos;
}

bool BreakBefore(const std::vector<uint32_t>& codes, size_t i) {
    const uint32_t before = codes[i - 1], at = codes[i];
    if (before == ' ') return true;
    if (at == ' ') return false;
    return (Cjk(before) || Cjk(at)) && !NoLineStart(at) && !NoLineEnd(before);
}

struct Pending {
    uint32_t code = 0;
    const FfntGlyph* glyph = nullptr;
    float pen = 0.0f;
    float advance = 0.0f;
};

void Emit(TextLayout& layout, std::vector<Pending>& line, const TextStyle& style, bool keep_trailing_spaces, float max_width = 0.0f) {
    while (!keep_trailing_spaces && !line.empty() && line.back().code == ' ') {
        line.pop_back();
    }
    TextLine out;
    const float em = static_cast<float>(std::max(1, style.font->EmSize()));
    const float pad = static_cast<float>(style.font->Pad());
    const float top = static_cast<float>(layout.lines.size()) * layout.line_pitch;
    const float start = line.empty() ? 0.0f : line.front().pen;
    if (style.font->Unicode()) {
        std::vector<uint32_t> codes;
        for (const auto& p : line) codes.push_back(p.code);
        const float sx = style.font_width / em, sy = style.font_height / em;
        auto measure = [&](size_t count) {
            auto shaped = style.font->ShapeLine(std::span<const uint32_t>(codes.data(), count), style.inline_advance / sx);
            return shaped.empty() ? 0.0f : sx * (shaped.back().pen + shaped.back().advance) + float(shaped.size()) * style.text_space;
        };
        if (max_width > 0 && codes.size() > 1 && measure(codes.size()) > max_width) {
            size_t low=1, high=codes.size();
            while (low+1 < high) { const size_t mid=(low+high)/2; if (measure(mid)<=max_width) low=mid; else high=mid; }
            size_t split=low;
            for (size_t i = low; i > 0; --i) if (BreakBefore(codes, i)) { split = i; break; }
            std::vector<Pending> rest(line.begin()+split, line.end()); line.resize(split);
            Emit(layout,line,style,false,0);
            while(!rest.empty() && rest.front().code==' ') rest.erase(rest.begin());
            line=std::move(rest); Emit(layout,line,style,keep_trailing_spaces,max_width); return;
        }
        const auto shaped = style.font->ShapeLine(codes, style.inline_advance / sx);
        float tracking=0;
        for (const auto& p : shaped) {
            out.width = sx * (p.pen + p.advance) + tracking + style.text_space;
            if (!p.glyph) {
                out.glyphs.push_back({nullptr, {sx*p.pen + tracking, top}, {sx*p.advance, layout.line_height}});
            } else if (p.glyph->width && p.glyph->height) {
                out.glyphs.push_back({p.glyph,
                    {sx*(p.pen + p.x + p.glyph->bearing) + tracking, top + sy*(p.y + p.glyph->top)},
                    {sx*p.glyph->width, sy*p.glyph->height}});
            }
            tracking += style.text_space;
        }
        layout.width = std::max(layout.width, out.width);
        layout.lines.push_back(std::move(out)); line.clear(); return;
    }
    for (const Pending& p : line) {
        out.width = p.pen - start + p.advance;
        if (p.code == kInlinePicture && !p.glyph) {
            out.glyphs.push_back({nullptr, {p.pen - start, top}, {p.advance, layout.line_height}});
            continue;
        }
        if (!p.glyph || p.glyph->width <= 1 || p.code == ' ') {
            continue;
        }
        LaidGlyph g;
        g.glyph = p.glyph;
        g.position = {p.pen - start + style.font_width * (pad + p.glyph->bearing) / em, top + style.font_height * (pad + p.glyph->top) / em};
        g.size = {style.font_width * p.glyph->width / em, style.font_height * p.glyph->height / em};
        out.glyphs.push_back(g);
    }
    layout.width = std::max(layout.width, out.width);
    layout.lines.push_back(std::move(out));
    line.clear();
}

}

void NormalizeSubtitleStyle(TextStyle& style) {
    if (!style.font || !style.font->Unicode()) return;
    style.font_width = style.font_height = 22.0f;
    style.text_space = 0.0f;
    style.line_space = 26.0f - style.font_height * style.font->LineFactor();
}

TextLayout LayoutText(std::string_view utf8, const TextStyle& style, float max_width) {
    TextLayout layout;
    if (!style.font) {
        return layout;
    }
    layout.line_height = style.font_height * style.font->LineFactor();
    layout.line_pitch = layout.line_height + style.line_space;
    std::vector<Pending> line;
    float pen = 0.0f;
    const std::vector<uint32_t> codes = DecodeUtf8(utf8);
    layout.rtl = style.font->Unicode() && std::any_of(codes.begin(), codes.end(), StrongRtl);
    for (size_t i = 0; i < codes.size(); ++i) {
        const uint32_t code = codes[i];
        if (code == '\r') {
            continue;
        }
        if (code == '\n') {
            Emit(layout, line, style, true, max_width);
            pen = 0.0f;
            continue;
        }
        Pending p;
        p.code = code;
        if (code == kInlinePicture && style.inline_advance > 0.0f) {
            p.advance = style.inline_advance + style.text_space;
        } else {
            p.glyph = style.font->FindOrFallback(code);
            p.advance = (p.glyph ? style.font_width * style.font->Advance(*p.glyph) : 0.0f) + style.text_space;
        }
        if (!style.font->Unicode() && max_width > 0.0f && !line.empty() && code != ' ' && pen + p.advance - line.front().pen > max_width) {
            auto space = std::find_if(line.rbegin(), line.rend(), [](const Pending& q) { return q.code == ' '; });
            if (space != line.rend()) {
                const size_t split = static_cast<size_t>(line.rend() - space);
                std::vector<Pending> rest(line.begin() + static_cast<std::ptrdiff_t>(split), line.end());
                line.resize(split);
                Emit(layout, line, style, false);
                line = std::move(rest);
            } else {
                Emit(layout, line, style, false);
            }
            if (!line.empty()) {
                const float shift = line.front().pen;
                for (Pending& q : line) {
                    q.pen -= shift;
                }
                pen = line.back().pen + line.back().advance;
            } else {
                pen = 0.0f;
            }
        }
        p.pen = pen;
        pen += p.advance;
        line.push_back(p);
    }
    if (!line.empty() || layout.lines.empty()) {
        Emit(layout, line, style, true, max_width);
    }
    const size_t n = layout.lines.size();
    layout.height = n ? static_cast<float>(n) * layout.line_height + static_cast<float>(n - 1) * style.line_space : 0.0f;
    return layout;
}

TextLayout LayoutTextInBox(std::string_view utf8, TextStyle style, glm::vec2 box_min, glm::vec2 box_max, bool mirror_rtl) {
    const glm::vec2 room = glm::max(box_max - box_min, glm::vec2(1.0f));
    TextLayout layout;
    glm::vec2 lo, hi;
    for (int attempt = 0; attempt < 60; ++attempt) {
        layout = LayoutText(utf8, style, room.x);
        lo = glm::vec2(0.0f);
        hi = {layout.width, layout.height};
        for (const auto& line : layout.lines) for (const auto& glyph : line.glyphs) {
            lo = glm::min(lo, glyph.position);
            hi = glm::max(hi, glyph.position + glyph.size);
        }
        const glm::vec2 extent = hi - lo;
        if (extent.x <= room.x && extent.y <= room.y) break;
        const float factor = .95f;
        style.font_width *= factor;
        style.font_height *= factor;
        style.text_space *= factor;
        style.line_space *= factor;
        style.inline_advance *= factor;
    }
    for (auto& line : layout.lines) {
        const float shift = layout.rtl && mirror_rtl ? std::max(0.0f, room.x - (line.width - lo.x)) : 0.0f;
        for (auto& glyph : line.glyphs) glyph.position += box_min - lo + glm::vec2(shift, 0.0f);
    }
    return layout;
}

void PlaceText(TextLayout& layout, glm::vec2 box_min, glm::vec2 box_max, TextAlign block_h, TextAlign line_h, TextAlign v, bool mirror_rtl) {
    if (layout.rtl && mirror_rtl) {
        auto mirror = [](TextAlign a) { return a == TextAlign::Start ? TextAlign::End : a == TextAlign::End ? TextAlign::Start : a; };
        block_h = mirror(block_h);
        line_h = mirror(line_h);
    }
    auto align = [](TextAlign a, float lo, float hi, float extent) {
        return a == TextAlign::Start ? lo : a == TextAlign::End ? hi - extent : (lo + hi - extent) * 0.5f;
    };
    const float block_x = align(block_h, box_min.x, box_max.x, layout.width);
    const float block_y = align(v, box_min.y, box_max.y, layout.height);
    for (TextLine& line : layout.lines) {
        const float x = align(line_h, block_x, block_x + layout.width, line.width);
        for (LaidGlyph& g : line.glyphs) {
            g.position += glm::vec2(x, block_y);
        }
    }
}

}
