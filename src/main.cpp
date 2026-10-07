#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "engine/platform/update_check.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <shobjidl.h>
#undef small
#endif
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include "engine/anim/anim_test.h"
#include "engine/assets/fmdl.h"
#include "engine/assets/enhanced_textures.h"
#include "engine/assets/geom.h"
#include "engine/core/log.h"
#include "engine/core/crash_report.h"
#include "engine/core/memory_status.h"
#include "build_id.h"
#include "engine/data/fox2.h"
#include "engine/fs/fox_crypt.h"
#include "engine/fs/mods.h"
#include "engine/fs/vfs.h"
#include "engine/physics/collision_world.h"
#include "engine/platform/input.h"
#include "engine/platform/livesplit.h"
#include "engine/render/model_cache.h"
#include "engine/render/renderer.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/texture_manager.h"
#include "engine/render/upscale/frame_generation.h"
#include "engine/render/upscale/streamline.h"
#include "engine/render/vfx_pass.h"
#include "engine/vfx/vfx_system.h"
#include "engine/script/lua_vm.h"
#include "engine/ui/asset_browser.h"
#include "engine/core/resource_path.h"
#include "engine/platform/settings.h"
#include "engine/platform/graphics_presets.h"
#include "engine/platform/virtual_pad.h"
#include "engine/voice/microphone.h"
#include "engine/voice/voice_recognizer.h"
#include "game/archive.h"
#include "game/archive_theater.h"
#include "game/debug_panel.h"
#include "game/game.h"
#include "game/loop_browser.h"
#include "game/render_mouse.h"
#include "game/game_sound.h"
#include "game/input_script.h"
#include "game/prompt_textures.h"
#include "game/render_scene.h"
#include "game/ui/photo_panel.h"
#include "game/script_api.h"
#include "game/stage_data.h"
#include "game/ui/game_ui.h"
#include "game/ui/pc_settings.h"
#include "game/vfx_scene.h"
#include "game/vr_play.h"
#include "engine/xr/xr_host.h"

namespace {

#ifdef PT_RELEASE_LOCKS
constexpr bool kReleaseLocks = true;
#else
constexpr bool kReleaseLocks = false;
#endif

struct Options {
    std::filesystem::path game_dir = "game/CUSA01127";
    bool game_dir_given = false;
    bool headless = false;
    bool debug_panel = false;
    bool dev_ui = false;
    bool validation = false;
    bool vsync = true;
    std::filesystem::path screenshot;
    int frames = 3;
    bool script_test = false;
    bool anim_test = false;
    std::filesystem::path voice_test;
    float voice_listen = 0.0f;
    std::string voice_device;
    std::string fox2_test;
    std::string stage;
    std::string texture_test;
    bool has_camera = false;
    glm::vec4 camera{0.0f};
    bool top_view = false;
    std::filesystem::path input_script;
    std::string input_text;
    std::string start_floor;
    uint32_t seed = 0;
    int handy_light_roll = 0;
    int bug_screen = -1;
    std::vector<std::string> lua_snippets;
    bool no_save = false;
    bool audio_offline = false;
    uint32_t audio_period = 0;
    std::filesystem::path audio_capture;
    std::filesystem::path save_dir;
    float demo_rate = 1.0f;
    float tick_rate = 60.0f;
    float display_rate = 0.0f;
    bool trap_log = false;
    bool first_boot_options = false;
    uint32_t width = 1600;
    uint32_t height = 900;
    bool size_set = false;
    bool vsync_set = false;
    bool render_all = false;
    int shot_warmup = 0;
    bool shot_settle = false;
    std::filesystem::path settings_path;
    std::filesystem::path log_path;
    bool virtual_pads = false;
    bool list_pads = false;
    std::optional<pt::PromptStyle> forced_prompts;
    std::filesystem::path mods_dir;
    bool no_mods = false;
    bool no_update_check = false;
    std::string fake_update;
    int street_offer = 0;
    bool release_locks = kReleaseLocks;
    bool third_person = false;
    std::filesystem::path make_loop_previews;
    std::filesystem::path make_museum_previews;
    std::filesystem::path museum_previews;
    bool vr = false;
    bool no_vr = false;
};

struct OptionSpec {
    const char* name;
    int values;
    const char* help;
};
constexpr OptionSpec kOptionSpecs[] = {
    {"--help", 0, "print this list and exit (also -h)"},
    {"--game", 1, "<folder> the extracted CUSA01127 folder"},
    {"--settings", 1, "<file> pt.ini to use instead of the one in the user folder"},
    {"--save-dir", 1, "<folder> where saves go"},
    {"--no-save", 0, "never write a save"},
    {"--log", 1, "<file> the log file (default: pt.log in the user folder, or the working folder for tools)"},
    {"--mods", 1, "<folder> the mods folder"},
    {"--no-mods", 0, "load no mods"},
    {"--no-update-check", 0, "do not look for a newer release"},
    {"--fake-update", 1, "<version> pretend this version was released (tests)"},
    {"--vr", 0, "the experimental VR mode (an OpenXR runtime and a headset; docs/vr.md)"},
    {"--no-vr", 0, "no VR mode whatever pt.ini says"},
    {"--no-vsync", 0, "present without v-sync"},
    {"--width", 1, "<pixels> window or render width"},
    {"--height", 1, "<pixels> window or render height"},
    {"--debug", 0, "debug panel and developer UI"},
    {"--dev-ui", 0, "developer UI (Tab toggles the debug panel)"},
    {"--validation", 0, "Vulkan validation layers"},
    {"--headless", 0, "no window: run the given number of frames and exit (tests)"},
    {"--frames", 1, "<n> frames to run headless"},
    {"--screenshot", 1, "<file> write the last frame"},
    {"--shot-warmup", 1, "<n> frames before each shot"},
    {"--shot-settle", 0, "wait for streaming before each shot"},
    {"--render-all", 0, "render every frame headless"},
    {"--input-script", 1, "<file> scripted input (tools/walkthrough.py routes)"},
    {"--input", 1, "<\"frame command args; ...\"> scripted input inline"},
    {"--lua", 1, "<frame:code> run Lua at a frame"},
    {"--start-floor", 1, "<fNNN> start on a floor"},
    {"--street-offer", 1, "<walk|restart|never> answer to the street offer"},
    {"--release-locks", 0, "the release build's loop browser locks"},
    {"--third-person", 0, "the experimental third person view on (Extras; pt.ini is not changed)"},
    {"--no-release-locks", 0, "no loop browser locks"},
    {"--make-loop-previews", 1, "<folder> shoot the loop browser previews and exit (headless)"},
    {"--make-museum-previews", 1, "<folder> shoot the Museum's thumbnails and exit (headless)"},
    {"--museum-previews", 1, "<folder> the Museum's thumbnails to show (tests)"},
    {"--seed", 1, "<n> random seed"},
    {"--f160-light", 1, "<red|green|blue|yellow|white|1-100> the f160 light roll"},
    {"--bug-screen", 1, "<-1..6> force the f120 bug screen"},
    {"--tick-rate", 1, "<hz> game tick rate"},
    {"--display-rate", 1, "<hz> paced display rate"},
    {"--demo-rate", 1, "<x> demo playback speed (tests)"},
    {"--audio-offline", 0, "render audio without a device"},
    {"--audio-period", 1, "<frames> offline audio period"},
    {"--audio-capture", 1, "<wav> write the mixed audio"},
    {"--trap-log", 0, "log trap evaluation"},
    {"--options-menu", 0, "show the first boot options menu"},
    {"--virtual-pads", 0, "scripted virtual gamepads"},
    {"--list-pads", 0, "list the connected gamepads and exit"},
    {"--prompts", 1, "<playstation|xbox|nintendo> force the button prompts"},
    {"--camera", 4, "<x y z yaw> start camera (viewer)"},
    {"--stage", 1, "<package> open a stage in the viewer"},
    {"--top", 0, "top view in the viewer"},
    {"--texture-test", 1, "<name> texture viewer"},
    {"--fox2-test", 1, "<package> parse a fox2 file and exit"},
    {"--script-test", 0, "run the script tests and exit"},
    {"--anim-test", -1, "[folder] run the animation tests and exit"},
    {"--anim-only", 1, "<name> one animation for --anim-test"},
    {"--voice-test", 1, "<wav | folder | list.txt> run files through the voice recognizer and exit"},
    {"--voice-listen", 1, "<seconds> the microphone through the voice recognizer in the console, then exit"},
    {"--voice-device", 1, "<name> recording device for --voice-listen"},
};

void PrintOptions(FILE* out) {
    std::fprintf(out, "pt.exe [options]\n\nWithout options the game starts. Options:\n");
    for (const OptionSpec& spec : kOptionSpecs) std::fprintf(out, "  %-22s %s\n", spec.name, spec.help);
}

int OptionValues(std::string_view arg) {
    for (const OptionSpec& spec : kOptionSpecs)
        if (arg == spec.name) return spec.values;
    return -2;
}

[[noreturn]] void RefuseCommandLine(const std::string& message) {
    std::fprintf(stderr, "pt.exe: %s\n\n", message.c_str());
    PrintOptions(stderr);
    std::exit(2);
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    if (const char* env = std::getenv("PT_GAME_DIR")) {
        options.game_dir = env;
        options.game_dir_given = true;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "/?") {
            PrintOptions(stdout);
            std::fflush(stdout);
            std::exit(0);
        }
        const int values = OptionValues(arg);
        if (values == -2) RefuseCommandLine("unknown option '" + arg + "'");
        for (int v = 1; v <= values; ++v) {
            if (i + v >= argc) RefuseCommandLine(arg + " needs " + (values == 1 ? std::string("a value") : std::to_string(values) + " values"));
        }
        const bool has_value = i + 1 < argc;
        if (arg == "--anim-test") {
            if (has_value && !std::string_view(argv[i + 1]).starts_with("--")) ++i;
            options.anim_test = true;
        } else if (arg == "--anim-only") {
            ++i;
        } else if (arg == "--game" && has_value) {
            options.game_dir = argv[++i];
            options.game_dir_given = true;
        } else if (arg == "--debug") {
            options.debug_panel = true;
            options.dev_ui = true;
        } else if (arg == "--dev-ui") {
            options.dev_ui = true;
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--camera" && i + 4 < argc) {
            options.has_camera = true;
            options.camera = glm::vec4(std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3]), std::stof(argv[i + 4]));
            i += 4;
        } else if (arg == "--texture-test" && has_value) {
            options.texture_test = argv[++i];
        } else if (arg == "--stage" && has_value) {
            options.stage = argv[++i];
        } else if (arg == "--top") {
            options.top_view = true;
        } else if (arg == "--fox2-test" && has_value) {
            options.fox2_test = argv[++i];
        } else if (arg == "--voice-test" || arg == "--voice-listen") {
            if (!has_value || !*argv[i + 1]) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(2);
            }
            if (arg == "--voice-test") options.voice_test = argv[++i];
            else options.voice_listen = std::strtof(argv[++i], nullptr);
            if (arg == "--voice-listen" && !(options.voice_listen > 0.0f)) {
                std::fprintf(stderr, "--voice-listen needs a number of seconds\n");
                std::exit(2);
            }
        } else if (arg == "--voice-device" && has_value) {
            options.voice_device = argv[++i];
        } else if (arg == "--script-test") {
            options.script_test = true;
        } else if (arg == "--input-script" && has_value) {
            options.input_script = argv[++i];
        } else if (arg == "--input" && has_value) {
            options.input_text = argv[++i];
        } else if (arg == "--lua" && has_value) {
            options.lua_snippets.push_back(argv[++i]);
        } else if (arg == "--start-floor" && has_value) {
            options.start_floor = argv[++i];
        } else if (arg == "--street-offer" && has_value) {
            const std::string answer = argv[++i];
            options.street_offer = answer == "walk" ? 1 : answer == "restart" ? 2 : answer == "never" ? -1 : 0;
        } else if (arg == "--release-locks") {
            options.release_locks = true;
        } else if (arg == "--no-release-locks") {
            options.release_locks = false;
        } else if (arg == "--third-person") {
            options.third_person = true;
        } else if (arg == "--make-loop-previews" && has_value) {
            options.make_loop_previews = argv[++i];
        } else if (arg == "--make-museum-previews" && has_value) {
            options.make_museum_previews = argv[++i];
        } else if (arg == "--museum-previews" && has_value) {
            options.museum_previews = argv[++i];
        } else if (arg == "--seed" && has_value) {
            options.seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--f160-light" && has_value) {
            const std::string colour = argv[++i];
            options.handy_light_roll = colour == "red"      ? 1
                                       : colour == "green"  ? 6
                                       : colour == "blue"   ? 11
                                       : colour == "yellow" ? 16
                                       : colour == "white"  ? 21
                                                            : std::clamp(std::atoi(colour.c_str()), 0, 100);
        } else if (arg == "--bug-screen" && has_value) {
            options.bug_screen = std::clamp(std::atoi(argv[++i]), -1, 6);
        } else if (arg == "--tick-rate" && has_value) {
            options.tick_rate = std::max(10.0f, std::stof(argv[++i]));
        } else if (arg == "--display-rate" && has_value) {
            options.display_rate = std::max(0.0f, std::stof(argv[++i]));
        } else if (arg == "--demo-rate" && has_value) {
            options.demo_rate = std::stof(argv[++i]);
        } else if (arg == "--audio-offline") {
            options.audio_offline = true;
        } else if (arg == "--audio-period" && has_value) {
            options.audio_period = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            options.audio_offline = true;
        } else if (arg == "--audio-capture" && has_value) {
            options.audio_capture = argv[++i];
            options.audio_offline = true;
        } else if (arg == "--save-dir" && has_value) {
            options.save_dir = argv[++i];
        } else if (arg == "--no-save") {
            options.no_save = true;
        } else if (arg == "--trap-log") {
            options.trap_log = true;
        } else if (arg == "--options-menu") {
            options.first_boot_options = true;
        } else if (arg == "--virtual-pads") {
            options.virtual_pads = true;
        } else if (arg == "--list-pads") {
            options.list_pads = true;
        } else if (arg == "--prompts" && has_value) {
            const std::string device = argv[++i];
            pt::PromptStyle style;
            if (device == "playstation") {
                style.device = pt::PromptDevice::PlayStation;
            } else if (device == "xbox") {
                style.device = pt::PromptDevice::Xbox;
                style.faces = {'A', 'B', 'X', 'Y'};
            } else if (device == "nintendo") {
                style.device = pt::PromptDevice::Nintendo;
                style.faces = {'B', 'A', 'Y', 'X'};
            }
            options.forced_prompts = style;
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--render-all") {
            options.render_all = true;
        } else if (arg == "--shot-warmup" && has_value) {
            options.shot_warmup = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--shot-settle") {
            options.shot_settle = true;
        } else if (arg == "--no-vsync") {
            options.vsync = false;
            options.vsync_set = true;
        } else if (arg == "--mods" && has_value) {
            options.mods_dir = argv[++i];
        } else if (arg == "--no-mods") {
            options.no_mods = true;
        } else if (arg == "--vr") {
            options.vr = true;
        } else if (arg == "--no-vr") {
            options.no_vr = true;
        } else if (arg == "--no-update-check") {
            options.no_update_check = true;
        } else if (arg == "--fake-update" && has_value) {
            options.fake_update = argv[++i];
        } else if (arg == "--settings" && has_value) {
            options.settings_path = argv[++i];
        } else if (arg == "--log" && has_value) {
            options.log_path = argv[++i];
        } else if (arg == "--screenshot" && has_value) {
            options.screenshot = argv[++i];
        } else if (arg == "--frames" && has_value) {
            options.frames = std::atoi(argv[++i]);
        } else if (arg == "--width" && has_value) {
            options.width = static_cast<uint32_t>(std::atoi(argv[++i]));
            options.size_set = true;
        } else if (arg == "--height" && has_value) {
            options.height = static_cast<uint32_t>(std::atoi(argv[++i]));
            options.size_set = true;
        }
    }
    if (const char* warmup = std::getenv("PT_SHOT_WARMUP"); warmup && options.shot_warmup == 0) {
        options.shot_warmup = std::max(0, std::atoi(warmup));
    }
    return options;
}

int RunScriptTest(pt::Vfs& vfs) {
    pt::LuaVm vm;
    pt::game::RegisterStubApi(vm);
    int loaded = 0;
    int failed = 0;
    for (const auto& name : vfs.Archive().Names()) {
        if (name.ends_with(".fpkd")) {
            auto package = vfs.LoadPackage(name);
            if (!package) {
                continue;
            }
            for (const auto& entry : package->Entries()) {
                if (entry.path.ends_with(".lua")) {
                    auto code = package->Read(entry);
                    vm.RunChunk(entry.path, code) ? ++loaded : ++failed;
                }
            }
        } else if (name.ends_with(".lua") && !name.starts_with("shaders/")) {
            auto code = vfs.Archive().Read(name);
            if (code && pt::fox::IsWrapped(*code)) {
                auto plain = pt::fox::Unwrap(*code);
                vm.RunChunk(name, plain) ? ++loaded : ++failed;
            }
        }
    }
    pt::LogInfo("script test: {} chunks ran, {} failed", loaded, failed);
    return failed == 0 ? 0 : 2;
}

int RunFox2Test(pt::Vfs& vfs, const std::string& package_path) {
    auto package = vfs.LoadPackage(package_path);
    if (!package) {
        return 1;
    }
    for (const auto& entry : package->Entries()) {
        if (!entry.path.ends_with(".fox2")) {
            continue;
        }
        pt::fox2::DataSetFile file;
        const auto data = package->Read(entry);
        if (!file.Load(entry.path, data)) {
            return 1;
        }
        std::map<std::string, int> counts;
        for (const auto& e : file.Entities()) {
            ++counts[e.class_name];
        }
        pt::LogInfo("{}: {} entities, {} classes", entry.path, file.Entities().size(), counts.size());
    }
    return 0;
}

int RunVoiceTest(const std::filesystem::path& input) {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        return 1;
    }
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(input)) {
        for (const auto& entry : std::filesystem::directory_iterator(input)) {
            if (entry.path().extension() == ".wav") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
    } else if (input.extension() == ".txt") {
        std::ifstream list(input);
        for (std::string line; std::getline(list, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) files.emplace_back(std::filesystem::u8path(line));
        }
    } else {
        files.push_back(input);
    }
    pt::VoiceRecognizer recognizer;
    if (!recognizer.Init(pt::ResourceDir("voice", PT_VOICE_MODEL_DIR), "jack")) {
        return 1;
    }
    recognizer.Drain();
    if (recognizer.GetState() != pt::VoiceRecognizer::State::Ready) {
        return 1;
    }
    int total = 0;
    for (const std::filesystem::path& wav_path : files) {
        SDL_AudioSpec spec{};
        Uint8* data = nullptr;
        Uint32 length = 0;
        if (!SDL_LoadWAV(wav_path.string().c_str(), &spec, &data, &length)) {
            pt::LogError("voice test: cannot load {}: {}", wav_path.string(), SDL_GetError());
            continue;
        }
        const SDL_AudioSpec target{SDL_AUDIO_S16, 1, pt::VoiceRecognizer::kSampleRate};
        Uint8* converted = nullptr;
        int converted_length = 0;
        SDL_ConvertAudioSamples(&spec, data, static_cast<int>(length), &target, &converted, &converted_length);
        SDL_free(data);
        recognizer.Reset();
        recognizer.TakeResults();
        const int16_t* samples = reinterpret_cast<const int16_t*>(converted);
        const size_t count = static_cast<size_t>(converted_length) / 2;
        int detections = 0;
        for (size_t pos = 0; pos < count; pos += pt::VoiceRecognizer::kSampleRate) {
            const size_t n = std::min<size_t>(pt::VoiceRecognizer::kSampleRate, count - pos);
            detections += recognizer.Feed({samples + pos, n}) ? 1 : 0;
            detections += recognizer.Drain() ? 1 : 0;
        }
        detections += recognizer.Finish() ? 1 : 0;
        std::string heard;
        const std::vector<pt::VoiceRecognizer::Result> results = recognizer.TakeResults();
        for (const pt::VoiceRecognizer::Result& r : results) {
            heard += std::format("{}[{}|{:.3f}|{:.2f}s|{:.0f}ms]", heard.empty() ? "" : " ", r.text, r.jack_probability, r.seconds, r.decode_ms);
        }
        pt::LogInfo("voice test: file {} | {} detections | {} utterances | {:.2f} s | {}", wav_path.filename().string(), detections,
                    results.size(), static_cast<double>(count) / pt::VoiceRecognizer::kSampleRate, heard);
        total += detections > 0 ? 1 : 0;
        SDL_free(converted);
    }
    pt::LogInfo("voice test: {} of {} files with the word", total, files.size());
    SDL_Quit();
    return 0;
}

void WriteWav(const std::filesystem::path& path, const std::vector<float>& samples, uint32_t rate, uint16_t channels) {
    std::ofstream out(path, std::ios::binary);
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + data_bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3);
    u16(channels);
    u32(rate);
    u32(rate * channels * 4);
    u16(static_cast<uint16_t>(channels * 4));
    u16(32);
    out.write("data", 4);
    u32(data_bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
    pt::LogInfo("audio capture: {} s written to {}", samples.size() / channels / static_cast<double>(rate), path.string());
}

struct App {
    Options options;
    pt::AppSettings settings;
    pt::update::Checker updates;
    bool update_polled = false;
    std::filesystem::path settings_path;
    std::filesystem::path streamline_marker;
    SDL_Window* window = nullptr;
    bool microphone_test = false;
    int extras_request = 0;
    std::string archive_request;
    pt::game::SubtitlePlayer* transcripts = nullptr;
    bool freecam_active = false;
    bool photo_active = false;
    bool microphone_monitor = false;
    float microphone_db = -80.0f;
    std::string microphone_hypothesis;
    std::string microphone_status = "pc_mic_waiting";
    pt::Renderer renderer;
    pt::TextureManager textures;
    pt::SceneRenderer scene;
    std::unique_ptr<pt::ModelCache> models;
    pt::EnhancedTextureJob texture_job;
    std::filesystem::path texture_cache;
    uint32_t texture_max_output = 0;
    std::filesystem::path texture_runtime;
    pt::Vfs* vfs = nullptr;
    bool texture_requested = false;
    std::unique_ptr<pt::mods::ModSet> mods;
    std::unique_ptr<pt::xr::Host> xr;
    pt::LiveSplitClient livesplit;
};

/* the game itself, for the preview captures it starts in the background */
std::filesystem::path GameExecutable() {
#ifdef __APPLE__
    return pt::ExecutablePath();
#elif defined(_WIN32)
    return std::filesystem::path(SDL_GetBasePath()) / "pt.exe";
#else
    return std::filesystem::path(SDL_GetBasePath()) / "pt";
#endif
}

std::filesystem::path UserDataDir();

std::filesystem::path DefaultModsDir() {
#ifdef __APPLE__
    /* the app bundle is no place for user files: mods/ in ~/Library/Application Support/pt-port/pt (docs/macos.md), unless a
       build folder has one next to pt */
    std::error_code ec;
    if (!std::filesystem::is_directory(pt::ExecutableDir() / "mods", ec)) {
        if (const std::filesystem::path user = UserDataDir(); !user.empty()) return user / "mods";
    }
#endif
    return pt::ExecutableDir() / "mods";
}

void MountMods(App& app) {
    const Options& options = app.options;
    if (options.no_mods || (options.headless && options.mods_dir.empty())) {
        return;
    }
    const std::filesystem::path dir = options.mods_dir.empty() ? DefaultModsDir() : options.mods_dir;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        if (!options.mods_dir.empty()) {
            pt::LogWarn("mods: {} is not a folder", dir.string());
        }
        return;
    }
    std::vector<std::string> warnings;
    app.mods = pt::mods::Load(dir, app.settings.mods, &warnings);
    for (const std::string& warning : warnings) {
        pt::LogWarn("mods: {}", warning);
    }
    for (const pt::mods::Mod& mod : app.mods->mods) {
        pt::LogInfo("mods: {} {} ({} files){}{}", mod.Name(), mod.manifest.version.empty() ? "-" : mod.manifest.version, mod.files.size(),
                    mod.has_script ? ", init.lua" : "", mod.enabled ? "" : ", disabled");
    }
    pt::mods::SetActive(app.mods.get());
    pt::LogInfo("mods: {} found in {}, {} files replaced", app.mods->mods.size(), dir.string(), app.mods->index.Size());
}

void RequestEnhancedTextures(App& app, bool enabled) {
    app.texture_requested = enabled;
    if (!enabled) {
        app.texture_job.Cancel();
        app.textures.SetEnhancedTextures(false);
        app.settings.graphics.enhanced_textures = false;
        return;
    }
    app.settings.graphics.enhanced_textures = true;
    const uint64_t model = pt::EnhancedModelKey(app.texture_runtime);
    app.textures.ConfigureEnhancedTextures(app.vfs->Textures(), app.texture_cache, model);
    app.texture_job.Start(app.vfs->GameDir(), app.texture_cache, app.texture_runtime, 0, app.texture_max_output);
}

void PollEnhancedTextures(App& app) {
    if (!app.texture_requested) return;
    const auto status = app.texture_job.GetStatus();
    if (status.state == pt::EnhancedTextureJob::State::Running) return;
    if(status.state == pt::EnhancedTextureJob::State::Cancelled) {
        app.texture_job.Start(app.vfs->GameDir(),app.texture_cache,app.texture_runtime,0,app.texture_max_output);
        return;
    }
    const bool ready = status.state == pt::EnhancedTextureJob::State::Ready;
    app.textures.SetEnhancedTextures(ready);
    app.texture_requested = false;
    if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
}

void ApplyFullscreen(App& app) {
    if (!app.window) {
        return;
    }
    const int mode = app.settings.display.fullscreen;
    if (mode == 2) {
        if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(app.window))) {
            SDL_SetWindowFullscreenMode(app.window, desktop);
        }
    } else {
        SDL_SetWindowFullscreenMode(app.window, nullptr);
    }
    SDL_SetWindowFullscreen(app.window, mode != 0);
}

std::filesystem::path g_output_dir;

std::filesystem::path UserDataDir() {
    std::filesystem::path dir;
    if (char* pref = SDL_GetPrefPath("pt-port", "pt")) {
        dir = std::filesystem::path(reinterpret_cast<const char8_t*>(pref));
        SDL_free(pref);
    }
    return dir;
}

bool LooksLikeGameDir(const std::filesystem::path& dir) {
    std::error_code error;
    return !dir.empty() && std::filesystem::exists(dir / "chunk1.psarc", error) && std::filesystem::exists(dir / "texture.qar", error);
}

std::filesystem::path RememberedGameDirFile() {
    const std::filesystem::path user = UserDataDir();
    return user.empty() ? std::filesystem::path() : user / "game_dir.txt";
}

