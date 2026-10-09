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
        // frames per second at most, 0 for no limit (v-sync still applies); menus and the paused game are held to 60 either way
        int fps_limit = 0;
        bool hdr = false;
        // black bars over and under the picture in play and in the demos, not in the original: 0 off, 1 2.39:1, 2 1.85:1 (the
        // photo mode's choices)
        int letterbox = 0;
    } display;
    struct Input {
        float mouse_sensitivity = 1.0f;
        // multiplier after the original gamepad look curve; 1 keeps the original response
        float gamepad_sensitivity = 1.0f;
        /* 26/255 is the DualShock 4 dead zone the original applies per axis. */
        float gamepad_dead_zone = 26.0f / 255.0f;
        bool rumble = true;
        int rumble_profile = 0;
    } input;
    struct Camera {
        float roll = 1.0f;
        // the experimental third person view (Extras), not in the original
        bool third_person = false;
    } camera;
    struct Audio {
        float volume = 1.0f;
        bool surround = false;
        bool controller_speaker = false;
        float controller_speaker_volume = 0.5f;
    } audio;
    // a check for a newer release when the game starts (docs/updates.md); off sends nothing
    struct Network {
        bool check_updates = true;
    } network;
    // voice recognition is always on (an older pt.ini's voice.enabled is ignored)
    struct Voice {
        std::string device;
        std::string key;
    } voice;
    struct Upscaling {
        std::string upscaler = "off";
        std::string quality = "quality";
        float scale = 0.667f;
        float sharpness = 0.0f;
        // the DLSS model: auto (NVIDIA's default per quality mode), k, l, m
        std::string dlss_model = "auto";
        // off, fsr3 (AMD FSR 3) or dlss (NVIDIA DLSS Frame Generation); 0 and 1 from older builds read as off and fsr3
        std::string frame_generation = "off";
    } upscaling;
    struct RayTracing {
        // 0 off, 1 sharp (as the shadow maps), 2 soft (penumbrae from the lights' size)
        int shadows = 0;
        // ray traced ambient occlusion of the probe ambient
        bool ambient_occlusion = false;
        // ray traced contact shadows for every light
        bool contact_shadows = false;
        // ray traced local reflections (an experiment: floors also reflect what is off screen)
        bool reflections = false;
        bool operator==(const RayTracing&) const = default;
    } ray_tracing;
    // PC options that work on every GPU; each default is the original's look
    struct Graphics {
        // anisotropic filtering of the textures: 0 the original's trilinear filtering, else 2, 4, 8 or 16
        int anisotropy = 0;
        // textures upscaled from the game files by the bundled upscaler, generated once into the user data folder
        bool enhanced_textures = false;
        // Raster shadow tile: 0 off, 1 512 px, 2 1024 px, 3 original 2048 px.
        int shadow_quality = 3;
        bool ambient_occlusion = true;
        bool reflections = true;
        bool bloom = true;
        bool depth_of_field = true;
        bool motion_blur = true;
        bool lens_distortion = true;
        // the lens flares' full screen ghosts (the original's large reflections across the screen near a lamp): on draws
        // them at the strength measured from the original, off leaves them out (rendering.md, lens flares)
        bool lens_ghosts = true;
        float film_grain = 1.0f;
        float clarity = 0.0f;
        // 0 reduced mip detail, 1 original, 2 fine detail.
        int texture_detail = 1;
        int ray_quality = 1;
        bool operator==(const Graphics&) const = default;
    } graphics;
    // [extras]: PC extras that change what the game shows or does; every default keeps the original
    struct Extras {
        // the speedrun timer: 0 off, 1 real time, 2 game time (docs/gameplay.md, speedrun mode)
        int speedrun = 0;
        // send the speedrun's start, splits and game time to LiveSplit's TCP server (an IPv4 address and its port)
        bool livesplit = false;
        std::string livesplit_host = "127.0.0.1";
        int livesplit_port = 16834;
        bool operator==(const Extras&) const = default;
    } extras;
    // [vr]: the experimental VR mode (docs/vr.md), untested on a real headset; off changes nothing. Read at start only.
    struct Vr {
        bool enabled = false;
        // the flashlight: 0 follows the head as the game's camera does, 1 the tracked controller (vr.flashlight_hand)
        int flashlight = 0;
        // 0 left, 1 right
        int flashlight_hand = 0;
        // turning with the right stick: 0 snap turns of vr.snap_degrees, 1 smooth at vr.smooth_speed degrees a second
        int turn = 0;
        float snap_degrees = 30.0f;
        float smooth_speed = 90.0f;
        // the eye images' size against the runtime's recommendation
        float resolution_scale = 1.0f;
        float height_offset = 0.0f;
        float world_scale = 1.0f;
        bool operator==(const Vr&) const = default;
    } vr;
    // [mods]: a mod folder's name = 0 or 1, over its mod.json "enabled" (docs/modding.md); read at start only
    std::map<std::string, bool> mods;
    // [progress]: the loop browser's unlocks in release builds (docs/gameplay.md, loop browser). Kept here and not in the save, so
    // Reset Progress keeps them; the Extras page can clear them
    struct Progress {
        // bit i: the browser's entry i (src/game/loop_browser.h) was reached in normal play
        uint32_t loops_reached = 0;
        // the game was finished once (the ending's credits ran to their end)
        bool game_finished = false;
        // the Archive's unlock keys reached in play (src/game/archive.h), separated by commas; Extras "Lock the archive" clears it
        std::string archive;
        bool operator==(const Progress&) const = default;
    } progress;
};

bool LoadAppSettings(const std::filesystem::path& path, AppSettings& out);
bool SaveAppSettings(const std::filesystem::path& path, const AppSettings& settings);

}
