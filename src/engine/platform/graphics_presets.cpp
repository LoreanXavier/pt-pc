#include "engine/platform/graphics_presets.h"
namespace pt {
void ApplyGraphicsPreset(AppSettings& s, GraphicsPreset preset, bool rt) {
    if(preset==GraphicsPreset::Custom) return;
    s.graphics=AppSettings::Graphics{};
    s.ray_tracing=AppSettings::RayTracing{};
    auto& g=s.graphics;
    switch(preset) {
    case GraphicsPreset::Low:
        g.shadow_quality=1;g.ambient_occlusion=false;g.reflections=false;g.bloom=false;
        g.depth_of_field=false;g.motion_blur=false;g.lens_distortion=false;g.lens_ghosts=false;g.film_grain=.25f;g.texture_detail=0;
        break;
    case GraphicsPreset::High:
        g.anisotropy=16;g.enhanced_textures=true;g.texture_detail=2;
        g.motion_blur=false;g.lens_ghosts=false;g.film_grain=.25f;g.clarity=.25f;
        break;
    case GraphicsPreset::Ultra:
        g.anisotropy=16;g.enhanced_textures=true;g.texture_detail=2;g.ray_quality=2;
        g.depth_of_field=true;g.motion_blur=true;g.bloom=true;g.lens_distortion=true;g.film_grain=1.0f;g.clarity=.4f;
        g.lens_ghosts=false;
        if(rt) {s.ray_tracing.shadows=2;s.ray_tracing.reflections=true;s.ray_tracing.ambient_occlusion=true;s.ray_tracing.contact_shadows=true;}
        break;
    default: break;
    }
}
GraphicsPreset DetectGraphicsPreset(const AppSettings& s, bool rt) {
    for(auto p:{GraphicsPreset::Low,GraphicsPreset::Original,GraphicsPreset::High,GraphicsPreset::Ultra}) {
        AppSettings candidate;ApplyGraphicsPreset(candidate,p,rt);
        if(candidate.graphics==s.graphics && candidate.ray_tracing==s.ray_tracing) return p;
    }
    return GraphicsPreset::Custom;
}
}
