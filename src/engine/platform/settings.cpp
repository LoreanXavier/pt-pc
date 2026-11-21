#include "engine/core/ascii.h"
#include "engine/platform/os.h"
#include "engine/platform/settings.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <cmath>
#include <map>
#include <sstream>

#include "engine/core/log.h"
#include "engine/platform/graphics_presets.h"

namespace pt {
namespace {

using Values = std::map<std::string, std::string>;

std::string Trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

Values Parse(std::istream& in) {
    Values values;
    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        values[section + "." + Trim(line.substr(0, eq))] = value;
    }
    return values;
}

void Read(const Values& values, const char* key, int& out) {
    if (auto it = values.find(key); it != values.end()) {
        try {
            out = std::stoi(it->second);
        } catch (...) {
            LogWarn("settings: {} = '{}' is not a number", key, it->second);
        }
    }
}

void Read(const Values& values, const char* key, float& out) {
    if (auto it = values.find(key); it != values.end()) {
        try {
            out = std::stof(it->second);
        } catch (...) {
            LogWarn("settings: {} = '{}' is not a number", key, it->second);
        }
    }
}

void Read(const Values& values, const char* key, bool& out) {
    if (auto it = values.find(key); it != values.end()) {
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(pt::AsciiLower(static_cast<char>(c))); });
        out = v == "1" || v == "true" || v == "yes" || v == "on";
    }
}

void Read(const Values& values, const char* key, std::string& out) {
    if (auto it = values.find(key); it != values.end()) {
        out = it->second;
    }
}

}

