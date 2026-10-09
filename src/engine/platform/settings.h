#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace pt {

struct AppSettings {
    struct Display {
        int width = 1600;
        int height = 900;
        int fullscreen = 0;
        bool vsync = true;
        bool pause_on_focus_loss = true;
        bool mute_in_background = false;
        int fps_limit = 0;
        bool hdr = false;
        int letterbox = 0;
    } display;
    struct Input {
        float mouse_sensitivity = 1.0f;
        float gamepad_sensitivity = 1.0f;
        /* 26/255 is the DualShock 4 dead zone the original applies per axis. */
        float gamepad_dead_zone = 26.0f / 255.0f;
        bool rumble = true;
        int rumble_profile = 0;
    } input;
    struct Camera {
        float roll = 1.0f;
        bool third_person = false;
    } camera;
    struct Audio {
        float volume = 1.0f;
        bool surround = false;
        bool controller_speaker = false;
        float controller_speaker_volume = 0.5f;
    } audio;
    struct Network {
        bool check_updates = true;
    } network;
    struct Voice {
        std::string device;
        std::string key;
    } voice;
    struct Upscaling {
        std::string upscaler = "off";
        std::string quality = "quality";
        float scale = 0.667f;
        float sharpness = 0.0f;
        std::string dlss_model = "auto";
        std::string frame_generation = "off";
    } upscaling;
    struct RayTracing {
        int shadows = 0;
        bool ambient_occlusion = false;
        bool contact_shadows = false;
        bool reflections = false;
        bool operator==(const RayTracing&) const = default;
    } ray_tracing;
    struct Graphics {
        int anisotropy = 0;
        bool enhanced_textures = false;
        int shadow_quality = 3;
        bool ambient_occlusion = true;
        bool reflections = true;
        bool bloom = true;
        bool depth_of_field = true;
        bool motion_blur = true;
        bool lens_distortion = true;
        bool lens_ghosts = true;
        float film_grain = 1.0f;
        float clarity = 0.0f;
        int texture_detail = 1;
        int ray_quality = 1;
        bool operator==(const Graphics&) const = default;
    } graphics;
    struct Extras {
        int speedrun = 0;
        bool livesplit = false;
        std::string livesplit_host = "127.0.0.1";
        int livesplit_port = 16834;
        bool operator==(const Extras&) const = default;
    } extras;
    struct Vr {
        bool enabled = false;
        int flashlight = 0;
        int flashlight_hand = 0;
        int turn = 0;
        float snap_degrees = 30.0f;
        float smooth_speed = 90.0f;
        float resolution_scale = 1.0f;
        float height_offset = 0.0f;
        float world_scale = 1.0f;
        bool operator==(const Vr&) const = default;
    } vr;
    std::map<std::string, bool> mods;
    struct Progress {
        uint32_t loops_reached = 0;
        bool game_finished = false;
        std::string archive;
        bool operator==(const Progress&) const = default;
    } progress;
};

bool LoadAppSettings(const std::filesystem::path& path, AppSettings& out);
bool SaveAppSettings(const std::filesystem::path& path, const AppSettings& settings);

}