std::filesystem::path FindGameDir(const Options& options) {
    if (options.game_dir_given || LooksLikeGameDir(options.game_dir)) {
        return options.game_dir;
    }
    std::filesystem::path dir = pt::ExecutableDir();
    if (!dir.empty() && dir.filename().empty()) {
        dir = dir.parent_path();
    }
    for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
        for (const std::filesystem::path candidate : {dir / "game" / "CUSA01127", dir / "CUSA01127"}) {
            if (LooksLikeGameDir(candidate)) {
                return candidate;
            }
        }
        if (dir == dir.parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    const std::filesystem::path remembered_file = RememberedGameDirFile();
    if (!remembered_file.empty()) {
        std::ifstream in(remembered_file, std::ios::binary);
        std::string line;
        if (std::getline(in, line)) {
            const std::filesystem::path remembered = std::filesystem::path(reinterpret_cast<const char8_t*>(line.c_str()));
            if (LooksLikeGameDir(remembered)) {
                return remembered;
            }
        }
    }
#ifdef _WIN32
    if (!options.headless && SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        std::filesystem::path picked;
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            DWORD flags = 0;
            dialog->GetOptions(&flags);
            dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dialog->SetTitle(L"P.T.: select the extracted CUSA01127 folder (it contains chunk1.psarc)");
            IShellItem* item = nullptr;
            PWSTR path = nullptr;
            if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                picked = path;
                CoTaskMemFree(path);
            }
            if (item) {
                item->Release();
            }
            dialog->Release();
        }
        CoUninitialize();
        if (LooksLikeGameDir(picked)) {
            if (!remembered_file.empty()) {
                std::ofstream out(remembered_file, std::ios::binary | std::ios::trunc);
                const std::u8string text = picked.u8string();
                out.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size()));
            }
            return picked;
        }
    }
#elif defined(__APPLE__)
    /* macOS has no installer (docs/macos.md): the first start asks for the folder, as on Windows */
    if (!options.headless && SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        struct Pick {
            std::filesystem::path path;
            bool done = false;
        } pick;
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "P.T.: select the extracted CUSA01127 folder (it contains chunk1.psarc)");
        SDL_ShowFileDialogWithProperties(
            SDL_FILEDIALOG_OPENFOLDER,
            [](void* user, const char* const* list, int) {
                auto* p = static_cast<Pick*>(user);
                if (list && list[0]) p->path = std::filesystem::path(reinterpret_cast<const char8_t*>(list[0]));
                p->done = true;
            },
            &pick, props);
        SDL_DestroyProperties(props);
        while (!pick.done) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        std::filesystem::path picked = pick.path;
        if (!LooksLikeGameDir(picked) && LooksLikeGameDir(picked / "CUSA01127")) picked /= "CUSA01127";
        if (LooksLikeGameDir(picked)) {
            if (!remembered_file.empty()) {
                std::ofstream out(remembered_file, std::ios::binary | std::ios::trunc);
                const std::u8string text = picked.u8string();
                out.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size()));
            }
            return picked;
        }
    }
#endif
    return options.game_dir;
}

int RunVoiceListen(float seconds, const std::string& device) {
    struct Prompt {
        float at;
        const char* text;
    };
    static constexpr Prompt kPrompts[] = {
        {0, "Stay quiet for 10 seconds while the recognizer starts."},
        {10, "Say JACK now, at your normal volume (1 of 10)."}, {18, "Say JACK (2 of 10)."},
        {26, "Say JACK quietly (3 of 10)."}, {34, "Say JACK loudly (4 of 10)."},
        {42, "Say JACK, a bit further from the microphone (5 of 10)."}, {50, "Say JACK quickly (6 of 10)."},
        {58, "Say JACK slowly (7 of 10)."}, {66, "Say HEY JACK (8 of 10)."}, {74, "Say JACK (9 of 10)."},
        {82, "Say JACK the way you would in the game (10 of 10)."},
        {90, "Say JACK once more, then wait."}, {97, "Say JARITH (it should NOT count)."},
        {104, "Now other words, NOT the name. Say: BACK."}, {110, "Say: CHECK."}, {116, "Say: HELLO."},
        {122, "Say: JACKET."}, {128, "Say: WHAT WAS THAT."}, {134, "Say: LISA."},
        {140, "Talk normally about anything for 15 seconds, without saying the name."},
        {155, "Stay quiet until the end."},
    };
    if (!SDL_Init(SDL_INIT_AUDIO)) return 1;
    pt::VoiceRecognizer recognizer;
    if (!recognizer.Init(pt::ResourceDir("voice", PT_VOICE_MODEL_DIR), "jack")) return 1;
    pt::Microphone microphone;
    if (!microphone.Open(pt::VoiceRecognizer::kSampleRate, device)) {
        std::printf("No microphone could be opened (see pt.log).\n");
        return 1;
    }
    std::printf("Listening for %.0f s. Follow the lines below.\n\n", seconds);
    const uint64_t start = SDL_GetTicks();
    size_t next = 0;
    std::vector<int16_t> samples;
    float loudest = -100.0f;
    uint32_t heard = 0;
    for (;;) {
        const float now = static_cast<float>(SDL_GetTicks() - start) / 1000.0f;
        if (now >= seconds) break;
        while (next < std::size(kPrompts) && kPrompts[next].at <= now) {
            std::printf("[%5.1f s] >>> %s\n", now, kPrompts[next].text);
            pt::LogInfo("voice listen: prompt at {:.1f} s: {}", now, kPrompts[next].text);
            ++next;
        }
        microphone.Read(samples);
        for (size_t at = 0; at + 160 <= samples.size(); at += 160) {
            double energy = 0.0;
            for (size_t i = at; i < at + 160; ++i) energy += static_cast<double>(samples[i]) * samples[i];
            loudest = std::max(loudest, static_cast<float>(10.0 * std::log10(std::max(energy / 160.0, 1.0) / (32768.0 * 32768.0))));
        }
        recognizer.Feed(samples);
        for (const pt::VoiceRecognizer::Result& r : recognizer.TakeResults()) {
            ++heard;
            std::printf("[%5.1f s]     heard \"%s\" (%.2f s, peak %.0f dBFS)%s\n", now, r.text.c_str(), r.seconds, loudest,
                        r.detected ? "  <- the word" : "");
            pt::LogInfo("voice listen: at {:.1f} s heard '{}', p(jack) {:.3f}, {:.2f} s, peak {:.0f} dBFS, decoded in {:.0f} ms{}", now,
                        r.text, r.jack_probability, r.seconds, loudest, r.decode_ms, r.detected ? ": the word" : "");
            loudest = -100.0f;
        }
        std::fflush(stdout);
        SDL_Delay(20);
    }
    microphone.Close();
    recognizer.Finish();
    std::printf("\nDone: %u utterances. pt.log and the segments are in this folder; send them back.\n", heard);
    SDL_Quit();
    return 0;
}

pt::Camera BlendCameras(const pt::Camera& from, const pt::Camera& to, float t, bool* cut = nullptr) {
    constexpr float kPi = 3.14159265f;
    if (cut) {
        *cut = true;
    }
    if (glm::length(to.position - from.position) > 1.0f) {
        return to;
    }
    float yaw_delta = std::fmod(to.yaw - from.yaw + kPi, 2.0f * kPi);
    if (yaw_delta < 0.0f) {
        yaw_delta += 2.0f * kPi;
    }
    yaw_delta -= kPi;
    if (std::abs(yaw_delta) > 0.5f || std::abs(to.pitch - from.pitch) > 0.5f) {
        return to;
    }
    if (cut) {
        *cut = false;
    }
    pt::Camera camera = to;
    camera.position = glm::mix(from.position, to.position, t);
    camera.yaw = to.yaw - yaw_delta * (1.0f - t);
    camera.pitch = glm::mix(from.pitch, to.pitch, t);
    camera.fov_y = glm::mix(from.fov_y, to.fov_y, t);
    return camera;
}

struct TickState {
    std::vector<pt::DrawItem> items;
    std::vector<glm::mat4> skins;
    std::unordered_map<uint64_t, size_t> index;
    pt::game::TickBlend lights;

    void Capture(pt::game::Game& game, pt::game::VfxScene& vfx) {
        lights.handy_aim = game.HandyAim();
        lights.demo_lights = game.Demos().Lights();
        vfx.SnapshotWorlds();
        items.clear();
        index.clear();
        game.CollectDraws(items);
        size_t total = 0;
        for (const pt::DrawItem& item : items) {
            total += item.skin.size();
        }
        skins.clear();
        skins.reserve(total);
        pt::DrawPairKeys pair_keys;
        for (size_t i = 0; i < items.size(); ++i) {
            pt::DrawItem& item = items[i];
            if (!item.skin.empty()) {
                const size_t base = skins.size();
                skins.insert(skins.end(), item.skin.begin(), item.skin.end());
                item.skin = std::span<const glm::mat4>(skins.data() + base, item.skin.size());
            }
            if (item.mesh) {
                index[pair_keys.Next(item)] = i;
            }
        }
    }
};

void BlendDraws(const TickState& from, std::vector<pt::DrawItem>& items, float t, std::vector<glm::mat4>& storage) {
    storage.clear();
    if (from.index.empty() || t >= 1.0f) {
        return;
    }
    size_t total = 0;
    for (const pt::DrawItem& item : items) {
        total += item.skin.size();
    }
    storage.reserve(total);
    pt::DrawPairKeys pair_keys;
    for (pt::DrawItem& item : items) {
        if (!item.mesh) {
            continue;
        }
        const auto it = from.index.find(pair_keys.Next(item));
        if (it == from.index.end()) {
            continue;
        }
        const pt::DrawItem& previous = from.items[it->second];
        if (glm::distance(glm::vec3(previous.transform[3]), glm::vec3(item.transform[3])) > 1.0f) {
            continue;
        }
        const glm::mat4 current_world = item.transform;
        const bool root_moved = previous.transform != current_world;
        if (root_moved) {
            item.transform = pt::BlendTransform(previous.transform, item.transform, t);
        }
        if (!item.skin.empty() && previous.skin.size() == item.skin.size() &&
            std::memcmp(previous.skin.data(), item.skin.data(), item.skin.size() * sizeof(glm::mat4)) != 0) {
            const size_t base = storage.size();
            const bool world_blend = root_moved && std::abs(glm::determinant(item.transform)) > 1e-8f;
            const glm::mat4 inverse_world = world_blend ? glm::inverse(item.transform) : glm::mat4(1.0f);
            for (size_t i = 0; i < item.skin.size(); ++i) {
                storage.push_back(world_blend
                    ? pt::BlendSkinTransform(previous.transform, current_world, inverse_world, previous.skin[i], item.skin[i], t)
                    : previous.skin[i] + (item.skin[i] - previous.skin[i]) * t);
            }
            item.skin = std::span<const glm::mat4>(storage.data() + base, item.skin.size());
        }
    }
}

void LogRenderTrace(uint64_t frame, int ticks, float t, const pt::Camera& camera, pt::game::Game& game, const std::vector<pt::DrawItem>& items) {
    std::string heads;
    for (const pt::game::PlayingDemo& demo : game.Demos().Playing()) {
        heads += std::format(" {}@{:.3f}", demo.demo_id, demo.frame);
        for (const pt::game::DemoModel& m : demo.models) {
            const int head = m.skeleton && m.drawn && m.visible && m.mesh ? m.skeleton->FindName("SKL_004_HEAD") : -1;
            if (head < 0 || static_cast<size_t>(head) >= m.bone_world.size()) {
                continue;
            }
            const size_t h = static_cast<size_t>(head);
            const glm::vec3 tick = glm::vec3(m.world * m.bone_world[h][3]);
            glm::vec3 drawn = tick;
            for (const pt::DrawItem& item : items) {
                if (item.mesh == m.mesh && h < item.skin.size() && h < m.skeleton->bind_world.size()) {
                    drawn = glm::vec3(item.transform * item.skin[h] * glm::vec4(m.skeleton->bind_world[h], 1.0f));
                    break;
                }
            }
            heads += std::format(" {}.head drawn ({:.4f} {:.4f} {:.4f}) tick ({:.4f} {:.4f} {:.4f})", m.name, drawn.x, drawn.y, drawn.z, tick.x,
                                 tick.y, tick.z);
        }
    }
    pt::LogInfo("render trace {}: ticks {} t {:.4f} camera ({:.4f} {:.4f} {:.4f}) yaw {:.4f} pitch {:.4f} roll {:.4f} vfov {:.4f}{}", frame, ticks, t,
                camera.position.x, camera.position.y, camera.position.z, glm::degrees(camera.yaw), glm::degrees(camera.pitch),
                glm::degrees(camera.roll), glm::degrees(camera.fov_y), heads);
}

void LogLightTrace(uint64_t frame, const pt::Camera& camera, const pt::SceneLighting& lighting, pt::game::VfxScene& vfx, float blend) {
    std::string text;
    for (const pt::SceneLight& l : lighting.lights) {
        const bool moving = l.name.empty() || l.name == "PlayerHandyLight" || l.name.starts_with("gc_");
        if (!moving || glm::distance(l.position, camera.position) > 20.0f) {
            continue;
        }
        text += std::format(" light {} ({:.4f} {:.4f} {:.4f}) dir ({:.4f} {:.4f} {:.4f})", l.name.empty() ? std::format("{:x}", l.id) : l.name,
                            l.position.x, l.position.y, l.position.z, l.direction.x, l.direction.y, l.direction.z);
    }
    vfx.System().ForEachMoving(blend, [&](const pt::vfx::InstanceKey& key, const pt::vfx::EffectDef& def, const glm::mat4& world) {
        const glm::vec3 p(world[3]);
        if (glm::distance(p, camera.position) <= 20.0f) {
            text += std::format(" effect {:x}:{:x} {} ({:.4f} {:.4f} {:.4f})", key.owner, key.id, def.name, p.x, p.y, p.z);
        }
    });
    pt::LogInfo("render lights {}:{}", frame, text);
}

void UpscaleToApp(const pt::UpscaleSettings& u, pt::AppSettings& s);

std::filesystem::path StartStreamline(App& app) {
    const char* force = std::getenv("PT_STREAMLINE");
    const bool forced = force && std::atoi(force) != 0;
    if (!forced && !(app.window && app.scene.upscale.frame_generation == pt::FrameGenKind::Dlss)) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path dir = forced ? std::filesystem::current_path(ec) : app.settings_path.parent_path();
    if (dir.empty()) {
        if (char* pref = SDL_GetPrefPath("pt-port", "pt")) {
            dir = pref;
            SDL_free(pref);
        }
    }
    /* DLSS-G can take the process down inside its first frames with nothing logged; a marker left over from such a start turns it off next time. */
    const std::filesystem::path marker = dir / "streamline-starting.txt";
    if (!forced && std::filesystem::exists(marker, ec)) {
        pt::LogWarn("streamline: last DLSS-G start died in its first frames, frame generation off");
        std::filesystem::remove(marker, ec);
        pt::UpscaleHost::Get().SetDlssFrameGenFailed(true);
        app.scene.upscale.frame_generation = pt::FrameGenKind::Off;
        UpscaleToApp(app.scene.upscale, app.settings);
        if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
        return {};
    }
    if (!forced) {
        std::ofstream(marker) << "pt.exe is starting NVIDIA Streamline (DLSS Frame Generation). If this file stays, the next start leaves it off.\n";
    }
    std::string reason;
    if (!pt::streamline::Start(dir / "streamline", reason)) {
        pt::LogWarn("streamline: not loaded: {}", reason);
        std::filesystem::remove(marker, ec);
        return {};
    }
    app.renderer.Context().loader = pt::streamline::InstanceProcAddr();
    return forced ? std::filesystem::path() : marker;
}

pt::UpscaleSettings UpscaleFromApp(const pt::AppSettings& s) {
    pt::UpscaleSettings u;
    if (!pt::ParseUpscaler(s.upscaling.upscaler, u.kind) || u.kind == pt::UpscalerKind::Spatial) {
        u.kind = pt::UpscalerKind::Off;
    }
    if (!pt::ParseUpscaleQuality(s.upscaling.quality, u.quality)) {
        u.quality = pt::UpscaleQuality::Quality;
    }
    u.scale = s.upscaling.scale;
    u.sharpness = s.upscaling.sharpness;
    if (!pt::ParseDlssModel(s.upscaling.dlss_model, u.dlss_model)) {
        u.dlss_model = pt::DlssModel::Auto;
    }
    if (!pt::ParseFrameGen(s.upscaling.frame_generation, u.frame_generation)) {
        u.frame_generation = pt::FrameGenKind::Off;
    }
    return u;
}

pt::RayTracingSettings RayTracingFromApp(const pt::AppSettings& s) {
    int mode = s.ray_tracing.shadows;
    if (const char* env = std::getenv("PT_RT_SHADOWS")) {
        mode = std::atoi(env);
    }
    pt::RayTracingSettings r;
    r.shadows = mode > 0;
    r.soft_shadows = mode > 1;
    r.reflections = s.ray_tracing.reflections;
    if (const char* env = std::getenv("PT_RT_REFLECTIONS")) {
        r.reflections = std::atoi(env) != 0;
    }
    r.ambient_occlusion = s.ray_tracing.ambient_occlusion;
    if (const char* env = std::getenv("PT_RT_AO")) {
        r.ambient_occlusion = std::atoi(env) != 0;
    }
    r.contact_shadows = s.ray_tracing.contact_shadows;
    if (const char* env = std::getenv("PT_RT_CONTACT")) {
        r.contact_shadows = std::atoi(env) != 0;
    }
    return r;
}

void ApplyGraphicsSettings(App& app) {
    const auto& g=app.settings.graphics;
    app.scene.graphics=g;
    app.scene.raytracing=RayTracingFromApp(app.settings);
    app.scene.toggles.shadows=g.shadow_quality>0 || app.scene.raytracing.shadows || app.scene.raytracing.contact_shadows;
    app.scene.toggles.occlusion=g.ambient_occlusion;
    app.scene.toggles.local_reflections=g.reflections || app.scene.raytracing.reflections;
    app.scene.toggles.bloom=g.bloom;
    app.scene.toggles.depth_of_field=g.depth_of_field;
    app.scene.toggles.motion_blur=g.motion_blur;
    app.scene.toggles.film_grain=g.film_grain>0;
    app.scene.toggles.distortion=g.lens_distortion;
    pt::vfx::SetFlareGhostScale(g.lens_ghosts?0.75f:0.0f);
}

int AnisotropyFromApp(const pt::AppSettings& s) {
    if (const char* env = std::getenv("PT_ANISOTROPY")) {
        return std::atoi(env);
    }
    return s.graphics.anisotropy;
}

void UpscaleToApp(const pt::UpscaleSettings& u, pt::AppSettings& s) {
    s.upscaling.upscaler = pt::UpscalerKey(u.kind == pt::UpscalerKind::Spatial ? pt::UpscalerKind::Off : u.kind);
    s.upscaling.quality = pt::UpscaleQualityKey(u.quality);
    s.upscaling.scale = u.scale;
    s.upscaling.sharpness = u.sharpness;
    s.upscaling.dlss_model = pt::DlssModelKey(u.dlss_model);
    s.upscaling.frame_generation = pt::FrameGenKey(u.frame_generation);
}