bool LoadAppSettings(const std::filesystem::path& path, AppSettings& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    const Values v = Parse(in);
    Read(v, "display.width", out.display.width);
    Read(v, "display.height", out.display.height);
    Read(v, "display.fullscreen", out.display.fullscreen);
    Read(v, "display.refresh_rate", out.display.refresh_rate);
    Read(v, "display.vsync", out.display.vsync);
    Read(v, "display.fps_limit", out.display.fps_limit);
    Read(v, "display.hdr", out.display.hdr);
    Read(v, "display.hdr_paper_white", out.display.hdr_paper_white);
    Read(v, "display.hdr_peak", out.display.hdr_peak);
    Read(v, "display.hdr_ui_white", out.display.hdr_ui_white);
    Read(v, "display.hdr_reach", out.display.hdr_reach);
    Read(v, "display.hdr_gamma", out.display.hdr_gamma);
    Read(v, "display.hdr_keep_hue", out.display.hdr_keep_hue);
    Read(v, "display.hdr_colour_range", out.display.hdr_colour_range);
    Read(v, "display.hdr_exposure", out.display.hdr_exposure);
    Read(v, "display.hdr_contrast", out.display.hdr_contrast);
    Read(v, "display.hdr_highlights", out.display.hdr_highlights);
    Read(v, "display.hdr_shadows", out.display.hdr_shadows);
    Read(v, "display.hdr_saturation", out.display.hdr_saturation);
    Read(v, "display.hdr_highlight_saturation", out.display.hdr_highlight_saturation);
    Read(v, "display.hdr_blowout", out.display.hdr_blowout);
    Read(v, "display.hdr_see_through", out.display.hdr_see_through);
    Read(v, "display.hdr_windows_switch", out.display.hdr_windows_switch);
    Read(v, "display.hdr_windows_held", out.display.hdr_windows_held);
    Read(v, "display.letterbox", out.display.letterbox);
    Read(v, "display.rendering_api", out.display.rendering_api);
    Read(v, "display.rendering_api_asked", out.display.rendering_api_asked);
    Read(v, "display.d3d12_crashed", out.display.d3d12_crashed);
    Read(v, "display.d3d12_interrupted", out.display.d3d12_interrupted);
    Read(v, "display.d3d12_auto_fallback", out.display.d3d12_auto_fallback);
    Read(v, "display.vulkan_implicit_layers", out.display.vulkan_implicit_layers);
    Read(v, "input.mouse_sensitivity", out.input.mouse_sensitivity);
    Read(v, "input.gamepad_sensitivity", out.input.gamepad_sensitivity);
    Read(v, "input.gamepad_dead_zone", out.input.gamepad_dead_zone);
    Read(v, "input.rumble", out.input.rumble);
    Read(v, "input.rumble_profile", out.input.rumble_profile);
    Read(v, "input.prompt_style", out.input.prompt_style);
    Read(v, "network.check_updates", out.network.check_updates);
    Read(v, "network.discord_presence", out.network.discord_presence);
    Read(v, "camera.roll", out.camera.roll);
    Read(v, "camera.third_person", out.camera.third_person);
    Read(v, "game.game_plus", out.game.game_plus);
    Read(v, "game.survival_best_wave", out.game.survival_best_wave);
    Read(v, "game.survival_difficulty", out.game.survival_difficulty);
    out.game.survival_difficulty = std::clamp(out.game.survival_difficulty, 0, 4);
    if (auto it = v.find("game.survival_profile"); it != v.end()) out.game.survival_profile = it->second;
    Read(v, "crosshair.shape", out.crosshair.shape);
    Read(v, "crosshair.color", out.crosshair.color);
    Read(v, "crosshair.length", out.crosshair.length);
    Read(v, "crosshair.thickness", out.crosshair.thickness);
    Read(v, "crosshair.gap", out.crosshair.gap);
    Read(v, "crosshair.dynamic", out.crosshair.dynamic);
    Read(v, "crosshair.dot", out.crosshair.dot);
    Read(v, "crosshair.outline", out.crosshair.outline);
    Read(v, "crosshair.opacity", out.crosshair.opacity);
    Read(v, "crosshair.hit_marker", out.crosshair.hit_marker);
    Read(v, "crosshair.behind_sights", out.crosshair.behind_sights);
    out.crosshair.shape = std::clamp(out.crosshair.shape, 0, 6);
    out.crosshair.color = std::clamp(out.crosshair.color, 0, 6);
    out.crosshair.length = std::clamp(out.crosshair.length, 1, 20);
    out.crosshair.thickness = std::clamp(out.crosshair.thickness, 1, 10);
    out.crosshair.gap = std::clamp(out.crosshair.gap, 0, 20);
    out.crosshair.opacity = std::clamp(out.crosshair.opacity, 1, 10);
    Read(v, "audio.volume", out.audio.volume);
    Read(v, "audio.surround", out.audio.surround);
    Read(v, "audio.spatial", out.audio.spatial);
    Read(v, "audio.controller_speaker", out.audio.controller_speaker);
    Read(v, "audio.controller_speaker_volume", out.audio.controller_speaker_volume);
    Read(v, "audio.output_device", out.audio.output_device);
    Read(v, "voice.device", out.voice.device);
    Read(v, "voice.key", out.voice.key);
    Read(v, "upscaling.upscaler", out.upscaling.upscaler);
    Read(v, "upscaling.quality", out.upscaling.quality);
    Read(v, "upscaling.scale", out.upscaling.scale);
    Read(v, "upscaling.sharpness", out.upscaling.sharpness);
    Read(v, "upscaling.dlss_model", out.upscaling.dlss_model);
    Read(v, "upscaling.frame_generation", out.upscaling.frame_generation);
    Read(v, "upscaling.dlssg_multiplier", out.upscaling.dlssg_multiplier);
    Read(v, "upscaling.dlssg_interrupted", out.upscaling.dlssg_interrupted);
    out.upscaling.dlssg_multiplier = std::clamp(out.upscaling.dlssg_multiplier, 2, 4);
    Read(v, "upscaling.ray_reconstruction", out.upscaling.ray_reconstruction);
    Read(v, "upscaling.neural_rendering", out.upscaling.neural_rendering);
    Read(v, "upscaling.neural_style", out.upscaling.neural_style);
    Read(v, "upscaling.neural_quality", out.upscaling.neural_quality);
    Read(v, "upscaling.neural_model", out.upscaling.neural_model);
    if (out.upscaling.neural_model != "small" && out.upscaling.neural_model != "large") out.upscaling.neural_model = "small";
    if (out.upscaling.neural_quality != "performance" && out.upscaling.neural_quality != "ultra") out.upscaling.neural_quality = "quality";
    Read(v, "upscaling.neural_intensity", out.upscaling.neural_intensity);
    if (!std::isfinite(out.upscaling.neural_intensity)) out.upscaling.neural_intensity = 1.0f;
    out.upscaling.neural_intensity = std::clamp(out.upscaling.neural_intensity, 0.25f, 3.0f);
    Read(v, "upscaling.neural_runtime", out.upscaling.neural_runtime);
    Read(v, "upscaling.neural_route_armed", out.upscaling.neural_route_armed);
    Read(v, "upscaling.dlssg_route_armed", out.upscaling.dlssg_route_armed);
    Read(v, "raytracing.shadows", out.ray_tracing.shadows);
    Read(v, "raytracing.ambient_occlusion", out.ray_tracing.ambient_occlusion);
    Read(v, "raytracing.contact_shadows", out.ray_tracing.contact_shadows);
    Read(v, "raytracing.reflections", out.ray_tracing.reflections);
    Read(v, "raytracing.full_shadows", out.ray_tracing.full_shadows);
    Read(v, "raytracing.bounce_light", out.ray_tracing.bounce_light);
    Read(v, "raytracing.glossy_reflections", out.ray_tracing.glossy_reflections);
    Read(v, "graphics.anisotropy", out.graphics.anisotropy);
    Read(v, "graphics.shadow_quality", out.graphics.shadow_quality);
    Read(v, "graphics.ambient_occlusion", out.graphics.ambient_occlusion);
    Read(v, "graphics.reflections", out.graphics.reflections);
    Read(v, "graphics.bloom", out.graphics.bloom);
    Read(v, "graphics.depth_of_field", out.graphics.depth_of_field);
    Read(v, "graphics.motion_blur", out.graphics.motion_blur);
    Read(v, "graphics.lens_distortion", out.graphics.lens_distortion);
    Read(v, "graphics.lens_ghosts", out.graphics.lens_ghosts);
    Read(v, "graphics.bloom_strength", out.graphics.bloom_strength);
    Read(v, "graphics.flare_strength", out.graphics.flare_strength);
    Read(v, "graphics.ghost_strength", out.graphics.ghost_strength);
    Read(v, "graphics.camera_motion_blur", out.graphics.camera_motion_blur);
    Read(v, "graphics.motion_blur_strength", out.graphics.motion_blur_strength);
    out.graphics.motion_blur_strength = std::clamp(out.graphics.motion_blur_strength, 10, 100);
    Read(v, "graphics.film_grain_type", out.graphics.film_grain_type);
    Read(v, "graphics.pt_floor_reflection", out.graphics.pt_floor_reflection);
    Read(v, "graphics.pt_glass_reflection", out.graphics.pt_glass_reflection);
    Read(v, "graphics.pt_reflection_softness", out.graphics.pt_reflection_softness);
    Read(v, "graphics.deband", out.graphics.deband);
    Read(v, "graphics.film_grain", out.graphics.film_grain);
    Read(v, "graphics.clarity", out.graphics.clarity);
    Read(v, "graphics.texture_detail", out.graphics.texture_detail);
    Read(v, "graphics.ray_quality", out.graphics.ray_quality);
    if (!v.contains("graphics.lens_ghosts")) {
        AppSettings probe = out;
        probe.graphics.lens_ghosts = true;
        out.graphics.lens_ghosts = DetectGraphicsPreset(probe, false) == GraphicsPreset::Original ||
                                   DetectGraphicsPreset(probe, true) == GraphicsPreset::Original;
    }
    if (!v.contains("graphics.deband")) {
        AppSettings probe = out;
        probe.graphics.deband = false;
        out.graphics.deband = !(DetectGraphicsPreset(probe, false) == GraphicsPreset::Original ||
                                DetectGraphicsPreset(probe, true) == GraphicsPreset::Original);
    }
    if (!v.contains("graphics.flare_strength")) {
        MoveOldGraphicsPreset(out);
    }
    Read(v, "vr.enabled", out.vr.enabled);
    Read(v, "vr.flashlight", out.vr.flashlight);
    Read(v, "vr.flashlight_hand", out.vr.flashlight_hand);
    Read(v, "vr.turn", out.vr.turn);
    Read(v, "vr.snap_degrees", out.vr.snap_degrees);
    Read(v, "vr.smooth_speed", out.vr.smooth_speed);
    Read(v, "vr.resolution_scale", out.vr.resolution_scale);
    Read(v, "vr.height_offset", out.vr.height_offset);
    Read(v, "vr.world_size", out.vr.world_size);
    if (v.contains("vr.world_scale") && !v.contains("vr.world_size")) {
        LogInfo("settings: vr.world_scale = {} from an older version is not used; the VR world takes its size from the headset "
                "(vr.world_size = 1)", v.at("vr.world_scale"));
    }
    {
        std::string reached;
        Read(v, "progress.loops_reached", reached);
        if (!reached.empty()) {
            try {
                out.progress.loops_reached = static_cast<uint32_t>(std::stoul(reached, nullptr, 16));
            } catch (...) {
                LogWarn("settings: progress.loops_reached = '{}' is not a hex number", reached);
            }
        }
        Read(v, "progress.game_finished", out.progress.game_finished);
        Read(v, "progress.night_unlocked", out.progress.night_unlocked);
        Read(v, "progress.survival_announced", out.progress.survival_announced);
        Read(v, "progress.archive", out.progress.archive);
        Read(v, "progress.archive_backfill", out.progress.archive_backfill);
    }
    Read(v, "accessibility.sound_captions", out.accessibility.sound_captions);
    Read(v, "accessibility.fps_counter", out.accessibility.fps_counter);
    Read(v, "accessibility.subtitle_size", out.accessibility.subtitle_size);
    Read(v, "accessibility.subtitle_bg", out.accessibility.subtitle_bg);
    Read(v, "accessibility.reduced_flashing", out.accessibility.reduced_flashing);
    Read(v, "accessibility.house_sprint", out.accessibility.house_sprint);
    Read(v, "accessibility.sprint_toggle", out.accessibility.sprint_toggle);
    Read(v, "accessibility.zoom_toggle", out.accessibility.zoom_toggle);
    out.accessibility.subtitle_size = std::clamp(out.accessibility.subtitle_size, 0, 2);
    out.accessibility.subtitle_bg = std::clamp(out.accessibility.subtitle_bg, 0, 2);
    Read(v, "accessibility.gyro", out.accessibility.gyro);
    Read(v, "accessibility.gyro_sensitivity", out.accessibility.gyro_sensitivity);
    Read(v, "accessibility.gyro_tightening", out.accessibility.gyro_tightening);
    Read(v, "accessibility.field_of_view", out.accessibility.fov_offset);
    out.accessibility.gyro_tightening =
        std::isfinite(out.accessibility.gyro_tightening) ? std::clamp(out.accessibility.gyro_tightening, 0.0f, 10.0f) : 4.0f;
    for (auto it = v.lower_bound("gyro_bias."); it != v.end() && it->first.starts_with("gyro_bias."); ++it) {
        std::istringstream in(it->second);
        std::array<float, 3> bias{};
        if ((in >> bias[0] >> bias[1] >> bias[2]) && std::isfinite(bias[0]) && std::isfinite(bias[1]) && std::isfinite(bias[2]) &&
            std::abs(bias[0]) < 0.35f && std::abs(bias[1]) < 0.35f && std::abs(bias[2]) < 0.35f) {
            out.accessibility.gyro_bias[it->first.substr(10)] = bias;
        } else {
            LogWarn("settings: {} = '{}' is not a gyro offset", it->first, it->second);
        }
    }
    out.accessibility.fov_offset = std::clamp(out.accessibility.fov_offset, -20, 40);
    out.accessibility.gyro = std::clamp(out.accessibility.gyro, 0, 2);
    out.accessibility.gyro_sensitivity =
        std::isfinite(out.accessibility.gyro_sensitivity) ? std::clamp(out.accessibility.gyro_sensitivity, 0.25f, 4.0f) : 1.0f;
    Read(v, "ui.support_prompts", out.ui.support_prompts);
    Read(v, "ui.last_version", out.ui.last_version);
    Read(v, "extras.speedrun", out.extras.speedrun);
    Read(v, "extras.livesplit", out.extras.livesplit);
    Read(v, "extras.livesplit_host", out.extras.livesplit_host);
    Read(v, "extras.livesplit_port", out.extras.livesplit_port);
    out.extras.speedrun = std::clamp(out.extras.speedrun, 0, 2);
    if (out.extras.livesplit_port <= 0 || out.extras.livesplit_port > 65535) out.extras.livesplit_port = 16834;
    for (auto it = v.lower_bound("mods."); it != v.end() && it->first.starts_with("mods."); ++it) {
        bool enabled = true;
        Read(v, it->first.c_str(), enabled);
        out.mods[it->first.substr(5)] = enabled;
    }
    for (auto it = v.lower_bound("keys."); it != v.end() && it->first.starts_with("keys."); ++it) {
        if (!it->second.empty()) out.keys[it->first.substr(5)] = it->second;
    }
    out.graphics.shadow_quality=std::clamp(out.graphics.shadow_quality,0,3);
    out.graphics.texture_detail=std::clamp(out.graphics.texture_detail,0,2);
    if (out.graphics.ray_quality == kRayQualityOldUltraIni) {
        out.graphics.ray_quality = 2;
    } else {
        out.graphics.ray_quality=std::clamp(out.graphics.ray_quality,0,4);
        if (out.graphics.ray_quality == 2) {
            out.graphics.ray_quality = 3;
            LogInfo("settings: ray quality Ultra from an older version is now Ultra with every light's traced shadows, bounce light and glossy reflections");
        }
    }
    out.graphics.film_grain=std::isfinite(out.graphics.film_grain)?std::clamp(out.graphics.film_grain,0.0f,1.0f):1.0f;
    out.graphics.clarity=std::isfinite(out.graphics.clarity)?std::clamp(out.graphics.clarity,0.0f,1.0f):0.0f;
    out.display.hdr_colour_range=std::clamp(out.display.hdr_colour_range,0,2);
    out.display.hdr_exposure=std::clamp(out.display.hdr_exposure,-30,30);
    out.display.hdr_contrast=std::clamp(out.display.hdr_contrast,0,200);
    out.display.hdr_highlights=std::clamp(out.display.hdr_highlights,0,200);
    out.display.hdr_shadows=std::clamp(out.display.hdr_shadows,0,200);
    out.display.hdr_saturation=std::clamp(out.display.hdr_saturation,0,200);
    out.display.hdr_highlight_saturation=std::clamp(out.display.hdr_highlight_saturation,0,200);
    out.display.hdr_blowout=std::clamp(out.display.hdr_blowout,0,100);
    out.graphics.bloom_strength=std::clamp(out.graphics.bloom_strength,0,200);
    out.graphics.flare_strength=std::clamp(out.graphics.flare_strength,0,200);
    out.graphics.ghost_strength=std::clamp(out.graphics.ghost_strength,0,200);
    out.graphics.film_grain_type=std::clamp(out.graphics.film_grain_type,0,1);
    out.graphics.pt_floor_reflection=std::clamp(out.graphics.pt_floor_reflection,0,200);
    out.graphics.pt_glass_reflection=std::clamp(out.graphics.pt_glass_reflection,0,200);
    out.graphics.pt_reflection_softness=std::clamp(out.graphics.pt_reflection_softness,0,100);
    out.display.width = std::clamp(out.display.width, 320, 16384);
    out.display.height = std::clamp(out.display.height, 240, 16384);
    out.display.fullscreen = std::clamp(out.display.fullscreen, 0, 2);
    out.display.refresh_rate = std::isfinite(out.display.refresh_rate) && out.display.refresh_rate > 0.0f
                                   ? std::clamp(out.display.refresh_rate, 20.0f, 1000.0f) : 0.0f;
    out.display.fps_limit = out.display.fps_limit <= 0 ? 0 : std::clamp(out.display.fps_limit, 20, 1000);
    out.display.letterbox = std::clamp(out.display.letterbox, 0, 2);
    out.input.mouse_sensitivity = std::clamp(out.input.mouse_sensitivity, 0.05f, 20.0f);
    out.input.gamepad_sensitivity = std::isfinite(out.input.gamepad_sensitivity) ? std::clamp(out.input.gamepad_sensitivity, 0.25f, 4.0f) : 1.0f;
    out.input.gamepad_dead_zone = std::clamp(out.input.gamepad_dead_zone, 0.0f, 0.9f);
    out.input.rumble_profile = std::clamp(out.input.rumble_profile, 0, 1);
    out.input.prompt_style = std::clamp(out.input.prompt_style, 0, 4);
    out.camera.roll = std::clamp(out.camera.roll, 0.0f, 1.0f);
    out.audio.volume = std::clamp(out.audio.volume, 0.0f, 2.0f);
    out.audio.controller_speaker_volume = std::isfinite(out.audio.controller_speaker_volume)
                                              ? std::clamp(out.audio.controller_speaker_volume, 0.0f, 1.0f)
                                              : 0.5f;
    out.upscaling.scale = std::clamp(out.upscaling.scale, 0.25f, 1.0f);
    out.upscaling.sharpness = std::clamp(out.upscaling.sharpness, 0.0f, 1.0f);
    out.ray_tracing.shadows = std::clamp(out.ray_tracing.shadows, 0, 2);
    out.vr.height_offset = std::isfinite(out.vr.height_offset) ? std::clamp(out.vr.height_offset, -0.5f, 0.5f) : 0.0f;
    out.vr.world_size = std::isfinite(out.vr.world_size) ? std::clamp(out.vr.world_size, 0.5f, 2.0f) : 1.0f;
    out.vr.flashlight = std::clamp(out.vr.flashlight, 0, 1);
    out.vr.flashlight_hand = std::clamp(out.vr.flashlight_hand, 0, 1);
    out.vr.turn = std::clamp(out.vr.turn, 0, 1);
    out.vr.snap_degrees = std::isfinite(out.vr.snap_degrees) ? std::clamp(out.vr.snap_degrees, 10.0f, 90.0f) : 30.0f;
    out.vr.smooth_speed = std::isfinite(out.vr.smooth_speed) ? std::clamp(out.vr.smooth_speed, 20.0f, 360.0f) : 90.0f;
    out.vr.resolution_scale = std::isfinite(out.vr.resolution_scale) ? std::clamp(out.vr.resolution_scale, 0.5f, 2.0f) : 1.0f;
    const int anisotropy = out.graphics.anisotropy;
    out.graphics.anisotropy = anisotropy >= 16 ? 16 : anisotropy >= 8 ? 8 : anisotropy >= 4 ? 4 : anisotropy >= 2 ? 2 : 0;
    return true;
}

