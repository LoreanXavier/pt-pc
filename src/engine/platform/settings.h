#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace pt {

constexpr int kRayQualityOldUltraIni = 5;

struct AppSettings {
    struct Display {
        int width = 1600;
        int height = 900;
        int fullscreen = 0;
        float refresh_rate = 0.0f;
        bool vsync = true;
        int fps_limit = 0;
        bool hdr = false;
        int hdr_paper_white = 200;
        int hdr_peak = 0;
        int hdr_ui_white = 0;
        int hdr_reach = 100;
        int hdr_gamma = 0;
        bool hdr_keep_hue = false;
        int hdr_colour_range = 2;
        int hdr_exposure = 0;
        int hdr_contrast = 100;
        int hdr_highlights = 100;
        int hdr_shadows = 100;
        int hdr_saturation = 100;
        int hdr_highlight_saturation = 100;
        int hdr_blowout = 0;
        bool hdr_see_through = true;
        bool hdr_windows_switch = false;
        bool hdr_windows_held = false;
        int letterbox = 0;
        std::string rendering_api = "vulkan";
        bool rendering_api_asked = false;
        int d3d12_crashed = 0;
        int d3d12_interrupted = 0;
        bool d3d12_auto_fallback = false;
        int vulkan_implicit_layers = 1;
    } display;
    struct Input {
        float mouse_sensitivity = 1.0f;
        float gamepad_sensitivity = 1.0f;
        /* 26/255 is the DualShock 4 dead zone the original applies per axis. */
        float gamepad_dead_zone = 26.0f / 255.0f;
        bool rumble = true;
        int rumble_profile = 0;
        int prompt_style = 0;
    } input;
    struct Camera {
        float roll = 1.0f;
        bool third_person = false;
    } camera;
    struct Audio {
        float volume = 1.0f;
        bool surround = false;
        bool spatial = false;
        bool controller_speaker = false;
        float controller_speaker_volume = 0.5f;
        std::string output_device;
    } audio;
    struct Network {
        bool check_updates = true;
        bool discord_presence = true;
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
        int dlssg_multiplier = 2;
        int dlssg_interrupted = 0;
        bool ray_reconstruction = true;
        std::string neural_rendering = "off";
        std::string neural_style = "default";
        std::string neural_quality = "quality";
        std::string neural_model = "small";
        float neural_intensity = 1.0f;
        std::string neural_runtime;
        bool neural_route_armed = false;
        bool dlssg_route_armed = false;
    } upscaling;
    struct RayTracing {
        int shadows = 0;
        bool ambient_occlusion = false;
        bool contact_shadows = false;
        bool reflections = false;
        bool full_shadows = true;
        bool bounce_light = true;
        bool glossy_reflections = true;
        bool operator==(const RayTracing&) const = default;
    } ray_tracing;
    struct Graphics {
        int anisotropy = 0;
        int shadow_quality = 3;
        bool ambient_occlusion = true;
        bool reflections = true;
        bool bloom = true;
        bool depth_of_field = true;
        bool motion_blur = true;
        bool lens_distortion = true;
        bool lens_ghosts = true;
        int bloom_strength = 100;
        int flare_strength = 100;
        int ghost_strength = 100;
        bool camera_motion_blur = true;
        int motion_blur_strength = 100;
        int pt_floor_reflection = 100;
        int pt_glass_reflection = 100;
        int pt_reflection_softness = 50;
        int film_grain_type = 0;
        bool deband = false;
        float film_grain = 1.0f;
        float clarity = 0.0f;
        int texture_detail = 1;
        int ray_quality = 1;
        bool operator==(const Graphics&) const = default;
    } graphics;
    struct Game {
        bool game_plus = true;
        int survival_best_wave = 0;
        int survival_difficulty = 1;
        std::string survival_profile;
        bool operator==(const Game&) const = default;
    } game;
    struct Crosshair {
        int shape = 0;
        int color = 0;
        int length = 7;
        int thickness = 3;
        int gap = 3;
        bool dynamic = true;
        bool dot = true;
        bool outline = true;
        int opacity = 8;
        bool hit_marker = true;
        bool behind_sights = false;
        bool operator==(const Crosshair&) const = default;
    } crosshair;
    struct Extras {
        int speedrun = 0;
        bool livesplit = false;
        std::string livesplit_host = "127.0.0.1";
        int livesplit_port = 16834;
        bool operator==(const Extras&) const = default;
    } extras;
    struct Accessibility {
        bool sound_captions = false;
        int gyro = 0;
        float gyro_sensitivity = 1.0f;
        float gyro_tightening = 4.0f;
        std::map<std::string, std::array<float, 3>> gyro_bias;
        int fov_offset = 0;
        bool fps_counter = false;
        int subtitle_size = 0;
        int subtitle_bg = 0;
        bool reduced_flashing = false;
        bool house_sprint = false;
        bool sprint_toggle = false;
        bool zoom_toggle = false;
        bool operator==(const Accessibility&) const = default;
    } accessibility;
    std::map<std::string, std::string> keys;
    struct Ui {
        bool support_prompts = true;
        std::string last_version;
        bool operator==(const Ui&) const = default;
    } ui;
    struct Vr {
        bool enabled = false;
        int flashlight = 0;
        int flashlight_hand = 0;
        int turn = 0;
        float snap_degrees = 30.0f;
        float smooth_speed = 90.0f;
        float resolution_scale = 1.0f;
        float height_offset = 0.0f;
        float world_size = 1.0f;
        bool operator==(const Vr&) const = default;
    } vr;
    std::map<std::string, bool> mods;
    struct Progress {
        uint32_t loops_reached = 0;
        bool game_finished = false;
        bool night_unlocked = false;
        bool survival_announced = false;
        std::string archive;
        int archive_backfill = 0;
        bool operator==(const Progress&) const = default;
    } progress;
};

bool LoadAppSettings(const std::filesystem::path& path, AppSettings& out);
bool SaveAppSettings(const std::filesystem::path& path, const AppSettings& settings);

}
