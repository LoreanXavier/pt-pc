#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "engine/platform/graphics_presets.h"
int main(int argc,char** argv) {
    int failures=0;
    const auto check=[&](const char* name,bool ok){std::printf("%s: %s\n",name,ok?"PASS":"FAIL");failures+=!ok;};
    pt::AppSettings s;
    s.display.width=2560;s.display.height=1440;
    s.upscaling.upscaler="dlss";s.upscaling.scale=.8f;s.upscaling.quality="native";
    s.voice.device="test microphone";s.audio.volume=.6f;
    pt::ApplyGraphicsPreset(s,pt::GraphicsPreset::Low,true);
    check("Low reduces effects and shadows",s.graphics.shadow_quality==1 && !s.graphics.reflections && !s.graphics.ambient_occlusion && !s.graphics.bloom);
    check("Low retains output size and upscale choice",s.display.width==2560 && s.display.height==1440 && s.upscaling.scale==.8f && s.upscaling.upscaler=="dlss" && s.upscaling.quality=="native");
    check("preset retains sound and microphone",s.voice.device=="test microphone" && s.audio.volume==.6f);
    check("Low detected",pt::DetectGraphicsPreset(s,true)==pt::GraphicsPreset::Low);
    pt::ApplyGraphicsPreset(s,pt::GraphicsPreset::Original,true);
    check("Original renderer defaults",s.graphics==pt::AppSettings{}.graphics && s.ray_tracing==pt::AppSettings{}.ray_tracing);
    check("Original detected",pt::DetectGraphicsPreset(s,true)==pt::GraphicsPreset::Original);
    check("Original draws the flare ghosts",s.graphics.lens_ghosts);
    s.ray_tracing.contact_shadows=true;
    check("Original then contact shadows is Custom with the ghosts still on",pt::DetectGraphicsPreset(s,true)==pt::GraphicsPreset::Custom && s.graphics.lens_ghosts);
    pt::ApplyGraphicsPreset(s,pt::GraphicsPreset::High,true);
    check("High leaves the flare ghosts out",!s.graphics.lens_ghosts && pt::DetectGraphicsPreset(s,true)==pt::GraphicsPreset::High);
    s.graphics.lens_ghosts=true;
    check("ghosts by hand make High Custom",pt::DetectGraphicsPreset(s,true)==pt::GraphicsPreset::Custom);
    s.graphics.lens_ghosts=false;
    pt::ApplyGraphicsPreset(s,pt::GraphicsPreset::Ultra,true);
    check("Ultra has every traced effect and fine detail",s.ray_tracing.shadows==2 && s.ray_tracing.reflections && s.ray_tracing.ambient_occlusion && s.ray_tracing.contact_shadows && s.graphics.enhanced_textures && s.graphics.ray_quality==2 && s.graphics.clarity==.4f);
    check("Ultra preserves the complete cinematic effects",s.graphics.depth_of_field && s.graphics.motion_blur && s.graphics.bloom && s.graphics.lens_distortion && s.graphics.film_grain==1.0f);
    check("Ultra detected",pt::DetectGraphicsPreset(s,true)==pt::GraphicsPreset::Ultra);
    s.graphics.bloom=false;
    check("manual change becomes Custom",pt::DetectGraphicsPreset(s,true)==pt::GraphicsPreset::Custom);
    pt::ApplyGraphicsPreset(s,pt::GraphicsPreset::Ultra,false);
    check("unsupported RT retains enhanced fallback",s.ray_tracing==pt::AppSettings{}.ray_tracing && s.graphics.enhanced_textures && s.graphics.ambient_occlusion && s.graphics.reflections);
    check("unsupported Ultra keeps cinematic effects",s.graphics.depth_of_field && s.graphics.motion_blur && s.graphics.bloom && s.graphics.lens_distortion && s.graphics.film_grain==1.0f);
    check("unsupported Ultra detected",pt::DetectGraphicsPreset(s,false)==pt::GraphicsPreset::Ultra);
    if(argc==2) {
        const auto path=std::filesystem::path(argv[1]);
        check("save graphics settings",pt::SaveAppSettings(path,s));
        pt::AppSettings restored;
        check("load graphics settings",pt::LoadAppSettings(path,restored));
        check("graphics persistence",restored.graphics==s.graphics && restored.ray_tracing==s.ray_tracing);
        const auto without_key=[&](pt::GraphicsPreset preset){
            pt::AppSettings written;pt::ApplyGraphicsPreset(written,preset,true);
            if(!pt::SaveAppSettings(path,written)) return false;
            std::string text;{std::ifstream in(path);std::stringstream ss;ss<<in.rdbuf();text=ss.str();}
            const auto at=text.find("lens_ghosts = ");if(at==std::string::npos) return false;
            text.erase(at,text.find('\n',at)+1-at);
            {std::ofstream out(path);out<<text;}
            pt::AppSettings loaded;return pt::LoadAppSettings(path,loaded) && loaded.graphics.lens_ghosts==(preset==pt::GraphicsPreset::Original) &&
                   pt::DetectGraphicsPreset(loaded,true)==preset;
        };
        check("missing lens_ghosts keeps High as High without ghosts",without_key(pt::GraphicsPreset::High));
        check("missing lens_ghosts keeps Original with ghosts",without_key(pt::GraphicsPreset::Original));
        check("missing lens_ghosts keeps Ultra as Ultra",without_key(pt::GraphicsPreset::Ultra));
    }
    return failures?1:0;
}
