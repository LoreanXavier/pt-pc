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
        g.depth_of_field=false;g.motion_blur=false;g.lens_distortion=false;g.lens_ghosts=false;g.film_grain=.25f;g.texture_detail=0;g.deband=true;
        break;
    case GraphicsPreset::High:
        g.anisotropy=16;g.texture_detail=2;
        g.motion_blur=false;g.clarity=.25f;g.deband=true;
        g.flare_strength=50;g.ghost_strength=25;g.film_grain=.5f;g.film_grain_type=1;
        break;
    case GraphicsPreset::Ultra:
        g.anisotropy=16;g.texture_detail=2;g.ray_quality=3;
        g.depth_of_field=true;g.motion_blur=true;g.bloom=true;g.lens_distortion=true;g.clarity=.4f;g.deband=true;
        g.flare_strength=50;g.ghost_strength=25;g.film_grain=.5f;g.film_grain_type=1;
        if(rt) {s.ray_tracing.shadows=2;s.ray_tracing.reflections=true;s.ray_tracing.ambient_occlusion=true;s.ray_tracing.contact_shadows=true;}
        if(rt) g.ray_quality=4;
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
void MoveOldGraphicsPreset(AppSettings& s) {
    for(const bool rt:{false,true}) {
        for(auto p:{GraphicsPreset::High,GraphicsPreset::Ultra}) {
            AppSettings now;ApplyGraphicsPreset(now,p,rt);
            AppSettings old=now;
            old.graphics.flare_strength=100;old.graphics.ghost_strength=100;old.graphics.film_grain_type=0;
            old.graphics.lens_ghosts=false;old.graphics.film_grain=p==GraphicsPreset::High?.25f:1.0f;
            if(old.graphics==s.graphics && old.ray_tracing==s.ray_tracing) {s.graphics=now.graphics;return;}
        }
    }
}
}
