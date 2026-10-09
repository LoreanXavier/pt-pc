#include <cstdio>
#include <string>
#include "engine/fs/vfs.h"
#include "engine/audio/subtitles.h"
#include "engine/ui/ffnt.h"
#include "engine/core/localized_text.h"
#include "game/save_data.h"

int main(int argc,char** argv) {
    if(argc!=2) return 2;
    pt::Vfs vfs; if(!vfs.Mount(argv[1])) return 2;
    pt::audio::SubtitleTable english,turkish; std::string error;
    if(!english.Load(vfs,"Eng",{},&error) || !turkish.Load(vfs,"Tur",{},&error)) {
        std::printf("FAIL: %s\n",error.c_str()); return 1;
    }
    int failures=0; size_t lines=0;
    for(const auto& entry:pt::turkish::kMenu) {
        for(uint32_t code:pt::ui::DecodeUtf8(entry.text)) {
            if(code==0xC3 || code==0xC2 || code==0xFFFD) { ++failures; std::printf("FAIL: text encoding %.*s\n", int(entry.key.size()), entry.key.data()); }
        }
    }
    for(const auto& e : english.Entries()) {
        const auto* t=turkish.FindByKey(e.key);
        if(!t || t->lines.size()!=e.lines.size() || t->category!=e.category || t->range!=e.range) {++failures;continue;}
        for(size_t i=0;i<e.lines.size();++i) {
            const auto& a=e.lines[i]; const auto& b=t->lines[i];
            failures += a.start_seconds!=b.start_seconds || a.end_seconds!=b.end_seconds || b.text.empty();
            ++lines;
        }
    }
    std::printf("Turkish subtitles: %zu entries, %zu lines, %d failures\n",turkish.Entries().size(),lines,failures);
    vfs.LoadPackage("/Assets/sh/ui/ui_default_lang.fpkd");
    for(const char* path : {"/Assets/sh/font/font_def_ltn.ffnt","/Assets/sh/font/LatinFont.ffnt"}) {
        auto bytes=vfs.ReadFile(path); pt::ui::FfntFont font;
        if(!bytes || !font.Parse(*bytes,&error)) {std::printf("FAIL: font %s\n",path);++failures;continue;}
        const auto before=font.Glyphs().size();
        const auto original_glyphs=font.Glyphs();
        std::vector<std::vector<uint8_t>> original_pixels;
        for(const auto& g:original_glyphs) {
            std::vector<uint8_t> pixels;
            for(int y=0;y<g.height;++y) for(int x=0;x<g.width;++x) pixels.push_back(font.Bit(g,x,y));
            original_pixels.push_back(std::move(pixels));
        }
        font.AddTurkishGlyphs();
        for(size_t i=0;i<original_glyphs.size();++i) {
            const auto& old=original_glyphs[i]; const auto* now=font.Find(old.code);
            if(!now || now->advance!=old.advance || now->top!=old.top || now->width!=old.width || now->height!=old.height) {++failures;continue;}
            size_t pixel=0;
            for(int y=0;y<old.height;++y) for(int x=0;x<old.width;++x) failures += font.Bit(*now,x,y)!=bool(original_pixels[i][pixel++]);
        }
        for(uint32_t code : pt::ui::DecodeUtf8("ÇçĞğİıÖöŞşÜü")) {
            const auto* glyph=font.Find(code); bool ink=false;
            if(glyph) for(int y=0;y<glyph->height;++y) for(int x=0;x<glyph->width;++x) ink |= font.Bit(*glyph,x,y);
            failures += !ink;
            if(!ink) std::printf("FAIL: glyph U+%04x\n",code);
        }
        std::vector<std::vector<uint8_t>> mips; failures += !font.BuildAtlas(mips,1);
        failures += !font.BuildAtlas(mips,3,false) || mips.size()!=3;
        for(const auto& g:font.Glyphs())
            for(int y=0;y<g.height;++y) for(int x=0;x<g.width;++x)
                failures += mips[0][(g.atlas_y+y)*font.AtlasWidth()+g.atlas_x+x] != (font.Bit(g,x,y)?255:0);
        std::printf("Turkish font %s: %zu -> %zu glyphs\n",path,before,font.Glyphs().size());
    }
    {
        const char* names[]={"","French","German","Spanish","Japanese","Italian","Portuguese"};
        for(int lang=1;lang<=6;++lang) {
            const char* path=lang==4?"/Assets/sh/font/font_def_jp.ffnt":"/Assets/sh/font/font_def_ltn.ffnt";
            auto bytes=vfs.ReadFile(path); pt::ui::FfntFont font;
            if(!bytes || !font.Parse(*bytes,&error)) {std::printf("FAIL: font %s\n",path);++failures;continue;}
            int count=0,local=0;
            auto tokens=[](std::string_view s){std::string out;for(size_t i=s.find('{');i!=std::string_view::npos;i=s.find('{',i+1))out+=s.substr(i,s.find('}',i)-i+1);return out;};
            for(const auto& e:pt::chinese::kMenu) {
                const bool wanted=e.key.starts_with("pc_") || e.key=="op_sub_turkish" || e.key=="op_sub_chinese" || e.key=="op_sub_arabic" ||
                                  e.key=="op_sub_russian" || e.key=="op_sub_ukrainian" || e.key=="op_sub_czech" || e.key=="op_sub_polish";
                if(!wanted) continue;
                const auto text=pt::localized::Menu(e.key,lang); ++count;
                if(text.empty()) {std::printf("FAIL: %s lacks %.*s\n",names[lang],int(e.key.size()),e.key.data());++local;continue;}
                if(tokens(text)!=tokens(pt::localized::Menu(e.key,8))) {std::printf("FAIL: %s tokens of %.*s\n",names[lang],int(e.key.size()),e.key.data());++local;}
                for(uint32_t code:pt::ui::DecodeUtf8(text)) if(code!=0x0A && !font.Find(code)) {
                    std::printf("FAIL: %s %.*s has U+%04X, not in %s\n",names[lang],int(e.key.size()),e.key.data(),code,path);++local;
                }
            }
            std::printf("%s: %d texts, %d failures\n",names[lang],count,local);
            failures+=local;
        }
    }
    std::printf("localization: %d failures\n",failures);
    return failures ? 1 : 0;
}
