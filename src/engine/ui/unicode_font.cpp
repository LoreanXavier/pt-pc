#include "engine/ui/ffnt.h"
#include <algorithm>
#include <filesystem>
#include <set>
#include <array>
#include <cstdio>

namespace pt::ui {
uint8_t FfntFont::Coverage(const FfntGlyph& glyph, int x, int y) const {
    if (unicode_) {
        auto it = composed_.find(glyph.code);
        return it != composed_.end() && x >= 0 && y >= 0 && x < glyph.width && y < glyph.height
            ? it->second[size_t(y) * glyph.width + x] : 0;
    }
    return Bit(glyph, x, y) ? 255 : 0;
}
}

#ifdef _WIN32
#include <windows.h>
#include <usp10.h>

namespace pt::ui {
struct UnicodeFontState {
    HDC dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ previous = nullptr;
    bool rtl = false;
    SCRIPT_CACHE cache = nullptr;
    ~UnicodeFontState() { ScriptFreeCache(&cache); if (dc) { SelectObject(dc, previous); DeleteObject(font); DeleteDC(dc); } }
};
namespace {
constexpr const wchar_t* kBundledFonts[] = {L"NotoSans.ttf", L"NotoSansSC.ttf", L"NotoKufiArabic.ttf", L"NotoNaskhArabic.ttf"};
struct PrivateFonts {
    std::vector<std::wstring> paths;
    PrivateFonts() {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
        auto dir = std::filesystem::path(module).parent_path() / "fonts";
        if (!std::filesystem::is_directory(dir)) dir = PT_FONT_DIR;
        for (const auto* name : kBundledFonts) {
            auto path = (dir / name).wstring();
            if (AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr)) paths.push_back(path);
        }
    }
    ~PrivateFonts() { for (auto& path : paths) RemoveFontResourceExW(path.c_str(), FR_PRIVATE, nullptr); }
};
std::wstring Wide(std::string_view text) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), n);
    return out;
}
}
bool FfntFont::LoadUnicodeFont(std::string_view family, std::string_view characters, bool rtl, std::string* error) {
    static PrivateFonts resources;
    if (resources.paths.size() != std::size(kBundledFonts)) { if (error) *error = "bundled fonts missing"; return false; }
    auto state = std::make_shared<UnicodeFontState>();
    state->dc = CreateCompatibleDC(nullptr);
    const auto name = Wide(family);
    state->font = CreateFontW(-48, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, name.c_str());
    if (!state->dc || !state->font) { if (error) *error = "Unicode font allocation failed"; return false; }
    state->previous = SelectObject(state->dc, state->font); state->rtl = rtl;
    wchar_t actual[128]{}; GetTextFaceW(state->dc, 128, actual);
    if (std::wstring(actual) != name) { if (error) *error = "bundled font face unavailable"; return false; }
    TEXTMETRICW tm{}; GetTextMetricsW(state->dc, &tm);
    std::set<uint16_t> ids;
    std::vector<std::pair<uint32_t,uint16_t>> codes;
    for (auto code : DecodeUtf8(characters)) {
        if (code > 0xFFFF || code < 32 || code == 0xE000) continue;
        WORD id = 0xFFFF; wchar_t wc = wchar_t(code);
        GetGlyphIndicesW(state->dc, &wc, 1, &id, GGI_MARK_NONEXISTING_GLYPHS);
        if (id == 0xFFFF) { if (error) *error = "missing translated character U+" + std::to_string(code); return false; }
        codes.emplace_back(code, id); ids.insert(id);
    }
    {
        const auto text = Wide(characters);
        std::vector<SCRIPT_ITEM> items(text.size()+2); int count=0; SCRIPT_CONTROL control{}; SCRIPT_STATE bidi{}; bidi.uBidiLevel=rtl?1:0;
        if(FAILED(ScriptItemize(text.data(),int(text.size()),int(items.size())-1,&control,&bidi,items.data(),&count))) return false;
        for(int i=0;i<count;++i) {
            const int n=items[i+1].iCharPos-items[i].iCharPos, capacity=n*3+32;
            std::vector<WORD> shaped(capacity),clusters(n);std::vector<SCRIPT_VISATTR> attrs(capacity);int glyph_count=0;
            if(FAILED(ScriptShape(state->dc,&state->cache,text.data()+items[i].iCharPos,n,capacity,&items[i].a,shaped.data(),clusters.data(),attrs.data(),&glyph_count))) return false;
            for(int g=0;g<glyph_count;++g) ids.insert(shaped[g]);
        }
    }
    int ascent = tm.tmAscent, height = tm.tmHeight;
    if (rtl) {
        int above = 0, below = 0;
        MAT2 identity{}; identity.eM11.value = identity.eM22.value = 1;
        for (auto id : ids) {
            GLYPHMETRICS gm{};
            if (GetGlyphOutlineW(state->dc, id, GGO_METRICS | GGO_GLYPH_INDEX, &gm, 0, nullptr, &identity) == GDI_ERROR) continue;
            above = std::max(above, int(gm.gmptGlyphOrigin.y));
            below = std::max(below, int(gm.gmBlackBoxY) - int(gm.gmptGlyphOrigin.y));
        }
        if (above > 0 && above + 2 <= tm.tmAscent && below + 2 <= tm.tmDescent) {
            ascent = above + 2;
            height = ascent + below + 2;
        }
    }
    if (rtl) {
        unsigned char maxp[6]{};
        if (GetFontData(state->dc, 0x7078616d, 0, maxp, 6) == GDI_ERROR) return false;
        const unsigned count = (unsigned(maxp[4]) << 8) | maxp[5];
        for (unsigned i=0; i<count; ++i) ids.insert(uint16_t(i));
    }
    glyphs_.clear(); bitmap_.clear(); composed_.clear(); em_size_ = 48; pad_ = 0;
    unicode_line_factor_ = float(height) / 48.0f;
    MAT2 matrix{}; matrix.eM11.value = matrix.eM22.value = 1;
    for (auto id : ids) {
        GLYPHMETRICS gm{};
        const DWORD n = GetGlyphOutlineW(state->dc, id, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX, &gm, 0, nullptr, &matrix);
        if (n == GDI_ERROR || gm.gmBlackBoxX > 1020 || gm.gmBlackBoxY > 1020) {if(error)*error="font glyph too large or unreadable";return false;}
        if (!n) gm.gmBlackBoxX = gm.gmBlackBoxY = 0;
        std::vector<uint8_t> raw(n);
        if (n && GetGlyphOutlineW(state->dc, id, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX, &gm, n, raw.data(), &matrix) == GDI_ERROR) return false;
        FfntGlyph g; g.code = 0x100000 + id; g.width = uint16_t(gm.gmBlackBoxX); g.height = uint16_t(gm.gmBlackBoxY);
        g.advance = uint16_t(std::clamp<int>(gm.gmCellIncX, 0, 65535)); g.bearing = int16_t(gm.gmptGlyphOrigin.x);
        g.top = int16_t(ascent - gm.gmptGlyphOrigin.y);
        if (n < ((gm.gmBlackBoxX + 3) & ~3u) * gm.gmBlackBoxY) { if(error)*error="font bitmap truncated";return false; }
        std::vector<uint8_t> alpha(size_t(g.width) * g.height);
        const unsigned pitch = (unsigned(g.width) + 3) & ~3u;
        for (unsigned y=0;y<g.height;++y) for (unsigned x=0;x<g.width;++x)
            alpha[size_t(y)*g.width+x] = uint8_t(std::min(255u, unsigned(raw[size_t(y)*pitch+x])*255/64));
        composed_[g.code] = alpha; glyphs_.push_back(g);
        for (auto [code, glyph_id] : codes) if (glyph_id == id && !composed_.contains(code)) {
            g.code = code; composed_[code] = alpha; glyphs_.push_back(g);
        }
    }
    std::sort(glyphs_.begin(),glyphs_.end(),[](auto& a,auto& b){return a.code<b.code;});
    unicode_ = std::move(state); return true;
}
std::vector<ShapedGlyph> FfntFont::ShapeLine(std::span<const uint32_t> codes, float inline_advance) const {
    std::vector<ShapedGlyph> out;
    if (!unicode_ || codes.empty()) return out;
    std::wstring text; for (auto c:codes) text.push_back(wchar_t(c));
    std::vector<SCRIPT_ITEM> items(text.size()+2); int count = 0;
    SCRIPT_CONTROL control{}; SCRIPT_STATE state{};
    const bool has_arabic = std::any_of(codes.begin(),codes.end(),[](uint32_t c){return c>=0x600 && c<=0x6FF;});
    state.uBidiLevel = unicode_->rtl && has_arabic ? 1 : 0;
    if (FAILED(ScriptItemize(text.data(), int(text.size()), int(items.size())-1, &control, &state, items.data(), &count))) return out;
    std::vector<BYTE> levels(count); std::vector<int> order(count);
    for(int i=0;i<count;++i) levels[i]=items[i].a.s.uBidiLevel;
    if (FAILED(ScriptLayout(count, levels.data(), order.data(), nullptr))) return out;
    float pen = 0;
    for (int logical : order) {
        auto item = items[logical]; const int n = items[logical+1].iCharPos - item.iCharPos;
        const int capacity = n*3+32;
        std::vector<WORD> glyphs(capacity), clusters(n); std::vector<SCRIPT_VISATTR> attrs(capacity);
        std::vector<int> advances(capacity); std::vector<GOFFSET> offsets(capacity);
        int glyph_count = 0; ABC abc{};
        HRESULT hr = ScriptShape(unicode_->dc, &unicode_->cache, text.data()+item.iCharPos,n,capacity,&item.a,glyphs.data(),clusters.data(),attrs.data(),&glyph_count);
        if (SUCCEEDED(hr)) hr = ScriptPlace(unicode_->dc,&unicode_->cache,glyphs.data(),glyph_count,attrs.data(),&item.a,advances.data(),offsets.data(),&abc);
        if (FAILED(hr)) return {};
        for(int g=0;g<glyph_count;++g) {
            bool picture=false;
            for(int c=0;c<n;++c) if (clusters[c]==g && text[item.iCharPos+c]==0xE000) picture=true;
            const float advance = picture ? inline_advance : float(advances[g]);
            out.push_back({picture ? nullptr : Find(0x100000 + glyphs[g]), pen, float(offsets[g].du), float(-offsets[g].dv), advance});
            pen += advance;
        }
    }
    return out;
}
}
#endif
