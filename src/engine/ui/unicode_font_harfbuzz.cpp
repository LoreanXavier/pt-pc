#ifndef _WIN32
#include "engine/ui/ffnt.h"
#include "engine/core/resource_path.h"

#include <hb.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace pt::ui {
struct UnicodeFontState {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    float scale = 1.0f;
    int ascent = 0;
    bool rtl = false;
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    ~UnicodeFontState() {
        if (font) hb_font_destroy(font);
        if (face) hb_face_destroy(face);
        if (blob) hb_blob_destroy(blob);
    }
};
namespace {
constexpr int kEm = 48;
const char* FontFile(std::string_view family) {
    if (family == "Noto Sans") return "NotoSans.ttf";
    if (family == "Noto Kufi Arabic") return "NotoKufiArabic.ttf";
    if (family == "Noto Naskh Arabic") return "NotoNaskhArabic.ttf";
    if (family == "Noto Sans SC") return "NotoSansSC.ttf";
    return nullptr;
}
uint16_t Be16(const unsigned char* p) { return uint16_t((p[0] << 8) | p[1]); }
bool WinMetrics(const UnicodeFontState& s, int& ascent, int& descent) {
    const uint32_t os2 = stbtt__find_table(const_cast<unsigned char*>(s.data.data()), uint32_t(s.info.fontstart), "OS/2");
    if (!os2 || os2 + 78 > s.data.size()) return false;
    ascent = Be16(&s.data[os2 + 74]);
    descent = Be16(&s.data[os2 + 76]);
    return true;
}
bool StrongRtl(uint32_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}
bool StrongLtr(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x0590 && c != 0xD7 && c != 0xF7);
}
struct Run { size_t begin, end; bool rtl; };
std::vector<Run> VisualRuns(std::span<const uint32_t> codes, bool paragraph_rtl) {
    std::vector<int> dir(codes.size());
    for (size_t i = 0; i < codes.size(); ++i) dir[i] = StrongRtl(codes[i]) ? 1 : StrongLtr(codes[i]) ? 0 : -1;
    for (size_t i = 0; i < codes.size(); ++i) {
        if (dir[i] != -1) continue;
        size_t j = i;
        while (j < codes.size() && dir[j] == -1) ++j;
        const int before = i > 0 ? dir[i - 1] : (paragraph_rtl ? 1 : 0);
        const int after = j < codes.size() ? dir[j] : (paragraph_rtl ? 1 : 0);
        const int resolved = before == after ? before : (paragraph_rtl ? 1 : 0);
        for (size_t k = i; k < j; ++k) dir[k] = resolved;
        i = j - 1;
    }
    std::vector<Run> runs;
    for (size_t i = 0; i < codes.size();) {
        size_t j = i;
        while (j < codes.size() && dir[j] == dir[i]) ++j;
        runs.push_back({i, j, dir[i] == 1});
        i = j;
    }
    if (paragraph_rtl) std::reverse(runs.begin(), runs.end());
    return runs;
}
struct Shaped { uint32_t glyph; uint32_t cluster; float advance, x, y; };
std::vector<Shaped> ShapeRun(const UnicodeFontState& s, std::span<const uint32_t> codes, bool rtl) {
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf32(buffer, codes.data(), int(codes.size()), 0, int(codes.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_shape(s.font, buffer, nullptr, 0);
    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);
    std::vector<Shaped> out;
    for (unsigned i = 0; i < count; ++i) {
        out.push_back({info[i].codepoint, info[i].cluster, std::round(pos[i].x_advance * s.scale), pos[i].x_offset * s.scale,
                       pos[i].y_offset * s.scale});
    }
    hb_buffer_destroy(buffer);
    return out;
}
}