bool DrawUpscaleSettings(App& app) {
    pt::UpscaleSettings& u = app.scene.upscale;
    bool changed = false;
    ImGui::SeparatorText("Upscaling");
    const pt::UpscalerKind kinds[] = {pt::UpscalerKind::Off, pt::UpscalerKind::Fsr, pt::UpscalerKind::Fsr4, pt::UpscalerKind::Dlss,
                                      pt::UpscalerKind::Xess};
    if (ImGui::BeginCombo("Upscaler", pt::UpscalerName(u.kind))) {
        for (pt::UpscalerKind kind : kinds) {
            std::string reason;
            const bool available = kind == pt::UpscalerKind::Off || app.scene.UpscalerAvailable(kind, reason);
            ImGui::BeginDisabled(!available);
            if (ImGui::Selectable(pt::UpscalerName(kind), u.kind == kind)) {
                u.kind = kind;
                changed = true;
            }
            ImGui::EndDisabled();
            if (!available) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", reason.c_str());
            }
        }
        ImGui::EndCombo();
    }
    const bool off = u.kind == pt::UpscalerKind::Off;
    ImGui::BeginDisabled(off);
    if (ImGui::BeginCombo("Quality", pt::UpscaleQualityName(u.quality))) {
        for (int i = 0; i < static_cast<int>(pt::UpscaleQuality::Count); ++i) {
            const auto quality = static_cast<pt::UpscaleQuality>(i);
            char label[96];
            if (quality == pt::UpscaleQuality::Custom) {
                std::snprintf(label, sizeof(label), "%s", pt::UpscaleQualityName(quality));
            } else {
                std::snprintf(label, sizeof(label), "%s (%.0f%%)", pt::UpscaleQualityName(quality), 100.0f / pt::UpscaleRatio(quality, 1.0f));
            }
            if (ImGui::Selectable(label, u.quality == quality)) {
                u.quality = quality;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (u.quality == pt::UpscaleQuality::Custom) {
        changed |= ImGui::SliderFloat("Render scale", &u.scale, 0.33f, 1.0f, "%.2f");
    }
    ImGui::BeginDisabled(u.kind != pt::UpscalerKind::Fsr && u.kind != pt::UpscalerKind::Fsr4);
    changed |= ImGui::SliderFloat("Sharpness (FSR)", &u.sharpness, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    if (u.kind == pt::UpscalerKind::Dlss) {
        const char* models[] = {"Auto (NVIDIA's default per quality)", "K", "L", "M"};
        int model = static_cast<int>(u.dlss_model);
        if (ImGui::Combo("DLSS model", &model, models, IM_ARRAYSIZE(models))) {
            u.dlss_model = static_cast<pt::DlssModel>(model);
            changed = true;
        }
    }
    ImGui::EndDisabled();
    std::string fg_reason;
    pt::FrameGeneration* fg = pt::UpscaleHost::Get().FrameGen();
    bool fg_available = fg && fg->Available(fg_reason);
    if (!fg) {
        fg_reason = "not built into this executable";
    } else if (fg_available && (off || u.kind == pt::UpscalerKind::Spatial)) {
        fg_available = false;
        fg_reason = "needs an upscaler (native AA works)";
    }
    ImGui::BeginDisabled(!fg_available);
    bool fsr_fg = u.frame_generation == pt::FrameGenKind::Fsr;
    if (ImGui::Checkbox("Frame generation (FSR 3)", &fsr_fg)) {
        u.frame_generation = fsr_fg ? pt::FrameGenKind::Fsr : pt::FrameGenKind::Off;
        changed = true;
    }
    ImGui::EndDisabled();
    if (!fg_available) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", fg_reason.c_str());
    }
    const pt::DlssFrameGenSupport& dlss_fg = pt::UpscaleHost::Get().DlssFrameGen();
    const bool dlss_fg_available = dlss_fg.hardware && dlss_fg.built && !off;
    ImGui::BeginDisabled(!dlss_fg_available);
    bool dlss_fg_on = u.frame_generation == pt::FrameGenKind::Dlss;
    if (ImGui::Checkbox("DLSS Frame Generation (experimental)", &dlss_fg_on)) {
        u.frame_generation = dlss_fg_on ? pt::FrameGenKind::Dlss : pt::FrameGenKind::Off;
        changed = true;
    }
    ImGui::EndDisabled();
    if (!dlss_fg_available) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", off && dlss_fg.hardware && dlss_fg.built ? "needs an upscaler" : dlss_fg.reason.c_str());
    }
    const pt::UpscaleStats& stats = app.scene.UpscaleStatistics();
    if (off) {
        ImGui::TextDisabled("Off: the image is rendered at the display resolution as in the original.");
    } else if (stats.active) {
        ImGui::TextDisabled("Rendering %ux%u, output %ux%u, upscaler %.2f ms", stats.render.width, stats.render.height, stats.output.width,
                            stats.output.height, stats.upscale_ms);
    } else if (!stats.error.empty()) {
        ImGui::TextDisabled("Not active: %s", stats.error.c_str());
    }
    return changed;
}

void DrawSettingsWindow(App& app, pt::InputDevice& input, pt::game::GameSound& sound, pt::game::Game& game) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings (F10)")) {
        ImGui::End();
        return;
    }
    pt::AppSettings& s = app.settings;
    bool changed = false;
    static const char* kModes[] = {"Window", "Borderless fullscreen", "Exclusive fullscreen"};
    if (ImGui::Combo("Display", &s.display.fullscreen, kModes, 3)) {
        ApplyFullscreen(app);
        changed = true;
    }
    changed |= ImGui::Checkbox("Vsync (next start)", &s.display.vsync);
    changed |= ImGui::Checkbox("Pause when the window loses focus", &s.display.pause_on_focus_loss);
    changed |= ImGui::Checkbox("Mute when in background", &s.display.mute_in_background);
    if (ImGui::SliderFloat("Mouse sensitivity", &s.input.mouse_sensitivity, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        input.settings.mouse_sensitivity = pt::InputSettings{}.mouse_sensitivity * s.input.mouse_sensitivity;
        changed = true;
    }
    if (ImGui::SliderFloat("Gamepad dead zone", &s.input.gamepad_dead_zone, 0.0f, 0.5f, "%.3f")) {
        input.settings.stick_dead_zone = s.input.gamepad_dead_zone;
        changed = true;
    }
    if (ImGui::Checkbox("Vibration", &s.input.rumble)) {
        input.settings.rumble = s.input.rumble;
        changed = true;
    }
    if (ImGui::SliderFloat("Camera tilt (1 = original)", &s.camera.roll, 0.0f, 1.0f, "%.2f")) {
        game.GetPlayer().camera_roll = s.camera.roll;
        changed = true;
    }
    ImGui::TextDisabled("%zu gamepad(s); View/Share/Create toggles this window", input.GamepadCount());
    if (ImGui::SliderFloat("Volume", &s.audio.volume, 0.0f, 2.0f, "%.2f")) {
        if (sound.Ready()) {
            sound.System().SetMasterVolume(s.audio.volume);
        }
        changed = true;
    }
    if (ImGui::BeginCombo("Microphone", s.voice.device.empty() ? "System default" : s.voice.device.c_str())) {
        if (ImGui::Selectable("System default", s.voice.device.empty())) {
            s.voice.device.clear();
            changed = true;
        }
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
        for (int i = 0; devices && i < count; ++i) {
            if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
                if (ImGui::Selectable(name, s.voice.device == name)) {
                    s.voice.device = name;
                    changed = true;
                }
            }
        }
        SDL_free(devices);
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Brightness, subtitles and camera inversion are in the game's own options.");
    if (DrawUpscaleSettings(app)) {
        UpscaleToApp(app.scene.upscale, s);
        changed = true;
    }
    ImGui::Separator();
    if (game.SavesEnabled()) {
        if (ImGui::Button("Reset progress")) {
            ImGui::OpenPopup("Reset progress?");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("next start is a first boot: options, then the preface");
        if (ImGui::BeginPopupModal("Reset progress?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Delete the save? The game stops saving until the next start.");
            if (ImGui::Button("Delete")) {
                pt::LogInfo("save: progress reset from the settings window ({})", game.ResetProgress() ? "deleted" : "nothing to delete");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    } else {
        ImGui::TextDisabled("Progress reset: the next start is a first boot.");
    }
    ImGui::End();
    if (changed && !app.settings_path.empty()) {
        pt::SaveAppSettings(app.settings_path, s);
    }
}

pt::InputState FreecamInput(const pt::InputState& input) {
    pt::InputState out;
    out.prompts = input.prompts;
    out.from_gamepad = input.from_gamepad;
    return out;
}

void ApplyPhotoLens(const pt::game::PhotoSettings& photo, const pt::game::Game& game, const pt::Camera& camera, pt::SceneLighting& lighting) {
    if (photo.depth_of_field) {
        float focus = photo.FocusDistance();
        if (focus <= 0.0f) {
            const glm::vec3 origin = camera.position;
            const glm::vec3 forward = camera.Forward();
            float nearest = 30.0f;
            const glm::vec3 eye = game.GetPlayer().Eye();
            const std::pair<glm::vec3, float> spheres[] = {{eye, 0.18f}, {eye - glm::vec3(0.0f, 0.55f, 0.0f), 0.3f},
                                                          {eye - glm::vec3(0.0f, 1.1f, 0.0f), 0.3f}};
            for (const auto& [center, radius] : spheres) {
                const glm::vec3 to = center - origin;
                const float along = glm::dot(to, forward);
                const float miss = glm::dot(to, to) - along * along;
                if (along > 0.0f && miss <= radius * radius) nearest = std::min(nearest, along - std::sqrt(radius * radius - miss));
            }
            pt::RayHit hit;
            if (game.Collision().Raycast(origin, forward, nearest, hit)) nearest = std::min(nearest, hit.distance);
            if (nearest < 30.0f) focus = std::max(nearest, 0.1f);
        }
        if (focus > 0.0f) lighting.screen.focus_distance = focus;
        const float aperture = photo.Aperture();
        if (aperture > 0.0f) {
            lighting.screen.aperture = aperture;
        } else if (!(lighting.screen.aperture > 0.0f && lighting.screen.aperture < 100.0f)) {
            lighting.screen.aperture = 4.0f;
        }
        lighting.screen.depth_of_field = true;
    }
    lighting.exposure.compensation += photo.ExposureEv();
    if (!photo.flashlight) {
        std::erase_if(lighting.lights, [](const pt::SceneLight& light) {
            return light.id == pt::kHandyLightId || light.id == pt::kMirrorLightId ||
                   (light.id >= pt::kHandyReflectionId && light.id < pt::kHandyReflectionId + 4);
        });
    }
}

struct LoopPreviewPose {
    std::string_view lines;
};
constexpr int kLoopPreviewVersion = 5;
constexpr uint32_t kLoopPreviewSeed = 2014;
constexpr int kLoopPreviewBugScreen = 1;
constexpr int kLoopPreviewF160Roll = 1;
constexpr std::array<LoopPreviewPose, 18> kLoopPreviewPoses{{
    {"sstep 15|sfree|sfade|sat f000 1|swait 30|sphoto 2|swait 2|sphotoset 50 0 0 0 0 3 1|swait 2|sphotocam -0.5 0.1 -1.6 -0.3 0 3|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|gfile -6.8 -1.4 7.5 600|fent shsb_radi001_0000|swait 30|sphoto 2|swait 2|"
     "sphotoset 40 0 0 0 0 2 0|swait 2|sphotofile -7.9 0.05 7.35 -8.79 -0.35 6.40|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|gfile -3.5 -1.4 7.5 600|ffile -11 -0.2 7.5|swait 60|sphoto 2|swait 2|"
     "sphotoset 45 0 0 0 0 2 1|swait 2|sphotocam 0.45 0.1 -1.3 -0.25 0 4|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|gfile -3.6 -1.4 7.0 600|ffile -3.6 -0.3 6.0|swait 30|sphoto 2|swait 2|"
     "sphotoset 55 0 0 0 0 2 1|swait 2|sphotofile -1.5 0.1 7.9 -3.6 -0.4 6.2|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|gfile -4.4 -1.4 7.5 600|swait 10|sfree|gfile -10.2 -1.4 7.9 600|swait 10|sfree|"
     "gfile -2.35 -1.4 7.15 600|fdir 6_Locator_trap_check_dir|swait 60|srate 1|szoom 1|sdframe gc_p01_021 57 900|sphoto 2|swait 2|"
     "sphotoset 40 0 0 0 0 2 0|swait 2|sphotocam 0.15 -0.05 -0.5 -0.1 0.75 1.0|swait 3"},
    {"gfile 0 -1.4 5.4 600|swait 20|sphoto 2|swait 2|sphotoset 30 0 0 0 0 2 0|swait 2|sphotofile -1.0 0.15 7.4 -8.49 -0.3 7.75|swait 3"},
    {"gfile 0 -1.4 6.3 600|swait 10|sfree|sdemo gc_p01_110|gfile -1.0 -1.4 7.4 600|gfile -3.2 -1.4 7.4 600|gfile -3.3 -1.4 5.7 600|"
     "srate 1|fent shsb_lght005_0000|sdframe gc_p00_030 260 900|sphoto 2|swait 2|sphotoset 50 0 0 0 0 4 1|swait 2|"
     "sphotocam 0.7 0.0 0.6 -0.1 0 0.3|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|gfile -8.0 -1.4 7.5 600|gfile -4.5 -1.4 7.5 600|swait 90|slisa 60|sphoto 2|swait 2|"
     "sphotoset 40 0 0 0 0 2 1|swait 2|sphotocam 0.3 0.05 1.5 -0.25 -0.3 -3|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|gfile -7.9 -1.4 7.5 600|fent shsb_hous001_pc1a_0000|swait 10|sphoto 2|swait 2|"
     "sphotoset 35 0 0 0 0 1 0|swait 2|sphotofile -7.55 0.0 7.05 -7.95 -0.25 6.05|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|srate 1|gfile -4.6 -1.4 7.5 600|sdframe gc_p04_120 133 900|sphoto 2|swait 2|"
     "sphotoset 50 0 0 0 0 3 0|swait 2|sphotofile -6.6 -0.6 7.5 -7.9 -0.6 8.8|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|gfile -6.3 -1.4 7.5 600|ffile -8.1 1.6 7.9|swait 60|sphoto 2|swait 2|"
     "sphotoset 40 0 0 0 0 2 0|swait 2|sphotofile -6.6 -0.6 7.4 -8.1 1.6 7.9|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile 0 -1.4 7.3 600|fent shsb_labl001_hhhh001_0000|swait 60|sphoto 2|swait 2|sphotoset 30 0 0 0 0 2 0|"
     "swait 2|sphotofile 0.45 -0.05 7.5 0.97 -0.1 6.3|swait 3"},
    {"gfile 0 -1.4 6.5 600|gfile -1.0 -1.4 7.5 600|ffile -8 0.5 7.5|swait 30|sphoto 2|swait 2|sphotoset 50 0 0 0 0 2 0|swait 2|"
     "sphotofile -1.2 -0.6 7.5 -6 1.0 7.5|swait 3"},
    {"sanchor next|gfile 0 0 1.5 600|gfile 0 0 13.6 600|gfile -9.8 0 13.6 900|gfile -9.8 0 1.3 900|gfile -29.2 0 1.3 900|"
     "gfile -29.2 0 -1.0 600|sanchor nextB|gfile -13.5 0 -30.2 900|gfile -7.1 0 -30.2 900|gfile -7.1 0 -34.5 900|"
     "fent shsb_labl001_holl001_0000|swait 20|sphoto 2|swait 2|sphotoset 50 0 0 0 0 3 1|swait 2|sphotocam -0.9 0.05 0.45 -0.05 0 0.35|"
     "swait 3"},
    {"sdframe gc_p02_080 1700 3600|srate 1|sdframe gc_p02_060 5 900"},
    {"gfile 0 -1.4 6.6 600|ffile -0.2 -0.5 8.3|swait 60|sphoto 2|swait 2|sphotoset 50 0 0 0 0 4 1|swait 2|"
     "sphotocam -0.45 0.2 -1.3 -0.2 0 1.5|swait 3"},
    {"srate 1|sstep 28|sdframe gc_p06_010_final 5760|sfade 600|srate 20"},
    {"sstep 31|swait 150|sfade 600|sphoto 2|swait 2|sphotoset 50 0 0 0 0 0 1|swait 2|sphotocam 0.0 0.05 -2.2 -0.1 0 4|swait 3"},
}};

std::string LoopPreviewRoute(const std::filesystem::path& dir, const std::vector<int>& loops) {
    std::string route = "20 sstep 15\n";
    for (int i : loops) {
        const auto& loop = pt::game::kBrowseLoops[i];
        const bool hallway = i > 0 && i < pt::game::kBrowseEnding;
        route += std::format("20 sloop {}\n", i);
        if (hallway) {
            route += "20 sstep 20\n20 sstep 15\n20 swait 5\n20 sfree\n20 goto 0 0 12.5 900\n20 swait 10\n20 sfree\n20 sbrowsed 900\n"
                     "20 sstep 15\n20 swait 5\n20 sfree\n20 sanchor\n";
        } else {
            route += "20 sbrowsed\n";
        }
        std::string_view lines = kLoopPreviewPoses[i].lines;
        while (!lines.empty()) {
            const size_t bar = lines.find('|');
            route += std::format("20 {}\n", lines.substr(0, bar));
            lines = bar == std::string_view::npos ? std::string_view() : lines.substr(bar + 1);
        }
        if (hallway) {
            route += std::format("20 sfade 120\n20 sat {} {}\n", loop.floor, loop.pass);
        }
        route += std::format("20 sshot {}\n20 swait 2\n20 sphoto 0\n20 szoom 0\n20 srate 20\n20 swait 2\n20 scamera off\n",
                             (dir / std::format("loop-{}.png", i)).generic_string());
    }
    route += "20 squit\n";
    return route;
}

void SetUpLoopPreviewRun(Options& options) {
    const std::filesystem::path dir = std::filesystem::absolute(options.make_loop_previews);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<int> all;
    for (int i = 0; i < static_cast<int>(pt::game::kBrowseLoops.size()); ++i) all.push_back(i);
    std::ofstream(dir / "capture.txt") << LoopPreviewRoute(dir, all);
    std::ofstream(dir / "version.txt") << kLoopPreviewVersion << "\n";
    pt::SaveAppSettings(dir / "preview.ini", pt::AppSettings{});
    options.headless = true;
    options.no_save = true;
    options.audio_offline = true;
    options.frames = 120000;
    options.demo_rate = 20.0f;
    options.shot_warmup = 30;
    options.shot_settle = true;
    options.width = 640;
    options.height = 360;
    options.size_set = true;
    options.street_offer = -1;
    options.seed = kLoopPreviewSeed;
    options.bug_screen = kLoopPreviewBugScreen;
    options.handy_light_roll = kLoopPreviewF160Roll;
    options.input_script = dir / "capture.txt";
    options.settings_path = dir / "preview.ini";
    if (options.log_path.empty()) options.log_path = dir / "capture.log";
}

constexpr int kMuseumPreviewVersion = 2;
constexpr float kMuseumPreviewModelEv = 0.5f;

std::string MuseumPreviewRoute(const std::filesystem::path& dir, const std::vector<const pt::game::ArchiveEntry*>& entries) {
    std::string route = "20 sstep 15\n20 swait 30\n";
    for (const pt::game::ArchiveEntry* e : entries) {
        route += std::format("20 sarchive {}\n20 sarchiveshown 3600\n20 swait {}\n20 sshot {}\n20 swait 2\n20 sarchive -\n20 sarchived\n20 swait 5\n",
                             e->id, pt::game::ArchiveThumbnailFrames(*e), (dir / std::format("{}.png", e->id)).generic_string());
    }
    route += "20 squit\n";
    return route;
}

std::vector<const pt::game::ArchiveEntry*> MuseumPreviewEntries() {
    std::vector<const pt::game::ArchiveEntry*> out;
    for (const pt::game::ArchiveEntry& e : pt::game::ArchiveEntries()) {
        if (e.media == pt::game::ArchiveMedia::Demo || e.media == pt::game::ArchiveMedia::Model) out.push_back(&e);
    }
    return out;
}

void SetUpMuseumPreviewRun(Options& options) {
    const std::filesystem::path dir = std::filesystem::absolute(options.make_museum_previews);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<const pt::game::ArchiveEntry*> missing;
    for (const pt::game::ArchiveEntry* e : MuseumPreviewEntries()) {
        if (!std::filesystem::exists(dir / std::format("{}.png", e->id), ec)) missing.push_back(e);
    }
    std::ofstream(dir / "capture.txt") << MuseumPreviewRoute(dir, missing);
    std::ofstream(dir / "version.txt") << kMuseumPreviewVersion << "\n";
    pt::SaveAppSettings(dir / "preview.ini", pt::AppSettings{});
    options.headless = true;
    options.no_save = true;
    options.audio_offline = true;
    options.frames = 200000;
    options.demo_rate = 1.0f;
    options.shot_warmup = 20;
    options.shot_settle = true;
    options.width = 480;
    options.height = 270;
    options.size_set = true;
    options.street_offer = -1;
    options.seed = kLoopPreviewSeed;
    options.release_locks = false;
    options.input_script = dir / "capture.txt";
    options.settings_path = dir / "preview.ini";
    if (options.log_path.empty()) options.log_path = dir / "capture.log";
}

std::filesystem::path PhotoPath() {
    std::filesystem::path base;
    if (const char* profile = std::getenv("USERPROFILE")) {
        base = profile;
    } else if (const char* home = std::getenv("HOME")) {
        base = home;
    }
    const std::filesystem::path dir = base / "Pictures" / "PT Photos";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    std::filesystem::path path = dir / std::format("pt_{}.png", stamp);
    for (int i = 2; std::filesystem::exists(path, ec) && i < 100; ++i) {
        path = dir / std::format("pt_{}_{}.png", stamp, i);
    }
    return path;
}

std::string UpdateNoticeKey(std::string_view version) {
    return std::format("pc_update_available{}{}{}{}", pt::game::kPcNoteArgument, version, pt::game::kPcNoteArgument, pt::update::CurrentVersion());
}

class PcSettings final : public pt::game::PcSettingsSource {
public:
    PcSettings(App& app, pt::game::Game& game, pt::InputDevice& input) : app_(app), game_(game), input_(input) {}
    ~PcSettings() override {
        if (preview_process_) SDL_DestroyProcess(preview_process_);
        if (museum_process_) SDL_DestroyProcess(museum_process_);
    }

    std::vector<pt::game::PcSettingSection> Sections() override {
        if (page_ == kArchivePage) return ArchiveSections();
        if (page_ == kArchiveListPage) return ArchiveListSections();
        if (page_ == kLoopPage) {
            pt::game::PcSettingSection loops{"pc_loop_choose", 0, {}};
            const bool generating = PreviewsGenerating();
            const auto preview_dir = PreviewDirectory();
            for (int i = 0; i < static_cast<int>(pt::game::kBrowseLoops.size()); ++i) {
                std::error_code ec;
                const bool missing = !std::filesystem::exists(preview_dir / std::format("loop-{}.png", i), ec);
                const bool unlocked = LoopUnlocked(i);
                const char* note = !unlocked                          ? "pc_note_loop_locked"
                                   : generating && missing             ? "pc_note_loop_generating"
                                   : i == 0                            ? "pc_note_loop_opening"
                                   : i == pt::game::kBrowseEnding      ? "pc_note_loop_ending"
                                   : i == pt::game::kBrowseStreet      ? "pc_note_loop_street"
                                                                       : "pc_note_loop_hallway";
                auto row = Row(kLoopFirst + i, std::string(pt::game::kBrowseLoops[i].label), {""}, 0, note);
                row.link = true;
                row.enabled = unlocked;
                loops.rows.push_back(std::move(row));
            }
            return {std::move(loops), pt::game::PcSettingSection{"pc_loop_preview", 1, {}}};
        }
        if (page_ == kStreetPage && game_.SpeedrunResultsPending()) return SpeedrunResults();
        if (page_ == kStreetPage) {
            pt::game::PcSettingSection street{"pc_street_question", 0, {}};
            auto walk = Row(kStreetWalk, "pc_street_walk", {""}, 0, "pc_note_street_walk");
            walk.link = true;
            street.rows.push_back(std::move(walk));
            auto restart = Row(kStreetRestart, "pc_street_restart", {""}, 0, "pc_note_street_restart");
            restart.link = true;
            street.rows.push_back(std::move(restart));
            return {std::move(street)};
        }
        if (page_ == kExtrasPage) {
            pt::game::PcSettingSection extras{"pc_extras", 0, {}};
            if (game_.StreetWalkActive()) {
                auto leave = Row(kStreetLeave, "pc_street_leave", {"pc_extras_start"}, 0, "pc_note_street_leave");
                leave.link = true;
                extras.rows.push_back(std::move(leave));
            }
            const bool locked = app_.options.release_locks && !game_.GameFinished();
            auto browser = Row(kLoopBrowser, "pc_loop_browser", {"pc_graphics_value"}, 0,
                               locked ? "pc_note_loop_browser_locked" : "pc_note_loop_browser_entry");
            browser.link = true;
            extras.rows.push_back(std::move(browser));
            auto archive = Row(kArchive, "pc_archive", {"pc_graphics_value"}, 0, locked ? "pc_note_archive_locked" : "pc_note_archive");
            archive.link = true;
            extras.rows.push_back(std::move(archive));
            auto freecam = Row(kFreecam, "pc_freecam", {app_.freecam_active ? "pc_extras_stop" : "pc_extras_start"}, 0, "pc_note_freecam");
            freecam.link = true;
            freecam.enabled = app_.window != nullptr && !app_.xr;
            extras.rows.push_back(std::move(freecam));
            auto photo = Row(kPhotoMode, "pc_photo_mode", {app_.photo_active ? "pc_extras_stop" : "pc_extras_start"}, 0, "pc_note_photo_mode");
            photo.link = true;
            photo.enabled = app_.window != nullptr && !app_.xr;
            extras.rows.push_back(std::move(photo));
            extras.rows.push_back(Row(kThirdPerson, "pc_third_person", OffOn(), game_.ThirdPerson() ? 1 : 0, "pc_note_third_person"));
            extras.rows.push_back(Row(kSpeedrun, "pc_speedrun", {"pc_off", "pc_speedrun_real_time", "pc_speedrun_game_time"},
                                      std::clamp(app_.settings.extras.speedrun, 0, 2), "pc_note_speedrun"));
            extras.rows.push_back(Row(kLiveSplit, "pc_livesplit", OffOn(), app_.settings.extras.livesplit ? 1 : 0,
                                      std::format("pc_note_livesplit{}{}", pt::game::kPcNoteArgument, app_.settings.extras.livesplit_port)));
            auto vr = Row(kVr, "pc_vr", {"pc_graphics_value"}, 0, "pc_note_vr");
            vr.link = true;
            extras.rows.push_back(std::move(vr));
            if (app_.options.release_locks) {
                auto lock = Row(kLoopLock, "pc_loop_lock", {"pc_extras_start"}, 0, "pc_note_loop_lock");
                lock.action = true;
                lock.enabled = game_.GameFinished() || game_.BrowseReached() != 0;
                extras.rows.push_back(std::move(lock));
            }
            return {std::move(extras)};
        }
        if (page_ == kVrPage) {
            const pt::AppSettings::Vr& v = app_.settings.vr;
            pt::game::PcSettingSection vr{"pc_vr", 0, {}};
            vr.rows.push_back(Row(kVrMode, "pc_vr_mode", OffOn(), v.enabled ? 1 : 0, "pc_note_vr_mode"));
            vr.rows.push_back(Row(kVrFlashlight, "pc_vr_flashlight", {"pc_vr_head", "pc_vr_controller"}, v.flashlight, "pc_note_vr_flashlight"));
            vr.rows.push_back(Row(kVrTurn, "pc_vr_turn", {"pc_vr_snap", "pc_vr_smooth"}, v.turn, "pc_note_vr_turn"));
            return {std::move(vr)};
        }
        if (page_ == kGraphicsPage) return GraphicsSections();
        if (page_ == kLightingPage) return LightingSections();
        if (page_ == kTexturesPage) return TextureSections();
        if (page_ == kEffectsPage) return EffectSections();
        if (page_ == kModsPage) return ModSections();
        if (page_ == kMicrophonePage) {
            pt::game::PcSettingSection test{"pc_microphone_test", 0, {}};
            const int level = std::clamp(static_cast<int>((app_.microphone_db + 60.0f) / 6.0f), 0, 10);
            test.rows.push_back(Row(kMicLevel, "pc_mic_level", {std::format("{} {:.0f} dBFS", std::string(level, '|'), app_.microphone_db)}, 0, app_.microphone_status));
            test.rows.push_back(Row(kMicHeard, "pc_mic_heard", {app_.microphone_hypothesis.empty() ? "pc_mic_no_word" : app_.microphone_hypothesis}, 0, "pc_mic_say_jack"));
            test.rows.push_back(Row(kMicMonitor, "pc_mic_monitor", OffOn(), app_.microphone_monitor ? 1 : 0, "pc_mic_monitor_note"));
            return {std::move(test)};
        }
        const pt::AppSettings& s = app_.settings;
        std::vector<pt::game::PcSettingSection> out;
        pt::game::PcSettingSection display{"pc_section_display", 0, {}};
        const int mode = std::clamp(s.display.fullscreen, 0, 2);
        display.rows.push_back(Row(kDisplayMode, "pc_display_mode", {"pc_window", "pc_borderless", "pc_fullscreen"}, mode, "pc_note_display_mode"));
        pt::game::PcSettingRow resolution = Row(kResolution, "pc_resolution", {}, 0, "pc_note_resolution");
        if (s.display.fullscreen == 0) {
            const std::vector<glm::ivec2> sizes = WindowSizes();
            for (size_t i = 0; i < sizes.size(); ++i) {
                resolution.values.push_back(std::format("{} x {}", sizes[i].x, sizes[i].y));
                if (sizes[i] == glm::ivec2(s.display.width, s.display.height)) {
                    resolution.value = static_cast<int>(i);
                }
            }
            resolution.wrap = false;
        } else {
            const glm::ivec2 desktop = DesktopSize();
            resolution.values.push_back(std::format("{} x {}", desktop.x, desktop.y));
            resolution.enabled = false;
            resolution.note = "pc_note_resolution_fullscreen";
        }
        display.rows.push_back(std::move(resolution));
        const bool dlssg_vsync = pt::streamline::Active() && pt::streamline::FrameGenNeedsVsyncOff() &&
                                 app_.scene.upscale.frame_generation == pt::FrameGenKind::Dlss;
        display.rows.push_back(Row(kVsync, "pc_vsync", OffOn(), s.display.vsync ? 1 : 0,
                                   dlssg_vsync ? "pc_note_vsync_dlssg" : s.display.vsync ? "pc_note_vsync" : "pc_note_vsync_off"));
        display.rows.push_back(Row(kFocusPause, "pc_focus_pause", OffOn(), s.display.pause_on_focus_loss ? 1 : 0, "pc_note_focus_pause"));
        display.rows.push_back(Row(kFocusMute, "pc_focus_mute", OffOn(), s.display.mute_in_background ? 1 : 0, "pc_note_focus_mute"));
        out.push_back(std::move(display));

        const pt::UpscaleSettings& u = app_.scene.upscale;
        const bool off = u.kind == pt::UpscalerKind::Off;
        pt::game::PcSettingSection upscaling{"pc_section_upscaling", 0, {}};
        const std::string upscaler_note = off ? std::string("pc_note_upscaler_off") : std::format("{}.", pt::UpscalerName(u.kind));
        pt::game::PcSettingRow upscaler = Row(kUpscaler, "pc_upscaler", {}, 0, upscaler_note);
        const std::vector<pt::UpscalerKind> kinds = Upscalers(&upscaler.value_notes);
        for (const pt::UpscalerKind kind : kinds) {
            if (kind == u.kind) {
                upscaler.value = static_cast<int>(upscaler.values.size());
            }
            upscaler.values.push_back(kind == pt::UpscalerKind::Off ? "pc_off" : ShortName(kind));
        }
        upscaling.rows.push_back(std::move(upscaler));
        pt::game::PcSettingRow quality =
            Row(kQuality, "pc_quality", {"pc_native", "pc_quality_quality", "pc_quality_balanced", "pc_quality_performance", "pc_quality_ultra"},
                std::clamp(static_cast<int>(u.quality), 0, 4), off ? "pc_note_needs_upscaler" : RenderNote());
        if (u.quality == pt::UpscaleQuality::Custom) {
            quality.values.push_back("pc_quality_custom");
            quality.value = 5;
        }
        quality.enabled = !off;
        upscaling.rows.push_back(std::move(quality));
        const int sharpness_step = std::clamp(static_cast<int>(std::lround(u.sharpness * 10.0f)), 0, 10);
        const bool fsr = u.kind == pt::UpscalerKind::Fsr || u.kind == pt::UpscalerKind::Fsr4;
        if (u.kind == pt::UpscalerKind::Dlss) {
            pt::game::PcSettingRow model = Row(kDlssModel, "pc_dlss_model", {"pc_dlss_model_auto", "K", "L", "M"},
                                               std::clamp(static_cast<int>(u.dlss_model), 0, 3), "pc_note_dlss_model");
            upscaling.rows.push_back(std::move(model));
        } else {
            pt::game::PcSettingRow sharpness = Row(kSharpness, "pc_sharpness", Numbers(0, 10), sharpness_step,
                                                   off ? "pc_note_needs_upscaler" : fsr ? "" : "pc_note_sharpness_fsr");
            sharpness.enabled = fsr;
            sharpness.wrap = false;
            upscaling.rows.push_back(std::move(sharpness));
        }
        pt::game::PcSettingRow frame_generation = Row(kFrameGeneration, "pc_frame_generation", {"pc_off", "FSR 3", "DLSS"},
                                                      static_cast<int>(u.frame_generation), "pc_note_frame_generation");
        std::string fg_reason;
        pt::FrameGeneration* fg = pt::UpscaleHost::Get().FrameGen();
        std::string fsr_note;
        if (!fg) {
            fsr_note = "pc_note_frame_generation_missing";
        } else if (!fg->Available(fg_reason)) {
            fsr_note = fg_reason.starts_with("needs an AMD Radeon RX 5000") ? "pc_note_fsr_fg_gpu" : Sentence(fg_reason);
        }
        const pt::DlssFrameGenSupport& dlss_fg = pt::UpscaleHost::Get().DlssFrameGen();
        const std::string dlss_note = dlss_fg.hardware && dlss_fg.built ? std::string() : dlss_fg.note;
        if (pt::streamline::Active() && fsr_note.empty()) {
            fsr_note = "pc_note_fsr_fg_restart";
        }
        frame_generation.value_notes = {std::string(), fsr_note, dlss_note};
        if (u.frame_generation == pt::FrameGenKind::Dlss) {
            frame_generation.note = pt::streamline::Active() ? "pc_note_frame_generation_dlss" : "pc_note_dlssg_restart";
        }
        if (off) {
            frame_generation.note = "pc_note_needs_upscaler";
            frame_generation.enabled = false;
            frame_generation.value = 0;
        }
        upscaling.rows.push_back(std::move(frame_generation));
        out.push_back(std::move(upscaling));
        pt::game::PcSettingRow graphics = Row(kGraphics, "pc_graphics", {"pc_graphics_value"}, 0, "pc_note_graphics");
        graphics.link = true;
        out.back().rows.push_back(std::move(graphics));
        if (app_.mods && !app_.mods->mods.empty()) {
            auto mods = Row(kMods, "pc_mods", {"pc_graphics_value"}, 0, "pc_note_mods");
            mods.link = true;
            out.back().rows.push_back(std::move(mods));
        }

        pt::game::PcSettingSection sound{"pc_section_sound", 1, {}};
        pt::game::PcSettingRow volume = Row(kVolume, "pc_volume", {}, std::clamp(static_cast<int>(std::lround(s.audio.volume * 10.0f)), 0, 20), "");
        for (int i = 0; i <= 20; ++i) {
            volume.values.push_back(std::format("{}%", i * 10));
        }
        volume.wrap = false;
        sound.rows.push_back(std::move(volume));
        pt::game::PcSettingRow microphone = Row(kMicrophone, "pc_microphone", {"pc_system_default"}, 0, "pc_note_voice");
        const std::vector<std::string> devices = Microphones();
        for (size_t i = 0; i < devices.size(); ++i) {
            microphone.values.push_back(devices[i].size() > 24 ? devices[i].substr(0, 22) + "..." : devices[i]);
            if (devices[i] == s.voice.device) {
                microphone.value = static_cast<int>(i) + 1;
            }
        }
        sound.rows.push_back(std::move(microphone));
        auto microphone_test = Row(kMicrophoneTest, "pc_microphone_test", {"pc_graphics_value"}, 0, "pc_mic_say_jack");
        microphone_test.link = true;
        sound.rows.push_back(std::move(microphone_test));
        out.push_back(std::move(sound));

        pt::game::PcSettingSection controls{"pc_section_controls", 1, {}};
        pt::game::PcSettingRow mouse = Row(kMouse, "pc_mouse", {}, Nearest(kMouseSteps, s.input.mouse_sensitivity), "");
        for (const float step : kMouseSteps) {
            mouse.values.push_back(step < 0.5f ? std::format("{:.2f}", step) : std::format("{:.1f}", step));
        }
        mouse.wrap = false;
        controls.rows.push_back(std::move(mouse));
        controls.rows.push_back(Row(kCameraTilt, "pc_camera_tilt", OffOn(), s.camera.roll > 0.5f ? 1 : 0, "pc_note_camera_tilt"));
        controls.rows.push_back(Row(kVibration, "pc_vibration", OffOn(), s.input.rumble ? 1 : 0, ""));
        const int dead_zone_step = Nearest(kDeadZoneSteps, s.input.gamepad_dead_zone);
        pt::game::PcSettingRow dead_zone = Row(kDeadZone, "pc_dead_zone", {}, dead_zone_step, "pc_note_dead_zone");
        for (const float step : kDeadZoneSteps) {
            dead_zone.values.push_back(std::format("{:.2f}", step));
        }
        dead_zone.wrap = false;
        controls.rows.push_back(std::move(dead_zone));
        out.push_back(std::move(controls));

        pt::game::PcSettingSection progress{"pc_section_progress", 1, {}};
        pt::game::PcSettingRow reset = Row(kReset, "pc_reset", {"pc_reset_value"}, 0, "pc_note_reset");
        reset.action = true;
        if (!game_.SavesEnabled()) {
            reset.values = {"pc_reset_done"};
            reset.enabled = false;
            reset.note = "pc_note_reset_done";
        }
        progress.rows.push_back(std::move(reset));
        auto extras = Row(kExtras, "pc_extras", {"pc_graphics_value"}, 0, "pc_note_extras_archive");
        extras.link = true;
        progress.rows.push_back(std::move(extras));
        out.push_back(std::move(progress));
        return out;
    }

    std::vector<std::string> DescriptionSamples() override {
        const int previous = page_;
        std::vector<std::string> notes;
        const int previous_section = archive_section_;
        for (int page = 0; page <= kArchiveListPage + pt::game::kArchiveSectionCount - 1; ++page) {
            page_ = std::min(page, kArchiveListPage);
            if (page >= kArchiveListPage) archive_section_ = page - kArchiveListPage;
            for (const auto& section : Sections()) for (const auto& row : section.rows) {
                if (!row.note.empty()) notes.push_back(row.note);
                for (const auto& note : row.value_notes) if (!note.empty()) notes.push_back(note);
            }
        }
        page_ = kVrPage;
        for (const auto& section : Sections()) for (const auto& row : section.rows) {
            if (!row.note.empty()) notes.push_back(row.note);
            for (const auto& note : row.value_notes) if (!note.empty()) notes.push_back(note);
        }
        page_ = previous;
        archive_section_ = previous_section;
        for (const char* note : {"pc_note_archive_playing", "pc_note_archive_locked", "pc_note_archive_lock"}) notes.push_back(note);
        for (const char* note : {"pc_note_fsr4_vulkan", "pc_note_dlss_model", "pc_note_frame_generation_dlss", "pc_note_dlssg_gpu",
                                 "pc_note_dlssg_driver", "pc_note_dlssg_hags", "pc_note_dlssg_missing", "pc_note_dlssg_failed",
                                 "pc_note_dlssg_restart", "pc_note_vsync_dlssg", "pc_note_fsr_fg_restart", "pc_note_vsync_off"}) {
            notes.push_back(note);
        }
        return notes;
    }

    void Set(int id, int value) override {
        pt::AppSettings& s = app_.settings;
        pt::UpscaleSettings& u = app_.scene.upscale;
        if (id >= kModFirst) {
            if (app_.mods && id - kModFirst < static_cast<int>(app_.mods->mods.size())) {
                s.mods[app_.mods->mods[id - kModFirst].folder] = value == 1;
                Save();
            }
            return;
        }
        switch (id) {
        case kMicMonitor:
            app_.microphone_monitor = value == 1;
            return;
        case kSpeedrun:
            s.extras.speedrun = std::clamp(value, 0, 2);
            game_.Speedrun().SetMode(s.extras.speedrun);
            Save();
            return;
        case kLiveSplit:
            s.extras.livesplit = value == 1;
            app_.livesplit.Configure(s.extras.livesplit, s.extras.livesplit_host, s.extras.livesplit_port);
            Save();
            return;
        case kDisplayMode:
            s.display.fullscreen = std::clamp(value, 0, 2);
            ApplyFullscreen(app_);
            break;
        case kResolution: {
            const std::vector<glm::ivec2> sizes = WindowSizes();
            if (value < 0 || value >= static_cast<int>(sizes.size())) {
                return;
            }
            s.display.width = sizes[value].x;
            s.display.height = sizes[value].y;
            if (app_.window && s.display.fullscreen == 0) {
                SDL_SetWindowSize(app_.window, s.display.width, s.display.height);
            }
            break;
        }
        case kVrMode:
            s.vr.enabled = value == 1;
            pt::LogInfo("vr: {} at the next start (PC settings)", s.vr.enabled ? "on" : "off");
            break;
        case kVrFlashlight:
            s.vr.flashlight = std::clamp(value, 0, 1);
            break;
        case kVrTurn:
            s.vr.turn = std::clamp(value, 0, 1);
            break;
        case kVsync:
            s.display.vsync = value == 1;
            app_.renderer.SetVsync(s.display.vsync);
            break;
        case kFocusPause:
            s.display.pause_on_focus_loss = value == 1;
            break;
        case kFocusMute:
            s.display.mute_in_background = value == 1;
            break;
        case kUpscaler: {
            const std::vector<pt::UpscalerKind> kinds = Upscalers(nullptr);
            if (value < 0 || value >= static_cast<int>(kinds.size())) {
                return;
            }
            u.kind = kinds[value];
            UpscaleToApp(u, s);
            break;
        }
        case kQuality:
            u.quality = static_cast<pt::UpscaleQuality>(std::clamp(value, 0, static_cast<int>(pt::UpscaleQuality::Custom)));
            UpscaleToApp(u, s);
            break;
        case kSharpness:
            u.sharpness = static_cast<float>(std::clamp(value, 0, 10)) * 0.1f;
            UpscaleToApp(u, s);
            break;
        case kDlssModel:
            u.dlss_model = static_cast<pt::DlssModel>(std::clamp(value, 0, static_cast<int>(pt::DlssModel::Count) - 1));
            UpscaleToApp(u, s);
            break;
        case kFrameGeneration:
            u.frame_generation = static_cast<pt::FrameGenKind>(std::clamp(value, 0, static_cast<int>(pt::FrameGenKind::Count) - 1));
            UpscaleToApp(u, s);
            break;
        case kVolume:
            s.audio.volume = static_cast<float>(std::clamp(value, 0, 20)) * 0.1f;
            break;
        case kMicrophone: {
            const std::vector<std::string> devices = Microphones();
            s.voice.device = value > 0 && value <= static_cast<int>(devices.size()) ? devices[value - 1] : std::string();
            break;
        }
        case kMouse:
            s.input.mouse_sensitivity = kMouseSteps[std::clamp(value, 0, static_cast<int>(std::size(kMouseSteps)) - 1)];
            input_.settings.mouse_sensitivity = pt::InputSettings{}.mouse_sensitivity * s.input.mouse_sensitivity;
            break;
        case kCameraTilt:
            s.camera.roll = value == 1 ? 1.0f : 0.0f;
            game_.GetPlayer().camera_roll = s.camera.roll;
            break;
        case kThirdPerson:
            s.camera.third_person = value == 1;
            game_.SetThirdPerson(s.camera.third_person);
            break;
        case kVibration:
            s.input.rumble = value == 1;
            input_.settings.rumble = s.input.rumble;
            break;
        case kDeadZone:
            s.input.gamepad_dead_zone = kDeadZoneSteps[std::clamp(value, 0, static_cast<int>(std::size(kDeadZoneSteps)) - 1)];
            input_.settings.stick_dead_zone = s.input.gamepad_dead_zone;
            break;
        case kGraphicsPreset: {
            std::string reason;
            pt::ApplyGraphicsPreset(s,static_cast<pt::GraphicsPreset>(std::clamp(value,0,4)),app_.scene.RayTracingSupported(reason));
            app_.textures.SetAnisotropy(s.graphics.anisotropy);
            RequestEnhancedTextures(app_,s.graphics.enhanced_textures);
            break;
        }
        case kShadowQuality:
            value=std::clamp(value,0,5);
            s.graphics.shadow_quality=value<=3?value:3;
            s.ray_tracing.shadows=value<=3?0:value-3;
            break;
        case kRasterAo:
            value=std::clamp(value,0,2);
            s.graphics.ambient_occlusion=value>=1;
            s.ray_tracing.ambient_occlusion=value==2;
            break;
        case kRasterReflections:
            value=std::clamp(value,0,2);
            s.graphics.reflections=value>=1;
            s.ray_tracing.reflections=value==2;
            break;
        case kRayQuality: s.graphics.ray_quality=std::clamp(value,0,2);break;
        case kTextureDetail: s.graphics.texture_detail=std::clamp(value,0,2);break;
        case kBloom: s.graphics.bloom=value==1;break;
        case kLensGhosts: s.graphics.lens_ghosts=value==1;break;
        case kDepthOfField: s.graphics.depth_of_field=value==1;break;
        case kMotionBlur: s.graphics.motion_blur=value==1;break;
        case kFilmGrain: s.graphics.film_grain=std::clamp(value,0,4)*.25f;break;
        case kLensDistortion: s.graphics.lens_distortion=value==1;break;
        case kClarity: s.graphics.clarity=std::clamp(value,0,10)*.1f;break;
        case kLetterbox: s.display.letterbox=std::clamp(value,0,2);break;
        case kRtShadows:
            s.ray_tracing.shadows = std::clamp(value, 0, 2);
            app_.scene.raytracing.shadows = s.ray_tracing.shadows > 0;
            app_.scene.raytracing.soft_shadows = s.ray_tracing.shadows > 1;
            break;
        case kRtContact:
            s.ray_tracing.contact_shadows = value == 1;
            app_.scene.raytracing.contact_shadows = s.ray_tracing.contact_shadows;
            break;
        case kRtAo:
            s.ray_tracing.ambient_occlusion = value == 1;
            app_.scene.raytracing.ambient_occlusion = s.ray_tracing.ambient_occlusion;
            break;
        case kRtReflections:
            s.ray_tracing.reflections = value == 1;
            app_.scene.raytracing.reflections = s.ray_tracing.reflections;
            break;
        case kAnisotropy:
            s.graphics.anisotropy = kAnisotropySteps[std::clamp(value, 0, static_cast<int>(std::size(kAnisotropySteps)) - 1)];
            app_.textures.SetAnisotropy(s.graphics.anisotropy);
            break;
        case kEnhancedTextures:
            RequestEnhancedTextures(app_, value == 1);
            break;
        default:
            return;
        }
        ApplyGraphicsSettings(app_);
        Save();
    }

    std::string_view Title() const override {
        switch(page_) {
        case kLoopPage:return "pc_loop_browser";
        case kStreetPage:return game_.SpeedrunResultsPending() ? "pc_speedrun_results" : "pc_street_title";
        case kGraphicsPage:return "pc_graphics";
        case kLightingPage:return "pc_graphics_lighting";
        case kTexturesPage:return "pc_section_textures";
        case kEffectsPage:return "pc_graphics_effects";
        case kMicrophonePage:return "pc_microphone_test";
        case kModsPage:return "pc_mods";
        case kExtrasPage:return "pc_extras";
        case kArchivePage:return "pc_archive";
        case kArchiveListPage:return "pc_archive";
        case kVrPage:return "pc_vr";
        default:return "pc_title";
        }
    }

    bool IsLoopBrowser() const override { return page_ == kLoopPage; }
    bool IsBrowser() const override { return page_ == kLoopPage; }
    pt::game::PcGallery Gallery() const override {
        return page_ == kArchivePage ? pt::game::PcGallery::Halls : page_ == kArchiveListPage ? pt::game::PcGallery::Wall : pt::game::PcGallery::None;
    }

    pt::game::PcPanel EntryPicture(const pt::game::ArchiveEntry& entry) const {
        pt::game::PcPanel panel;
        using M = pt::game::ArchiveMedia;
        switch (entry.media) {
        case M::Image:
        case M::String:
            panel.texture = std::string(entry.asset);
            panel.string = entry.media == M::String;
            break;
        case M::Photo:
            panel.photo = entry.asset.empty() ? std::string("complete") : std::string(entry.asset);
            break;
        case M::Demo:
        case M::Model:
            panel.thumbnail = (MuseumPreviewDirectory() / std::format("{}.png", entry.id)).string();
            panel.lift = entry.media == M::Model ? 10.0f : 6.0f;
            break;
        case M::Sound:
        case M::Dialogue:
            if (const pt::game::ArchiveEntry* picture = pt::game::FindArchiveEntry(pt::game::ArchivePictureOf(entry))) return EntryPicture(*picture);
            break;
        }
        return panel;
    }

    pt::game::PcPanel Panel(int id) const override {
        pt::game::PcPanel panel;
        if (page_ == kLoopPage) {
            panel.file = PreviewFile(id);
            return panel;
        }
        if (page_ == kArchivePage && id >= kArchiveSectionFirst && id < kArchiveSectionFirst + pt::game::kArchiveSectionCount) {
            const auto section = static_cast<pt::game::ArchiveSection>(id - kArchiveSectionFirst);
            if (section == pt::game::ArchiveSection::Voices) return panel;
            if (const pt::game::ArchiveEntry* cover = pt::game::FindArchiveEntry(pt::game::ArchiveSectionCover(section)); cover && game_.ArchiveUnlocked(*cover)) {
                pt::game::PcPanel picture = EntryPicture(*cover);
                if (picture.HasPicture()) return picture;
            }
            for (const pt::game::ArchiveEntry& e : pt::game::ArchiveEntries()) {
                if (e.section != section || !game_.ArchiveUnlocked(e)) continue;
                pt::game::PcPanel cover = EntryPicture(e);
                if (cover.HasPicture()) return cover;
            }
            panel.locked = true;
            return panel;
        }
        const pt::game::ArchiveEntry* entry = EntryOf(id);
        if (page_ != kArchiveListPage || !entry) return panel;
        const int language = Language();
        panel.title = pt::game::ArchiveLabel(*entry, language);
        panel.caption = pt::game::ArchiveCaption(*entry);
        if (!game_.ArchiveUnlocked(*entry)) {
            panel.locked = true;
            return panel;
        }
        pt::game::PcPanel picture = EntryPicture(*entry);
        panel.texture = std::move(picture.texture);
        panel.string = picture.string;
        panel.photo = std::move(picture.photo);
        panel.thumbnail = std::move(picture.thumbnail);
        panel.lift = picture.lift;
        using M = pt::game::ArchiveMedia;
        switch (entry->media) {
        case M::Sound:
        case M::Dialogue: {
            if (!app_.transcripts) break;
            const float now = voice_entry_ == entry ? VoiceSeconds() : -1.0f;
            for (const auto& line : app_.transcripts->Transcript(SubtitleOf(*entry), language)) {
                if (now >= 0.0f && now >= line.start) panel.current_line = static_cast<int>(panel.lines.size());
                panel.lines.push_back(line.text);
            }
            if (now >= 0.0f && panel.current_line < 0 && !panel.lines.empty()) panel.current_line = 0;
            break;
        }
        default:
            break;
        }
        return panel;
    }
    bool FullScreen() const override { return page_ == kArchiveListPage && archive_full_; }
    std::string_view FullScreenHint() const override { return "pc_archive_view_hint"; }
    void CursorMoved() override { StopVoice(); }
    int TakeCursor() override { return std::exchange(pending_cursor_, -1); }
    bool KeepsCursor() const override { return keep_cursor_; }
    bool CompactRows() const override { return page_ == kLoopPage || (page_ == kStreetPage && game_.SpeedrunResultsPending()); }
    std::string_view Credit() const override {
        if (page_ == kExtrasPage) return "Port by LoreanXavier";
        if (page_ == kMainPage) {
            if (const auto newer = app_.updates.Newer()) {
                if (update_notice_.empty()) update_notice_ = UpdateNoticeKey(newer->version);
                return update_notice_;
            }
            if (version_text_.empty()) version_text_ = std::format("pc_credit_version{}{}", pt::game::kPcNoteArgument, pt::update::CurrentVersion());
            return version_text_;
        }
        return std::string_view();
    }
    mutable std::string update_notice_;
    mutable std::string version_text_;
    std::vector<std::string> PreviewFiles(int id) const override {
        std::vector<std::string> files;
        if (page_ == kArchivePage || page_ == kArchiveListPage) {
            MuseumPreviewsGenerating();
            auto add = [&](const pt::game::PcPanel& panel) {
                if (!panel.thumbnail.empty() && std::find(files.begin(), files.end(), panel.thumbnail) == files.end()) files.push_back(panel.thumbnail);
            };
            add(Panel(id));
            if (page_ == kArchivePage) {
                for (int s = 0; s < pt::game::kArchiveSectionCount; ++s) add(Panel(kArchiveSectionFirst + s));
            } else {
                const auto entries = pt::game::ArchiveEntries();
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (entries[i].section == static_cast<pt::game::ArchiveSection>(archive_section_)) add(Panel(kArchiveEntryFirst + static_cast<int>(i)));
                }
            }
            return files;
        }
        if (!IsLoopBrowser()) return files;
        if (std::string current = PreviewFile(id); !current.empty()) files.push_back(std::move(current));
        for (int i = 0; i < kLoopCount; ++i) {
            if (std::string file = PreviewFile(kLoopFirst + i); !file.empty() && (files.empty() || file != files.front())) files.push_back(std::move(file));
        }
        return files;
    }
    std::string PreviewFile(int id) const override {
        const int index = id - kLoopFirst;
        return IsLoopBrowser() && index >= 0 && index < kLoopCount && LoopUnlocked(index) ?
            (PreviewDirectory() / std::format("loop-{}.png", index)).string() : std::string();
    }

    void Activate(int id) override {
        keep_cursor_ = false;
        if (id == kArchive) {
            page_ = kArchivePage;
            GenerateMuseumPreviews();
        }
        if (id >= kArchiveSectionFirst && id < kArchiveSectionFirst + pt::game::kArchiveSectionCount) {
            archive_section_ = id - kArchiveSectionFirst;
            page_ = kArchiveListPage;
        }
        if (id == kArchiveLock) {
            game_.SetArchiveSeen({});
            app_.settings.progress.archive.clear();
            Save();
            pt::LogInfo("archive: unlocks cleared from the Archive page");
        }
        if (const pt::game::ArchiveEntry* entry = EntryOf(id)) {
            ActivateEntry(*entry);
        }
        if (id == kLoopBrowser) { page_ = kLoopPage; GeneratePreviews(); }
        if (id == kExtras) page_ = kExtrasPage;
        if (id == kVr) page_ = kVrPage;
        if (id == kFreecam) app_.extras_request = 1;
        if (id == kPhotoMode) app_.extras_request = 2;
        if (id >= kLoopFirst && id < kLoopFirst + kLoopCount && LoopUnlocked(id - kLoopFirst)) game_.BrowseLoop(id - kLoopFirst);
        if (id == kStreetWalk) game_.AnswerStreetOffer(true);
        if (id == kRunMenu) game_.AnswerSpeedrunMenu();
        if (id == kStreetRestart) game_.AnswerStreetOffer(false);
        if (id == kStreetLeave) app_.extras_request = 3;
        if (id == kLoopLock) {
            game_.SetBrowseUnlocks(0, false);
            app_.settings.progress = {};
            Save();
            pt::LogInfo("loop browser: unlocks cleared from the Extras page");
        }
        if(id==kLighting) page_=kLightingPage;
        if(id==kTextures) page_=kTexturesPage;
        if(id==kEffects) page_=kEffectsPage;
        if (id == kMods) page_ = kModsPage;
        if (id == kReset && game_.SavesEnabled()) {
            pt::LogInfo("save: progress reset from the PC settings page ({})", game_.ResetProgress() ? "accepted" : "failed");
        }
        if (id == kGraphics) {
            page_ = kGraphicsPage;
        }
        if (id == kMicrophoneTest) {
            page_ = kMicrophonePage;
            app_.microphone_test = true;
            app_.microphone_monitor = false;
            app_.microphone_hypothesis.clear();
            app_.microphone_db = -80.0f;
            app_.microphone_status = "pc_mic_waiting";
        }
    }

    void Opened() override {
        page_ = game_.StreetOfferPending() ? kStreetPage : kMainPage;
        app_.microphone_test = app_.microphone_monitor = false;
        archive_full_ = false;
        StopVoice();
    }

    void Closed() override {
        app_.microphone_test = app_.microphone_monitor = false;
        archive_full_ = false;
        StopVoice();
        if (game_.StreetOfferPending()) game_.AnswerStreetOffer(false);
    }

    int RefreshEveryFrames() const override { return page_ == kMicrophonePage || voice_entry_ ? 3 : 15; }

    bool Back() override {
        if (page_ == kMainPage) {
            return false;
        }
        StopVoice();
        if (page_ == kArchiveListPage && archive_full_) {
            archive_full_ = false;
            return true;
        }
        if (page_ == kArchiveListPage) {
            page_ = kArchivePage;
            pending_cursor_ = archive_section_;
            return true;
        }
        if (page_ == kArchivePage) {
            page_ = kExtrasPage;
            pending_cursor_ = game_.StreetWalkActive() ? 2 : 1;
            return true;
        }
        page_ = page_==kLightingPage || page_==kTexturesPage || page_==kEffectsPage ? kGraphicsPage : page_ == kLoopPage || page_ == kVrPage ? kExtrasPage : kMainPage;
        app_.microphone_test = app_.microphone_monitor = false;
        return true;
    }

private:
    enum RowId {
        kDisplayMode,
        kResolution,
        kVsync,
        kFocusPause,
        kFocusMute,
        kUpscaler,
        kQuality,
        kSharpness,
        kDlssModel,
        kFrameGeneration,
        kVolume,
        kMicrophone,
        kMicrophoneTest,
        kMicLevel,
        kMicHeard,
        kMicMonitor,
        kMouse,
        kCameraTilt,
        kVibration,
        kDeadZone,
        kReset,
        kRtShadows,
        kGraphics,
        kRtContact,
        kRtAo,
        kAnisotropy,
        kEnhancedTextures,
        kRtReflections,
        kGraphicsPreset,kRayTracing,kLighting,kTextures,kEffects,
        kShadowQuality,kRasterAo,kRasterReflections,kRayQuality,kTextureDetail,
        kBloom,kLensGhosts,kDepthOfField,kMotionBlur,kFilmGrain,kClarity,kLensDistortion,kLetterbox,
        kMods,
        kLoopBrowser, kExtras, kFreecam, kPhotoMode, kStreetWalk, kStreetRestart, kStreetLeave, kLoopLock, kThirdPerson, kSpeedrun, kLiveSplit, kRunMenu, kRunReal, kRunGame, kRunBest,
        kArchive, kArchiveLock,
        kVr, kVrMode, kVrFlashlight, kVrTurn,
        kLoopFirst = 1000,
        kRunSplitFirst = 3000,
        kModFirst = 2000,
        kArchiveSectionFirst = 3000,
        kArchiveEntryFirst = 4000,
    };
    static constexpr int kArchivePage = 10;
    static constexpr int kArchiveListPage = 11;
    int archive_section_ = 0;
    bool archive_full_ = false;
    int pending_cursor_ = -1;
    bool keep_cursor_ = false;
    const pt::game::ArchiveEntry* voice_entry_ = nullptr;
    uint32_t voice_id_ = 0;
    uint64_t voice_start_ = 0;

    int Language() const { return std::clamp(game_.Options().subtitle_language, 0, 11); }

    const pt::game::ArchiveEntry* EntryOf(int id) const {
        const auto entries = pt::game::ArchiveEntries();
        const int index = id - kArchiveEntryFirst;
        return index >= 0 && index < static_cast<int>(entries.size()) ? &entries[index] : nullptr;
    }

    std::vector<pt::game::PcSettingSection> ArchiveSections() const {
        pt::game::PcSettingSection sections{"pc_archive_sections", 0, {}};
        for (int s = 0; s < pt::game::kArchiveSectionCount; ++s) {
            const auto section = static_cast<pt::game::ArchiveSection>(s);
            int open = 0;
            int total = 0;
            for (const pt::game::ArchiveEntry& e : pt::game::ArchiveEntries()) {
                if (e.section != section) continue;
                ++total;
                open += game_.ArchiveUnlocked(e) ? 1 : 0;
            }
            auto row = Row(kArchiveSectionFirst + s, std::string(pt::game::ArchiveSectionTitle(section)), {std::format("{} / {}", open, total)}, 0,
                           std::string(pt::game::ArchiveSectionNote(section)));
            row.link = true;
            sections.rows.push_back(std::move(row));
        }
        if (app_.options.release_locks) {
            auto lock = Row(kArchiveLock, "pc_archive_lock", {"pc_extras_start"}, 0, "pc_note_archive_lock");
            lock.action = true;
            lock.enabled = !game_.ArchiveSeen().empty();
            sections.rows.push_back(std::move(lock));
        }
        return {std::move(sections)};
    }

    std::vector<pt::game::PcSettingSection> ArchiveListSections() {
        const auto section = static_cast<pt::game::ArchiveSection>(archive_section_);
        pt::game::PcSettingSection list{std::string(pt::game::ArchiveSectionTitle(section)), 0, {}};
        if (voice_id_ && (!game_.Audio() || !game_.Audio()->IsPlaying(voice_id_))) {
            voice_id_ = 0;
            voice_entry_ = nullptr;
        }
        const int language = Language();
        const auto entries = pt::game::ArchiveEntries();
        for (size_t i = 0; i < entries.size(); ++i) {
            const pt::game::ArchiveEntry& e = entries[i];
            if (e.section != section) continue;
            const bool open = game_.ArchiveUnlocked(e);
            const char* note = !open ? "pc_note_archive_locked" : voice_entry_ == &e ? "pc_note_archive_playing" : nullptr;
            auto row = Row(kArchiveEntryFirst + static_cast<int>(i), pt::game::ArchiveLabel(e, language), {""}, 0, note ? note : std::string(e.note));
            row.link = true;
            row.enabled = open;
            list.rows.push_back(std::move(row));
        }
        return {std::move(list)};
    }

    std::filesystem::path MuseumPreviewDirectory() const {
        if (!app_.options.museum_previews.empty()) return app_.options.museum_previews;
        static const std::filesystem::path directory = [] {
            std::filesystem::path dir;
            if (char* pref = SDL_GetPrefPath("pt-port", "pt")) {
                dir = std::filesystem::path(pref) / std::format("museum-previews-v{}", kMuseumPreviewVersion);
                SDL_free(pref);
            }
            return dir;
        }();
        return directory;
    }
    bool MuseumPreviewsGenerating() const {
        if (!museum_process_) return false;
        int code = 0;
        if (!SDL_WaitProcess(museum_process_, false, &code)) return true;
        pt::LogInfo("museum: thumbnail capture finished (exit {})", code);
        SDL_DestroyProcess(museum_process_);
        museum_process_ = nullptr;
        return false;
    }
    void GenerateMuseumPreviews() {
        if (museum_started_ || app_.options.headless || !app_.options.museum_previews.empty()) return;
        const auto dir = MuseumPreviewDirectory();
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return;
        bool missing = false;
        for (const pt::game::ArchiveEntry* e : MuseumPreviewEntries()) {
            if (!std::filesystem::exists(dir / std::format("{}.png", e->id), ec)) missing = true;
        }
        if (!missing) return;
        const auto exe = GameExecutable();
        std::vector<std::string> args{exe.string(), "--game", app_.options.game_dir.string(), "--make-museum-previews", dir.string(),
            "--log", (dir / "capture.log").string()};
        std::vector<const char*> argv;
        for (auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        auto props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, SDL_GetBasePath());
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
        museum_process_ = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        museum_started_ = museum_process_ != nullptr;
        if (museum_process_) pt::LogInfo("museum: thumbnail capture started ({})", dir.string());
        else pt::LogWarn("museum: thumbnail capture could not start: {}", SDL_GetError());
    }
    bool museum_started_ = false;
    mutable SDL_Process* museum_process_ = nullptr;

    void ActivateEntry(const pt::game::ArchiveEntry& entry) {
        using M = pt::game::ArchiveMedia;
        keep_cursor_ = true;
        if (!game_.ArchiveUnlocked(entry)) return;
        switch (entry.media) {
        case M::Image:
        case M::String:
        case M::Photo:
            archive_full_ = true;
            break;
        case M::Sound:
        case M::Dialogue:
            if (voice_entry_ == &entry) {
                StopVoice();
            } else {
                PlayVoice(entry);
            }
            break;
        case M::Demo:
        case M::Model:
            StopVoice();
            app_.archive_request = std::string(entry.id);
            break;
        }
    }

    static std::string SubtitleOf(const pt::game::ArchiveEntry& entry) {
        if (entry.media == pt::game::ArchiveMedia::Dialogue && entry.unlock.starts_with("voice:")) return std::string(entry.unlock.substr(6));
        return std::string(entry.extra);
    }

    void PlayVoice(const pt::game::ArchiveEntry& entry) {
        StopVoice();
        auto* sound = dynamic_cast<pt::game::GameSound*>(game_.Audio());
        if (!sound || !sound->Ready()) {
            pt::LogWarn("archive: no sound to play {}", entry.id);
            return;
        }
        const glm::vec3 at = game_.GetCamera().position;
        if (entry.media == pt::game::ArchiveMedia::Dialogue) {
            constexpr uint32_t kDialogueEvent = 0xC48783C5;
            const std::string_view args[] = {entry.asset, entry.extra};
            voice_id_ = sound->PostDialogueAt(kDialogueEvent, args, at);
        } else {
            voice_id_ = sound->PostEvent(entry.asset, &at);
        }
        voice_entry_ = voice_id_ ? &entry : nullptr;
        voice_start_ = sound->System().RenderedFrames();
        pt::LogInfo("archive: {} plays {} (playing id {})", entry.id, entry.asset, voice_id_);
    }

    void StopVoice() {
        if (voice_id_ && game_.Audio()) game_.Audio()->StopPlayingId(voice_id_, 0.2f);
        voice_id_ = 0;
        voice_entry_ = nullptr;
    }

    float VoiceSeconds() const {
        auto* sound = dynamic_cast<pt::game::GameSound*>(game_.Audio());
        if (!sound || !sound->Ready()) return 0.0f;
        return static_cast<float>(sound->System().RenderedFrames() - voice_start_) / 48000.0f;
    }
    static constexpr int kPreviewVersion = kLoopPreviewVersion;
    static constexpr int kLoopCount = static_cast<int>(pt::game::kBrowseLoops.size());
    bool LoopUnlocked(int index) const { return game_.BrowseUnlocked(index); }
    static std::filesystem::path BundledPreviewDirectory() {
        static const std::filesystem::path bundled = [] {
            std::filesystem::path directory;
            if (const char* base = SDL_GetBasePath()) {
                directory = std::filesystem::path(base) / "loop-previews";
            }
            std::error_code ec;
            int version = 0;
            if (!directory.empty()) std::ifstream(directory / "version.txt") >> version;
            return version == kPreviewVersion && std::filesystem::exists(directory / "loop-0.png", ec) ? directory : std::filesystem::path();
        }();
        return bundled;
    }
    static std::filesystem::path PreviewDirectory() {
        if (auto bundled = BundledPreviewDirectory(); !bundled.empty()) return bundled;
        std::filesystem::path directory;
        if (char* pref = SDL_GetPrefPath("pt-port", "pt")) {
            directory = std::filesystem::path(pref) / std::format("loop-previews-v{}", kPreviewVersion);
            SDL_free(pref);
        }
        return directory;
    }
    bool PreviewsGenerating() {
        if (!preview_process_) return false;
        int code = 0;
        if (!SDL_WaitProcess(preview_process_, false, &code)) return true;
        pt::LogInfo("loop browser: preview generation finished (exit {})", code);
        SDL_DestroyProcess(preview_process_);
        preview_process_ = nullptr;
        if (preview_runs_ < 2) {
            preview_started_ = false;
            GeneratePreviews();
            return preview_process_ != nullptr;
        }
        return false;
    }
    void GeneratePreviews() {
        if (preview_started_ || preview_runs_ >= 2 || app_.options.headless || !BundledPreviewDirectory().empty()) return;
        const auto dir = PreviewDirectory();
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(dir.parent_path() / "loop-previews", ec);
        for (int v = 2; v < kPreviewVersion; ++v) std::filesystem::remove_all(dir.parent_path() / std::format("loop-previews-v{}", v), ec);
        std::filesystem::create_directories(dir, ec);
        if (ec) return;
        std::vector<int> missing;
        for (int i = 0; i < kLoopCount; ++i) {
            if (!std::filesystem::exists(dir / std::format("loop-{}.png", i), ec)) missing.push_back(i);
        }
        if (missing.empty()) return;
        std::ofstream(dir / "capture.txt") << LoopPreviewRoute(dir, missing);
        pt::SaveAppSettings(dir / "preview.ini", pt::AppSettings{});
        const auto exe = GameExecutable();
        std::vector<std::string> args{exe.string(), "--headless", "--no-save", "--audio-offline", "--game", app_.options.game_dir.string(),
            "--frames", "120000", "--demo-rate", "20", "--street-offer", "never", "--seed", std::to_string(kLoopPreviewSeed),
            "--bug-screen", std::to_string(kLoopPreviewBugScreen), "--f160-light", std::to_string(kLoopPreviewF160Roll), "--shot-warmup", "30", "--shot-settle", "--width", "640", "--height", "360", "--input-script", (dir/"capture.txt").string(),
            "--settings", (dir/"preview.ini").string(), "--log", (dir/"capture.log").string()};
        std::vector<const char*> argv;
        for (auto& arg:args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        auto props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, SDL_GetBasePath());
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
        SDL_Process* child = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        preview_started_ = child != nullptr;
        preview_process_ = child;
        if (child) ++preview_runs_;
        if (child) pt::LogInfo("loop browser: generating {} preview(s)", missing.size());
        else pt::LogWarn("loop browser: preview generation could not start: {}", SDL_GetError());
    }
    bool preview_started_ = false;
    int preview_runs_ = 0;
    SDL_Process* preview_process_ = nullptr;
    static constexpr int kLoopPage = 6;
    static constexpr int kModsPage = 7;
    static constexpr int kExtrasPage = 8;
    static constexpr int kStreetPage = 9;
    static constexpr int kVrPage = 12;
    static constexpr int kMaxModRows = 20;
    static constexpr int kMainPage = 0;
    static constexpr int kGraphicsPage = 1;
    static constexpr int kMicrophonePage = 2;
    static constexpr int kLightingPage=3,kTexturesPage=4,kEffectsPage=5;
    static constexpr int kAnisotropySteps[] = {0, 2, 4, 8, 16};
    int page_ = kMainPage;
    static constexpr float kMouseSteps[] = {0.25f, 0.35f, 0.5f, 0.7f, 1.0f, 1.4f, 2.0f, 2.8f, 4.0f, 5.0f};
    static constexpr float kDeadZoneSteps[] = {0.0f, 0.05f, 26.0f / 255.0f, 0.15f, 0.2f, 0.25f, 0.3f};

    static pt::game::PcSettingRow Row(int id, std::string label, std::vector<std::string> values, int value, std::string note) {
        pt::game::PcSettingRow row;
        row.id = id;
        row.label = std::move(label);
        row.values = std::move(values);
        row.value = value;
        row.note = std::move(note);
        return row;
    }

    static std::vector<std::string> OffOn() { return {"pc_off", "pc_on"}; }

    std::vector<pt::game::PcSettingSection> SpeedrunResults() const {
        const pt::game::SpeedrunTimer& run = game_.Speedrun();
        using pt::game::SpeedrunTimer;
        const int language = std::clamp(game_.Options().subtitle_language, 0, 11);
        pt::game::PcSettingSection result{"pc_speedrun_run", 0, {}};
        auto menu = Row(kRunMenu, "pc_speedrun_menu", {""}, 0, "pc_note_speedrun_menu");
        menu.link = true;
        result.rows.push_back(std::move(menu));
        auto outside = Row(kStreetWalk, "pc_speedrun_outside", {""}, 0, "pc_note_speedrun_outside");
        outside.link = true;
        result.rows.push_back(std::move(outside));
        result.rows.push_back(Row(kRunReal, "pc_speedrun_real_time", {SpeedrunTimer::Format(run.Real())}, 0, "pc_note_speedrun_real"));
        result.rows.push_back(Row(kRunGame, "pc_speedrun_game_time", {SpeedrunTimer::Format(run.Game())}, 0, "pc_note_speedrun_game"));
        const std::string best = !run.FullRun() ? std::string("pc_speedrun_partial")
                                 : run.NewBest() ? std::string("pc_speedrun_new_best")
                                                 : SpeedrunTimer::Format(run.BestTotal());
        const std::string previous = run.PreviousBest() > 0.0 ? SpeedrunTimer::Format(run.PreviousBest()) : std::string("-");
        result.rows.push_back(Row(kRunBest, "pc_speedrun_best", {best}, 0,
                                  std::format("pc_note_speedrun_best{}{}", pt::game::kPcNoteArgument, previous)));
        constexpr size_t kRightRows = 19;
        pt::game::PcSettingSection right{"pc_speedrun_splits", 1, {}};
        pt::game::PcSettingSection more{"pc_speedrun_splits", 0, {}};
        const auto& splits = run.Splits();
        for (size_t i = 0; i < splits.size(); ++i) {
            const auto& s = splits[i];
            auto row = Row(kRunSplitFirst + static_cast<int>(i), SpeedrunTimer::SplitName(s, language),
                           {SpeedrunTimer::Format(run.ShowsGameTime() ? s.game : s.real)}, 0, std::string());
            (i < kRightRows ? right : more).rows.push_back(std::move(row));
        }
        std::vector<pt::game::PcSettingSection> out{std::move(result), std::move(right)};
        if (!more.rows.empty()) out.push_back(std::move(more));
        return out;
    }

    pt::game::PcSettingRow RtSwitchRow(int id, const char* label, bool on, const char* note_on) const {
        std::string reason;
        const bool supported = app_.scene.RayTracingSupported(reason);
        const int value = supported && on ? 1 : 0;
        const char* note = !supported ? "pc_note_rt_unsupported" : value > 0 && !app_.scene.RayTracingReady() ? "pc_note_restart" : note_on;
        pt::game::PcSettingRow row = Row(id, label, OffOn(), value, note);
        row.enabled = supported;
        return row;
    }

    pt::game::PcSettingRow EnhancedRow() const {
        auto values=OffOn();
        const bool preparing=app_.texture_requested;
        if(preparing) values[1]="pc_texture_preparing";
        const auto job=app_.texture_job.GetStatus();
        const char* note=preparing?"pc_note_texture_preparing":"pc_note_enhanced_textures";
        if(!preparing && job.state==pt::EnhancedTextureJob::State::Failed) {
            note=job.note=="Enhanced texture tools or model are missing."?"pc_note_texture_tools_missing":
                 job.note=="Texture generation is already running in another instance."?"pc_note_texture_busy":"pc_note_texture_failed";
        }
        return Row(kEnhancedTextures,"pc_enhanced_textures",std::move(values),app_.settings.graphics.enhanced_textures?1:0,note);
    }

    std::vector<pt::game::PcSettingSection> GraphicsSections() const {
        const auto& s=app_.settings;
        std::string reason;
        const bool supported=app_.scene.RayTracingSupported(reason);
        pt::game::PcSettingSection quality{"pc_graphics_quality",0,{}};
        auto preset=Row(kGraphicsPreset,"pc_graphics_preset",{"pc_preset_low","pc_preset_original","pc_preset_high","pc_preset_ultra","pc_preset_custom"},
                        static_cast<int>(pt::DetectGraphicsPreset(s,supported)),"pc_note_preset");
        preset.wrap=false;quality.rows.push_back(std::move(preset));
        auto lighting = LightingSections();
        auto textures = TextureSections();
        auto effects = EffectSections();
        textures[0].column = 1;
        effects[0].column = 1;
        effects[1].column = 1;
        return {std::move(quality), std::move(lighting[0]),
                std::move(textures[0]), std::move(effects[0]), std::move(effects[1])};
    }

    std::vector<pt::game::PcSettingSection> LightingSections() const {
        const auto& s=app_.settings;
        const auto& g=s.graphics;
        std::string reason;
        const bool supported=app_.scene.RayTracingSupported(reason);
        const bool ready=app_.scene.RayTracingReady();
        const int rt_shadows=supported?s.ray_tracing.shadows:0;
        const bool rt_ao=supported&&s.ray_tracing.ambient_occlusion;
        const bool rt_reflections=supported&&s.ray_tracing.reflections;
        auto note=[&](bool traced,const char* base){return traced&&!ready?"pc_note_restart":base;};
        pt::game::PcSettingSection lighting{"pc_graphics_lighting",0,{}};
        std::vector<std::string> shadow_values{"pc_off","pc_preset_low","pc_preset_medium","pc_preset_original"};
        if(supported){shadow_values.push_back("pc_rt_sharp_long");shadow_values.push_back("pc_rt_soft_long");}
        auto shadows=Row(kShadowQuality,"pc_shadows",std::move(shadow_values),rt_shadows>0?3+rt_shadows:g.shadow_quality,note(rt_shadows>0,"pc_note_shadows"));
        shadows.wrap=false;lighting.rows.push_back(std::move(shadows));
        lighting.rows.push_back(RtSwitchRow(kRtContact,"pc_rt_contact",s.ray_tracing.contact_shadows,"pc_note_rt_contact"));
        std::vector<std::string> traced{"pc_off","pc_standard"};
        if(supported)traced.push_back("pc_ray_traced");
        auto ao=Row(kRasterAo,"pc_rt_ao",traced,rt_ao?2:g.ambient_occlusion?1:0,note(rt_ao,"pc_note_ao"));
        ao.wrap=false;lighting.rows.push_back(std::move(ao));
        auto reflections=Row(kRasterReflections,"pc_reflections",std::move(traced),rt_reflections?2:g.reflections?1:0,note(rt_reflections,"pc_note_reflections"));
        reflections.wrap=false;lighting.rows.push_back(std::move(reflections));
        auto quality=Row(kRayQuality,"pc_ray_quality",{"pc_ray_balanced","pc_preset_high","pc_preset_ultra"},g.ray_quality,"pc_note_ray_quality");
        quality.enabled=supported&&(rt_shadows==2||rt_ao);lighting.rows.push_back(std::move(quality));
        return {std::move(lighting)};
    }

    std::vector<pt::game::PcSettingSection> TextureSections() const {
        const auto& g=app_.settings.graphics;
        pt::game::PcSettingSection textures{"pc_section_textures",0,{}};
        textures.rows.push_back(EnhancedRow());
        auto anisotropy=Row(kAnisotropy,"pc_anisotropy",{"pc_off","2x","4x","8x","16x"},g.anisotropy>=16?4:g.anisotropy>=8?3:g.anisotropy>=4?2:g.anisotropy>=2?1:0,"pc_note_anisotropy");
        anisotropy.wrap=false;textures.rows.push_back(std::move(anisotropy));
        textures.rows.push_back(Row(kTextureDetail,"pc_texture_detail",{"pc_detail_reduced","pc_preset_original","pc_detail_fine"},g.texture_detail,"pc_note_texture_detail"));
        return {std::move(textures)};
    }

    std::vector<pt::game::PcSettingSection> ModSections() const {
        pt::game::PcSettingSection left{"pc_section_mods", 0, {}};
        pt::game::PcSettingSection right{"pc_section_mods", 1, {}};
        if (!app_.mods) return {std::move(left)};
        const int language = std::clamp(game_.Options().subtitle_language, 0, 11);
        const std::string restart(pt::game::PcText("pc_note_mods", language));
        const auto& mods = app_.mods->mods;
        for (size_t i = 0; i < mods.size() && i < static_cast<size_t>(kMaxModRows); ++i) {
            const pt::mods::Mod& mod = mods[i];
            const auto choice = app_.settings.mods.find(mod.folder);
            const bool on = choice != app_.settings.mods.end() ? choice->second : mod.manifest.enabled;
            std::string label = mod.manifest.version.empty() ? mod.Name() : std::format("{} {}", mod.Name(), mod.manifest.version);
            if (label.size() > 28) label = label.substr(0, 26) + "...";
            std::string note = mod.manifest.description;
            if (!mod.manifest.author.empty()) note += (note.empty() ? "" : " ") + std::format("({})", mod.manifest.author);
            note += (note.empty() ? "" : " ") + restart;
            (i < static_cast<size_t>(kMaxModRows / 2) ? left : right).rows.push_back(Row(kModFirst + static_cast<int>(i), label, OffOn(), on ? 1 : 0, note));
        }
        std::vector<pt::game::PcSettingSection> out;
        out.push_back(std::move(left));
        if (!right.rows.empty()) out.push_back(std::move(right));
        return out;
    }

    std::vector<pt::game::PcSettingSection> EffectSections() const {
        const auto& g=app_.settings.graphics;
        pt::game::PcSettingSection effects{"pc_graphics_effects",0,{}};
        effects.rows.push_back(Row(kDepthOfField,"pc_dof",OffOn(),g.depth_of_field?1:0,"pc_note_dof"));
        effects.rows.push_back(Row(kMotionBlur,"pc_motion_blur",OffOn(),g.motion_blur?1:0,"pc_note_motion_blur"));
        effects.rows.push_back(Row(kBloom,"pc_bloom",OffOn(),g.bloom?1:0,"pc_note_bloom"));
        effects.rows.push_back(Row(kLensGhosts,"pc_lens_ghosts",OffOn(),g.lens_ghosts?1:0,"pc_note_lens_ghosts"));
        pt::game::PcSettingSection clarity{"pc_graphics_image",1,{}};
        clarity.rows.push_back(Row(kFilmGrain,"pc_film_grain",{"pc_off","25%","50%","75%","100%"},static_cast<int>(g.film_grain*4+.5f),"pc_note_film_grain"));
        clarity.rows.push_back(Row(kLensDistortion,"pc_lens_distortion",OffOn(),g.lens_distortion?1:0,"pc_note_lens_distortion"));
        clarity.rows.push_back(Row(kClarity,"pc_clarity",Numbers(0,10),static_cast<int>(g.clarity*10+.5f),"pc_note_clarity"));
        clarity.rows.push_back(Row(kLetterbox,"pc_photo_letterbox",{"pc_off","2.39:1","1.85:1"},std::clamp(app_.settings.display.letterbox,0,2),"pc_note_letterbox"));
        return {std::move(effects),std::move(clarity)};
    }

    static std::vector<std::string> Numbers(int first, int last) {
        std::vector<std::string> out;
        for (int i = first; i <= last; ++i) {
            out.push_back(std::to_string(i));
        }
        return out;
    }

    template <size_t N>
    static int Nearest(const float (&steps)[N], float value) {
        int best = 0;
        for (size_t i = 1; i < N; ++i) {
            if (std::abs(steps[i] - value) < std::abs(steps[best] - value)) {
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    static std::string Sentence(std::string text) {
        if (!text.empty()) {
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            text += '.';
        }
        return text;
    }

    static std::string ShortName(pt::UpscalerKind kind) {
        switch (kind) {
        case pt::UpscalerKind::Fsr: return "FSR 3";
        case pt::UpscalerKind::Fsr4: return "FSR 4";
        case pt::UpscalerKind::Dlss: return "DLSS";
        case pt::UpscalerKind::Xess: return "XeSS";
        default: return pt::UpscalerName(kind);
        }
    }

    std::vector<pt::UpscalerKind> Upscalers(std::vector<std::string>* notes) const {
        std::vector<pt::UpscalerKind> kinds{pt::UpscalerKind::Off};
        if (notes) notes->assign(1, std::string());
        for (const pt::UpscalerKind kind : {pt::UpscalerKind::Fsr, pt::UpscalerKind::Fsr4, pt::UpscalerKind::Dlss, pt::UpscalerKind::Xess}) {
            std::string reason;
            const bool available = app_.scene.UpscalerAvailable(kind, reason);
            kinds.push_back(kind);
            if (!notes) continue;
            if (available) {
                notes->emplace_back();
            } else if (kind == pt::UpscalerKind::Fsr4 && reason.starts_with("AMD releases FSR 4 for DirectX 12 only")) {
                notes->emplace_back("pc_note_fsr4_vulkan");
            } else {
                notes->push_back(std::format("{}: {}", ShortName(kind), Sentence(reason)));
            }
        }
        return kinds;
    }

    std::string RenderNote() const {
        const pt::UpscaleStats& stats = app_.scene.UpscaleStatistics();
        if (stats.active) {
            return std::format("pc_note_render_size{}{} x {}{}{} x {}", pt::game::kPcNoteArgument, stats.render.width, stats.render.height,
                pt::game::kPcNoteArgument, stats.output.width, stats.output.height);
        }
        return stats.error;
    }

    static std::vector<std::string> Microphones() {
        std::vector<std::string> names;
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_WasInit(SDL_INIT_AUDIO) ? SDL_GetAudioRecordingDevices(&count) : nullptr;
        for (int i = 0; devices && i < count; ++i) {
            if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
                names.emplace_back(name);
            }
        }
        SDL_free(devices);
        return names;
    }

    glm::ivec2 DesktopSize() const {
        const SDL_DisplayMode* mode = app_.window ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(app_.window)) : nullptr;
        return mode ? glm::ivec2(mode->w, mode->h) : glm::ivec2(1920, 1080);
    }

    std::vector<glm::ivec2> WindowSizes() const {
        const glm::ivec2 desktop = DesktopSize();
        std::vector<glm::ivec2> sizes;
        for (const glm::ivec2 size : {glm::ivec2(1280, 720), glm::ivec2(1600, 900), glm::ivec2(1920, 1080), glm::ivec2(2560, 1440), glm::ivec2(3200, 1800),
                                      glm::ivec2(3840, 2160)}) {
            if (size.x <= desktop.x && size.y <= desktop.y) {
                sizes.push_back(size);
            }
        }
        const glm::ivec2 current(app_.settings.display.width, app_.settings.display.height);
        if (std::find(sizes.begin(), sizes.end(), current) == sizes.end()) {
            sizes.push_back(current);
            std::sort(sizes.begin(), sizes.end(), [](glm::ivec2 a, glm::ivec2 b) { return a.x * a.y < b.x * b.y; });
        }
        return sizes;
    }

    void Save() {
        if (!app_.settings_path.empty()) {
            pt::SaveAppSettings(app_.settings_path, app_.settings);
        }
    }

    App& app_;
    pt::game::Game& game_;
    pt::InputDevice& input_;
};

bool PumpEvents(App& app, pt::InputDevice* input, bool& running) {
    if (!app.window) {
        return true;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (input) {
            input->ProcessEvent(event);
        }
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (running) {
                pt::LogInfo("window: {}", event.type == SDL_EVENT_QUIT ? "quit requested (window closed, Alt+F4 or the system)" : "close requested");
            }
            running = false;
        } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT)) {
            app.settings.display.fullscreen = app.settings.display.fullscreen ? 0 : 1;
            ApplyFullscreen(app);
            if (!app.settings_path.empty()) {
                pt::SaveAppSettings(app.settings_path, app.settings);
            }
        } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            app.renderer.Resize(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2));
        }
    }
    if (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(16);
        return false;
    }
    return true;
}

