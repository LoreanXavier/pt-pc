#include <cstdio>
#include <algorithm>
#include "engine/ui/ffnt.h"
#include "engine/ui/text_layout.h"
#include "engine/core/localized_text.h"
#include "engine/audio/subtitles.h"
#include "engine/fs/vfs.h"
int main(int argc, char** argv) {
 if(argc!=2)return 2;pt::Vfs vfs;if(!vfs.Mount(argv[1]))return 2;
 pt::audio::SubtitleTable english;std::string error;if(!english.Load(vfs,"Eng",{},&error))return 2;
 int failures=0;const char* codes[]={"Tur","Zhs","Ara","Rus","Ukr","Ces"};
 for(int lang=7;lang<13;++lang){
  pt::audio::SubtitleTable translated;
  if(!translated.Load(vfs,codes[lang-7],{},&error)){printf("FAIL: %s\n",error.c_str());++failures;continue;}
  pt::ui::FfntFont font;auto family=lang==8?"Noto Sans SC":lang==9?"Noto Kufi Arabic":"Noto Sans";
  if(!font.LoadUnicodeFont(family,pt::localized::Characters(lang),lang==9,&error)){printf("FAIL: font %s: %s\n",family,error.c_str());++failures;continue;}
  for(const char* arrow:{"<",">"}){auto s=font.ShapeLine(pt::ui::DecodeUtf8(arrow),0);const auto* own=font.Find(uint8_t(arrow[0]));
   bool same=s.size()==1&&s[0].glyph&&own&&s[0].glyph->width==own->width&&s[0].glyph->height==own->height;
   for(int y=0;same&&y<own->height;++y)for(int x=0;same&&x<own->width;++x)same=font.Coverage(*s[0].glyph,x,y)==font.Coverage(*own,x,y);
   if(!same){printf("FAIL: arrow %s mirrored or missing, language %d\n",arrow,lang);++failures;}}
  if(lang==9){pt::ui::FfntFont naskh;if(!naskh.LoadUnicodeFont("Noto Naskh Arabic",pt::localized::Characters(lang),true,&error)){printf("FAIL: font Noto Naskh Arabic: %s\n",error.c_str());++failures;}}
  auto check=[&](std::string_view text){std::string plain(text);plain.erase(std::remove(plain.begin(),plain.end(),'\r'),plain.end());std::replace(plain.begin(),plain.end(),'\n',' ');auto cp=pt::ui::DecodeUtf8(plain);for(auto c:cp)if(c==0xFFFD)++failures;
   auto shaped=font.ShapeLine(cp,24);if(!text.empty()&&shaped.empty()){printf("FAIL: shape %d\n",lang);++failures;}
   for(auto g:shaped)if(!g.glyph){printf("FAIL: shaped glyph %d\n",lang);++failures;}
  };
  for(const auto& e:pt::turkish::kMenu){auto value=pt::localized::Menu(e.key,lang);
   if(value.empty()){printf("FAIL: missing %.*s language %d\n",int(e.key.size()),e.key.data(),lang);++failures;}else check(value);
   if(e.text.find("{accept}")!=std::string_view::npos&&value.find("{accept}")==std::string_view::npos)++failures;
  }
  size_t lines=0;
  for(const auto& e:english.Entries()){auto* t=translated.FindByKey(e.key);
   if(!t||t->lines.size()!=e.lines.size()||t->range!=e.range||t->category!=e.category){++failures;continue;}
   for(size_t i=0;i<t->lines.size();++i){const auto& a=e.lines[i];const auto& b=t->lines[i];
    if(b.text.empty()||a.start_seconds!=b.start_seconds||a.end_seconds!=b.end_seconds)++failures;if(b.text.find('|')!=std::string::npos || std::count(a.text.begin(),a.text.end(),'\n')!=std::count(b.text.begin(),b.text.end(),'\n')) {printf("FAIL: subtitle line breaks %d key %08x line %zu expected %d actual %d\n",lang,e.key,i,int(std::count(a.text.begin(),a.text.end(),'\n')),int(std::count(b.text.begin(),b.text.end(),'\n')));++failures;}check(b.text);++lines;
   }
  }
  std::vector<std::vector<uint8_t>> mips;if(!font.BuildAtlas(mips,3,false)||font.AtlasHeight()>65535)++failures;
  pt::ui::TextStyle style;style.font=&font;style.font_width=style.font_height=20;style.text_space=.25f;
  for(const auto& e:pt::turkish::kMenu){auto layout=pt::ui::LayoutText(pt::localized::Menu(e.key,lang),style,300);
   for(const auto& line:layout.lines)if(line.width>300.01f){printf("FAIL: wrap %d width %f\n",lang,line.width);++failures;}
  }
  {
   pt::ui::TextStyle subtitle;subtitle.font=&font;subtitle.font_width=subtitle.font_height=32;subtitle.text_space=-6;subtitle.line_space=-4;
   pt::ui::NormalizeSubtitleStyle(subtitle);
   auto layout=pt::ui::LayoutText("Jack Jack\nJack",subtitle,1024);
   if(subtitle.font_width!=22 || subtitle.text_space!=0 || std::abs(layout.line_pitch-26)>0.01f) {printf("FAIL: localized subtitle metrics %d\n",lang);++failures;}
   for(const auto& e:translated.Entries())for(const auto& l:e.lines){auto t=pt::ui::LayoutText(l.text,subtitle,1024);for(const auto& row:t.lines)if(row.width>1024.01f)++failures;}
  }
  if(lang==9){
   auto joined=font.ShapeLine(pt::ui::DecodeUtf8("لا"),0);bool contextual=false;for(const auto& g:joined)if(g.glyph && g.glyph->code>=0x100000)contextual=true;
   auto separate=font.ShapeLine(pt::ui::DecodeUtf8("ل ا"),0); auto lam=font.ShapeLine(pt::ui::DecodeUtf8("ل"),0),alef=font.ShapeLine(pt::ui::DecodeUtf8("ا"),0);
   bool joined_forms=joined.size()!=2 || (joined[0].glyph!=alef[0].glyph || joined[1].glyph!=lam[0].glyph);if(!contextual || !joined_forms || joined.size()>=separate.size()){printf("FAIL: Arabic joining\n");++failures;}
   auto mixed=font.ShapeLine(pt::ui::DecodeUtf8("مرحبا Jack 204863"),0),digits=font.ShapeLine(pt::ui::DecodeUtf8("204863"),0);bool digit_run=false;
   for(size_t i=0;i+digits.size()<=mixed.size();++i){bool match=true;for(size_t j=0;j<digits.size();++j)match&=mixed[i+j].glyph==digits[j].glyph;digit_run|=match;}
   auto jack=font.ShapeLine(pt::ui::DecodeUtf8("Jack"),0);if(mixed.empty() || mixed[0].glyph!=jack[0].glyph){printf("FAIL: Arabic paragraph ordering\n");++failures;}
   if(!digit_run){printf("FAIL: Arabic puzzle digits\n");++failures;}
   auto resolution=font.ShapeLine(pt::ui::DecodeUtf8("1280 x 720"),0),one=font.ShapeLine(pt::ui::DecodeUtf8("1"),0);
   if(resolution.empty() || resolution[0].glyph!=one[0].glyph){printf("FAIL: resolution order\n");++failures;}
   auto picture=font.ShapeLine(pt::ui::DecodeUtf8("اضغط \xEE\x80\x80 الآن"),30);int inline_count=0;
   for(const auto& g:picture)if(!g.glyph&&g.advance==30)++inline_count;
   if(inline_count!=1){printf("FAIL: Arabic inline prompt\n");++failures;}
  }
  printf("language %d: %zu entries/%zu lines, %zu glyphs, atlas %ux%u; failures %d\n",lang,translated.Entries().size(),lines,font.Glyphs().size(),font.AtlasWidth(),font.AtlasHeight(),failures);
 }
 printf("multilingual: %d failures\n",failures);return failures?1:0;
}