bool FfntFont::LoadUnicodeFont(std::string_view family, std::string_view characters, bool rtl, std::string* error) {
    auto fail = [&](const char* text) { if (error) *error = text; return false; };
    const char* file = FontFile(family);
    if (!file) return fail("bundled font face unavailable");
    auto dir = ExecutableDir() / "fonts";
    if (!std::filesystem::is_directory(dir)) dir = PT_FONT_DIR;
    auto state = std::make_shared<UnicodeFontState>();
    {
        std::ifstream in(dir / file, std::ios::binary);
        if (!in) return fail("bundled fonts missing");
        state->data.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (!stbtt_InitFont(&state->info, state->data.data(), stbtt_GetFontOffsetForIndex(state->data.data(), 0)))
        return fail("Unicode font allocation failed");
    state->scale = stbtt_ScaleForMappingEmToPixels(&state->info, float(kEm));
    int win_ascent = 0, win_descent = 0;
    if (!WinMetrics(*state, win_ascent, win_descent)) {
        int a = 0, d = 0, gap = 0;
        stbtt_GetFontVMetrics(&state->info, &a, &d, &gap);
        win_ascent = a;
        win_descent = -d;
    }
    state->ascent = int(std::lround(win_ascent * state->scale));
    const int height = state->ascent + int(std::lround(win_descent * state->scale));
    state->rtl = rtl;
    state->blob = hb_blob_create(reinterpret_cast<const char*>(state->data.data()), unsigned(state->data.size()), HB_MEMORY_MODE_READONLY,
                                 nullptr, nullptr);
    state->face = hb_face_create(state->blob, 0);
    state->font = hb_font_create(state->face);
    const unsigned upem = hb_face_get_upem(state->face);
    hb_font_set_scale(state->font, int(upem), int(upem));

    std::set<uint32_t> ids;
    std::vector<std::pair<uint32_t, uint32_t>> codes;
    const auto decoded = DecodeUtf8(characters);
    for (auto code : decoded) {
        if (code > 0xFFFF || code < 32 || code == 0xE000) continue;
        const int id = stbtt_FindGlyphIndex(&state->info, int(code));
        if (id == 0) { if (error) *error = "missing translated character U+" + std::to_string(code); return false; }
        codes.emplace_back(code, uint32_t(id));
        ids.insert(uint32_t(id));
    }
    for (const auto& run : VisualRuns(decoded, rtl))
        for (const auto& g : ShapeRun(*state, std::span<const uint32_t>(decoded).subspan(run.begin, run.end - run.begin), run.rtl)) ids.insert(g.glyph);
    if (rtl) {
        for (int i = 0; i < state->info.numGlyphs; ++i) ids.insert(uint32_t(i));
    }
    glyphs_.clear(); bitmap_.clear(); composed_.clear(); em_size_ = kEm; pad_ = 0;
    unicode_line_factor_ = float(height) / float(kEm);
    for (auto id : ids) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0, advance = 0, bearing = 0;
        stbtt_GetGlyphBitmapBox(&state->info, int(id), state->scale, state->scale, &x0, &y0, &x1, &y1);
        stbtt_GetGlyphHMetrics(&state->info, int(id), &advance, &bearing);
        if (x1 - x0 > 1020 || y1 - y0 > 1020) return fail("font glyph too large or unreadable");
        FfntGlyph g;
        g.code = 0x100000 + id;
        g.width = uint16_t(std::max(0, x1 - x0));
        g.height = uint16_t(std::max(0, y1 - y0));
        g.advance = uint16_t(std::clamp<long>(std::lround(advance * state->scale), 0, 65535));
        g.bearing = int16_t(x0);
        g.top = int16_t(state->ascent + y0);
        std::vector<uint8_t> alpha(size_t(g.width) * g.height);
        if (!alpha.empty()) stbtt_MakeGlyphBitmap(&state->info, alpha.data(), g.width, g.height, g.width, state->scale, state->scale, int(id));
        composed_[g.code] = alpha;
        glyphs_.push_back(g);
        for (auto [code, glyph_id] : codes) {
            if (glyph_id == id && !composed_.contains(code)) {
                g.code = code;
                composed_[code] = alpha;
                glyphs_.push_back(g);
            }
        }
    }
    std::sort(glyphs_.begin(), glyphs_.end(), [](auto& a, auto& b) { return a.code < b.code; });
    unicode_ = std::move(state);
    return true;
}

std::vector<ShapedGlyph> FfntFont::ShapeLine(std::span<const uint32_t> codes, float inline_advance) const {
    std::vector<ShapedGlyph> out;
    if (!unicode_ || codes.empty()) return out;
    const bool has_arabic = std::any_of(codes.begin(), codes.end(), [](uint32_t c) { return c >= 0x600 && c <= 0x6FF; });
    float pen = 0;
    for (const auto& run : VisualRuns(codes, unicode_->rtl && has_arabic)) {
        const auto part = codes.subspan(run.begin, run.end - run.begin);
        for (const auto& g : ShapeRun(*unicode_, part, run.rtl)) {
            const bool picture = g.cluster < part.size() && part[g.cluster] == 0xE000;
            const float advance = picture ? inline_advance : g.advance;
            out.push_back({picture ? nullptr : Find(0x100000 + g.glyph), pen, g.x, -g.y, advance});
            pen += advance;
        }
    }
    return out;
}
}
#endif