int RunViewer(App& app, pt::Vfs& vfs) {
    const Options& options = app.options;
    auto box = app.scene.Upload(pt::MakeBoxMesh(glm::vec3(0.5f)));
    std::vector<pt::DrawItem> draw_items;
    std::unique_ptr<pt::game::StageData> stage;
    if (!options.stage.empty()) {
        auto package = vfs.LoadPackage(options.stage);
        for (const auto& entry : package ? package->Entries() : std::vector<pt::FoxPackage::Entry>{}) {
            if (entry.path.ends_with(".fox2")) {
                auto file = std::make_shared<pt::fox2::DataSetFile>();
                const auto data = package->Read(entry);
                if (file->Load(entry.path, data)) {
                    stage = pt::game::BuildStageData(file, options.stage);
                    pt::game::LogStageData(*stage);
                }
                break;
            }
        }
    }
    pt::Camera camera;
    camera.position = glm::vec3(0.0f, 1.6f, 4.0f);
    camera.pitch = -0.2f;
    if (stage) {
        std::string fpk_path = options.stage;
        if (fpk_path.ends_with(".fpkd")) {
            fpk_path.pop_back();
        }
        vfs.LoadPackage(fpk_path);
        glm::vec3 lo(1e9f);
        glm::vec3 hi(-1e9f);
        const glm::vec3 root(stage->root[3]);
        for (const auto& placement : stage->static_models) {
            if (const pt::ModelEntry* model = app.models->Get(placement.model_file)) {
                pt::DrawItem item;
                item.mesh = model->mesh.get();
                item.transform = placement.world;
                draw_items.push_back(item);
            }
            const glm::vec3 p(placement.world[3]);
            if (glm::length(p - root) <= 60.0f) {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
        }
        pt::LogInfo("viewer: {} models loaded, {} failed, {} drawn, {} textures missing", app.models->Loaded(), app.models->Failed(), draw_items.size(),
                    app.models->MissingTextures());
        const glm::vec3 center = (lo + hi) * 0.5f;
        if (options.top_view) {
            camera.position = center + glm::vec3(0.0f, glm::max(hi.x - lo.x, hi.z - lo.z) * 0.9f + 5.0f, 0.01f);
            camera.pitch = -1.5f;
        }
    }
    if (!options.texture_test.empty()) {
        bool loaded = false;
        const uint32_t tex = app.textures.LoadFox(vfs.Textures(), options.texture_test, &loaded);
        pt::LogInfo("texture test {}: {} (index {})", options.texture_test, loaded ? "loaded" : "failed", tex);
        pt::MaterialGpu material;
        material.albedo = tex;
        pt::DrawItem item;
        item.mesh = box.get();
        item.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.6f, 2.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(1.6f, 1.6f, 0.01f));
        item.material_override = static_cast<int32_t>(app.textures.AddMaterial(material));
        draw_items.push_back(item);
        app.scene.debug_view = pt::DebugView::Albedo;
    }
    if (options.has_camera) {
        camera.position = glm::vec3(options.camera);
        camera.yaw = options.camera.w;
        camera.pitch = 0.0f;
    }
    app.scene.LoadResources(vfs);
    pt::game::RenderSceneBuilder scene_builder;
    pt::SceneLighting lighting;
    if (stage) {
        scene_builder.BuildFromStage(*stage, vfs, lighting);
    }
    pt::AssetBrowser browser(vfs);
    bool running = true;
    int frame = 0;
    uint64_t last_ticks = SDL_GetTicksNS();
    while (running) {
        if (!PumpEvents(app, nullptr, running)) {
            continue;
        }
        PollEnhancedTextures(app);
        if (app.window) {
            const uint64_t now = SDL_GetTicksNS();
            const float dt = static_cast<float>(now - last_ticks) * 1e-9f;
            last_ticks = now;
            if (!ImGui::GetIO().WantCaptureKeyboard) {
                const bool* keys = SDL_GetKeyboardState(nullptr);
                const float speed = (keys[SDL_SCANCODE_LSHIFT] ? 6.0f : 2.0f) * dt;
                if (keys[SDL_SCANCODE_W]) camera.position += camera.Forward() * speed;
                if (keys[SDL_SCANCODE_S]) camera.position -= camera.Forward() * speed;
                if (keys[SDL_SCANCODE_D]) camera.position += camera.Right() * speed;
                if (keys[SDL_SCANCODE_A]) camera.position -= camera.Right() * speed;
                if (keys[SDL_SCANCODE_E]) camera.position.y += speed;
                if (keys[SDL_SCANCODE_Q]) camera.position.y -= speed;
            }
            float mouse_dx = 0.0f;
            float mouse_dy = 0.0f;
            if (SDL_GetRelativeMouseState(&mouse_dx, &mouse_dy) & SDL_BUTTON_RMASK) {
                camera.yaw -= mouse_dx * 0.003f;
                camera.pitch = glm::clamp(camera.pitch - mouse_dy * 0.003f, -1.5f, 1.5f);
            }
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
            browser.Draw();
            if (ImGui::Begin("View")) {
                ImGui::Text("camera %.2f %.2f %.2f yaw %.2f pitch %.2f", camera.position.x, camera.position.y, camera.position.z, camera.yaw, camera.pitch);
                ImGui::SliderFloat("exposure", &app.renderer.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            }
            ImGui::End();
            app.scene.DrawDebugUi();
            ImGui::Render();
        }
        if (!app.renderer.BeginFrame()) {
            continue;
        }
        app.scene.Render(camera, draw_items, lighting, 1.0f / 60.0f);
        app.renderer.EndFrame(app.window != nullptr);
        ++frame;
        if (options.headless && frame >= options.frames) {
            if (!options.screenshot.empty()) {
                app.renderer.SaveScreenshot(options.screenshot);
            }
            running = false;
        }
    }
    vkDeviceWaitIdle(app.renderer.Context().device);
    app.scene.Destroy(*box);
    return 0;
}

int RunGame(App& app, pt::Vfs& vfs) {
    const Options& options = app.options;
    pt::game::Game game(vfs, *app.models);
    pt::game::GameConfig config;
    config.use_save = !options.no_save && !options.headless;
    if (char* pref = SDL_GetPrefPath("pt-port", "pt")) {
        config.save_path = std::filesystem::path(pref) / "PT_Save_Data.sav";
        SDL_free(pref);
    }
    if (!options.save_dir.empty()) {
        config.save_path = options.save_dir / "PT_Save_Data";
        config.use_save = !options.no_save;
    }
    config.start_floor = options.start_floor;
    config.seed = options.seed;
    config.handy_light_roll = options.handy_light_roll;
    config.bug_screen = options.bug_screen;
    config.street_offer = options.street_offer;
    config.release_locks = options.release_locks;
    const char* trap_log_env = std::getenv("PT_TRAP_LOG");
    config.trap_log = options.trap_log || (trap_log_env && *trap_log_env == '1');
    config.first_boot_options = options.first_boot_options;
    if (!game.Init(config)) {
        return 1;
    }
    game.Speedrun().SetMode(app.settings.extras.speedrun);
    if (!app.settings_path.empty()) game.Speedrun().SetRecordDirectory(std::filesystem::absolute(app.settings_path).parent_path());
    app.livesplit.Configure(app.settings.extras.livesplit, app.settings.extras.livesplit_host, app.settings.extras.livesplit_port);
    game.Speedrun().SetLiveSplit(&app.livesplit);
    uint32_t unlocks_seen = 0;
    if (game.SavesEnabled()) {
        auto& progress = app.settings.progress;
        if (game.GamePlus() && !progress.game_finished) {
            progress.game_finished = true;
            progress.loops_reached = (1u << pt::game::kBrowseLoops.size()) - 1u;
            if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
            pt::LogInfo("loop browser: save has the finished flag, all entries unlocked");
        }
        game.SetBrowseUnlocks(progress.loops_reached, progress.game_finished);
        unlocks_seen = game.BrowseUnlockGeneration();
        std::set<std::string> archive_keys;
        for (size_t at = 0; at < progress.archive.size();) {
            const size_t comma = std::min(progress.archive.find(',', at), progress.archive.size());
            if (comma > at) archive_keys.insert(progress.archive.substr(at, comma - at));
            at = comma + 1;
        }
        game.SetArchiveSeen(std::move(archive_keys));
    }
    uint32_t archive_seen = game.ArchiveGeneration();
    if (app.mods) {
        game.LoadModScripts(*app.mods);
    }
    game.GetPlayer().camera_roll = app.settings.camera.roll;
    game.SetThirdPerson(app.settings.camera.third_person || options.third_person);
    pt::game::GameSound sound(game);
    std::vector<float> captured;
    if ((app.window || options.audio_offline) && sound.Init(app.window != nullptr, "Eng")) {
        sound.System().SetMasterVolume(app.settings.audio.volume);
        if (options.seed) {
            sound.System().SetRandomSeed(options.seed);
        }
        game.SetAudio(&sound);
        game.Stages().ForEachStage([&](pt::game::Stage& stage) { sound.OnStageLoaded(stage); });
    }
    game.Demos().time_scale = options.demo_rate;
    pt::game::GameUi ui;
    bool hide_game_ui = false;
    pt::game::PhotoPanelView photo_view;
    const bool ui_ready = ui.Init(app.renderer, app.textures, vfs);
    if (ui_ready) {
        ui.SetViewExtent({app.options.width, app.options.height});
        game.SetOptionsUiAvailable(app.window != nullptr || options.first_boot_options);
        app.renderer.overlay = [&ui, &hide_game_ui, &photo_view, &app](VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {
            ui.SetLetterbox(pt::game::LetterboxAspect(app.settings.display.letterbox));
            if (!hide_game_ui) {
                ui.Record(cmd, view, extent);
            } else {
                ui.RecordPhotoMode(cmd, extent, photo_view);
            }
        };
    }
    pt::game::InputScript script;
    if (!options.input_script.empty()) {
        script.Load(options.input_script);
    }
    if (!options.input_text.empty()) {
        script.Parse(options.input_text);
    }
    pt::InputDevice input;
    input.settings.mouse_sensitivity *= app.settings.input.mouse_sensitivity;
    input.settings.stick_dead_zone = app.settings.input.gamepad_dead_zone;
    input.settings.rumble = app.settings.input.rumble;
    PcSettings pc_settings(app, game, input);
    if (ui_ready) {
        ui.SetPcSettings(&pc_settings);
        app.transcripts = &ui.Subtitles();
    }
    const bool pads = app.window || options.virtual_pads;
    if (pads) {
        input.Init();
    }
    if (app.window) {
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    }
    pt::VirtualPads virtual_pads;
    pt::game::PromptTextures prompt_textures;
    prompt_textures.Init(vfs, app.textures, app.window != nullptr);
    script.SetPads(options.virtual_pads ? &virtual_pads : nullptr, &input);
    std::vector<float> offline_audio;
    double audio_device_time = 0.0;
    uint64_t audio_device_frames = 0;
    const SDL_Scancode voice_key = app.settings.voice.key.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(app.settings.voice.key.c_str());
    bool voice_key_down = false;
    std::unique_ptr<pt::VoiceRecognizer> recognizer;
    pt::Microphone microphone;
    std::string active_microphone_device;
    bool previous_microphone_test = false;
    bool microphone_failed = false;
    std::vector<int16_t> mic_samples;
    std::vector<int16_t> voice_input;
    size_t voice_input_at = 0;
    double voice_input_due = 0.0;
    if (const char* path = std::getenv("PT_VOICE_INPUT")) {
        SDL_AudioSpec spec{};
        Uint8* data = nullptr;
        Uint32 length = 0;
        const SDL_AudioSpec target{SDL_AUDIO_S16, 1, pt::VoiceRecognizer::kSampleRate};
        Uint8* converted = nullptr;
        int converted_length = 0;
        if (SDL_LoadWAV(path, &spec, &data, &length) &&
            SDL_ConvertAudioSamples(&spec, data, static_cast<int>(length), &target, &converted, &converted_length)) {
            const auto* samples = reinterpret_cast<const int16_t*>(converted);
            voice_input.assign(samples, samples + converted_length / 2);
            pt::LogInfo("voice: PT_VOICE_INPUT {} ({:.2f} s) replaces the microphone", path,
                        static_cast<double>(voice_input.size()) / pt::VoiceRecognizer::kSampleRate);
        } else {
            pt::LogError("voice: cannot read PT_VOICE_INPUT {}: {}", path, SDL_GetError());
        }
        SDL_free(data);
        SDL_free(converted);
    }
    bool show_debug = !options.headless && options.debug_panel;
    bool show_settings = false;
    bool paused_by_settings = false;
    bool mouse_captured = false;
    std::vector<pt::DrawItem> draw_items;
    app.scene.LoadResources(vfs);
    pt::VfxPass vfx_pass;
    pt::game::VfxScene vfx_scene;
    if (vfx_pass.Init(app.renderer, app.textures, vfs)) {
        app.scene.vfx_forward = [&vfx_pass](const pt::SceneVfxContext& c) { vfx_pass.RecordForward(c); };
        app.scene.vfx_filter = [&vfx_pass](const pt::SceneFilterContext& c) { vfx_pass.RecordFilter(c); };
    }
    pt::game::RenderSceneBuilder scene_builder;
    pt::SceneLighting lighting;
    double last_scene_time = game.Time();
    bool running = true;
    uint64_t frame = 0;
    uint64_t last_ticks = SDL_GetTicksNS();
    pt::InputState pending_input;
    float accumulator = 0.0f;
    pt::Camera tick_camera_from = game.ViewCamera();
    pt::Camera motion_camera_from = tick_camera_from;
    const bool paced = app.window != nullptr || options.display_rate > 0.0f;
    TickState tick_state;
    std::vector<glm::mat4> blended_skins;
    static const bool render_trace = std::getenv("PT_RENDER_TRACE") != nullptr;
    static const int tick_blend = [] {
        const char* v = std::getenv("PT_TICK_BLEND");
        return !v ? 2 : std::strcmp(v, "camera") == 0 ? 1 : v[0] == '0' ? 0 : 2;
    }();
    bool scripted_camera = false;
    uint64_t last_render_frame = 0;
    uint64_t reflection_readbacks = 0;
    auto take_reflection_readback = [&](pt::game::Game& target) {
        const pt::RenderStats& stats = app.scene.Stats();
        if (stats.reflection_readbacks != reflection_readbacks) {
            reflection_readbacks = stats.reflection_readbacks;
            target.SetHandyReflectionReadback(stats.reflection_readback);
        }
    };
    auto toggle_settings = [&]() {
        show_settings = !show_settings;
        if (show_settings && !game.Paused()) {
            game.SetPaused(true);
            paused_by_settings = true;
        } else if (!show_settings && paused_by_settings) {
            game.SetPaused(false);
            paused_by_settings = false;
        }
    };
    constexpr float kFreecamRange = 15.0f;
    bool freecam = false;
    pt::Camera freecam_camera;
    glm::vec3 freecam_origin(0.0f);
    bool photo_mode = false;
    bool photo_paused = false;
    bool photo_had_freecam = false;
    pt::Camera photo_saved_camera;
    pt::RenderToggles photo_saved_toggles;
    float photo_saved_grain = 0.0f;
    pt::Camera photo_start_camera;
    pt::game::PhotoPanel photo_panel;
    bool photo_overlay = true;
    bool photo_shot = false;
    std::string photo_status;
    float photo_status_time = 0.0f;
    auto start_freecam = [&]() {
        if (freecam) return;
        freecam = true;
        app.freecam_active = true;
        freecam_camera = game.ViewCamera();
        freecam_camera.roll = 0.0f;
        freecam_origin = freecam_camera.position;
        game.SetCameraOverride(freecam_camera);
        game.SetShowBody(true);
        game.SetDetachedView(true);
        pt::LogInfo("extras: free camera on at ({:.2f} {:.2f} {:.2f})", freecam_origin.x, freecam_origin.y, freecam_origin.z);
    };
    auto stop_freecam_only = [&]() {
        if (!freecam) return;
        freecam = false;
        app.freecam_active = false;
        game.SetCameraOverride(std::nullopt);
        game.SetShowBody(false);
        game.SetDetachedView(false);
        pt::LogInfo("extras: free camera off");
    };
    auto stop_photo = [&]() {
        if (!photo_mode) return;
        photo_mode = false;
        app.photo_active = false;
        hide_game_ui = false;
        photo_shot = false;
        photo_view = {};
        app.scene.toggles = photo_saved_toggles;
        app.scene.graphics.film_grain = photo_saved_grain;
        game.SetShowBody(true);
        if (photo_paused) {
            photo_paused = false;
            game.SetPaused(false);
            if (game.Audio()) game.Audio()->PostEvent("Resume_All", nullptr);
        }
        if (photo_had_freecam) {
            freecam_camera = photo_saved_camera;
            game.SetCameraOverride(freecam_camera);
        } else {
            stop_freecam_only();
        }
        pt::LogInfo("extras: photo mode off");
    };
    auto stop_freecam = [&]() {
        stop_photo();
        stop_freecam_only();
    };
    auto start_photo = [&]() {
        if (photo_mode) return;
        photo_had_freecam = freecam;
        start_freecam();
        if (!photo_had_freecam) {
            const pt::game::Player& player = game.GetPlayer();
            glm::vec3 facing = player.CameraForward();
            facing.y = 0.0f;
            if (glm::length(facing) > 1e-4f) {
                facing = glm::normalize(facing);
                const glm::vec3 eye = player.Eye();
                float distance = 1.1f;
                pt::RayHit hit;
                if (game.Collision().Raycast(eye, facing, distance + 0.2f, hit)) {
                    distance = std::clamp(hit.distance - 0.2f, 0.35f, distance);
                }
                freecam_camera.position = eye + facing * distance;
                freecam_camera.yaw = std::atan2(facing.x, facing.z);
                freecam_camera.pitch = 0.0f;
                freecam_camera.roll = 0.0f;
                freecam_origin = freecam_camera.position;
                game.SetCameraOverride(freecam_camera);
            }
        }
        photo_saved_camera = freecam_camera;
        photo_mode = true;
        app.photo_active = true;
        hide_game_ui = true;
        photo_start_camera = freecam_camera;
        photo_overlay = true;
        photo_shot = false;
        photo_status.clear();
        photo_status_time = 0.0f;
        photo_saved_toggles = app.scene.toggles;
        photo_saved_grain = app.scene.graphics.film_grain;
        pt::game::PhotoSettings settings;
        settings.fov = std::clamp(static_cast<int>(std::lround(glm::degrees(freecam_camera.fov_y) / 5.0f)) * 5, 20, 100);
        settings.roll = std::clamp(static_cast<int>(std::lround(glm::degrees(freecam_camera.roll) / 5.0f)) * 5, -45, 45);
        settings.depth_of_field = app.scene.toggles.depth_of_field;
        settings.bloom = app.scene.toggles.bloom;
        settings.lens = app.scene.toggles.distortion;
        settings.grain = app.scene.toggles.film_grain;
        settings.grading = app.scene.toggles.color_lut;
        settings.letterbox = std::clamp(app.settings.display.letterbox, 0, 2);
        photo_panel.Open(settings);
        if (!game.Paused()) {
            game.SetPaused(true);
            photo_paused = true;
            if (game.Audio()) game.Audio()->PostEvent("Pause_All", nullptr);
        }
        pt::LogInfo("extras: photo mode on");
    };
    struct TheaterView {
        std::unique_ptr<pt::game::ArchiveTheater> theater;
        pt::game::RenderSceneBuilder scene_builder;
        pt::game::VfxScene vfx;
    };
    std::unique_ptr<TheaterView> theater;
    bool theater_paused = false;
    auto start_theater = [&](const std::string& id) {
        if (id == "-") {
            if (theater) theater->theater->Stop();
            return;
        }
        const pt::game::ArchiveEntry* entry = pt::game::FindArchiveEntry(id);
        if (!entry || theater || !ui_ready) {
            pt::LogWarn("archive: {} cannot open{}", id, entry ? "" : " (no such entry)");
            return;
        }
        stop_freecam();
        pt::game::ArchiveTheater::Settings settings;
        settings.sound = sound.Ready();
        settings.open_device = sound.Ready() && sound.System().DeviceOpen();
        settings.volume = app.settings.audio.volume;
        settings.demo_rate = options.demo_rate;
        settings.options = game.Options();
        if (!options.make_museum_previews.empty()) settings.model_ev = kMuseumPreviewModelEv;
        auto view = std::make_unique<TheaterView>();
        view->theater = std::make_unique<pt::game::ArchiveTheater>(vfs, *app.models, *entry, settings);
        if (!view->theater->Start()) {
            return;
        }
        if (!ui.MenuOpen() && !game.Paused()) {
            game.SetPaused(true);
            theater_paused = true;
            if (game.Audio()) game.Audio()->PostEvent("Pause_All", nullptr);
        }
        const int language = std::clamp(game.Options().subtitle_language, 0, 11);
        ui.SetMenuSuspended(true);
        ui.EnterTheater(view->theater->Sandbox());
        if (options.make_museum_previews.empty()) {
            ui.SetTheaterHint(pt::game::ArchiveLabel(*entry, language),
                              std::string(pt::game::PcText(entry->media == pt::game::ArchiveMedia::Model ? "pc_archive_model_hint" : "pc_archive_demo_hint",
                                                           language)));
        }
        theater = std::move(view);
        game.SetArchiveTheaterActive(true);
        pt::LogInfo("archive: theater opens {}", id);
    };
    auto end_theater = [&]() {
        if (!theater) return;
        vkDeviceWaitIdle(app.renderer.Context().device);
        theater.reset();
        ui.SetTheaterHint({}, {});
        ui.SetMenuSuspended(false);
        ui.LeaveTheater(game);
        if (theater_paused) {
            theater_paused = false;
            game.SetPaused(false);
            if (game.Audio()) game.Audio()->PostEvent("Resume_All", nullptr);
        }
        game.SetArchiveTheaterActive(false);
        pt::LogInfo("archive: theater ended");
    };
    std::unique_ptr<pt::game::VrPlay> vr;
    if (app.xr) {
        vr = std::make_unique<pt::game::VrPlay>(*app.xr, app.settings.vr);
    }
    bool frozen = false;
    bool was_focused = true;
    float applied_volume = -1.0f;
    while (running) {
        bool visible = PumpEvents(app, &input, running) || vr != nullptr;
        if (vr) {
            vr->BeginLoop(running);
            if (!vr->Host().SessionRunning()) {
                visible = false;
                SDL_Delay(10);
            }
        }
        PollEnhancedTextures(app);
        const std::optional<bool> forced_focus = script.ForcedFocus(frame);
        if (app.window || forced_focus || vr) {
            const bool focused = forced_focus ? *forced_focus
                                 : vr         ? !vr->Host().FocusLost()
                                              : visible && (SDL_GetWindowFlags(app.window) & SDL_WINDOW_INPUT_FOCUS);
            const bool pause = app.settings.display.pause_on_focus_loss || forced_focus;
            if (was_focused && !focused && pause && ui_ready && !ui.MenuOpen() && !game.Status().IsSet("S_DISABLE_GAME_PAUSE")) {
                stop_freecam();
                ui.OpenMenu(game, false);
                pt::LogInfo("focus: window in the background, pause menu opened");
            }
            was_focused = focused;
            const bool freeze = !visible || (!focused && pause && !(ui_ready && ui.MenuOpen()));
            if (freeze != frozen) {
                frozen = freeze;
                if (sound.Ready()) {
                    sound.System().SetFrozen(frozen);
                }
                pt::LogInfo("focus: game {} at frame {}, audio clock {}", frozen ? "frozen in the background" : "resumed", frame,
                            sound.Ready() ? sound.System().RenderedFrames() : 0);
                last_ticks = SDL_GetTicksNS();
            }
            const float volume = !focused && app.settings.display.mute_in_background ? 0.0f : app.settings.audio.volume;
            if (sound.Ready() && volume != applied_volume) {
                sound.System().SetMasterVolume(volume);
                applied_volume = volume;
            }
        }
        if (!visible || frozen) {
            input.SetRumble(0, 0);
            microphone.Close();
            if (recognizer) recognizer->Reset();
            app.microphone_monitor = false;
            if (app.window && visible) {
                SDL_Delay(16);
            }
            if (++frame >= static_cast<uint64_t>(options.frames) && options.headless) {
                running = false;
            }
            pt::LogSetTick(frame);
            continue;
        }
        if (!app.window && options.virtual_pads) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                input.ProcessEvent(event);
            }
        }
        float dt = options.display_rate > 0.0f ? 1.0f / options.display_rate : 1.0f / options.tick_rate;
        if (app.window) {
            const uint64_t now = SDL_GetTicksNS();
            dt = std::clamp(static_cast<float>(now - last_ticks) * 1e-9f, 0.0001f, 0.1f);
            last_ticks = now;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            static bool tab_was_down = false;
            if (options.dev_ui && keys[SDL_SCANCODE_TAB] && !tab_was_down) {
                show_debug = !show_debug;
            }
            tab_was_down = keys[SDL_SCANCODE_TAB];
            const bool photo_pointer = photo_mode && photo_overlay && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK);
            const bool want_capture = !show_debug && !show_settings && !(ui_ready && ui.MenuOpen()) && !photo_pointer && !vr &&
                                      (SDL_GetWindowFlags(app.window) & SDL_WINDOW_INPUT_FOCUS);
            if (want_capture != mouse_captured) {
                SDL_SetWindowRelativeMouseMode(app.window, want_capture);
                mouse_captured = want_capture;
            }
        }
        const bool keyboard_free = app.window && !show_settings && !(show_debug && ImGui::GetIO().WantCaptureKeyboard);
        const bool menu_open = ui_ready && ui.MenuOpen();
        const pt::MouseUse mouse_use = mouse_captured || (!app.window && !menu_open) ? pt::MouseUse::Look
                                       : menu_open && !show_settings                   ? pt::MouseUse::Menu
                                                                                       : pt::MouseUse::None;
        pt::InputState polled = pads ? input.Poll(keyboard_free, mouse_use, !show_settings) : pt::InputState{};
        if (options.forced_prompts) {
            polled.prompts = *options.forced_prompts;
        }
        if (vr) {
            vr->ApplyControls(game, polled, ui_ready && ui.MenuOpen(), vr->ScreenMode(game), dt);
        }
        if (polled.pc_settings && !freecam) {
            if (options.dev_ui && app.window) {
                toggle_settings();
            } else if (ui_ready) {
                ui.OpenPcSettings();
            }
        }
        pending_input.left_stick = polled.left_stick;
        pending_input.right_stick = polled.right_stick;
        pending_input.held = polled.held;
        pending_input.raw_held = polled.raw_held;
        pending_input.mouse_look += polled.mouse_look;
        pending_input.pressed |= polled.pressed;
        pending_input.raw_pressed |= polled.raw_pressed;
        pending_input.pause = pending_input.pause || polled.pause;
        pending_input.confirm = pending_input.confirm || polled.confirm;
        pending_input.cancel = pending_input.cancel || polled.cancel;
        pending_input.any_button = pending_input.any_button || polled.any_button;
        pending_input.from_gamepad = polled.from_gamepad;
        pending_input.prompts = polled.prompts;
        pending_input.vr_look = polled.vr_look;
        pending_input.vr_look_angles = polled.vr_look_angles;
        prompt_textures.Update(polled.prompts);
        pending_input.click = pending_input.click || polled.click;
        pending_input.right_click = pending_input.right_click || polled.right_click;
        pending_input.house_pressed = pending_input.house_pressed || polled.house_pressed;
        pending_input.pointer_valid = false;
        if (app.window && !mouse_captured) {
            float x = 0.0f;
            float y = 0.0f;
            SDL_GetMouseState(&x, &y);
            pending_input.pointer = glm::vec2(x, y) * SDL_GetWindowPixelDensity(app.window);
            pending_input.pointer_valid = true;
        }
        if (const std::vector<int> set = game.TakePhotoSettingsRequest(); set.size() == 7 && photo_mode) {
            pt::game::PhotoSettings& photo = photo_panel.Settings();
            photo.fov = set[0];
            photo.roll = set[1];
            photo.focus = set[2];
            photo.aperture = set[3];
            photo.letterbox = set[4];
            photo.depth_of_field = set[2] != 0 || set[3] != 0;
            photo.exposure = pt::game::PhotoSettings::kExposureZero + set[5];
            photo.body = set[6] != 0;
        }
        if (const auto shot = game.TakePhotoCameraRequest(); shot && photo_mode) {
            const pt::game::Player& player = game.GetPlayer();
            glm::vec3 facing = player.CameraForward();
            facing.y = 0.0f;
            facing = glm::length(facing) > 1e-4f ? glm::normalize(facing) : glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 right = glm::normalize(glm::cross(facing, glm::vec3(0.0f, 1.0f, 0.0f)));
            const glm::vec3 eye = player.Eye();
            freecam_camera.position = eye + right * shot->x + glm::vec3(0.0f, shot->y, 0.0f) + facing * shot->z;
            const glm::vec2 aim = game.PhotoTarget();
            const glm::vec3 look = glm::normalize(eye + glm::vec3(0.0f, shot->w, 0.0f) + right * aim.x + facing * aim.y - freecam_camera.position);
            freecam_camera.yaw = std::atan2(-look.x, -look.z);
            freecam_camera.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
            freecam_camera.roll = 0.0f;
            freecam_origin = freecam_camera.position;
            game.SetCameraOverride(freecam_camera);
        }
        if (const auto view = game.TakePhotoViewRequest(); view && photo_mode) {
            const glm::vec3 d = view->second - view->first;
            const glm::vec3 look = glm::length(d) > 1e-4f ? glm::normalize(d) : glm::vec3(0.0f, 0.0f, -1.0f);
            freecam_camera.position = view->first;
            freecam_camera.yaw = std::atan2(-look.x, -look.z);
            freecam_camera.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
            freecam_camera.roll = 0.0f;
            freecam_origin = freecam_camera.position;
            game.SetCameraOverride(freecam_camera);
            pt::LogInfo("extras: photo camera at ({:.2f} {:.2f} {:.2f}) looking at ({:.2f} {:.2f} {:.2f})", view->first.x, view->first.y,
                        view->first.z, view->second.x, view->second.y, view->second.z);
        }
        if (const int request = game.TakeFreeCameraRequest(); request != 0) {
            if (request == 1) {
                start_freecam();
            } else {
                stop_freecam();
            }
        }
        if (const int request = game.TakePhotoModeRequest(); request != 0) {
            if (request == 1 || request == 2) {
                start_photo();
                photo_overlay = request == 1;
            } else {
                stop_photo();
            }
        }
        if (app.window) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            static bool f6_was_down = false;
            static bool f7_was_down = false;
            static bool h_was_down = false;
            static bool p_was_down = false;
            const bool usable = keyboard_free && !show_settings && !(ui_ready && ui.MenuOpen()) && app.extras_request == 0 && !vr;
            const bool f6 = keys[SDL_SCANCODE_F6] && !f6_was_down && usable;
            const bool f7 = keys[SDL_SCANCODE_F7] && !f7_was_down && usable;
            const bool h = keys[SDL_SCANCODE_H] && !h_was_down && usable;
            const bool p = keys[SDL_SCANCODE_P] && !p_was_down && usable;
            f6_was_down = keys[SDL_SCANCODE_F6];
            f7_was_down = keys[SDL_SCANCODE_F7];
            h_was_down = keys[SDL_SCANCODE_H];
            p_was_down = keys[SDL_SCANCODE_P];
            if (freecam && polled.pause) {
                pending_input.pause = false;
                if (photo_mode) {
                    stop_photo();
                } else {
                    stop_freecam();
                }
            } else if (f7) {
                if (photo_mode) {
                    stop_photo();
                } else {
                    start_photo();
                }
            } else if (f6 && !photo_mode) {
                if (freecam) {
                    stop_freecam();
                } else {
                    start_freecam();
                }
            }
            if (photo_mode) {
                if (p || (polled.raw_pressed & pt::kRawSquare)) photo_shot = true;
                if (h || (polled.raw_pressed & pt::kRawTriangle)) photo_overlay = !photo_overlay;
                static bool return_was_down = false;
                const bool return_key = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];
                const bool return_pressed = return_key && !return_was_down && usable;
                return_was_down = return_key;
                const bool keyboard_cross = keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_E] || return_key ||
                                            (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK);
                if (photo_overlay && usable) {
                    pt::game::PhotoPanel::Input panel_input;
                    panel_input.held_dirs = polled.raw_held & (pt::kRawUp | pt::kRawDown | pt::kRawLeft | pt::kRawRight);
                    panel_input.accept = return_pressed || ((polled.raw_pressed & pt::kRawCross) && !keyboard_cross);
                    const auto action = photo_panel.Update(game, panel_input, dt);
                    if (action == pt::game::PhotoPanel::Action::TakePhoto) {
                        photo_shot = true;
                    } else if (action == pt::game::PhotoPanel::Action::ResetCamera) {
                        freecam_camera.position = photo_start_camera.position;
                        freecam_camera.yaw = photo_start_camera.yaw;
                        freecam_camera.pitch = photo_start_camera.pitch;
                        game.SetCameraOverride(freecam_camera);
                    }
                }
                if (usable && ((polled.raw_pressed & pt::kRawCircle) || polled.cancel)) {
                    stop_photo();
                }
            }
        }
        const float step = 1.0f / options.tick_rate;
        accumulator = paced ? std::min(accumulator + dt, 0.25f) : step;
        const auto render_offline = [&](uint32_t samples) {
            std::vector<float>& target = options.audio_capture.empty() ? offline_audio : captured;
            const size_t base = options.audio_capture.empty() ? 0 : captured.size();
            target.resize(base + samples * 2);
            sound.System().RenderOffline(target.data() + base, samples);
            if (theater && theater->theater->Sound() && theater->theater->Sound()->Ready()) {
                static std::vector<float> mix;
                mix.assign(static_cast<size_t>(samples) * 2, 0.0f);
                theater->theater->Sound()->System().RenderOffline(mix.data(), samples);
                for (size_t i = 0; i < mix.size(); ++i) target[base + i] += mix[i];
            }
        };
        const bool device_periods = options.audio_offline && options.audio_period > 0 && options.display_rate > 0.0f && sound.Ready();
        if (device_periods) {
            audio_device_time += dt;
            while (static_cast<double>(audio_device_frames + options.audio_period) <= audio_device_time * 48000.0) {
                render_offline(options.audio_period);
                audio_device_frames += options.audio_period;
            }
        }
        int ticks = 0;
        while (accumulator >= step && ticks < 8) {
            tick_camera_from = game.ViewCamera();
            if (paced && !(accumulator - step >= step && ticks + 1 < 8)) {
                tick_state.Capture(game, vfx_scene);
            }
            pt::InputState state = pending_input;
            script.Apply(frame, game, state);
            if (freecam) {
                state = FreecamInput(state);
            }
            motion_camera_from = game.ViewCamera();
            scripted_camera = motion_camera_from.position != tick_camera_from.position || motion_camera_from.yaw != tick_camera_from.yaw ||
                              motion_camera_from.pitch != tick_camera_from.pitch || motion_camera_from.roll != tick_camera_from.roll;
            static const char* tick_csv = std::getenv("PT_TICK_CSV");
            const auto tick_t0 = std::chrono::steady_clock::now();
            if (theater) {
                const bool model = theater->theater->ModelView();
                if (state.pause || state.cancel || (state.confirm && !model)) theater->theater->Stop();
                theater->theater->Update(step, state);
            } else {
                game.Update(step, state);
            }
            const auto tick_t1 = std::chrono::steady_clock::now();
            if (theater) {
                theater->vfx.Update(theater->theater->Sandbox(), step);
            } else {
                vfx_scene.Update(game, step);
            }
            const auto tick_t2 = std::chrono::steady_clock::now();
            ui.Update(theater ? theater->theater->Sandbox() : game, state, step);
            const auto tick_t3 = std::chrono::steady_clock::now();
            if (options.audio_offline && sound.Ready() && !device_periods) {
                render_offline(static_cast<uint32_t>(48000.0f * step + 0.5f));
            }
            if (tick_csv) {
                static std::vector<std::array<float, 4>> ticks_trace;
                static const bool registered = [] {
                    std::atexit([] {
                        if (FILE* f = std::fopen(std::getenv("PT_TICK_CSV"), "w")) {
                            std::fprintf(f, "tick,game_ms,vfx_ms,ui_ms,audio_ms\n");
                            for (size_t i = 0; i < ticks_trace.size(); ++i) {
                                std::fprintf(f, "%zu,%.4f,%.4f,%.4f,%.4f\n", i, ticks_trace[i][0], ticks_trace[i][1], ticks_trace[i][2], ticks_trace[i][3]);
                            }
                            std::fclose(f);
                        }
                    });
                    return true;
                }();
                (void)registered;
                using ms = std::chrono::duration<float, std::milli>;
                const auto tick_t4 = std::chrono::steady_clock::now();
                ticks_trace.push_back({ms(tick_t1 - tick_t0).count(), ms(tick_t2 - tick_t1).count(), ms(tick_t3 - tick_t2).count(),
                                       ms(tick_t4 - tick_t3).count()});
            }
            pending_input.mouse_look = glm::vec2(0.0f);
            pending_input.pressed = 0;
            pending_input.raw_pressed = 0;
            pending_input.pause = pending_input.confirm = pending_input.cancel = pending_input.any_button = false;
            pending_input.click = pending_input.right_click = pending_input.house_pressed = false;
            accumulator -= step;
            ++ticks;
        }
        if (ui_ready) ui.AdvancePresentation(paced ? dt : step);
        if (!app.update_polled && app.updates.Done()) {
            app.update_polled = true;
            if (const auto newer = app.updates.Newer()) {
                pt::LogInfo("update: version {} available at {}{}{}", newer->version, newer->url, newer->notes.empty() ? "" : ": ", newer->notes);
                if (ui_ready) ui.ShowUpdateNotice(UpdateNoticeKey(newer->version));
            }
        }
        if (ui_ready) ui.HoldUpdateNotice(photo_mode || vr != nullptr);
        game.Speedrun().Poll();
        if (game.SavesEnabled() && (game.BrowseUnlockGeneration() != unlocks_seen || game.ArchiveGeneration() != archive_seen)) {
            unlocks_seen = game.BrowseUnlockGeneration();
            archive_seen = game.ArchiveGeneration();
            app.settings.progress.loops_reached = game.BrowseReached();
            app.settings.progress.game_finished = game.GameFinished();
            std::string keys;
            for (const std::string& key : game.ArchiveSeen()) keys += (keys.empty() ? "" : ",") + key;
            app.settings.progress.archive = keys;
            if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
        }
        game.SetArchiveTheaterShowing(theater && theater->theater->Showing());
        if (theater && theater->theater->Finished()) {
            end_theater();
        }
        if (std::string id = std::exchange(app.archive_request, std::string()); !id.empty()) {
            start_theater(id);
        } else if (std::string script_id = game.TakeArchiveRequest(); !script_id.empty()) {
            start_theater(script_id);
        }
        if (app.extras_request != 0 && ui_ready) {
            if (ui.MenuOpen()) {
                ui.CloseMenu();
            } else {
                const int request = std::exchange(app.extras_request, 0);
                if (request == 1) {
                    if (freecam) {
                        stop_freecam();
                    } else {
                        start_freecam();
                    }
                } else if (request == 2) {
                    if (photo_mode) {
                        stop_photo();
                    } else {
                        start_photo();
                    }
                } else if (request == 3) {
                    game.LeaveStreetWalk();
                }
            }
        } else if (freecam && ui_ready && ui.MenuOpen()) {
            stop_freecam();
        }
        if (freecam) {
            const bool* keys = app.window ? SDL_GetKeyboardState(nullptr) : nullptr;
            const auto key = [&](SDL_Scancode code) { return keys && keyboard_free && keys[code]; };
            const bool fast = key(SDL_SCANCODE_LSHIFT) || key(SDL_SCANCODE_RSHIFT) || (polled.raw_held & pt::kRawL3);
            float rise = 0.0f;
            if (key(SDL_SCANCODE_SPACE) || (polled.raw_held & pt::kRawR2)) rise += 1.0f;
            if (key(SDL_SCANCODE_LCTRL) || key(SDL_SCANCODE_RCTRL) || (polled.raw_held & pt::kRawL2)) rise -= 1.0f;
            const glm::vec3 move = freecam_camera.Forward() * polled.left_stick.y + freecam_camera.Right() * polled.left_stick.x +
                                   glm::vec3(0.0f, rise, 0.0f);
            const float speed_scale = photo_mode ? photo_panel.Settings().SpeedScale() : 1.0f;
            freecam_camera.position += move * ((fast ? 6.0f : 1.5f) * speed_scale * dt);
            const glm::vec3 offset = freecam_camera.position - freecam_origin;
            if (glm::length(offset) > kFreecamRange) {
                freecam_camera.position = freecam_origin + glm::normalize(offset) * kFreecamRange;
            }
            const glm::vec2 look = polled.mouse_look + polled.right_stick * (2.5f * dt);
            freecam_camera.yaw -= look.x * (game.Options().invert_x ? -1.0f : 1.0f);
            freecam_camera.pitch = std::clamp(freecam_camera.pitch - look.y * (game.Options().invert_y ? -1.0f : 1.0f), -1.55f, 1.55f);
            if (photo_mode) {
                const pt::game::PhotoSettings& photo = photo_panel.Settings();
                freecam_camera.fov_y = glm::radians(static_cast<float>(photo.fov));
                freecam_camera.roll = glm::radians(static_cast<float>(photo.roll));
                app.scene.toggles.depth_of_field = photo.depth_of_field;
                app.scene.toggles.bloom = photo.bloom;
                app.scene.toggles.distortion = photo.lens;
                app.scene.toggles.film_grain = photo.grain;
                app.scene.toggles.color_lut = photo.grading;
                app.scene.graphics.film_grain = photo.grain && photo_saved_grain <= 0.0f ? 1.0f : photo_saved_grain;
                game.SetShowBody(photo.body);
            }
            game.SetCameraOverride(freecam_camera);
        }
        if (pads) {
            const pt::audio::MotionLevels motion = sound.Ready() ? sound.System().Motion() : pt::audio::MotionLevels{};
            input.SetRumble(motion.large_motor, motion.small_motor);
        }
        if (vr && sound.Ready()) {
            const pt::audio::MotionLevels motion = sound.System().Motion();
            vr->Rumble(motion.large_motor, motion.small_motor);
        }
        for (const std::string& snippet : options.lua_snippets) {
            const std::string prefix = std::to_string(frame) + ":";
            if (snippet.starts_with(prefix)) {
                const std::string code = snippet.substr(prefix.size());
                game.Scripts().Vm().RunChunk("debug", std::span(reinterpret_cast<const uint8_t*>(code.data()), code.size()));
            }
        }

        if (app.window && voice_key != SDL_SCANCODE_UNKNOWN) {
            const bool down = SDL_GetKeyboardState(nullptr)[voice_key];
            if (down && !voice_key_down && game.VoiceListening() && !game.Paused() && !app.microphone_test) {
                game.OnVoiceKeyword("jack");
            }
            voice_key_down = down;
        }
        if (!ui.MenuOpen() || ui.Menu().CurrentPage() != pt::game::OptionsMenu::Page::Pc)
            app.microphone_test = app.microphone_monitor = false;
        if (active_microphone_device != app.settings.voice.device || previous_microphone_test != app.microphone_test) {
            microphone.Close();
            microphone_failed = false;
            active_microphone_device = app.settings.voice.device;
            previous_microphone_test = app.microphone_test;
        }
        const bool voice_file = !voice_input.empty();
        const bool hearing = (app.window || voice_file) && ((game.VoiceListening() && !game.Paused()) || app.microphone_test);
        if ((app.window || voice_file) && (game.VoiceListening() || app.microphone_test)) {
            if (!recognizer) {
                recognizer = std::make_unique<pt::VoiceRecognizer>();
                recognizer->Init(pt::ResourceDir("voice", PT_VOICE_MODEL_DIR), "jack");
            }
        } else if (recognizer) {
            microphone.Close();
            recognizer.reset();
        }
        if (hearing && recognizer) {
            if (!microphone.IsOpen() && !microphone_failed && !voice_file) {
                microphone_failed = !microphone.Open(pt::VoiceRecognizer::kSampleRate, app.settings.voice.device);
                recognizer->Reset();
                if (microphone_failed) pt::LogError("voice: no microphone to listen with");
            }
            const pt::VoiceRecognizer::State state = recognizer->GetState();
            app.microphone_status = microphone_failed || state == pt::VoiceRecognizer::State::Failed ? "pc_mic_unavailable"
                                    : state == pt::VoiceRecognizer::State::Ready                    ? "pc_mic_say_jack"
                                                                                                    : "pc_mic_waiting";
            if (voice_file) {
                voice_input_due += dt * pt::VoiceRecognizer::kSampleRate;
                mic_samples.clear();
                for (; voice_input_due >= 1.0; voice_input_due -= 1.0) {
                    mic_samples.push_back(voice_input[voice_input_at]);
                    voice_input_at = (voice_input_at + 1) % voice_input.size();
                }
            }
            if (microphone.IsOpen() || voice_file) {
                if (!microphone.SetMonitor(app.microphone_test && app.microphone_monitor)) app.microphone_monitor = false;
                if (!voice_file) microphone.Read(mic_samples);
                float loudest = -80.0f;
                for (size_t at = 0; at + 160 <= mic_samples.size(); at += 160) {
                    double energy = 0.0;
                    for (size_t i = at; i < at + 160; ++i) energy += static_cast<double>(mic_samples[i]) * mic_samples[i];
                    loudest = std::max(loudest, static_cast<float>(10.0 * std::log10(std::max(energy / 160.0, 1.0) / (32768.0 * 32768.0))));
                }
                app.microphone_db = std::max({-80.0f, loudest, app.microphone_db - 60.0f * dt});
                bool heard = recognizer->Feed(mic_samples);
                if (voice_file) heard = recognizer->Drain() || heard;
                if (heard && !app.microphone_test) game.OnVoiceKeyword(recognizer->Keyword());
                app.microphone_hypothesis = recognizer->LastHypothesis();
                if (app.microphone_hypothesis.size() > 32) {
                    size_t cut = 30;
                    while (cut > 0 && (static_cast<unsigned char>(app.microphone_hypothesis[cut]) & 0xC0) == 0x80) --cut;
                    app.microphone_hypothesis = app.microphone_hypothesis.substr(0, cut) + "...";
                }
            }
        } else {
            microphone_failed = false;
            if (microphone.IsOpen()) {
                microphone.Close();
                if (recognizer) recognizer->Reset();
            }
        }

        pt::game::Game& view = theater ? theater->theater->Sandbox() : game;
        pt::game::RenderSceneBuilder& view_builder = theater ? theater->scene_builder : scene_builder;
        pt::game::VfxScene& view_vfx = theater ? theater->vfx : vfx_scene;
        const pt::game::ScreenEffects& fx = view.Effects();
        app.renderer.fade[0] = fx.FadeShown().r;
        app.renderer.fade[1] = fx.FadeShown().g;
        app.renderer.fade[2] = fx.FadeShown().b;
        app.renderer.fade[3] = ui_ready && !(vr && !vr->ScreenMode(game)) ? 0.0f : fx.FadeShown().a;
        app.renderer.output_brightness = game.Options().BrightnessValue();
        draw_items.clear();
        pt::Camera camera = game.ViewCamera();
        if (theater) {
            if (theater->theater->Showing()) {
                theater->theater->CollectDraws(draw_items);
            } else {
                app.renderer.fade[0] = app.renderer.fade[1] = app.renderer.fade[2] = 0.0f;
                app.renderer.fade[3] = 1.0f;
            }
            camera = theater->theater->ViewCamera();
        } else {
            game.CollectDraws(draw_items);
        }
        float blend = 1.0f;
        if (paced && !theater) {
            const float t = std::clamp(accumulator / step, 0.0f, 1.0f);
            bool cut = false;
            if (tick_blend >= 1) {
                camera = BlendCameras(tick_camera_from, camera, t, &cut);
            }
            blend = cut || tick_blend < 2 ? 1.0f : t;
            BlendDraws(tick_state, draw_items, blend, blended_skins);
            tick_state.lights.t = blend;
        }
        const pt::game::TickBlend* light_blend = paced && !theater ? &tick_state.lights : nullptr;
        if (paced && !theater && !freecam && polled.prompts.device == pt::PromptDevice::Keyboard && !scripted_camera &&
            !game.Demos().ControlsPlayer() && !game.Paused() && !(game.GetPlayer().locks.Mask('B') & 2)) {
            const pt::Camera unturned = camera;
            camera = pt::game::RenderMouseLook(camera, game.ViewCamera(), pending_input.mouse_look,
                                              game.Options().invert_x, game.Options().invert_y);
            camera = game.ThirdPersonTurn(unturned, camera);
            camera = game.PeepholeTurn(camera);
        }
        if (freecam) {
            camera = freecam_camera;
        }

        if (paced && render_trace) {
            LogRenderTrace(frame, ticks, blend, camera, game, draw_items);
        }

        if (app.window) {
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags = show_settings ? io.ConfigFlags | ImGuiConfigFlags_NavEnableGamepad : io.ConfigFlags & ~ImGuiConfigFlags_NavEnableGamepad;
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            if (show_debug) {
                pt::game::DrawGameDebugPanel(game);
                if (ImGui::Begin("View")) {
                    ImGui::SliderFloat("exposure", &app.renderer.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                    ImGui::Text("%zu draws, Tab hides this panel", draw_items.size());
                }
                ImGui::End();
                app.scene.DrawDebugUi();
            }
            if (show_settings) {
                DrawSettingsWindow(app, input, sound, game);
            }
            ImGui::Render();
        }
        if (photo_mode) {
            const int language = std::clamp(game.Options().subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1);
            photo_status_time = std::max(0.0f, photo_status_time - dt);
            if (photo_status_time <= 0.0f) photo_status.clear();
            photo_view = photo_panel.View(language, photo_status);
            photo_view.panel = photo_overlay && !photo_shot;
            if (photo_shot) {
                const std::filesystem::path path = PhotoPath();
                game.RequestScreenshot(path.string());
                std::string saved(pt::game::PcText("pc_photo_saved", language));
                if (const size_t at = saved.find("{file}"); at != std::string::npos) saved.replace(at, 6, path.filename().string());
                photo_status = saved;
                photo_status_time = 4.0f;
                photo_shot = false;
            }
        }
        const std::vector<std::string> shots = game.TakeScreenshotRequests();
        const bool render = app.window || options.render_all || frame + 1 >= static_cast<uint64_t>(options.frames) || !shots.empty();
        ++frame;
        pt::LogSetTick(frame);
        app.scene.AdvanceTime(dt);
        bool warmed = false;
        if (vr) {
            const bool screen = vr->ScreenMode(game);
            const bool menu_open = ui_ready && ui.MenuOpen();
            const float scene_dt = static_cast<float>(game.Time() - last_scene_time);
            auto begin = [&](bool present) { return app.renderer.BeginFrame(present && app.window) || (present && app.renderer.BeginFrame(false)); };
            auto save_shots = [&](const char* suffix) {
                for (const std::string& shot : shots) {
                    std::filesystem::path path(shot);
                    if (*suffix) path = path.parent_path() / (path.stem().string() + suffix + path.extension().string());
                    app.renderer.SaveScreenshot(path);
                }
            };
            pt::XrTarget screen_target;
            pt::game::VrPlay::Stereo stereo;
            if (screen && vr->PrepareScreen(screen_target)) {
                app.renderer.SetRenderExtent(pt::Renderer::kHudExtent);
                if (begin(true)) {
                    scene_builder.Build(game, camera, scene_dt, lighting, light_blend);
                    last_scene_time = game.Time();
                    vfx_scene.Prepare(game, camera, 16.0f / 9.0f, lighting, vfx_pass, blend);
                    app.scene.SetVrEye(-1);
                    app.scene.Render(camera, draw_items, lighting, dt);
                    ui.SetVrHud(false);
                    pt::XrFrame xr_frame;
                    xr_frame.eye = &screen_target;
                    xr_frame.overlay_on_frame = true;
                    app.renderer.SetXrFrame(xr_frame);
                    app.renderer.EndFrame(app.window != nullptr);
                    take_reflection_readback(game);
                    save_shots("");
                }
                vr->FinishScreen();
                last_render_frame = frame;
            } else if (!screen && vr->PrepareStereo(game, camera, dt, menu_open, stereo)) {
                app.renderer.SetRenderExtent(stereo.render);
                scene_builder.Build(game, stereo.head, scene_dt, lighting, light_blend);
                last_scene_time = game.Time();
                vfx_scene.Prepare(game, stereo.head, static_cast<float>(stereo.render.width) / static_cast<float>(std::max(1u, stereo.render.height)),
                                  lighting, vfx_pass, blend);
                for (int eye = 0; eye < 2; ++eye) {
                    if (!begin(eye == 0)) {
                        continue;
                    }
                    app.scene.SetVrEye(eye);
                    app.scene.Render(stereo.eyes[eye], draw_items, lighting, eye == 0 ? dt : 0.0f);
                    ui.SetVrHud(true);
                    pt::XrFrame xr_frame;
                    xr_frame.eye = &stereo.targets[eye];
                    xr_frame.hud = eye == 0 ? &stereo.hud : nullptr;
                    app.renderer.SetXrFrame(xr_frame);
                    app.renderer.EndFrame(app.window != nullptr && eye == 0);
                    save_shots(eye == 0 ? "_left" : "_right");
                }
                app.scene.SetVrEye(-1);
                ui.SetVrHud(false);
                take_reflection_readback(game);
                vr->FinishStereo(stereo);
                static const bool vr_trace = std::getenv("PT_VR_TRACE") != nullptr;
                if (vr_trace || !shots.empty()) {
                    const pt::Camera& h = stereo.head;
                    pt::LogInfo("vr frame {}: game camera yaw {:.2f} pitch {:.2f} at ({:.3f} {:.3f} {:.3f}), head yaw {:.2f} pitch {:.2f} roll {:.2f} at "
                                "({:.3f} {:.3f} {:.3f}), eyes {:.4f} m apart, render {}x{}",
                                frame, glm::degrees(camera.yaw), glm::degrees(camera.pitch), camera.position.x, camera.position.y, camera.position.z,
                                glm::degrees(h.yaw), glm::degrees(h.pitch), glm::degrees(h.roll), h.position.x, h.position.y, h.position.z,
                                glm::distance(stereo.eyes[0].position, stereo.eyes[1].position), stereo.render.width, stereo.render.height);
                }
                last_render_frame = frame;
            } else {
                vr->EndEmpty();
            }
        }
        if (render && !app.window && !shots.empty() && !vr) {
            if (options.shot_settle && last_render_frame + 1 != frame) {
                app.scene.ResetExposure();
            }
            for (int i = 0; i < options.shot_warmup && app.renderer.BeginFrame(); ++i) {
                warmed = true;
                view_builder.Build(view, camera, 0.0f, lighting, light_blend);
                const VkExtent2D warm_extent = app.renderer.RenderExtent();
                const float warm_aspect = static_cast<float>(warm_extent.width) / static_cast<float>(std::max(1u, warm_extent.height));
                view_vfx.Prepare(view, camera, warm_aspect, lighting, vfx_pass, blend);
                app.scene.SetMotionReference(camera, 1.0f / options.tick_rate, i > 0 || last_render_frame + 1 == frame);
                app.scene.Render(camera, draw_items, lighting, dt);
                take_reflection_readback(view);
                app.renderer.EndFrame(false);
                last_render_frame = frame - 1;
            }
        }
        if (render && !vr) {
            static const char* parts_csv = std::getenv("PT_PARTS_CSV");
            using parts_ms = std::chrono::duration<float, std::milli>;
            const auto part_t0 = std::chrono::steady_clock::now();
            if (!app.renderer.BeginFrame()) {
                continue;
            }
            const auto part_t1 = std::chrono::steady_clock::now();
            view_builder.log_lens_next = !shots.empty();
            view_builder.Build(view, camera, theater ? static_cast<float>(dt) : static_cast<float>(game.Time() - last_scene_time), lighting,
                               light_blend);
            if (!theater) last_scene_time = game.Time();
            if (photo_mode) {
                ApplyPhotoLens(photo_panel.Settings(), game, camera, lighting);
            }
            const auto part_t2 = std::chrono::steady_clock::now();
            const VkExtent2D vfx_extent = app.renderer.RenderExtent();
            view_vfx.Prepare(view, camera, static_cast<float>(vfx_extent.width) / static_cast<float>(std::max(1u, vfx_extent.height)), lighting,
                             vfx_pass, blend);
            const auto part_t3 = std::chrono::steady_clock::now();
            struct PartsTrace {
                std::vector<std::array<float, 7>> rows;
                ~PartsTrace() {
                    if (const char* path = std::getenv("PT_PARTS_CSV"); path && !rows.empty()) {
                        if (FILE* f = std::fopen(path, "w")) {
                            std::fprintf(f, "frame,wait_ms,build_ms,vfx_ms,render_ms,submit_ms,tick,loop_ms\n");
                            for (size_t i = 0; i < rows.size(); ++i) {
                                std::fprintf(f, "%zu,%.4f,%.4f,%.4f,%.4f,%.4f,%.0f,%.4f\n", i, rows[i][0], rows[i][1], rows[i][2], rows[i][3],
                                             rows[i][4], rows[i][5], rows[i][6]);
                            }
                            std::fclose(f);
                        }
                    }
                }
            };
            static PartsTrace parts_trace;
            static std::chrono::steady_clock::time_point parts_last_end{};
            std::array<float, 7> parts_row{parts_ms(part_t1 - part_t0).count(), parts_ms(part_t2 - part_t1).count(),
                                           parts_ms(part_t3 - part_t2).count(), 0.0f, 0.0f, static_cast<float>(frame),
                                           parts_last_end.time_since_epoch().count() ? parts_ms(part_t0 - parts_last_end).count() : 0.0f};
            if (render_trace) {
                LogLightTrace(frame, camera, lighting, view_vfx, blend);
                if (!paced) {
                    LogRenderTrace(frame, ticks, blend, camera, game, draw_items);
                }
            }
            if (!paced) {
                const float demo_scale = view.Demos().CameraParams() ? std::max(view.Demos().time_scale, 1.0e-3f) : 1.0f;
                const pt::Camera& reference = warmed ? camera : options.render_all ? tick_camera_from : scripted_camera ? camera : motion_camera_from;
                app.scene.SetMotionReference(reference, demo_scale / options.tick_rate, last_render_frame + 1 == frame);
            }
            static const bool dump_targets = std::getenv("PT_TARGET_DUMP") != nullptr;
            if (dump_targets && !shots.empty()) {
                app.scene.RequestTargetDump();
            }
            const auto part_t4 = std::chrono::steady_clock::now();
            app.scene.Render(camera, draw_items, lighting, dt);
            take_reflection_readback(view);
            last_render_frame = frame;
            const auto part_t5 = std::chrono::steady_clock::now();
            app.renderer.EndFrame(app.window != nullptr);
            if (parts_csv) {
                parts_row[3] = parts_ms(part_t5 - part_t4).count();
                parts_last_end = std::chrono::steady_clock::now();
                parts_row[4] = parts_ms(parts_last_end - part_t5).count();
                parts_trace.rows.push_back(parts_row);
            }
            for (const std::string& shot : shots) {
                app.renderer.SaveScreenshot(shot);
                if (dump_targets) {
                    const std::filesystem::path path(shot);
                    app.scene.DumpTargets((path.parent_path() / path.stem()).string());
                }
                const pt::RenderStats& stats = app.scene.Stats();
                pt::LogInfo("screenshot ev {:.3f} (exposure {:.6f}, metered luminance {:.5f}), {} lights, {} shadow views, camera ({:.3f} {:.3f} {:.3f}) "
                            "yaw {:.2f} pitch {:.2f} frame {}: {}",
                            stats.ev, stats.exposure, stats.luminance, stats.lights, stats.shadow_views, camera.position.x, camera.position.y,
                            camera.position.z, glm::degrees(camera.yaw), glm::degrees(camera.pitch), frame, shot);
                static const bool log_lights = std::getenv("PT_SHOT_LIGHTS") != nullptr;
                if (log_lights && lighting.tpp.enabled) {
                    const pt::TppAtmosphereSettings& t = lighting.tpp;
                    pt::LogInfo("shot tpp fog: density {:.5f} falloff {:.4f} near {:.3f} far {:.3f} self ({:.4f} {:.4f} {:.4f}) mie ({:.3f} {:.3f} {:.3f}) "
                                "g {:.3f} area {} density {:.5f} colour ({:.4f} {:.4f} {:.4f}) near {:.2f} falloff {:.3f} box ({:.2f} {:.2f} {:.2f})-"
                                "({:.2f} {:.2f} {:.2f})",
                                t.fog_density, t.fog_falloff, t.fog_near, t.fog_far, t.fog_self.x, t.fog_self.y, t.fog_self.z, t.fog_mie.x,
                                t.fog_mie.y, t.fog_mie.z, t.fog_mie_anisotropy, t.area ? 1 : 0, t.area_density, t.area_color.x, t.area_color.y,
                                t.area_color.z, t.area_near, t.area_falloff, t.area_min.x, t.area_min.y, t.area_min.z, t.area_max.x, t.area_max.y,
                                t.area_max.z);
                }
                if (log_lights) {
                    const auto& g = game.Objects().GetGimmick(pt::game::GimmickType::Ocho);
                    pt::LogInfo("shot Lisa: state {} enabled {} shown {} hidden {} world ({:.3f} {:.3f} {:.3f}) mesh {}",
                                game.Objects().Ocho().State(), g.enabled, g.shown, g.hidden_views, g.world[3].x, g.world[3].y, g.world[3].z, g.mesh != nullptr);
                    if (g.mesh) for (const auto& sub : g.mesh->submeshes) pt::LogInfo("shot Lisa sub: shadow {} skinned {} kind {}", sub.shadow, sub.skinned, sub.kind);
                    if (std::getenv("PT_LISA_BONES")) {
                        const auto skin = game.Demos().Gimmicks().Skin(pt::game::GimmickType::Ocho);
                        for (size_t bone = 0; bone < skin.size(); ++bone) {
                            const auto& m = skin[bone];
                            pt::LogInfo("shot Lisa skin {}: {} {} {} {} / {} {} {} {} / {} {} {} {}",
                                        bone, m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1],
                                        m[0][2], m[1][2], m[2][2], m[3][2]);
                        }
                    }
                }
                for (size_t i = 0; log_lights && i < lighting.lights.size(); ++i) {
                    const pt::SceneLight& l = lighting.lights[i];
                    pt::LogInfo("shot light {} {} {}: pos ({:.3f} {:.3f} {:.3f}) dir ({:.3f} {:.3f} {:.3f}) intensity ({:.4f} {:.4f} {:.4f}) "
                                "inner {:.3f} outer {:.3f} dimmer {:.3f} cos_outer {:.4f} shadow {} strength {:.3f} diffuse {:.3f} specular {:.3f} "
                                "area {} hidden {} source {:.3f}",
                                i, l.type == pt::LightType::Spot ? "spot" : "point", l.name.empty() ? "(effect)" : l.name, l.position.x,
                                l.position.y, l.position.z, l.direction.x, l.direction.y, l.direction.z, l.intensity.x, l.intensity.y,
                                l.intensity.z, l.inner_range, l.outer_range, l.dimmer, l.cos_outer, l.cast_shadow ? 1 : 0, l.shadow_strength,
                                l.diffuse_scale, l.specular_scale, l.has_area ? 1 : 0, l.hidden_views, l.source_radius);
                }
            }
        }
        {
            static auto guard_last = std::chrono::steady_clock::time_point{};
            static double guard_vram_used = 0.0;
            static double guard_vram_budget = 0.0;
            const auto guard_now = std::chrono::steady_clock::now();
            if (guard_now - guard_last > std::chrono::seconds(2)) {
                guard_last = guard_now;
                VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
                vmaGetHeapBudgets(app.renderer.Context().allocator, budgets);
                const VkPhysicalDeviceMemoryProperties* memory = nullptr;
                vmaGetMemoryProperties(app.renderer.Context().allocator, &memory);
                guard_vram_used = guard_vram_budget = 0.0;
                for (uint32_t h = 0; memory && h < memory->memoryHeapCount; ++h) {
                    if (memory->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                        guard_vram_used += static_cast<double>(budgets[h].usage) / 1048576.0;
                        guard_vram_budget += static_cast<double>(budgets[h].budget) / 1048576.0;
                    }
                }
                static bool guard_low = false;
                const bool low = pt::MemoryLow(1536.0, guard_vram_used, guard_vram_budget, 0.95);
                if (low != guard_low) {
                    guard_low = low;
                    game.Stages().SetPrefetchAllowed(!low);
                    if (!low) {
                        pt::LogInfo("memory: room again, backing off ends");
                    }
                }
            }
        }
        static const bool status_log = app.window || std::getenv("PT_STATUS_LOG") != nullptr;
        if (status_log) {
            static auto status_start = std::chrono::steady_clock::now();
            static uint64_t status_frames = 0;
            static double status_gpu = 0.0;
            ++status_frames;
            status_gpu += app.scene.Stats().gpu_ms;
            const auto status_now = std::chrono::steady_clock::now();
            const double status_seconds = std::chrono::duration<double>(status_now - status_start).count();
            if (status_seconds >= 10.0) {
                VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
                vmaGetHeapBudgets(app.renderer.Context().allocator, budgets);
                const VkPhysicalDeviceMemoryProperties* memory = nullptr;
                vmaGetMemoryProperties(app.renderer.Context().allocator, &memory);
                double used = 0.0;
                double budget = 0.0;
                for (uint32_t h = 0; memory && h < memory->memoryHeapCount; ++h) {
                    if (memory->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                        used += static_cast<double>(budgets[h].usage);
                        budget += static_cast<double>(budgets[h].budget);
                    }
                }
                const VkExtent2D render_extent = app.renderer.RenderExtent();
                pt::LogInfo("status: {:.0f} fps, gpu {:.2f} ms, vram {:.0f} of {:.0f} MB, {}x{}, upscaler {} {}, frame generation {}, vsync {}, "
                            "fps limit {}, floor {}{}",
                            status_frames / status_seconds, status_gpu / std::max<uint64_t>(status_frames, 1), used / 1048576.0,
                            budget / 1048576.0, render_extent.width, render_extent.height, app.settings.upscaling.upscaler,
                            app.settings.upscaling.quality, app.settings.upscaling.frame_generation,
                            app.settings.display.vsync ? "on" : "off", app.settings.display.fps_limit, game.Floor().CurrentFloorName(),
                            ui_ready && ui.MenuOpen() ? ", menu open" : "");
                status_start = status_now;
                status_frames = 0;
                status_gpu = 0.0;
            }
        }
        if (app.window && !vr) {
            constexpr int kMenuFpsCap = 60;
            static std::chrono::steady_clock::time_point frame_deadline{};
            const bool menu = ((ui_ready && ui.MenuOpen()) || show_settings || game.Paused()) && !photo_mode;
            int cap = app.settings.display.fps_limit;
            if (menu) {
                cap = cap > 0 ? std::min(cap, kMenuFpsCap) : kMenuFpsCap;
            }
            pt::UpscaleHost::Get().SetMenuOpen(menu);
            pt::FrameGeneration* dlss_fg = pt::streamline::Active() ? pt::UpscaleHost::Get().DlssFrameGenImpl() : nullptr;
            const bool reflex_paced = dlss_fg && dlss_fg->Generating() && !menu;
            if (pt::streamline::Active()) {
                pt::streamline::SetFrameLimit(reflex_paced && cap > 0 ? static_cast<uint32_t>(2000000 / cap) : 0u);
                static uint64_t streamline_frames = 0;
                if (!app.streamline_marker.empty() && ++streamline_frames == 600) {
                    std::error_code ec;
                    std::filesystem::remove(app.streamline_marker, ec);
                    pt::LogInfo("streamline: 600 frames rendered, start marker removed");
                }
            }
            if (reflex_paced) {
                cap = 0;
            }
            if (cap > 0) {
                const auto period = std::chrono::nanoseconds(1000000000LL / cap);
                const auto now = std::chrono::steady_clock::now();
                if (frame_deadline.time_since_epoch().count() == 0 || now - frame_deadline > period) {
                    frame_deadline = now;
                }
                frame_deadline += period;
                const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(frame_deadline - std::chrono::steady_clock::now());
                if (wait.count() > 0) {
                    SDL_DelayPrecise(static_cast<Uint64>(wait.count()));
                }
            } else {
                frame_deadline = {};
            }
        }
        if ((options.headless && frame >= static_cast<uint64_t>(options.frames)) || game.QuitRequested()) {
            const glm::vec3 feet = game.GetPlayer().Feet();
            pt::LogInfo("game: stopped at frame {}: step {}, floor {}, loop {}, feet ({:.2f} {:.2f} {:.2f})", frame, game.Controller().Step(),
                        game.Floor().CurrentFloorName(), game.Floor().LoopCount(), feet.x, feet.y, feet.z);
            if (!options.screenshot.empty()) {
                app.renderer.SaveScreenshot(options.screenshot);
            }
            running = false;
        }
    }
    microphone.Close();
    input.Shutdown();
    if (script.Expectations() > 0) {
        pt::LogInfo("input script: {} expectations, {} failed", script.Expectations(), script.Failures());
    }
    if (!options.audio_capture.empty()) {
        WriteWav(options.audio_capture, captured, 48000, 2);
    }
    game.SetAudio(nullptr);
    sound.Shutdown();
    vkDeviceWaitIdle(app.renderer.Context().device);
    app.scene.vfx_forward = nullptr;
    app.scene.vfx_filter = nullptr;
    vfx_pass.Shutdown();
    app.renderer.overlay = nullptr;
    ui.Shutdown();
    return script.Failures() > 0 ? 3 : 0;
}

}

