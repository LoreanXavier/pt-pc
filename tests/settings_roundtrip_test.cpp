#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
    return x.hdr==y.hdr && x.hdr_paper_white==y.hdr_paper_white && x.hdr_peak==y.hdr_peak && x.fps_limit==y.fps_limit && a.audio.surround==b.audio.surround && a.audio.controller_speaker==b.audio.controller_speaker && a.audio.output_device==b.audio.output_device && Near(a.audio.controller_speaker_volume,b.audio.controller_speaker_volume) && a.input.rumble_profile==b.input.rumble_profile && x.width == y.width && x.height == y.height && x.fullscreen == y.fullscreen && x.vsync == y.vsync &&
           x.letterbox == y.letterbox &&
           Near(a.input.mouse_sensitivity, b.input.mouse_sensitivity) && Near(a.input.gamepad_sensitivity, b.input.gamepad_sensitivity) &&
           Near(a.input.gamepad_dead_zone, b.input.gamepad_dead_zone) &&
           a.input.rumble == b.input.rumble && Near(a.camera.roll, b.camera.roll) && a.camera.third_person == b.camera.third_person && a.game == b.game && Near(a.audio.volume, b.audio.volume) &&
           a.voice.device == b.voice.device && a.voice.key == b.voice.key && a.upscaling.upscaler == b.upscaling.upscaler &&
           a.upscaling.quality == b.upscaling.quality && Near(a.upscaling.scale, b.upscaling.scale) &&
           Near(a.upscaling.sharpness, b.upscaling.sharpness) && a.upscaling.dlss_model == b.upscaling.dlss_model &&
           a.upscaling.frame_generation == b.upscaling.frame_generation && a.upscaling.dlssg_multiplier == b.upscaling.dlssg_multiplier && a.upscaling.ray_reconstruction == b.upscaling.ray_reconstruction &&
           a.upscaling.neural_model == b.upscaling.neural_model && a.upscaling.neural_quality == b.upscaling.neural_quality &&
           Near(a.upscaling.neural_intensity, b.upscaling.neural_intensity) &&
           a.ray_tracing == b.ray_tracing && a.graphics == b.graphics && a.mods == b.mods && a.vr == b.vr && a.accessibility == b.accessibility && a.progress == b.progress;
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

    {
        const auto old = LoadText(ini, "[upscaling]\nneural_quality=performance\n");
        Check("legacy Performance keeps its Small model", old.upscaling.neural_model == "small");
        const auto balanced = LoadText(ini, "[upscaling]\nneural_quality=balanced\n");
        Check("legacy Balanced migrates to Quality, new model", balanced.upscaling.neural_quality == "quality" && balanced.upscaling.neural_model == "small");
        const auto explicit_model = LoadText(ini, "[upscaling]\nneural_quality=performance\nneural_model=large\nneural_intensity=4\n");
        Check("explicit model independent of quality", explicit_model.upscaling.neural_model == "large");
        Check("neural strength capped at 300 percent", Near(explicit_model.upscaling.neural_intensity, 3.0f));
        Check("neural strength minimum", Near(LoadText(ini, "[upscaling]\nneural_intensity=-1\n").upscaling.neural_intensity, .25f));
        Check("neural strength rejects nan", Near(LoadText(ini, "[upscaling]\nneural_intensity=nan\n").upscaling.neural_intensity, 1.0f));
    }

    {
        const auto legacy = LoadText(ini, "[display]\npause_on_focus_loss=1\nmute_in_background=1\n[graphics]\nenhanced_textures=1\ndeband=0\nlens_ghosts=1\nanisotropy=8\ntexture_detail=2\n[audio]\nvolume=0.6\n");
        auto expected = pt::AppSettings{};
        expected.graphics.anisotropy = 8;
        expected.graphics.texture_detail = 2;
        expected.audio.volume = 0.6f;
        Check("removed legacy options are ignored", Same(legacy, expected));
        Check("save legacy settings", pt::SaveAppSettings(ini, legacy));
        std::ifstream in(ini, std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Check("removed options are not saved", saved.find("pause_on_focus_loss") == std::string::npos &&
              saved.find("mute_in_background") == std::string::npos && saved.find("enhanced_textures") == std::string::npos);
    }

    Check("DLSS multiplier below minimum", LoadText(ini, "[upscaling]\ndlssg_multiplier=0\n").upscaling.dlssg_multiplier == 2);
    Check("DLSS multiplier above maximum", LoadText(ini, "[upscaling]\ndlssg_multiplier=8\n").upscaling.dlssg_multiplier == 4);
    Check("DLSS multiplier malformed", LoadText(ini, "[upscaling]\ndlssg_multiplier=wrong\n").upscaling.dlssg_multiplier == 2);

    const std::vector<std::pair<std::string, std::function<void(pt::AppSettings&)>>> changes = {
        {"upscaling.neural_model", [](auto& s) { s.upscaling.neural_model = "small"; }},
        {"upscaling.neural_quality", [](auto& s) { s.upscaling.neural_quality = "ultra"; }},
        {"upscaling.neural_intensity", [](auto& s) { s.upscaling.neural_intensity = 3.0f; }},
        {"display.width/height", [](auto& s) { s.display.width = 2560; s.display.height = 1080; }},
        {"display.hdr", [](auto& s) { s.display.hdr = true; }},
        {"display.fps_limit", [](auto& s) { s.display.fps_limit = 144; }},
        {"display.hdr_paper_white", [](auto& s) { s.display.hdr_paper_white = 320; }},
        {"display.hdr_peak", [](auto& s) { s.display.hdr_peak = 1600; }},
        {"audio.controller_speaker", [](auto& s) { s.audio.controller_speaker = true; }},
        {"audio.controller_speaker_volume", [](auto& s) { s.audio.controller_speaker_volume = 0.25f; }},
        {"audio.surround", [](auto& s) { s.audio.surround = true; }},
        {"audio.output_device", [](auto& s) { s.audio.output_device = "Speakers (DualSense Wireless Controller)"; }},
        {"audio.output_device unicode", [](auto& s) { s.audio.output_device = "耳机 (Realtek(R) Audio)"; }},
        {"input.rumble_profile", [](auto& s) { s.input.rumble_profile = 1; }},
        {"vr.height_offset", [](auto& s) { s.vr.height_offset = -0.2f; }},
        {"vr.world_size", [](auto& s) { s.vr.world_size = 1.5f; }},
        {"display.fullscreen", [](auto& s) { s.display.fullscreen = 2; }},
        {"display.vsync", [](auto& s) { s.display.vsync = false; }},
        {"display.letterbox 1", [](auto& s) { s.display.letterbox = 1; }},
        {"display.letterbox 2", [](auto& s) { s.display.letterbox = 2; }},
        {"input.mouse_sensitivity", [](auto& s) { s.input.mouse_sensitivity = 0.35f; }},
        {"input.gamepad_sensitivity", [](auto& s) { s.input.gamepad_sensitivity = 1.5f; }},
        {"input.gamepad_dead_zone", [](auto& s) { s.input.gamepad_dead_zone = 0.25f; }},
        {"input.rumble", [](auto& s) { s.input.rumble = false; }},
        {"camera.roll", [](auto& s) { s.camera.roll = 0.0f; }},
        {"camera.third_person", [](auto& s) { s.camera.third_person = true; }},
        {"game.game_plus", [](auto& s) { s.game.game_plus = false; }},
        {"audio.volume", [](auto& s) { s.audio.volume = 1.7f; }},
        {"voice.device", [](auto& s) { s.voice.device = "Microphone (USB Audio Device)"; }},
        {"voice.key", [](auto& s) { s.voice.key = "J"; }},
        {"upscaling.upscaler", [](auto& s) { s.upscaling.upscaler = "dlss"; }},
        {"upscaling.quality", [](auto& s) { s.upscaling.quality = "ultra_performance"; }},
        {"upscaling.scale", [](auto& s) { s.upscaling.scale = 0.5f; }},
        {"upscaling.sharpness", [](auto& s) { s.upscaling.sharpness = 0.7f; }},
        {"upscaling.dlss_model", [](auto& s) { s.upscaling.dlss_model = "k"; }},
        {"upscaling.dlssg_multiplier", [](auto& s) { s.upscaling.dlssg_multiplier = 4; }},
        {"upscaling.frame_generation", [](auto& s) { s.upscaling.frame_generation = "dlss"; }},
        {"upscaling.ray_reconstruction", [](auto& s) { s.upscaling.ray_reconstruction = false; }},
        {"raytracing.shadows", [](auto& s) { s.ray_tracing.shadows = 2; }},
        {"raytracing.ambient_occlusion", [](auto& s) { s.ray_tracing.ambient_occlusion = true; }},
        {"raytracing.contact_shadows", [](auto& s) { s.ray_tracing.contact_shadows = true; }},
        {"raytracing.reflections", [](auto& s) { s.ray_tracing.reflections = true; }},
        {"raytracing.full_shadows", [](auto& s) { s.ray_tracing.full_shadows = false; }},
        {"raytracing.bounce_light", [](auto& s) { s.ray_tracing.bounce_light = false; }},
        {"raytracing.glossy_reflections", [](auto& s) { s.ray_tracing.glossy_reflections = false; }},
        {"graphics.anisotropy", [](auto& s) { s.graphics.anisotropy = 8; }},
        {"graphics.shadow_quality", [](auto& s) { s.graphics.shadow_quality = 0; }},
        {"graphics.ambient_occlusion", [](auto& s) { s.graphics.ambient_occlusion = false; }},
        {"graphics.reflections", [](auto& s) { s.graphics.reflections = false; }},
        {"graphics.bloom", [](auto& s) { s.graphics.bloom = false; }},
        {"graphics.depth_of_field", [](auto& s) { s.graphics.depth_of_field = false; }},
        {"graphics.motion_blur", [](auto& s) { s.graphics.motion_blur = false; }},
        {"graphics.lens_distortion", [](auto& s) { s.graphics.lens_distortion = false; }},
        {"graphics.lens_ghosts", [](auto& s) { s.graphics.lens_ghosts = false; }},
        {"graphics.bloom_strength", [](auto& s) { s.graphics.bloom_strength = 75; }},
        {"graphics.flare_strength", [](auto& s) { s.graphics.flare_strength = 10; }},
        {"graphics.ghost_strength", [](auto& s) { s.graphics.ghost_strength = 5; }},
        {"graphics.camera_motion_blur", [](auto& s) { s.graphics.camera_motion_blur = false; }},
        {"graphics.film_grain_type", [](auto& s) { s.graphics.film_grain_type = 1; }},
        {"graphics.pt_floor_reflection", [](auto& s) { s.graphics.pt_floor_reflection = 60; }},
        {"graphics.pt_glass_reflection", [](auto& s) { s.graphics.pt_glass_reflection = 40; }},
        {"graphics.pt_reflection_softness", [](auto& s) { s.graphics.pt_reflection_softness = 15; }},
        {"display.hdr_exposure", [](auto& s) { s.display.hdr_exposure = -7; }},
        {"display.hdr_saturation", [](auto& s) { s.display.hdr_saturation = 150; }},
        {"display.hdr_blowout", [](auto& s) { s.display.hdr_blowout = 40; }},
        {"display.hdr_see_through", [](auto& s) { s.display.hdr_see_through = false; }},
        {"display.hdr_windows_switch", [](auto& s) { s.display.hdr_windows_switch = true; }},
        {"graphics.deband", [](auto& s) { s.graphics.deband = true; }},
        {"graphics.film_grain", [](auto& s) { s.graphics.film_grain = 0.75f; }},
        {"graphics.clarity", [](auto& s) { s.graphics.clarity = 0.4f; }},
        {"graphics.texture_detail", [](auto& s) { s.graphics.texture_detail = 0; }},
        {"graphics.ray_quality older ultra", [](auto& s) { s.graphics.ray_quality = 2; }},
        {"graphics.ray_quality ultra", [](auto& s) { s.graphics.ray_quality = 3; }},
        {"graphics.ray_quality path traced", [](auto& s) { s.graphics.ray_quality = 4; }},
        {"accessibility.sound_captions", [](auto& s) { s.accessibility.sound_captions = true; }},
        {"accessibility.house_sprint", [](auto& s) { s.accessibility.house_sprint = true; }},
        {"progress.survival_announced", [](auto& s) { s.progress.survival_announced = true; }},
        {"game.survival_difficulty insanity", [](auto& s) { s.game.survival_difficulty = 4; }},
        {"accessibility.gyro camera", [](auto& s) { s.accessibility.gyro = 1; }},
        {"accessibility.gyro flashlight", [](auto& s) { s.accessibility.gyro = 2; s.accessibility.gyro_sensitivity = 2.0f; }},
        {"accessibility.gyro_tightening", [](auto& s) { s.accessibility.gyro_tightening = 7.5f; }},
        {"accessibility.gyro_tightening off", [](auto& s) { s.accessibility.gyro_tightening = 0.0f; }},
        {"gyro_bias", [](auto& s) {
             s.accessibility.gyro_bias["030000007e0500000920000000006800"] = {0.125f, -0.0625f, 0.03125f};
             s.accessibility.gyro_bias["030000004c050000e60c000000006800"] = {0.0f, 0.25f, -0.015625f};
         }},
        {"accessibility.field_of_view wider", [](auto& s) { s.accessibility.fov_offset = 30; }},
        {"accessibility.field_of_view narrower", [](auto& s) { s.accessibility.fov_offset = -10; }},
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
    s.display.hdr=true; s.display.fps_limit=144; s.audio.surround=true; s.input.rumble_profile=1; s.vr.height_offset=-0.15f;
        change(s);
        const pt::AppSettings loaded = RoundTrip(ini, s);
        Check(name + " changes the settings", !Same(s, pt::AppSettings{}));
        Check(name + " round trip", Same(loaded, s));
    }
    const pt::AppSettings odd = LoadText(ini,
        "[display]\nletterbox = 7\nfullscreen = -3\nwidth = 99999\n[graphics]\nanisotropy = 5\nfilm_grain = nan\nclarity = 3\n"
        "shadow_quality = 9\ntexture_detail = -1\nray_quality = 7\n[raytracing]\nshadows = 4\n[audio]\nvolume = loud\n[input]\nrumble = yes\ngamepad_sensitivity = 9\n[nothing]\nx = 1\n"
        "[vr]\nturn = 3\nresolution_scale = 9\nsnap_degrees = nan\n"
        "[accessibility]\ngyro = 7\ngyro_sensitivity = nan\ngyro_tightening = nan\nsound_captions = yes\nfield_of_view = 90\n"
        "[gyro_bias]\nfine = 0.125 -0.0625 0.03125\ntoo_big = 9 0 0\nshort = 0.1\nnot_a_number = a b c\n");
    Check("letterbox clamped", odd.display.letterbox == 2);
    Check("fullscreen clamped", odd.display.fullscreen == 0);
    Check("width clamped", odd.display.width == 16384);
    Check("anisotropy rounded down to a step", odd.graphics.anisotropy == 4);
    Check("film grain nan is the original's", odd.graphics.film_grain == 1.0f);
    Check("clarity clamped", odd.graphics.clarity == 1.0f);
    Check("shadow quality clamped", odd.graphics.shadow_quality == 3);
    Check("texture detail clamped", odd.graphics.texture_detail == 0);
    Check("ray quality clamped to Path traced", odd.graphics.ray_quality == 4);
    Check("ray quality 2 reads as Ultra", LoadText(dir / "rq2.ini", "[graphics]\nray_quality = 2\n").graphics.ray_quality == 3);
    Check("ray quality 5 reads as the older Ultra", LoadText(dir / "rq5.ini", "[graphics]\nray_quality = 5\n").graphics.ray_quality == 2);
    Check("ray quality 3 stays Ultra", LoadText(dir / "rq3.ini", "[graphics]\nray_quality = 3\n").graphics.ray_quality == 3);
    Check("traced shadows clamped", odd.ray_tracing.shadows == 2);
    Check("gamepad sensitivity clamped", odd.input.gamepad_sensitivity == 4.0f);
    Check("malformed volume keeps the default", odd.audio.volume == 1.0f);
    Check("yes reads as on", odd.input.rumble);
    Check("vr turn clamped", odd.vr.turn == 1);
    Check("vr resolution scale clamped", odd.vr.resolution_scale == 2.0f);
    Check("vr snap nan is the default", odd.vr.snap_degrees == 30.0f);
    Check("vr off by default", !pt::AppSettings{}.vr.enabled);
    Check("game plus on by default", pt::AppSettings{}.game.game_plus);
    Check("game plus off reads", !LoadText(ini, "[game]\ngame_plus = 0\n").game.game_plus);
    Check("gyro clamped", odd.accessibility.gyro == 2);
    Check("gyro sensitivity nan is the default", odd.accessibility.gyro_sensitivity == 1.0f);
    Check("gyro tightening nan is the default", odd.accessibility.gyro_tightening == 4.0f);
    Check("a malformed or implausible gyro offset is dropped, a good one kept",
          odd.accessibility.gyro_bias.size() == 1 && odd.accessibility.gyro_bias.count("fine") == 1 &&
              odd.accessibility.gyro_bias.at("fine") == std::array<float, 3>{0.125f, -0.0625f, 0.03125f});
    Check("captions read yes as on", odd.accessibility.sound_captions);
    Check("field of view clamped", odd.accessibility.fov_offset == 40);
    Check("field of view is the original's by default", pt::AppSettings{}.accessibility.fov_offset == 0);
    Check("accessibility off by default", pt::AppSettings{}.accessibility == pt::AppSettings::Accessibility{} && !pt::AppSettings{}.accessibility.sound_captions &&
                                            pt::AppSettings{}.accessibility.gyro == 0);
    Check("vr world_scale of an older version leaves the size at 1", LoadText(ini, "[vr]\nworld_scale = 0.5\n").vr.world_size == 1.0f);
    Check("vr world_size wins over an old world_scale", LoadText(ini, "[vr]\nworld_scale = 0.5\nworld_size = 1.25\n").vr.world_size == 1.25f);
    Check("vr world_size clamped", LoadText(ini, "[vr]\nworld_size = 9\n").vr.world_size == 2.0f);
    std::filesystem::remove(ini);
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