bool SaveAppSettings(const std::filesystem::path& path, const AppSettings& s) {
    std::ostringstream text;
    text << "; P.T. port settings. The in-game options (brightness, subtitles, camera inversion) live in the save data.\n\n"
         << "[display]\n"
         << "width = " << s.display.width << "\n"
         << "height = " << s.display.height << "\n"
         << "; 0 window, 1 borderless fullscreen, 2 exclusive fullscreen at a mode of the display (Alt+Enter toggles)\n"
         << "fullscreen = " << s.display.fullscreen << "\n"
         << "; the exclusive fullscreen mode's refresh rate in Hz, 0 for the highest the resolution offers (window and borderless\n"
         << "; follow the desktop's)\n"
         << "refresh_rate = " << s.display.refresh_rate << "\n"
         << "vsync = " << (s.display.vsync ? 1 : 0) << "\n"
         << "; frames per second at most, 0 for no limit (menus and the paused game stay at 60 or less either way)\n"
         << "fps_limit = " << s.display.fps_limit << "\n"
         << "hdr = " << (s.display.hdr ? 1 : 0) << "\n"
         << "; HDR output levels in nits: paper white (80 to 500) is the picture's white and the menus, the peak (400 to 4000) is how bright\n"
         << "; the lamps, the flashlight and the glow around them go; a peak of 0 takes the display's own, 1000 where Windows does not tell it\n"
         << "hdr_paper_white = " << s.display.hdr_paper_white << "\n"
         << "hdr_peak = " << s.display.hdr_peak << "\n"
         << "; the menus, subtitles and notices on an HDR output, in nits (80 to 500); 0 takes paper white\n"
         << "hdr_ui_white = " << s.display.hdr_ui_white << "\n"
         << "; how far the highlights reach on an HDR output, in percent: 100 Natural (the game's own curve), 150 Bright, 200 Intense\n"
         << "hdr_reach = " << s.display.hdr_reach << "\n"
         << "; the HDR picture: the gamma below paper white (0 2.2, 1 sRGB, 2 2.4), the highlights' colour kept by the curve (0 or 1)\n"
         << "; and the colour range (0 the game's, 2 wide: colours may leave it, held inside Display P3, 1 extended toward Display P3)\n"
         << "hdr_gamma = " << s.display.hdr_gamma << "\n"
         << "hdr_keep_hue = " << (s.display.hdr_keep_hue ? 1 : 0) << "\n"
         << "hdr_colour_range = " << s.display.hdr_colour_range << "\n"
         << "; the HDR colour grading: exposure in tenths of a stop (-30 to 30), the rest in percent (0 to 200, 100 neutral), blowout 0 to 100\n"
         << "hdr_exposure = " << s.display.hdr_exposure << "\n"
         << "hdr_contrast = " << s.display.hdr_contrast << "\n"
         << "hdr_highlights = " << s.display.hdr_highlights << "\n"
         << "hdr_shadows = " << s.display.hdr_shadows << "\n"
         << "hdr_saturation = " << s.display.hdr_saturation << "\n"
         << "hdr_highlight_saturation = " << s.display.hdr_highlight_saturation << "\n"
         << "hdr_blowout = " << s.display.hdr_blowout << "\n"
         << "; 1 the graphics and HDR pages show the game behind them on an HDR output\n"
         << "hdr_see_through = " << (s.display.hdr_see_through ? 1 : 0) << "\n"
         << "; 1 the game turns Windows HDR on for its display while it runs with HDR on, and off again when it closes\n"
         << "hdr_windows_switch = " << (s.display.hdr_windows_switch ? 1 : 0) << "\n"
         << "hdr_windows_held = " << (s.display.hdr_windows_held ? 1 : 0) << "\n"
         << "; black bars over and under the picture in play and in the cutscenes, not in the original: 0 off, 1 2.39:1, 2 1.85:1\n"
         << "letterbox = " << s.display.letterbox << "\n"
         << "; the graphics API the game draws with at the next start: vulkan, or d3d12 (DirectX 12, Windows)\n"
         << "rendering_api = " << s.display.rendering_api << "\n"
         << "rendering_api_asked = " << (s.display.rendering_api_asked ? 1 : 0) << "\n"
         << "; an earlier start died while DirectX 12 was starting (1 its check, 2 its device, 3 its first frames): DirectX 12 stays\n"
         << "; off until the player chooses DirectX 12 in the settings or a start with --backend d3d12 draws its first frames; 0 never\n"
         << "d3d12_crashed = " << s.display.d3d12_crashed << "\n"
         << "; starts that ended while DirectX 12 started with no sign of a crash (closed or ended from outside): one is retried\n"
         << "d3d12_interrupted = " << s.display.d3d12_interrupted << "\n"
         << "; 1: the game moved rendering_api to vulkan after a DirectX 12 crash; it returns to d3d12 once DirectX 12 runs again\n"
         << "d3d12_auto_fallback = " << (s.display.d3d12_auto_fallback ? 1 : 0) << "\n"
         << "; 0 keeps the implicit Vulkan layers of other programs (overlays, recorders) out of the game; the game sets 0 when a start\n"
         << "; died inside the Vulkan loader and the next one worked without them; 1 loads them\n"
         << "vulkan_implicit_layers = " << s.display.vulkan_implicit_layers << "\n\n"
         << "[input]\n"
         << "mouse_sensitivity = " << s.input.mouse_sensitivity << "\n"
         << "; gamepad turn speed after the original stick response, 1 keeps the original\n"
         << "gamepad_sensitivity = " << s.input.gamepad_sensitivity << "\n"
         << "; per stick axis, as the original reads its pads (0.102 is the DualShock 4 dead zone)\n"
         << "gamepad_dead_zone = " << s.input.gamepad_dead_zone << "\n"
         << "; pad vibration from the game's Wwise motion sounds, 0 turns it off\n"
         << "rumble = " << (s.input.rumble ? 1 : 0) << "\n"
         << "; 0 original motor rumble only, 1 also enables supported trigger rumble and DualSense USB cry haptics\n"
         << "rumble_profile = " << s.input.rumble_profile << "\n"
         << "; controller button prompts: 0 follow the pad in use, 1 PlayStation, 2 Xbox, 3 Nintendo, 4 Steam\n"
         << "prompt_style = " << s.input.prompt_style << "\n\n"
         << "[camera]\n"
         << "; share of the head's roll the camera takes: 1 as the original (it leans with the walk, most when strafing right), 0 keeps it level\n"
         << "roll = " << s.camera.roll << "\n"
         << "; experimental third person view (Extras), not in the original: the camera behind the shoulder in free play, first\n"
         << "; person for the zoom and the cutscenes; 0 keeps the original's first person view\n"
         << "third_person = " << (s.camera.third_person ? 1 : 0) << "\n\n"
         << "[game]\n"
         << "; Game+, shown in the options after the first finished game: 1 plays the content the finishes unlocked, 0 plays the original\n"
         << "; game (the finishes and the tier stay saved); a change applies from the next loop\n"
         << "game_plus = " << (s.game.game_plus ? 1 : 0) << "\n"
         << "; the survival night's furthest wave (Game+ tier 4)\n"
         << "survival_best_wave = " << s.game.survival_best_wave << "\n"
         << "; its difficulty: 0 easy (weaker, fewer enemies, more drops), 1 normal, 2 hard (stronger, more of them, fewer drops)\n"
         << "survival_difficulty = " << s.game.survival_difficulty << "\n"
         << "; the night's own record: the checkpoints to take a night up again from (the loop browser's night page), the best scores,\n"
         << "; the day's night, what it has unlocked and the looks chosen from it\n"
         << "survival_profile = \"" << s.game.survival_profile << "\"\n\n"
         << "[crosshair]\n"
         << "; the survival night's crosshair: shape 0 ticks, 1 cross, 2 T, 3 circle, 4 circle and dot, 5 dot, 6 none; color 0 white,\n"
         << "; 1 green, 2 yellow, 3 cyan, 4 magenta, 5 red, 6 orange; length 1 to 20 and gap 0 to 20 (pixels at 720 lines);\n"
         << "; thickness 1 to 10 half pixels; dynamic: the gap opens with the shot's spread; opacity 1 to 10 tenths; behind_sights: shown\n"
         << "; while aiming down the first person sights too\n"
         << "shape = " << s.crosshair.shape << "\n"
         << "color = " << s.crosshair.color << "\n"
         << "length = " << s.crosshair.length << "\n"
         << "thickness = " << s.crosshair.thickness << "\n"
         << "gap = " << s.crosshair.gap << "\n"
         << "dynamic = " << (s.crosshair.dynamic ? 1 : 0) << "\n"
         << "dot = " << (s.crosshair.dot ? 1 : 0) << "\n"
         << "outline = " << (s.crosshair.outline ? 1 : 0) << "\n"
         << "opacity = " << s.crosshair.opacity << "\n"
         << "hit_marker = " << (s.crosshair.hit_marker ? 1 : 0) << "\n"
         << "behind_sights = " << (s.crosshair.behind_sights ? 1 : 0) << "\n\n"
         << "[audio]\n"
         << "volume = " << s.audio.volume << "\n"
         << "surround = " << (s.audio.surround ? 1 : 0) << "\n"
         << "; 1: the 7.1 mix through Windows spatial sound (Windows Sonic, Dolby Atmos, DTS:X) when Windows has one on for the device\n"
         << "spatial = " << (s.audio.spatial ? 1 : 0) << "\n\n"
         << "; DualSense speaker toggle; DualShock 4 Bluetooth speaker starts automatically\n"
         << "controller_speaker = " << (s.audio.controller_speaker ? 1 : 0) << "\n"
         << "; separate controller-speaker level, 0 to 1; does not change game volume or DualSense haptics\n"
         << "controller_speaker_volume = " << s.audio.controller_speaker_volume << "\n"
         << "; the playback device of the game's sound by its name as the game lists it, empty for the system default\n"
         << "output_device = \"" << s.audio.output_device << "\"\n\n"
         << "[network]\n"
         << "; at start, ask the release page whether a newer version exists (a small note in the PC settings); 0 sends nothing\n"
         << "check_updates = " << (s.network.check_updates ? 1 : 0) << "\n"
         << "; show the loop you are in on your Discord profile (Rich Presence, through the Discord app on this PC); 0 off\n"
         << "discord_presence = " << (s.network.discord_presence ? 1 : 0) << "\n\n"
         << "[voice]\n"
         << "; the microphone listens only where the original game does (f160)\n"
         << "; part of the recording device name, empty for the system default\n"
         << "device = \"" << s.voice.device << "\"\n"
         << "; optional key name (SDL scancode name, e.g. J) that stands in for saying the word, empty to disable\n"
         << "key = \"" << s.voice.key << "\"\n\n"
         << "[upscaling]\n"
         << "; off, fsr3 (AMD FSR 3; fsr reads as fsr3), fsr4 (AMD FSR 4, needs a FidelityFX Vulkan build with FSR 4), dlss (NVIDIA DLSS,\n"
         << "; RTX GPUs), xess (Intel XeSS); off renders at the display resolution like the original\n"
         << "upscaler = " << s.upscaling.upscaler << "\n"
         << "; native (anti-aliasing only: FSR native AA, DLAA, XeSS AA), quality, balanced, performance, ultra_performance, custom\n"
         << "quality = " << s.upscaling.quality << "\n"
         << "; render scale per axis for quality = custom (0.25 to 1.0)\n"
         << "scale = " << s.upscaling.scale << "\n"
         << "; 0 to 1, FSR sharpening (RCAS) after the upscaler\n"
         << "sharpness = " << s.upscaling.sharpness << "\n"
         << "; DLSS model: auto (NVIDIA's default for each quality), k, l or m (l and m cost about twice as much on RTX 20 and 30)\n"
         << "dlss_model = " << s.upscaling.dlss_model << "\n"
         << "; frame generation, a generated frame between two rendered frames (needs an upscaler, any quality): off, fsr3 (AMD FSR 3),\n"
         << "; dlss (NVIDIA DLSS Frame Generation, experimental: RTX 40 or newer, starts with the next launch); 1 reads as fsr3\n"
         << "frame_generation = " << s.upscaling.frame_generation << "\n"
         << "; DLSS frame multiplier: 2, 3 or 4; limited to the current runtime capability\n"
         << "dlssg_multiplier = " << std::clamp(s.upscaling.dlssg_multiplier, 2, 4) << "\n"
         << "; starts that ended in DLSS Frame Generation's first frames with no sign of a crash: one is retried\n"
         << "dlssg_interrupted = " << s.upscaling.dlssg_interrupted << "\n"
         << "; NVIDIA DLSS Ray Reconstruction with the Path traced ray quality and dlss (RTX GPUs): 1 it denoises and upscales the path\n"
         << "; traced light, 0 the port's denoiser and DLSS Super Resolution\n"
         << "ray_reconstruction = " << (s.upscaling.ray_reconstruction ? 1 : 0) << "\n"
         << "; NVIDIA DLSS 5 neural rendering: off or dlss5, requires the runtime installed through PC Settings\n"
         << "; neural_style: default, natural or cinematic; neural_intensity: 0.25 to 3 (above 1 amplifies the neural change)\n"
         << "neural_rendering = " << s.upscaling.neural_rendering << "\n"
         << "neural_style = " << s.upscaling.neural_style << "\n"
         << "neural_intensity = " << s.upscaling.neural_intensity << "\n"
         << "; neural rendering quality: performance, quality or ultra; every rendered frame\n"
         << "neural_quality = " << s.upscaling.neural_quality << "\n"
         << "; independently chosen network: small or large; no automatic model downgrade\n"
         << "neural_model = " << s.upscaling.neural_model << "\n"
         << "; a path to the player's nvngx_dlssnr.dll (a file or its folder) when it is not next to pt.exe; never shipped\n"
         << "neural_runtime = " << s.upscaling.neural_runtime << "\n"
         << "; set while the community DLSS 5 route runs, cleared on a clean exit; a start that finds it set keeps DLSS 5 off (it crashed last time)\n"
         << "neural_route_armed = " << (s.upscaling.neural_route_armed ? "yes" : "no") << "\n"
         << "; set while the community DLSS Frame Generation unlock runs, cleared on a clean exit; a start that finds it set keeps FG off (it crashed last time)\n"
         << "dlssg_route_armed = " << (s.upscaling.dlssg_route_armed ? "yes" : "no") << "\n";
    text << "\n[raytracing]\n"
         << "; ray traced shadows in place of the shadow maps, not in the original: 0 off, 1 sharp (as the shadow maps), 2 soft\n"
         << "; (penumbrae from each light's size); needs a GPU with Vulkan ray queries\n"
         << "; changes apply during play on supported devices\n"
         << "shadows = " << s.ray_tracing.shadows << "\n"
         << "; ray traced ambient occlusion: the probe ambient darkened in corners, under and behind objects (same rules)\n"
         << "ambient_occlusion = " << (s.ray_tracing.ambient_occlusion ? 1 : 0) << "\n"
         << "; ray traced contact shadows: what touches or nearly touches a surface shadows it, for every light (same rules)\n"
         << "contact_shadows = " << (s.ray_tracing.contact_shadows ? 1 : 0) << "\n"
         << "; experiment: the floors' local reflections traced against the scene, so they also show what is off screen (same rules)\n"
         << "reflections = " << (s.ray_tracing.reflections ? 1 : 0) << "\n"
         << "; the parts of the Ultra ray quality ([graphics] ray_quality = 3), each 0 or 1: traced shadows from every light, not only\n"
         << "; the ones the original shadows; one bounce of the lights' diffuse light; traced reflections on glossy walls and props\n"
         << "full_shadows = " << (s.ray_tracing.full_shadows ? 1 : 0) << "\n"
         << "bounce_light = " << (s.ray_tracing.bounce_light ? 1 : 0) << "\n"
         << "glossy_reflections = " << (s.ray_tracing.glossy_reflections ? 1 : 0) << "\n";
    text << "\n[graphics]\n"
         << "; not in the original; 0 keeps the original's look\n"
         << "; anisotropic filtering of the textures: 0 (the original's trilinear filtering), 2, 4, 8 or 16\n"
         << "anisotropy = " << s.graphics.anisotropy << "\n"
         << "shadow_quality = " << s.graphics.shadow_quality << "\n"
         << "ambient_occlusion = " << s.graphics.ambient_occlusion << "\n"
         << "reflections = " << s.graphics.reflections << "\n"
         << "bloom = " << s.graphics.bloom << "\n"
         << "depth_of_field = " << s.graphics.depth_of_field << "\n"
         << "motion_blur = " << s.graphics.motion_blur << "\n"
         << "lens_distortion = " << s.graphics.lens_distortion << "\n"
         << "; the lens flares' large reflections across the screen near a lamp, as in the original; 0 leaves them out\n"
         << "lens_ghosts = " << s.graphics.lens_ghosts << "\n"
         << "; the strength of the bloom, the lens flares and the full screen ghosts in percent, 0 to 200, 100 the original's\n"
         << "bloom_strength = " << s.graphics.bloom_strength << "\n"
         << "flare_strength = " << s.graphics.flare_strength << "\n"
         << "ghost_strength = " << s.graphics.ghost_strength << "\n"
         << "; 1 the motion blur takes the camera's movement too, 0 only what moves in the picture\n"
         << "camera_motion_blur = " << s.graphics.camera_motion_blur << "\n"
         << "motion_blur_strength = " << s.graphics.motion_blur_strength << "\n"
         << "; the film grain: 0 the original's, 1 fine\n"
         << "film_grain_type = " << s.graphics.film_grain_type << "\n"
         << "; Path traced: how strongly floors and glass reflect in percent (0 to 200, 100 the original's strength), how soft it is (0 to 100)\n"
         << "pt_floor_reflection = " << s.graphics.pt_floor_reflection << "\n"
         << "pt_glass_reflection = " << s.graphics.pt_glass_reflection << "\n"
         << "pt_reflection_softness = " << s.graphics.pt_reflection_softness << "\n"
         << "; the sub-LSB dither at the tonemap, the colour grading and the final image, against banding in a lamp's glow; 0 is the original's (PS4) output\n"
         << "deband = " << s.graphics.deband << "\n"
         << "film_grain = " << s.graphics.film_grain << "\n"
         << "clarity = " << s.graphics.clarity << "\n"
         << "texture_detail = " << s.graphics.texture_detail << "\n"
         << "; ray quality: 0 balanced (2 rays for the soft shadows and the traced ambient occlusion), 1 high (4), 3 ultra (8 rays,\n"
         << "; traced shadows from every light, bounce light and glossy reflections, the [raytracing] full_* parts), 4 path traced\n"
         << "; (the lights sampled per pixel with traced shadows, bounces of light in place of the probe ambient; no preset uses it);\n"
         << "; 2 reads as 3; 5 is the older Ultra (8 rays alone), for comparisons only\n"
         << "ray_quality = " << (s.graphics.ray_quality == 2 ? kRayQualityOldUltraIni : s.graphics.ray_quality) << "\n";
    text << "\n[vr]\n"
         << "; experimental, untested on a real headset (docs/vr.md); needs an OpenXR runtime; applies at the next start\n"
         << "enabled = " << (s.vr.enabled ? 1 : 0) << "\n"
         << "; the flashlight: 0 follows the head, 1 the controller (flashlight_hand: 0 left, 1 right)\n"
         << "flashlight = " << s.vr.flashlight << "\n"
         << "flashlight_hand = " << s.vr.flashlight_hand << "\n"
         << "; turning with the right stick: 0 snap turns of snap_degrees, 1 smooth at smooth_speed degrees a second\n"
         << "turn = " << s.vr.turn << "\n"
         << "snap_degrees = " << s.vr.snap_degrees << "\n"
         << "smooth_speed = " << s.vr.smooth_speed << "\n"
         << "; the eye images' size against the headset's recommendation (0.5 to 2)\n"
         << "resolution_scale = " << s.vr.resolution_scale << "\n"
         << "; the eye anchor moved up or down, metres (-0.5 to 0.5)\n"
         << "height_offset = " << s.vr.height_offset << "\n"
         << "; the world's size: 1 as the headset measures the player (the eye above its floor), 2 twice as large, 0.5 half\n"
         << "world_size = " << s.vr.world_size << "\n";
    text << "\n[progress]\n"
         << "; the loop browser's unlocks (release builds): the entries reached in play (hex bits) and the game finished once\n"
         << "loops_reached = " << std::hex << s.progress.loops_reached << std::dec << "\n"
         << "game_finished = " << (s.progress.game_finished ? 1 : 0) << "\n"
         << "; the survival night's loop browser entry: 1 once New Game+ 4 is reached or the night is unlocked\n"
         << "night_unlocked = " << (s.progress.night_unlocked ? 1 : 0) << "\n"
         << "; 1 once the one-time 1.1 menu banner for Survive the Night was shown\n"
         << "survival_announced = " << (s.progress.survival_announced ? 1 : 0) << "\n"
         << "; the Archive's unlocks (release builds): what play reached, by the entries' unlock keys (src/game/archive.cpp)\n"
         << "archive = " << s.progress.archive << "\n"
         << "; 1: a finished profile was given the museum's keys every finished game reaches (done once)\n"
         << "archive_backfill = " << s.progress.archive_backfill << "\n";
    text << "\n[accessibility]\n"
         << "; not in the original; 0 keeps the original game\n"
         << "; a caption for the sounds you need to hear (a baby laughing, a phone ringing, steps behind you), with a side hint and a short\n"
         << "; rumble where vibration is on; the game's own subtitles are the options screen's\n"
         << "sound_captions = " << (s.accessibility.sound_captions ? 1 : 0) << "\n"
         << "fps_counter = " << (s.accessibility.fps_counter ? 1 : 0) << "\n"
         << "; subtitle scale: 0 normal, 1 large, 2 extra large; background plate: 0 off, 1 subtle, 2 dark\n"
         << "subtitle_size = " << s.accessibility.subtitle_size << "\n"
         << "subtitle_bg = " << s.accessibility.subtitle_bg << "\n"
         << "; 1 softens rapid screen flashes (accusations, glitches)\n"
         << "reduced_flashing = " << (s.accessibility.reduced_flashing ? 1 : 0) << "\n"
         << "; 1 allows sprinting inside the house hallway with Sprint / L3 (0 keeps the original P.T. walk pace)\n"
         << "house_sprint = " << (s.accessibility.house_sprint ? 1 : 0) << "\n"
         << "; 1 toggles sprint or zoom/aim on press instead of requiring hold\n"
         << "sprint_toggle = " << (s.accessibility.sprint_toggle ? 1 : 0) << "\n"
         << "zoom_toggle = " << (s.accessibility.zoom_toggle ? 1 : 0) << "\n"
         << "; the controller's motion sensor (DualSense, DualShock 4, Switch Pro): 0 off, 1 turns the camera, 2 aims the flashlight apart\n"
         << "; from the view (it comes back to the middle when the pad is still or L3 is pressed)\n"
         << "gyro = " << s.accessibility.gyro << "\n"
         << "; 0.25 to 4, 1 turns the view or the light as far as the controller turns\n"
         << "gyro_sensitivity = " << s.accessibility.gyro_sensitivity << "\n"
         << "; the gyro tightening, 0 to 10: a turn slower than this many degrees a second is scaled down by its share of it, so a resting\n"
         << "; controller's last jitter fades out; 0 is off. The gyro also learns its resting offset by itself while the controller rests\n"
         << "gyro_tightening = " << s.accessibility.gyro_tightening << "\n"
         << "; degrees added to the vertical angle of the view while you walk (the original's is 54.9, about 85 across at 16:9): -20 to 40;\n"
         << "; the menu offers -10, 0, 10, 20 and 30. The zoom, cutscenes, the peephole, photo mode, third person and VR keep their own view\n"
         << "field_of_view = " << s.accessibility.fov_offset << "\n";
    text << "\n[extras]\n"
         << "; not in the original; 0 keeps the original game\n"
         << "; speedrun timer: 0 off, 1 real time (everything counts), 2 game time (no pauses, no loads); records in this folder\n"
         << "speedrun = " << s.extras.speedrun << "\n"
         << "; LiveSplit: send the start, the splits and the game time to its TCP server (Control > Start TCP Server)\n"
         << "livesplit = " << (s.extras.livesplit ? 1 : 0) << "\n"
         << "livesplit_host = " << s.extras.livesplit_host << "\n"
         << "livesplit_port = " << s.extras.livesplit_port << "\n";
    text << "\n[ui]\n"
         << "; the Support on Patreon button on the start screen, the pause menu, the Museum, the loop browser and the credits: 1 shown, 0 hidden\n"
         << "support_prompts = " << (s.ui.support_prompts ? 1 : 0) << "\n"
         << "; the version the last start ran (a new version thanks you for the update once)\n"
         << "last_version = " << s.ui.last_version << "\n";
    if (!s.accessibility.gyro_bias.empty()) {
        text << "\n[gyro_bias]\n"
             << "; the resting offset (rad/s, x y z) of each controller calibrated in Accessibility > Calibrate gyro, by SDL joystick GUID\n";
        for (const auto& [guid, bias] : s.accessibility.gyro_bias) {
            text << guid << " = " << bias[0] << " " << bias[1] << " " << bias[2] << "\n";
        }
    }
    if (!s.mods.empty()) {
        text << "\n[mods]\n"
             << "; a folder in mods/ = 1 on, 0 off; applies at the next start\n";
        for (const auto& [folder, enabled] : s.mods) {
            text << folder << " = " << (enabled ? 1 : 0) << "\n";
        }
    }
    if (!s.keys.empty()) {
        text << "\n[keys]\n"
             << "; custom keyboard/mouse bindings (action = key:NAME or mouse:1|2|3); remove a line to restore its default\n";
        for (const auto& [action, token] : s.keys) {
            if (!token.empty()) text << action << " = " << token << "\n";
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto write = [&](const std::filesystem::path& file) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text.str();
        out.flush();
        return static_cast<bool>(out);
    };
    std::error_code link_error;
    if (!std::filesystem::is_symlink(path, link_error)) {
        std::filesystem::path temp = path;
        temp += ".tmp";
        if (write(temp)) {
            std::filesystem::rename(temp, path, ec);
            if (!ec) return true;
            LogWarn("settings: {} could not replace pt.ini ({}), written in place", pt::os::PathToUtf8(temp), ec.message());
        }
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
    }
    if (!write(path)) {
        LogWarn("settings: cannot write {}", pt::os::PathToUtf8(path));
        return false;
    }
    return true;
}

}
