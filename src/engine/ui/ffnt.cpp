#include "engine/ui/ffnt.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace pt::ui {
namespace {

uint16_t U16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool Fail(std::string* error, std::string text) {
    if (error) {
        *error = std::move(text);
    }
    return false;
}

}

std::vector<uint32_t> DecodeUtf8(std::string_view text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        uint32_t code = c;
        int extra = 0;
        if (c >= 0xF0) {
            code = c & 0x07;
            extra = 3;
        } else if (c >= 0xE0) {
            code = c & 0x0F;
            extra = 2;
        } else if (c >= 0xC0) {
            code = c & 0x1F;
            extra = 1;
        }
        ++i;
        for (int k = 0; k < extra && i < text.size(); ++k, ++i) {
            code = (code << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
        }
        out.push_back(code);
    }
    return out;
}

bool FfntFont::Parse(std::span<const uint8_t> data, std::string* error) {
    unicode_.reset();
    glyphs_.clear();
    bitmap_.clear();
    composed_.clear();
    if (data.size() < 0x10 || std::memcmp(data.data(), "FXFT", 4) != 0) {
        return Fail(error, "not an FFNT file");
    }
    const uint8_t entry_count = data[6];
    const uint8_t* glyp = nullptr;
    const uint8_t* ftdt = nullptr;
    for (uint8_t i = 0; i < entry_count; ++i) {
        const size_t at = 0x10 + static_cast<size_t>(i) * 12;
        if (at + 12 > data.size()) {
            return Fail(error, "FFNT entry table truncated");
        }
        const uint32_t offset = U32(data.data() + at + 4);
        if (offset + 16 > data.size()) {
            return Fail(error, "FFNT entry offset out of range");
        }
        if (std::memcmp(data.data() + at, "GLYP", 4) == 0) {
            glyp = data.data() + offset;
        } else if (std::memcmp(data.data() + at, "FTDT", 4) == 0) {
            ftdt = data.data() + offset;
        }
    }
    if (!glyp || !ftdt) {
        return Fail(error, "FFNT without GLYP or FTDT");
    }
    em_size_ = glyp[2];
    pad_ = glyp[12];
    const uint16_t count = U16(glyp + 6);
    const uint8_t* end = data.data() + data.size();
    if (glyp + 16 + static_cast<size_t>(count) * 20 > end || em_size_ == 0) {
        return Fail(error, "FFNT glyph table truncated");
    }
    glyphs_.resize(count);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* g = glyp + 16 + static_cast<size_t>(i) * 20;
        FfntGlyph& glyph = glyphs_[i];
        glyph.code = U32(g) & 0xFFFFFFF;
        glyph.x = U16(g + 4);
        glyph.y = U16(g + 6);
        glyph.width = g[8];
        glyph.height = g[9];
        glyph.layer = g[10];
        glyph.advance = g[11];
        glyph.bearing = g[12];
        glyph.top = static_cast<int8_t>(g[13]);
        glyph.flags = U16(g + 14);
    }
    std::sort(glyphs_.begin(), glyphs_.end(), [](const FfntGlyph& a, const FfntGlyph& b) { return a.code < b.code; });
    bitmap_width_ = 1u << ftdt[1];
    bitmap_height_ = 1u << ftdt[2];
    const uint32_t size = U32(ftdt + 4);
    if (ftdt + 16 + size > end || size < bitmap_width_ * bitmap_height_) {
        return Fail(error, std::format("FFNT bitmap {}x{} does not fit {} bytes", bitmap_width_, bitmap_height_, size));
    }
    bitmap_.assign(ftdt + 16, ftdt + 16 + static_cast<size_t>(bitmap_width_) * bitmap_height_);
    return true;
}

const FfntGlyph* FfntFont::Find(uint32_t code) const {
    auto it = std::lower_bound(glyphs_.begin(), glyphs_.end(), code, [](const FfntGlyph& g, uint32_t c) { return g.code < c; });
    return it != glyphs_.end() && it->code == code ? &*it : nullptr;
}

const FfntGlyph* FfntFont::FindOrFallback(uint32_t code) const {
    if (const FfntGlyph* glyph = Find(code)) {
        return glyph;
    }
    if (const FfntGlyph* glyph = Find(0x25A2)) {
        return glyph;
    }
    return Find('?');
}

float FfntFont::LineFactor() const {
    if (unicode_) return unicode_line_factor_;
    return em_size_ ? static_cast<float>(em_size_ + 2 * pad_) / static_cast<float>(em_size_) : 1.0f;
}

float FfntFont::Advance(const FfntGlyph& glyph) const {
    float extra = static_cast<float>(2 * pad_);
    if (glyph.flags & 1) {
        extra -= static_cast<float>(pad_);
    }
    if (glyph.flags & 2) {
        extra -= static_cast<float>(pad_);
    }
    return (extra + static_cast<float>(glyph.advance)) / static_cast<float>(em_size_);
}

bool FfntFont::Bit(const FfntGlyph& glyph, int x, int y) const {
    if (x < 0 || y < 0 || x >= glyph.width || y >= glyph.height) {
        return false;
    }
    if (const auto it = composed_.find(glyph.code); it != composed_.end()) {
        return it->second[static_cast<size_t>(y) * glyph.width + x] != 0;
    }
    const size_t at = static_cast<size_t>(glyph.y + y) * bitmap_width_ + glyph.x + x;
    return at < bitmap_.size() && (bitmap_[at] & (1u << (glyph.layer & 7))) != 0;
}