bool HeadlessOnly() {
    const char* value = std::getenv("PT_HEADLESS_ONLY");
    return value && *value && std::string_view(value) != "0";
}

bool WouldOpenWindow(const Options& options) {
    return !(options.headless || options.script_test || options.anim_test || !options.voice_test.empty() || options.voice_listen > 0.0f ||
             !options.fox2_test.empty() || options.list_pads);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    const HANDLE inherited = GetStdHandle(STD_ERROR_HANDLE);
    if ((inherited == nullptr || inherited == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif
    App app;
    app.options = ParseOptions(argc, argv);
    if (!app.options.make_loop_previews.empty()) {
        SetUpLoopPreviewRun(app.options);
    }
    if (!app.options.make_museum_previews.empty()) {
        SetUpMuseumPreviewRun(app.options);
    }
    const Options& options = app.options;
    std::filesystem::path log_path = options.log_path;
    if (log_path.empty()) {
        const bool tool = options.headless || options.script_test || options.anim_test || !options.voice_test.empty() || options.voice_listen > 0.0f ||
                          !options.texture_test.empty() || options.list_pads;
        const std::filesystem::path user = tool ? std::filesystem::path() : UserDataDir();
        log_path = user.empty() ? std::filesystem::path("pt.log") : user / "pt.log";
    }
    g_output_dir = log_path.parent_path();
    pt::LogSetFile(log_path.string().c_str());
    if (HeadlessOnly() && WouldOpenWindow(options)) {
        pt::LogError("refused: PT_HEADLESS_ONLY set, run would open a window");
        std::fprintf(stderr, "refused: PT_HEADLESS_ONLY set and this run would open a window\n");
        return 3;
    }
    pt::LogInfo("pt-port version {} ({})", pt::update::CurrentVersion(), pt::update::Platform());
#ifdef _WIN32
    pt::InstallCrashReporting(g_output_dir, PT_BUILD_ID);
    if (const char* test = std::getenv("PT_TEST_CRASH")) {
        const std::string kind = test;
        pt::LogInfo("crash test: {}", kind);
        if (kind == "access") {
            *static_cast<volatile int*>(nullptr) = 1;
        } else if (kind == "terminate") {
            std::terminate();
        } else if (kind == "abort") {
            std::abort();
        } else if (kind == "purecall") {
            struct Base {
                Base() { Call(this); }
                virtual ~Base() = default;
                virtual void Pure() = 0;
                static void Call(Base* b) { b->Pure(); }
            };
            struct Derived : Base {
                void Pure() override {}
            };
            Derived d;
        }
    }
    pt::InstallAllocationFailureLog();
    const double memory_gate = std::getenv("PT_MEMORY_GATE") ? std::atof(std::getenv("PT_MEMORY_GATE")) : 4096.0;
    if (options.headless && memory_gate > 0.0 && !pt::WaitForFreeMemory(memory_gate, 120)) {
        pt::LogExit(75, "not enough free memory to start");
        return 75;
    }
#endif
    if (!options.voice_test.empty()) {
        return RunVoiceTest(options.voice_test);
    }
    if (options.voice_listen > 0.0f) {
        return RunVoiceListen(options.voice_listen, options.voice_device);
    }
    if (options.list_pads) {
        const bool ok = SDL_Init(SDL_INIT_GAMEPAD);
        if (ok) {
            pt::LogJoysticks(3000);
        }
        SDL_Quit();
        return ok ? 0 : 1;
    }
    if (options.virtual_pads) {
        pt::VirtualPads::UseOnlyVirtualDevices();
    }

    pt::Vfs vfs;
    const std::filesystem::path game_dir = FindGameDir(options);
    if (game_dir != options.game_dir) {
        pt::LogInfo("game files found at {}", game_dir.string());
    }
    if (!vfs.Mount(game_dir)) {
        if (!options.headless) {
#ifdef __APPLE__
            const std::string text = "The P.T. game files were not found in\n" + std::filesystem::absolute(game_dir).string() +
                                     "\n\nStart P.T. again and pick your extracted CUSA01127 folder (it contains chunk1.psarc and "
                                     "texture.qar), put that folder next to P.T. PC Port.app, or start pt with --game <folder>.";
#else
            const std::string text = "The P.T. game files were not found in\n" + std::filesystem::absolute(game_dir).string() +
                                     "\n\nStart pt.exe with --game <folder>, where the folder is your extracted CUSA01127 package "
                                     "(it contains chunk1.psarc and texture.qar), or put that folder at game\\CUSA01127 next to the "
                                     "working directory.";
#endif
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "P.T.", text.c_str(), nullptr);
        }
        return 1;
    }
    app.options.game_dir = std::filesystem::absolute(game_dir);
    if (const int code = pt::anim::RunAnimTestIfRequested(argc, argv, vfs); code >= 0) {
        return code;
    }
    if (options.script_test) {
        return RunScriptTest(vfs);
    }
    if (!options.fox2_test.empty()) {
        return RunFox2Test(vfs, options.fox2_test);
    }

    if (!SDL_Init(options.headless ? (options.virtual_pads ? SDL_INIT_GAMEPAD : 0) : (SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))) {
        pt::LogError("SDL_Init: {}", SDL_GetError());
        return 1;
    }
    if (options.headless && !options.settings_path.empty()) {
        app.settings_path = options.settings_path;
        pt::LoadAppSettings(app.settings_path, app.settings);
    }
    if (!options.headless) {
        app.settings_path = options.settings_path;
        if (app.settings_path.empty()) {
            if (char* pref = SDL_GetPrefPath("pt-port", "pt")) {
                app.settings_path = std::filesystem::path(pref) / "pt.ini";
                SDL_free(pref);
            }
        }
        if (!app.settings_path.empty() && !pt::LoadAppSettings(app.settings_path, app.settings)) {
            pt::SaveAppSettings(app.settings_path, app.settings);
        }
        if (app.settings.network.check_updates && !options.no_update_check) {
            app.updates.Start();
        }
    }
    if (!options.fake_update.empty() && !options.no_update_check) {
        app.updates.Fake(options.fake_update);
        pt::LogInfo("update: fake release {} (--fake-update)", options.fake_update);
    }
    if (!options.headless) {
        if (options.size_set) {
            app.settings.display.width = static_cast<int>(options.width);
            app.settings.display.height = static_cast<int>(options.height);
        }
        if (options.vsync_set) {
            app.settings.display.vsync = options.vsync;
        }
        app.options.width = static_cast<uint32_t>(app.settings.display.width);
        app.options.height = static_cast<uint32_t>(app.settings.display.height);
        app.options.vsync = app.settings.display.vsync;
        if (const std::string vulkan = pt::vk::VulkanLibraryPath(); !vulkan.empty()) {
            SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, vulkan.c_str());
        }
        app.window = SDL_CreateWindow("P.T.", app.settings.display.width, app.settings.display.height,
                                      SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!app.window) {
            pt::LogError("SDL_CreateWindow: {}", SDL_GetError());
            return 1;
        }
        ApplyFullscreen(app);
    }
    MountMods(app);
    pt::RendererSettings settings;
    settings.headless = options.headless;
    settings.validation = options.validation;
    settings.vsync = options.vsync;
    settings.width = options.width;
    settings.height = options.height;
    if ((app.settings.vr.enabled || options.vr) && !options.no_vr) {
        app.xr = std::make_unique<pt::xr::Host>();
        if (app.xr->Init("P.T. (pt-port)")) {
            app.renderer.Context().creator = app.xr.get();
            settings.vsync = false;
        } else {
            pt::LogWarn("vr: off for this run: {}", app.xr->Error());
            app.xr.reset();
        }
    }
    app.renderer.Context().hooks = &pt::UpscaleHost::Get();
    app.scene.upscale = UpscaleFromApp(app.settings);
    if (app.xr) {
        app.scene.upscale.kind = pt::UpscalerKind::Off;
        app.scene.upscale.frame_generation = pt::FrameGenKind::Off;
    }
    ApplyGraphicsSettings(app);
    const pt::RayTracingSettings& rt = app.scene.raytracing;
    app.renderer.Context().want_ray_query = true;
    pt::UpscaleHost::Get().SetStartupUpscaler(app.scene.upscale.kind);
    app.streamline_marker = StartStreamline(app);
    int result = 1;
    const bool test_device_lost = std::getenv("PT_TEST_CRASH") && std::string(std::getenv("PT_TEST_CRASH")) == "devicelost";
    const bool renderer_ready = app.renderer.Init(app.window, settings);
    if (renderer_ready && test_device_lost) {
        app.renderer.Context().CheckDeviceLost(VK_ERROR_DEVICE_LOST, "PT_TEST_CRASH");
    }
    if (renderer_ready && app.textures.Init(app.renderer.Context()) && app.scene.Init(app.renderer, app.textures)) {
        app.textures.SetAnisotropy(AnisotropyFromApp(app.settings));
        app.vfs = &vfs;
        app.texture_runtime = pt::ExecutableDir() / "texture-tools";
        std::filesystem::path texture_data = app.settings_path.parent_path();
        if (!options.save_dir.empty()) texture_data = options.save_dir;
        if (texture_data.empty()) {
            if (char* pref = SDL_GetPrefPath("pt-port", "pt")) {
                texture_data = pref;
                SDL_free(pref);
            }
        }
        {
            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(app.renderer.Context().physical, &memory);
            uint64_t local = 0;
            for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
                if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local = std::max<uint64_t>(local, memory.memoryHeaps[i].size);
            }
            app.texture_max_output = local >= 10ull * 1000 * 1000 * 1000 ? 0u : 2048u;
            if (const char* cap = std::getenv("PT_ENHANCED_TEXTURES_CAP")) app.texture_max_output = static_cast<uint32_t>(std::atoi(cap));
            pt::LogInfo("enhanced textures: {:.1f} GB device-local video memory, {} mode", static_cast<double>(local) / (1024.0 * 1024.0 * 1024.0),
                        app.texture_max_output ? std::format("capped ({} px)", app.texture_max_output) : std::string("full (2x, up to 4096 px)"));
        }
        app.texture_cache = texture_data / (app.texture_max_output ? std::format("enhanced-textures-{}", app.texture_max_output) : std::string("enhanced-textures"));
        if (app.settings.graphics.enhanced_textures) RequestEnhancedTextures(app, true);
        static std::string imgui_ini;
        if (app.window && !g_output_dir.empty()) {
            imgui_ini = (g_output_dir / "pt_imgui.ini").string();
            ImGui::GetIO().IniFilename = imgui_ini.c_str();
        }
        app.models = std::make_unique<pt::ModelCache>(vfs, app.scene, app.textures);
        if (app.xr && !app.xr->StartSession(app.renderer.Context(), app.settings.vr.resolution_scale)) {
            pt::LogWarn("vr: off for this run: {}", app.xr->Error());
            app.xr->Shutdown();
            app.xr.reset();
        }
        const bool viewer = !options.stage.empty() || !options.texture_test.empty();
        result = viewer ? RunViewer(app, vfs) : RunGame(app, vfs);
        app.texture_job.Cancel();
        vkDeviceWaitIdle(app.renderer.Context().device);
        if (app.xr) {
            app.xr->Shutdown();
        }
        app.models->Clear();
        app.models.reset();
        app.scene.Shutdown();
        app.textures.Shutdown();
    }
    app.renderer.Shutdown();
    if (!app.streamline_marker.empty()) {
        std::error_code ec;
        std::filesystem::remove(app.streamline_marker, ec);
    }
    if (app.window) {
        SDL_DestroyWindow(app.window);
    }
    SDL_Quit();
    pt::LogExit(result, "normal end");
    return result;
}
