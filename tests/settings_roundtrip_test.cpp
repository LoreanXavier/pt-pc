#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "engine/platform/settings.h"

namespace {

int failures = 0;

void Check(const std::string& name, bool ok) {
    std::printf("%s: %s\n", name.c_str(), ok ? "PASS" : "FAIL");
    failures += ok ? 0 : 1;
}

bool Near(float a, float b) { return std::abs(a - b) <= 1e-5f * std::max(1.0f, std::abs(b)); }

bool Same(const pt::AppSettings& a, const pt::AppSettings& b) {
    const auto& x = a.display;
    const auto& y = b.display;
    return x.width == y.width && x.height == y.height && x.fullscreen == y.fullscreen && x.vsync == y.vsync &&
           x.pause_on_focus_loss == y.pause_on_focus_loss && x.mute_in_background == y.mute_in_background && x.letterbox == y.letterbox &&
           Near(a.input.mouse_sensitivity, b.input.mouse_sensitivity) && Near(a.input.gamepad_dead_zone, b.input.gamepad_dead_zone) &&
           a.input.rumble == b.input.rumble && Near(a.camera.roll, b.camera.roll) && a.camera.third_person == b.camera.third_person && Near(a.audio.volume, b.audio.volume) &&
           a.voice.device == b.voice.device && a.voice.key == b.voice.key && a.upscaling.upscaler == b.upscaling.upscaler &&
           a.upscaling.quality == b.upscaling.quality && Near(a.upscaling.scale, b.upscaling.scale) &&
           Near(a.upscaling.sharpness, b.upscaling.sharpness) && a.upscaling.dlss_model == b.upscaling.dlss_model &&
           a.upscaling.frame_generation == b.upscaling.frame_generation &&
           a.ray_tracing == b.ray_tracing && a.graphics == b.graphics && a.mods == b.mods && a.vr == b.vr;
}

pt::AppSettings RoundTrip(const std::filesystem::path& path, const pt::AppSettings& s) {
    pt::AppSettings out;
    if (!pt::SaveAppSettings(path, s) || !pt::LoadAppSettings(path, out)) ++failures;
    return out;
}

pt::AppSettings LoadText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
    pt::AppSettings out;
    if (!pt::LoadAppSettings(path, out)) ++failures;
    return out;
}

}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    const std::filesystem::path ini = dir / "roundtrip.ini";

    Check("defaults", Same(RoundTrip(ini, {}), {}));
    const std::vector<std::pair<std::string, std::function<void(pt::AppSettings&)>>> changes = {
        {"display.width/height", [](auto& s) { s.display.width = 2560; s.display.height = 1080; }},
        {"display.fullscreen", [](auto& s) { s.display.fullscreen = 2; }},
        {"display.vsync", [](auto& s) { s.display.vsync = false; }},
        {"display.pause_on_focus_loss", [](auto& s) { s.display.pause_on_focus_loss = false; }},
        {"display.mute_in_background", [](auto& s) { s.display.mute_in_background = true; }},
        {"display.letterbox 1", [](auto& s) { s.display.letterbox = 1; }},
        {"display.letterbox 2", [](auto& s) { s.display.letterbox = 2; }},
        {"input.mouse_sensitivity", [](auto& s) { s.input.mouse_sensitivity = 0.35f; }},
        {"input.gamepad_dead_zone", [](auto& s) { s.input.gamepad_dead_zone = 0.25f; }},
        {"input.rumble", [](auto& s) { s.input.rumble = false; }},
        {"camera.roll", [](auto& s) { s.camera.roll = 0.0f; }},
        {"camera.third_person", [](auto& s) { s.camera.third_person = true; }},
        {"audio.volume", [](auto& s) { s.audio.volume = 1.7f; }},
        {"voice.device", [](auto& s) { s.voice.device = "Microphone (USB Audio Device)"; }},
        {"voice.key", [](auto& s) { s.voice.key = "J"; }},
        {"upscaling.upscaler", [](auto& s) { s.upscaling.upscaler = "dlss"; }},
        {"upscaling.quality", [](auto& s) { s.upscaling.quality = "ultra_performance"; }},
        {"upscaling.scale", [](auto& s) { s.upscaling.scale = 0.5f; }},
        {"upscaling.sharpness", [](auto& s) { s.upscaling.sharpness = 0.7f; }},
        {"upscaling.dlss_model", [](auto& s) { s.upscaling.dlss_model = "k"; }},
        {"upscaling.frame_generation", [](auto& s) { s.upscaling.frame_generation = "dlss"; }},
        {"raytracing.shadows", [](auto& s) { s.ray_tracing.shadows = 2; }},
        {"raytracing.ambient_occlusion", [](auto& s) { s.ray_tracing.ambient_occlusion = true; }},
        {"raytracing.contact_shadows", [](auto& s) { s.ray_tracing.contact_shadows = true; }},
        {"raytracing.reflections", [](auto& s) { s.ray_tracing.reflections = true; }},
        {"graphics.anisotropy", [](auto& s) { s.graphics.anisotropy = 8; }},
        {"graphics.enhanced_textures", [](auto& s) { s.graphics.enhanced_textures = true; }},
        {"graphics.shadow_quality", [](auto& s) { s.graphics.shadow_quality = 0; }},
        {"graphics.ambient_occlusion", [](auto& s) { s.graphics.ambient_occlusion = false; }},
        {"graphics.reflections", [](auto& s) { s.graphics.reflections = false; }},
        {"graphics.bloom", [](auto& s) { s.graphics.bloom = false; }},
        {"graphics.depth_of_field", [](auto& s) { s.graphics.depth_of_field = false; }},
        {"graphics.motion_blur", [](auto& s) { s.graphics.motion_blur = false; }},
        {"graphics.lens_distortion", [](auto& s) { s.graphics.lens_distortion = false; }},
        {"graphics.lens_ghosts", [](auto& s) { s.graphics.lens_ghosts = false; }},
        {"graphics.film_grain", [](auto& s) { s.graphics.film_grain = 0.75f; }},
        {"graphics.clarity", [](auto& s) { s.graphics.clarity = 0.4f; }},
        {"graphics.texture_detail", [](auto& s) { s.graphics.texture_detail = 0; }},
        {"graphics.ray_quality", [](auto& s) { s.graphics.ray_quality = 2; }},
        {"mods", [](auto& s) { s.mods["a mod"] = false; s.mods["other"] = true; }},
        {"vr.enabled", [](auto& s) { s.vr.enabled = true; }},
        {"vr.flashlight", [](auto& s) { s.vr.flashlight = 1; }},
        {"vr.flashlight_hand", [](auto& s) { s.vr.flashlight_hand = 1; }},
        {"vr.turn", [](auto& s) { s.vr.turn = 1; }},
        {"vr.snap_degrees", [](auto& s) { s.vr.snap_degrees = 45.0f; }},
        {"vr.smooth_speed", [](auto& s) { s.vr.smooth_speed = 120.0f; }},
        {"vr.resolution_scale", [](auto& s) { s.vr.resolution_scale = 0.8f; }},
    };
    for (const auto& [name, change] : changes) {
        pt::AppSettings s;
        change(s);
        const pt::AppSettings loaded = RoundTrip(ini, s);
        Check(name + " changes the settings", !Same(s, pt::AppSettings{}));
        Check(name + " round trip", Same(loaded, s));
    }
    const pt::AppSettings odd = LoadText(ini,
        "[display]\nletterbox = 7\nfullscreen = -3\nwidth = 99999\n[graphics]\nanisotropy = 5\nfilm_grain = nan\nclarity = 3\n"
        "shadow_quality = 9\ntexture_detail = -1\n[raytracing]\nshadows = 4\n[audio]\nvolume = loud\n[input]\nrumble = yes\n[nothing]\nx = 1\n"
        "[vr]\nturn = 3\nresolution_scale = 9\nsnap_degrees = nan\n");
    Check("letterbox clamped", odd.display.letterbox == 2);
    Check("fullscreen clamped", odd.display.fullscreen == 0);
    Check("width clamped", odd.display.width == 16384);
    Check("anisotropy rounded down to a step", odd.graphics.anisotropy == 4);
    Check("film grain nan is the original's", odd.graphics.film_grain == 1.0f);
    Check("clarity clamped", odd.graphics.clarity == 1.0f);
    Check("shadow quality clamped", odd.graphics.shadow_quality == 3);
    Check("texture detail clamped", odd.graphics.texture_detail == 0);
    Check("traced shadows clamped", odd.ray_tracing.shadows == 2);
    Check("malformed volume keeps the default", odd.audio.volume == 1.0f);
    Check("yes reads as on", odd.input.rumble);
    Check("vr turn clamped", odd.vr.turn == 1);
    Check("vr resolution scale clamped", odd.vr.resolution_scale == 2.0f);
    Check("vr snap nan is the default", odd.vr.snap_degrees == 30.0f);
    Check("vr off by default", !pt::AppSettings{}.vr.enabled);
    std::filesystem::remove(ini);
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