void FfntFont::AddTurkishGlyphs() {
    // Retain the original Latin letter shapes and advances; add only missing Turkish marks.
    struct Addition { uint32_t code, base; int kind; };
    for (const auto a : {Addition{0x11e,'G',0}, {0x11f,'g',0}, {0x130,'I',1}, {0x131,'i',2}, {0x15e,'S',3}, {0x15f,'s',3}}) {
        if (Find(a.code)) continue;
        const auto* original = Find(a.base);
        if (!original) continue;
        const FfntGlyph base = *original;
        FfntGlyph glyph = base; glyph.code = a.code;
        const int above = (a.kind == 0 || a.kind == 1) ? 5 : 0;
        const int below = a.kind == 3 ? 4 : 0;
        glyph.height = static_cast<uint8_t>(base.height + above + below);
        glyph.top = static_cast<int8_t>(base.top - above);
        std::vector<uint8_t> pixels(static_cast<size_t>(glyph.width) * glyph.height);
        const auto put = [&](int x,int y) {
            if (x>=0 && x<glyph.width && y>=0 && y<glyph.height) pixels[y*glyph.width+x]=1;
        };
        for(int y=0;y<base.height;++y) for(int x=0;x<base.width;++x) if(Bit(base,x,y)) put(x,y+above);
        const int cx = glyph.width / 2;
        if(a.kind==0) {
            for(int x=-3;x<=3;++x) { put(cx+x,std::abs(x)>=2 ? 1 : 2); put(cx+x,std::abs(x)>=2 ? 2 : 3); }
        } else if(a.kind==1) { put(cx,1); put(cx+1,1); put(cx,2); put(cx+1,2); }
        else if(a.kind==2) {
            // Remove the isolated dot above the original lowercase i, stopping at its first blank row.
            bool ink=false; int gap=-1;
            for(int y=0;y<base.height;++y) {
                bool row=false; for(int x=0;x<base.width;++x) row |= Bit(base,x,y);
                if(ink && !row) { gap=y; break; } ink |= row;
            }
            if(gap>=0) for(int y=0;y<gap;++y) for(int x=0;x<glyph.width;++x) pixels[y*glyph.width+x]=0;
        } else {
            put(cx,base.height); put(cx-1,base.height+1); put(cx,base.height+2); put(cx+1,base.height+2);
            put(cx-1,base.height+3); put(cx,base.height+3);
        }
        composed_[a.code]=std::move(pixels); glyphs_.push_back(glyph);
        std::sort(glyphs_.begin(),glyphs_.end(),[](const FfntGlyph& a,const FfntGlyph& b){return a.code<b.code;});
    }
}

bool FfntFont::BuildAtlas(std::vector<std::vector<uint8_t>>& mips, uint32_t mip_count, bool soften) {
    mip_count = std::max(1u, mip_count);
    const uint32_t align = 1u << (mip_count - 1);
    const uint32_t border = std::max(2u, align);
    const uint32_t width = 1024;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t row = 0;
    auto round_up = [&](uint32_t v) { return (v + align - 1) / align * align; };
    for (FfntGlyph& glyph : glyphs_) {
        const uint32_t cell_w = round_up(glyph.width + 2 * border);
        const uint32_t cell_h = round_up(glyph.height + 2 * border);
        if (x + cell_w > width) {
            x = 0;
            y += row;
            row = 0;
        }
        glyph.atlas_x = static_cast<uint16_t>(x + border);
        glyph.atlas_y = static_cast<uint16_t>(y + border);
        x += cell_w;
        row = std::max(row, cell_h);
    }
    uint32_t height = align;
    while (height < y + row) {
        height *= 2;
    }
    atlas_width_ = width;
    atlas_height_ = height;
    mips.assign(1, std::vector<uint8_t>(static_cast<size_t>(width) * height, 0));
    for (const FfntGlyph& glyph : glyphs_) {
        auto bit = [&](int gx, int gy) -> float {
            return static_cast<float>(Coverage(glyph, gx, gy));
        };
        for (int gy = 0; gy < glyph.height; ++gy) {
            for (int gx = 0; gx < glyph.width; ++gx) {
                const float v = bit(gx - 1, gy - 1) * 0.0625f + bit(gx, gy - 1) * 0.125f + bit(gx + 1, gy - 1) * 0.0625f + bit(gx - 1, gy) * 0.125f +
                                bit(gx, gy) * 0.25f + bit(gx + 1, gy) * 0.125f + bit(gx - 1, gy + 1) * 0.0625f + bit(gx, gy + 1) * 0.125f +
                                bit(gx + 1, gy + 1) * 0.0625f;
                mips[0][static_cast<size_t>(glyph.atlas_y + gy) * width + glyph.atlas_x + gx] = static_cast<uint8_t>(soften ? v : bit(gx, gy));
            }
        }
    }
    for (uint32_t level = 1; level < mip_count; ++level) {
        const uint32_t pw = width >> (level - 1);
        const uint32_t ph = height >> (level - 1);
        const uint32_t w = std::max(1u, pw / 2);
        const uint32_t h = std::max(1u, ph / 2);
        const std::vector<uint8_t>& src = mips[level - 1];
        std::vector<uint8_t> dst(static_cast<size_t>(w) * h);
        for (uint32_t my = 0; my < h; ++my) {
            for (uint32_t mx = 0; mx < w; ++mx) {
                const uint32_t sx = std::min(mx * 2, pw - 1);
                const uint32_t sy = std::min(my * 2, ph - 1);
                const uint32_t sx1 = std::min(sx + 1, pw - 1);
                const uint32_t sy1 = std::min(sy + 1, ph - 1);
                const uint32_t sum = src[sy * pw + sx] + src[sy * pw + sx1] + src[sy1 * pw + sx] + src[sy1 * pw + sx1];
                dst[my * w + mx] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
        mips.push_back(std::move(dst));
    }
    return true;
}

}
