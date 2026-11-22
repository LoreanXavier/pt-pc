#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "engine/core/ascii.h"
#include "engine/platform/update_check.h"
#include "engine/platform/update_journal.h"
#include "engine/platform/json_value.h"
#include "engine/platform/macos_data_path.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <shobjidl.h>
#include <shlobj.h>
#undef small
#else
#include <unistd.h>
#endif
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
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
#include <thread>
#include <tuple>
#include <unordered_map>

#include "engine/anim/anim_test.h"
#include "engine/assets/fmdl.h"
#include "engine/assets/geom.h"
#include "engine/core/log.h"
#include "engine/core/crash_upload.h"
#include "engine/platform/discord_presence.h"
#include "engine/audio/audio_engine.h"
#include "engine/audio/output_device.h"
#include "engine/audio/playback_endpoints.h"
#include "engine/core/crash_report.h"
#include "engine/core/memory_status.h"
#include "build_id.h"
#include "engine/data/fox2.h"
#include "engine/fs/file_prefetch.h"
#include "engine/fs/fox_crypt.h"
#include "engine/fs/game_folder.h"
#include "engine/fs/io_stats.h"
#include "engine/fs/mods.h"
#include "engine/fs/vfs.h"
#include "engine/physics/collision_world.h"
#include "engine/platform/input.h"
#include "engine/platform/controller_speaker.h"
#include "engine/platform/controller_feedback.h"
#include "engine/platform/livesplit.h"
#include "engine/render/intel_vulkan_driver.h"
#include "engine/render/model_cache.h"
#include "engine/render/gpu/gpu_vk_native.h"
#include "engine/render/renderer.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/texture_manager.h"
#include "engine/render/d3d12/vk_d3d12_share.h"
#include "engine/render/upscale/frame_generation.h"
#include "engine/render/upscale/fg_start_guard.h"
#include "engine/render/upscale/neural_rendering.h"
#include "engine/render/upscale/neural_native.h"
#include "engine/render/upscale/neural_unlock.h"
#include "engine/render/upscale/fg_unlock.h"
#include "engine/render/upscale/route_queue.h"
#include "engine/render/upscale/streamline.h"
#include "engine/render/upscale/fsr_provider_gate.h"
#include "engine/render/vfx_pass.h"
#include "engine/vfx/vfx_system.h"
#include "engine/script/lua_vm.h"
#include "engine/ui/asset_browser.h"
#include "engine/core/resource_path.h"
#include "engine/platform/settings.h"
#include "engine/platform/display_modes.h"
#include "engine/platform/display_policy.h"
#include "engine/platform/os.h"
#include "engine/platform/rendering_api.h"
#include "engine/platform/start_watch.h"
#include "engine/render/vk_layers.h"
#ifdef _WIN32
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
namespace {
std::filesystem::path g_d3d12_marker;
void ArmD3D12Marker(int stage) {
    if (g_d3d12_marker.empty()) return;
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    std::ofstream(g_d3d12_marker) << stage << "\npid " << pid
                                  << "\npt.exe is starting DirectX 12. If this file stays, the next start looks for a crash of this start.\n";
}
void ClearD3D12Marker() {
    if (g_d3d12_marker.empty()) return;
    std::error_code ec;
    std::filesystem::remove(g_d3d12_marker, ec);
}
int ReadD3D12Marker() {
    int stage = 0;
    std::ifstream(g_d3d12_marker) >> stage;
    return stage >= 1 && stage <= 3 ? stage : 1;
}
std::string D3D12CrashReason(int stage) {
    return stage == 2 ? "DirectX 12 stopped the game while its device was starting at an earlier start"
         : stage == 3 ? "DirectX 12 stopped the game in its first frames at an earlier start"
                      : "DirectX 12 stopped the game while the game checked whether it runs here, at an earlier start";
}
struct StartEvidence {
    bool crashed = false;
    bool exited = false;
    std::string what;
};
StartEvidence EvidenceOfEarlierStart(const std::filesystem::path& marker, const std::filesystem::path& previous_log) {
    StartEvidence e;
    std::error_code ec;
    const auto marker_time = std::filesystem::last_write_time(marker, ec);
    const bool timed = !ec;
    unsigned long pid = 0;
    {
        std::ifstream in(marker);
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("pid ", 0) == 0) pid = std::strtoul(line.c_str() + 4, nullptr, 10);
    }
    if (std::ifstream in(previous_log); in) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.find(" crash: ") != std::string::npos || line.find(" fatal: ") != std::string::npos ||
                line.find(" hang: ") != std::string::npos || line.find("device lost at") != std::string::npos ||
                line.find("DEVICE_REMOVED") != std::string::npos || line.find("DEVICE_HUNG") != std::string::npos) {
                if (!e.crashed) e.what = "pt-previous.log: " + line;
                e.crashed = true;
            }
            if (line.find(" exit: ") != std::string::npos) e.exited = true;
        }
    }
    if (!e.crashed && timed) {
        for (const auto& entry : std::filesystem::directory_iterator(previous_log.parent_path(), ec)) {
            const std::string name = pt::os::PathToUtf8(entry.path().filename());
            if (name.rfind("pt-crash-", 0) != 0 || entry.path().extension() != ".dmp") continue;
            std::error_code tec;
            const auto t = std::filesystem::last_write_time(entry.path(), tec);
            if (!tec && t + std::chrono::seconds(2) >= marker_time) {
                e.crashed = true;
                e.what = "dump " + name;
                break;
            }
        }
    }
#ifdef _WIN32
    if (!e.crashed && timed && pid) {
        const double hours =
            std::chrono::duration<double>(std::filesystem::file_time_type::clock::now() - marker_time).count() / 3600.0 + 0.05;
        char hex[32];
        std::snprintf(hex, sizeof(hex), "0x%lx", pid);
        for (const std::string& report : pt::start_watch::RecentErrorReports(std::clamp(hours, 0.05, 24.0 * 30), 8)) {
            if (report.find(hex) != std::string::npos || report.find("=" + std::to_string(pid) + ";") != std::string::npos) {
                e.crashed = true;
                e.what = "Windows error report " + report;
                break;
            }
        }
    }
#endif
    if (!e.crashed) e.what = e.exited ? "none (that start logged its own exit)" : "none (no dump, no crash, hang or lost device line, no Windows error report)";
    return e;
}
}
#ifdef PT_D3D12_GAME
#include "engine/render/d3d12/backend/agility_exports.h"
#include "engine/render/d3d12/backend/d3d12_backend.h"
#include "engine/render/d3d12/backend/d3d12_raytracing.h"
#include "engine/render/d3d12/backend/d3d12_upscale.h"
PT_D3D12_AGILITY_EXPORTS()
namespace {
bool ProbeD3D12(std::string& reason) {
    pt::gpu::d3d12::CreateOptions o;
    o.debug = std::getenv("PT_D3D12_DEBUG") != nullptr;
    o.dred = false;
    ArmD3D12Marker(1);
    if (const char* test = std::getenv("PT_TEST_CRASH"); test && std::string_view(test) == "d3d12probe") {
        pt::LogInfo("crash test: d3d12probe");
        *static_cast<volatile int*>(nullptr) = 1;
    }
    const bool made = pt::gpu::d3d12::Create(o, reason) != nullptr;
    ClearD3D12Marker();
    if (const char* fail = std::getenv("PT_D3D12_PROBE_FAIL"); fail && fail[0] == '1') {
        reason = "PT_D3D12_PROBE_FAIL is set";
        return false;
    }
    pt::LogInfo("rendering api: DirectX 12 {}{}", made ? "can run here" : "cannot run here: ", made ? "" : reason);
    return made;
}
}
#endif
#include "engine/platform/sdl_diag.h"
#include "engine/platform/third_party.h"
#include "engine/platform/boot_timing.h"
#include "engine/platform/device_start.h"
#include "engine/platform/user_data.h"
#include "engine/platform/graphics_presets.h"
#include "engine/platform/virtual_pad.h"
#include "engine/voice/microphone.h"
#include "engine/voice/voice_recognizer.h"
#include "game/archive.h"
#include "game/survival/street_city.h"
#include "game/support_link.h"
#include "game/controller_speaker_events.h"
#include "game/museum_transcripts.h"
#include "game/archive_theater.h"
#include "engine/audio/wem.h"
#include "game/museum_music.h"
#include "game/lisa_date/lisa_date.h"
#include "game/debug_panel.h"
#include "game/game.h"
#include "game/archive_model_camera.h"
#include "game/survival/survival.h"
#include "game/outro_input.h"
#include "game/loop_browser.h"
#include "game/render_mouse.h"
#include "game/game_sound.h"
#include "game/input_script.h"
#include "game/prompt_textures.h"
#include "game/render_scene.h"
#include "game/ui/photo_panel.h"
#include "game/script_api.h"
#include "game/stage_data.h"
#include "game/survival/survival_visions.h"
#include "game/ui/game_ui.h"
#include "game/ui/pc_settings.h"
#include "game/first_run.h"
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
    bool update_now = false;
    std::vector<std::string> relaunch_args;
    int street_offer = 0;
    bool release_locks = kReleaseLocks;
    bool third_person = false;
    std::filesystem::path make_loop_previews;
    std::filesystem::path dlss5_install;
    int dlss5_arch = 0;
    std::filesystem::path make_museum_previews;
    std::filesystem::path museum_previews;
    std::filesystem::path make_survival_visions;
    bool vr = false;
    bool no_vr = false;
    std::string backend;
};

struct OptionSpec {
    const char* name;
    int values;
    const char* help;
};
constexpr OptionSpec kOptionSpecs[] = {
    {"--help", 0, "print this list and exit (also -h)"},
    {"--game", 1, "<folder> your extracted P.T. game folder (any name, for example CUSA01127)"},
    {"--settings", 1, "<file> pt.ini to use instead of data/pt.ini next to the executable"},
    {"--save-dir", 1, "<folder> where saves go"},
    {"--no-save", 0, "never write a save"},
    {"--log", 1, "<file> the log file (default: data/pt.log next to the executable, or the working folder for tools and --headless)"},
    {"--mods", 1, "<folder> the mods folder"},
    {"--no-mods", 0, "load no mods"},
    {"--no-update-check", 0, "do not look for a newer release"},
    {"--fake-update", 1, "<version> pretend this version was released (tests)"},
    {"--update-now", 0, "tests: start the in-game update as soon as one is offered (docs/updates.md)"},
    {"--vr", 0, "the experimental VR mode (an OpenXR runtime and a headset; docs/vr.md)"},
    {"--no-vr", 0, "no VR mode whatever pt.ini says"},
    {"--backend", 1, "<vulkan|d3d12> the graphics API for this run (pt.ini [display] rendering_api otherwise)"},
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
    {"--dlss5-install", 1, "<folder> install DLSS 5 into that data folder and exit (tests)"},
    {"--dlss5-arch", 1, "<0|75|86|89> the GPU code --dlss5-install takes (0: RTX 50, none)"},
    {"--make-museum-previews", 1, "<folder> shoot the Museum's thumbnails and exit (headless)"},
    {"--make-survival-visions", 1, "<folder> shoot the survival night's notice pictures and exit (headless)"},
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
    if (const std::string env = pt::os::GetEnv("PT_GAME_DIR"); !env.empty()) {
        options.game_dir = pt::os::PathFromUtf8(env);
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
            options.game_dir = pt::os::PathFromUtf8(argv[++i]);
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
            if (arg == "--voice-test") options.voice_test = pt::os::PathFromUtf8(argv[++i]);
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
            options.input_script = pt::os::PathFromUtf8(argv[++i]);
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
        } else if (arg == "--dlss5-install" && has_value) {
            options.dlss5_install = pt::os::PathFromUtf8(argv[++i]);
        } else if (arg == "--dlss5-arch" && has_value) {
            options.dlss5_arch = std::atoi(argv[++i]);
        } else if (arg == "--make-loop-previews" && has_value) {
            options.make_loop_previews = pt::os::PathFromUtf8(argv[++i]);
        } else if (arg == "--make-museum-previews" && has_value) {
            options.make_museum_previews = pt::os::PathFromUtf8(argv[++i]);
        } else if (arg == "--make-survival-visions" && has_value) {
            options.make_survival_visions = pt::os::PathFromUtf8(argv[++i]);
        } else if (arg == "--museum-previews" && has_value) {
            options.museum_previews = pt::os::PathFromUtf8(argv[++i]);
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
            options.audio_capture = pt::os::PathFromUtf8(argv[++i]);
            options.audio_offline = true;
        } else if (arg == "--save-dir" && has_value) {
            options.save_dir = pt::os::PathFromUtf8(argv[++i]);
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
            options.mods_dir = pt::os::PathFromUtf8(argv[++i]);
        } else if (arg == "--no-mods") {
            options.no_mods = true;
        } else if (arg == "--vr") {
            options.vr = true;
        } else if (arg == "--no-vr") {
            options.no_vr = true;
        } else if (arg == "--backend" && has_value) {
            options.backend = argv[++i];
            pt::RenderingApi api;
            if (!pt::ParseRenderingApi(options.backend, api)) RefuseCommandLine("--backend takes vulkan or d3d12, not '" + options.backend + "'");
        } else if (arg == "--no-update-check") {
            options.no_update_check = true;
        } else if (arg == "--fake-update" && has_value) {
            options.fake_update = argv[++i];
        } else if (arg == "--update-now") {
            options.update_now = true;
        } else if (arg == "--settings" && has_value) {
            options.settings_path = pt::os::PathFromUtf8(argv[++i]);
        } else if (arg == "--log" && has_value) {
            options.log_path = pt::os::PathFromUtf8(argv[++i]);
        } else if (arg == "--screenshot" && has_value) {
            options.screenshot = pt::os::PathFromUtf8(argv[++i]);
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
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--fake-update" || arg == "--input-script") {
            ++i;
            continue;
        }
        if (arg != "--update-now") options.relaunch_args.emplace_back(arg);
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
        if (!SDL_LoadWAV(pt::os::PathToUtf8(wav_path).c_str(), &spec, &data, &length)) {
            pt::LogError("voice test: cannot load {}: {}", pt::os::PathToUtf8(wav_path), SDL_GetError());
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
        pt::LogInfo("voice test: file {} | {} detections | {} utterances | {:.2f} s | {}", pt::os::PathToUtf8(wav_path.filename()), detections,
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
    pt::LogInfo("audio capture: {} s written to {}", samples.size() / channels / static_cast<double>(rate), pt::os::PathToUtf8(path));
}

struct App {
    Options options;
    pt::AppSettings settings;
    pt::update::Checker updates;
    bool update_polled = false;
    bool update_requested = false;
    bool update_question = false;
    int update_language = 0;
    pt::nr_unlock::GpuClass nr_gpu = pt::nr_unlock::GpuClass::Other;
    std::optional<pt::nr_unlock::Install> nr_install;
    bool nr_install_usable = false;
    pt::nr_unlock::Job nr_job;
    uint32_t nr_job_serial = 0;
    pt::nr_native::Job nr_native_job;
    uint32_t nr_native_serial = 0;
    int nr_arch = -1;
    bool nr_native_remove_pending = false;
    bool nr_crash_hold = false;
    int d3d12_crash_stage = 0;
    bool dlssg_crash_hold = false;
    int nr_job_request = 0;
    bool nr_reshade_loaded = false;
    bool nr_marker_read = false;
    bool nr_marker_at_start = false;
    bool nr_prearmed = false;
    bool nr_addon_parked = false;
    bool nr_addon_boot_on = false;
    int nr_hook_tries = 0;
    double nr_hook_try_time = -10.0;
    bool nr_hook_failed = false;
    bool nr_restart_pending = false;
    uint32_t nr_toggles_logged = 0;
    pt::route_queue::Queue routes;
    int NrPending() const { return routes.Pending(pt::route_queue::Route::Nr); }
    int FgPending() const { return routes.Pending(pt::route_queue::Route::Fg); }
    std::string nr_row_note;
    bool nr_route_failed = false;
    bool nr_route_armed = false;
    double nr_route_check_time = 0.0;
    bool fg_applicable = false;
    std::optional<pt::fg_unlock::Install> fg_install;
    bool fg_install_usable = false;
    pt::fg_unlock::Job fg_job;
    uint32_t fg_job_serial = 0;
    int fg_job_request = 0;
    std::string fg_row_note;
    bool fg_route_failed = false;
    bool fg_route_armed = false;
    int fg_unlock_choice = 0;
    int nr_unlock_choice = 0;
    std::vector<std::string> pending_notices;
    int fg_test_percent = -1;
    int nr_test_percent = -1;
    bool route_state_test = false;
    double fg_route_check_time = 0.0;
    pt::RenderingApi rendering_api = pt::RenderingApi::Vulkan;
    std::filesystem::path settings_path;
    std::filesystem::path streamline_marker;
    bool d3d12_marker = false;
    bool start_layers_retry = false;
    SDL_Window* window = nullptr;
    bool microphone_test = false;
    int extras_request = 0;
    std::string archive_request;
    pt::game::SubtitlePlayer* transcripts = nullptr;
    bool freecam_active = false;
    bool photo_active = false;
    int display_hdr = -1;
    bool display_hdr_capable = false;
    bool windows_hdr_switched = false;
    pt::display::Environment display_env;
    std::string steam_deck;
    bool display_mode_chosen = false;
    bool microphone_monitor = false;
    std::string audio_output_name;
    float microphone_db = -80.0f;
    std::string microphone_hypothesis;
    std::string microphone_status = "pc_mic_waiting";
    std::string microphone_reason = "pc_mic_st_loading";
    pt::Renderer renderer;
    pt::TextureManager textures;
    pt::SceneRenderer scene;
    std::unique_ptr<pt::ModelCache> models;
    pt::Vfs* vfs = nullptr;
    std::unique_ptr<pt::mods::ModSet> mods;
    std::unique_ptr<pt::xr::Host> xr;
    pt::LiveSplitClient livesplit;
};

/* the game itself, for the preview captures it starts in the background */
std::filesystem::path GameExecutable() {
#ifdef __APPLE__
    return pt::ExecutablePath();
#elif defined(_WIN32)
    return pt::ExecutableDir() / "pt.exe";
#else
    return pt::ExecutableDir() / "pt";
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
            pt::LogWarn("mods: {} is not a folder", pt::os::PathToUtf8(dir));
            return;
        }
        if (options.headless || !std::filesystem::create_directories(dir, ec)) {
            return;
        }
        std::ofstream note(dir / "README.txt", std::ios::binary);
        note << "Put each mod in its own folder here, then restart the game.\r\n"
                "Once a mod is installed, PC settings shows a Mods row under Graphics to turn each one on or off. Search pt.log (in the data folder) for \"mods:\".\r\n"
                "How to make one: docs/modding.md and docs/modding-quickstart.md at https://github.com/LoreanXavier/pt-pc\r\n";
        pt::LogInfo("mods: made the empty folder {}", pt::os::PathToUtf8(dir));
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
    pt::LogInfo("mods: {} found in {}, {} files replaced", app.mods->mods.size(), pt::os::PathToUtf8(dir), app.mods->index.Size());
}

int WindowMode(const App& app) {
    return pt::display::PlanMode(app.display_env, app.settings.display.fullscreen, app.display_mode_chosen).mode;
}

void LogWindowState(const App& app, std::string_view what) {
    if (!app.window) return;
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(app.window, &w, &h);
    SDL_GetWindowSizeInPixels(app.window, &pw, &ph);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(app.window);
    pt::LogInfo("display: {}: {} (pt.ini {} {}x{}), window {}x{}, pixels {}x{}, flags {:#x}{}, display {}", what,
                pt::display::ModeName(WindowMode(app)), pt::display::ModeName(app.settings.display.fullscreen), app.settings.display.width,
                app.settings.display.height, w, h, pw, ph, static_cast<uint64_t>(flags),
                (flags & SDL_WINDOW_FULLSCREEN) ? " (fullscreen)" : " (no fullscreen flag)", SDL_GetDisplayForWindow(app.window));
}

void LogDisplays() {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    for (int i = 0; ids && i < count; ++i) {
        const SDL_DisplayID id = ids[i];
        SDL_Rect bounds{}, usable{};
        SDL_GetDisplayBounds(id, &bounds);
        SDL_GetDisplayUsableBounds(id, &usable);
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(id);
        const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(id);
        int mode_count = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &mode_count);
        std::string listed;
        for (int m = 0; modes && m < mode_count && m < 64; ++m) {
            listed += std::format("{}{}x{}@{:.0f}", listed.empty() ? "" : " ", modes[m]->w, modes[m]->h, modes[m]->refresh_rate);
        }
        if (mode_count > 64) listed += std::format(" (and {} more)", mode_count - 64);
        SDL_free(modes);
        const char* name = SDL_GetDisplayName(id);
        pt::LogInfo("display {}{}: '{}', bounds {}x{} at {},{}, usable {}x{} at {},{}, content scale {:.2f}, desktop mode {}x{} @ {:.2f} Hz "
                    "density {:.2f}, current mode {}x{} @ {:.2f} Hz, {} fullscreen modes: {}",
                    id, id == SDL_GetPrimaryDisplay() ? " (primary)" : "", name ? name : "", bounds.w, bounds.h, bounds.x, bounds.y, usable.w,
                    usable.h, usable.x, usable.y, SDL_GetDisplayContentScale(id), desktop ? desktop->w : 0, desktop ? desktop->h : 0,
                    desktop ? desktop->refresh_rate : 0.0f, desktop ? desktop->pixel_density : 0.0f, current ? current->w : 0,
                    current ? current->h : 0, current ? current->refresh_rate : 0.0f, mode_count, listed.empty() ? "none" : listed);
    }
    SDL_free(ids);
}

std::string SteamDeckHardware() {
#ifdef __linux__
    std::ifstream file("/sys/devices/virtual/dmi/id/product_name");
    std::string product;
    if (file && std::getline(file, product)) {
        if (const char* model = pt::display::SteamDeckModel(product)) return model;
    }
#endif
    return {};
}

void DetectDisplayEnvironment(App& app) {
    app.display_env = pt::display::DetectEnvironment([](const char* name) { return pt::os::GetEnv(name); });
    app.steam_deck = SteamDeckHardware();
    std::string vars;
    for (const char* name : {"GAMESCOPE_WAYLAND_DISPLAY", "WAYLAND_DISPLAY", "DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "SteamDeck",
                             "SteamGamepadUI", "SteamOS"}) {
        const std::string value = pt::os::GetEnv(name);
        if (!value.empty()) vars += std::format("{}{}={}", vars.empty() ? "" : ", ", name, value);
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    pt::LogInfo("display: SDL driver {}, gamescope {}{}{}, environment: {}", driver ? driver : "none", app.display_env.gamescope ? "yes (" : "no",
                app.display_env.evidence, app.display_env.gamescope ? ")" : "", vars.empty() ? "none of the display variables" : vars);
    if (!app.steam_deck.empty()) pt::LogInfo("display: hardware {} (built-in screen 1280x800)", app.steam_deck);
}

void LogGamescopeScreen(const App& app) {
    if (!app.window || !app.display_env.gamescope || app.steam_deck.empty()) return;
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(app.window));
    if (!desktop || pt::display::SameShape({desktop->w, desktop->h}, pt::display::kSteamDeckScreen)) return;
    pt::LogWarn("display: gamescope's X screen is {}x{}, another shape than this {}'s 1280x800 screen: gamescope fits it into the screen "
                "with black bars. Steam picks that size (the game's Properties > General > Game Resolution); Default fills the Deck's "
                "screen. On an external display of that shape there are no bars.",
                desktop->w, desktop->h, app.steam_deck);
}

void ApplyFullscreen(App& app) {
    if (!app.window) {
        return;
    }
    const pt::display::ModePlan plan = pt::display::PlanMode(app.display_env, app.settings.display.fullscreen, app.display_mode_chosen);
    int mode = plan.mode;
    if (*plan.reason) {
        pt::LogInfo("display: {} instead of pt.ini's {} in this session: {}", pt::display::ModeName(mode),
                    pt::display::ModeName(app.settings.display.fullscreen), plan.reason);
    }
    if (mode == 2) {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(app.window);
        int count = 0;
        SDL_DisplayMode** listed = SDL_GetFullscreenDisplayModes(display, &count);
        std::vector<glm::ivec2> choices;
        std::vector<pt::DisplayModeInfo> infos;
        choices.reserve(std::max(count, 0));
        for (int i = 0; listed && i < count; ++i) {
            choices.emplace_back(listed[i]->w, listed[i]->h);
            infos.push_back({listed[i]->w, listed[i]->h, static_cast<double>(listed[i]->refresh_rate)});
        }
        choices = pt::UniqueDisplaySizes(choices);
        const glm::ivec2 selected = pt::ClosestDisplaySize(choices, {app.settings.display.width, app.settings.display.height});
        const double wanted_hz = app.settings.display.refresh_rate;
        const int picked = pt::PickExclusiveMode(infos, selected, wanted_hz);
        const SDL_DisplayMode* chosen = picked >= 0 ? listed[picked] : nullptr;
        if (!chosen) chosen = SDL_GetDesktopDisplayMode(display);
        if (chosen) {
            std::string offered;
            for (const double r : pt::RefreshRatesFor(infos, {chosen->w, chosen->h})) offered += (offered.empty() ? "" : ", ") + pt::RefreshRateText(r);
            const bool rate_missing = wanted_hz > 0.0 && !pt::SameRefreshRate(chosen->refresh_rate, wanted_hz);
            pt::LogInfo("display: exclusive mode {}x{} @ {:.2f} Hz, the closest of {} sizes to pt.ini's {}x{}; refresh rate {}{} (offered at "
                        "this size: {})",
                        chosen->w, chosen->h, chosen->refresh_rate, choices.size(), app.settings.display.width, app.settings.display.height,
                        wanted_hz > 0.0 ? pt::RefreshRateText(wanted_hz) : std::string("Auto, the highest"),
                        rate_missing ? " is not offered at this size, the highest instead" : "", offered.empty() ? "none" : offered);
            app.settings.display.width = chosen->w;
            app.settings.display.height = chosen->h;
            if (!SDL_SetWindowFullscreenMode(app.window, chosen)) {
                pt::LogWarn("display: exclusive mode {}x{} unavailable: {}", chosen->w, chosen->h, SDL_GetError());
                mode = 1;
                app.settings.display.fullscreen = mode;
                SDL_SetWindowFullscreenMode(app.window, nullptr);
            }
        } else {
            pt::LogWarn("display: no exclusive modes available: {}", SDL_GetError());
            mode = 1;
            app.settings.display.fullscreen = mode;
            SDL_SetWindowFullscreenMode(app.window, nullptr);
        }
        SDL_free(listed);
    } else {
        SDL_SetWindowFullscreenMode(app.window, nullptr);
    }
    if (!SDL_SetWindowFullscreen(app.window, mode != 0)) {
        pt::LogWarn("display: fullscreen mode {} failed: {}", mode, SDL_GetError());
        app.settings.display.fullscreen = 0;
        SDL_SetWindowFullscreenMode(app.window, nullptr);
        SDL_SetWindowFullscreen(app.window, false);
        SDL_SetWindowSize(app.window, app.settings.display.width, app.settings.display.height);
    } else if (mode == 1) {
        SDL_SyncWindow(app.window);
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(app.window, &pw, &ph);
        if (pw > 0 && ph > 0) {
            app.renderer.Resize(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
        }
    }
    LogWindowState(app, "window mode requested");
}

void LogDisplayPacing(App& app) {
    if (!app.window) return;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(app.window);
    const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
    const double hz = mode ? static_cast<double>(mode->refresh_rate) : 0.0;
    const bool vsync = app.settings.display.vsync;
    const int limit = app.settings.display.fps_limit;
    const bool generation = app.settings.upscaling.frame_generation != "off";
    static std::string last;
    const std::string key = std::format("{} {} {:.3f} {} {} {} {} {}", display, mode ? mode->w : 0, hz, vsync, limit, generation,
                                        app.settings.display.fullscreen, mode ? mode->h : 0);
    if (key == last) return;
    last = key;
    std::string pacing;
    if (!vsync) {
        pacing = "v-sync off: each frame is shown when it is finished";
    } else if (generation) {
        pacing = "v-sync on with frame generation: the generator paces the shown frames";
    } else if (const pt::LimiterPacing p = pt::PacingOfLimit(hz, limit); p.min_refreshes == 0) {
        pacing = limit > 0 ? "v-sync on, the limit at or above the refresh rate: the display paces the frames"
                           : "v-sync on, no fps limit: the display paces the frames";
    } else if (p.min_refreshes == p.max_refreshes) {
        pacing = std::format("v-sync on, fps limit {}: every frame stays {} refreshes", limit, p.min_refreshes);
    } else {
        pacing = std::format("v-sync on, fps limit {}: {:.2f} refreshes a frame, so frames stay {} or {} refreshes on screen (uneven: "
                             "steady motion judders unless the display runs variable refresh)",
                             limit, hz / limit, p.min_refreshes, p.max_refreshes);
    }
    const char* name = display ? SDL_GetDisplayName(display) : nullptr;
    pt::LogInfo("display: {} {}x{} at {:.2f} Hz, window mode {}; {}", name ? name : "unknown", mode ? mode->w : 0, mode ? mode->h : 0, hz,
                app.settings.display.fullscreen, pacing);
}

std::filesystem::path g_output_dir;
std::filesystem::path g_log_path;

std::filesystem::path UserDataDir() {
#ifdef __APPLE__
    /* inside the app bundle (ExecutableDir() is Contents/Resources) nothing may be written: a signed bundle, often in
       /Applications; the data folder is then the user's (docs/macos.md). A build folder keeps it next to pt. */
    static const std::filesystem::path directory = [] {
        const std::filesystem::path base = pt::ExecutableDir();
        const bool bundled = std::any_of(base.begin(), base.end(), [](const std::filesystem::path& part) { return part.extension() == ".app"; });
        std::filesystem::path requested = base / "data";
        if (bundled) {
            const auto home = pt::os::GetEnv("HOME");
            if (!home.empty()) requested = pt::os::PathFromUtf8(home) / "Library" / "Application Support" / "pt-port" / "pt";
        }
        std::error_code error;
        const auto resolved = pt::platform::ResolveMacDataPath(requested, error);
        if (!error) return resolved;
        pt::LogWarn("Cannot resolve macOS data folder '{}': {}", pt::os::PathToUtf8(requested), error.message());
        return requested;
    }();
    return directory;
#else
    return pt::ExecutableDir() / "data";
#endif
}

bool SaveMoveTest() { return !pt::os::GetEnv("PT_TEST_SAVE_MOVE").empty(); }

std::filesystem::path LegacyUserDataDir() {
#if defined(__APPLE__)
    return {};
#elif defined(_WIN32)
    if (SaveMoveTest()) {
        const auto value = pt::os::GetEnv("APPDATA");
        wchar_t logon[MAX_PATH * 2] = {};
        DWORD size = sizeof(logon);
        std::filesystem::path real;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Volatile Environment", L"APPDATA", RRF_RT_REG_SZ, nullptr, logon, &size) == ERROR_SUCCESS) {
            real = logon;
        }
        const auto inside_real = [&](std::filesystem::path dir) {
            for (dir = std::filesystem::absolute(dir).lexically_normal(); !dir.empty(); dir = dir.parent_path()) {
                std::error_code ec;
                if (std::filesystem::equivalent(dir, real, ec)) return true;
                if (dir == dir.parent_path()) break;
            }
            return false;
        };
        if (value.empty() || real.empty() || inside_real(pt::os::PathFromUtf8(value))) {
            pt::LogWarn("user data: PT_TEST_SAVE_MOVE needs APPDATA set to a test folder; the old profile is ignored");
            return {};
        }
        return pt::os::PathFromUtf8(value) / "pt-port" / "pt";
    }
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &roaming))) {
        const auto dir = std::filesystem::path(roaming) / "pt-port" / "pt";
        CoTaskMemFree(roaming);
        return dir;
    }
    CoTaskMemFree(roaming);
    const auto value = pt::os::GetEnv("APPDATA");
    return value.empty() ? std::filesystem::path() : pt::os::PathFromUtf8(value) / "pt-port" / "pt";
#else
    const auto value = pt::os::GetEnv("XDG_DATA_HOME");
    if (value.empty()) {
        const auto home = pt::os::GetEnv("HOME");
        return home.empty() ? std::filesystem::path() : pt::os::PathFromUtf8(home) / ".local" / "share" / "pt-port" / "pt";
    }
    return pt::os::PathFromUtf8(value) / "pt-port" / "pt";
#endif
}

bool LooksLikeGameDir(const std::filesystem::path& dir) { return pt::game_folder::LooksLikeGameDir(dir); }

std::filesystem::path RememberedGameDirFile() {
    const std::filesystem::path user = UserDataDir();
    return user.empty() ? std::filesystem::path() : user / "game_dir.txt";
}

std::filesystem::path FindGameDir(const Options& options, std::string& source, bool* player_quit = nullptr) {
    if (options.game_dir_given || LooksLikeGameDir(options.game_dir)) {
        source = options.game_dir_given ? "--game" : "the default folder";
        if (!LooksLikeGameDir(options.game_dir)) {
            if (std::filesystem::path inner = pt::game_folder::FindGameFolder(options.game_dir); !inner.empty()) {
                pt::LogInfo("game files: {} holds no game files itself; using {}", pt::os::PathToUtf8(options.game_dir), pt::os::PathToUtf8(inner));
                return inner;
            }
        }
        return options.game_dir;
    }
    if (const pt::game_folder::Found found_near = pt::game_folder::FindNearProgram(pt::ExecutableDir()); !found_near.folder.empty()) {
        source = found_near.where;
        return found_near.folder;
    }
    const std::filesystem::path remembered_file = RememberedGameDirFile();
    if (!remembered_file.empty()) {
        std::ifstream in(remembered_file, std::ios::binary);
        std::string line;
        if (std::getline(in, line)) {
            std::filesystem::path remembered = std::filesystem::path(reinterpret_cast<const char8_t*>(line.c_str()));
#ifdef __APPLE__
            std::error_code path_error;
            const auto resolved = pt::platform::ResolveMacDataPath(remembered, path_error);
            if (!path_error) remembered = resolved;
#endif
            if (LooksLikeGameDir(remembered)) {
                source = "game_dir.txt, the folder picked at an earlier start";
                return remembered;
            }
        }
    }
    source = "not found";
    if (options.headless && pt::os::GetEnv("PT_TEST_FIRST_RUN").empty()) {
        return options.game_dir;
    }
    pt::LogInfo("game files: none next to the program, in the folders above it or in game_dir.txt; the first-run screen asks for the "
                "PKG or the extracted folder");
    pt::first_run::Context context;
    context.program_dir = pt::ExecutableDir();
    context.user_dir = UserDataDir();
    context.remembered_file = remembered_file;
    context.interactive = !options.headless;
    context.language = pt::first_run::PlayerLanguage(context.user_dir);
#ifdef _WIN32
    context.helper_candidates.push_back(context.program_dir / "extractor" / "PT.PkgExtract.exe");
#else
    context.helper_candidates.push_back(context.program_dir / "extractor" / "PT.PkgExtract");
#endif
#ifdef __APPLE__
    context.helper_candidates.push_back(pt::ExecutablePath().parent_path() / "extractor" / "PT.PkgExtract");
    context.program_dir_read_only = UserDataDir() != pt::ExecutableDir() / "data";
#endif
    context.missing_packages = [](const std::filesystem::path& dir) {
        pt::Vfs check;
        return check.Mount(dir) ? check.MissingCorePackages() : -1;
    };
    const pt::first_run::Result result = pt::first_run::Run(context);
    if (result.game_dir.empty()) {
        source = "the player quit the first-run screen";
        if (player_quit) *player_quit = true;
        return options.game_dir;
    }
    source = result.source;
    return result.game_dir;
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
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        pt::LogError("SDL_Init(audio): {} (audio drivers built in: {})", SDL_GetError(), pt::SdlCompiledAudioDrivers());
        return 1;
    }
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

bool ViewAttachOff() {
    static const bool off = [] {
        const char* v = std::getenv("PT_VIEW_ATTACH");
        return v && v[0] == '0';
    }();
    return off;
}

void IntoViewFrame(std::vector<pt::DrawItem>& items, const pt::Camera& view) {
    if (ViewAttachOff()) return;
    const glm::mat4 into = view.View();
    for (pt::DrawItem& item : items) {
        if (item.view_attached) item.transform = into * item.transform;
    }
}

void OntoView(std::vector<pt::DrawItem>& items, const pt::Camera& view) {
    if (ViewAttachOff()) return;
    const glm::mat4 onto = glm::inverse(view.View());
    for (pt::DrawItem& item : items) {
        if (item.view_attached) item.transform = onto * item.transform;
    }
}

struct TickState {
    std::vector<pt::DrawItem> items;
    std::vector<glm::mat4> skins;
    std::unordered_map<uint64_t, size_t> index;
    pt::game::TickBlend lights;

    void Capture(pt::game::Game& game, pt::game::VfxScene& vfx) {
        lights.handy_aim = game.HandyAim();
        lights.handy_mask_roll = game.HandyMaskRoll();
        lights.handy_pickup_roll = game.HandyPickupMaskRoll();
        lights.demo_lights = game.Demos().Lights();
        vfx.SnapshotWorlds();
        items.clear();
        index.clear();
        game.CollectDraws(items);
        IntoViewFrame(items, game.ViewCamera());
        lights.handy_camera = game.GetPlayer().MakeCamera();
        lights.handy_camera.position += game.GetPlayer().DrawnOffset();
        lights.handy_lens_valid = game.HandyLens(lights.handy_lens);
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

struct FramePhases {
    enum Phase { kTicks, kLoop, kCap, kFence, kSleep, kAcquire, kBuild, kVfx, kRecord, kEndRecord, kSubmit, kPresent, kCount };
    static constexpr const char* kNames[kCount] = {"ticks", "loop", "fps cap", "fence", "reflex", "acquire", "build", "vfx", "record",
                                                   "end record", "submit", "present"};
    std::array<float, kCount> current{};
    std::array<double, kCount> sum{};
    double frame_sum = 0.0;
    float frame_max = 0.0f;
    std::array<float, kCount> max_phases{};
    uint32_t frames = 0;
    uint32_t spikes = 0;
    uint32_t spikes_logged = 0;
    std::chrono::steady_clock::time_point last_end{};

    void EndFrame(uint64_t frame, float gpu_ms, bool log) {
        const auto now = std::chrono::steady_clock::now();
        const bool first = last_end.time_since_epoch().count() == 0;
        const float frame_ms = first ? 0.0f : std::chrono::duration<float, std::milli>(now - last_end).count();
        last_end = now;
        if (first) {
            current = {};
            return;
        }
        float covered = 0.0f;
        for (int i = 0; i < kCount; ++i) {
            if (i != kLoop) covered += current[i];
        }
        current[kLoop] = std::max(0.0f, frame_ms - covered);
        for (int i = 0; i < kCount; ++i) sum[i] += current[i];
        frame_sum += frame_ms;
        ++frames;
        if (frame_ms > frame_max) {
            frame_max = frame_ms;
            max_phases = current;
        }
        if (frame_ms > 50.0f) {
            ++spikes;
            if (log && spikes_logged < 8) {
                ++spikes_logged;
                int top = 0;
                for (int i = 1; i < kCount; ++i) {
                    if (current[i] > current[top]) top = i;
                }
                pt::LogInfo("frame spike: {:.1f} ms at frame {}, {} {:.1f} ms ({}; gpu {:.2f} ms two frames back)", frame_ms, frame,
                            kNames[top], current[top], Describe(current), gpu_ms);
            }
        }
        current = {};
    }

    static std::string Describe(const std::array<float, kCount>& p) {
        std::string text;
        for (int i = 0; i < kCount; ++i) {
            text += std::format("{}{} {:.1f}", i ? ", " : "", kNames[i], p[i]);
        }
        return text;
    }

    std::string StatusAndReset() {
        std::string text;
        if (frames > 0) {
            std::array<float, kCount> mean{};
            for (int i = 0; i < kCount; ++i) mean[i] = static_cast<float>(sum[i] / frames);
            text = std::format("cpu frame {:.2f} ms ({}), worst {:.1f} ms ({}), {} frames over 50 ms", frame_sum / frames, Describe(mean),
                               frame_max, Describe(max_phases), spikes);
        }
        sum = {};
        frame_sum = 0.0;
        frame_max = 0.0f;
        max_phases = {};
        frames = 0;
        spikes = 0;
        spikes_logged = 0;
        return text;
    }
};

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

#ifdef _WIN32
bool NvidiaAdapterPresent() {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dxgi ? reinterpret_cast<CreateFactory1>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    bool found = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        found = d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        adapter->Release();
    }
    factory->Release();
    return found;
}
#endif

#ifdef _WIN32
bool g_no_gpu_driver = false;
#endif

std::vector<std::string> g_own_proxies;
void ScanThirdPartyTools() {
    static std::optional<std::string> last;
    const std::string tools = pt::third_party::LoadedTools(g_own_proxies);
    if (last && tools == *last) return;
    last = tools;
    pt::LogInfo("third party: {}", tools.empty() ? std::string("none") : tools);
    pt::SetThirdPartyNote(tools);
}
#ifdef _WIN32

void LogGraphicsAdapters() {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dxgi ? reinterpret_cast<CreateFactory1>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        pt::LogWarn("gpu: DXGI could not list the graphics adapters");
        return;
    }
    IDXGIFactory6* factory6 = nullptr;
    factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6));
    bool microsoft_only = true;
    int listed = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT hr = factory6 ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                                           reinterpret_cast<void**>(&adapter))
                                    : factory->EnumAdapters1(i, &adapter);
        if (FAILED(hr) || !adapter) break;
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        LARGE_INTEGER umd{};
        const bool has_version = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
        std::wstring wide_name(d.Description);
        pt::LogInfo("gpu: adapter {}{}: {} (vendor {:04x} device {:04x}, {} MB dedicated, LUID {:08x}:{:08x}{}{})", i,
                    factory6 ? " by high performance order" : "", pt::os::PathToUtf8(std::filesystem::path(wide_name)), d.VendorId, d.DeviceId,
                    d.DedicatedVideoMemory / (1024 * 1024), static_cast<unsigned>(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart,
                    has_version ? std::format(", driver {}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                                              LOWORD(umd.LowPart))
                                : std::string(),
                    (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : "");
        microsoft_only = microsoft_only && d.VendorId == 0x1414;
        ++listed;
        adapter->Release();
    }
    if (factory6) factory6->Release();
    factory->Release();
    g_no_gpu_driver = listed > 0 && microsoft_only;
    if (g_no_gpu_driver) {
        pt::LogWarn("gpu: only Microsoft's Basic Render Driver is listed: the graphics card's own driver is not installed (or Windows "
                    "turned it off); the game does not render in software");
    }
}
#endif

std::filesystem::path StartStreamline(App& app, bool d3d12) {
    const char* force = std::getenv("PT_STREAMLINE");
    const bool forced = force && std::atoi(force) != 0;
#ifdef _WIN32
    const bool wanted = d3d12 ? app.window && NvidiaAdapterPresent() : app.window && app.scene.upscale.frame_generation == pt::FrameGenKind::Dlss;
#else
    const bool wanted = app.window && app.scene.upscale.frame_generation == pt::FrameGenKind::Dlss;
#endif
    if (!forced && !wanted) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path dir = forced ? std::filesystem::current_path(ec) : app.settings_path.parent_path();
    if (dir.empty()) {
        dir = UserDataDir();
    }
    /* DLSS-G can take the process down inside its first frames with nothing logged; a marker left over from such a start turns it off next time. */
    const std::filesystem::path marker = dir / "streamline-starting.txt";
    bool retry = false;
    if (!forced && std::filesystem::exists(marker, ec)) {
        const StartEvidence evidence = EvidenceOfEarlierStart(marker, g_log_path.parent_path() / "pt-previous.log");
        int& interrupted = app.settings.upscaling.dlssg_interrupted;
        if (!evidence.crashed && (evidence.exited || interrupted < 1)) {
            if (!evidence.exited) interrupted = 1;
            pt::LogWarn("streamline: the last start ended in Streamline's first frames with no sign of a crash, evidence: {}; it loads "
                        "again{}", evidence.what, evidence.exited ? "" : " (a second such start in a row turns DLSS Frame Generation off, pt.ini dlssg_interrupted = 1)");
            if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
            retry = true;
        }
    }
    if (!forced && !retry && std::filesystem::exists(marker, ec)) {
        app.settings.upscaling.dlssg_interrupted = 0;
        pt::LogWarn("streamline: last DLSS-G start died in its first frames, DLSS Frame Generation off");
        std::filesystem::remove(marker, ec);
        pt::UpscaleHost::Get().SetDlssFrameGenFailed(true);
        app.dlssg_crash_hold = true;
        if (app.scene.upscale.frame_generation == pt::FrameGenKind::Dlss) app.scene.upscale.frame_generation = pt::FrameGenKind::Off;
        UpscaleToApp(app.scene.upscale, app.settings);
        if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
        return {};
    }
    if (!forced) {
#ifdef _WIN32
        const unsigned long pid = GetCurrentProcessId();
#else
        const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
        std::ofstream(marker) << "pid " << pid << "\npt.exe is starting NVIDIA Streamline (DLSS Frame Generation). If this file stays "
                                 "and the start crashed, the next start leaves it off.\n";
    }
    std::string reason;
    if (!pt::streamline::Start(dir / "streamline", reason, d3d12 ? pt::streamline::Api::D3D12 : pt::streamline::Api::Vulkan)) {
        pt::LogWarn("streamline: not loaded: {}", reason);
        std::filesystem::remove(marker, ec);
        return {};
    }
    if (!d3d12) {
        app.renderer.Context().loader = pt::streamline::InstanceProcAddr();
    }
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
    u.dlssg_multiplier = std::clamp(s.upscaling.dlssg_multiplier, 2, 4);
    u.ray_reconstruction = s.upscaling.ray_reconstruction;
    if (const char* env = std::getenv("PT_RR")) {
        u.ray_reconstruction = std::atoi(env) != 0;
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
    r.full = s.graphics.ray_quality >= 3;
    r.full_unused = r.full && !r.shadows && !r.reflections && !r.ambient_occlusion;
    if (r.full_unused) r.full = false;
    if (const char* env = std::getenv("PT_RT_FULL")) {
        r.full = std::atoi(env) != 0;
        r.full_unused = false;
    }
    r.path_traced = s.graphics.ray_quality >= 4;
    if (const char* env = std::getenv("PT_RT_PATH")) {
        r.path_traced = std::atoi(env) != 0;
    }
    if (r.path_traced) {
        r.full = true;
        r.full_unused = false;
    }
    r.full_shadows = s.ray_tracing.full_shadows;
    if (const char* env = std::getenv("PT_RT_FULL_SHADOWS")) {
        r.full_shadows = std::atoi(env) != 0;
    }
    r.bounce_light = s.ray_tracing.bounce_light;
    if (const char* env = std::getenv("PT_RT_GI")) {
        r.bounce_light = std::atoi(env) != 0;
    }
    r.glossy_reflections = s.ray_tracing.glossy_reflections;
    if (const char* env = std::getenv("PT_RT_GLOSSY")) {
        r.glossy_reflections = std::atoi(env) != 0;
    }
    return r;
}

constexpr bool kCommunityRouteRetired = true;

std::filesystem::path NrDataDir(const std::filesystem::path& settings_path) {
    return settings_path.empty() ? UserDataDir() : settings_path.parent_path();
}

bool UsesNrRoute(pt::nr_unlock::GpuClass gpu) {
    return gpu == pt::nr_unlock::GpuClass::NeedsUnlock || gpu == pt::nr_unlock::GpuClass::Native;
}

struct NeuralQualityPlan { float scale; bool pre; };
NeuralQualityPlan PlanForNeuralQuality(const std::string& quality) {
    if (quality == "performance") return {0.75f, true};
    if (quality == "ultra") return {1.0f, false};
    return {1.0f, true};
}
int NeuralQualityIndex(const std::string& quality) {
    return quality == "performance" ? 0 : quality == "ultra" ? 2 : 1;
}

bool NrUpscalerReady(const App& app) {
    return app.settings.upscaling.upscaler == pt::UpscalerKey(pt::UpscalerKind::Dlss);
}

pt::NeuralRenderingSupport NeuralSupport(App& app) {
    pt::NeuralRenderingSupport s;
    if (pt::nr_native::Enabled() && app.renderer.Backend() == pt::gpu::Backend::D3D12) {
        if (const auto* actual = app.scene.NativeNeuralStatus()) return *actual;
        s.runtime = pt::NativeNeuralRuntime();
        s.available = app.nr_gpu != pt::nr_unlock::GpuClass::Other && s.runtime.has_value();
        s.reason = s.available ? "private native D3D12 preflight; actual feature create is logged by the renderer" : "native NR needs an NVIDIA RTX GPU and a supplied runtime";
        if (!s.available) s.note = s.runtime ? "pc_note_nr_gpu" : "pc_note_nr_runtime";
        return s;
    }
    if (UsesNrRoute(app.nr_gpu)) {
        if (!app.nr_install_usable) {
            s.reason = "DLSS 5 needs the community route installed first (the Graphics page's Download or Unlock action)";
            s.note = "pc_note_nr_unlock";
            return s;
        }
        if (app.renderer.Backend() != pt::gpu::Backend::D3D12) {
            s.reason = "the DLSS 5 unlock runs on the DirectX 12 backend; set the Rendering API to DirectX 12 and restart";
            s.note = "pc_note_nr_route_d3d12";
            return s;
        }
        s.available = true;
        return s;
    }
    if (app.nr_gpu == pt::nr_unlock::GpuClass::Other) {
        s.reason = "DLSS 5 needs an NVIDIA GeForce RTX GPU";
        s.note = "pc_note_nr_gpu";
        return s;
    }
    if (app.renderer.Backend() == pt::gpu::Backend::Vulkan) {
        if (pt::NeuralBackend* nr = pt::UpscaleHost::Get().Neural()) {
            return nr->Support(app.settings.upscaling.neural_runtime);
        }
        s.reason = "not in this build";
        s.note = "pc_note_nr_build";
        return s;
    }
    s.reason = "DLSS 5 neural rendering runs on the Vulkan backend; restart with Vulkan to use it";
    s.note = "pc_note_nr_d3d12";
    return s;
}

bool NeuralRouteActive(const App& app) {
    if (kCommunityRouteRetired || pt::nr_native::Enabled()) return false;
    return UsesNrRoute(app.nr_gpu) && app.nr_install_usable && app.renderer.Backend() == pt::gpu::Backend::D3D12;
}

bool NeuralRouteNeedsRestart(const App& app) {
    if (kCommunityRouteRetired || pt::nr_native::Enabled()) return false;
    return pt::nr_unlock::ToggleNeedsRestart(app.nr_gpu, app.nr_install_usable,
                                           app.renderer.Backend() == pt::gpu::Backend::D3D12, app.nr_hook_failed);
}

bool NrTestInstall() {
    const char* fake = std::getenv("PT_TEST_NR_INSTALLED");
    return fake && *fake == '1';
}

void WriteNeuralRoute(App& app) {
    if (!NeuralRouteActive(app) || NrTestInstall()) return;
    pt::NeuralRenderingKind kind = pt::NeuralRenderingKind::Off;
    pt::ParseNeuralRendering(app.settings.upscaling.neural_rendering, kind);
    pt::NeuralStyle style = pt::NeuralStyle::Default;
    pt::ParseNeuralStyle(app.settings.upscaling.neural_style, style);
    pt::nr_unlock::AddonSettings addon;
    addon.on = kind == pt::NeuralRenderingKind::Dlss5 && NrUpscalerReady(app);
    addon.parked = app.nr_addon_parked;
    addon.style = static_cast<int>(style);
    addon.intensity = std::clamp(app.settings.upscaling.neural_intensity, 0.0f, 1.0f);
    if (const char* keys = std::getenv("PT_NR_ADDON_KEYS")) addon.extra = pt::nr_unlock::ParseAddonKeys(keys);
    std::string error;
    if (pt::nr_unlock::WriteAddonSettings(pt::ExecutableDir(), addon, &error)) {
        pt::LogInfo("neural rendering: route add-on set to {} (style {}, strength {:.2f})", addon.on ? "on" : "off", addon.style, addon.intensity);
    } else {
        pt::LogWarn("neural rendering: could not write the route add-on settings: {}", error);
    }
}

void Save(App& app) {
    if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
}

void ArmNeuralRoute(App& app, bool armed) {
    if (app.nr_route_armed == armed || app.settings.upscaling.neural_route_armed == armed) {
        app.nr_route_armed = armed;
        app.settings.upscaling.neural_route_armed = armed;
        return;
    }
    app.nr_route_armed = armed;
    app.settings.upscaling.neural_route_armed = armed;
    Save(app);
    pt::LogInfo("neural rendering: route crash marker {}", armed ? "set" : "cleared");
}

void DisableNeuralRoute(App& app, const char* note, const std::string& detail) {
    pt::LogWarn("neural rendering: DLSS 5 is off for this session: {}", detail);
    app.nr_route_failed = true;
    app.nr_row_note = note;
    app.settings.upscaling.neural_rendering = "off";
    app.scene.neural.enabled = false;
    if (NeuralRouteActive(app) && app.nr_reshade_loaded && !NrTestInstall()) {
        pt::nr_unlock::AddonSettings off;
        off.on = false;
        off.parked = app.nr_addon_parked;
        if (const char* keys = std::getenv("PT_NR_ADDON_KEYS")) off.extra = pt::nr_unlock::ParseAddonKeys(keys);
        pt::nr_unlock::WriteAddonSettings(pt::ExecutableDir(), off);
    }
    ArmNeuralRoute(app, false);
    Save(app);
}

bool FgRouteActive(const App& app) {
    return app.fg_install_usable && app.settings.upscaling.frame_generation == "dlss" && pt::UpscaleHost::Get().DlssFrameGen().hardware;
}

void ArmFgRoute(App& app, bool armed) {
    if (app.fg_route_armed == armed && app.settings.upscaling.dlssg_route_armed == armed) return;
    app.fg_route_armed = armed;
    app.settings.upscaling.dlssg_route_armed = armed;
    Save(app);
    pt::LogInfo("frame generation: unlock crash marker {}", armed ? "set" : "cleared");
}

void DisableFgRoute(App& app, const char* note, const std::string& detail) {
    pt::LogWarn("frame generation: the community unlock is off for this session: {}", detail);
    app.fg_route_failed = true;
    app.fg_row_note = note;
    app.settings.upscaling.frame_generation = "off";
    app.scene.upscale.frame_generation = pt::FrameGenKind::Off;
    ArmFgRoute(app, false);
    Save(app);
}

void ApplyNeuralToScene(App& app) {
    const auto& u = app.settings.upscaling;
    std::string wanted = u.neural_rendering;
    if (const char* env = std::getenv("PT_NEURAL_RENDERING")) wanted = env;
    pt::NeuralRenderingKind kind = pt::NeuralRenderingKind::Off;
    pt::ParseNeuralRendering(wanted, kind);
    if (!NrUpscalerReady(app) || app.nr_crash_hold) kind = pt::NeuralRenderingKind::Off;
    const bool passthrough = std::getenv("PT_NEURAL_PASSTHROUGH") != nullptr;
    const bool native_pass = ((app.renderer.Backend() == pt::gpu::Backend::D3D12 && pt::nr_native::Enabled()) ||
                             (app.renderer.Backend() == pt::gpu::Backend::Vulkan && pt::UpscaleHost::Get().Neural() != nullptr)) && NeuralSupport(app).available;
    app.scene.neural.enabled = passthrough || (kind == pt::NeuralRenderingKind::Dlss5 && native_pass);
    pt::NeuralStyle style = pt::NeuralStyle::Default;
    pt::ParseNeuralStyle(u.neural_style, style);
    app.scene.neural.style = style;
    app.scene.neural.intensity = std::clamp(u.neural_intensity, 0.25f, 3.0f);
    const NeuralQualityPlan plan = PlanForNeuralQuality(u.neural_quality);
    app.scene.neural.small_model = u.neural_model == "small";
    app.scene.neural.scale = plan.scale;
    app.scene.neural.every = 1;
    app.scene.neural.pre = plan.pre;
    app.scene.neural.paper_white = 1.0f;
    WriteNeuralRoute(app);
    const bool wanted_on = kind == pt::NeuralRenderingKind::Dlss5;
    const bool deferred = NeuralRouteNeedsRestart(app);
    const bool pending = deferred && wanted_on != app.nr_addon_boot_on;
    if (pending && !app.nr_restart_pending) {
        app.pending_notices.push_back("pc_note_nr_next_start");
        pt::LogInfo("neural rendering: DLSS 5 {} saved for the next start; the add-on stays {} this session",
                    wanted_on ? "on" : "off", app.nr_addon_boot_on ? "on" : "off");
    }
    app.nr_restart_pending = pending;
    if (NeuralRouteActive(app) && app.nr_reshade_loaded && !app.nr_route_failed) {
        ArmNeuralRoute(app, deferred ? app.nr_addon_boot_on : wanted_on);
    }
    if (pt::nr_native::Active() && app.renderer.Backend() == pt::gpu::Backend::D3D12 && !app.nr_route_failed) {
        ArmNeuralRoute(app, app.scene.neural.enabled);
    }
    if (pending) Save(app);
}

void SyncNeuralToggle(App& app) {
    if (NeuralRouteNeedsRestart(app)) return;
    if (!NeuralRouteActive(app) || !app.nr_reshade_loaded || NrTestInstall() || app.nr_hook_failed) return;
    if (!pt::nr_unlock::NrToggleHookReady()) {
        const double now = SDL_GetTicks() / 1000.0;
        if (now - app.nr_hook_try_time < 1.0) return;
        app.nr_hook_try_time = now;
        if (!pt::nr_unlock::InstallNrToggleHook() && ++app.nr_hook_tries >= 10) {
            app.nr_hook_failed = true;
            pt::LogWarn("neural rendering: the route's add-on could not be reached; DLSS 5 on and off apply at the next start");
            ApplyNeuralToScene(app);
        }
        return;
    }
    pt::NeuralRenderingKind kind = pt::NeuralRenderingKind::Off;
    pt::ParseNeuralRendering(app.settings.upscaling.neural_rendering, kind);
    const bool wanted = kind == pt::NeuralRenderingKind::Dlss5 && !app.nr_route_failed;
    const bool addon_on = app.nr_addon_boot_on != ((pt::nr_unlock::NrTogglesStarted() & 1u) != 0);
    if (wanted != addon_on && pt::nr_unlock::PressNrToggle()) {
        pt::LogInfo("neural rendering: DLSS 5 switched {} in play (the add-on's toggle, press {})", wanted ? "on" : "off",
                    pt::nr_unlock::NrTogglesStarted());
    }
    const uint32_t delivered = pt::nr_unlock::NrTogglesDelivered();
    if (delivered != app.nr_toggles_logged) {
        app.nr_toggles_logged = delivered;
        pt::LogInfo("neural rendering: the add-on read toggle press {}", delivered);
    }
}

pt::render::HdrLook HdrLookOf(const pt::AppSettings& s) {
    pt::render::HdrLook look;
    look.gamma = s.display.hdr_gamma;
    look.keep_hue = s.display.hdr_keep_hue;
    look.colour_range = s.display.hdr_colour_range == 2 ? 1 : s.display.hdr_colour_range == 1 ? 2 : 0;
    look.exposure = s.display.hdr_exposure * .1f;
    look.contrast = s.display.hdr_contrast * .01f;
    look.highlights = s.display.hdr_highlights * .01f;
    look.shadows = s.display.hdr_shadows * .01f;
    look.saturation = s.display.hdr_saturation * .01f;
    look.highlight_saturation = s.display.hdr_highlight_saturation * .01f;
    look.blowout = s.display.hdr_blowout * .01f;
    return look;
}

void ApplyGraphicsSettings(App& app) {
    const auto& g=app.settings.graphics;
    app.scene.graphics=g;
    app.scene.raytracing=RayTracingFromApp(app.settings);
    app.scene.toggles.shadows=g.shadow_quality>0 || app.scene.raytracing.shadows || app.scene.raytracing.contact_shadows;
    {
        const auto& rt=app.scene.raytracing;
        const std::string modes=std::format("shadows {}, contact shadows {}, ambient occlusion {}, reflections {}, ray quality {}{}",
            !rt.shadows?"off":!rt.soft_shadows?"sharp (1 ray)":std::format("soft ({} rays)",2<<std::clamp(g.ray_quality,0,2)),rt.contact_shadows?"on":"off",
            rt.ambient_occlusion?"on":"off",rt.reflections?"on":"off",g.ray_quality,
            rt.path_traced?std::string(" (path traced lighting)"):
            rt.full?std::format(" (full: every light's shadow {}, bounce light {}, glossy reflections {})",rt.full_shadows?"on":"off",
                                rt.bounce_light?"on":"off",rt.glossy_reflections?"on":"off"):
            rt.full_unused?std::string(" (Ultra's bounce light and glossy reflections off: no traced shadows, reflections or ambient occlusion)"):std::string());
        static std::string last;
        if(modes!=last){last=modes;pt::LogInfo("ray tracing: {}",modes);}
    }
    {
        static constexpr const char* kPresetNames[]={"low","original","high","ultra","custom"};
        pt::GraphicsPreset detected=pt::DetectGraphicsPreset(app.settings,true);
        if(detected==pt::GraphicsPreset::Custom) detected=pt::DetectGraphicsPreset(app.settings,false);
        app.scene.original_look=detected==pt::GraphicsPreset::Original;
        const int preset=std::clamp(static_cast<int>(detected),0,4);
        const std::string text=std::format("preset {}, shadow maps {}, ambient occlusion {}, reflections {}, bloom {}, depth of field {}, motion blur {}, "
            "lens distortion {}, film grain {:.2f}, anisotropy {}, texture detail {}",kPresetNames[preset],g.shadow_quality,
            g.ambient_occlusion?"on":"off",g.reflections?"on":"off",g.bloom?"on":"off",g.depth_of_field?"on":"off",g.motion_blur?"on":"off",
            g.lens_distortion?"on":"off",g.film_grain,g.anisotropy,g.texture_detail);
        static std::string last;
        if(text!=last){last=text;pt::LogInfo("graphics: {}",text);}
    }
    app.scene.toggles.occlusion=g.ambient_occlusion;
    app.scene.toggles.local_reflections=g.reflections || app.scene.raytracing.reflections;
    app.scene.toggles.bloom=g.bloom;
    app.scene.toggles.depth_of_field=g.depth_of_field;
    app.scene.toggles.motion_blur=g.motion_blur;
    app.scene.toggles.film_grain=g.film_grain>0;
    app.scene.toggles.distortion=g.lens_distortion;
    pt::vfx::SetFlareGhostScale(g.lens_ghosts?0.75f:0.0f);
    pt::vfx::SetFlareStrength(g.flare_strength*.01f,g.ghost_strength*.01f);
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
    s.upscaling.dlssg_multiplier = std::clamp(u.dlssg_multiplier, 2, 4);
    s.upscaling.ray_reconstruction = u.ray_reconstruction;
}

bool DrawUpscaleSettings(App& app) {
    pt::UpscaleSettings& u = app.scene.upscale;
    bool changed = false;
    ImGui::SeparatorText("Upscaling");
    const pt::UpscalerKind kinds[] = {pt::UpscalerKind::Off, pt::UpscalerKind::Fsr, pt::UpscalerKind::Fsr4, pt::UpscalerKind::Dlss,
                                      pt::UpscalerKind::Xess, pt::UpscalerKind::MetalFx};
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
    pt::FrameGeneration* metalfx_fg = pt::UpscaleHost::Get().MetalFxFrameGen();
    std::string metalfx_reason;
    bool metalfx_available = metalfx_fg && metalfx_fg->Available(metalfx_reason);
    if (metalfx_available && (off || u.kind != pt::UpscalerKind::MetalFx)) {
        metalfx_available = false;
        metalfx_reason = u.kind == pt::UpscalerKind::MetalFx ? "needs an upscaler (native AA works)" : metalfx_reason;
        if (u.kind != pt::UpscalerKind::MetalFx) {
            metalfx_reason = "needs the MetalFX upscaler";
        }
    }
    ImGui::BeginDisabled(!metalfx_available);
    bool metalfx_fg_on = u.frame_generation == pt::FrameGenKind::Metalfx;
    if (ImGui::Checkbox("Frame generation (MetalFX, experimental; macOS 26+)", &metalfx_fg_on)) {
        u.frame_generation = metalfx_fg_on ? pt::FrameGenKind::Metalfx : pt::FrameGenKind::Off;
        changed = true;
    }
    ImGui::EndDisabled();
    if (!metalfx_available) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", off ? "needs an upscaler" : metalfx_reason.c_str());
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
    if (dlss_fg_on) {
        const char* multipliers[] = {"2x", "3x", "4x"};
        int multiplier = std::clamp(u.dlssg_multiplier, 2, 4) - 2;
        ImGui::BeginDisabled(!dlss_fg_available);
        if (ImGui::BeginCombo("DLSS frame multiplier", multipliers[multiplier])) {
            for (int i = 0; i < 3; ++i) {
                ImGui::BeginDisabled(static_cast<unsigned>(i + 2) > pt::streamline::FrameGenMaxMultiplier());
                if (ImGui::Selectable(multipliers[i], i == multiplier)) { u.dlssg_multiplier = i + 2; changed = true; }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    pt::FrameGeneration* xefg = pt::UpscaleHost::Get().XefgFrameGen();
    std::string xefg_reason = "Windows only";
    bool xefg_available = xefg && xefg->Available(xefg_reason);
    if (xefg_available && (off || u.kind == pt::UpscalerKind::Spatial)) {
        xefg_available = false;
        xefg_reason = "needs an upscaler (native AA works)";
    }
    ImGui::BeginDisabled(!xefg_available);
    bool xefg_on = u.frame_generation == pt::FrameGenKind::Xess;
    if (ImGui::Checkbox("XeSS Frame Generation (experimental, DirectX 12 present)", &xefg_on)) {
        u.frame_generation = xefg_on ? pt::FrameGenKind::Xess : pt::FrameGenKind::Off;
        changed = true;
    }
    ImGui::EndDisabled();
    if (!xefg_available) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", xefg_reason.c_str());
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
        app.display_mode_chosen = true;
        ApplyFullscreen(app);
        changed = true;
    }
    changed |= ImGui::Checkbox("Vsync (next start)", &s.display.vsync);
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
        input.settings.trigger_rumble = pt::FeaturesForRumbleProfile(s.input.rumble_profile, s.input.rumble).trigger_rumble;
        if (!s.input.rumble) {
            input.SetRumble(0, 0);
            input.SetTriggerRumble(0, 0);
        }
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
    lighting.screen.focal_length = photo.focal_length_mm;
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
                             pt::os::PathToUtf8((dir / std::format("loop-{}.png", i)).generic_u8string()));
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

constexpr int kMuseumPreviewVersion = 3;
constexpr float kMuseumPreviewModelEv = pt::game::kArchiveModelEv;

std::string MuseumPreviewRoute(const std::filesystem::path& dir, const std::vector<const pt::game::ArchiveEntry*>& entries) {
    std::string route = "20 sstep 15\n20 swait 30\n";
    for (const pt::game::ArchiveEntry* e : entries) {
        route += std::format("20 sarchive {}\n20 sarchiveshown 3600\n20 swait {}\n20 sshot {}\n20 swait 2\n20 sarchive -\n20 sarchived\n20 swait 5\n",
                             e->id, pt::game::ArchiveThumbnailFrames(*e), pt::os::PathToUtf8((dir / std::format("{}.png", e->id)).generic_u8string()));
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

std::filesystem::path SurvivalVisionDirectory() {
    return UserDataDir() / std::format("survival-visions-v{}", pt::game::kSurvivalVisionVersion);
}

void SetProcessEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void SetUpSurvivalVisionRun(Options& options) {
    const std::filesystem::path dir = std::filesystem::absolute(options.make_survival_visions);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "capture.txt") << pt::game::SurvivalVisionRoute(dir);
    std::ofstream(dir / "version.txt") << pt::game::kSurvivalVisionVersion << "\n";
    pt::SaveAppSettings(dir / "preview.ini", pt::AppSettings{});
    SetProcessEnvironment("PT_SURVIVAL_SCRIPT", "0.1:hold 0.15:unhurt 0.3:weapon=0");
    SetProcessEnvironment("PT_SURVIVAL_HUD", "0");
    SetProcessEnvironment("PT_SURVIVAL_EVENTS", "0");
    options.headless = true;
    options.no_save = true;
    options.audio_offline = true;
    options.frames = 20000;
    options.demo_rate = 20.0f;
    options.shot_warmup = 30;
    options.shot_settle = true;
    options.width = 640;
    options.height = 880;
    options.size_set = true;
    options.street_offer = -1;
    options.seed = kLoopPreviewSeed;
    options.release_locks = false;
    options.start_floor = "f010";
    options.input_script = dir / "capture.txt";
    options.settings_path = dir / "preview.ini";
    if (options.log_path.empty()) options.log_path = dir / "capture.log";
}

class SurvivalVisionCapture {
public:
    void Update(const Options& options) {
        if (process_.Started()) {
            int code = 0;
            if (process_.Running(&code)) return;
            pt::LogInfo("survival visions: capture finished (exit {})", code);
        }
        if (runs_ >= 2 || options.headless || !options.input_script.empty()) return;
        const auto dir = SurvivalVisionDirectory();
        if (pt::game::SurvivalVisionsReady(dir)) {
            runs_ = 2;
            return;
        }
        const auto exe = GameExecutable();
        std::vector<std::string> args{"--game", pt::os::PathToUtf8(options.game_dir), "--make-survival-visions", pt::os::PathToUtf8(dir),
            "--log", pt::os::PathToUtf8(dir / "capture.log")};
        ++runs_;
        if (process_.Start(exe, args, pt::ExecutableDir())) pt::LogInfo("survival visions: capture started ({})", pt::os::PathToUtf8(dir));
        else pt::LogWarn("survival visions: capture could not start ({})", pt::os::PathToUtf8(exe));
    }

private:
    int runs_ = 0;
    pt::os::ChildProcess process_;
};

SurvivalVisionCapture& SurvivalVisions() {
    static SurvivalVisionCapture capture;
    return capture;
}

std::filesystem::path PhotoPath() {
    std::filesystem::path pictures;
#ifdef _WIN32
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DONT_VERIFY, nullptr, &known)) && known) pictures = known;
    CoTaskMemFree(known);
#endif
    if (pictures.empty()) {
        if (const std::string profile = pt::os::GetEnv("USERPROFILE"); !profile.empty()) {
            pictures = pt::os::PathFromUtf8(profile) / "Pictures";
        } else if (const std::string home = pt::os::GetEnv("HOME"); !home.empty()) {
            pictures = pt::os::PathFromUtf8(home) / "Pictures";
        }
    }
    std::filesystem::path dir = pictures / "PT Photos";
    if (const std::string other = pt::os::GetEnv("PT_PHOTO_DIR"); !other.empty()) dir = pt::os::PathFromUtf8(other);
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

std::filesystem::path UpdaterExecutable() {
#ifdef _WIN32
    return pt::ExecutableDir() / "pt-updater.exe";
#else
    return pt::ExecutableDir() / "pt-updater";
#endif
}

struct UpdateHere {
    enum class Kind { None, Install, Portable } kind = Kind::None;
    std::string problem;
};
std::string FolderWriteProblem(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return pt::os::PathToUtf8(dir) + " is not a folder";
    const std::filesystem::path probe = dir / std::format(".pt-write-probe-{}", pt::os::ProcessId());
    if (!std::filesystem::create_directory(probe, ec) || ec) return pt::os::PathToUtf8(dir) + " is not writable" + (ec ? " (" + ec.message() + ")" : std::string());
    bool written = false;
    {
        std::ofstream out(probe / "write-test", std::ios::binary | std::ios::trunc);
        out.put('x');
        out.close();
        written = static_cast<bool>(out);
    }
    std::filesystem::remove_all(probe, ec);
    return written ? std::string() : pt::os::PathToUtf8(dir) + " is not writable";
}
bool PortableFileList(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || std::filesystem::file_size(file, ec) > 16 * 1024 * 1024) return false;
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    pt::json::update_json::Value root;
    if (!pt::json::update_json::Reader(text).Parse(root) || !root.object) return false;
    const auto files = root.members.find("files");
    if (files == root.members.end()) return false;
    const std::string game = pt::os::PathToUtf8(GameExecutable().filename());
    return std::any_of(files->second.items.begin(), files->second.items.end(), [&](const pt::json::update_json::Value& item) {
        const auto path = item.members.find("path");
        return path != item.members.end() && path->second.text == game;
    });
}
const UpdateHere& InGameUpdateHere() {
    static const UpdateHere here = [] {
        UpdateHere result;
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
        std::error_code ec;
        std::filesystem::path install = pt::ExecutableDir();
        if (!install.has_filename()) install = install.parent_path();
        if (!std::filesystem::is_regular_file(UpdaterExecutable(), ec)) {
            pt::LogInfo("update: no in-game update here ({} is missing); the notice links the release page", pt::os::PathToUtf8(UpdaterExecutable()));
            return result;
        }
        if (std::filesystem::is_regular_file(install / "pt-install-manifest.txt", ec)) {
            result.kind = UpdateHere::Kind::Install;
            pt::LogInfo("update: in-game update for this install ({})", pt::os::PathToUtf8(install));
            return result;
        }
        if (!PortableFileList(install / "pt-files.json")) {
            pt::LogInfo("update: no in-game update here (no pt-install-manifest.txt and no pt-files.json naming the game in {}); the notice links the release page",
                        pt::os::PathToUtf8(install));
            return result;
        }
        result.kind = UpdateHere::Kind::Portable;
        result.problem = FolderWriteProblem(install);
        if (result.problem.empty() && std::filesystem::is_directory(install / "data", ec)) result.problem = FolderWriteProblem(install / "data");
        if (result.problem.empty() && install.has_parent_path() && install.parent_path() != install) result.problem = FolderWriteProblem(install.parent_path());
        if (result.problem.empty()) pt::LogInfo("update: in-game update for this portable copy ({})", pt::os::PathToUtf8(install));
        else pt::LogWarn("update: this portable copy cannot update itself: {}; the Update button says so and the notice links the release page", result.problem);
#endif
        return result;
    }();
    return here;
}
bool InGameUpdate(const pt::update::Release& release) {
    if (!release.signed_update) return false;
    const UpdateHere& here = InGameUpdateHere();
    return here.kind != UpdateHere::Kind::None && here.problem.empty();
}
bool InGameUpdateBlocked(const pt::update::Release& release) {
    if (!release.signed_update) return false;
    const UpdateHere& here = InGameUpdateHere();
    return here.kind == UpdateHere::Kind::Portable && !here.problem.empty();
}

std::string UpdateVersionLabel(const pt::update::Release& release) {
    if (!InGameUpdate(release) || release.download_bytes == 0) return release.version;
    const uint64_t mb = (release.download_bytes + 999'999) / 1'000'000;
    return std::format("{} ({} MB)", release.version, mb);
}
std::string UpdateNoticeKey(const pt::update::Release& release) {
    return std::format("{}{}{}{}{}", InGameUpdate(release) ? "pc_update_available_auto" : "pc_update_available", pt::game::kPcNoteArgument,
                       UpdateVersionLabel(release), pt::game::kPcNoteArgument, pt::update::CurrentVersion());
}

bool g_update_mark_started = false;
std::filesystem::path g_update_backup;
bool g_fg_proxy_restored = false;
bool g_fg_proxy_reinstall = false;
enum class JournalAtStart { Nothing, Relaunch };
JournalAtStart UpdateJournalAtStart(const Options& options) {
    namespace uj = pt::update_journal;
    const std::filesystem::path file = UserDataDir() / uj::kName;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return JournalAtStart::Nothing;
    const auto journal = uj::Read(file);
    if (!journal) {
        pt::LogWarn("update: the journal {} is not valid; left as it is", pt::os::PathToUtf8(file));
        return JournalAtStart::Nothing;
    }
    switch (journal->state) {
    case uj::State::Swapped:
        g_update_mark_started = true;
        g_update_backup = journal->backup;
        pt::LogInfo("update: first start of version {} (from {}); the backup is kept until this start reaches its first frame", journal->new_version,
                    journal->old_version);
        return JournalAtStart::Nothing;
    case uj::State::Started:
        uj::RemoveFolders(*journal);
        std::filesystem::remove(file, ec);
        pt::LogInfo("update: version {} started before; its backup was removed", journal->new_version);
        return JournalAtStart::Nothing;
    case uj::State::Swapping:
        break;
    }
    if (journal->pid && pt::os::ProcessAlive(journal->pid)) {
        pt::LogWarn("update: an update of this install is running (process {}); the game starts as it is", journal->pid);
        return JournalAtStart::Nothing;
    }
    std::string why;
    if (!uj::Rollback(*journal, &why)) {
        pt::LogError("update: the interrupted update to {} could not be undone: {}", journal->new_version, why);
        return JournalAtStart::Nothing;
    }
    uj::RemoveFolders(*journal);
    std::filesystem::remove(file, ec);
    pt::LogWarn("update: the update to {} was interrupted; version {} was restored and starts again", journal->new_version, journal->old_version);
    if (options.headless) return JournalAtStart::Nothing;
    return pt::os::StartDetached(GameExecutable(), options.relaunch_args, pt::ExecutableDir()) ? JournalAtStart::Relaunch : JournalAtStart::Nothing;
}
void MarkUpdateStarted() {
    if (!g_update_mark_started) return;
    g_update_mark_started = false;
    const bool marked = pt::update_journal::SetState(UserDataDir() / pt::update_journal::kName, pt::update_journal::State::Started);
    pt::LogInfo("update: first frame of the new version {}", marked ? "marked" : "could not be marked");
}

bool StartUpdater(const App& app, int language) {
    std::filesystem::path install = pt::ExecutableDir();
    if (!install.has_filename()) install = install.parent_path();
    std::vector<std::string> args{"--game-update", pt::os::PathToUtf8(install), "--pid", std::to_string(pt::os::ProcessId()), "--lang",
                                  std::to_string(language), "--"};
    args.insert(args.end(), app.options.relaunch_args.begin(), app.options.relaunch_args.end());
    const bool started = pt::os::StartDetached(UpdaterExecutable(), args, pt::ExecutableDir());
    if (started) pt::LogInfo("update: {} started", pt::os::PathToUtf8(UpdaterExecutable()));
    else pt::LogError("update: {} could not be started", pt::os::PathToUtf8(UpdaterExecutable()));
    return started;
}

bool StartRouteQueueUpdater(const App& app, int language) {
    std::filesystem::path install = pt::ExecutableDir();
    if (!install.has_filename()) install = install.parent_path();
    const std::filesystem::path data = app.settings_path.empty() ? UserDataDir() : app.settings_path.parent_path();
    const std::string ops = app.routes.Serialize();
    std::vector<std::string> args{"--route-queue", pt::os::PathToUtf8(install), "--pid", std::to_string(pt::os::ProcessId()), "--lang",
                                  std::to_string(language), "--ops", ops, "--nr-work", pt::os::PathToUtf8(data / "dlss5-unlock"), "--fg-work",
                                  pt::os::PathToUtf8(data / "dlssg-unlock"), "--"};
    args.insert(args.end(), app.options.relaunch_args.begin(), app.options.relaunch_args.end());
    if (app.route_state_test) {
        pt::LogInfo("route queue: the test state would start pt-updater --route-queue with the operations {}", ops);
        return true;
    }
    const bool started = pt::os::StartDetached(UpdaterExecutable(), args, pt::ExecutableDir());
    if (started) pt::LogInfo("route queue: pt-updater --route-queue {} started", ops);
    else pt::LogError("route queue: pt-updater --route-queue {} could not be started", ops);
    return started;
}

struct UnlockRowTexts {
    const char* label;
    const char* unlock_action;
    const char* retry_action;
    const char* reinstall_action;
    const char* remove_action;
    const char* idle_note;
    const char* working_value;
    const char* working_note;
    const char* verifying_value;
    const char* verifying_note;
    const char* restart_value;
    const char* restart_install;
    const char* restart_reinstall;
    const char* restart_remove;
    const char* dlg_unlock;
    const char* dlg_reinstall;
    const char* dlg_remove;
    const char* reinstall_note;
    const char* remove_note;
};
constexpr UnlockRowTexts kFgUnlockTexts{"pc_fg_unlock", "pc_fg_unlock_action", "pc_fg_retry_action", "pc_fg_reinstall_action",
                                        "pc_fg_uninstall_action", "pc_note_fg_unlock", "pc_fg_unlock_working", "pc_note_fg_unlock_working",
                                        "pc_fg_unlock_verifying", "pc_note_fg_unlock_verifying", "pc_fg_unlock_restart",
                                        "pc_note_fg_unlock_restart", "pc_note_fg_reinstall_restart", "pc_note_fg_remove_restart",
                                        "pc_dlg_fg_unlock", "pc_dlg_fg_reinstall", "pc_dlg_fg_uninstall", "pc_note_fg_reinstall",
                                        "pc_note_fg_uninstall"};
constexpr UnlockRowTexts kNrUnlockTexts{"pc_nr_unlock", "pc_nr_unlock_action", "pc_nr_retry_action", "pc_nr_reinstall_action",
                                        "pc_nr_uninstall_action", "pc_note_nr_unlock", "pc_nr_unlock_working", "pc_note_nr_unlock_working",
                                        "pc_nr_unlock_verifying", "pc_note_nr_unlock_verifying", "pc_nr_unlock_restart",
                                        "pc_note_nr_unlock_restart", "pc_note_nr_reinstall_restart", "pc_note_nr_remove_restart",
                                        "pc_dlg_nr_unlock", "pc_dlg_nr_reinstall", "pc_dlg_nr_uninstall", "pc_note_nr_reinstall",
                                        "pc_note_nr_uninstall"};
constexpr UnlockRowTexts kNrDownloadTexts{"pc_nr_unlock", "pc_nr_download_action", "pc_nr_retry_action", "pc_nr_reinstall_action",
                                          "pc_nr_uninstall_action", "pc_note_nr_download", "pc_nr_unlock_working", "pc_note_nr_unlock_working",
                                          "pc_nr_unlock_verifying", "pc_note_nr_unlock_verifying", "pc_nr_unlock_restart",
                                          "pc_note_nr_unlock_restart", "pc_note_nr_reinstall_restart", "pc_note_nr_remove_restart",
                                          "pc_dlg_nr_download", "pc_dlg_nr_reinstall", "pc_dlg_nr_uninstall", "pc_note_nr_reinstall",
                                          "pc_note_nr_uninstall"};

struct UnlockRowState {
    bool installed = false;
    bool busy = false;
    int percent = 0;
    int requested = 0;
    std::string row_note;
    bool failed_at_run_time = false;
    int choice = 0;
};

pt::game::PcSettingRow UnlockRow(int id, const UnlockRowTexts& t, const UnlockRowState& st) {
    pt::game::PcSettingRow row;
    row.id = id;
    row.label = t.label;
    row.action = true;
    row.wrap = false;
    if (st.busy) {
        const bool verifying = st.percent >= 100;
        row.values = {verifying ? t.verifying_value : t.working_value};
        row.note = verifying ? std::string(t.verifying_note) : std::format("{}{}{}", t.working_note, pt::game::kPcNoteArgument, st.percent);
        row.enabled = false;
        return row;
    }
    if (st.requested != 0) {
        row.values = {t.restart_value};
        row.note = st.requested == 2 ? t.restart_remove : st.requested == 3 ? t.restart_reinstall : t.restart_install;
        row.enabled = false;
        return row;
    }
    if (!st.installed) {
        const bool failed = !st.row_note.empty();
        row.values = {failed ? t.retry_action : t.unlock_action};
        row.note = failed ? st.row_note : std::string(t.idle_note);
        row.confirm_note = t.dlg_unlock;
        row.confirm_default_yes = true;
        return row;
    }
    row.values = {t.reinstall_action, t.remove_action};
    row.value = std::clamp(st.choice, 0, 1);
    row.note = st.failed_at_run_time && !st.row_note.empty() ? st.row_note : std::string(row.value == 1 ? t.remove_note : t.reinstall_note);
    row.confirm_note = row.value == 1 ? t.dlg_remove : t.dlg_reinstall;
    row.confirm_default_yes = row.value == 0;
    return row;
}

bool OpenPatreonPage(App& app, std::string_view from) {
    using pt::game::support::kPatreonUrl;
    bool opened = false;
    if (std::getenv("PT_TEST_OPEN_URL_FAIL") != nullptr) {
        opened = false;
    } else if (!app.window) {
        pt::LogInfo("patreon: {} (headless, not opened; from {})", kPatreonUrl, from);
        opened = true;
    } else {
        if (WindowMode(app) == pt::display::kExclusive) SDL_MinimizeWindow(app.window);
        opened = pt::game::support::OpenSupportPage([](const char* url) { return SDL_OpenURL(url); });
        if (!opened) pt::LogWarn("patreon: the browser could not be opened: {}", SDL_GetError());
        else pt::LogInfo("patreon: opened {} (from {})", kPatreonUrl, from);
    }
    if (!opened) app.pending_notices.push_back("pc_notice_patreon_failed");
    return opened;
}

pt::game::CrosshairStyle CrosshairOf(const pt::AppSettings::Crosshair& c) {
    pt::game::CrosshairStyle style;
    style.shape = std::clamp(c.shape, 0, 6);
    style.color = std::clamp(c.color, 0, 6);
    style.length = static_cast<float>(std::clamp(c.length, 1, 20));
    style.thickness = 0.5f * static_cast<float>(std::clamp(c.thickness, 1, 10));
    style.gap = static_cast<float>(std::clamp(c.gap, 0, 20));
    style.dynamic = c.dynamic;
    style.dot = c.dot;
    style.outline = c.outline;
    style.opacity = 0.1f * static_cast<float>(std::clamp(c.opacity, 1, 10));
    style.hit_marker = c.hit_marker;
    style.behind_sights = c.behind_sights;
    return style;
}

void ApplyCrosshair(pt::game::Game& game, const pt::AppSettings& settings) { game.SetCrosshair(CrosshairOf(settings.crosshair)); }

class PcSettings final : public pt::game::PcSettingsSource {
public:
    PcSettings(App& app, pt::game::Game& game, pt::InputDevice& input) : app_(app), game_(game), input_(input) {}
    ~PcSettings() override {
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
        if (page_ == kCrosshairPage) return CrosshairSections();
        if (page_ == kKeyBindingsPage) return KeyBindingsSections();
        if (page_ == kNightPage) {
            pt::game::PcSettingSection night{"pc_survival_choose", 0, {}};
            static constexpr std::array<std::tuple<int, const char*, const char*>, 4> kLevels{{
                {kNightNormal, "pc_survival_normal", "pc_note_survival_pick_normal"},
                {kNightHard, "pc_survival_hard", "pc_note_survival_pick_hard"},
                {kNightNightmare, "pc_survival_nightmare", "pc_note_survival_pick_nightmare"},
                {kNightInsanity, "pc_survival_insanity", "pc_note_survival_pick_insanity"},
            }};
            for (const auto& [id, label, note] : kLevels) {
                auto row = Row(id, label, {""}, 0, note);
                row.link = true;
                night.rows.push_back(std::move(row));
            }
            pt::game::PcSettingSection runs{"pc_survival_runs", 0, {}};
            const pt::game::SurvivalProfile& profile = game_.SurvivalProfileData();
            static constexpr std::array<std::pair<int, const char*>, 4> kKept{{
                {kNightContinueNormal, "pc_survival_continue_normal"},
                {kNightContinueHard, "pc_survival_continue_hard"},
                {kNightContinueNightmare, "pc_survival_continue_nightmare"},
                {kNightContinueInsanity, "pc_survival_continue_insanity"},
            }};
            for (size_t d = 1; d <= 4; ++d) {
                const pt::game::SurvivalCheckpoint& kept = profile.checkpoints[d];
                if (kept.wave <= 0) continue;
                const int kept_night = pt::game::SurvivalNightOf(kept.wave);
                auto row = Row(kKept[d - 1].first, kKept[d - 1].second, {std::to_string(kept.wave)}, 0,
                               kept_night > 1 ? std::format("pc_note_survival_continue_night{}{}", pt::game::kPcNoteArgument, kept_night)
                                              : std::format("pc_note_survival_continue{}{}{}{}", pt::game::kPcNoteArgument, kept.wave, pt::game::kPcNoteArgument, kept.score));
                row.link = true;
                runs.rows.push_back(std::move(row));
            }
            {
                const int today_date = pt::game::SurvivalToday();
                const bool today = profile.daily_date == today_date;
                const pt::game::DailyMutatorDef& mut = pt::game::DailyMutatorForDate(today_date);
                const char arg = pt::game::kPcNoteArgument;
                const std::string val = today && profile.daily_wave > 0 ? std::format("Wave {}", profile.daily_wave) : std::string(mut.name_key);
                const std::string note = std::format("pc_note_survival_daily_mut{}{}{}{}{}{}{}{}", arg, mut.name_key, arg, mut.note_key, arg,
                                                     profile.daily_wins, arg, profile.daily_streak);
                auto row = Row(kNightDaily, "pc_survival_daily", {val}, 0, note);
                row.link = true;
                runs.rows.push_back(std::move(row));
            }
            pt::game::PcSettingSection gear{"pc_survival_gear", 0, {}};
            {
                auto armory = Row(kNightArmory, "pc_armory_title", {std::format("${}", profile.credits)}, 0, "pc_note_armory_open");
                armory.link = true;
                gear.rows.push_back(std::move(armory));
            }
            auto crosshair = Row(kNightCrosshair, "pc_crosshair", {""}, 0, "pc_note_survival_pick_crosshair");
            crosshair.link = true;
            gear.rows.push_back(std::move(crosshair));
            return {std::move(night), std::move(runs), std::move(gear)};
        }
        if (page_ == kArmoryPage) {
            return {};
        }
        if (page_ == kSurvivalPromoPage) {
            pt::game::PcSettingSection promo{"pc_survival", 0, {}};
            auto play = Row(kPromoPlay, "pc_survival_promo_yes", {""}, 0, "pc_note_loop_survival");
            play.link = true;
            promo.rows.push_back(std::move(play));
            auto later = Row(kPromoLater, "pc_update_no", {""}, 0, "");
            later.link = true;
            promo.rows.push_back(std::move(later));
            return {std::move(promo)};
        }
        if (page_ == kUpdatePage) {
            pt::game::PcSettingSection question{"pc_update_title", 0, {}};
            const auto newer = app_.updates.Newer();
            auto yes = Row(kUpdateYes, "pc_update_yes", {""}, 0,
                           newer ? std::format("pc_note_update_now{}{}", pt::game::kPcNoteArgument, newer->version) : std::string("pc_note_update_later"));
            yes.link = true;
            yes.enabled = !app_.update_requested;
            question.rows.push_back(std::move(yes));
            auto later = Row(kUpdateLater, "pc_update_no", {""}, 0, "pc_note_update_later");
            later.link = true;
            question.rows.push_back(std::move(later));
            return {std::move(question)};
        }
        if (page_ == kStreetPage && game_.SpeedrunResultsPending()) return SpeedrunResults();
        if (page_ == kStreetPage) {
            pt::game::PcSettingSection street{"pc_street_question", 0, {}};
            auto walk = game_.SurvivalWithWalk() ? Row(kStreetSurvival, "pc_street_survival", {""}, 0, "pc_note_street_survival")
                                                 : Row(kStreetWalk, "pc_street_walk", {""}, 0, "pc_note_street_walk");
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
                if (game_.SurvivalActive()) {
                    auto survive = Row(kSurvival, "pc_survival", {"pc_extras_stop"}, 0, "pc_note_survival");
                    survive.link = true;
                    extras.rows.push_back(std::move(survive));
                }
            }
            const bool locked = app_.options.release_locks && !game_.GameFinished();
            auto browser = Row(kLoopBrowser, "pc_loop_browser", {"pc_graphics_value"}, 0,
                               locked ? "pc_note_loop_browser_locked" : "pc_note_loop_browser_entry");
            browser.link = true;
            extras.rows.push_back(std::move(browser));
            auto archive = Row(kArchive, "pc_archive", {"pc_graphics_value"}, 0, locked ? "pc_note_archive_gate" : "pc_note_archive");
            archive.link = true;
            extras.rows.push_back(std::move(archive));
            if (game_.SurvivalAvailable() && !game_.SurvivalActive()) {
                auto night = Row(kLoopNight, "pc_survival", {"pc_graphics_value"}, 0, "pc_note_loop_survival");
                night.link = true;
                extras.rows.push_back(std::move(night));
            }
            auto freecam = Row(kFreecam, "pc_freecam", {app_.freecam_active ? "pc_extras_stop" : "pc_extras_start"}, 0, "pc_note_freecam");
            freecam.link = true;
            freecam.enabled = app_.window != nullptr && !app_.xr;
            extras.rows.push_back(std::move(freecam));
            auto photo = Row(kPhotoMode, "pc_photo_mode", {app_.photo_active ? "pc_extras_stop" : "pc_extras_start"}, 0, "pc_note_photo_mode");
            photo.link = true;
            photo.enabled = app_.window != nullptr && !app_.xr;
            extras.rows.push_back(std::move(photo));
            extras.rows.push_back(Row(kThirdPerson, "pc_third_person", OffOn(), game_.ThirdPerson() ? 1 : 0, "pc_note_third_person"));
            if (game_.GamePlus()) {
                extras.rows.push_back(Row(kGamePlus, "pc_game_plus", OffOn(), app_.settings.game.game_plus ? 1 : 0, "pc_note_game_plus"));
            }
            const int ng_done = game_.NightUnlocked()
                                    ? pt::game::Game::kTierSurvival
                                    : std::clamp(static_cast<int>(game_.Finishes()) - 1, 0, pt::game::Game::kTierSurvival - 1);
            auto ng_state = Row(kNgPlusState, "pc_ng_plus_state",
                                {std::format("{}/{}", ng_done, pt::game::Game::kTierSurvival)}, 0, "pc_note_ng_plus_state");
            ng_state.wrap = false;
            extras.rows.push_back(std::move(ng_state));
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
                lock.confirm_note = "pc_dlg_loop_lock";
                lock.enabled = game_.GameFinished() || game_.BrowseReached() != 0;
                extras.rows.push_back(std::move(lock));
            }
            std::vector<pt::game::PcSettingSection> out;
            out.push_back(std::move(extras));
            AddProgressAndSupport(out);
            return out;
        }
        if (page_ == kHdrPage) return HdrSections();
        if (page_ == kVrPage) {
            const pt::AppSettings::Vr& v = app_.settings.vr;
            pt::game::PcSettingSection vr{"pc_vr", 0, {}};
            vr.rows.push_back(Row(kVrMode, "pc_vr_mode", OffOn(), v.enabled ? 1 : 0, "pc_note_vr_mode"));
            vr.rows.push_back(Row(kVrFlashlight, "pc_vr_flashlight", {"pc_vr_head", "pc_vr_controller"}, v.flashlight, "pc_note_vr_flashlight"));
            vr.rows.push_back(Row(kVrTurn, "pc_vr_turn", {"pc_vr_snap", "pc_vr_smooth"}, v.turn, "pc_note_vr_turn"));
            auto height=Row(kVrHeight,"pc_vr_height",{},std::clamp(static_cast<int>(std::lround(v.height_offset/0.05f))+10,0,20),"pc_note_vr_height");
            for(int i=0;i<=20;++i) height.values.push_back(std::format("{:+d} cm",(i-10)*5));
            height.wrap=false; vr.rows.push_back(std::move(height));
            auto scale=Row(kVrWorldScale,"pc_vr_world_scale",{},std::clamp(static_cast<int>(std::lround((v.world_size-0.5f)/0.05f)),0,30),"pc_note_vr_world_scale");
            for(int i=0;i<=30;++i) scale.values.push_back(std::format("{}%",50+i*5));
            scale.wrap=false; vr.rows.push_back(std::move(scale));
            return {std::move(vr)};
        }
        if (page_ == kAccessibilityPage) {
            const pt::AppSettings::Accessibility& a = app_.settings.accessibility;
            pt::game::PcSettingSection access{"pc_accessibility", 0, {}};
            access.rows.push_back(Row(kSoundCaptions, "pc_sound_captions", OffOn(), a.sound_captions ? 1 : 0, "pc_note_sound_captions"));
            access.rows.push_back(Row(kSubtitleSize, "pc_subtitle_size",
                                      {"pc_subtitle_size_normal", "pc_subtitle_size_large", "pc_subtitle_size_xlarge"},
                                      std::clamp(a.subtitle_size, 0, 2), "pc_note_subtitle_size"));
            access.rows.push_back(Row(kSubtitleBg, "pc_subtitle_bg",
                                      {"pc_off", "pc_subtitle_bg_subtle", "pc_subtitle_bg_dark"},
                                      std::clamp(a.subtitle_bg, 0, 2), "pc_note_subtitle_bg"));
            access.rows.push_back(Row(kReducedFlashing, "pc_reduced_flashing", OffOn(), a.reduced_flashing ? 1 : 0, "pc_note_reduced_flashing"));
            access.rows.push_back(Row(kHouseSprint, "pc_house_sprint", OffOn(), a.house_sprint ? 1 : 0, "pc_note_house_sprint"));
            access.rows.push_back(Row(kSprintMode, "pc_sprint_mode", {"pc_input_hold", "pc_input_toggle"}, a.sprint_toggle ? 1 : 0, "pc_note_sprint_mode"));
            access.rows.push_back(Row(kZoomMode, "pc_zoom_mode", {"pc_input_hold", "pc_input_toggle"}, a.zoom_toggle ? 1 : 0, "pc_note_zoom_mode"));
            access.rows.push_back(Row(kFpsCounter, "pc_fps_counter", OffOn(), a.fps_counter ? 1 : 0, "pc_note_fps_counter"));
            access.rows.push_back(Row(kSupportPrompts, "pc_support_prompts", OffOn(), app_.settings.ui.support_prompts ? 1 : 0, "pc_note_support_prompts"));
            access.rows.push_back(Row(kGyro, "pc_gyro", {"pc_off", "pc_gyro_camera", "pc_gyro_flashlight"}, std::clamp(a.gyro, 0, 2),
                                      a.gyro == 1 ? "pc_note_gyro_camera" : a.gyro == 2 ? "pc_note_gyro_flashlight" : "pc_note_gyro"));
            pt::game::PcSettingRow sensitivity = Row(kGyroSensitivity, "pc_gyro_sensitivity", {}, Nearest(kGyroSensitivitySteps, a.gyro_sensitivity),
                                                     "pc_note_gyro_sensitivity");
            for (const float step : kGyroSensitivitySteps) sensitivity.values.push_back(std::format("{:.2f}", step));
            sensitivity.wrap = false;
            sensitivity.enabled = a.gyro != 0;
            access.rows.push_back(std::move(sensitivity));
            {
                const auto state = input_.GyroCalibration();
                const bool failed = gyro_calibrate_no_pad_ || state == pt::GyroTracker::Calibration::Failed;
                const char* value = failed ? "pc_gyro_calibrate_failed" : state == pt::GyroTracker::Calibration::Running ? "pc_gyro_calibrating"
                                    : state == pt::GyroTracker::Calibration::Done ? "pc_gyro_calibrated" : "pc_gyro_calibrate_value";
                std::string note = failed ? "pc_note_gyro_calibrate_failed" : state == pt::GyroTracker::Calibration::Done ? "pc_note_gyro_calibrated"
                                   : "pc_note_gyro_calibrate";
                if (!failed && state == pt::GyroTracker::Calibration::Running) {
                    note = std::format("pc_note_gyro_calibrating{}{}", pt::game::kPcNoteArgument,
                                       static_cast<int>(std::lround(input_.GyroCalibrationProgress() * 100.0f)));
                }
                pt::game::PcSettingRow calibrate = Row(kGyroCalibrate, "pc_gyro_calibrate", {value}, 0, note);
                calibrate.action = true;
                calibrate.wrap = false;
                calibrate.confirm_note = "pc_dlg_gyro_calibrate";
                calibrate.confirm_default_yes = true;
                calibrate.enabled = a.gyro != 0 && state != pt::GyroTracker::Calibration::Running;
                access.rows.push_back(std::move(calibrate));
            }
            pt::game::PcSettingRow fov = Row(kFieldOfView, "pc_fov", {}, FovStep(a.fov_offset), "pc_note_fov");
            for (const int step : kFovSteps) fov.values.push_back(step == 0 ? "pc_rumble_original" : std::format("{:+d}", step));
            fov.wrap = false;
            access.rows.push_back(std::move(fov));
            return {std::move(access)};
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
            test.rows.push_back(Row(kMicHeard, "pc_mic_heard", {app_.microphone_hypothesis.empty() ? app_.microphone_reason : app_.microphone_hypothesis}, 0, app_.microphone_status));
            test.rows.push_back(Row(kMicMonitor, "pc_mic_monitor", OffOn(), app_.microphone_monitor ? 1 : 0, "pc_mic_monitor_note"));
            return {std::move(test)};
        }
        const pt::AppSettings& s = app_.settings;
        pt::game::PcSettingSection display{"pc_section_display", 0, {}};
        const int mode = WindowMode(app_);
        display.rows.push_back(Row(kDisplayMode, "pc_display_mode", {"pc_window", "pc_borderless", "pc_fullscreen"}, mode, "pc_note_display_mode"));
        const char* resolution_note = mode == 0 ? "pc_note_resolution" : "pc_note_resolution_fullscreen";
        pt::game::PcSettingRow resolution = Row(kResolution, "pc_resolution", {}, 0, resolution_note);
        if (mode == pt::display::kBorderless) {
            int pw = 0, ph = 0;
            const bool is_fs = app_.window && (SDL_GetWindowFlags(app_.window) & SDL_WINDOW_FULLSCREEN) != 0;
            if (is_fs) SDL_GetWindowSizeInPixels(app_.window, &pw, &ph);
            const glm::ivec2 desk = DesktopSize();
            const int dw = (is_fs && pw > 0 && ph > 0) ? pw : desk.x;
            const int dh = (is_fs && pw > 0 && ph > 0) ? ph : desk.y;
            resolution.values.push_back(std::format("{} x {}", dw, dh));
            resolution.value = 0;
            resolution.enabled = false;
        } else {
            const std::vector<glm::ivec2> sizes = WindowSizes();
            for (size_t i = 0; i < sizes.size(); ++i) {
                resolution.values.push_back(std::format("{} x {}", sizes[i].x, sizes[i].y));
                if (sizes[i] == glm::ivec2(s.display.width, s.display.height)) resolution.value = static_cast<int>(i);
            }
            resolution.enabled = true;
            resolution.dropdown = resolution.values.size() > 1;
        }
        resolution.wrap = false;
        display.rows.push_back(std::move(resolution));
        {
            const std::vector<double> rates = RefreshRates();
            pt::game::PcSettingRow refresh = Row(kRefreshRate, "pc_refresh_rate", {}, 0,
                                                 mode == pt::display::kExclusive ? "pc_note_refresh_rate" : "pc_note_refresh_rate_desktop");
            if (mode == pt::display::kExclusive && !rates.empty()) {
                refresh.values.push_back("pc_refresh_auto");
                for (size_t i = 0; i < rates.size(); ++i) {
                    refresh.values.push_back(pt::RefreshRateText(rates[i]));
                    if (s.display.refresh_rate > 0.0f && pt::SameRefreshRate(rates[i], s.display.refresh_rate)) refresh.value = static_cast<int>(i) + 1;
                }
            } else {
                const SDL_DisplayMode* desktop = app_.window ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(app_.window)) : nullptr;
                refresh.values.push_back(desktop && desktop->refresh_rate > 0.0f ? pt::RefreshRateText(desktop->refresh_rate)
                                                                                 : std::string("pc_refresh_auto"));
                refresh.enabled = false;
            }
            refresh.wrap = false;
            display.rows.push_back(std::move(refresh));
        }
        const bool dlssg_vsync = pt::streamline::Active() && pt::streamline::FrameGenNeedsVsyncOff() &&
                                 app_.scene.upscale.frame_generation == pt::FrameGenKind::Dlss;
        const bool metalfx_vsync = app_.scene.upscale.frame_generation == pt::FrameGenKind::Metalfx;
        display.rows.push_back(Row(kVsync, "pc_vsync", OffOn(), s.display.vsync ? 1 : 0,
                                   dlssg_vsync ? "pc_note_vsync_dlssg"
                                   : metalfx_vsync ? "pc_note_vsync_metalfx"
                                   : s.display.vsync ? "pc_note_vsync_refresh" : "pc_note_vsync_off"));
        pt::game::PcSettingRow cap = Row(kFpsLimit, "pc_fps_limit", {"pc_unlimited","30","60","90","120","144","165","240","360"}, 0, "pc_note_fps_limit");
        constexpr int fps_caps[] = {0,30,60,90,120,144,165,240,360};
        for (int i=0;i<9;++i) if (fps_caps[i]==s.display.fps_limit) cap.value=i;
        if (s.display.fps_limit>0 && cap.value==0) { cap.values.push_back(std::to_string(s.display.fps_limit)); cap.value=9; }
        cap.wrap=false;
        display.rows.push_back(std::move(cap));
        const bool hdr_available = app_.display_hdr != 0 || (s.display.hdr_windows_switch && app_.display_hdr_capable);
        pt::game::PcSettingRow hdr = Row(kHdr, "pc_hdr", OffOn(), s.display.hdr && hdr_available ? 1 : 0,
                                         hdr_available ? "pc_note_hdr_restart" : "pc_note_hdr_windows_off");
        hdr.enabled = hdr_available;
        display.rows.push_back(std::move(hdr));
        if (app_.display_hdr_capable) {
            display.rows.push_back(Row(kHdrWindowsSwitch, "pc_hdr_windows_switch", OffOn(), s.display.hdr_windows_switch ? 1 : 0, "pc_note_hdr_windows_switch"));
        }
        if (s.display.hdr && hdr_available) {
            auto hdr_page = Row(kHdrSettings, "pc_hdr_settings", {"pc_graphics_value"}, 0, "pc_note_hdr_settings");
            hdr_page.link = true;
            display.rows.push_back(std::move(hdr_page));
        }
        if (pt::RenderingApiSelectable()) {
            pt::RenderingApi api = pt::RenderingApi::Vulkan;
            pt::ParseRenderingApi(s.display.rendering_api, api);
            pt::game::PcSettingRow rendering_api = Row(kRenderingApi, "pc_rendering_api",
                                                       {std::string(pt::RenderingApiName(pt::RenderingApi::D3D12)),
                                                        std::string(pt::RenderingApiName(pt::RenderingApi::Vulkan))},
                                                       api == pt::RenderingApi::D3D12 ? 0 : 1, "pc_note_rendering_api");
            std::string reason;
            if (app_.d3d12_crash_stage) {
                rendering_api.value_warnings = {"pc_note_d3d12_crashed_retry", std::string()};
            } else if (!pt::D3D12Usable(reason)) {
                rendering_api.value_notes = {Sentence(reason), std::string()};
            }
            display.rows.push_back(std::move(rendering_api));
        }
        if (page_ == kDisplayPage) return {std::move(display)};

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
        const std::vector<pt::FrameGenKind> generators = FrameGenerators();
        pt::game::PcSettingRow frame_generation = Row(kFrameGeneration, "pc_frame_generation", {}, 0, "pc_note_frame_generation");
        for (const pt::FrameGenKind kind : generators) {
            if (kind == u.frame_generation) frame_generation.value = static_cast<int>(frame_generation.values.size());
            frame_generation.values.push_back(FrameGenName(kind));
        }
        std::string fg_reason;
        pt::FrameGeneration* fg = pt::UpscaleHost::Get().FrameGen();
        std::string fsr_note;
        if (!fg) {
#if defined(__linux__)
            fsr_note = Sentence("AMD's FSR 3 frame generation for Vulkan runs on Windows only");
#else
            fsr_note = "pc_note_frame_generation_missing";
#endif
        } else if (!fg->Available(fg_reason)) {
            fsr_note = fg_reason.starts_with("needs an AMD Radeon RX 5000") ? "pc_note_fsr_fg_gpu" : Sentence(fg_reason);
        }
        const pt::DlssFrameGenSupport& dlss_fg = pt::UpscaleHost::Get().DlssFrameGen();
        std::string dlss_note = dlss_fg.hardware && dlss_fg.built ? std::string() : dlss_fg.note;
        if (app_.fg_applicable && !app_.fg_install_usable) dlss_note = "pc_note_fg_unlock_first";
        std::string dlss_warning;
        if (app_.dlssg_crash_hold && dlss_note == "pc_note_dlssg_failed") {
            dlss_note.clear();
            dlss_warning = "pc_note_dlssg_crashed_retry";
        }
        if (pt::streamline::Active() && fsr_note.empty()) {
            fsr_note = "pc_note_fsr_fg_restart";
        }
        std::string metalfx_note;
        if (pt::FrameGeneration* metalfx_fg = pt::UpscaleHost::Get().MetalFxFrameGen()) {
            std::string metalfx_reason;
            if (!metalfx_fg->Available(metalfx_reason)) {
                metalfx_note = Sentence(metalfx_reason);
            }
        } else {
            metalfx_note = "pc_note_frame_generation_missing";
        }
        std::string xefg_note;
        if (pt::FrameGeneration* xefg_fg = pt::UpscaleHost::Get().XefgFrameGen()) {
            std::string xefg_reason;
            if (!xefg_fg->Available(xefg_reason)) {
                const char* key = pt::XefgNoteKey(xefg_reason);
                xefg_note = *key ? std::string(key) : Sentence(xefg_reason);
            }
        } else {
            xefg_note = "pc_note_frame_generation_missing";
        }
#ifdef PT_D3D12_GAME
        if (app_.renderer.Backend() == pt::gpu::Backend::D3D12) {
            const std::string fsr_reason = app_.renderer.FrameGenReasonD3D12(pt::gpu::d3d12::FrameGenOwner::Fsr);
            const std::string xefg_reason = app_.renderer.FrameGenReasonD3D12(pt::gpu::d3d12::FrameGenOwner::Xess);
            const char* xefg_key = pt::XefgNoteKey(xefg_reason);
            fsr_note = fsr_reason.empty()                                       ? std::string()
                       : fsr_reason.starts_with("needs an AMD Radeon RX 5000") ? std::string("pc_note_fsr_fg_gpu")
                                                                                : Sentence(fsr_reason);
            xefg_note = xefg_reason.empty() ? std::string() : *xefg_key ? std::string(xefg_key) : Sentence(xefg_reason);
        }
#endif
        for (const pt::FrameGenKind kind : generators) {
            frame_generation.value_notes.push_back(kind == pt::FrameGenKind::Fsr       ? fsr_note
                                                   : kind == pt::FrameGenKind::Dlss    ? dlss_note
                                                   : kind == pt::FrameGenKind::Metalfx ? metalfx_note
                                                   : kind == pt::FrameGenKind::Xess    ? xefg_note
                                                                                       : std::string());
            frame_generation.value_warnings.push_back(kind == pt::FrameGenKind::Dlss ? dlss_warning : std::string());
        }
        if (u.frame_generation == pt::FrameGenKind::Dlss) {
            frame_generation.note = pt::streamline::Active() ? "pc_note_frame_generation_dlss" : "pc_note_dlssg_restart";
        } else if (u.frame_generation == pt::FrameGenKind::Metalfx) {
            frame_generation.note = "pc_note_frame_generation_metalfx";
        } else if (u.frame_generation == pt::FrameGenKind::Xess) {
            frame_generation.note = "pc_note_frame_generation_xess";
        }
        if (u.frame_generation == pt::FrameGenKind::Dlss) {
            frame_generation.note = pt::streamline::Active() ? "pc_note_frame_generation_dlss" : "pc_note_dlssg_restart";
        }
        if (off) {
            frame_generation.note = "pc_note_needs_upscaler";
            frame_generation.enabled = false;
            frame_generation.value = 0;
        }
        if (app_.FgPending() != 0) {
            frame_generation.enabled = false;
            frame_generation.note = app_.FgPending() == 2 ? "pc_note_fg_remove_restart"
                                    : app_.FgPending() == 3 ? "pc_note_fg_reinstall_restart" : "pc_note_fg_unlock_restart";
        }
        pt::game::PcSettingSection generation{"pc_frame_generation", 0, {}};
        if (generators.size() > 1) generation.rows.push_back(std::move(frame_generation));
        if (u.frame_generation == pt::FrameGenKind::Dlss) {
            const unsigned max_multiplier = pt::streamline::FrameGenMaxMultiplier();
            auto multiplier = Row(kDlssgMultiplier, "pc_dlssg_multiplier", {"2x", "3x", "4x"},
                static_cast<int>(pt::streamline::ClampFrameGenMultiplier(u.dlssg_multiplier, max_multiplier - 1)) - 2,
                "pc_note_dlssg_multiplier");
            multiplier.enabled = !off && dlss_fg.hardware && dlss_fg.built && pt::streamline::Active() && app_.FgPending() == 0;
            multiplier.value_notes = {std::string(), max_multiplier < 3 ? "pc_note_dlssg_multiplier_limit" : "",
                max_multiplier < 4 ? "pc_note_dlssg_multiplier_limit" : ""};
            if (!multiplier.enabled) multiplier.note = off ? "pc_note_needs_upscaler" :
                !dlss_warning.empty() ? dlss_warning : !dlss_fg.note.empty() ? dlss_fg.note : "pc_note_dlssg_restart";
            generation.rows.push_back(std::move(multiplier));
        }
        if (app_.fg_applicable) {
            UnlockRowState st;
            st.installed = app_.fg_install_usable;
            st.busy = app_.fg_job.Busy() || app_.fg_test_percent >= 0;
            st.percent = app_.fg_test_percent >= 0 ? app_.fg_test_percent : app_.fg_job.Get().percent;
            st.requested = app_.FgPending();
            st.row_note = app_.fg_row_note;
            st.failed_at_run_time = app_.fg_route_failed;
            st.choice = app_.fg_unlock_choice;
            auto row = UnlockRow(kFgUnlock, kFgUnlockTexts, st);
            if (app_.renderer.Backend() != pt::gpu::Backend::D3D12 && !st.busy && st.requested == 0) {
                row.note = "pc_note_fg_unlock_d3d12";
                if (!st.installed) row.enabled = false;
            }
            generation.rows.push_back(std::move(row));
        }
        pt::game::PcSettingSection neural_section{"pc_section_dlss5", 0, {}};
        {
            const auto& nu = app_.settings.upscaling;
            const pt::NeuralRenderingSupport nr = NeuralSupport(app_);
            pt::NeuralRenderingKind nr_kind = pt::NeuralRenderingKind::Off;
            pt::ParseNeuralRendering(nu.neural_rendering, nr_kind);
            pt::NeuralStyle style = pt::NeuralStyle::Default;
            pt::ParseNeuralStyle(nu.neural_style, style);
            const bool is_rtx50 = app_.nr_gpu == pt::nr_unlock::GpuClass::Native;
            const bool route = UsesNrRoute(app_.nr_gpu);
            const bool dlss = NrUpscalerReady(app_);
            const bool nr_on = nr.available && dlss && nr_kind == pt::NeuralRenderingKind::Dlss5 && !app_.nr_route_failed && !app_.nr_crash_hold;
            auto neural = Row(kNeuralRendering, "pc_neural_rendering", OffOn(), nr_on ? 1 : 0, "pc_note_neural_rendering");
            std::string reason;
            if (app_.nr_route_failed && !app_.nr_row_note.empty()) reason = app_.nr_row_note;
            else if (!nr.available) reason = route && !app_.nr_install_usable ? (is_rtx50 ? "pc_note_nr_download_first" : "pc_note_nr_unlock_first") : nr.note;
            else if (!dlss) reason = "pc_note_nr_needs_dlss";
            if (!reason.empty()) {
                neural.enabled = false;
                neural.note = reason;
            } else if (app_.nr_hook_failed || NeuralRouteNeedsRestart(app_)) neural.note = "pc_note_nr_next_start";
            if (app_.NrPending() != 0) {
                neural.enabled = false;
                neural.note = app_.NrPending() == 2 ? "pc_note_nr_remove_restart"
                              : app_.NrPending() == 3 ? "pc_note_nr_reinstall_restart" : "pc_note_nr_unlock_restart";
            }
            neural_section.rows.push_back(std::move(neural));
            if (nr_on && app_.NrPending() == 0) {
                neural_section.rows.push_back(Row(kNeuralStyle, "pc_neural_style", {"pc_nr_style_default", "pc_nr_style_natural", "pc_nr_style_cinematic"},
                                                  static_cast<int>(style), "pc_note_neural_style"));
                neural_section.rows.push_back(Row(kNeuralModel, "pc_neural_model", {"pc_nr_model_small", "pc_nr_model_large"},
                                                  nu.neural_model == "small" ? 0 : 1, "pc_note_neural_model"));
                auto quality = Row(kNeuralQuality, "pc_neural_quality", {"pc_quality_performance", "pc_quality_quality", "pc_preset_ultra"},
                                   NeuralQualityIndex(nu.neural_quality), "pc_note_neural_quality");
                quality.wrap = false;
                neural_section.rows.push_back(std::move(quality));
                auto strength = Row(kNeuralIntensity, "pc_neural_intensity", {"25%", "50%", "75%", "100%", "125%", "150%", "175%", "200%", "225%", "250%", "275%", "300%"},
                                    std::clamp(static_cast<int>(std::lround(nu.neural_intensity * 4.0f)) - 1, 0, 11), "pc_note_neural_intensity");
                strength.wrap = false;
                neural_section.rows.push_back(std::move(strength));
            }
            if (route) {
                UnlockRowState st;
                st.installed = app_.nr_install_usable;
                st.busy = app_.nr_job.Busy() || app_.nr_native_job.Busy() || app_.nr_test_percent >= 0;
                st.percent = app_.nr_test_percent >= 0 ? app_.nr_test_percent
                             : app_.nr_native_job.Busy() ? app_.nr_native_job.Get().percent : app_.nr_job.Get().percent;
                st.requested = app_.nr_native_remove_pending ? 2 : app_.NrPending();
                st.row_note = app_.nr_row_note;
                st.failed_at_run_time = app_.nr_route_failed;
                st.choice = app_.nr_unlock_choice;
                neural_section.rows.push_back(UnlockRow(kNeuralUnlock, is_rtx50 ? kNrDownloadTexts : kNrUnlockTexts, st));
            }
        }
        if (page_ == kUpscalingPage) {
            std::vector<pt::game::PcSettingSection> out;
            out.push_back(std::move(upscaling));
            if (!generation.rows.empty()) out.push_back(std::move(generation));
            out.push_back(std::move(neural_section));
            return out;
        }
        pt::game::PcSettingSection sound{"pc_section_sound", 1, {}};
        pt::game::PcSettingRow volume = Row(kVolume, "pc_volume", {}, std::clamp(static_cast<int>(std::lround(s.audio.volume * 10.0f)), 0, 20), "");
        for (int i = 0; i <= 20; ++i) {
            volume.values.push_back(std::format("{}%", i * 10));
        }
        volume.wrap = false;
        sound.rows.push_back(std::move(volume));
        {
            pt::game::PcSettingRow surround = Row(kSurround, "pc_surround", {"pc_off", "pc_on", "pc_surround_spatial"},
                                                  s.audio.spatial ? 2 : s.audio.surround ? 1 : 0, "pc_note_surround");
            if (s.audio.spatial) surround.note = "pc_note_surround_spatial";
            sound.rows.push_back(std::move(surround));
        }
        {
            const std::vector<std::string> outputs = OutputDevices();
            std::string default_name = pt::devices::AudioStarting() ? std::string() : pt::audio::SystemDefaultPlaybackName();
            std::string playing = app_.audio_output_name.empty() ? default_name : app_.audio_output_name;
            if (const char* test_default = std::getenv("PT_TEST_OUTPUT_DEFAULT"); test_default && *test_default) {
                default_name = playing = test_default;
            }
            const bool default_controller = pt::audio::ControllerEndpointKind(default_name) != pt::audio::ControllerEndpoint::None;
            pt::game::PcSettingRow output = Row(kOutputDevice, "pc_output_device",
                                                {default_controller ? "pc_system_default_controller" : "pc_system_default"}, 0, "");
            for (size_t i = 0; i < outputs.size(); ++i) {
                output.values.push_back(ShortDeviceName(outputs[i]));
                if (outputs[i] == s.audio.output_device) output.value = static_cast<int>(i) + 1;
            }
            const bool missing = !s.audio.output_device.empty() && output.value == 0;
            if (missing) {
                output.values.push_back(ShortDeviceName(s.audio.output_device));
                output.value = static_cast<int>(output.values.size()) - 1;
            }
            const char arg = pt::game::kPcNoteArgument;
            if (missing) {
                output.note = std::format("pc_note_output_device_missing{}{}{}{}", arg, s.audio.output_device, arg, playing);
            } else if (pt::audio::ControllerEndpointKind(playing) != pt::audio::ControllerEndpoint::None) {
                output.note = std::format("pc_note_output_device_controller{}{}", arg, playing);
            } else {
                output.note = std::format("pc_note_output_device{}{}", arg, default_name.empty() ? std::string("-") : default_name);
            }
            sound.rows.push_back(std::move(output));
        }
        {
            pt::game::PcSettingRow speaker = Row(kControllerSpeaker, "pc_controller_speaker", OffOn(), s.audio.controller_speaker ? 1 : 0,
                                                 "pc_note_controller_speaker");
            if (s.audio.controller_speaker &&
                pt::audio::ControllerEndpointKind(app_.audio_output_name) == pt::audio::ControllerEndpoint::DualSense) {
                speaker.note = std::format("pc_note_output_device_controller{}{}", pt::game::kPcNoteArgument, app_.audio_output_name);
            }
            sound.rows.push_back(std::move(speaker));
        }
        pt::game::PcSettingRow controller_speaker_volume = Row(
            kControllerSpeakerVolume, "pc_controller_speaker_volume", {},
            std::clamp(static_cast<int>(std::lround(s.audio.controller_speaker_volume * 20.0f)), 0, 20),
            "pc_note_controller_speaker_volume");
        for (int i = 0; i <= 20; ++i) controller_speaker_volume.values.push_back(std::format("{}%", i * 5));
        controller_speaker_volume.wrap = false;
        sound.rows.push_back(std::move(controller_speaker_volume));
        pt::game::PcSettingRow microphone = Row(kMicrophone, "pc_microphone", {"pc_system_default"}, 0, "pc_note_voice");
        const std::vector<std::string> devices = Microphones();
        for (size_t i = 0; i < devices.size(); ++i) {
            microphone.values.push_back(ShortDeviceName(devices[i]));
            if (devices[i] == s.voice.device) {
                microphone.value = static_cast<int>(i) + 1;
            }
        }
        sound.rows.push_back(std::move(microphone));
        std::string trigger_input = s.voice.key.empty() ? "J" : s.voice.key;
        if(input_.Prompts().device != pt::PromptDevice::Keyboard) {
            trigger_input = (input_.Prompts().device == pt::PromptDevice::PlayStation || input_.Prompts().device == pt::PromptDevice::Steam) ? "L2 + R2" : input_.Prompts().device == pt::PromptDevice::Nintendo ? "ZL + ZR" : "LT + RT";
        }
        std::string trigger_label(pt::game::PcText("pc_microphone_trigger_assign",Language()));
        if(const size_t at=trigger_label.find("{input}"); at!=std::string::npos) trigger_label.replace(at,7,trigger_input);
        auto trigger = Row(kMicrophoneTrigger, trigger_label, OffOn(), s.voice.key.empty() ? 0 : 1, "pc_note_microphone_trigger");
        sound.rows.push_back(std::move(trigger));
        auto microphone_test = Row(kMicrophoneTest, "pc_microphone_test", {"pc_graphics_value"}, 0, "pc_note_microphone_test");
        microphone_test.link = true;
        sound.rows.push_back(std::move(microphone_test));
        if (page_ == kSoundPage) {
            pt::game::PcSettingSection voice{"pc_microphone", 0, {}};
            voice.rows.assign(std::make_move_iterator(sound.rows.begin() + 5), std::make_move_iterator(sound.rows.end()));
            sound.rows.resize(5);
            sound.column = 0;
            return {std::move(sound), std::move(voice)};
        }

        pt::game::PcSettingSection controls{"pc_section_controls", 0, {}};
        if (input_.Prompts().device == pt::PromptDevice::Keyboard) {
            pt::game::PcSettingRow mouse = Row(kMouse, "pc_mouse", {}, Nearest(kMouseSteps, s.input.mouse_sensitivity), "");
            for (const float step : kMouseSteps) {
                mouse.values.push_back(StepText(step));
            }
            mouse.wrap = false;
            controls.rows.push_back(std::move(mouse));
        } else {
            pt::game::PcSettingRow gamepad = Row(kGamepadSensitivity, "pc_gamepad_sensitivity", {},
                                                  Nearest(kGamepadSensitivitySteps, s.input.gamepad_sensitivity),
                                                  "pc_note_gamepad_sensitivity");
            for (const float step : kGamepadSensitivitySteps) {
                gamepad.values.push_back(StepText(step));
            }
            gamepad.wrap = false;
            controls.rows.push_back(std::move(gamepad));
        }
        controls.rows.push_back(Row(kCameraTilt, "pc_camera_tilt", OffOn(), s.camera.roll > 0.5f ? 1 : 0, "pc_note_camera_tilt"));
        controls.rows.push_back(Row(kVibration, "pc_vibration", OffOn(), s.input.rumble ? 1 : 0, ""));
        controls.rows.push_back(Row(kPromptStyle, "pc_prompt_style", {"pc_prompt_auto", "PlayStation", "Xbox", "Nintendo", "Steam"},
                                    std::clamp(s.input.prompt_style, 0, 4), "pc_note_prompt_style"));
        controls.rows.push_back(Row(kRumbleProfile, "pc_rumble_profile", {"pc_rumble_original", "pc_rumble_enhanced"},
                                    std::clamp(s.input.rumble_profile, 0, 1), "pc_note_rumble_profile"));
        const int dead_zone_step = Nearest(kDeadZoneSteps, s.input.gamepad_dead_zone);
        pt::game::PcSettingRow dead_zone = Row(kDeadZone, "pc_dead_zone", {}, dead_zone_step, "pc_note_dead_zone");
        for (const float step : kDeadZoneSteps) {
            dead_zone.values.push_back(std::format("{:.2f}", step));
        }
        dead_zone.wrap = false;
        controls.rows.push_back(std::move(dead_zone));
        if (game_.SurvivalAvailable()) {
            auto crosshair = Row(kCrosshair, "pc_crosshair", {"pc_graphics_value"}, 0, "pc_note_crosshair");
            crosshair.link = true;
            controls.rows.push_back(std::move(crosshair));
        }
        {
            auto keys_row = Row(kKeyBindings, "pc_key_bindings", {"pc_graphics_value"}, 0, "pc_note_key_bindings");
            keys_row.link = true;
            controls.rows.push_back(std::move(keys_row));
        }
        return {std::move(controls)};
    }

    struct KeyBindRowSpec {
        pt::KeyAction action;
        const char* label;
    };
    static constexpr std::array<KeyBindRowSpec, 14> kKeyBindRows = {{
        {pt::KeyAction::WalkForward, "pc_key_walk_forward"},
        {pt::KeyAction::WalkBack, "pc_key_walk_back"},
        {pt::KeyAction::WalkLeft, "pc_key_walk_left"},
        {pt::KeyAction::WalkRight, "pc_key_walk_right"},
        {pt::KeyAction::Zoom, "pc_key_zoom"},
        {pt::KeyAction::Act, "pc_key_act"},
        {pt::KeyAction::SurvivalFire, "pc_key_surv_fire"},
        {pt::KeyAction::SurvivalReload, "pc_key_surv_reload"},
        {pt::KeyAction::SurvivalSprint, "pc_key_surv_sprint"},
        {pt::KeyAction::SurvivalSwap, "pc_key_surv_swap"},
        {pt::KeyAction::SurvivalMelee, "pc_key_surv_melee"},
        {pt::KeyAction::SurvivalFlare, "pc_key_surv_flare"},
        {pt::KeyAction::SurvivalUse, "pc_key_surv_use"},
        {pt::KeyAction::SurvivalInfo, "pc_key_surv_info"},
    }};

    std::vector<pt::game::PcSettingSection> KeyBindingsSections() {
        if (rebind_row_ >= 0 && rebind_row_ < static_cast<int>(kKeyBindRows.size())) {
            uint16_t sc = 0;
            uint8_t ms = 0;
            bool cancelled = false;
            if (input_.PollCapturedKey(sc, ms, cancelled)) {
                if (!cancelled && pt::SetKeyBinding(kKeyBindRows[static_cast<size_t>(rebind_row_)].action, sc, ms)) {
                    app_.settings.keys = pt::ExportKeyBindings();
                    Save();
                }
                rebind_row_ = -1;
            }
        }
        pt::game::PcSettingSection section{"pc_key_bindings", 0, {}};
        for (size_t i = 0; i < kKeyBindRows.size(); ++i) {
            const bool listening = rebind_row_ == static_cast<int>(i);
            std::string val;
            if (listening) {
                val = "pc_key_press_any";
            } else if (const pt::KeyBinding* b = pt::FirstBinding(kKeyBindRows[i].action)) {
                val = pt::KeyBindingName(*b);
            }
            auto row = Row(kKeyBindFirst + static_cast<int>(i), kKeyBindRows[i].label, {val}, 0, "pc_note_key_bind_row");
            row.link = true;
            section.rows.push_back(std::move(row));
        }
        auto reset = Row(kKeyBindReset, "pc_key_reset", {""}, 0, "pc_note_key_reset");
        reset.action = true;
        reset.confirm_note = "pc_dlg_key_reset";
        section.rows.push_back(std::move(reset));
        return {std::move(section)};
    }

    static constexpr std::array<pt::AppSettings::Crosshair, 7> kCrosshairPresets{{
        {0, 0, 7, 3, 3, true, true, true, 8, true, false},
        {5, 0, 4, 4, 0, false, true, true, 9, true, false},
        {1, 1, 6, 2, 0, false, false, true, 9, true, false},
        {2, 3, 6, 3, 3, true, true, true, 9, true, false},
        {4, 6, 11, 3, 4, true, true, true, 8, true, false},
        {0, 0, 4, 2, 2, true, false, true, 6, true, false},
        {6, 0, 7, 3, 3, false, false, false, 8, false, false},
    }};
    static int DetectCrosshairPreset(const pt::AppSettings::Crosshair& c) {
        for (size_t i = 0; i < kCrosshairPresets.size(); ++i) {
            if (c == kCrosshairPresets[i]) return static_cast<int>(i);
        }
        return static_cast<int>(kCrosshairPresets.size());
    }

    std::vector<pt::game::PcSettingSection> CrosshairSections() {
        const pt::AppSettings::Crosshair& c = app_.settings.crosshair;
        pt::game::PcSettingSection look{"pc_crosshair_look", 0, {}};
        const int preset = DetectCrosshairPreset(c);
        std::vector<std::string> preset_names = {
            "pc_crosshair_preset_default", "pc_crosshair_preset_dot",      "pc_crosshair_preset_classic",
            "pc_crosshair_preset_tactical", "pc_crosshair_preset_circle",  "pc_crosshair_preset_minimal",
            "pc_crosshair_preset_off",
        };
        if (preset >= static_cast<int>(kCrosshairPresets.size())) preset_names.push_back("pc_crosshair_preset_custom");
        look.rows.push_back(Row(kCrossPreset, "pc_crosshair_preset", std::move(preset_names), preset, "pc_note_crosshair_preset"));
        look.rows.push_back(Row(kCrossShape, "pc_crosshair_shape",
                                {"pc_crosshair_ticks", "pc_crosshair_cross", "pc_crosshair_tee", "pc_crosshair_circle", "pc_crosshair_circle_dot",
                                 "pc_crosshair_dot", "pc_crosshair_none"},
                                std::clamp(c.shape, 0, 6), "pc_note_crosshair_shape"));
        look.rows.push_back(Row(kCrossColor, "pc_crosshair_color",
                                {"pc_color_white", "pc_color_green", "pc_color_yellow", "pc_color_cyan", "pc_color_magenta", "pc_color_red", "pc_color_orange"},
                                std::clamp(c.color, 0, 6), "pc_note_crosshair_color"));
        auto numbers = [](int from, int to, auto text) {
            std::vector<std::string> values;
            for (int i = from; i <= to; ++i) values.push_back(text(i));
            return values;
        };
        auto length = Row(kCrossLength, "pc_crosshair_length", numbers(1, 20, [](int i) { return std::to_string(i); }), std::clamp(c.length, 1, 20) - 1,
                          "pc_note_crosshair_length");
        length.wrap = false;
        look.rows.push_back(std::move(length));
        auto thickness = Row(kCrossThickness, "pc_crosshair_thickness", numbers(1, 10, [](int i) { return std::format("{:.1f}", 0.5f * static_cast<float>(i)); }),
                             std::clamp(c.thickness, 1, 10) - 1, "pc_note_crosshair_thickness");
        thickness.wrap = false;
        look.rows.push_back(std::move(thickness));
        auto gap = Row(kCrossGap, "pc_crosshair_gap", numbers(0, 20, [](int i) { return std::to_string(i); }), std::clamp(c.gap, 0, 20), "pc_note_crosshair_gap");
        gap.wrap = false;
        gap.enabled = c.shape != 1 && c.shape != 5 && c.shape != 6;
        look.rows.push_back(std::move(gap));
        auto opacity = Row(kCrossOpacity, "pc_crosshair_opacity", numbers(1, 10, [](int i) { return std::format("{}%", i * 10); }),
                           std::clamp(c.opacity, 1, 10) - 1, "pc_note_crosshair_opacity");
        opacity.wrap = false;
        look.rows.push_back(std::move(opacity));
        pt::game::PcSettingSection behaviour{"pc_crosshair_behaviour", 0, {}};
        behaviour.rows.push_back(Row(kCrossDynamic, "pc_crosshair_dynamic", OffOn(), c.dynamic ? 1 : 0, "pc_note_crosshair_dynamic"));
        behaviour.rows.push_back(Row(kCrossDot, "pc_crosshair_dot_row", OffOn(), c.dot ? 1 : 0, "pc_note_crosshair_dot"));
        behaviour.rows.push_back(Row(kCrossOutline, "pc_crosshair_outline", OffOn(), c.outline ? 1 : 0, "pc_note_crosshair_outline"));
        behaviour.rows.push_back(Row(kCrossHitMarker, "pc_crosshair_hit_marker", OffOn(), c.hit_marker ? 1 : 0, "pc_note_crosshair_hit_marker"));
        behaviour.rows.push_back(Row(kCrossSights, "pc_crosshair_sights", OffOn(), c.behind_sights ? 1 : 0, "pc_note_crosshair_sights"));
        auto reset = Row(kCrossReset, "pc_crosshair_reset", {"pc_crosshair_reset_value"}, 0, "pc_note_crosshair_reset");
        reset.action = true;
        reset.confirm_note = "pc_dlg_crosshair_reset";
        reset.confirm_default_yes = true;
        behaviour.rows.push_back(std::move(reset));
        return {std::move(look), std::move(behaviour)};
    }

    void AddProgressAndSupport(std::vector<pt::game::PcSettingSection>& out) {
        pt::game::PcSettingSection progress{"pc_section_progress", 0, {}};
        pt::game::PcSettingRow reset = Row(kReset, "pc_reset", {"pc_reset_value"}, 0, "pc_note_reset");
        reset.action = true;
        reset.confirm_note = "pc_dlg_reset";
        if (!game_.SavesEnabled()) {
            reset.values = {"pc_reset_done"};
            reset.enabled = false;
            reset.note = "pc_note_reset_done";
        }
        progress.rows.push_back(std::move(reset));
        pt::game::PcSettingSection support{"pc_section_support", 0, {}};
        {
            const pt::LogReportState state = pt::GetLogReportState();
            const char* value = state == pt::LogReportState::Sending ? "pc_send_log_sending"
                                : state == pt::LogReportState::Sent  ? "pc_send_log_sent"
                                : state == pt::LogReportState::Failed ? "pc_send_log_failed"
                                                                      : "pc_send_log_value";
            pt::game::PcSettingRow send = Row(kSendLog, "pc_send_log", {value}, 0, "pc_note_send_log");
            send.action = true;
            send.confirm_note = "pc_dlg_send_log";
            send.confirm_default_yes = false;
            send.enabled = state != pt::LogReportState::Sending && state != pt::LogReportState::Sent;
            support.rows.push_back(std::move(send));
        }
        out.push_back(std::move(progress));
        out.push_back(std::move(support));
    }

    std::vector<int> Tabs() const {
        std::vector<int> tabs = {kDisplayPage, kUpscalingPage, kGraphicsPage, kSoundPage, kControlsPage, kAccessibilityPage, kExtrasPage};
        if (app_.mods && !app_.mods->mods.empty()) tabs.push_back(kModsPage);
        return tabs;
    }
    std::optional<pt::game::PcSettingRow> FooterAction() override {
        if (TabCount() == 0) return std::nullopt;
        if (const auto newer = app_.updates.Newer(); newer && InGameUpdateBlocked(*newer)) {
            pt::game::PcSettingRow update = Row(kUpdateNow, "pc_update_now", {newer->version}, 0,
                                                std::format("pc_note_update_readonly{}{}", pt::game::kPcNoteArgument, newer->version));
            update.action = true;
            update.enabled = false;
            return update;
        }
        if (const auto newer = app_.updates.Newer(); newer && InGameUpdate(*newer)) {
            pt::game::PcSettingRow update = Row(kUpdateNow, "pc_update_now", {UpdateVersionLabel(*newer)}, 0,
                                                std::format("pc_note_update_now{}{}", pt::game::kPcNoteArgument, newer->version));
            update.action = true;
            update.confirm_note = "pc_dlg_update";
            update.confirm_default_yes = true;
            update.enabled = !app_.update_requested;
            return update;
        }
        return std::nullopt;
    }
    int TabCount() const override {
        const std::vector<int> tabs = Tabs();
        return std::find(tabs.begin(), tabs.end(), page_) != tabs.end() ? static_cast<int>(tabs.size()) : 0;
    }
    int Tab() const override {
        const std::vector<int> tabs = Tabs();
        return static_cast<int>(std::find(tabs.begin(), tabs.end(), page_) - tabs.begin());
    }
    static const char* TabKey(int page, bool note) {
        switch (page) {
        case kDisplayPage: return note ? "pc_note_display_page" : "pc_section_display";
        case kUpscalingPage: return note ? "pc_note_upscaling_page" : "pc_section_upscaling";
        case kGraphicsPage: return note ? "pc_note_graphics" : "pc_graphics";
        case kSoundPage: return note ? "pc_note_sound_page" : "pc_section_sound";
        case kControlsPage: return note ? "pc_note_controls_page" : "pc_section_controls";
        case kAccessibilityPage: return note ? "pc_note_accessibility" : "pc_accessibility";
        case kExtrasPage: return note ? "pc_note_extras_archive" : "pc_extras";
        case kModsPage: return note ? "pc_note_mods" : "pc_mods";
        default: return "";
        }
    }
    std::string_view TabLabel(int i) const override {
        const std::vector<int> tabs = Tabs();
        return i >= 0 && i < static_cast<int>(tabs.size()) ? TabKey(tabs[i], false) : "";
    }
    std::string_view TabNote(int i) const override {
        const std::vector<int> tabs = Tabs();
        return i >= 0 && i < static_cast<int>(tabs.size()) ? TabKey(tabs[i], true) : "";
    }
    void SelectTab(int i) override {
        const std::vector<int> tabs = Tabs();
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        page_ = last_tab_ = tabs[i];
        app_.microphone_test = app_.microphone_monitor = false;
    }
    void MenuShown() override {
        last_tab_ = kDisplayPage;
        hdr_screen_ = HdrScreen::None;
    }
    void Donate() override { Activate(kPatreon); }
    bool SupportPrompts() const override { return app_.settings.ui.support_prompts; }

    bool FindSettingRow(std::string_view label, pt::game::PcSettingRow& found) override {
        const int previous = page_;
        bool hit = false;
        for (const int page : kSettingPages) {
            page_ = page;
            for (auto& section : Sections()) {
                for (auto& row : section.rows) {
                    if (!hit && row.label == label) {
                        found = std::move(row);
                        hit = true;
                    }
                }
            }
            if (hit) break;
        }
        page_ = previous;
        return hit;
    }

    bool ShowSettingPage(std::string_view label) override {
        const int previous = page_;
        for (const int page : kSettingPages) {
            page_ = page;
            for (const auto& section : Sections()) {
                for (const auto& row : section.rows) {
                    if (row.label == label) {
                        pt::LogInfo("input script: PC settings page {} opened for row {}", page, label);
                        if (TabCount() > 0) last_tab_ = page_;
                        return true;
                    }
                }
            }
        }
        page_ = previous;
        return false;
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
        for (const int page : {kVrPage, kSoundPage, kAccessibilityPage, kDisplayPage, kHdrPage, kUpscalingPage, kControlsPage}) {
            page_ = page;
            for (const auto& section : Sections()) for (const auto& row : section.rows) {
                if (!row.note.empty()) notes.push_back(row.note);
                for (const auto& note : row.value_notes) if (!note.empty()) notes.push_back(note);
            }
        }
        for (const char* note : {"pc_hdr_calibrate_peak", "pc_hdr_calibrate_paper", "pc_hdr_calibrate_shadows", "pc_hdr_calibrate_menus"}) {
            notes.push_back(note);
        }
        if (const auto footer = FooterAction(); footer && !footer->note.empty()) notes.push_back(footer->note);
        page_ = previous;
        archive_section_ = previous_section;
        for (const char* note : {"pc_note_archive_playing", "pc_note_archive_locked", "pc_note_archive_locked_chance", "pc_note_archive_locked_radio",
                                 "pc_note_archive_gate", "pc_note_archive_lock"}) {
            notes.push_back(note);
        }
        for (const char* note : {"pc_note_fsr_rr", "pc_note_fsr4_vulkan", "pc_note_fsr4_amd", "pc_note_fsr4_driver", "pc_note_dlss_model", "pc_note_ray_reconstruction", "pc_note_rr_unavailable", "pc_note_in_path_tracing",
                                 "pc_note_frame_generation_dlss", "pc_note_dlssg_gpu",
                                 "pc_note_dlssg_multiplier", "pc_note_dlssg_multiplier_limit",
                                 "pc_note_dlssg_driver", "pc_note_dlssg_hags", "pc_note_dlssg_missing", "pc_note_dlssg_failed",
                                 "pc_note_dlssg_crashed_retry", "pc_note_d3d12_crashed_retry",
                                 "pc_note_dlssg_restart", "pc_note_vsync_dlssg", "pc_note_fsr_fg_restart", "pc_note_vsync_off",
                                 "pc_note_frame_generation_xess", "pc_note_xefg_gpu", "pc_note_xefg_windows", "pc_note_xefg_interop",
                                 "pc_note_xefg_failed", "pc_note_xefg_restart", "pc_note_nr_gpu", "pc_note_nr_sdk",
                                 "pc_note_nr_runtime", "pc_note_nr_create", "pc_note_nr_driver", "pc_note_nr_d3d12",
                                 "pc_note_nr_build", "pc_note_nr_needs_dlss", "pc_note_neural_style", "pc_note_neural_model", "pc_note_neural_quality", "pc_note_neural_intensity", "pc_note_nr_route_d3d12",
                                 "pc_note_nr_unlock", "pc_note_nr_unlock_working", "pc_note_nr_unlock_restart",
                                 "pc_note_nr_unlock_failed", "pc_note_nr_unlock_hash", "pc_note_nr_unlock_download",
                                 "pc_note_nr_uninstall", "pc_notice_nr_unlock_ready",
                                 "pc_note_nr_crash", "pc_note_nr_addon", "pc_nr_reinstall", "pc_nr_reinstall_action", "pc_note_nr_reinstall",
                                 "pc_notice_nr_failed",
                                 "pc_nr_download_action", "pc_note_nr_download", "pc_note_nr_unlock_offline",
                                 "pc_fg_unlock", "pc_fg_unlock_action", "pc_fg_retry_action", "pc_fg_failed_value", "pc_note_fg_unlock", "pc_note_fg_unlock_d3d12",
                                 "pc_note_fg_unlock_working", "pc_note_fg_unlock_failed", "pc_note_fg_unlock_hash",
                                 "pc_note_fg_unlock_download", "pc_fg_reinstall", "pc_fg_reinstall_action", "pc_note_fg_reinstall",
                                 "pc_fg_uninstall", "pc_fg_uninstall_action", "pc_note_fg_uninstall",
                                 "pc_note_fg_crash", "pc_note_fg_proxy", "pc_note_fg_fallback", "pc_notice_fg_failed",
                                 "pc_nr_unlock", "pc_nr_retry_action", "pc_nr_unlock_verifying", "pc_note_nr_unlock_verifying",
                                 "pc_note_fg_unlock_first", "pc_note_nr_unlock_first", "pc_note_nr_download_first",
                                 "pc_note_nr_reinstall_restart", "pc_note_nr_remove_restart", "pc_notice_nr_remove_ready", "pc_notice_nr_installed",
                                 "pc_notice_nr_removed", "pc_notice_nr_route_failed", "pc_note_nr_route_failed", "pc_note_nr_next_start", "pc_fg_unlock_working",
                                 "pc_fg_unlock_verifying", "pc_note_fg_unlock_verifying", "pc_fg_unlock_restart", "pc_note_fg_unlock_restart",
                                 "pc_note_fg_reinstall_restart", "pc_note_fg_remove_restart", "pc_notice_fg_unlock_ready", "pc_notice_fg_remove_ready",
                                 "pc_notice_fg_installed", "pc_notice_fg_removed", "pc_notice_fg_route_failed", "pc_note_fg_route_failed"}) {
            notes.push_back(note);
        }
        return notes;
    }

    void TestSetFrameGeneration(int kind) {
        const std::vector<pt::FrameGenKind> generators = FrameGenerators();
        const auto at = std::find(generators.begin(), generators.end(), static_cast<pt::FrameGenKind>(kind));
        if (at != generators.end()) Set(kFrameGeneration, static_cast<int>(at - generators.begin()));
    }
    void Set(int id, int value) override {
        pt::AppSettings& s = app_.settings;
        pt::UpscaleSettings& u = app_.scene.upscale;
        if (id == kCrossPreset) {
            if (value >= 0 && value < static_cast<int>(kCrosshairPresets.size())) {
                s.crosshair = kCrosshairPresets[static_cast<size_t>(value)];
                ApplyCrosshair(game_, s);
                Save();
            }
            return;
        }
        if (id >= kCrossShape && id <= kCrossSights) {
            pt::AppSettings::Crosshair& c = s.crosshair;
            switch (id) {
            case kCrossShape: c.shape = std::clamp(value, 0, 6); break;
            case kCrossColor: c.color = std::clamp(value, 0, 6); break;
            case kCrossLength: c.length = std::clamp(value + 1, 1, 20); break;
            case kCrossThickness: c.thickness = std::clamp(value + 1, 1, 10); break;
            case kCrossGap: c.gap = std::clamp(value, 0, 20); break;
            case kCrossOpacity: c.opacity = std::clamp(value + 1, 1, 10); break;
            case kCrossDynamic: c.dynamic = value == 1; break;
            case kCrossDot: c.dot = value == 1; break;
            case kCrossOutline: c.outline = value == 1; break;
            case kCrossHitMarker: c.hit_marker = value == 1; break;
            case kCrossSights: c.behind_sights = value == 1; break;
            default: break;
            }
            ApplyCrosshair(game_, s);
            Save();
            return;
        }
        if (id >= kModFirst) {
            if (app_.mods && id - kModFirst < static_cast<int>(app_.mods->mods.size())) {
                s.mods[app_.mods->mods[id - kModFirst].folder] = value == 1;
                Save();
            }
            return;
        }
        if (id == kArmoryWeapon) {
            armory_weapon_ = std::clamp(value + 1, 1, 5);
            if (armory_filter_ == 0) {
                armory_skin_ = game_.SurvivalProfileData().EquippedSkin(armory_weapon_);
            } else {
                const std::vector<int> list = ArmoryFilteredSkins();
                if (!list.empty() && std::find(list.begin(), list.end(), armory_skin_) == list.end()) {
                    armory_skin_ = list.front();
                }
            }
            armory_reload_timer_ = 0.0f;
            return;
        }
        if (id == kArmoryFilter) {
            armory_filter_ = std::clamp(value, 0, 6);
            const std::vector<int> list = ArmoryFilteredSkins();
            if (!list.empty() && std::find(list.begin(), list.end(), armory_skin_) == list.end()) {
                armory_skin_ = list.front();
            }
            return;
        }
        if (id == kArmorySkin) {
            const std::vector<int> list = ArmoryFilteredSkins();
            if (!list.empty()) {
                armory_skin_ = list[static_cast<size_t>(std::clamp(value, 0, static_cast<int>(list.size()) - 1))];
            }
            return;
        }
        if (id == kArmoryTorch) {
            armory_torch_ = std::clamp(value, 0, pt::game::kSurvivalTorchCount - 1);
            return;
        }
        if (id == kNightFinish || id == kNightTorch) {
            pt::game::SurvivalProfile& profile = game_.SurvivalProfileData();
            const std::span<const uint32_t> needs = id == kNightFinish ? std::span<const uint32_t>(pt::game::kFinishNeeds) : std::span<const uint32_t>(pt::game::kTorchNeeds);
            int seen = 0;
            for (size_t i = 0; i < needs.size(); ++i) {
                if ((profile.unlocks & needs[i]) != needs[i]) continue;
                if (seen++ == value) (id == kNightFinish ? profile.finish : profile.torch) = static_cast<int>(i);
            }
            game_.MarkSurvivalProfile();
            return;
        }
        switch (id) {
        case kMicMonitor:
            app_.microphone_monitor = value == 1;
            return;
        case kSpeedrun:
            s.extras.speedrun = std::clamp(value, 0, 2);
            game_.Speedrun().SetMode(s.extras.speedrun);
            if (game_.Controller().InGame()) game_.SpeedrunStartGame();
            Save();
            return;
        case kLiveSplit:
            s.extras.livesplit = value == 1;
            app_.livesplit.Configure(s.extras.livesplit, s.extras.livesplit_host, s.extras.livesplit_port);
            Save();
            return;
        case kDisplayMode:
            s.display.fullscreen = std::clamp(value, 0, 2);
            app_.display_mode_chosen = true;
            ApplyFullscreen(app_);
            break;
        case kResolution: {
            const std::vector<glm::ivec2> sizes = WindowSizes();
            if (value < 0 || value >= static_cast<int>(sizes.size())) {
                return;
            }
            s.display.width = sizes[value].x;
            s.display.height = sizes[value].y;
            if (app_.window && WindowMode(app_) == pt::display::kWindow) {
                SDL_SetWindowSize(app_.window, s.display.width, s.display.height);
            } else if (WindowMode(app_) == pt::display::kExclusive) {
                ApplyFullscreen(app_);
            }
            break;
        }
        case kRefreshRate: {
            const std::vector<double> rates = RefreshRates();
            if (WindowMode(app_) != pt::display::kExclusive || value < 0 || value > static_cast<int>(rates.size())) return;
            s.display.refresh_rate = value == 0 ? 0.0f : static_cast<float>(rates[value - 1]);
            pt::LogInfo("display: refresh rate {} (PC settings)", value == 0 ? std::string("Auto") : pt::RefreshRateText(rates[value - 1]));
            ApplyFullscreen(app_);
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
        case kFpsLimit: {
            constexpr int limits[] = {0,30,60,90,120,144,165,240,360};
            if(value>=0 && value<9) s.display.fps_limit = limits[value];
            break;
        }
        case kHdr:
            s.display.hdr = value == 1;
            app_.renderer.SetHdr(s.display.hdr);
            break;
        case kHdrPaperWhite: {
            const std::vector<int> steps = HdrPaperWhiteSteps(s.display.hdr_paper_white);
            if (value >= 0 && value < static_cast<int>(steps.size())) s.display.hdr_paper_white = steps[value];
            app_.renderer.SetHdrLevels(s.display.hdr_paper_white, s.display.hdr_peak, s.display.hdr_ui_white);
            break;
        }
        case kHdrPeak: {
            const std::vector<int> steps = HdrPeakSteps(s.display.hdr_peak);
            if (value >= 0 && value < static_cast<int>(steps.size())) s.display.hdr_peak = steps[value];
            app_.renderer.SetHdrLevels(s.display.hdr_paper_white, s.display.hdr_peak, s.display.hdr_ui_white);
            break;
        }
        case kHdrReach:
            s.display.hdr_reach = value == 2 ? 200 : value == 1 ? 150 : 100;
            app_.renderer.SetHdrReach(s.display.hdr_reach);
            break;
        case kHdrGamma:
            s.display.hdr_gamma = std::clamp(value, 0, 2);
            app_.renderer.SetHdrLook(HdrLookOf(s));
            break;
        case kHdrHue:
            s.display.hdr_keep_hue = value == 1;
            app_.renderer.SetHdrLook(HdrLookOf(s));
            break;
        case kHdrColourRange:
            s.display.hdr_colour_range = value == 2 ? 1 : value == 1 ? 2 : 0;
            app_.renderer.SetHdrLook(HdrLookOf(s));
            break;
        case kHdrGrade:
            if (value < 2) {
                SetHdrGrade(s, value == 1 ? kHdrGradeRich : kHdrGradeOriginal);
                app_.renderer.SetHdrLook(HdrLookOf(s));
            }
            break;
        case kHdrGradeReset:
            SetHdrGrade(s, kHdrGradeOriginal);
            app_.renderer.SetHdrLook(HdrLookOf(s));
            break;
        case kHdrExposure: s.display.hdr_exposure = value - 30; app_.renderer.SetHdrLook(HdrLookOf(s)); break;
        case kHdrContrast: s.display.hdr_contrast = value * 5; app_.renderer.SetHdrLook(HdrLookOf(s)); break;
        case kHdrHighlights: s.display.hdr_highlights = value * 5; app_.renderer.SetHdrLook(HdrLookOf(s)); break;
        case kHdrShadows: s.display.hdr_shadows = value * 5; app_.renderer.SetHdrLook(HdrLookOf(s)); break;
        case kHdrSaturation: s.display.hdr_saturation = value * 5; app_.renderer.SetHdrLook(HdrLookOf(s)); break;
        case kHdrHighlightSaturation: s.display.hdr_highlight_saturation = value * 5; app_.renderer.SetHdrLook(HdrLookOf(s)); break;
        case kHdrBlowout: s.display.hdr_blowout = value * 5; app_.renderer.SetHdrLook(HdrLookOf(s)); break;
        case kHdrSeeThrough: s.display.hdr_see_through = value == 1; break;
        case kHdrWindowsSwitch: s.display.hdr_windows_switch = value == 1; break;
        case kPtFloorReflection: s.graphics.pt_floor_reflection = value * 5; break;
        case kPtGlassReflection: s.graphics.pt_glass_reflection = value * 5; break;
        case kPtReflectionSoftness: s.graphics.pt_reflection_softness = value * 5; break;
        case kHdrUiWhite: {
            const std::vector<int> steps = HdrUiWhiteSteps(s.display.hdr_ui_white);
            if (value >= 0 && value < static_cast<int>(steps.size())) s.display.hdr_ui_white = steps[value];
            app_.renderer.SetHdrLevels(s.display.hdr_paper_white, s.display.hdr_peak, s.display.hdr_ui_white);
            break;
        }
        case kSurround:
            if ((s.audio.spatial ? 2 : s.audio.surround ? 1 : 0) != value) game_.ShowNoticeDialog("pc_restart_needed");
            s.audio.surround = value >= 1;
            s.audio.spatial = value == 2;
            break;
        case kControllerSpeaker:
            s.audio.controller_speaker = value == 1;
            break;
        case kOutputDevice: {
            const std::vector<std::string> outputs = OutputDevices();
            if (value == 0) {
                s.audio.output_device.clear();
            } else if (value <= static_cast<int>(outputs.size())) {
                s.audio.output_device = outputs[value - 1];
            }
            break;
        }
        case kControllerSpeakerVolume:
            s.audio.controller_speaker_volume = static_cast<float>(std::clamp(value, 0, 20)) * 0.05f;
            break;
        case kPromptStyle:
            s.input.prompt_style = std::clamp(value, 0, 4);
            input_.settings.prompt_style = s.input.prompt_style;
            break;
        case kRumbleProfile:
            s.input.rumble_profile = std::clamp(value, 0, 1);
            input_.settings.trigger_rumble = pt::FeaturesForRumbleProfile(s.input.rumble_profile, s.input.rumble).trigger_rumble;
            if (!input_.settings.trigger_rumble) input_.SetTriggerRumble(0, 0);
            break;
        case kVrHeight:
            s.vr.height_offset = static_cast<float>(std::clamp(value,0,20)-10)*0.05f;
            break;
        case kVrWorldScale:
            s.vr.world_size = 0.5f + static_cast<float>(std::clamp(value,0,30))*0.05f;
            break;
        case kVsync:
            s.display.vsync = value == 1;
            app_.renderer.SetVsync(s.display.vsync);
            break;
        case kRenderingApi: {
            const pt::RenderingApi api = value == 0 ? pt::RenderingApi::D3D12 : pt::RenderingApi::Vulkan;
            const std::string key(pt::RenderingApiKey(api));
            s.display.rendering_api_asked = true;
            const bool after_crash = api == pt::RenderingApi::D3D12 && (s.display.d3d12_crashed || s.display.d3d12_interrupted);
            if (key != s.display.rendering_api || after_crash) {
                s.display.rendering_api = key;
                s.display.d3d12_auto_fallback = false;
                if (api == pt::RenderingApi::D3D12) {
                    if (after_crash)
                        pt::LogInfo("rendering api: the player chose DirectX 12 after it stopped an earlier start (d3d12_crashed = {}, "
                                    "d3d12_interrupted = {}); both cleared, it is tried again at the next start",
                                    s.display.d3d12_crashed, s.display.d3d12_interrupted);
                    s.display.d3d12_crashed = 0;
                    s.display.d3d12_interrupted = 0;
                }
                pt::LogInfo("rendering api: {} at the next start (PC settings)", pt::RenderingApiName(api));
                game_.ShowNoticeDialog("pc_restart_needed");
            }
            break;
        }
        case kUpscaler: {
            const std::vector<pt::UpscalerKind> kinds = Upscalers(nullptr);
            if (value < 0 || value >= static_cast<int>(kinds.size())) {
                return;
            }
            u.kind = kinds[value];
            UpscaleToApp(u, s);
            if (u.kind != pt::UpscalerKind::Dlss && s.upscaling.neural_rendering != "off") {
                s.upscaling.neural_rendering = "off";
                pt::LogInfo("neural rendering: off, the upscaler is no longer DLSS");
                ApplyNeuralToScene(app_);
            }
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
        case kRayReconstruction:
            u.ray_reconstruction = value == 1;
            UpscaleToApp(u, s);
            break;
        case kDlssgMultiplier:
            u.dlssg_multiplier = static_cast<int>(pt::streamline::ClampFrameGenMultiplier(
                static_cast<unsigned>(std::clamp(value, 0, 2) + 2), pt::streamline::FrameGenMaxMultiplier() - 1));
            UpscaleToApp(u, s);
            pt::LogInfo("frame generation: DLSS multiplier selected {}x", u.dlssg_multiplier);
            break;
        case kFrameGeneration: {
            const std::vector<pt::FrameGenKind> generators = FrameGenerators();
            if (value < 0 || value >= static_cast<int>(generators.size())) return;
            u.frame_generation = generators[value];
            UpscaleToApp(u, s);
            if (app_.dlssg_crash_hold && u.frame_generation == pt::FrameGenKind::Dlss) {
                s.upscaling.dlssg_interrupted = 0;
                pt::LogInfo("streamline: the player chose DLSS Frame Generation after it stopped an earlier start; it is tried again "
                            "at the next start");
                game_.ShowNoticeDialog("pc_restart_needed");
            }
            break;
        }
        case kVolume:
            s.audio.volume = static_cast<float>(std::clamp(value, 0, 20)) * 0.1f;
            break;
        case kMicrophoneTrigger:
            if(value==0) s.voice.key.clear();
            else if(s.voice.key.empty()) s.voice.key="J";
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
        case kFpsCounter:
            s.accessibility.fps_counter = value == 1;
            break;
        case kSupportPrompts:
            s.ui.support_prompts = value == 1;
            pt::LogInfo("support: prompts {}", s.ui.support_prompts ? "shown" : "hidden");
            break;
        case kSoundCaptions:
            s.accessibility.sound_captions = value == 1;
            game_.SetSoundCaptions(s.accessibility.sound_captions);
            break;
        case kSubtitleSize:
            s.accessibility.subtitle_size = std::clamp(value, 0, 2);
            break;
        case kSubtitleBg:
            s.accessibility.subtitle_bg = std::clamp(value, 0, 2);
            break;
        case kReducedFlashing:
            s.accessibility.reduced_flashing = value == 1;
            break;
        case kHouseSprint:
            s.accessibility.house_sprint = value == 1;
            game_.SetHouseSprint(s.accessibility.house_sprint);
            break;
        case kSprintMode:
            s.accessibility.sprint_toggle = value == 1;
            input_.SetToggleModes(s.accessibility.sprint_toggle, s.accessibility.zoom_toggle);
            break;
        case kZoomMode:
            s.accessibility.zoom_toggle = value == 1;
            input_.SetToggleModes(s.accessibility.sprint_toggle, s.accessibility.zoom_toggle);
            break;
        case kGyro:
            s.accessibility.gyro = std::clamp(value, 0, 2);
            input_.settings.gyro = s.accessibility.gyro;
            break;
        case kGyroSensitivity:
            s.accessibility.gyro_sensitivity = kGyroSensitivitySteps[std::clamp(value, 0, static_cast<int>(std::size(kGyroSensitivitySteps)) - 1)];
            input_.settings.gyro_sensitivity = s.accessibility.gyro_sensitivity;
            break;
        case kFieldOfView:
            s.accessibility.fov_offset = kFovSteps[std::clamp(value, 0, static_cast<int>(std::size(kFovSteps)) - 1)];
            game_.SetFovOffset(s.accessibility.fov_offset);
            break;
        case kGamepadSensitivity:
            s.input.gamepad_sensitivity = kGamepadSensitivitySteps[std::clamp(value, 0, static_cast<int>(std::size(kGamepadSensitivitySteps)) - 1)];
            input_.settings.gamepad_sensitivity = s.input.gamepad_sensitivity;
            break;
        case kCameraTilt:
            s.camera.roll = value == 1 ? 1.0f : 0.0f;
            game_.GetPlayer().camera_roll = s.camera.roll;
            break;
        case kThirdPerson:
            s.camera.third_person = value == 1;
            game_.SetThirdPerson(s.camera.third_person);
            break;
        case kGamePlus:
            s.game.game_plus = value == 1;
            game_.SetGamePlusSetting(s.game.game_plus, false);
            break;
        case kVibration:
            s.input.rumble = value == 1;
            input_.settings.rumble = s.input.rumble;
            input_.settings.trigger_rumble = pt::FeaturesForRumbleProfile(s.input.rumble_profile, s.input.rumble).trigger_rumble;
            if (!s.input.rumble) {
                input_.SetRumble(0, 0);
                input_.SetTriggerRumble(0, 0);
            }
            break;
        case kDeadZone:
            s.input.gamepad_dead_zone = kDeadZoneSteps[std::clamp(value, 0, static_cast<int>(std::size(kDeadZoneSteps)) - 1)];
            input_.settings.stick_dead_zone = s.input.gamepad_dead_zone;
            break;
        case kGraphicsPreset: {
            std::string reason;
            pt::ApplyGraphicsPreset(s,static_cast<pt::GraphicsPreset>(std::clamp(value,0,4)),app_.scene.RayTracingSupported(reason));
            app_.textures.SetAnisotropy(s.graphics.anisotropy);
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
        case kRayQuality: {constexpr int kQualities[]={0,1,3,4};s.graphics.ray_quality=kQualities[std::clamp(value,0,3)];break;}
        case kTextureDetail: s.graphics.texture_detail=std::clamp(value,0,2);break;
        case kBloom: s.graphics.bloom_strength=std::clamp(value,0,40)*5;s.graphics.bloom=value>0;break;
        case kFlareStrength: s.graphics.flare_strength=std::clamp(value,0,40)*5;break;
        case kLensGhosts: s.graphics.ghost_strength=std::clamp(value,0,40)*5;s.graphics.lens_ghosts=value>0;break;
        case kDepthOfField: s.graphics.depth_of_field=value==1;break;
        case kMotionBlur: s.graphics.motion_blur=value<2;s.graphics.camera_motion_blur=value==0;break;
        case kMotionBlurStrength: s.graphics.motion_blur_strength=(std::clamp(value,0,9)+1)*10;break;
        case kFilmGrain: s.graphics.film_grain=std::clamp(value,0,4)*.25f;break;
        case kFilmGrainType: s.graphics.film_grain_type=std::clamp(value,0,1);break;
        case kLensDistortion: s.graphics.lens_distortion=value==1;break;
        case kClarity: s.graphics.clarity=std::clamp(value,0,10)*.1f;break;
        case kLetterbox: s.display.letterbox=std::clamp(value,0,2);break;
        case kNeuralRendering: {
            const bool available=NeuralSupport(app_).available&&!app_.nr_route_failed;
            const bool on=value>=1&&available&&NrUpscalerReady(app_);
            if(app_.nr_crash_hold){app_.nr_crash_hold=false;pt::LogInfo("neural rendering: the player chose DLSS 5 {} after the hold",on?"on":"off");}
            if(on&&app_.nr_row_note=="pc_note_nr_crash") app_.nr_row_note.clear();
            if(on&&s.upscaling.neural_rendering!=pt::NeuralRenderingKey(pt::NeuralRenderingKind::Dlss5)) s.upscaling.neural_model="small";
            s.upscaling.neural_rendering=pt::NeuralRenderingKey(on?pt::NeuralRenderingKind::Dlss5:pt::NeuralRenderingKind::Off);
            ApplyNeuralToScene(app_);
            break;
        }
        case kFgUnlock: app_.fg_unlock_choice=std::clamp(value,0,1);break;
        case kNeuralUnlock: app_.nr_unlock_choice=std::clamp(value,0,1);break;
        case kNeuralStyle: s.upscaling.neural_style=pt::NeuralStyleKey(static_cast<pt::NeuralStyle>(std::clamp(value,0,2)));ApplyNeuralToScene(app_);break;
        case kNeuralModel: s.upscaling.neural_model=value == 0 ? "small" : "large";ApplyNeuralToScene(app_);break;
        case kNeuralQuality: {
            static constexpr const char* kQualities[] = {"performance", "quality", "ultra"};
            s.upscaling.neural_quality = kQualities[std::clamp(value, 0, 2)];
            ApplyNeuralToScene(app_);
            break;
        }
        case kNeuralIntensity: s.upscaling.neural_intensity=static_cast<float>(std::clamp(value,0,11)+1)*.25f;ApplyNeuralToScene(app_);break;
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
        default:
            return;
        }
        ApplyGraphicsSettings(app_);
        Save();
    }

    std::string_view Title() const override {
        switch(page_) {
        case kLoopPage:return "pc_loop_browser";
        case kNightPage:return "pc_survival";
        case kSurvivalPromoPage:return "pc_survival";
        case kArmoryPage:return "pc_armory_title";
        case kCrosshairPage:return "pc_crosshair";
        case kKeyBindingsPage:return "pc_key_bindings";
        case kStreetPage:return game_.SpeedrunResultsPending() ? "pc_speedrun_results" : "pc_street_title";
        case kUpdatePage:return "pc_update_title";
        case kGraphicsPage:return "pc_graphics";
        case kLightingPage:return "pc_graphics_lighting";
        case kTexturesPage:return "pc_section_textures";
        case kEffectsPage:return "pc_graphics_effects";
        case kMicrophonePage:return "pc_microphone_test";
        case kSoundPage:return "pc_section_sound";
        case kAccessibilityPage:return "pc_accessibility";
        case kModsPage:return "pc_mods";
        case kExtrasPage:return "pc_extras";
        case kArchivePage:return "pc_archive";
        case kArchiveListPage:return "pc_archive";
        case kVrPage:return "pc_vr";
        case kHdrPage:return "pc_hdr_title";
        case kDisplayPage:return "pc_section_display";
        case kUpscalingPage:return "pc_section_upscaling";
        case kControlsPage:return "pc_section_controls";
        default:return "pc_title";
        }
    }

    bool IsLoopBrowser() const override { return page_ == kLoopPage; }
    bool IsBrowser() const override { return page_ == kLoopPage || page_ == kCrosshairPage; }
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
        case M::Screen:
            panel.texture = std::string(entry.asset);
            panel.aspect = 16.0f / 9.0f;
            break;
        case M::Photo:
            panel.photo = entry.asset.empty() ? std::string("complete") : std::string(entry.asset);
            break;
        case M::Demo:
        case M::Model:
            panel.thumbnail = pt::os::PathToUtf8((MuseumPreviewDirectory() / std::format("{}.png", entry.id)));
            panel.lift = 1.0f;
            break;
        case M::Sound:
        case M::Dialogue:
            if (const pt::game::ArchiveEntry* picture = pt::game::FindArchiveEntry(pt::game::ArchivePictureOf(entry))) return EntryPicture(*picture);
            break;
        case M::Scene:
            panel.thumbnail = pt::game::LisaDatePicturePath("museum.png");
            break;
        case M::Music:
            break;
        }
        return panel;
    }

    pt::game::PcPanel Panel(int id) const override {
        pt::game::PcPanel panel;
        if (page_ == kCrosshairPage) {
            panel.crosshair = CrosshairOf(app_.settings.crosshair);
            return panel;
        }
        if (page_ == kLoopPage) {
            panel.file = PreviewFile(id);
            return panel;
        }
        if (page_ == kArchivePage && id >= kArchiveSectionFirst && id < kArchiveSectionFirst + pt::game::kArchiveSectionCount) {
            const auto section = static_cast<pt::game::ArchiveSection>(id - kArchiveSectionFirst);
            if (section == pt::game::ArchiveSection::Radio || section == pt::game::ArchiveSection::Voices || section == pt::game::ArchiveSection::Music) {
                return panel;
            }
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
            panel.title = std::string(pt::game::kArchiveLockedLabel);
            panel.caption.clear();
            panel.locked = true;
            return panel;
        }
        pt::game::PcPanel picture = EntryPicture(*entry);
        panel.texture = std::move(picture.texture);
        panel.string = picture.string;
        panel.aspect = picture.aspect;
        panel.photo = std::move(picture.photo);
        panel.thumbnail = std::move(picture.thumbnail);
        panel.lift = picture.lift;
        using M = pt::game::ArchiveMedia;
        switch (entry->media) {
        case M::Sound:
        case M::Dialogue: {
            panel.audio = true;
            if (!app_.transcripts) break;
            const float now = voice_entry_ == entry ? VoiceSeconds() : -1.0f;
            const std::span<const pt::game::MuseumLine> full = pt::game::MuseumTranscript(entry->id);
            if (!full.empty()) {
                std::string_view loaded_id;
                std::vector<pt::game::SubtitlePlayer::TranscriptLine> loaded;
                for (const pt::game::MuseumLine& line : full) {
                    std::string text;
                    if (!line.subtitle.empty()) {
                        if (line.subtitle != loaded_id) {
                            loaded_id = line.subtitle;
                            loaded = app_.transcripts->Transcript(line.subtitle, language);
                        }
                        if (line.subtitle_line >= 0 && line.subtitle_line < static_cast<int>(loaded.size())) text = loaded[line.subtitle_line].text;
                    } else {
                        text = std::string(line.original);
                        if (!line.key.empty()) {
                            const std::string translated(pt::game::PcText(line.key, language));
                            text = text.empty() ? translated : text + "\n(" + translated + ")";
                        }
                    }
                    if (text.empty()) continue;
                    if (now >= 0.0f && now >= line.start) panel.current_line = static_cast<int>(panel.lines.size());
                    panel.lines.push_back(std::move(text));
                }
            } else {
                for (const auto& line : app_.transcripts->Transcript(SubtitleOf(*entry), language)) {
                    if (now >= 0.0f && now >= line.start) panel.current_line = static_cast<int>(panel.lines.size());
                    panel.lines.push_back(line.text);
                }
            }
            if (now >= 0.0f && panel.current_line < 0 && !panel.lines.empty()) panel.current_line = 0;
            break;
        }
        case M::Music:
            panel.music = true;
            panel.music_loop = music_.Loop();
            if (music_.Entry() == entry) {
                panel.music_position = static_cast<float>(music_.Position());
                panel.music_length = static_cast<float>(music_.Length());
                panel.music_playing = music_.Playing();
            } else {
                panel.music_length = static_cast<float>(MusicLength(*entry));
            }
            break;
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
    std::string_view Question() const override {
        if (page_ == kUpdatePage) {
            const auto newer = app_.updates.Newer();
            update_question_text_ = newer ? std::format("pc_update_question{}{}{}{}", pt::game::kPcNoteArgument, UpdateVersionLabel(*newer),
                                                        pt::game::kPcNoteArgument, pt::update::CurrentVersion())
                                          : std::string("pc_update_title");
            return update_question_text_;
        }
        if (page_ == kSurvivalPromoPage) return "pc_survival_promo_question";
        return page_ == kStreetPage && !game_.SpeedrunResultsPending() ? "pc_street_question" : std::string_view();
    }
    mutable std::string update_question_text_;

    bool CompactRows() const override { return page_ == kLoopPage || (page_ == kStreetPage && game_.SpeedrunResultsPending()); }
    std::string_view Credit() const override {
        if (page_ == kExtrasPage) return "Hallway Engine by LoreanXavier";
        if (TabCount() > 0) {
            if (const auto newer = app_.updates.Newer()) {
                if (update_notice_.empty()) update_notice_ = UpdateNoticeKey(*newer);
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
        const int index = id == kLoopNight ? pt::game::kBrowseStreet : id - kLoopFirst;
        return IsLoopBrowser() && index >= 0 && index < kLoopCount && LoopUnlocked(index) ?
            pt::os::PathToUtf8((PreviewDirectory() / std::format("loop-{}.png", index))) : std::string();
    }

    void Activate(int id) override {
        if (id == kFgUnlock && app_.fg_install_usable) id = app_.fg_unlock_choice == 1 ? kFgUninstall : kFgReinstall;
        if (id == kNeuralUnlock && app_.nr_install_usable && UsesNrRoute(app_.nr_gpu)) id = app_.nr_unlock_choice == 1 ? kNeuralUninstall : kNeuralReinstall;
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
        if (id == kLoopBrowser) {
            page_ = kLoopPage;
            GeneratePreviews();
            if (game_.SurvivalAvailable() && !app_.options.vr) SurvivalVisions().Update(app_.options);
        }
        if (id == kExtras) page_ = kExtrasPage;
        if (id == kVr) page_ = kVrPage;
        if (id == kFreecam) app_.extras_request = 1;
        if (id == kPhotoMode) app_.extras_request = 2;
        if (id >= kLoopFirst && id < kLoopFirst + kLoopCount && LoopUnlocked(id - kLoopFirst)) game_.BrowseLoop(id - kLoopFirst);
        if (id == kLoopNight && game_.SurvivalAvailable()) {
            page_ = kNightPage;
            if (!app_.options.vr) SurvivalVisions().Update(app_.options);
        }
        if (id == kPromoPlay && game_.SurvivalAvailable()) {
            DismissSurvivalPromo();
            night_from_promo_ = true;
            page_ = kNightPage;
            if (!app_.options.vr) SurvivalVisions().Update(app_.options);
        }
        if (id == kPromoLater) {
            DismissSurvivalPromo();
            page_ = last_tab_;
            return_to_options_ = true;
        }
        if (id == kNightArmory && game_.SurvivalAvailable()) {
            if (const char* env_cr = std::getenv("PT_SURVIVAL_CREDITS")) {
                game_.SurvivalProfileData().credits = std::max(game_.SurvivalProfileData().credits, std::atoi(env_cr));
            }
            armory_weapon_ = std::clamp(armory_weapon_, 1, 5);
            armory_mode_ = 0;
            armory_skin_ = game_.SurvivalProfileData().EquippedSkin(armory_weapon_);
            armory_anim_ = game_.SurvivalProfileData().EquippedReloadChoice(armory_weapon_);
            armory_torch_ = game_.SurvivalProfileData().EquippedTorch();
            armory_pitch_ = 0.0f;
            armory_drag_cooldown_ = 0.0f;
            armory_reload_timer_ = 0.0f;
            page_ = kArmoryPage;
        }
        if (id == kArmoryAction) {
            pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
            const pt::game::SurvivalSkinDef& sk = pt::game::SurvivalSkin(armory_skin_);
            if (!prof.OwnsSkin(sk.id)) {
                if (prof.credits >= sk.price) {
                    prof.credits -= sk.price;
                    prof.GrantSkin(sk.id);
                    prof.weapon_skins[static_cast<size_t>(armory_weapon_)] = sk.id;
                    game_.MarkSurvivalProfile();
                    app_.settings.game.survival_profile = prof.Save();
                    Save();
                    if (pt::game::SurvivalNight* s = game_.EnsureSurvival()) s->RefreshLooks();
                    pt::LogInfo("armory: bought & equipped '{}' (${}, remaining ${}) on weapon {}", sk.name, sk.price, prof.credits, armory_weapon_);
                }
            } else {
                prof.weapon_skins[static_cast<size_t>(armory_weapon_)] = sk.id;
                game_.MarkSurvivalProfile();
                app_.settings.game.survival_profile = prof.Save();
                Save();
                if (pt::game::SurvivalNight* s = game_.EnsureSurvival()) s->RefreshLooks();
                pt::LogInfo("armory: equipped '{}' on weapon {}", sk.name, armory_weapon_);
            }
            keep_cursor_ = true;
        }
        if (id == kArmoryApplyAll) {
            pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
            if (prof.OwnsSkin(armory_skin_)) {
                for (size_t w = 1; w <= 5; ++w) prof.weapon_skins[w] = armory_skin_;
                game_.MarkSurvivalProfile();
                app_.settings.game.survival_profile = prof.Save();
                Save();
                if (pt::game::SurvivalNight* s = game_.EnsureSurvival()) s->RefreshLooks();
                pt::LogInfo("armory: applied '{}' to all 5 weapons", pt::game::SurvivalSkin(armory_skin_).name);
            }
            keep_cursor_ = true;
        }
        if (id == kArmoryReload) {
            const pt::game::ReloadStyle rs = pt::game::SurvivalSkin(armory_skin_).reload;
            armory_reload_length_ = rs == pt::game::ReloadStyle::Telekinetic ? 1.8f : rs == pt::game::ReloadStyle::Gunslinger ? 1.6f : 1.4f;
            armory_reload_timer_ = armory_reload_length_;
            keep_cursor_ = true;
        }
        if (id == kArmoryTorchAction) {
            pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
            const pt::game::SurvivalTorchDef& td = pt::game::SurvivalTorch(armory_torch_);
            if (!prof.OwnsTorch(td.id)) {
                if (prof.credits >= td.price) {
                    prof.credits -= td.price;
                    prof.GrantTorch(td.id);
                    prof.torch = td.id;
                    game_.MarkSurvivalProfile();
                    app_.settings.game.survival_profile = prof.Save();
                    Save();
                    if (pt::game::SurvivalNight* s = game_.EnsureSurvival()) s->RefreshLooks();
                    pt::LogInfo("armory: bought & equipped torch {} (${}, remaining ${})", td.id, td.price, prof.credits);
                }
            } else {
                prof.torch = td.id;
                game_.MarkSurvivalProfile();
                app_.settings.game.survival_profile = prof.Save();
                Save();
                if (pt::game::SurvivalNight* s = game_.EnsureSurvival()) s->RefreshLooks();
                pt::LogInfo("armory: equipped torch {}", td.id);
            }
            keep_cursor_ = true;
        }
        if (id == kCrosshair || id == kNightCrosshair) {
            crosshair_from_ = page_;
            page_ = kCrosshairPage;
        }
        if (id == kCrossReset) {
            app_.settings.crosshair = {};
            ApplyCrosshair(game_, app_.settings);
            Save();
            pt::LogInfo("crosshair: reset to the default");
        }
        if (id == kKeyBindings) {
            rebind_row_ = -1;
            input_.CancelKeyCapture();
            page_ = kKeyBindingsPage;
        }
        if (id >= kKeyBindFirst && id < kKeyBindFirst + static_cast<int>(kKeyBindRows.size())) {
            rebind_row_ = id - kKeyBindFirst;
            input_.BeginKeyCapture();
            keep_cursor_ = true;
        }
        if (id == kKeyBindReset) {
            rebind_row_ = -1;
            input_.CancelKeyCapture();
            pt::ResetKeyBindings();
            app_.settings.keys.clear();
            Save();
            keep_cursor_ = true;
            pt::LogInfo("input: key bindings reset to defaults");
        }
        auto launch_night = [&]() {
            night_from_promo_ = false;
            night_from_death_ = false;
            if (game_.StreetWalkActive()) {
                game_.StartSurvival();
                game_.RequestMenuClose();
            } else {
                game_.BrowseSurvival();
            }
        };
        if ((id == kNightContinueNormal || id == kNightContinueHard || id == kNightContinueNightmare || id == kNightContinueInsanity) && game_.SurvivalAvailable()) {
            const int level = id == kNightContinueNormal ? 1 : id == kNightContinueHard ? 2 : id == kNightContinueNightmare ? 3 : 4;
            game_.SetSurvivalDifficulty(level);
            game_.SetSurvivalStart(pt::game::SurvivalStart::Continue);
            launch_night();
        }
        if (id == kNightDaily && game_.SurvivalAvailable()) {
            game_.SetSurvivalStart(pt::game::SurvivalStart::Daily);
            launch_night();
        }
        if ((id == kNightNormal || id == kNightHard || id == kNightNightmare || id == kNightInsanity) && game_.SurvivalAvailable()) {
            const int level = id == kNightNormal ? 1 : id == kNightHard ? 2 : id == kNightNightmare ? 3 : 4;
            app_.settings.game.survival_difficulty = level;
            game_.SetSurvivalDifficulty(level);
            Save();
            launch_night();
        }
        if (id == kStreetWalk) game_.AnswerStreetOffer(true);
        if (id == kStreetSurvival && game_.SurvivalAvailable()) {
            game_.RequestSurvival();
            game_.AnswerStreetOffer(true);
        }
        if (id == kSurvival) {
            if (game_.SurvivalActive()) {
                game_.StopSurvival();
            } else if (game_.SurvivalAvailable()) {
                game_.StartSurvival();
            }
            game_.RequestMenuClose();
        }
        if (id == kRunMenu) game_.AnswerSpeedrunMenu();
        if (id == kStreetRestart) game_.AnswerStreetOffer(false);
        if (id == kStreetLeave) app_.extras_request = 3;
        if (id == kLoopLock) {
            game_.SetBrowseUnlocks(0, false);
            game_.SetNightUnlocked(false);
            app_.settings.progress = {};
            Save();
            pt::LogInfo("loop browser: unlocks cleared from the Extras page");
        }
        if(id==kLighting) page_=kLightingPage;
        if(id==kTextures) page_=kTexturesPage;
        if(id==kEffects) page_=kEffectsPage;
        if (id == kMods) page_ = kModsPage;
        if (id == kUpdateLater) {
            app_.update_question = false;
            page_ = last_tab_;
            game_.RequestMenuClose();
            pt::LogInfo("update: the player chose Not now at the start's question");
        }
        if ((id == kUpdateNow || id == kUpdateYes) && !app_.update_requested) {
            app_.update_requested = true;
            app_.update_language = game_.Options().subtitle_language;
            Save();
            game_.RequestQuit();
            pt::LogInfo("update: the player chose to update to {}", app_.updates.Newer() ? app_.updates.Newer()->version : std::string("?"));
        }
        if ((id == kNeuralUnlock || id == kNeuralUninstall || id == kNeuralReinstall) && kCommunityRouteRetired && !app_.nr_native_job.Busy() &&
            !app_.nr_native_remove_pending) {
            const auto kind = id == kNeuralUninstall ? pt::nr_native::Job::Kind::Remove
                              : id == kNeuralReinstall ? pt::nr_native::Job::Kind::Reinstall : pt::nr_native::Job::Kind::Install;
            if (kind != pt::nr_native::Job::Kind::Install && app_.settings.upscaling.neural_rendering != "off") {
                app_.settings.upscaling.neural_rendering = "off";
                ApplyNeuralToScene(app_);
            }
            app_.nr_row_note.clear();
            if (app_.nr_native_job.Start(kind, NrDataDir(app_.settings_path), app_.nr_arch)) {
                pt::LogInfo("neural rendering: DLSS 5 {} started (runtime {}{})", kind == pt::nr_native::Job::Kind::Remove ? "remove" : "install",
                            pt::nr_native::kRuntimeVersion, app_.nr_arch > 0 ? std::format(", GPU code sm_{}", app_.nr_arch) : std::string());
            }
        }
        if ((id == kNeuralUnlock || id == kNeuralUninstall || id == kNeuralReinstall) && !kCommunityRouteRetired && app_.NrPending() == 0 && !app_.nr_job.Busy()) {
            const int request = id == kNeuralUninstall ? 2 : id == kNeuralReinstall ? 3 : 1;
            const char* what = id == kNeuralUninstall ? "uninstall" : id == kNeuralReinstall ? "reinstall" : "unlock";
            const auto kind = request == 2 ? pt::nr_unlock::Job::Kind::Remove : pt::nr_unlock::Job::Kind::Install;
            const std::filesystem::path work = (app_.settings_path.empty() ? UserDataDir() : app_.settings_path.parent_path()) / "dlss5-unlock";
            app_.nr_row_note.clear();
            if (app_.nr_job.Start(kind, work)) {
                app_.nr_job_serial = app_.nr_job.Get().serial;
                app_.nr_job_request = request;
                pt::LogInfo("neural rendering: community {} started (downloading {} {})", what, pt::nr_unlock::kToolName, pt::nr_unlock::kToolVersion);
            } else {
                pt::LogWarn("neural rendering: the community {} could not be started", what);
            }
        }
        if ((id == kFgUnlock || id == kFgUninstall || id == kFgReinstall) && app_.FgPending() == 0 && !app_.fg_job.Busy()) {
            const int request = id == kFgUninstall ? 2 : id == kFgReinstall ? 3 : 1;
            if (request != 2 && app_.renderer.Backend() != pt::gpu::Backend::D3D12) {
                app_.fg_row_note = "pc_note_fg_unlock_d3d12";
                pt::LogInfo("frame generation: community install requires the DirectX 12 backend");
                return;
            }
            const char* what = id == kFgUninstall ? "uninstall" : id == kFgReinstall ? "reinstall" : "unlock";
            const auto kind = request == 2 ? pt::fg_unlock::Job::Kind::Remove : request == 3 ? pt::fg_unlock::Job::Kind::Reinstall : pt::fg_unlock::Job::Kind::Install;
            const std::filesystem::path work = (app_.settings_path.empty() ? UserDataDir() : app_.settings_path.parent_path()) / "dlssg-unlock";
            app_.fg_row_note.clear();
            if (request == 2) {
                app_.routes.Request(pt::route_queue::Route::Fg, pt::route_queue::Op::Remove);
                app_.update_language = game_.Options().subtitle_language;
                app_.pending_notices.push_back("pc_notice_fg_remove_ready");
                pt::LogInfo("frame generation: community uninstall queued for the next start");
            } else if (app_.fg_job.Start(kind, work, request == 1 && pt::streamline::ActiveD3D12() ? pt::ExecutableDir() : std::filesystem::path())) {
                app_.fg_job_serial = app_.fg_job.Get().serial;
                app_.fg_job_request = request;
                pt::LogInfo("frame generation: community {} started (downloading {} {})", what, pt::fg_unlock::kToolName, pt::fg_unlock::kToolVersion);
            } else {
                pt::LogWarn("frame generation: the community {} could not be started", what);
            }
        }
        if (id == kGyroCalibrate && app_.settings.accessibility.gyro != 0) {
            gyro_calibrate_no_pad_ = !input_.AnyGyroPad();
            if (gyro_calibrate_no_pad_) {
                pt::LogWarn("input: gyro calibration asked for, but no connected gamepad has a motion sensor");
            } else {
                input_.StartGyroCalibration();
                pt::LogInfo("input: gyro calibration started");
            }
        }
        if (id == kSendLog) pt::SendLogReport(g_log_path, g_output_dir.empty() ? std::filesystem::path(".") : g_output_dir, std::string(pt::update::CurrentVersion()));
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
            app_.microphone_reason = "pc_mic_st_loading";
        }
        if (id == kSoundSettings) page_ = kSoundPage;
        if (id == kAccessibility) page_ = kAccessibilityPage;
        if (id == kHdrSettings) page_ = kHdrPage;
        if (id == kHdrCalibrate || id == kHdrPreview) {
            hdr_screen_ = id == kHdrCalibrate ? HdrScreen::Calibrate : HdrScreen::Preview;
            hdr_step_ = 0;
            keep_cursor_ = true;
            pt::LogInfo("hdr: {} opened", id == kHdrCalibrate ? "calibration" : "look preview");
        }
        if (id == kHdrGradeReset) {
            SetHdrGrade(app_.settings, kHdrGradeOriginal);
            app_.renderer.SetHdrLook(HdrLookOf(app_.settings));
            keep_cursor_ = true;
        }
        if (id == kPatreon) {
            keep_cursor_ = true;
            OpenPatreonPage(app_, page_ == kLoopPage ? "loop browser" : (page_ == kArchivePage || page_ == kArchiveListPage) ? "museum" : "pc settings");
        }
    }

    void CollectGyroCalibration() {
        std::string guid;
        std::array<float, 3> offset{};
        if (!input_.TakeGyroCalibrated(guid, offset)) return;
        app_.settings.accessibility.gyro_bias[guid] = offset;
        input_.settings.gyro_bias = app_.settings.accessibility.gyro_bias;
        Save();
    }

    bool OpenCrosshair() override {
        crosshair_direct_ = true;
        return true;
    }

    bool OpenSurvivalNight() override {
        if (!game_.SurvivalAvailable()) return false;
        night_direct_ = true;
        night_direct_from_death_ = false;
        return true;
    }

    bool OpenSurvivalNightFromDeath() override {
        if (!game_.SurvivalAvailable()) return false;
        night_direct_ = true;
        night_direct_from_death_ = true;
        return true;
    }

    bool OpenSurvivalPromo() override {
        if (!SurvivalPromoDue()) return false;
        promo_direct_ = true;
        return true;
    }

    bool TakeReturnToOptions() override {
        return std::exchange(return_to_options_, false);
    }

    bool TakeCloseToGame() override {
        return std::exchange(close_to_game_, false);
    }

    bool SurvivalPromoDue() const override {
        return !app_.settings.progress.survival_announced && !game_.SurvivalActive();
    }

    bool SurvivalUnlocked() const override {
        return game_.SurvivalAvailable();
    }

    void DismissSurvivalPromo() override {
        if (app_.settings.progress.survival_announced) return;
        app_.settings.progress.survival_announced = true;
        Save();
        pt::LogInfo("survival: 1.1 menu promo dismissed and persisted");
    }

    void Opened() override {
        return_to_options_ = false;
        close_to_game_ = false;
        page_ = game_.StreetOfferPending() ? kStreetPage : app_.update_question ? kUpdatePage : last_tab_;
        if (std::exchange(crosshair_direct_, false) && page_ == last_tab_) {
            crosshair_from_ = -1;
            page_ = kCrosshairPage;
        }
        if (std::exchange(promo_direct_, false) && page_ == last_tab_) {
            page_ = kSurvivalPromoPage;
        }
        if (std::exchange(night_direct_, false) && page_ == last_tab_) {
            night_from_death_ = std::exchange(night_direct_from_death_, false);
            night_from_promo_ = !night_from_death_;
            page_ = kNightPage;
        } else if (page_ != kNightPage) {
            night_from_promo_ = false;
            night_from_death_ = false;
        }
        input_.ClearGyroCalibration();
        rebind_row_ = -1;
        input_.CancelKeyCapture();
        gyro_calibrate_no_pad_ = false;
        app_.microphone_test = app_.microphone_monitor = false;
        archive_full_ = false;
        StopVoice();
        StopMusic();
    }

    void Closed() override {
        RestoreArmoryLooks();
        rebind_row_ = -1;
        input_.CancelKeyCapture();
        app_.microphone_test = app_.microphone_monitor = false;
        archive_full_ = false;
        StopVoice();
        StopMusic();
        if (game_.StreetOfferPending()) game_.AnswerStreetOffer(false);
        if (app_.update_question) {
            app_.update_question = false;
            if (page_ == kUpdatePage) page_ = last_tab_;
            pt::LogInfo("update: the start's question closed without updating");
        }
        if (page_ == kSurvivalPromoPage) {
            DismissSurvivalPromo();
            page_ = last_tab_;
        }
    }

    int RefreshEveryFrames() const override {
        if (rebind_row_ >= 0) return 1;
        return page_ == kMicrophonePage || input_.GyroCalibration() == pt::GyroTracker::Calibration::Running || voice_entry_ || music_.Playing() ? 3 : 15;
    }

    bool Back() override {
        if (TabCount() > 0) {
            return false;
        }
        StopVoice();
        if (page_ == kKeyBindingsPage) {
            if (rebind_row_ >= 0) {
                rebind_row_ = -1;
                input_.CancelKeyCapture();
                return true;
            }
            page_ = kControlsPage;
            int index = 0;
            for (const auto& section : Sections()) for (const auto& row : section.rows) {
                if (row.id == kKeyBindings) pending_cursor_ = index;
                ++index;
            }
            return true;
        }
        if (page_ == kArchiveListPage && !archive_full_) StopMusic();
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
        if (page_ == kCrosshairPage && crosshair_from_ < 0) {
            page_ = last_tab_;
            crosshair_from_ = kControlsPage;
            return false;
        }
        if (page_ == kCrosshairPage) {
            page_ = crosshair_from_;
            const int link = page_ == kNightPage ? kNightCrosshair : kCrosshair;
            int index = 0;
            for (const auto& section : Sections()) for (const auto& row : section.rows) {
                if (row.id == link) pending_cursor_ = index;
                ++index;
            }
            return true;
        }
        if (page_ == kArmoryPage) {
            RestoreArmoryLooks();
            page_ = kNightPage;
            int index = 0;
            for (const auto& section : Sections()) for (const auto& row : section.rows) {
                if (row.id == kNightArmory) pending_cursor_ = index;
                ++index;
            }
            return true;
        }
        if (page_ == kSurvivalPromoPage) {
            DismissSurvivalPromo();
            page_ = last_tab_;
            return false;
        }
        if (page_ == kNightPage && night_from_death_) {
            night_from_death_ = false;
            close_to_game_ = true;
            page_ = last_tab_;
            return false;
        }
        if (page_ == kNightPage && night_from_promo_) {
            night_from_promo_ = false;
            page_ = last_tab_;
            return false;
        }
        const int parent_row = page_ == kMicrophonePage ? kMicrophoneTest : page_ == kLightingPage ? kLighting : page_ == kTexturesPage ? kTextures
                               : page_ == kEffectsPage ? kEffects : page_ == kLoopPage ? kLoopBrowser : page_ == kVrPage ? kVr
                               : page_ == kHdrPage ? kHdrSettings : page_ == kNightPage ? kLoopNight : -1;
        page_ = page_ == kMicrophonePage ? kSoundPage : page_==kLightingPage || page_==kTexturesPage || page_==kEffectsPage ? kGraphicsPage
              : page_ == kLoopPage || page_ == kVrPage || page_ == kNightPage ? kExtrasPage : last_tab_;
        if (page_ != kLoopPage) last_tab_ = page_;
        if (parent_row >= 0) {
            int index = 0;
            for (const auto& section : Sections()) for (const auto& row : section.rows) {
                if (row.id == parent_row) pending_cursor_ = index;
                ++index;
            }
        }
        app_.microphone_test = app_.microphone_monitor = false;
        return true;
    }

private:
    enum RowId {
        kDisplayMode,
        kResolution,
        kRefreshRate,
        kVsync, kFpsLimit, kHdr, kHdrPaperWhite, kHdrPeak, kHdrUiWhite, kHdrReach, kSurround, kControllerSpeaker, kControllerSpeakerVolume, kRumbleProfile, kPromptStyle,
        kUpscaler,
        kQuality,
        kSharpness,
        kDlssModel,
        kRayReconstruction,
        kFrameGeneration,
        kDlssgMultiplier,
        kVolume,
        kOutputDevice,
        kMicrophone,
        kMicrophoneTest, kMicrophoneTrigger, kSoundSettings,
        kMicLevel,
        kMicHeard,
        kMicMonitor,
        kMouse,
        kGamepadSensitivity,
        kCameraTilt,
        kVibration,
        kDeadZone,
        kReset,
        kSendLog,
        kUpdateNow, kUpdateYes, kUpdateLater,
        kRtShadows,
        kGraphics,
        kRtContact,
        kRtAo,
        kAnisotropy,
        kRtReflections,
        kGraphicsPreset,kRayTracing,kLighting,kTextures,kEffects,
        kShadowQuality,kRasterAo,kRasterReflections,kRayQuality,kTextureDetail,
        kBloom,kFlareStrength,kLensGhosts,kDepthOfField,kMotionBlur,kMotionBlurStrength,kFilmGrain,kFilmGrainType,kClarity,kLensDistortion,kLetterbox,
        kMods,
        kLoopBrowser, kExtras, kFreecam, kPhotoMode, kStreetWalk, kStreetRestart, kStreetLeave, kStreetSurvival, kSurvival, kLoopNight,
        kNightNormal, kNightHard, kNightNightmare, kNightInsanity, kNightDaily, kNightContinueNormal, kNightContinueHard, kNightContinueNightmare, kNightContinueInsanity, kNightFinish, kNightTorch, kNightArmory, kPromoPlay, kPromoLater, kArmoryBalance, kArmoryWeapon, kArmoryFilter, kArmorySkin, kArmoryAction, kArmoryApplyAll, kArmoryReload, kArmoryTorch, kArmoryTorchAction, kLoopLock, kThirdPerson, kGamePlus, kNgPlusState, kSpeedrun, kLiveSplit, kRunMenu, kRunReal, kRunGame, kRunBest,
        kArchive, kArchiveLock,
        kVr, kVrMode, kVrFlashlight, kVrTurn, kVrHeight, kVrWorldScale,
        kAccessibility, kSoundCaptions, kSubtitleSize, kSubtitleBg, kReducedFlashing, kHouseSprint, kSprintMode, kZoomMode, kFpsCounter, kSupportPrompts, kGyro, kGyroSensitivity, kGyroCalibrate, kFieldOfView,
        kRenderingApi,
        kNeuralRendering, kNeuralStyle, kNeuralModel, kNeuralQuality, kNeuralIntensity, kNeuralUnlock, kNeuralUninstall, kNeuralReinstall,
        kFgUnlock, kFgReinstall, kFgUninstall,
        kPatreon,
        kHdrSettings, kHdrCalibrate, kHdrGamma, kHdrPreview, kHdrHue, kHdrColourRange,
        kHdrGrade, kHdrExposure, kHdrContrast, kHdrHighlights, kHdrShadows, kHdrSaturation, kHdrHighlightSaturation, kHdrBlowout, kHdrGradeReset,
        kHdrSeeThrough, kHdrWindowsSwitch, kPtFloorReflection, kPtGlassReflection, kPtReflectionSoftness,
        kCrosshair, kNightCrosshair, kCrossPreset, kCrossShape, kCrossColor, kCrossLength, kCrossThickness, kCrossGap, kCrossDynamic, kCrossDot,
        kCrossOutline, kCrossOpacity, kCrossHitMarker, kCrossSights, kCrossReset,
        kKeyBindings, kKeyBindReset,
        kLoopFirst = 1000,
        kRunSplitFirst = 3000,
        kModFirst = 2000,
        kArchiveSectionFirst = 3000,
        kArchiveEntryFirst = 4000,
        kKeyBindFirst = 5000,
    };
    static constexpr int kArchivePage = 10;
    static constexpr int kArchiveListPage = 11;
    int archive_section_ = 0;
    bool archive_full_ = false;
    int pending_cursor_ = -1;
    bool keep_cursor_ = false;
    bool gyro_calibrate_no_pad_ = false;
    const pt::game::ArchiveEntry* voice_entry_ = nullptr;
    uint32_t voice_id_ = 0;
    uint64_t voice_start_ = 0;

    int Language() const { return std::clamp(game_.Options().subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1); }

    const pt::game::ArchiveEntry* EntryOf(int id) const {
        const auto entries = pt::game::ArchiveEntries();
        const int index = id - kArchiveEntryFirst;
        return index >= 0 && index < static_cast<int>(entries.size()) ? &entries[index] : nullptr;
    }

    pt::game::ArchiveTally ArchiveTallyOf(int section) const {
        return pt::game::ArchiveCount(pt::game::ArchiveEntries(), section, [&](const pt::game::ArchiveEntry& e) { return game_.ArchiveUnlocked(e); });
    }
    std::string ArchiveFoundText(const pt::game::ArchiveTally& tally, int language) const {
        if (tally.total == 0) return pt::game::PcNoteText(std::format("pc_archive_bonus{}{}", pt::game::kPcNoteArgument, tally.bonus), language);
        return pt::game::PcNoteText(std::format("pc_archive_found{}{}{}{}", pt::game::kPcNoteArgument, tally.found, pt::game::kPcNoteArgument, tally.total),
                                    language);
    }

    std::vector<pt::game::PcSettingSection> ArchiveSections() const {
        const int language = Language();
        const pt::game::ArchiveTally all = ArchiveTallyOf(-1);
        std::string header = std::format("{}   {}", pt::game::PcText("pc_archive_sections", language),
                                         pt::game::PcNoteText(std::format("pc_archive_complete{}{}{}{}{}{}", pt::game::kPcNoteArgument,
                                                                          pt::game::ArchivePercent(all), pt::game::kPcNoteArgument, all.found,
                                                                          pt::game::kPcNoteArgument, all.total), language));
        if (all.bonus > 0) header += "   " + pt::game::PcNoteText(std::format("pc_archive_bonus{}{}", pt::game::kPcNoteArgument, all.bonus), language);
        if (archive_logged_ != std::make_tuple(all.found, all.total, all.bonus)) {
            archive_logged_ = {all.found, all.total, all.bonus};
            pt::LogInfo("museum: {} / {} found ({}%), bonus {}", all.found, all.total, pt::game::ArchivePercent(all), all.bonus);
        }
        pt::game::PcSettingSection sections{header, 0, {}};
        for (int s = 0; s < pt::game::kArchiveSectionCount; ++s) {
            const auto section = static_cast<pt::game::ArchiveSection>(s);
            const pt::game::ArchiveTally tally = ArchiveTallyOf(s);
            if (tally.total == 0 && tally.bonus == 0) continue;
            auto row = Row(kArchiveSectionFirst + s, std::string(pt::game::ArchiveSectionTitle(section)), {ArchiveFoundText(tally, language)}, 0,
                           std::string(pt::game::ArchiveSectionNote(section)));
            row.link = true;
            sections.rows.push_back(std::move(row));
        }
        if (app_.options.release_locks) {
            auto lock = Row(kArchiveLock, "pc_archive_lock", {"pc_extras_start"}, 0, "pc_note_archive_lock");
            lock.action = true;
            lock.confirm_note = "pc_dlg_archive_lock";
            lock.enabled = !game_.ArchiveSeen().empty();
            sections.rows.push_back(std::move(lock));
        }
        return {std::move(sections)};
    }
    mutable std::tuple<int, int, int> archive_logged_{-1, -1, -1};

    std::vector<pt::game::PcSettingSection> ArchiveListSections() {
        const auto section = static_cast<pt::game::ArchiveSection>(archive_section_);
        const int language = Language();
        pt::game::PcSettingSection list{std::format("{}   {}", pt::game::PcText(pt::game::ArchiveSectionTitle(section), language),
                                                    ArchiveFoundText(ArchiveTallyOf(archive_section_), language)), 0, {}};
        if (voice_id_ && (!game_.Audio() || !game_.Audio()->IsPlaying(voice_id_))) {
            voice_id_ = 0;
            voice_entry_ = nullptr;
        }
        if (auto* sound = dynamic_cast<pt::game::GameSound*>(game_.Audio())) music_.Update(*sound);
        const auto entries = pt::game::ArchiveEntries();
        for (size_t i = 0; i < entries.size(); ++i) {
            const pt::game::ArchiveEntry& e = entries[i];
            if (e.section != section) continue;
            const bool open = game_.ArchiveUnlocked(e);
            if (e.secret && !open) continue;
            const std::string note = !open ? pt::game::ArchiveHint(e) : voice_entry_ == &e ? std::string("pc_note_archive_playing") : std::string(e.note);
            auto row = Row(kArchiveEntryFirst + static_cast<int>(i), open ? pt::game::ArchiveLabel(e, language) : std::string(pt::game::kArchiveLockedLabel),
                           {""}, 0, note);
            row.link = true;
            row.enabled = open;
            list.rows.push_back(std::move(row));
        }
        return {std::move(list)};
    }

    std::filesystem::path MuseumPreviewDirectory() const {
        if (!app_.options.museum_previews.empty()) return app_.options.museum_previews;
        return UserDataDir() / std::format("museum-previews-v{}", kMuseumPreviewVersion);
    }
    bool MuseumPreviewsGenerating() const {
        if (!museum_process_.Started()) return false;
        int code = 0;
        if (museum_process_.Running(&code)) return true;
        pt::LogInfo("museum: thumbnail capture finished (exit {})", code);
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
        std::vector<std::string> args{"--game", pt::os::PathToUtf8(app_.options.game_dir), "--make-museum-previews", pt::os::PathToUtf8(dir),
            "--log", pt::os::PathToUtf8((dir / "capture.log"))};
        museum_started_ = museum_process_.Start(exe, args, pt::ExecutableDir());
        if (museum_started_) pt::LogInfo("museum: thumbnail capture started ({})", pt::os::PathToUtf8(dir));
        else pt::LogWarn("museum: thumbnail capture could not start ({})", pt::os::PathToUtf8(exe));
    }
    bool museum_started_ = false;
    mutable pt::os::ChildProcess museum_process_;

    void ActivateEntry(const pt::game::ArchiveEntry& entry) {
        using M = pt::game::ArchiveMedia;
        keep_cursor_ = true;
        if (!game_.ArchiveUnlocked(entry)) return;
        switch (entry.media) {
        case M::Image:
        case M::String:
        case M::Screen:
        case M::Photo:
            archive_full_ = true;
            break;
        case M::Sound:
        case M::Dialogue:
            StopMusic();
            if (voice_entry_ == &entry) {
                StopVoice();
            } else {
                PlayVoice(entry);
            }
            break;
        case M::Music:
            StopVoice();
            PlayPauseMusic(entry);
            break;
        case M::Demo:
        case M::Model:
        case M::Scene:
            StopVoice();
            StopMusic();
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

    pt::game::MuseumMusic music_;

    pt::game::GameSound* MusicSound() const {
        auto* sound = dynamic_cast<pt::game::GameSound*>(game_.Audio());
        return sound && sound->Ready() ? sound : nullptr;
    }
    double MusicLength(const pt::game::ArchiveEntry& entry) const {
        pt::game::GameSound* sound = MusicSound();
        if (!sound || !entry.asset.starts_with("media:")) return entry.seconds;
        const auto media = pt::game::MuseumMusic::LoadSource(game_.GetVfs(), *sound, entry.asset);
        if (!media || media->SampleRate() == 0) return entry.seconds;
        const double whole = static_cast<double>(media->Frames()) / media->SampleRate();
        return entry.seconds > 0.0f ? std::min(whole, static_cast<double>(entry.seconds)) : whole;
    }
    void PlayPauseMusic(const pt::game::ArchiveEntry& entry) {
        pt::game::GameSound* sound = MusicSound();
        if (!sound) {
            pt::LogWarn("archive: no sound to play {}", entry.id);
            return;
        }
        if (music_.Entry() == &entry && music_.Playing()) {
            music_.Pause(*sound);
        } else if (music_.Entry() == &entry && music_.Paused()) {
            music_.Resume(*sound);
        } else {
            music_.Play(game_.GetVfs(), *sound, entry);
        }
    }
    void StopMusic() {
        music_.Stop(MusicSound());
    }
    void Music(int id, pt::game::PcMusicCommand command, float value) override {
        const pt::game::ArchiveEntry* entry = EntryOf(id);
        pt::game::GameSound* sound = MusicSound();
        if (!entry || entry->media != pt::game::ArchiveMedia::Music || !game_.ArchiveUnlocked(*entry) || !sound) return;
        using C = pt::game::PcMusicCommand;
        StopVoice();
        const bool loaded = music_.Entry() == entry;
        switch (command) {
        case C::PlayPause:
            PlayPauseMusic(*entry);
            break;
        case C::SeekBy:
            if (loaded) music_.Seek(*sound, music_.Position() + value);
            else music_.Play(game_.GetVfs(), *sound, *entry, std::max(0.0f, value));
            break;
        case C::SeekTo: {
            const double length = loaded ? music_.Length() : MusicLength(*entry);
            if (loaded) music_.Seek(*sound, value * length);
            else music_.Play(game_.GetVfs(), *sound, *entry, value * length);
            break;
        }
        case C::Restart:
            if (loaded) {
                music_.Seek(*sound, 0.0);
                if (music_.Paused()) music_.Resume(*sound);
            } else {
                music_.Play(game_.GetVfs(), *sound, *entry);
            }
            break;
        case C::Loop:
            music_.SetLoop(!music_.Loop());
            break;
        }
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
            const std::filesystem::path directory = pt::ExecutableDir() / "loop-previews";
            std::error_code ec;
            int version = 0;
            if (!directory.empty()) std::ifstream(directory / "version.txt") >> version;
            return version == kPreviewVersion && std::filesystem::exists(directory / "loop-0.png", ec) ? directory : std::filesystem::path();
        }();
        return bundled;
    }
    static std::filesystem::path PreviewDirectory() {
        if (auto bundled = BundledPreviewDirectory(); !bundled.empty()) return bundled;
        return UserDataDir() / std::format("loop-previews-v{}", kPreviewVersion);
    }
    bool PreviewsGenerating() {
        if (!preview_process_.Started()) return false;
        int code = 0;
        if (preview_process_.Running(&code)) return true;
        pt::LogInfo("loop browser: preview generation finished (exit {})", code);
        if (preview_runs_ < 2) {
            preview_started_ = false;
            GeneratePreviews();
            return preview_process_.Started();
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
        std::vector<std::string> args{"--headless", "--no-save", "--audio-offline", "--game", pt::os::PathToUtf8(app_.options.game_dir),
            "--frames", "120000", "--demo-rate", "20", "--street-offer", "never", "--seed", std::to_string(kLoopPreviewSeed),
            "--bug-screen", std::to_string(kLoopPreviewBugScreen), "--f160-light", std::to_string(kLoopPreviewF160Roll), "--shot-warmup", "30", "--shot-settle", "--width", "640", "--height", "360", "--input-script", pt::os::PathToUtf8((dir/"capture.txt")),
            "--settings", pt::os::PathToUtf8((dir/"preview.ini")), "--log", pt::os::PathToUtf8((dir/"capture.log"))};
        preview_started_ = preview_process_.Start(exe, args, pt::ExecutableDir());
        if (preview_started_) ++preview_runs_;
        if (preview_started_) pt::LogInfo("loop browser: generating {} preview(s)", missing.size());
        else pt::LogWarn("loop browser: preview generation could not start ({})", pt::os::PathToUtf8(exe));
    }
    bool preview_started_ = false;
    int preview_runs_ = 0;
    pt::os::ChildProcess preview_process_;
    static constexpr int kLoopPage = 6;
    static constexpr int kNightPage = 20;
    static constexpr int kArmoryPage = 23;
    static constexpr int kSurvivalPromoPage = 24;
    int armory_weapon_ = 1;
    int armory_mode_ = 0;
    int armory_filter_ = 0;
    int armory_skin_ = 0;
    int armory_anim_ = 0;
    int armory_torch_ = 0;
    float armory_angle_ = 0.55f;
    float armory_pitch_ = 0.0f;
    float armory_drag_cooldown_ = 0.0f;
    float armory_reload_timer_ = 0.0f;
    float armory_reload_length_ = 1.6f;
    float armory_fx_time_ = 0.0f;
    int last_armory_weapon_ = -1;
    int last_armory_skin_ = -1;
    int last_armory_torch_ = -1;
    bool armory_preview_dirty_ = false;
    bool promo_direct_ = false;
    bool night_direct_from_death_ = false;
    bool night_from_death_ = false;
    bool close_to_game_ = false;
    bool return_to_options_ = false;
    std::vector<int> ArmoryFilteredSkins() const {
        const pt::game::SurvivalProfile& profile = game_.SurvivalProfileData();
        std::vector<int> out;
        for (int i = 0; i < pt::game::kSurvivalSkinCount; ++i) {
            const pt::game::SurvivalSkinDef& sk = pt::game::SurvivalSkin(i);
            if (armory_filter_ == 0) out.push_back(i);
            else if (armory_filter_ >= 1 && armory_filter_ <= 5 && static_cast<int>(sk.rarity) == armory_filter_ - 1) out.push_back(i);
            else if (armory_filter_ == 6 && profile.OwnsSkin(i)) out.push_back(i);
        }
        return out;
    }
    void RestoreArmoryLooks() {
        last_armory_weapon_ = last_armory_skin_ = last_armory_torch_ = -1;
        if (!armory_preview_dirty_) return;
        armory_preview_dirty_ = false;
        if (pt::game::SurvivalNight* s = game_.EnsureSurvival()) {
            const pt::game::SurvivalModels* sm = s->ArmoryModelsIfLoaded();
            if (sm) {
                const pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
                for (size_t w = 1; w < sm->guns.size(); ++w) {
                    if (sm->guns[w].body) {
                        pt::game::ApplyWeaponSkin(app_.textures, sm->guns[w], game_.SurvivalActive() ? prof.EquippedSkin(static_cast<int>(w)) : 0);
                    }
                }
                if (sm->chest_light) {
                    pt::game::ApplyTorchSkin(app_.textures, *sm, game_.SurvivalActive() ? prof.EquippedTorch() : 0, 60.0f);
                }
            }
            s->RefreshLooks();
        }
    }
    static constexpr int kCrosshairPage = 21;
    int crosshair_from_ = 17;
    bool crosshair_direct_ = false;
    bool night_direct_ = false;
    bool night_from_promo_ = false;
    static constexpr int kKeyBindingsPage = 22;
    int rebind_row_ = -1;
    static constexpr int kModsPage = 7;
    static constexpr int kExtrasPage = 8;
    static constexpr int kStreetPage = 9;
    static constexpr int kUpdatePage = 18;
    static constexpr int kVrPage = 12;
    static constexpr int kHdrPage = 19;
    static constexpr int kMaxModRows = 20;
    static constexpr int kMainPage = 0;
    static constexpr int kGraphicsPage = 1;
    static constexpr int kMicrophonePage = 2;
    static constexpr int kSoundPage = 13;
    static constexpr int kAccessibilityPage = 14;
    static constexpr int kDisplayPage = 15;
    static constexpr int kUpscalingPage = 16;
    static constexpr int kControlsPage = 17;
    static constexpr int kSettingPages[] = {kDisplayPage, kHdrPage, kUpscalingPage, kGraphicsPage, 3, 4, 5, kSoundPage, kControlsPage,
                                            kAccessibilityPage, kExtrasPage, kVrPage, kModsPage};
    static constexpr float kGyroSensitivitySteps[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};
    static constexpr int kFovSteps[] = {-10, 0, 10, 20, 30};
    static int FovStep(int degrees) {
        int best = 0;
        for (int i = 1; i < static_cast<int>(std::size(kFovSteps)); ++i) {
            if (std::abs(kFovSteps[i] - degrees) < std::abs(kFovSteps[best] - degrees)) best = i;
        }
        return best;
    }
    static constexpr int kLightingPage=3,kTexturesPage=4,kEffectsPage=5;
    static constexpr int kAnisotropySteps[] = {0, 2, 4, 8, 16};
    int page_ = kDisplayPage;
    int last_tab_ = kDisplayPage;
    static constexpr float kMouseSteps[] = {0.25f, 0.35f, 0.5f, 0.7f, 1.0f, 1.4f, 2.0f, 2.8f, 4.0f, 5.0f};
    static constexpr float kGamepadSensitivitySteps[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};
    static constexpr float kDeadZoneSteps[] = {0.0f, 0.05f, 26.0f / 255.0f, 0.15f, 0.2f, 0.25f, 0.3f};

    static std::string StepText(float step) {
        std::string text = std::format("{:.2f}", step);
        if (text.back() == '0') text.pop_back();
        return text;
    }

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

    std::vector<pt::game::PcSettingSection> HdrSections() const {
        pt::game::PcSettingSection levels{"pc_hdr_levels", 0, {}};
        auto calibrate = Row(kHdrCalibrate, "pc_hdr_calibrate", {"pc_graphics_value"}, 0, "pc_note_hdr_calibrate");
        calibrate.link = true;
        levels.rows.push_back(std::move(calibrate));
        for (const int id : kHdrCalibrateSteps) levels.rows.push_back(HdrRow(id));
        pt::game::PcSettingSection picture{"pc_hdr_picture", 0, {}};
        auto preview = Row(kHdrPreview, "pc_hdr_preview", {"pc_graphics_value"}, 0, "pc_note_hdr_preview");
        preview.link = true;
        picture.rows.push_back(std::move(preview));
        for (const int id : kHdrPictureRows) picture.rows.push_back(HdrRow(id));
        picture.rows.push_back(HdrRow(kHdrSeeThrough));
        pt::game::PcSettingSection grading{"pc_hdr_grading", 0, {}};
        for (const int id : kHdrGradeRows) grading.rows.push_back(HdrRow(id));
        auto reset = Row(kHdrGradeReset, "pc_hdr_grade_reset", {"pc_graphics_value"}, 0, "pc_note_hdr_grade_reset");
        reset.link = true;
        grading.rows.push_back(std::move(reset));
        return {std::move(levels), std::move(picture), std::move(grading)};
    }

    struct HdrGradeValues {
        int exposure, contrast, highlights, shadows, saturation, highlight_saturation, blowout;
        bool operator==(const HdrGradeValues&) const = default;
    };
    static constexpr HdrGradeValues kHdrGradeOriginal{0, 100, 100, 100, 100, 100, 0};
    static constexpr HdrGradeValues kHdrGradeRich{0, 105, 120, 100, 130, 110, 0};
    static HdrGradeValues HdrGradeOf(const pt::AppSettings& s) {
        const auto& d = s.display;
        return {d.hdr_exposure, d.hdr_contrast, d.hdr_highlights, d.hdr_shadows, d.hdr_saturation, d.hdr_highlight_saturation, d.hdr_blowout};
    }
    static void SetHdrGrade(pt::AppSettings& s, const HdrGradeValues& v) {
        auto& d = s.display;
        d.hdr_exposure = v.exposure;
        d.hdr_contrast = v.contrast;
        d.hdr_highlights = v.highlights;
        d.hdr_shadows = v.shadows;
        d.hdr_saturation = v.saturation;
        d.hdr_highlight_saturation = v.highlight_saturation;
        d.hdr_blowout = v.blowout;
    }

    pt::game::PcSettingRow HdrRow(int id) const {
        const auto& d = app_.settings.display;
        const auto stepped = [](int row_id, const char* label, const char* note, const std::vector<int>& steps, int current, auto text) {
            pt::game::PcSettingRow row = Row(row_id, label, {}, 0, note);
            for (size_t i = 0; i < steps.size(); ++i) {
                row.values.push_back(text(steps[i]));
                if (steps[i] == current) row.value = static_cast<int>(i);
            }
            row.wrap = false;
            return row;
        };
        const auto nits = [](int v) { return std::format("{} nits", v); };
        const auto percent = [](int v) { return std::format("{}%", v); };
        switch (id) {
        case kHdrPeak:
            return stepped(id, "pc_hdr_peak", "pc_note_hdr_peak", HdrPeakSteps(d.hdr_peak), d.hdr_peak,
                           [&](int v) { return v == 0 ? std::string("pc_hdr_peak_auto") : nits(v); });
        case kHdrPaperWhite:
            return stepped(id, "pc_hdr_paper_white", "pc_note_hdr_paper_white", HdrPaperWhiteSteps(d.hdr_paper_white), d.hdr_paper_white, nits);
        case kHdrUiWhite:
            return stepped(id, "pc_hdr_ui_white", "pc_note_hdr_ui_white", HdrUiWhiteSteps(d.hdr_ui_white), d.hdr_ui_white,
                           [&](int v) { return v == 0 ? std::string("pc_hdr_ui_white_same") : nits(v); });
        case kHdrGamma: {
            auto row = Row(id, "pc_hdr_gamma", {"pc_hdr_gamma_22", "pc_hdr_gamma_srgb", "pc_hdr_gamma_24"}, std::clamp(d.hdr_gamma, 0, 2),
                           "pc_note_hdr_gamma");
            row.wrap = false;
            return row;
        }
        case kHdrReach: {
            auto row = Row(id, "pc_hdr_reach", {"pc_hdr_reach_natural", "pc_hdr_reach_bright", "pc_hdr_reach_intense"},
                           d.hdr_reach >= 175 ? 2 : d.hdr_reach >= 125 ? 1 : 0, "pc_note_hdr_reach");
            row.wrap = false;
            return row;
        }
        case kHdrHue: {
            auto row = Row(id, "pc_hdr_hue", {"pc_hdr_hue_game", "pc_hdr_hue_keep"}, d.hdr_keep_hue ? 1 : 0, "pc_note_hdr_hue");
            row.wrap = false;
            return row;
        }
        case kHdrColourRange: {
            auto row = Row(id, "pc_hdr_colour_range", {"pc_hdr_colour_range_game", "pc_hdr_colour_range_wide", "pc_hdr_colour_range_extended"},
                           d.hdr_colour_range == 2 ? 1 : d.hdr_colour_range == 1 ? 2 : 0,
                           d.hdr_colour_range == 2 ? "pc_note_hdr_colour_range_wide" : d.hdr_colour_range == 1 ? "pc_note_hdr_colour_range" : "pc_note_hdr_colour_range_game");
            row.wrap = false;
            return row;
        }
        case kHdrGrade: {
            const HdrGradeValues now = HdrGradeOf(app_.settings);
            auto row = Row(id, "pc_hdr_grade", {"pc_hdr_grade_original", "pc_hdr_grade_rich"}, now == kHdrGradeRich ? 1 : 0, "pc_note_hdr_grade");
            if (now != kHdrGradeOriginal && now != kHdrGradeRich) {
                row.values.push_back("pc_quality_custom");
                row.value = 2;
            }
            row.wrap = false;
            return row;
        }
        case kHdrExposure: {
            std::vector<int> steps;
            for (int v = -30; v <= 30; ++v) steps.push_back(v);
            return stepped(id, "pc_hdr_exposure", "pc_note_hdr_exposure", steps, d.hdr_exposure, [](int v) { return std::format("{:+.1f} EV", v * .1f); });
        }
        case kHdrContrast: return stepped(id, "pc_hdr_contrast", "pc_note_hdr_contrast", HdrPercentSteps(200), d.hdr_contrast, percent);
        case kHdrHighlights: return stepped(id, "pc_hdr_highlights", "pc_note_hdr_highlights", HdrPercentSteps(200), d.hdr_highlights, percent);
        case kHdrShadows: return stepped(id, "pc_hdr_shadows", "pc_note_hdr_shadows", HdrPercentSteps(200), d.hdr_shadows, percent);
        case kHdrSaturation: return stepped(id, "pc_hdr_saturation", "pc_note_hdr_saturation", HdrPercentSteps(200), d.hdr_saturation, percent);
        case kHdrHighlightSaturation:
            return stepped(id, "pc_hdr_highlight_saturation", "pc_note_hdr_highlight_saturation", HdrPercentSteps(200), d.hdr_highlight_saturation, percent);
        case kHdrBlowout: return stepped(id, "pc_hdr_blowout", "pc_note_hdr_blowout", HdrPercentSteps(100), d.hdr_blowout, percent);
        case kHdrSeeThrough: return Row(id, "pc_hdr_see_through", OffOn(), d.hdr_see_through ? 1 : 0, "pc_note_hdr_see_through");
        default:
            return Row(id, "", {""}, 0, "");
        }
    }

    enum class HdrScreen { None, Calibrate, Preview };
    static constexpr int kHdrCalibrateSteps[] = {kHdrPeak, kHdrPaperWhite, kHdrGamma, kHdrUiWhite};
    static constexpr int kHdrCalibratePatterns[] = {1, 2, 4, 3};
    static constexpr const char* kHdrCalibrateNotes[] = {"pc_hdr_calibrate_peak", "pc_hdr_calibrate_paper", "pc_hdr_calibrate_shadows",
                                                         "pc_hdr_calibrate_menus"};
    static constexpr int kHdrPictureRows[] = {kHdrReach, kHdrHue, kHdrColourRange};
    static constexpr int kHdrGradeRows[] = {kHdrGrade, kHdrExposure, kHdrHighlights, kHdrShadows, kHdrContrast, kHdrSaturation, kHdrHighlightSaturation, kHdrBlowout};
    static constexpr int kHdrPreviewRows[] = {kHdrReach, kHdrHue, kHdrColourRange, kHdrGrade, kHdrExposure, kHdrHighlights, kHdrShadows, kHdrContrast,
                                              kHdrSaturation, kHdrHighlightSaturation, kHdrBlowout};
    static std::vector<int> HdrPercentSteps(int last) {
        std::vector<int> steps;
        for (int v = 0; v <= last; v += 5) steps.push_back(v);
        return steps;
    }
    HdrScreen hdr_screen_ = HdrScreen::None;
    int hdr_step_ = 0;

    bool ScreenShown() const override {
        return page_ == kHdrPage && hdr_screen_ != HdrScreen::None && app_.settings.display.hdr && app_.display_hdr != 0;
    }

    bool SeeThrough() const override {
        if (page_ == kArmoryPage) return true;
        const bool picture_page = page_ == kHdrPage || page_ == kGraphicsPage || page_ == kLightingPage || page_ == kTexturesPage || page_ == kEffectsPage;
        return picture_page && app_.settings.display.hdr && app_.display_hdr != 0 && app_.settings.display.hdr_see_through;
    }

public:
    bool ArmoryPreviewActive() const { return page_ == kArmoryPage; }
    bool IsArmory() const override { return page_ == kArmoryPage; }

    bool IsNightHub() const override { return page_ == kNightPage; }

    pt::game::NightHubSnapshot NightHubData() const override {
        pt::game::NightHubSnapshot out;
        const pt::game::SurvivalProfile& profile = game_.SurvivalProfileData();
        out.credits = profile.credits;
        out.best_wave = app_.settings.game.survival_best_wave;
        out.daily_streak = profile.daily_streak;

        int owned_skins = 0;
        for (int i = 0; i < pt::game::kSurvivalSkinCount; ++i) {
            if (profile.OwnsSkin(i)) ++owned_skins;
        }

        pt::game::NightHubSection diffs;
        diffs.title_key = "pc_survival_choose";
        struct DiffEntry {
            int id;
            const char* label;
            const char* note;
            const char* mult;
            size_t slot;
        };
        static constexpr std::array<DiffEntry, 4> kDiffs{{
            {kNightNormal, "pc_survival_normal", "pc_note_survival_pick_normal", "x1.0", 1},
            {kNightHard, "pc_survival_hard", "pc_note_survival_pick_hard", "x1.4", 2},
            {kNightNightmare, "pc_survival_nightmare", "pc_note_survival_pick_nightmare", "x2.0", 3},
            {kNightInsanity, "pc_survival_insanity", "pc_note_survival_pick_insanity", "x3.0", 4},
        }};
        for (const auto& d : kDiffs) {
            pt::game::NightHubItem it;
            it.id = d.id;
            it.label_key = d.label;
            it.tag = d.mult;
            it.note = d.note;
            it.stats.push_back({"pc_armory_credits", d.mult, false});
            const int rec_score = profile.best_score[d.slot];
            if (rec_score > 0) {
                it.stats.push_back({"pc_survival_difficulty", std::format("{}", rec_score), false});
            }
            diffs.items.push_back(std::move(it));
        }
        out.sections.push_back(std::move(diffs));

        pt::game::NightHubSection runs;
        runs.title_key = "pc_survival_runs";
        static constexpr std::array<std::pair<int, const char*>, 4> kKept{{
            {kNightContinueNormal, "pc_survival_continue_normal"},
            {kNightContinueHard, "pc_survival_continue_hard"},
            {kNightContinueNightmare, "pc_survival_continue_nightmare"},
            {kNightContinueInsanity, "pc_survival_continue_insanity"},
        }};
        for (size_t d = 1; d <= 4; ++d) {
            const pt::game::SurvivalCheckpoint& kept = profile.checkpoints[d];
            if (kept.wave <= 0) continue;
            const int kept_night = pt::game::SurvivalNightOf(kept.wave);
            pt::game::NightHubItem it;
            it.id = kKept[d - 1].first;
            it.label_key = kKept[d - 1].second;
            it.tag = std::format("#{}", kept.wave);
            it.note = kept_night > 1
                          ? std::format("pc_note_survival_continue_night{}{}", pt::game::kPcNoteArgument, kept_night)
                          : std::format("pc_note_survival_continue{}{}{}{}", pt::game::kPcNoteArgument, kept.wave, pt::game::kPcNoteArgument, kept.score);
            runs.items.push_back(std::move(it));
        }
        {
            const int today_date = pt::game::SurvivalToday();
            const bool today = profile.daily_date == today_date;
            const pt::game::DailyMutatorDef& mut = pt::game::DailyMutatorForDate(today_date);
            const char arg = pt::game::kPcNoteArgument;
            pt::game::NightHubItem it;
            it.id = kNightDaily;
            it.label_key = "pc_survival_daily";
            if (today && profile.daily_wave > 0) {
                it.tag = std::format("#{}", profile.daily_wave);
                it.tag_is_key = false;
            } else {
                it.tag = mut.name_key;
                it.tag_is_key = true;
            }
            it.note = mut.note_key;
            it.detail_sub = std::format("pc_note_survival_daily_mut{}{}{}{}{}{}{}{}", arg, mut.name_key, arg, mut.note_key, arg,
                                        profile.daily_wins, arg, profile.daily_streak);
            it.stats.push_back({"pc_survival_daily", mut.name_key, true});
            it.stats.push_back({"pc_armory_credits", std::format("x{:.1f}", mut.credits_mult), false});
            runs.items.push_back(std::move(it));
        }
        out.sections.push_back(std::move(runs));

        pt::game::NightHubSection gear;
        gear.title_key = "pc_survival_gear";
        {
            pt::game::NightHubItem armory;
            armory.id = kNightArmory;
            armory.label_key = "pc_armory_title";
            armory.tag = std::format("${}", profile.credits);
            armory.note = "pc_note_armory_open";
            armory.stats.push_back({"pc_armory_credits", std::format("${}", profile.credits), false});
            armory.stats.push_back({"pc_armory_tab_skins", std::format("{} / {}", owned_skins, pt::game::kSurvivalSkinCount), false});
            armory.stats.push_back({"pc_armory_torch", pt::game::SurvivalTorch(profile.EquippedTorch()).name_key, true});
            gear.items.push_back(std::move(armory));
        }
        {
            pt::game::NightHubItem cross;
            cross.id = kNightCrosshair;
            cross.label_key = "pc_crosshair";
            cross.note = "pc_note_survival_pick_crosshair";
            gear.items.push_back(std::move(cross));
        }
        out.sections.push_back(std::move(gear));
        return out;
    }

    pt::game::ReloadStyle ArmoryActiveReloadStyle() const {
        const pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
        const int w = std::clamp(armory_weapon_, 1, 5);
        if (armory_mode_ == 1) {
            if (armory_anim_ >= 1 && armory_anim_ <= 4) {
                return static_cast<pt::game::ReloadStyle>(armory_anim_ - 1);
            }
            return pt::game::SurvivalSkin(prof.EquippedSkin(w)).reload;
        }
        return pt::game::SurvivalSkin(armory_skin_).reload;
    }

    pt::game::ArmorySnapshot ArmoryData() const override {
        const pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
        pt::game::ArmorySnapshot snap;
        snap.weapon = std::clamp(armory_weapon_, 1, 6);
        snap.mode = snap.weapon == 6 ? 0 : std::clamp(armory_mode_, 0, 1);
        snap.filter = std::clamp(armory_filter_, 0, 6);
        snap.selected_skin = std::clamp(armory_skin_, 0, pt::game::kSurvivalSkinCount - 1);
        snap.selected_anim = std::clamp(armory_anim_, 0, 4);
        snap.selected_torch = std::clamp(armory_torch_, 0, pt::game::kSurvivalTorchCount - 1);
        snap.credits = prof.credits;
        snap.best_wave = app_.settings.game.survival_best_wave;
        snap.daily_wins = prof.daily_wins;
        const int w = std::clamp(armory_weapon_, 1, 5);
        snap.equipped_skin = prof.EquippedSkin(w);
        snap.equipped_anim = prof.EquippedReloadChoice(w);
        snap.equipped_torch = prof.EquippedTorch();
        snap.playing_reload = armory_reload_timer_ > 0.0f;
        snap.playing_style = static_cast<int>(ArmoryActiveReloadStyle());
        snap.owned_skins = prof.owned_skins | 1ull;
        uint32_t reloads_mask = 1u;
        for (int s = 0; s < 4; ++s) {
            if (prof.OwnsReload(s)) reloads_mask |= (1u << static_cast<unsigned>(s));
        }
        snap.owned_reloads = reloads_mask;
        snap.owned_torches = prof.owned_torches | 1u;
        snap.filtered_skins = ArmoryFilteredSkins();
        return snap;
    }

    void ArmorySelectWeapon(int weapon) override {
        armory_weapon_ = std::clamp(weapon, 1, 6);
        if (armory_weapon_ <= 5) {
            const pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
            armory_skin_ = prof.EquippedSkin(armory_weapon_);
            armory_anim_ = prof.EquippedReloadChoice(armory_weapon_);
            const std::vector<int> list = ArmoryFilteredSkins();
            if (!list.empty() && std::find(list.begin(), list.end(), armory_skin_) == list.end()) {
                armory_skin_ = list.front();
            }
            if (armory_mode_ == 1) ArmoryPreviewReload();
            else armory_reload_timer_ = 0.0f;
        } else {
            armory_torch_ = game_.SurvivalProfileData().EquippedTorch();
            armory_reload_timer_ = 0.0f;
        }
    }

    void ArmorySelectMode(int mode) override {
        armory_mode_ = std::clamp(mode, 0, 1);
        if (armory_weapon_ <= 5) {
            const pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
            armory_anim_ = prof.EquippedReloadChoice(armory_weapon_);
            if (armory_mode_ == 1) {
                ArmoryPreviewReload();
            } else {
                armory_reload_timer_ = 0.0f;
            }
        }
    }

    void ArmorySelectFilter(int filter) override {
        armory_filter_ = std::clamp(filter, 0, 6);
        const std::vector<int> list = ArmoryFilteredSkins();
        if (!list.empty() && std::find(list.begin(), list.end(), armory_skin_) == list.end()) {
            armory_skin_ = list.front();
        }
    }

    void ArmoryFocusItem(int item) override {
        if (armory_weapon_ == 6) {
            armory_torch_ = std::clamp(item, 0, pt::game::kSurvivalTorchCount - 1);
            return;
        }
        if (armory_mode_ == 1) {
            armory_anim_ = std::clamp(item, 0, 4);
            ArmoryPreviewReload();
            return;
        }
        armory_skin_ = std::clamp(item, 0, pt::game::kSurvivalSkinCount - 1);
    }

    void ArmoryAction(bool apply_all) override {
        pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
        auto persist = [&]() {
            game_.MarkSurvivalProfile();
            app_.settings.game.survival_profile = prof.Save();
            Save();
            if (pt::game::SurvivalNight* s = game_.EnsureSurvival()) s->RefreshLooks();
        };
        if (armory_weapon_ == 6) {
            const pt::game::SurvivalTorchDef& td = pt::game::SurvivalTorch(armory_torch_);
            if (!prof.OwnsTorch(td.id)) {
                if (prof.credits < td.price) return;
                prof.credits -= td.price;
                prof.GrantTorch(td.id);
                pt::LogInfo("armory: bought torch {} (${}, remaining ${})", td.id, td.price, prof.credits);
            }
            prof.torch = td.id;
            persist();
            return;
        }
        const int w = std::clamp(armory_weapon_, 1, 5);
        if (armory_mode_ == 1) {
            const int choice = std::clamp(armory_anim_, 0, 4);
            if (choice > 0) {
                const int style = choice - 1;
                if (!prof.OwnsReload(style)) {
                    const int price = pt::game::SurvivalProfile::ReloadPrice(style);
                    if (prof.credits < price) return;
                    prof.credits -= price;
                    prof.GrantReload(style);
                    pt::LogInfo("armory: bought reload style {} (${}, remaining ${})", style, price, prof.credits);
                }
            }
            if (apply_all) {
                for (size_t i = 1; i <= 5; ++i) prof.weapon_reloads[i] = choice;
                pt::LogInfo("armory: applied reload choice {} to all 5 weapons", choice);
            } else {
                prof.weapon_reloads[static_cast<size_t>(w)] = choice;
                pt::LogInfo("armory: equipped reload choice {} on weapon {}", choice, w);
            }
            persist();
            ArmoryPreviewReload();
            return;
        }
        const pt::game::SurvivalSkinDef& sk = pt::game::SurvivalSkin(armory_skin_);
        if (apply_all) {
            if (!prof.OwnsSkin(sk.id)) return;
            for (size_t i = 1; i <= 5; ++i) prof.weapon_skins[i] = sk.id;
            persist();
            pt::LogInfo("armory: applied '{}' to all 5 weapons", sk.name);
            return;
        }
        if (!prof.OwnsSkin(sk.id)) {
            if (sk.price <= 0 || prof.credits < sk.price) return;
            prof.credits -= sk.price;
            prof.GrantSkin(sk.id);
            pt::LogInfo("armory: bought '{}' (${}, remaining ${})", sk.name, sk.price, prof.credits);
        }
        prof.weapon_skins[static_cast<size_t>(w)] = sk.id;
        persist();
        pt::LogInfo("armory: equipped '{}' on weapon {}", sk.name, w);
    }

    void ArmoryPreviewReload() override {
        if (armory_weapon_ == 6) return;
        const pt::game::ReloadStyle rs = ArmoryActiveReloadStyle();
        armory_reload_length_ = rs == pt::game::ReloadStyle::Telekinetic ? 2.1f : rs == pt::game::ReloadStyle::Gunslinger ? 1.8f : 1.5f;
        armory_reload_timer_ = armory_reload_length_;
    }

    void ArmoryOrbit(float dyaw, float dpitch) override {
        armory_angle_ += dyaw;
        armory_pitch_ = std::clamp(armory_pitch_ + dpitch, -0.85f, 0.85f);
        armory_drag_cooldown_ = 2.5f;
    }

    void BuildArmoryStudio(float dt, pt::Camera& out_cam, std::vector<pt::DrawItem>& out_draws, pt::SceneLighting& out_lighting) {
        out_draws.clear();
        const float step_dt = std::clamp(dt, 0.0f, 0.05f);
        armory_fx_time_ += step_dt;
        if (armory_drag_cooldown_ > 0.0f) {
            armory_drag_cooldown_ = std::max(0.0f, armory_drag_cooldown_ - step_dt);
        } else {
            armory_angle_ += step_dt * 0.45f;
        }
        if (armory_reload_timer_ > 0.0f) {
            armory_reload_timer_ = std::max(0.0f, armory_reload_timer_ - step_dt);
        }
        pt::game::SurvivalNight* survival = game_.EnsureSurvival();
        const pt::game::SurvivalModels* sm = survival ? survival->EnsureArmoryModels(armory_weapon_) : nullptr;
        if (!sm) return;

        out_cam = pt::Camera{};
        out_cam.position = glm::vec3(0.0f, 0.0f, 0.0f);
        out_cam.yaw = 0.0f;
        out_cam.pitch = 0.0f;
        out_cam.roll = 0.0f;
        out_cam.fov_y = glm::radians(38.0f);
        out_cam.near_plane = 0.02f;
        out_cam.far_plane = 20.0f;

        auto emit = [&](const pt::GpuMesh* mesh, const glm::mat4& xform) {
            if (!mesh) return;
            pt::DrawItem item;
            item.mesh = mesh;
            item.transform = xform;
            out_draws.push_back(item);
        };

        auto setup_studio_lights = [&](const glm::vec3& center, float radius) {
            out_lighting = pt::SceneLighting{};
            out_lighting.valid = true;
            const float reach = std::max(radius, 0.05f) * 3.8f;
            const glm::vec3 to_cam = -out_cam.Forward();
            struct Lamp {
                glm::vec3 dir;
                glm::vec3 color;
                float strength;
                float spec;
            };
            const std::array<Lamp, 4> lamps{{
                {to_cam * 0.42f - out_cam.Right() * 0.96f + out_cam.Up() * 1.15f, {1.00f, 0.98f, 0.95f}, 0.78f, 0.14f},
                {to_cam * 0.50f + out_cam.Right() * 1.05f + out_cam.Up() * 0.25f, {0.84f, 0.90f, 1.00f}, 0.44f, 0.10f},
                {-to_cam + out_cam.Up() * 0.85f + out_cam.Right() * 0.28f,        {0.94f, 0.96f, 1.00f}, 0.58f, 0.32f},
                {to_cam * 0.40f - out_cam.Up() * 0.95f,                           {0.86f, 0.88f, 0.94f}, 0.28f, 0.08f},
            }};
            for (size_t i = 0; i < lamps.size(); ++i) {
                pt::SceneLight light;
                light.id = 0x41524D4F52590000ull + i;
                light.name = "ArmoryStudio";
                light.position = center + glm::normalize(lamps[i].dir) * reach;
                light.intensity = lamps[i].color * (lamps[i].strength * reach * reach);
                light.inner_range = reach * 0.14f;
                light.outer_range = reach * 5.0f;
                light.source_radius = reach * 0.10f;
                light.specular_scale = lamps[i].spec;
                out_lighting.lights.push_back(light);
            }
            pt::SceneProbe amb;
            const float box = reach * 4.0f;
            amb.box_world = glm::scale(glm::translate(glm::mat4(1.0f), center), glm::vec3(box));
            amb.world_to_box = glm::inverse(amb.box_world);
            amb.weight = 1.0f;
            amb.sh[0] = glm::vec3(0.21f, 0.22f, 0.25f);
            amb.name = "ArmoryAmbient";
            out_lighting.probes.push_back(amb);

            out_lighting.exposure.pinned = true;
            out_lighting.exposure.pinned_ev = pt::game::kArchiveModelEv;
            out_lighting.exposure.compensation = 0.0f;
            for (float& c : out_lighting.exposure.add_comp) c = 0.0f;
            out_lighting.exposure.bloom_weight = 0.16f;
            out_lighting.exposure.bloom_extraction = 2.0f;
            out_lighting.screen.color_scale = glm::vec3(1.0f);
            out_lighting.screen.film_grain = false;
            out_lighting.screen.screen_distortion = false;
            out_lighting.screen.depth_of_field = false;
            out_lighting.screen.motion_blur = false;
            out_lighting.screen.local_reflections = false;
        };

        if (armory_weapon_ == 6) {
            if (!sm->chest_light) return;
            if (last_armory_torch_ != armory_torch_) {
                pt::game::ApplyTorchSkin(app_.textures, *sm, armory_torch_, 3.4f);
                last_armory_torch_ = armory_torch_;
                armory_preview_dirty_ = true;
            }
            const pt::game::SurvivalTorchDef& td = pt::game::SurvivalTorch(armory_torch_);
            const glm::vec3 bmin = sm->chest_light->bounds_min;
            const glm::vec3 bmax = sm->chest_light->bounds_max;
            const glm::vec3 mid = 0.5f * (bmin + bmax);
            const float radius = std::max(0.045f, 0.5f * glm::length(bmax - bmin));
            const float dist = radius * 4.35f;
            const glm::vec3 stage_center = out_cam.position + out_cam.Forward() * dist + out_cam.Right() * (dist * 0.22f) - out_cam.Up() * (radius * 0.04f);
            const float yaw = 2.55f + armory_angle_;
            const float pitch = -0.10f + armory_pitch_;
            const glm::mat4 world = glm::translate(glm::mat4(1.0f), stage_center)
                                  * glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0))
                                  * glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1, 0, 0))
                                  * glm::translate(glm::mat4(1.0f), -mid);
            emit(sm->chest_light, world);
            if (sm->lens_glow) {
                emit(sm->lens_glow.get(), world * glm::translate(glm::mat4(1.0f), sm->chest_lens + glm::vec3(0.0f, 0.0f, 0.0006f)));
            }
            setup_studio_lights(stage_center, radius);
            pt::SceneLight beam;
            beam.id = 0x41524D4F52590010ull;
            beam.name = "ArmoryTorchBeam";
            const glm::vec3 lens_world = glm::vec3(world * glm::vec4(sm->chest_lens, 1.0f));
            const glm::vec3 lens_fwd = glm::normalize(glm::mat3(world) * glm::vec3(0.0f, 0.0f, 1.0f));
            beam.position = lens_world + lens_fwd * (radius * 0.55f);
            beam.intensity = td.tint * (4.2f * radius * radius);
            beam.inner_range = radius * 0.08f;
            beam.outer_range = radius * 14.0f;
            beam.source_radius = radius * 0.30f;
            beam.specular_scale = 1.4f;
            out_lighting.lights.push_back(beam);
            return;
        }

        const int w = std::clamp(armory_weapon_, 1, 5);
        if (!sm->guns[static_cast<size_t>(w)].body) return;
        const pt::game::SurvivalGunModel& gun = sm->guns[static_cast<size_t>(w)];
        const pt::game::SurvivalProfile& prof = game_.SurvivalProfileData();
        int shown_skin = armory_skin_;
        if (armory_mode_ == 1) {
            shown_skin = prof.EquippedSkin(w);
        } else {
            const std::vector<int> list = ArmoryFilteredSkins();
            if (!list.empty() && std::find(list.begin(), list.end(), armory_skin_) == list.end()) {
                armory_skin_ = list.front();
            }
            shown_skin = armory_skin_;
        }
        const pt::game::SurvivalSkinDef& sk = pt::game::SurvivalSkin(shown_skin);
        if (sk.rarity == pt::game::SkinRarity::Mythic) {
            pt::game::ApplyWeaponSkinAnimated(app_.textures, gun, shown_skin, armory_fx_time_);
            last_armory_weapon_ = w;
            last_armory_skin_ = shown_skin;
            armory_preview_dirty_ = true;
        } else if (last_armory_weapon_ != w || last_armory_skin_ != shown_skin) {
            pt::game::ApplyWeaponSkin(app_.textures, gun, shown_skin);
            last_armory_weapon_ = w;
            last_armory_skin_ = shown_skin;
            armory_preview_dirty_ = true;
        }

        glm::vec3 bmin = gun.body->bounds_min;
        glm::vec3 bmax = gun.body->bounds_max;
        if (gun.moving) {
            bmin = glm::min(bmin, gun.moving->bounds_min);
            bmax = glm::max(bmax, gun.moving->bounds_max);
        }
        const glm::vec3 mid = 0.5f * (bmin + bmax);
        const float radius = std::max(0.10f, 0.5f * glm::length(bmax - bmin));

        const float dist = radius * 2.85f;
        const glm::vec3 stage_center = out_cam.position + out_cam.Forward() * dist + out_cam.Right() * (dist * 0.22f) - out_cam.Up() * (radius * 0.02f);

        constexpr float kPi = 3.14159265358979323846f;
        auto smooth = [](float x) {
            x = std::clamp(x, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        };
        const pt::game::ReloadStyle rs = ArmoryActiveReloadStyle();
        glm::vec3 extra_pos(0.0f);
        glm::mat4 extra_rot(1.0f);
        float mag_drop = 0.0f;
        float slide_back = 0.0f;
        float cyl_open = 0.0f;
        float barrel_break = 0.0f;
        float tele_glow = 0.0f;
        if (armory_reload_timer_ > 0.0f && armory_reload_length_ > 0.05f) {
            const float p = std::clamp(1.0f - armory_reload_timer_ / armory_reload_length_, 0.0f, 1.0f);
            mag_drop = p < 0.50f ? smooth(p / 0.25f) : smooth((0.82f - p) / 0.30f);
            cyl_open = smooth(p / 0.18f) * smooth((0.86f - p) / 0.16f);
            barrel_break = cyl_open;
            if (p > 0.72f && p < 0.94f) slide_back = std::sin(((p - 0.72f) / 0.22f) * kPi);
            if (rs == pt::game::ReloadStyle::Tactical) {
                const float env = smooth(p / 0.15f) * smooth((1.0f - p) / 0.18f);
                const float snap = p > 0.76f && p < 0.96f ? std::sin(((p - 0.76f) / 0.20f) * kPi) : 0.0f;
                extra_rot = glm::rotate(glm::mat4(1.0f), 0.55f * env, glm::vec3(0, 0, 1))
                          * glm::rotate(glm::mat4(1.0f), -0.22f * snap, glm::vec3(1, 0, 0));
            } else if (rs == pt::game::ReloadStyle::Gunslinger) {
                const float su = smooth(std::clamp((p - 0.54f) / 0.42f, 0.0f, 1.0f));
                const float lift = std::sin(su * kPi);
                extra_pos.y += radius * 0.24f * lift;
                extra_rot = glm::rotate(glm::mat4(1.0f), su * 2.0f * kPi, glm::vec3(1, 0, 0))
                          * glm::rotate(glm::mat4(1.0f), 0.24f * lift, glm::vec3(0, 0, 1));
            } else if (rs == pt::game::ReloadStyle::Telekinetic) {
                const float hover = smooth(p / 0.16f) * smooth((1.0f - p) / 0.16f);
                tele_glow = hover;
                extra_pos.y += radius * (0.22f + 0.06f * std::sin(p * 4.0f * kPi)) * hover;
                extra_rot = glm::rotate(glm::mat4(1.0f), 0.65f * std::sin(p * kPi) * hover, glm::vec3(0, 1, 0))
                          * glm::rotate(glm::mat4(1.0f), 0.34f * std::sin(p * 2.0f * kPi) * hover, glm::vec3(0, 0, 1))
                          * glm::rotate(glm::mat4(1.0f), -0.18f * std::cos(p * 2.0f * kPi) * hover, glm::vec3(1, 0, 0));
            }
        }
        const float yaw = kPi * 0.5f + armory_angle_;
        const float pitch = -0.10f + armory_pitch_ + (armory_drag_cooldown_ <= 0.0f ? 0.04f * std::sin(armory_angle_ * 0.7f) : 0.0f);
        const glm::mat4 world = glm::translate(glm::mat4(1.0f), stage_center + extra_pos)
                              * glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0))
                              * glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1, 0, 0))
                              * extra_rot
                              * glm::translate(glm::mat4(1.0f), -mid);

        emit(gun.body, world);
        if (gun.moving) {
            glm::mat4 m_local(1.0f);
            if (w == 1) {
                m_local = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -gun.slide_travel * slide_back));
            } else if (w == 2) {
                m_local = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -gun.pump_travel * slide_back));
            } else if (w == 3) {
                m_local = glm::translate(glm::mat4(1.0f), gun.cylinder_swing * cyl_open);
            } else if (w == 4) {
                m_local = glm::translate(glm::mat4(1.0f), gun.lever_pivot)
                        * glm::rotate(glm::mat4(1.0f), 0.75f * slide_back, glm::vec3(1, 0, 0))
                        * glm::translate(glm::mat4(1.0f), -gun.lever_pivot);
            } else if (w == 5) {
                m_local = glm::translate(glm::mat4(1.0f), gun.hinge)
                        * glm::rotate(glm::mat4(1.0f), 0.55f * barrel_break, glm::vec3(1, 0, 0))
                        * glm::translate(glm::mat4(1.0f), -gun.hinge);
            }
            emit(gun.moving, world * m_local);
        }
        if (gun.hammer) emit(gun.hammer, world);
        if (gun.magazine) {
            const glm::mat4 mag_local = glm::translate(glm::mat4(1.0f), gun.magazine_drop * (0.14f * mag_drop));
            emit(gun.magazine, world * mag_local);
        }

        if (sk.rarity == pt::game::SkinRarity::Mythic) {
            pt::game::EmitMythicWeaponVfx(app_.textures, *sm, gun, world, shown_skin, armory_fx_time_, 0, false, false, 4.2f, out_draws);
        }

        setup_studio_lights(stage_center, radius);
        if (sk.rarity == pt::game::SkinRarity::Mythic || tele_glow > 0.01f) {
            pt::SceneLight aura;
            aura.id = 0x41524D4F52590009ull;
            aura.name = "ArmoryAura";
            aura.position = stage_center - out_cam.Up() * (radius * 0.45f) - out_cam.Forward() * (radius * 0.75f);
            const float pulse = 0.75f + 0.25f * std::sin(armory_fx_time_ * (sk.id == 54 ? 17.0f : 3.6f));
            const float gain = ((sk.rarity == pt::game::SkinRarity::Mythic ? 1.15f * pulse : 0.0f) + 2.2f * tele_glow) * radius * radius;
            const glm::vec3 aura_col = tele_glow > 0.01f && sk.rarity != pt::game::SkinRarity::Mythic
                                           ? glm::vec3(0.96f, 0.22f, 0.32f)
                                           : pt::game::MythicLiveColor(shown_skin, armory_fx_time_);
            aura.intensity = aura_col * gain;
            aura.inner_range = radius * 0.1f;
            aura.outer_range = radius * 7.0f;
            aura.source_radius = radius * 0.25f;
            aura.specular_scale = 1.25f;
            out_lighting.lights.push_back(aura);
        }
    }

private:

    pt::game::PcScreen Screen() const override {
        pt::game::PcScreen screen;
        if (!ScreenShown()) return screen;
        const bool calibrate = hdr_screen_ == HdrScreen::Calibrate;
        const int count = calibrate ? static_cast<int>(std::size(kHdrCalibrateSteps)) : static_cast<int>(std::size(kHdrPreviewRows));
        const int step = std::clamp(hdr_step_, 0, count - 1);
        const pt::game::PcSettingRow row = HdrRow(calibrate ? kHdrCalibrateSteps[step] : kHdrPreviewRows[step]);
        const char arg = pt::game::kPcNoteArgument;
        screen.header = std::format("{}{}{}{}{}", calibrate ? "pc_hdr_calibrate_step" : "pc_hdr_preview_step", arg, step + 1, arg, count);
        screen.label = row.label;
        screen.value = row.value >= 0 && row.value < static_cast<int>(row.values.size()) ? row.values[row.value] : std::string();
        screen.note = calibrate ? kHdrCalibrateNotes[step] : row.note;
        screen.hint = !calibrate ? "pc_hdr_preview_hint" : step + 1 == count ? "pc_hdr_calibrate_hint_last" : "pc_hdr_calibrate_hint";
        screen.scene = !calibrate;
        screen.pattern = calibrate ? kHdrCalibratePatterns[step] : 0;
        screen.can_left = HdrStepTo(row, -1) >= 0;
        screen.can_right = HdrStepTo(row, 1) >= 0;
        return screen;
    }

    static int HdrStepTo(const pt::game::PcSettingRow& row, int delta) {
        int v = row.value + delta;
        while (v >= 0 && v < static_cast<int>(row.values.size()) && row.ValueDisabled(v)) v += delta;
        return v >= 0 && v < static_cast<int>(row.values.size()) ? v : -1;
    }

    void ScreenInput(int dx, int dy, bool accept, bool back) override {
        if (!ScreenShown()) return;
        const bool calibrate = hdr_screen_ == HdrScreen::Calibrate;
        const int count = calibrate ? static_cast<int>(std::size(kHdrCalibrateSteps)) : static_cast<int>(std::size(kHdrPreviewRows));
        hdr_step_ = std::clamp(hdr_step_, 0, count - 1);
        if (dx != 0) {
            const pt::game::PcSettingRow row = HdrRow(calibrate ? kHdrCalibrateSteps[hdr_step_] : kHdrPreviewRows[hdr_step_]);
            if (const int v = HdrStepTo(row, dx); v >= 0) Set(row.id, v);
            return;
        }
        bool leave = false;
        if (calibrate) {
            if (accept) leave = ++hdr_step_ >= count;
            else if (back) leave = --hdr_step_ < 0;
        } else if (dy != 0) {
            hdr_step_ = (hdr_step_ + dy + count) % count;
        } else {
            leave = accept || back;
        }
        if (leave) {
            pt::LogInfo("hdr: {} closed", calibrate ? "calibration" : "look preview");
            hdr_screen_ = HdrScreen::None;
            hdr_step_ = 0;
        }
    }

    static std::vector<int> HdrPaperWhiteSteps(int current) {
        std::vector<int> steps;
        for (int nits = 80; nits <= 500; nits += 10) steps.push_back(nits);
        if (std::find(steps.begin(), steps.end(), current) == steps.end()) steps.push_back(current);
        return steps;
    }
    static std::vector<int> HdrUiWhiteSteps(int current) {
        std::vector<int> steps = {0};
        for (int nits = 80; nits <= 500; nits += 10) steps.push_back(nits);
        if (std::find(steps.begin(), steps.end(), current) == steps.end()) steps.push_back(current);
        return steps;
    }
    static std::vector<int> HdrPeakSteps(int current) {
        std::vector<int> steps = {0, 400, 500, 600, 700, 800, 900, 1000, 1200, 1400, 1600, 1800, 2000, 2500, 3000, 3500, 4000};
        if (std::find(steps.begin(), steps.end(), current) == steps.end()) steps.push_back(current);
        return steps;
    }

    std::vector<pt::game::PcSettingSection> SpeedrunResults() const {
        const pt::game::SpeedrunTimer& run = game_.Speedrun();
        using pt::game::SpeedrunTimer;
        const int language = std::clamp(game_.Options().subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1);
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

    static const char* PresetNote(pt::GraphicsPreset preset) {
        switch (preset) {
            case pt::GraphicsPreset::Low: return "pc_note_preset_low";
            case pt::GraphicsPreset::Original: return "pc_note_preset_original";
            case pt::GraphicsPreset::High: return "pc_note_preset_high";
            case pt::GraphicsPreset::Ultra: return "pc_note_preset_ultra";
            default: return "pc_note_preset_custom";
        }
    }

    std::vector<pt::game::PcSettingSection> GraphicsSections() const {
        const auto& s=app_.settings;
        std::string reason;
        const bool supported=app_.scene.RayTracingSupported(reason);
        pt::game::PcSettingSection quality{"pc_graphics_quality",0,{}};
        auto preset=Row(kGraphicsPreset,"pc_graphics_preset",{"pc_preset_low","pc_preset_original","pc_preset_high","pc_preset_ultra","pc_preset_custom"},
                        static_cast<int>(pt::DetectGraphicsPreset(s,supported)),PresetNote(pt::DetectGraphicsPreset(s,supported)));
        preset.wrap=false;quality.rows.push_back(std::move(preset));
        auto lighting = LightingSections();
        auto textures = TextureSections();
        auto effects = EffectSections();
        effects[1].column = 0;
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
        const bool path=supported&&g.ray_quality>=4;
        auto note=[&](bool traced,const char* base){return path?"pc_note_in_path_tracing":traced&&!ready?"pc_note_restart":base;};
        pt::game::PcSettingSection lighting{"pc_graphics_lighting",0,{}};
        std::vector<std::string> shadow_values{"pc_off","pc_preset_low","pc_preset_medium","pc_preset_original"};
        if(supported){shadow_values.push_back("pc_rt_sharp_long");shadow_values.push_back("pc_rt_soft_long");}
        auto shadows=Row(kShadowQuality,"pc_shadows",std::move(shadow_values),rt_shadows>0?3+rt_shadows:g.shadow_quality,note(rt_shadows>0,"pc_note_shadows"));
        shadows.wrap=false;shadows.enabled=!path;lighting.rows.push_back(std::move(shadows));
        auto contact=RtSwitchRow(kRtContact,"pc_rt_contact",s.ray_tracing.contact_shadows,"pc_note_rt_contact");
        if(path){contact.note="pc_note_in_path_tracing";contact.enabled=false;}
        lighting.rows.push_back(std::move(contact));
        std::vector<std::string> traced{"pc_off","pc_standard"};
        if(supported)traced.push_back("pc_ray_traced");
        auto ao=Row(kRasterAo,"pc_rt_ao",traced,rt_ao?2:g.ambient_occlusion?1:0,note(rt_ao,"pc_note_ao"));
        ao.wrap=false;ao.enabled=!path;lighting.rows.push_back(std::move(ao));
        auto reflections=Row(kRasterReflections,"pc_reflections",std::move(traced),rt_reflections?2:g.reflections?1:0,note(rt_reflections,"pc_note_reflections"));
        reflections.wrap=false;reflections.enabled=!path;lighting.rows.push_back(std::move(reflections));
        if(path) {
            const auto percent_row=[&](int id,const char* label,int percent,int last,const char* row_note){
                std::vector<std::string> values;
                for(int v=0;v<=last;v+=5) values.push_back(std::format("{}%",v));
                auto row=Row(id,label,std::move(values),std::clamp(percent,0,last)/5,row_note);
                row.wrap=false;
                return row;
            };
            lighting.rows.push_back(percent_row(kPtFloorReflection,"pc_pt_floor",g.pt_floor_reflection,200,"pc_note_pt_floor"));
            lighting.rows.push_back(percent_row(kPtGlassReflection,"pc_pt_glass",g.pt_glass_reflection,200,"pc_note_pt_glass"));
            lighting.rows.push_back(percent_row(kPtReflectionSoftness,"pc_pt_softness",g.pt_reflection_softness,100,"pc_note_pt_softness"));
        }
        std::vector<std::string> ray_values{"pc_ray_balanced","pc_preset_high","pc_preset_ultra"};
        if(supported) ray_values.push_back("pc_ray_path_traced");
        const int ray_index=g.ray_quality>=4?3:g.ray_quality>=2?2:std::max(g.ray_quality,0);
        auto quality=Row(kRayQuality,"pc_ray_quality",std::move(ray_values),supported?ray_index:std::min(ray_index,2),"pc_note_ray_quality");
        quality.enabled=supported&&(rt_shadows>0||rt_ao||rt_reflections||g.ray_quality>=4);lighting.rows.push_back(std::move(quality));
        if(supported&&(g.ray_quality>=3||rt_shadows>0||rt_ao||rt_reflections||s.ray_tracing.contact_shadows)){
            const auto& u=app_.scene.upscale;
            std::string rr_reason;
            const bool fsr=u.kind==pt::UpscalerKind::Fsr4;
            const bool rr=app_.scene.RayReconstructionAvailable(rr_reason);
            auto reconstruction=Row(kRayReconstruction,fsr?"pc_fsr_ray_regeneration":"pc_ray_reconstruction",OffOn(),rr&&u.ray_reconstruction?1:0,
                                    fsr?"pc_note_fsr_rr":rr?"pc_note_ray_reconstruction":"pc_note_rr_unavailable");
            reconstruction.enabled=rr;lighting.rows.push_back(std::move(reconstruction));
        }
        return {std::move(lighting)};
    }

    std::vector<pt::game::PcSettingSection> TextureSections() const {
        const auto& g=app_.settings.graphics;
        pt::game::PcSettingSection textures{"pc_section_textures",0,{}};
        auto anisotropy=Row(kAnisotropy,"pc_anisotropy",{"pc_off","2x","4x","8x","16x"},g.anisotropy>=16?4:g.anisotropy>=8?3:g.anisotropy>=4?2:g.anisotropy>=2?1:0,"pc_note_anisotropy");
        anisotropy.wrap=false;textures.rows.push_back(std::move(anisotropy));
        textures.rows.push_back(Row(kTextureDetail,"pc_texture_detail",{"pc_detail_reduced","pc_preset_original","pc_detail_fine"},g.texture_detail,"pc_note_texture_detail"));
        return {std::move(textures)};
    }

    std::vector<pt::game::PcSettingSection> ModSections() const {
        pt::game::PcSettingSection left{"pc_section_mods", 0, {}};
        if (!app_.mods) return {std::move(left)};
        const int language = std::clamp(game_.Options().subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1);
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
            left.rows.push_back(Row(kModFirst + static_cast<int>(i), label, OffOn(), on ? 1 : 0, note));
        }
        return {std::move(left)};
    }

    std::vector<pt::game::PcSettingSection> EffectSections() const {
        const auto& g=app_.settings.graphics;
        pt::game::PcSettingSection effects{"pc_graphics_effects",0,{}};
        effects.rows.push_back(Row(kDepthOfField,"pc_dof",OffOn(),g.depth_of_field?1:0,"pc_note_dof"));
        effects.rows.push_back(Row(kMotionBlur,"pc_motion_blur",{"pc_motion_blur_all","pc_motion_blur_object","pc_off"},
                                   !g.motion_blur?2:g.camera_motion_blur?0:1,"pc_note_motion_blur_modes"));
        {
            std::vector<std::string> mb_vals;
            for (int pct = 10; pct <= 100; pct += 10) mb_vals.push_back(std::format("{}%", pct));
            auto row = Row(kMotionBlurStrength, "pc_motion_blur_strength", std::move(mb_vals),
                           std::clamp((g.motion_blur_strength - 10) / 10, 0, 9), "pc_note_motion_blur_strength");
            row.wrap = false;
            row.enabled = g.motion_blur;
            effects.rows.push_back(std::move(row));
        }
        const auto strength=[&](int id,const char* label,int percent,bool on,const char* note){
            auto row=Row(id,label,Percents(),on?percent/5:0,note);
            row.wrap=false;
            return row;
        };
        effects.rows.push_back(strength(kBloom,"pc_bloom",g.bloom_strength,g.bloom,"pc_note_bloom_strength"));
        effects.rows.push_back(strength(kFlareStrength,"pc_lens_flare",g.flare_strength,true,"pc_note_lens_flare"));
        effects.rows.push_back(strength(kLensGhosts,"pc_lens_ghosts",g.ghost_strength,g.lens_ghosts,"pc_note_ghost_strength"));
        pt::game::PcSettingSection clarity{"pc_graphics_image",1,{}};
        clarity.rows.push_back(Row(kFilmGrain,"pc_film_grain",{"pc_off","25%","50%","75%","100%"},static_cast<int>(g.film_grain*4+.5f),"pc_note_film_grain"));
        clarity.rows.push_back(Row(kFilmGrainType,"pc_film_grain_type",{"pc_film_grain_original","pc_film_grain_fine"},std::clamp(g.film_grain_type,0,1),"pc_note_film_grain_type"));
        clarity.rows.push_back(Row(kLensDistortion,"pc_lens_distortion",OffOn(),g.lens_distortion?1:0,"pc_note_lens_distortion"));
        clarity.rows.push_back(Row(kClarity,"pc_clarity",Numbers(0,10),static_cast<int>(g.clarity*10+.5f),"pc_note_clarity"));
        clarity.rows.push_back(Row(kLetterbox,"pc_photo_letterbox",{"pc_off","2.39:1","1.85:1"},std::clamp(app_.settings.display.letterbox,0,2),"pc_note_letterbox"));
        return {std::move(effects),std::move(clarity)};
    }

    static std::vector<std::string> Percents() {
        std::vector<std::string> values{"pc_off"};
        for(int i=5;i<=200;i+=5) values.push_back(std::format("{}%",i));
        return values;
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
            text[0] = static_cast<char>(pt::AsciiUpper(text[0]));
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
        case pt::UpscalerKind::MetalFx: return "MetalFX";
        default: return pt::UpscalerName(kind);
        }
    }

    static std::vector<pt::FrameGenKind> FrameGenerators() {
        std::vector<pt::FrameGenKind> kinds;
        for (const pt::FrameGenKind kind : {pt::FrameGenKind::Off, pt::FrameGenKind::Fsr, pt::FrameGenKind::Dlss, pt::FrameGenKind::Metalfx,
                                            pt::FrameGenKind::Xess}) {
            if (pt::FrameGenOnPlatform(kind)) kinds.push_back(kind);
        }
        return kinds;
    }
    static std::string FrameGenName(pt::FrameGenKind kind) {
        switch (kind) {
        case pt::FrameGenKind::Fsr: return "FSR 3";
        case pt::FrameGenKind::Dlss: return "DLSS";
        case pt::FrameGenKind::Metalfx: return "MetalFX";
        case pt::FrameGenKind::Xess: return "XeSS";
        default: return "pc_off";
        }
    }

    std::vector<pt::UpscalerKind> Upscalers(std::vector<std::string>* notes) const {
        std::vector<pt::UpscalerKind> kinds{pt::UpscalerKind::Off};
        if (notes) notes->assign(1, std::string());
        for (const pt::UpscalerKind kind : {pt::UpscalerKind::Fsr, pt::UpscalerKind::Fsr4, pt::UpscalerKind::Dlss, pt::UpscalerKind::Xess,
                                            pt::UpscalerKind::MetalFx}) {
            if (!pt::UpscalerOnPlatform(kind)) continue;
            std::string reason;
            const bool available = app_.scene.UpscalerAvailable(kind, reason);
            kinds.push_back(kind);
            if (!notes) continue;
            if (available) {
                notes->emplace_back();
            } else if (const char* note = kind == pt::UpscalerKind::Fsr4 ? pt::FsrReasonNote(reason) : nullptr) {
                notes->emplace_back(note);
            } else if (reason.starts_with(ShortName(kind) + " ")) {
                notes->push_back(Sentence(reason));
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

    static std::string ShortDeviceName(const std::string& name) {
        size_t points = 0;
        size_t cut = std::string::npos;
        for (size_t i = 0; i < name.size(); ++i) {
            if ((static_cast<unsigned char>(name[i]) & 0xC0) == 0x80) continue;
            if (points == 22) cut = i;
            ++points;
        }
        return points > 24 ? name.substr(0, cut) + "..." : name;
    }

    static std::vector<std::string> OutputDevices() {
        std::vector<std::string> names;
        if (pt::devices::AudioStarting() || !SDL_WasInit(SDL_INIT_AUDIO)) return names;
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
        for (int i = 0; devices && i < count; ++i) {
            if (const char* name = SDL_GetAudioDeviceName(devices[i])) names.emplace_back(name);
        }
        SDL_free(devices);
        return names;
    }

    static std::vector<std::string> Microphones() {
        std::vector<std::string> names;
        int count = 0;
        SDL_AudioDeviceID* devices = !pt::devices::AudioStarting() && SDL_WasInit(SDL_INIT_AUDIO) ? SDL_GetAudioRecordingDevices(&count) : nullptr;
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

    std::vector<double> RefreshRates() const {
        if (!app_.window) return {};
        int count = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(app_.window), &count);
        std::vector<pt::DisplayModeInfo> infos;
        for (int i = 0; modes && i < count; ++i) infos.push_back({modes[i]->w, modes[i]->h, static_cast<double>(modes[i]->refresh_rate)});
        SDL_free(modes);
        return pt::RefreshRatesFor(infos, {app_.settings.display.width, app_.settings.display.height});
    }

    std::vector<glm::ivec2> WindowSizes() const {
        if (WindowMode(app_) == pt::display::kExclusive && app_.window) {
            int count = 0;
            SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(app_.window), &count);
            std::vector<glm::ivec2> listed;
            listed.reserve(std::max(count, 0));
            for (int i = 0; modes && i < count; ++i) listed.emplace_back(modes[i]->w, modes[i]->h);
            SDL_free(modes);
            std::vector<glm::ivec2> sizes = pt::UniqueDisplaySizes(listed);
            const glm::ivec2 current(app_.settings.display.width, app_.settings.display.height);
            if (!sizes.empty() && std::find(sizes.begin(), sizes.end(), current) == sizes.end()) {
                sizes.push_back(pt::ClosestDisplaySize(sizes, current));
                sizes = pt::UniqueDisplaySizes(sizes);
            }
            if (!sizes.empty()) return sizes;
        }
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

struct FocusTrace {
    std::string last_event = "none";
    uint64_t events = 0;
    uint64_t frame = 0;
    float zoom = 1.0f;
    bool peephole = false;
    bool right_down = false;
    unsigned input_logs = 0;
};
FocusTrace g_focus_trace;

std::string DescribeForeground(const pt::os::ForegroundWindow& fg) {
    if (!fg.handle) return "no foreground window";
    return std::format("foreground window 0x{:x} \"{}\" class {} of {} (pid {}{})", fg.handle, fg.title, fg.window_class,
                       fg.process.empty() ? "?" : fg.process, fg.pid, fg.pid == pt::os::ProcessId() ? ", this game" : "");
}

void* WindowHandle(SDL_Window* window) {
    return window ? SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) : nullptr;
}

std::string DescribeWindowInput(SDL_Window* window) {
    if (!window) {
        return std::format("frame {}, zoom {:.3f}, peephole {}, no window", g_focus_trace.frame, g_focus_trace.zoom, g_focus_trace.peephole);
    }
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    return std::format("frame {}, zoom {:.3f}, peephole {}, right event {}, buttons {:#x}, flags {:#x}, relative {}, "
                       "keyboard focus {}, mouse focus {}, pixels {}x{}",
                       g_focus_trace.frame, g_focus_trace.zoom, g_focus_trace.peephole, g_focus_trace.right_down,
                       SDL_GetMouseState(nullptr, nullptr), SDL_GetWindowFlags(window), SDL_GetWindowRelativeMouseMode(window),
                       SDL_GetKeyboardFocus() == window, SDL_GetMouseFocus() == window, width, height);
}

void LogWindowInput(SDL_Window* window, std::string_view reason) {
    if (g_focus_trace.input_logs < 128) {
        ++g_focus_trace.input_logs;
        pt::LogInfo("window input: {}; {}", reason, DescribeWindowInput(window));
    } else if (g_focus_trace.input_logs == 128) {
        ++g_focus_trace.input_logs;
        pt::LogInfo("window input: event detail limit reached; focus losses still include the current state");
    }
}

bool WindowActive(SDL_Window* window) {
    if (!window) return false;
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) return true;
    return pt::os::IsForegroundWindow(WindowHandle(window)) || pt::os::ForegroundIsOwnProcess();
}

bool PumpEvents(App& app, pt::InputDevice* input, bool& running, pt::KeyPressLatch* voice_key_latch = nullptr, uint32_t voice_key = SDL_SCANCODE_UNKNOWN) {
    if (!app.window) {
        return true;
    }
    if (pt::devices::TakeGamepadsReady()) {
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
        if (input) input->Init();
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
            event.button.windowID == SDL_GetWindowID(app.window) && event.button.button == SDL_BUTTON_RIGHT) {
            g_focus_trace.right_down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            LogWindowInput(app.window, g_focus_trace.right_down ? "right button down" : "right button up");
        }
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (input) {
            input->ProcessEvent(event);
        }
        if (voice_key_latch) {
            voice_key_latch->ProcessEvent(event, voice_key);
        }
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (running) {
                pt::LogInfo("window: {}", event.type == SDL_EVENT_QUIT ? "quit requested (window closed, Alt+F4 or the system)" : "close requested");
            }
            running = false;
        } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT)) {
            app.settings.display.fullscreen = WindowMode(app) ? 0 : 1;
            app.display_mode_chosen = true;
            ApplyFullscreen(app);
            if (!app.settings_path.empty()) {
                pt::SaveAppSettings(app.settings_path, app.settings);
            }
        } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            LogWindowInput(app.window, std::format("pixel size event from SDL window {}, game window {}, requested {}x{}",
                                                  event.window.windowID, SDL_GetWindowID(app.window), event.window.data1, event.window.data2));
            app.renderer.Resize(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2));
        } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                   event.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED) {
            LogWindowInput(app.window, std::format("display/HDR event {:#x} from SDL window {}, game window {}, data {} {}, game display {}",
                                                  event.type, event.window.windowID, SDL_GetWindowID(app.window), event.window.data1,
                                                  event.window.data2, SDL_GetDisplayForWindow(app.window)));
        } else if (event.type >= SDL_EVENT_DISPLAY_FIRST && event.type <= SDL_EVENT_DISPLAY_LAST) {
            LogWindowInput(app.window, std::format("display event {:#x} from SDL display {}, data {} {}, game window {}, game display {}",
                                                  event.type, event.display.displayID, event.display.data1, event.display.data2,
                                                  SDL_GetWindowID(app.window), SDL_GetDisplayForWindow(app.window)));
        }
        const char* focus_event = nullptr;
        switch (event.type) {
        case SDL_EVENT_WINDOW_FOCUS_LOST: focus_event = "FOCUS_LOST"; break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: focus_event = "FOCUS_GAINED"; break;
        case SDL_EVENT_WINDOW_MINIMIZED: focus_event = "MINIMIZED"; break;
        case SDL_EVENT_WINDOW_RESTORED: focus_event = "RESTORED"; break;
        case SDL_EVENT_WINDOW_HIDDEN: focus_event = "HIDDEN"; break;
        case SDL_EVENT_WINDOW_SHOWN: focus_event = "SHOWN"; break;
        case SDL_EVENT_WINDOW_OCCLUDED: focus_event = "OCCLUDED"; break;
        case SDL_EVENT_WINDOW_EXPOSED: focus_event = "EXPOSED"; break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: focus_event = "ENTER_FULLSCREEN"; break;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: focus_event = "LEAVE_FULLSCREEN"; break;
        default: break;
        }
        if (focus_event) {
            const bool main_window = event.window.windowID == SDL_GetWindowID(app.window);
            g_focus_trace.last_event =
                std::format("{} of {} (SDL window {})", focus_event, main_window ? "the game window" : "another window", event.window.windowID);
            ++g_focus_trace.events;
            const bool loss = event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_WINDOW_HIDDEN;
            if (loss) {
                pt::LogInfo("focus: SDL {}; {}; {}", g_focus_trace.last_event, DescribeForeground(pt::os::GetForegroundWindowInfo()),
                            DescribeWindowInput(app.window));
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_RESTORED) {
                pt::LogInfo("focus: SDL {}; {}", g_focus_trace.last_event, DescribeWindowInput(app.window));
            } else {
                pt::LogDebug("focus: SDL {}", g_focus_trace.last_event);
            }
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
            app.renderer.ImGuiNewFrame();
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
    app.renderer.Gpu().WaitIdle();
    app.scene.Destroy(*box);
    return 0;
}

int RunGame(App& app, pt::Vfs& vfs) {
    const Options& options = app.options;
    pt::game::Game game(vfs, *app.models);
    pt::game::GameConfig config;
    config.use_save = !options.no_save && !options.headless;
    config.save_path = UserDataDir() / "PT_Save_Data.sav";
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
    pt::boot::Phase("game data");
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
        game.SetNightUnlocked(progress.night_unlocked);
        unlocks_seen = game.BrowseUnlockGeneration();
        std::set<std::string> archive_keys;
        for (size_t at = 0; at < progress.archive.size();) {
            const size_t comma = std::min(progress.archive.find(',', at), progress.archive.size());
            if (comma > at) archive_keys.insert(progress.archive.substr(at, comma - at));
            at = comma + 1;
        }
        game.SetArchiveSeen(std::move(archive_keys));
    }
    bool save_progress_read = false;
    uint32_t archive_seen = game.ArchiveGeneration();
    if (app.mods) {
        game.LoadModScripts(*app.mods);
    }
    game.GetPlayer().camera_roll = app.settings.camera.roll;
    game.SetThirdPerson(app.settings.camera.third_person || options.third_person);
    game.SetGamePlusSetting(app.settings.game.game_plus, true);
    game.SetSurvivalBest(app.settings.game.survival_best_wave);
    game.SetSurvivalProfile(app.settings.game.survival_profile);
    game.SetSurvivalDifficulty(app.settings.game.survival_difficulty);
    ApplyCrosshair(game, app.settings);
    game.SetSoundCaptions(app.settings.accessibility.sound_captions);
    game.SetFovOffset(app.settings.accessibility.fov_offset);
    game.SetHouseSprint(app.settings.accessibility.house_sprint);
    pt::game::GameSound sound(game);
    std::vector<float> captured;
    pt::audio::AudioOutput::SetPreferSpatial(app.settings.audio.spatial);
    pt::audio::AudioOutput::SetPreferredDevice(app.settings.audio.output_device);
    const bool audio_device_now = app.window && pt::devices::WaitAudio(0.25);
    bool audio_device_pending = app.window && !audio_device_now;
    if (audio_device_pending) pt::LogWarn("audio: the audio devices are still starting; the sound opens its device when they are ready");
    if ((app.window || options.audio_offline) && sound.Init(audio_device_now, "Eng", app.settings.audio.surround || app.settings.audio.spatial)) {
        sound.System().SetMasterVolume(app.settings.audio.volume);
        if (options.seed) {
            sound.System().SetRandomSeed(options.seed);
        }
        game.SetAudio(&sound);
        game.Stages().ForEachStage([&](pt::game::Stage& stage) { sound.OnStageLoaded(stage); });
    }
    if (!sound.Ready()) audio_device_pending = false;
    pt::boot::Phase("sound");
    game.Demos().time_scale = options.demo_rate;
    pt::game::GameUi ui;
    bool hide_game_ui = false;
    pt::game::PhotoPanelView photo_view;
    pt::LogInfo("startup: game UI");
    const bool ui_ready = ui.Init(app.renderer, app.textures, vfs);
    ui.SetSurvivalVisionDirectory(pt::os::PathToUtf8(SurvivalVisionDirectory()));
    static constexpr const char* kDiscordClientId = "1559538474065993750";
    const char* discord_id_env = std::getenv("PT_DISCORD_CLIENT_ID");
    const std::string discord_id = discord_id_env && *discord_id_env ? discord_id_env : kDiscordClientId;
    const bool presence_on = app.window && !options.vr && options.input_script.empty() && app.settings.network.discord_presence && !discord_id.empty();
    const int64_t presence_start = static_cast<int64_t>(std::time(nullptr));
    if (presence_on) pt::PresenceStart(discord_id);
    float boot_splash_seconds = 0.0f;
    uint32_t boot_splash_texture = pt::TextureManager::kWhite;
    const bool shader_screen_wanted = ui_ready && app.window && options.input_script.empty() && app.renderer.Context().pipeline_cache_cold;
    if (ui_ready && app.window && options.input_script.empty()) {
        const uint32_t splash = app.textures.LoadImageFile(options.game_dir / "sce_sys" / "pic1.png", "boot_splash");
        if (splash != pt::TextureManager::kWhite) {
            boot_splash_seconds = 4.0f;
            boot_splash_texture = splash;
            if (!shader_screen_wanted) ui.ShowBootSplash(splash, boot_splash_seconds);
        } else {
            pt::LogInfo("boot: no title picture at {} (the setup copies it from the dump or package), the start goes to its loading screen",
                        pt::os::PathToUtf8(options.game_dir / "sce_sys" / "pic1.png"));
        }
    }
    if (ui_ready) {
        ui.SetViewExtent({app.options.width, app.options.height});
        ui.SetSupportPrompts(app.settings.ui.support_prompts);
        {
            const std::string current(pt::update::CurrentVersion());
            const std::string last = app.settings.ui.last_version;
            if (g_update_mark_started || (!last.empty() && last != current)) {
                pt::LogInfo("support: first start of {} after {}: the update's thank-you notice", current, last.empty() ? "an update" : last);
                ui.ShowNotice(std::string(pt::game::GameUi::kUpdatedNotice));
            }
            if (last != current) {
                app.settings.ui.last_version = current;
                if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
            }
        }
        game.SetOptionsUiAvailable(app.window != nullptr || options.first_boot_options);
        app.renderer.overlay = [&ui, &hide_game_ui, &photo_view, &app, &game](pt::gpu::CommandList& cmd, pt::gpu::ImageViewHandle view,
                                                                             pt::gpu::Extent2D extent,
                                                                             pt::Renderer::OverlayPart part) {
            if (part == pt::Renderer::OverlayPart::Top) {
                ui.Record(cmd, view, extent, pt::game::GameUi::Part::Top);
                return;
            }
            ui.SetLetterbox(pt::game::LetterboxAspect(app.settings.display.letterbox));
            ui.SetAccessibility(app.settings.accessibility.subtitle_size, app.settings.accessibility.subtitle_bg,
                                app.settings.accessibility.reduced_flashing);
            if (!hide_game_ui) {
                if (game.StreetWalkActive() && ui.MenuOpen()) app.renderer.grain[0] = 0.0f;
                ui.Record(cmd, view, extent,
                          part == pt::Renderer::OverlayPart::Below ? pt::game::GameUi::Part::Below : pt::game::GameUi::Part::All);
            } else {
                ui.RecordPhotoMode(cmd, extent, photo_view);
            }
        };
        app.renderer.overlay_top_pending = [&ui, &hide_game_ui] { return !hide_game_ui && ui.TopPending(); };
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
    input.settings.gamepad_sensitivity = app.settings.input.gamepad_sensitivity;
    input.settings.stick_dead_zone = app.settings.input.gamepad_dead_zone;
    input.settings.rumble = app.settings.input.rumble;
    input.settings.trigger_rumble = pt::FeaturesForRumbleProfile(app.settings.input.rumble_profile, app.settings.input.rumble).trigger_rumble;
    input.settings.prompt_style = app.settings.input.prompt_style;
    input.settings.gyro = app.settings.accessibility.gyro;
    input.settings.gyro_sensitivity = app.settings.accessibility.gyro_sensitivity;
    input.settings.gyro_tightening = app.settings.accessibility.gyro_tightening;
    input.settings.gyro_bias = app.settings.accessibility.gyro_bias;
    input.SetToggleModes(app.settings.accessibility.sprint_toggle, app.settings.accessibility.zoom_toggle);
    pt::ApplyKeyBindings(app.settings.keys);
    PcSettings pc_settings(app, game, input);
    if (ui_ready) {
        ui.SetPcSettings(&pc_settings);
        app.transcripts = &ui.Subtitles();
    }
    const bool pads = app.window || options.virtual_pads;
    if (app.window) input.ListenCaptureKeyEverywhere();
    pt::devices::TakeGamepadsReady();
    if (pads && pt::devices::GamepadsReady()) {
        input.Init();
    } else if (pads) {
        pt::LogInfo("input: the controllers are still starting; the pads join when they are ready");
    }
    if (app.window && pt::devices::GamepadsReady()) {
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    }
    pt::VirtualPads virtual_pads;
    pt::game::PromptTextures prompt_textures;
    prompt_textures.Init(vfs, app.textures, app.window != nullptr);
    script.SetPads(options.virtual_pads ? &virtual_pads : nullptr, &input);
    std::vector<float> offline_audio;
    double audio_device_time = 0.0;
    uint64_t audio_device_frames = 0;
    SDL_Scancode voice_key = app.settings.voice.key.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(app.settings.voice.key.c_str());
    pt::KeyPressLatch voice_key_press;
    if (!app.settings.voice.key.empty()) {
        if (voice_key == SDL_SCANCODE_UNKNOWN) {
            pt::LogInfo("voice: keyboard fallback key '{}' is invalid", app.settings.voice.key);
        } else {
            pt::LogInfo("voice: keyboard fallback key '{}' resolved to {}", app.settings.voice.key, SDL_GetScancodeName(voice_key));
        }
    }
    std::unique_ptr<pt::VoiceRecognizer> recognizer;
    pt::Microphone microphone;
    std::string active_microphone_device;
    bool previous_microphone_test = false;
    bool microphone_failed = false;
    float mic_quiet_seconds = 0.0f;
    std::vector<int16_t> mic_samples;
    std::vector<int16_t> voice_input;
    size_t voice_input_at = 0;
    double voice_input_due = 0.0;
    const bool voice_input_live = std::getenv("PT_VOICE_INPUT_LIVE") && std::string_view(std::getenv("PT_VOICE_INPUT_LIVE")) == "1";
    if (const std::string voice_path = pt::os::GetEnv("PT_VOICE_INPUT"); !voice_path.empty()) {
        const char* path = voice_path.c_str();
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
    bool mouse_capture_requested = false;
    std::vector<pt::DrawItem> draw_items;
    app.scene.LoadResources(vfs);
    pt::VfxPass vfx_pass;
    pt::game::VfxScene vfx_scene;
    pt::LogInfo("startup: effects pass");
    if (vfx_pass.Init(app.renderer, app.textures, vfs)) {
        app.scene.vfx_forward = [&vfx_pass](const pt::SceneVfxContext& c) { vfx_pass.RecordForward(c); };
        app.scene.vfx_liquid_depth = [&vfx_pass](const pt::SceneVfxContext& c) { vfx_pass.RecordLiquidDepth(c); };
        app.scene.vfx_filter = [&vfx_pass](const pt::SceneFilterContext& c) { vfx_pass.RecordFilter(c); };
        app.scene.vfx_list = [&vfx_pass]() { return &vfx_pass.List(); };
    }
    pt::vk::PipelineWarmup pipeline_warmup;
    vfx_pass.AddPipelineWarmup(pipeline_warmup, pt::SceneRenderer::kHdrTargetFormat, pt::SceneRenderer::kDepthTargetFormat,
                               pt::SceneRenderer::kPostTargetFormat);
    app.renderer.AddPipelineWarmup(pipeline_warmup);
    pipeline_warmup.Start(std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u));
    const bool shaders_cold = app.renderer.Context().pipeline_cache_cold;
    bool pipelines_warm = false;
    bool shader_screen = shader_screen_wanted;
    const std::filesystem::path boot_shots = std::getenv("PT_BOOT_SHOTS") ? std::filesystem::path(std::getenv("PT_BOOT_SHOTS")) : std::filesystem::path();
    pt::game::RenderSceneBuilder scene_builder;
    pt::SceneLighting lighting;
    double last_scene_time = game.Time();
    bool running = true;
    uint64_t frame = 0;
    bool first_frame_shown = false;
    uint64_t last_ticks = SDL_GetTicksNS();
    pt::InputState pending_input;
    bool ending_skip_press = false;
    bool ending_skip_held = false;
    float accumulator = 0.0f;
    FramePhases frame_phases;
    std::chrono::steady_clock::time_point frozen_since{};
    double frozen_seconds = 0.0;
    pt::Camera tick_camera_from = game.ViewCamera();
    pt::Camera motion_camera_from = tick_camera_from;
    const bool paced = app.window != nullptr || options.display_rate > 0.0f;
    const int headless_fps_limit = options.headless && std::getenv("PT_HEADLESS_FPS_LIMIT")
                                       ? std::clamp(std::atoi(std::getenv("PT_HEADLESS_FPS_LIMIT")), 0, 60) : 0;
    std::chrono::steady_clock::time_point headless_frame_deadline{};
    if (headless_fps_limit > 0) pt::LogInfo("headless: rendering limited to {} FPS (PT_HEADLESS_FPS_LIMIT)", headless_fps_limit);
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
    std::unique_ptr<pt::game::LisaDate> lisa_date;
    bool lisa_date_paused = false;
    bool lisa_date_replay = false;
    bool lisa_date_was_showing = false;
    auto start_freecam = [&]() {
        if (lisa_date) return;
        if (freecam) return;
        freecam = true;
        app.freecam_active = true;
        freecam_camera = game.ViewCamera(false);
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
        if (lisa_date) return;
        if (photo_mode) return;
        photo_had_freecam = freecam;
        start_freecam();
        const bool frame_player = !game.Demos().HasActiveCamera() &&
            (!game.Floor().IsCurrentFloorName("ending") || game.StreetWalkActive());
        if (!photo_had_freecam && frame_player) {
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
        settings.focal_length_mm = pt::game::PhotoFocalLengthFromFovYDegrees(glm::degrees(freecam_camera.fov_y));
        settings.roll = std::clamp(static_cast<int>(std::lround(glm::degrees(freecam_camera.roll) / 5.0f)) * 5, -90, 90);
        settings.depth_of_field = app.scene.toggles.depth_of_field;
        settings.bloom = app.scene.toggles.bloom;
        settings.lens = app.scene.toggles.distortion;
        settings.grain = app.scene.toggles.film_grain;
        settings.grading = app.scene.toggles.color_lut;
        settings.aspect = std::clamp(app.settings.display.letterbox, 0, 2);
        photo_panel.Open(settings);
        if (!game.Paused()) {
            game.SetPaused(true);
            photo_paused = true;
            if (game.Audio()) game.Audio()->PostEvent("Pause_All", nullptr);
        }
        const auto& photo = photo_panel.Settings();
        pt::LogInfo("extras: photo mode on, lens {} mm, roll {}, exposure {}, aspect {}, resolution {}, filter {}",
                    photo.focal_length_mm, photo.roll, photo.exposure, photo.aspect, static_cast<int>(photo.resolution),
                    static_cast<int>(photo.filter));
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
        const bool shown = entry && (entry->media == pt::game::ArchiveMedia::Demo || entry->media == pt::game::ArchiveMedia::Model);
        if (!shown || theater || !ui_ready) {
            pt::LogWarn("archive: {} cannot open{}", id, entry ? (shown ? "" : " in the theater") : " (no such entry)");
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
        const int language = std::clamp(game.Options().subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1);
        ui.SetMenuSuspended(true);
        ui.EnterTheater(view->theater->Sandbox());
        if (options.make_museum_previews.empty()) {
            ui.SetTheaterHint(pt::game::ArchiveLabel(*entry, language),
                              std::string(pt::game::PcText(entry->media == pt::game::ArchiveMedia::Model ? "pc_archive_model_hint"
                                                           : entry->free_camera                           ? "pc_archive_free_hint"
                                                                                                          : "pc_archive_demo_hint",
                                                           language)));
        }
        theater = std::move(view);
        game.SetArchiveTheaterActive(true);
        pt::LogInfo("archive: theater opens {}", id);
    };
    auto end_theater = [&]() {
        if (!theater) return;
        app.renderer.Gpu().WaitIdle();
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
    auto start_lisa_date = [&](const std::string& cause, bool replay) {
        if (lisa_date || theater || !ui_ready) {
            pt::LogWarn("lisa date: cannot start ({}){}", cause, lisa_date ? ", already running" : theater ? ", the theater runs" : "");
            return;
        }
        stop_freecam();
        pt::game::LisaDate::Settings settings;
        settings.language = std::clamp(game.Options().subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1);
        settings.replay = replay;
        settings.cause = cause;
        const pt::game::Gimmick& ocho = game.Objects().Gimmicks()[static_cast<size_t>(pt::game::GimmickType::Ocho)];
        if (ocho.mesh) {
            settings.lisa_mesh = ocho.mesh;
            if (!ocho.hidden_meshes.empty()) settings.lisa_hidden = pt::HiddenMeshes(*ocho.mesh, ocho.hidden_meshes);
        }
        if (const std::string& file = game.Demos().PlayerModelFile(); !file.empty()) {
            if (const pt::ModelEntry* player = app.models->Get(file); player && player->mesh) {
                settings.player_mesh = player->mesh.get();
                settings.player_hidden = pt::HiddenMeshes(*player->mesh, game.Demos().PlayerDefaultHidden());
                settings.player_help = game.Demos().PlayerHelpBones();
            }
        }
        auto date = std::make_unique<pt::game::LisaDate>(vfs, *app.models, ui.Assets(), game.Audio());
        if (!date->Start(settings)) return;
        if (!game.Paused()) {
            game.SetPaused(true);
            lisa_date_paused = true;
            if (game.Audio()) game.Audio()->PostEvent("Pause_All", nullptr);
        }
        lisa_date_replay = replay;
        if (replay) ui.SetMenuSuspended(true);
        game.HoldSaves(true);
        game.Speedrun().PauseReal();
        lisa_date = std::move(date);
        game.SetLisaDateScene(lisa_date.get());
        ui.SetLisaDate(lisa_date.get());
    };
    auto end_lisa_date = [&]() {
        if (!lisa_date) return;
        const bool completed = lisa_date->Completed();
        const std::string ending(lisa_date->Ending());
        app.renderer.Gpu().WaitIdle();
        ui.SetLisaDate(nullptr);
        game.SetLisaDateScene(nullptr);
        lisa_date.reset();
        app.scene.SetBackdrop({});
        app.scene.ResetUpscaleHistory();
        if (lisa_date_replay) ui.SetMenuSuspended(false);
        if (lisa_date_paused) {
            lisa_date_paused = false;
            game.SetPaused(false);
            if (game.Audio()) game.Audio()->PostEvent("Resume_All", nullptr);
        }
        game.Speedrun().ResumeReal();
        game.HoldSaves(false);
        if (completed && !lisa_date_replay) game.NoteArchive("lisa_date");
        pt::LogInfo("lisa date: returned to {}{} (ending {})", lisa_date_replay ? "the menu on " : "", game.Floor().CurrentFloorName(),
                    ending.empty() ? "none" : ending);
        lisa_date_replay = false;
    };
    std::unique_ptr<pt::game::VrPlay> vr;
    if (app.xr) {
        vr = std::make_unique<pt::game::VrPlay>(*app.xr, app.settings.vr);
    }
    bool frozen = false;
    bool was_focused = false;
    bool ever_focused = false;
    int activate_tries = 0;
    uint64_t activate_next_ns = 0;
    bool boot_wait = true;
    uint64_t boot_wait_start = SDL_GetTicksNS();
    const uint64_t shader_screen_start = boot_wait_start;
    double boot_splash_gone = -1.0;
    float applied_volume = -1.0f;
    std::string applied_output_device = app.settings.audio.output_device;
    pt::ControllerSpeakerOutput controller_speaker;
    SDL_JoystickID speaker_gamepad_id = 0;
    uint64_t next_speaker_retry_ns = 0;
    std::string last_speaker_error;
    bool speaker_route_requested = false;
    const auto& kLisaCryEvents = pt::game::kControllerSpeakerEvents;
    const auto& kPlayerStepEvents = pt::game::kControllerHapticOnlyEvents;
    std::atomic<int> display_hdr{-1};
    std::atomic<bool> display_capable{false};
    std::chrono::steady_clock::time_point display_hdr_recheck{};
    int display_hdr_rechecks = 0;
    bool windows_hdr_failed = false;
    struct JoinedThread {
        std::thread thread;
        ~JoinedThread() {
            if (thread.joinable()) thread.join();
        }
    } display_hdr_read;
    void* display_hwnd = app.window ? SDL_GetPointerProperty(SDL_GetWindowProperties(app.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) : nullptr;
    const char* const hdr_switch_dry = std::getenv("PT_HDR_SWITCH_DRY");
    const bool hdr_switch_dry_off = hdr_switch_dry && std::string_view(hdr_switch_dry) == "off";
    auto read_display_hdr = [&]() {
        if (!display_hwnd) return;
        if (display_hdr_read.thread.joinable()) display_hdr_read.thread.join();
        display_hdr_read.thread = std::thread([hwnd = display_hwnd, &display_hdr, &display_capable, hdr_switch_dry_off]() {
            if (hdr_switch_dry_off) {
                display_hdr.store(0);
                display_capable.store(true);
                return;
            }
            const std::optional<bool> hdr_on = pt::os::WindowDisplayHdr(hwnd);
            display_hdr.store(hdr_on ? (*hdr_on ? 1 : 0) : -1);
            display_capable.store(pt::os::WindowDisplayHdrCapable(hwnd));
        });
    };
    struct RestoreWindowsHdr {
        App& app;
        void* hwnd;
        bool dry;
        ~RestoreWindowsHdr() {
            if (!hwnd || !app.windows_hdr_switched) return;
            app.windows_hdr_switched = false;
            const bool done = dry || pt::os::SetWindowDisplayHdr(hwnd, false);
            if (app.settings.display.hdr_windows_held) {
                app.settings.display.hdr_windows_held = false;
                if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
            }
            pt::LogInfo("display: the game closes, Windows HDR turned back off for this display: {}", done ? "done" : "failed");
        }
    } restore_windows_hdr{app, display_hwnd, hdr_switch_dry != nullptr};
    if (display_hwnd && app.settings.display.hdr_windows_held) {
        if (app.settings.display.hdr && app.settings.display.hdr_windows_switch) {
            app.windows_hdr_switched = true;
            pt::LogInfo("display: previous session left Windows HDR held; restoring ownership for clean exit");
        } else {
            const bool done = hdr_switch_dry || pt::os::SetWindowDisplayHdr(display_hwnd, false);
            app.settings.display.hdr_windows_held = false;
            if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
            pt::LogInfo("display: recovered Windows HDR left on by an earlier crash: {}", done ? "done" : "failed");
        }
    }
    read_display_hdr();
    bool display_focus = app.window && (SDL_GetWindowFlags(app.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    SDL_DisplayID display_id = app.window ? SDL_GetDisplayForWindow(app.window) : 0;
    int display_hdr_seen = -1;
    std::vector<uint64_t> hdr_toggle_frames;
    if (const char* toggles = std::getenv("PT_TEST_HDR_TOGGLE")) {
        for (std::string_view rest = toggles; !rest.empty();) {
            const size_t comma = rest.find(',');
            hdr_toggle_frames.push_back(std::strtoull(std::string(rest.substr(0, comma)).c_str(), nullptr, 10));
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        }
    }
    while (running) {
        if (app.window) {
            const bool focus = (SDL_GetWindowFlags(app.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
            const SDL_DisplayID id = SDL_GetDisplayForWindow(app.window);
            if ((focus && !display_focus) || id != display_id) read_display_hdr();
            display_focus = focus;
            display_id = id;
        }
        if (!hdr_toggle_frames.empty() && frame >= hdr_toggle_frames.front()) {
            hdr_toggle_frames.erase(hdr_toggle_frames.begin());
            app.settings.display.hdr = !app.settings.display.hdr;
            app.renderer.SetHdr(app.settings.display.hdr);
        }
        app.display_hdr_capable = display_capable.load();
        if (!(app.settings.display.hdr && app.settings.display.hdr_windows_switch)) windows_hdr_failed = false;
        if (display_hwnd && app.display_hdr == 0 && app.display_hdr_capable && app.settings.display.hdr && app.settings.display.hdr_windows_switch &&
            !app.windows_hdr_switched && !windows_hdr_failed) {
            const bool done = hdr_switch_dry || pt::os::SetWindowDisplayHdr(display_hwnd, true);
            app.windows_hdr_switched = done;
            windows_hdr_failed = !done;
            if (done && !app.settings.display.hdr_windows_held) {
                app.settings.display.hdr_windows_held = true;
                if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
            }
            pt::LogInfo("display: turning Windows HDR on for this display: {}", done ? "done" : "failed");
            display_hdr_rechecks = 6;
            display_hdr_recheck = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        } else if (display_hwnd && app.windows_hdr_switched && !(app.settings.display.hdr && app.settings.display.hdr_windows_switch)) {
            app.windows_hdr_switched = false;
            if (!hdr_switch_dry) pt::os::SetWindowDisplayHdr(display_hwnd, false);
            if (app.settings.display.hdr_windows_held) {
                app.settings.display.hdr_windows_held = false;
                if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
            }
            pt::LogInfo("display: Windows HDR turned back off for this display");
            display_hdr_rechecks = 6;
            display_hdr_recheck = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (display_hdr_rechecks > 0 && std::chrono::steady_clock::now() >= display_hdr_recheck) {
            --display_hdr_rechecks;
            display_hdr_recheck = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            read_display_hdr();
        }
        if (const int now = display_hdr.load(); now >= 0 && now != display_hdr_seen) {
            app.display_hdr = now;
            if (display_hdr_seen >= 0) {
                pt::LogInfo("display: Windows HDR turned {} for this display", now ? "on" : "off");
                app.renderer.RecheckOutput();
            }
            display_hdr_seen = now;
        }
        const SDL_Scancode next_voice_key = app.settings.voice.key.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(app.settings.voice.key.c_str());
        if(next_voice_key!=voice_key) { voice_key_press.Discard(); voice_key=next_voice_key; }
        g_focus_trace.frame = frame;
        g_focus_trace.zoom = game.GetPlayer().zoom;
        const bool peephole = game.Nazo().IsPeepholeTheaterActive();
        if (g_focus_trace.peephole != peephole) {
            g_focus_trace.peephole = peephole;
            if (app.window) LogWindowInput(app.window, peephole ? "peephole entered" : "peephole left");
        }
        bool visible = PumpEvents(app, &input, running, &voice_key_press, static_cast<uint32_t>(voice_key)) || vr != nullptr;
        if (audio_device_pending && !pt::devices::AudioStarting()) {
            audio_device_pending = false;
            const bool opened = sound.System().OpenDevice(app.settings.audio.surround || app.settings.audio.spatial);
            pt::LogInfo("audio: the audio devices are ready at frame {}; playback device {}", frame, opened ? "open" : "not available");
        }
        if (vr) {
            vr->BeginLoop(running);
            if (!vr->Host().SessionRunning()) {
                visible = false;
                SDL_Delay(10);
            }
        }
        if (!pipelines_warm && pipeline_warmup.Done()) {
            pipelines_warm = true;
            pipeline_warmup.Join();
            pt::LogInfo("pipelines: {} made ahead in {:.2f} s{}", pipeline_warmup.Pipelines(), pipeline_warmup.Seconds(),
                        shaders_cold ? " (first start, cleared cache or new driver)" : "");
            pt::vk::SetPipelinePlayPhase(true);
            app.renderer.Context().pipeline_saver.Request();
        }
        if (shader_screen) {
            const double on_screen = static_cast<double>(SDL_GetTicksNS() - shader_screen_start) * 1e-9;
            if (pipelines_warm || on_screen > 300.0) {
                shader_screen = false;
                ui.SetShaderScreen({});
                boot_wait_start = SDL_GetTicksNS();
                if (boot_splash_texture != pt::TextureManager::kWhite) ui.ShowBootSplash(boot_splash_texture, boot_splash_seconds);
                pt::LogInfo("boot: shader preparation screen shown for {:.1f} s{}", on_screen, pipelines_warm ? "" : " (pipelines still being made)");
            } else {
                ui.SetShaderScreen(std::format("pc_boot_preparing_shaders{}{}", pt::game::kPcNoteArgument,
                                               100u * pipeline_warmup.Finished() / std::max(1u, pipeline_warmup.Total())));
            }
        }
        if (boot_wait) {
            const double waited = static_cast<double>(SDL_GetTicksNS() - boot_wait_start) * 1e-9;
            const bool splash = ui_ready && ui.BootSplashShown() && waited < 30.0;
            if (splash) boot_splash_gone = waited;
            const bool after_splash = boot_splash_gone >= 0.0 && waited < boot_splash_gone + 1.0;
            const bool warming = !pipelines_warm && (shader_screen || waited < 60.0);
            const bool hold = (warming || waited < boot_splash_seconds + 1.0 || splash || after_splash) &&
                              game.Controller().Step() <= 5;
            if (!hold) {
                boot_wait = false;
                pt::LogInfo("start: loading screen released after {:.1f} s{}", waited,
                            pipelines_warm ? "" : " (pipelines still being made)");
            }
            game.SetBootHold(hold);
        }
        const std::optional<bool> forced_focus = script.ForcedFocus(frame);
        if (app.window || forced_focus || vr) {
            const bool focused = forced_focus ? *forced_focus
                                 : vr         ? !vr->Host().FocusLost()
                                              : visible && WindowActive(app.window);
            if (app.window && !forced_focus && !vr && visible && !focused && !ever_focused && activate_tries < 4 &&
                SDL_GetTicksNS() > activate_next_ns) {
                ++activate_tries;
                activate_next_ns = SDL_GetTicksNS() + 700000000ull;
                SDL_RaiseWindow(app.window);
                const bool in_front = pt::os::ActivateWindow(WindowHandle(app.window));
                pt::LogInfo("focus: window not in front yet, raise {} of 4: {} ({})", activate_tries, in_front ? "in front" : "still behind",
                            DescribeForeground(pt::os::GetForegroundWindowInfo()));
            }
            if (vr && was_focused && !focused && ui_ready && game.Controller().Step() >= 0 && !ui.MenuOpen() && !lisa_date &&
                !game.Status().IsSet("S_DISABLE_GAME_PAUSE")) {
                stop_freecam();
                ui.OpenMenu(game, false);
            }
            if (was_focused && !focused) {
                pt::LogInfo("focus: lost at frame {}: {}; last SDL event {}; {}", frame, DescribeForeground(pt::os::GetForegroundWindowInfo()),
                            g_focus_trace.last_event, DescribeWindowInput(app.window));
            }
            was_focused = focused;
            ever_focused = ever_focused || focused;
            const bool freeze = !visible || (vr && !focused && !(ui_ready && ui.MenuOpen()));
            if (freeze != frozen) {
                frozen = freeze;
                if (sound.Ready()) {
                    sound.System().SetFrozen(frozen);
                }
                pt::LogInfo("focus: game {} at frame {}, audio clock {}", frozen ? "frozen in the background" : "resumed", frame,
                            sound.Ready() ? sound.System().RenderedFrames() : 0);
                if (frozen) {
                    pt::LogInfo("focus: frozen because {}; last SDL event {}; {}", !visible ? "the window is minimized" : "the window lost the focus",
                                g_focus_trace.last_event, DescribeForeground(pt::os::GetForegroundWindowInfo()));
                }
                last_ticks = SDL_GetTicksNS();
            }
            const float volume = app.settings.audio.volume;
            if (sound.Ready() && volume != applied_volume) {
                sound.System().SetMasterVolume(volume);
                applied_volume = volume;
            }
        }
        if (sound.Ready() && !audio_device_pending) {
            const bool surround = app.settings.audio.surround || app.settings.audio.spatial;
            if (app.settings.audio.output_device != applied_output_device) {
                applied_output_device = app.settings.audio.output_device;
                pt::audio::AudioOutput::SetPreferredDevice(applied_output_device);
                pt::LogInfo("audio: output device set to {} in the settings", applied_output_device.empty()
                                                                                   ? std::string("the system default")
                                                                                   : std::format("'{}'", applied_output_device));
                const bool opened = sound.System().ReopenDevice(surround);
                pt::LogInfo("audio: playback device opened again: {}", opened ? sound.System().OutputDeviceName() : std::string("none"));
            } else {
                sound.System().UpdateOutput(surround, SDL_GetTicksNS());
            }
            app.audio_output_name = sound.System().OutputDeviceName();
        }
        SDL_Gamepad* selected_gamepad = (app.window || options.virtual_pads) ? input.LastUsedGamepad() : nullptr;
        const pt::SonyPadAudio selected_model = selected_gamepad ?
            pt::SonyPadAudioModel(SDL_GetGamepadVendor(selected_gamepad), SDL_GetGamepadProduct(selected_gamepad)) : pt::SonyPadAudio::None;
        const bool speaker_audio_requested = pt::ControllerSpeakerAudioEnabled(selected_model, app.settings.audio.controller_speaker) &&
                                             app.settings.audio.controller_speaker_volume > 0.0f;
        const pt::ControllerFeedbackFeatures feedback =
            pt::FeaturesForRumbleProfile(app.settings.input.rumble_profile, app.settings.input.rumble);
        static const bool speaker_stand_in = std::getenv("PT_CONTROLLER_SPEAKER_DEVICE") != nullptr;
        const bool speaker_requested = (options.virtual_pads ? speaker_stand_in : app.window != nullptr) && (speaker_audio_requested || feedback.dualsense_haptics);
        if (speaker_requested != speaker_route_requested) {
            controller_speaker.Close();
            speaker_gamepad_id = 0;
            next_speaker_retry_ns = 0;
            last_speaker_error.clear();
            speaker_route_requested = speaker_requested;
        }
        const float controller_volume = app.settings.audio.volume;
        const bool capture_allowed = speaker_requested && sound.Ready() && visible && !frozen && controller_volume > 0.0f;
        const SDL_JoystickID selected_gamepad_id = selected_gamepad ? SDL_GetGamepadID(selected_gamepad) : 0;
        if (selected_gamepad_id != speaker_gamepad_id) {
            const bool had_route = controller_speaker.IsOpen();
            if (!controller_speaker.MatchesGamepad(selected_gamepad)) controller_speaker.Close();
            speaker_gamepad_id = selected_gamepad_id;
            if (had_route) {
                next_speaker_retry_ns = 0;
                last_speaker_error.clear();
            }
        }
        if (!speaker_audio_requested && controller_speaker.Route() == pt::ControllerPcmRoute::DualShock4BluetoothSpeaker) controller_speaker.Close();
        const bool selected_dualsense = selected_model == pt::SonyPadAudio::DualSense;
        if (capture_allowed && selected_gamepad && (speaker_audio_requested || selected_dualsense) && !controller_speaker.IsOpen() &&
            SDL_GetTicksNS() >= next_speaker_retry_ns) {
            next_speaker_retry_ns = SDL_GetTicksNS() + 2'000'000'000ull;
            std::string reason;
            if (!controller_speaker.OpenForGamepad(selected_gamepad, &reason)) {
                if (reason != last_speaker_error) {
                    pt::LogInfo("input: controller PCM unavailable: {}", reason);
                    last_speaker_error = std::move(reason);
                }
            } else {
                last_speaker_error.clear();
            }
        }
        const bool dualsense_route = controller_speaker.Route() == pt::ControllerPcmRoute::DualSenseQuad ||
                                     controller_speaker.Route() == pt::ControllerPcmRoute::DualSenseBluetoothHaptics;
        const bool controller_haptics = feedback.dualsense_haptics && dualsense_route;
        if (controller_speaker.Route() == pt::ControllerPcmRoute::DualSenseQuad) {
            const bool main_output_on_pad = pt::SameAudioEndpoint(app.audio_output_name, controller_speaker.DeviceName());
            controller_speaker.SelectDualSenseSpeakerPath(pt::DualSenseSpeakerPathWanted(speaker_audio_requested, main_output_on_pad),
                                                          app.audio_output_name);
        }
        const bool speaker_audio = pt::ControllerSpeakerPcmAllowed(controller_speaker.Route(), speaker_audio_requested,
                                                                   controller_speaker.DualSenseSpeakerPathSelected());
        const bool route_has_output = controller_speaker.IsOpen() && (speaker_audio || controller_haptics);
        if (capture_allowed && route_has_output) {
            sound.System().SetControllerCaptureEvents(kLisaCryEvents);
            sound.System().SetControllerHapticEvents(controller_haptics ? std::span<const uint32_t>(kPlayerStepEvents) : std::span<const uint32_t>{});
        } else {
            sound.System().SetControllerCaptureEvents(std::span<const uint32_t>{});
            sound.System().SetControllerHapticEvents(std::span<const uint32_t>{});
            controller_speaker.ClearPending();
        }
        pt::audio::ControllerPcmBlock controller_block;
        while (sound.Ready() && sound.System().TryReadControllerPcm(controller_block)) {
            if (capture_allowed && route_has_output) {
                if (!controller_speaker.WriteCapturedBlock(controller_block, speaker_audio,
                                                           controller_haptics && app.settings.input.rumble,
                                                           controller_volume * app.settings.audio.controller_speaker_volume, controller_volume)) {
                    pt::LogWarn("input: controller PCM write failed for '{}': {}", controller_speaker.DeviceName(), SDL_GetError());
                    controller_speaker.Close();
                    next_speaker_retry_ns = SDL_GetTicksNS() + 2'000'000'000ull;
                }
            }
        }
        if (!visible || frozen) {
            if (frozen_since.time_since_epoch().count() == 0) frozen_since = std::chrono::steady_clock::now();
            frame_phases.last_end = {};
            voice_key_press.Discard();
            input.Poll(false, pt::MouseUse::None, false);
            input.SetRumble(0, 0);
            input.SetTriggerRumble(0, 0);
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
            const bool photo_pointer = ((photo_mode && photo_overlay) || (theater && (theater->theater->ModelView() || theater->theater->FreeView()))) &&
                                       !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK);
            const bool want_capture = !show_debug && !show_settings && !(ui_ready && ui.MenuOpen() && !theater) && !photo_pointer && !vr && !lisa_date &&
                                      !(ui_ready && ui.SurvivalChoosing()) && WindowActive(app.window);
            if (want_capture != mouse_capture_requested) {
                mouse_capture_requested = want_capture;
                if (want_capture) {
                    const bool relative = SDL_SetWindowRelativeMouseMode(app.window, true);
                    const std::string relative_error = relative ? std::string{} : SDL_GetError();
                    const bool grabbed = relative || SDL_SetWindowMouseGrab(app.window, true);
                    mouse_captured = grabbed;
                    const char* driver = SDL_GetCurrentVideoDriver();
                    if (relative) {
                        pt::LogInfo("input: mouse captured with relative mode (SDL driver {})", driver ? driver : "none");
                    } else if (grabbed) {
                        pt::LogWarn("input: relative mouse mode unavailable ({}); cursor confined with SDL mouse grab (driver {})",
                                    relative_error, driver ? driver : "none");
                    } else {
                        pt::LogWarn("input: cannot capture mouse with relative mode ({}) or SDL mouse grab ({}) (driver {})",
                                    relative_error, SDL_GetError(), driver ? driver : "none");
                    }
                    LogWindowInput(app.window, mouse_captured ? "gameplay mouse captured" : "gameplay mouse capture failed");
                } else {
                    SDL_SetWindowRelativeMouseMode(app.window, false);
                    SDL_SetWindowMouseGrab(app.window, false);
                    mouse_captured = false;
                    LogWindowInput(app.window, "gameplay mouse capture released");
                }
            }
        }
        const bool keyboard_free = app.window && !show_settings && !(show_debug && ImGui::GetIO().WantCaptureKeyboard);
        const bool menu_open = (ui_ready && ui.MenuOpen() && !theater) || lisa_date != nullptr;
        const bool pointer_up = menu_open || (ui_ready && ui.SurvivalChoosing());
        const pt::MouseUse mouse_use = mouse_captured || (!app.window && !pointer_up) ? pt::MouseUse::Look
                                       : pointer_up && !show_settings                  ? pt::MouseUse::Menu
                                                                                       : pt::MouseUse::None;
        pt::InputState polled = pads ? input.Poll(keyboard_free, mouse_use, !show_settings) : pt::InputState{};
        pc_settings.CollectGyroCalibration();
        if (app.window) {
            static bool cursor_hidden = false;
            static glm::vec2 cursor_last(-1.0f);
            glm::vec2 cursor;
            SDL_GetMouseState(&cursor.x, &cursor.y);
            const bool mouse_moved = cursor_last.x >= 0.0f && glm::length(cursor - cursor_last) > 3.0f;
            cursor_last = cursor;
            const bool pad_used = (polled.any_button && polled.from_gamepad) ||
                                  (polled.left_stick_from_pad && glm::length(polled.left_stick) > 0.5f) ||
                                  (polled.from_gamepad && glm::length(polled.right_stick) > 0.5f);
            const bool pointer_menu = menu_open && !show_settings && !show_debug && !mouse_captured;
            bool hide = cursor_hidden;
            if (pad_used) hide = true;
            if (mouse_moved || polled.click || polled.right_click || (polled.any_button && !polled.from_gamepad)) hide = false;
            if (!pointer_menu) hide = false;
            if (ImGui::GetCurrentContext()) {
                ImGuiIO& io = ImGui::GetIO();
                if (cursor_hidden || hide) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
                else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            }
            if (cursor_hidden && hide && SDL_CursorVisible()) SDL_HideCursor();
            if (hide != cursor_hidden) {
                cursor_hidden = hide;
                if (hide) {
                    SDL_HideCursor();
                } else {
                    SDL_ShowCursor();
                }
                pt::LogInfo("input: menu cursor {}", hide ? "hidden for the controller" : "shown");
            }
        }
        const bool ending_outro = pt::game::EndingOutroInputBlocked(game.Controller().Step());
        ending_skip_press = ending_skip_press || polled.pause;
        ending_skip_held = polled.pause_held;
        if (!freecam && !photo_mode && !theater) polled = pt::game::GateEndingOutroInput(game.Controller().Step(), polled);
        if (options.forced_prompts) {
            polled.prompts = *options.forced_prompts;
        }
        if (vr && !ending_outro) {
            vr->ApplyControls(game, polled, (ui_ready && ui.MenuOpen()) || lisa_date != nullptr, vr->ScreenMode(game), dt);
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
        pending_input.mouse_look += polled.mouse_look;
        pending_input.gyro_turn += polled.gyro_turn;
        pending_input.gyro_light = polled.gyro_light;
        pending_input.gamepad_sensitivity = polled.gamepad_sensitivity;
        pt::MergeFrameButtons(pending_input, polled);
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
        pending_input.click_held = polled.click_held;
        pending_input.right_click = pending_input.right_click || polled.right_click;
        pending_input.wheel += polled.wheel;
        pending_input.house_pressed = pending_input.house_pressed || polled.house_pressed;
        pending_input.pointer_valid = false;
        if (app.window && !mouse_captured && (!ending_outro || photo_mode)) {
            float x = 0.0f;
            float y = 0.0f;
            SDL_GetMouseState(&x, &y);
            pending_input.pointer = glm::vec2(x, y) * SDL_GetWindowPixelDensity(app.window);
            pending_input.pointer_valid = true;
        }
        if (const std::vector<int> set = game.TakePhotoSettingsRequest(); (set.size() == 7 || set.size() == 8) && photo_mode) {
            pt::game::PhotoSettings& photo = photo_panel.Settings();
            photo.focal_length_mm = pt::game::PhotoFocalLengthFromFovYDegrees(static_cast<float>(set[0]));
            photo.roll = set[1];
            photo.focus = set[2];
            photo.aperture = set[3];
            photo.aspect = set[4];
            photo.depth_of_field = set[2] != 0 || set[3] != 0;
            photo.exposure = pt::game::PhotoSettings::kExposureZero + set[5];
            photo.body = set[6] != 0;
            if(set.size()==8) photo.resolution = set[7] == 1 ? pt::game::PhotoResolution::FourK : pt::game::PhotoResolution::Native;
        }
        if (const int grid = game.TakePhotoGridRequest(); grid >= 0 && photo_mode) {
            photo_panel.Settings().grid = std::clamp(grid, 0, static_cast<int>(pt::game::PhotoGrid::Count) - 1);
            pt::LogInfo("extras: photo grid {}", photo_panel.Settings().grid);
        }
        if (game.TakePhotoTakeRequest() && photo_mode) photo_shot = true;
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
        if (const int request = game.TakeNeuralRenderingRequest(); request != 0) {
            const int value = request - 1;
            const bool on = value >= 1 && NeuralSupport(app).available && !app.nr_route_failed;
            app.settings.upscaling.neural_rendering = pt::NeuralRenderingKey(on ? pt::NeuralRenderingKind::Dlss5 : pt::NeuralRenderingKind::Off);
            if (on) app.settings.upscaling.neural_style = pt::NeuralStyleKey(static_cast<pt::NeuralStyle>(value - 1));
            ApplyNeuralToScene(app);
            pt::LogInfo("input script: neural rendering {}", on ? app.settings.upscaling.neural_style : std::string("off"));
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
        static int f9_burst_left = 0;
        static int f9_burst_index = 0;
        static std::filesystem::path f9_folder;
        if (polled.capture_pose_pressed) {
            game.LogCapturePose(game.ViewCamera());
            if (app.window && f9_burst_left == 0) {
                const std::time_t now = std::time(nullptr);
                std::tm local{};
#ifdef _WIN32
                localtime_s(&local, &now);
#else
                localtime_r(&now, &local);
#endif
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "f9-%Y%m%d-%H%M%S", &local);
                f9_folder = UserDataDir() / "reports" / stamp;
                std::error_code ec;
                std::filesystem::create_directories(f9_folder, ec);
                if (!app.settings_path.empty()) std::filesystem::copy_file(app.settings_path, f9_folder / "pt.ini", std::filesystem::copy_options::overwrite_existing, ec);
                f9_burst_left = 24;
                f9_burst_index = 0;
                pt::LogInfo("capture report: {} (pt.ini and the next 24 frames)", pt::os::PathToUtf8(f9_folder));
            }
        }
        if (f9_burst_left > 0) {
            game.RequestScreenshot(pt::os::PathToUtf8(f9_folder / std::format("f{:02}.png", f9_burst_index++)));
            --f9_burst_left;
        }
        if (app.window) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            static bool f6_was_down = false;
            static bool f7_was_down = false;
            static bool h_was_down = false;
            static bool p_was_down = false;
            static bool f8_was_down = false;
            static bool street_f_was_down = false;
            if (keys[SDL_SCANCODE_F] && !street_f_was_down && keyboard_free && !(ui_ready && ui.MenuOpen()) && !photo_mode) {
                game.ToggleStreetFlashlight();
            }
            street_f_was_down = keys[SDL_SCANCODE_F];
            if (keys[SDL_SCANCODE_F8] && !f8_was_down && keyboard_free && pt::mods::Active()) {
                game.LogModInspect(game.ViewCamera());
            }
            f8_was_down = keys[SDL_SCANCODE_F8];
            const bool usable = keyboard_free && !show_settings && !(ui_ready && ui.MenuOpen()) && app.extras_request == 0 && !vr;
            const bool f6 = keys[SDL_SCANCODE_F6] && !f6_was_down && usable;
            const bool f7 = keys[SDL_SCANCODE_F7] && !f7_was_down && usable;
            const bool h = keys[SDL_SCANCODE_H] && !h_was_down && usable;
            const bool p = keys[SDL_SCANCODE_P] && !p_was_down && usable && !(ui_ready && ui.SupportNoticeShown());
            f6_was_down = keys[SDL_SCANCODE_F6];
            f7_was_down = keys[SDL_SCANCODE_F7];
            h_was_down = keys[SDL_SCANCODE_H];
            p_was_down = keys[SDL_SCANCODE_P];
            if (freecam && polled.pause) {
                pending_input.pause = false;
                ending_skip_press = false;
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
                    } else if (action == pt::game::PhotoPanel::Action::ResetSettings) {
                        photo_status.clear();
                        photo_status_time = 0.0f;
                        pt::LogInfo("extras: photo settings reset");
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
            static const uint32_t capture_channels = [] {
                const char* v = std::getenv("PT_CAPTURE_CHANNELS");
                const int n = v ? std::atoi(v) : 2;
                return static_cast<uint32_t>(n == 6 || n == 8 ? n : 2);
            }();
            if (!options.audio_capture.empty() && capture_channels != 2) {
                target.resize(base + samples * capture_channels);
                sound.System().RenderOfflineChannels(target.data() + base, samples, capture_channels);
                return;
            }
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
            game.SurvivalAgentInput(state);
            if (!theater) game.UpdateEndingSkip(ending_skip_press, ending_skip_held, step, freecam || photo_mode);
            ending_skip_press = false;
            if (!theater) state = pt::game::GateEndingOutroInput(game.Controller().Step(), state);
            if (freecam) {
                state = FreecamInput(state);
            }
            motion_camera_from = game.ViewCamera();
            scripted_camera = motion_camera_from.position != tick_camera_from.position || motion_camera_from.yaw != tick_camera_from.yaw ||
                              motion_camera_from.pitch != tick_camera_from.pitch || motion_camera_from.roll != tick_camera_from.roll;
            static const char* tick_csv = std::getenv("PT_TICK_CSV");
            const auto tick_t0 = std::chrono::steady_clock::now();
            if (lisa_date) {
                if (ui.MenuOpen() && !ui.MenuSuspended()) {
                } else {
                    if (lisa_date_replay && (state.pause || (state.raw_pressed & pt::kRawCircle))) lisa_date->Leave();
                    lisa_date->Update(step, state);
                    pt::InputState menu_only;
                    menu_only.pause = state.pause && !lisa_date_replay;
                    menu_only.pc_settings = state.pc_settings && !lisa_date_replay;
                    menu_only.prompts = state.prompts;
                    menu_only.pointer = state.pointer;
                    menu_only.pointer_valid = state.pointer_valid;
                    state = menu_only;
                }
            } else if (theater) {
                const bool model = theater->theater->ModelView() || theater->theater->FreeView() || theater->theater->Entry().free_camera;
                const bool back = state.pause || state.cancel || (state.raw_pressed & pt::kRawCircle);
                const bool accept = state.confirm || (state.raw_pressed & pt::kRawCross);
                if (back || (accept && !model)) theater->theater->Stop();
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
            if (!theater && game.SurvivalActive() && !app.options.vr) SurvivalVisions().Update(app.options);
            const auto tick_t3 = std::chrono::steady_clock::now();
            frame_phases.current[FramePhases::kTicks] += std::chrono::duration<float, std::milli>(tick_t3 - tick_t0).count();
            static const bool io_log = std::getenv("PT_IO_LOG") != nullptr;
            if (io_log) {
                if (const pt::IoTotals io = pt::IoTakeMain(); io.reads > 0) {
                    pt::LogInfo("io: frame {}: the main thread read {} files, {:.1f} KB in {:.1f} ms (archive locks {:.1f} ms); tick {:.1f} ms",
                                frame, io.reads, io.bytes / 1024.0, io.read_ms, io.lock_ms,
                                std::chrono::duration<double, std::milli>(tick_t3 - tick_t0).count());
                }
            }
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
            pt::SpendTickInput(pending_input);
            accumulator -= step;
            ++ticks;
        }
        if (ui_ready) ui.AdvancePresentation(paced ? dt : step);
        if (!app.update_polled && app.updates.Done()) {
            app.update_polled = true;
            if (const auto newer = app.updates.Newer()) {
                pt::LogInfo("update: version {} available at {}{}{}", newer->version, newer->url, newer->notes.empty() ? "" : ": ", newer->notes);
                if (newer->download_bytes) pt::LogInfo("update: the in-game update downloads about {} bytes", newer->download_bytes);
                if (newer->signed_update) InGameUpdateHere();
                if (ui_ready && InGameUpdate(*newer) && !options.update_now) {
                    app.update_question = true;
                    ui.AskUpdate();
                    pt::LogInfo("update: asking at the start to update to {}", newer->version);
                } else if (ui_ready) {
                    ui.ShowUpdateNotice(UpdateNoticeKey(*newer));
                }
            }
        }
        if (options.update_now && !app.update_requested && app.updates.Done()) {
            if (const auto newer = app.updates.Newer(); newer && InGameUpdate(*newer)) {
                app.update_requested = true;
                app.update_language = game.Options().subtitle_language;
                game.RequestQuit();
                pt::LogInfo("update: --update-now, updating to {}", newer->version);
            }
        }
        if (app.NrPending() == 0) {
            const pt::nr_unlock::Job::Status job = app.nr_job.Get();
            if (job.serial != app.nr_job_serial) {
                app.nr_job_serial = job.serial;
                if (job.phase == pt::nr_unlock::Job::Phase::Ready) {
                    app.routes.Request(pt::route_queue::Route::Nr,
                                       static_cast<pt::route_queue::Op>(app.nr_job_request ? app.nr_job_request : (job.kind == pt::nr_unlock::Job::Kind::Install ? 1 : 2)));
                    app.update_language = game.Options().subtitle_language;
                    app.nr_row_note.clear();
                    if (ui_ready) ui.ShowNotice(app.NrPending() == 2 ? "pc_notice_nr_remove_ready" : "pc_notice_nr_unlock_ready");
                    pt::LogInfo("neural rendering: the community installer is ready; restart to {}",
                                app.NrPending() == 2 ? "remove" : app.NrPending() == 3 ? "reinstall" : "install");
                } else if (job.phase == pt::nr_unlock::Job::Phase::Failed) {
                    app.nr_row_note = job.failure.empty() ? "pc_note_nr_unlock_failed" : job.failure;
                    pt::LogWarn("neural rendering: the community download failed: {}", job.detail);
                }
            }
        }
        if (const pt::nr_native::Job::Status job = app.nr_native_job.Get(); job.serial != app.nr_native_serial) {
            app.nr_native_serial = job.serial;
            if (job.phase == pt::nr_native::Job::Phase::Done && job.kind == pt::nr_native::Job::Kind::Remove) {
                app.nr_install_usable = false;
                app.nr_native_remove_pending = job.deferred;
                app.settings.upscaling.neural_rendering = "off";
                ApplyNeuralToScene(app);
                Save(app);
                if (ui_ready) ui.ShowNotice(job.deferred ? "pc_note_nr_remove_restart" : "pc_notice_nr_removed");
                pt::LogInfo("neural rendering: DLSS 5 removed{}", job.deferred ? " at the next start" : "");
            } else if (job.phase == pt::nr_native::Job::Phase::Done) {
                pt::nr_native::Startup(NrDataDir(app.settings_path));
                app.nr_install_usable = pt::nr_native::UseCard(app.nr_arch);
                pt::ConfigureNativeNeuralRuntime(app.settings.upscaling.neural_runtime);
                app.nr_route_failed = false;
                app.nr_row_note.clear();
                if (ui_ready) ui.ShowNotice(app.nr_install_usable ? "pc_notice_nr_installed" : "pc_note_nr_unlock_failed");
                pt::LogInfo("neural rendering: DLSS 5 installed and ready{}", app.nr_install_usable ? "" : ", but it does not read back for this card");
            } else if (job.phase == pt::nr_native::Job::Phase::Failed) {
                app.nr_row_note = job.failure;
                pt::LogWarn("neural rendering: the DLSS 5 install failed: {}", job.detail);
            }
        }
        if (pt::nr_native::Active() && app.scene.neural.enabled && !app.nr_route_failed) {
            if (const auto* status = app.scene.NativeNeuralStatus(); status && !status->available && !status->note.empty()) {
                DisableNeuralRoute(app, status->note.c_str(), status->reason);
                if (ui_ready) ui.ShowNotice("pc_notice_nr_failed");
            }
        }
        if (frame >= 1) MarkUpdateStarted();
        SyncNeuralToggle(app);
        if (NeuralRouteActive(app) && app.nr_reshade_loaded && app.nr_route_armed && !app.nr_route_failed) {
            const double now = SDL_GetTicks() / 1000.0;
            if (now - app.nr_route_check_time > 1.0) {
                app.nr_route_check_time = now;
                const pt::nr_unlock::RouteReport rep = pt::nr_unlock::ReadRouteReport(pt::ExecutableDir());
                if (rep.state == pt::nr_unlock::RouteReport::State::Failed) {
                    DisableNeuralRoute(app, "pc_note_nr_create",
                                       std::format("the add-on reported a failure{}{}: {}", rep.code.empty() ? "" : " code ", rep.code, rep.line));
                    if (ui_ready) ui.ShowNotice("pc_notice_nr_failed");
                } else if (rep.state == pt::nr_unlock::RouteReport::State::Running) {
                    ArmNeuralRoute(app, false);
                }
            }
        }
        if (ui_ready && !app.pending_notices.empty()) {
            for (const std::string& notice : app.pending_notices) ui.ShowNotice(notice);
            app.pending_notices.clear();
        }
        {
            static long fg_set_frame = -1;
            static int fg_set_value = 0;
            static bool fg_set_menu = false;
            static bool fg_set_read = false;
            if (!fg_set_read) {
                fg_set_read = true;
                if (const char* v = std::getenv("PT_TEST_SET_FG"); v && std::strchr(v, ':')) {
                    fg_set_frame = std::atol(v);
                    fg_set_value = std::atoi(std::strchr(v, ':') + 1);
                    fg_set_menu = std::strstr(v, ":menu") != nullptr;
                }
            }
            if (fg_set_frame >= 0 && static_cast<long>(frame) >= fg_set_frame && (!fg_set_menu || (ui_ready && ui.MenuOpen()))) {
                pt::LogInfo("frame generation: set to {} by PT_TEST_SET_FG at frame {}{}", fg_set_value, frame, fg_set_menu ? " (menu open)" : "");
                pc_settings.TestSetFrameGeneration(fg_set_value);
                fg_set_frame = -1;
            }
        }
        static const long fg_test_frame = [] { const char* v = std::getenv("PT_TEST_FG_LIVE_UNLOCK"); return v ? std::atol(v) : -1L; }();
        if (fg_test_frame >= 0 && static_cast<long>(frame) == fg_test_frame && app.FgPending() == 0 && !app.fg_job.Busy() && !app.fg_install_usable) {
            const std::filesystem::path work = (app.settings_path.empty() ? UserDataDir() : app.settings_path.parent_path()) / "dlssg-unlock";
            if (app.fg_job.Start(pt::fg_unlock::Job::Kind::Install, work, pt::streamline::ActiveD3D12() ? pt::ExecutableDir() : std::filesystem::path())) {
                app.fg_job_serial = app.fg_job.Get().serial;
                app.fg_job_request = 1;
                pt::LogInfo("frame generation: community unlock started by PT_TEST_FG_LIVE_UNLOCK at frame {}", frame);
            }
        }
        if (app.FgPending() == 0 && app.fg_job_request != 0) {
            const pt::fg_unlock::Job::Status job = app.fg_job.Get();
            if (job.serial != app.fg_job_serial) {
                app.fg_job_serial = job.serial;
                if (job.phase == pt::fg_unlock::Job::Phase::Ready && job.installed_live) {
                    app.renderer.Gpu().WaitIdle();
                    ArmFgRoute(app, true);
                    app.scene.PrepareNgxReload();
                    std::string reason;
                    const bool ready = pt::fg_unlock::LoadLiveProxy(pt::ExecutableDir(), reason) && pt::streamline::ReloadD3D12(reason);
                    ArmFgRoute(app, false);
                    pt::UpscaleHost::Get().SetDlssFrameGenFailed(!ready);
                    app.fg_install = pt::fg_unlock::ReadInstall(pt::ExecutableDir());
                    app.fg_install_usable = app.fg_install && pt::fg_unlock::Usable(*app.fg_install);
                    app.fg_route_failed = !ready;
                    app.fg_row_note = ready ? "" : "pc_note_fg_unlock_failed";
                    app.fg_job_request = 0;
                    if (ui_ready) ui.ShowNotice(ready ? "pc_notice_fg_installed" : "pc_notice_fg_failed");
                    pt::LogInfo("frame generation: live unlock {}{}", ready ? "ready without restarting the game" : "activation failed: ", reason);
                } else if (job.phase == pt::fg_unlock::Job::Phase::Ready) {
                    app.routes.Request(pt::route_queue::Route::Fg, static_cast<pt::route_queue::Op>(app.fg_job_request));
                    app.update_language = game.Options().subtitle_language;
                    app.fg_row_note.clear();
                    if (ui_ready) ui.ShowNotice("pc_notice_fg_unlock_ready");
                    pt::LogInfo("frame generation: the community package is ready; restart to {}",
                                app.FgPending() == 3 ? "reinstall" : "install");
                } else if (job.phase == pt::fg_unlock::Job::Phase::Failed) {
                    app.fg_row_note = job.failure.empty() ? "pc_note_fg_unlock_failed" : job.failure;
                    pt::LogWarn("frame generation: the community download failed: {}", job.detail);
                }
            }
        }
        if (FgRouteActive(app) && app.fg_route_armed && !app.fg_route_failed) {
            const double now = SDL_GetTicks() / 1000.0;
            if (now - app.fg_route_check_time > 1.0) {
                app.fg_route_check_time = now;
                const pt::fg_unlock::RouteReport rep = pt::fg_unlock::ReadRouteReport(pt::ExecutableDir());
                if (rep.state == pt::fg_unlock::RouteReport::State::Fallback) {
                    DisableFgRoute(app, "pc_note_fg_fallback", std::format("the proxy fell back to the stock path: {}", rep.line));
                    if (ui_ready) ui.ShowNotice("pc_notice_fg_failed");
                } else if (rep.state == pt::fg_unlock::RouteReport::State::Active) {
                    ArmFgRoute(app, false);
                }
            }
        }
        if (ui_ready) ui.HoldUpdateNotice(photo_mode || vr != nullptr);
        if (presence_on && frame % 60 == 0) {
            pt::PresenceActivity activity;
            activity.start = presence_start;
            const int step = game.Controller().Step();
            const int loop = game.StreetWalkActive() ? pt::game::kBrowseStreet
                             : game.Floor().IsCurrentFloorName("ending") ? pt::game::kBrowseEnding
                                                                         : game.CurrentLoopIndex();
            if (step < 15 && !game.StreetWalkActive()) {
                activity.details = "In the menus";
            } else if (loop >= 0 && loop < static_cast<int>(pt::game::kBrowseLoops.size())) {
                activity.details = std::string(pt::game::PcText(pt::game::kBrowseLoops[static_cast<size_t>(loop)].label, 0));
            } else {
                activity.details = "In the hallway";
            }
            if (photo_mode) {
                activity.state = "Photo mode";
            } else if (const int tier = std::min(game.GamePlusTier(), 4); tier > 0) {
                activity.state = std::format("New Game+ {}/4", tier);
            }
            pt::PresenceSet(activity);
        }
        if (ui_ready && !vr) {
            if (const int tier = game.GamePlusNoticeDue(); tier > 0) {
                ui.ShowNotice(std::format("pc_gameplus_notice{}{}", pt::game::kPcNoteArgument, tier));
                game.TakeGamePlusNotice(tier);
            }
        }
        game.Speedrun().Poll();
        if (game.SavesEnabled() && !save_progress_read && game.SaveLoads() > 0) {
            save_progress_read = true;
            auto& progress = app.settings.progress;
            bool changed = false;
            if (game.GamePlus() && !progress.game_finished) {
                progress.game_finished = true;
                progress.loops_reached = (1u << pt::game::kBrowseLoops.size()) - 1u;
                game.SetBrowseUnlocks(progress.loops_reached, true);
                pt::LogInfo("loop browser: save has the finished flag, all entries unlocked");
                changed = true;
            }
            if (progress.game_finished && progress.archive_backfill < 1) {
                game.BackfillArchive(std::max<uint32_t>(1, game.Finishes()));
                progress.archive_backfill = 1;
                changed = true;
            }
            if (!progress.night_unlocked && game.Finishes() > static_cast<uint32_t>(pt::game::Game::kTierSurvival)) {
                game.UnlockNight("the save completed Game+ tier 4");
                progress.night_unlocked = true;
                changed = true;
            }
            if (changed) {
                std::string keys;
                for (const std::string& key : game.ArchiveSeen()) keys += (keys.empty() ? "" : ",") + key;
                progress.archive = keys;
                unlocks_seen = game.BrowseUnlockGeneration();
                archive_seen = game.ArchiveGeneration();
                if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
            }
        }
        if (game.SavesEnabled() && (game.BrowseUnlockGeneration() != unlocks_seen || game.ArchiveGeneration() != archive_seen)) {
            unlocks_seen = game.BrowseUnlockGeneration();
            archive_seen = game.ArchiveGeneration();
            app.settings.progress.loops_reached = game.BrowseReached();
            app.settings.progress.game_finished = game.GameFinished();
            if (game.NightUnlockKept() && !app.settings.progress.night_unlocked && !app.settings_path.empty()) {
                pt::LogInfo("loop browser: the survival night's unlock written to pt.ini ([progress] night_unlocked = 1)");
            }
            app.settings.progress.night_unlocked = game.NightUnlockKept();
            std::string keys;
            for (const std::string& key : game.ArchiveSeen()) keys += (keys.empty() ? "" : ",") + key;
            app.settings.progress.archive = keys;
            if (!app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
        }
        if (const auto profile = game.TakeSurvivalProfile(); profile && *profile != app.settings.game.survival_profile) {
            app.settings.game.survival_profile = *profile;
            if (game.SavesEnabled() && !app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
        }
        if (const int record = game.TakeSurvivalRecord(); record > app.settings.game.survival_best_wave) {
            app.settings.game.survival_best_wave = record;
            if (game.SavesEnabled() && !app.settings_path.empty()) pt::SaveAppSettings(app.settings_path, app.settings);
        }
        if (lisa_date && game.TakeLeaveLisaDateRequest()) lisa_date->Leave();
        {
            static bool menu_over_date = false;
            const bool over = lisa_date && ui_ready && ui.MenuOpen() && !ui.MenuSuspended();
            if (menu_over_date && !over && lisa_date) {
                if (game.Audio()) game.Audio()->PostEvent("Pause_All", nullptr);
                lisa_date->RestartMusic();
            }
            menu_over_date = over;
        }
        if (lisa_date && lisa_date->Finished()) end_lisa_date();
        if (std::optional<std::string> cause = game.TakeLisaDateRequest()) start_lisa_date(*cause, false);
        game.SetArchiveTheaterShowing(theater && theater->theater->Showing());
        if (theater && theater->theater->Finished()) {
            end_theater();
        }
        if (std::string id = std::exchange(app.archive_request, std::string()); id == "coffee_lisa") {
            start_lisa_date("the Museum", true);
        } else if (!id.empty()) {
            start_theater(id);
        } else if (std::string script_id = game.TakeArchiveRequest(); !script_id.empty()) {
            if (script_id == "coffee_lisa") start_lisa_date("the Museum", true);
            else start_theater(script_id);
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
        const auto camera_controls = [&]() {
            const bool* keys = app.window ? SDL_GetKeyboardState(nullptr) : nullptr;
            const auto key = [&](SDL_Scancode code) { return keys && keyboard_free && keys[code]; };
            pt::FreeCameraControls controls;
            controls.movement = polled.left_stick;
            controls.mouse_look = polled.mouse_look;
            controls.right_stick = polled.right_stick;
            controls.fast = key(SDL_SCANCODE_LSHIFT) || key(SDL_SCANCODE_RSHIFT) || (polled.raw_held & pt::kRawL3);
            if (key(SDL_SCANCODE_SPACE) || (polled.raw_held & pt::kRawR2)) controls.rise += 1.0f;
            if (key(SDL_SCANCODE_LCTRL) || (polled.raw_held & pt::kRawL2)) controls.rise -= 1.0f;
            return controls;
        };
        if (theater && (theater->theater->ModelView() || theater->theater->FreeView())) theater->theater->UpdateModelCamera(dt, camera_controls());
        if (freecam) {
            const float speed_scale = photo_mode ? photo_panel.Settings().SpeedScale() : 1.0f;
            pt::AdvanceFreeCamera(freecam_camera, camera_controls(), dt, game.Options().invert_x, game.Options().invert_y, speed_scale);
            if (photo_mode) pt::LimitFreeCameraRange(freecam_camera, freecam_origin, kFreecamRange);
            if (photo_mode) {
                const pt::game::PhotoSettings& photo = photo_panel.Settings();
                freecam_camera.fov_y = glm::radians(pt::game::PhotoFovYDegrees(photo.focal_length_mm));
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
            pt::audio::MotionLevels motion = sound.Ready() ? sound.System().Motion() : pt::audio::MotionLevels{};
            if (game.Captions().Enabled()) {
                const auto [large, small] = game.Captions().Pulse(dt);
                motion.large_motor = std::max(motion.large_motor, large);
                motion.small_motor = std::max(motion.small_motor, small);
            }
            input.SetRumble(motion.large_motor, motion.small_motor);
            input.SetTriggerRumble(motion.large_motor, motion.small_motor);
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

        const bool voice_key_tapped = voice_key_press.Consume();
        if ((voice_key_tapped || polled.voice_keyword_pressed) && (app.window || options.virtual_pads) && voice_key != SDL_SCANCODE_UNKNOWN &&
            game.VoiceListening() && !game.Paused() && !ui.MenuOpen() && !show_settings && !show_debug && !app.microphone_test) {
            pt::LogInfo("voice: fallback Jack from {}", polled.voice_keyword_pressed ? "controller" : "keyboard");
            game.OnVoiceKeyword("jack");
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
            bool microphone_opening = false;
            if (!microphone.IsOpen() && !microphone_failed && !voice_file) {
                const auto state = microphone.OpenAsync(pt::VoiceRecognizer::kSampleRate, app.settings.voice.device);
                microphone_opening = state == pt::Microphone::AsyncState::Pending;
                if (state != pt::Microphone::AsyncState::Pending) {
                    microphone_failed = state == pt::Microphone::AsyncState::Failed;
                    recognizer->Reset();
                    mic_quiet_seconds = 0.0f;
                    if (microphone_failed) pt::LogError("voice: no microphone to listen with");
                }
            }
            const pt::VoiceRecognizer::State state = recognizer->GetState();
            if (microphone_opening) {
                app.microphone_status = "pc_mic_waiting";
                app.microphone_reason = "pc_mic_st_loading";
            } else if (microphone_failed) {
                app.microphone_status = "pc_mic_unavailable";
                app.microphone_reason = "pc_mic_st_nomic";
            } else if (state == pt::VoiceRecognizer::State::Failed) {
                using Failure = pt::VoiceRecognizer::Failure;
                switch (recognizer->GetFailure()) {
                case Failure::Files: app.microphone_status = "pc_mic_err_files"; app.microphone_reason = "pc_mic_st_files"; break;
                case Failure::Cpu: app.microphone_status = "pc_mic_err_cpu"; app.microphone_reason = "pc_mic_st_cpu"; break;
                case Failure::Model: app.microphone_status = "pc_mic_err_model"; app.microphone_reason = "pc_mic_st_model"; break;
                default: app.microphone_status = "pc_mic_err_runtime"; app.microphone_reason = "pc_mic_st_runtime"; break;
                }
            } else if (state == pt::VoiceRecognizer::State::Ready) {
                const bool silent = mic_quiet_seconds >= 3.0f && app.microphone_hypothesis.empty();
                app.microphone_status = silent ? "pc_mic_err_silent" : "pc_mic_say_jack";
                app.microphone_reason = silent ? "pc_mic_st_silent" : "pc_mic_no_word";
            } else {
                app.microphone_status = "pc_mic_waiting";
                app.microphone_reason = "pc_mic_st_loading";
            }
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
                if (mic_samples.empty()) mic_quiet_seconds += static_cast<float>(dt);
                else mic_quiet_seconds = loudest <= -75.0f ? mic_quiet_seconds + static_cast<float>(dt) : 0.0f;
                bool heard = recognizer->Feed(mic_samples);
                if (voice_file && !voice_input_live) heard = recognizer->Drain() || heard;
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
        if (ui_ready) {
            const auto s = ui.SubliminalString();
            app.scene.SetSubliminalString({s.texture, s.position, s.value, s.ink});
        }
        app.renderer.plain_output = ui.BootSplashShown() || ui.ShaderScreenShown();
        draw_items.clear();
        pt::Camera camera = game.ViewCamera();
        const bool date_scene = lisa_date && lisa_date->ShowingScene();
        if (date_scene != lisa_date_was_showing) {
            lisa_date_was_showing = date_scene;
            app.scene.ResetUpscaleHistory();
        }
        app.scene.SetBackdrop(date_scene ? lisa_date->Backdrop() : pt::SceneRenderer::Backdrop{});
        app.scene.SetDateResolve(date_scene, date_scene && lisa_date->SceneStill());
        if (date_scene) {
            lisa_date->CollectDraws(draw_items);
            camera = lisa_date->ViewCamera();
        } else if (theater) {
            if (theater->theater->Showing()) {
                theater->theater->CollectDraws(draw_items);
            } else {
                app.renderer.fade[0] = app.renderer.fade[1] = app.renderer.fade[2] = 0.0f;
                app.renderer.fade[3] = 1.0f;
            }
            camera = theater->theater->ViewCamera();
        } else {
            game.SetTracedBody(app.scene.raytracing.path_traced);
            game.CollectDraws(draw_items);
            IntoViewFrame(draw_items, game.ViewCamera());
        }
        float blend = 1.0f;
        if (paced && !theater && !date_scene) {
            const float t = std::clamp(accumulator / step, 0.0f, 1.0f);
            bool cut = false;
            if (tick_blend >= 1) {
                camera = pt::BlendCameras(tick_camera_from, camera, t, &cut);
            }
            blend = cut || tick_blend < 2 ? 1.0f : t;
            BlendDraws(tick_state, draw_items, blend, blended_skins);
            tick_state.lights.t = blend;
        }
        const pt::game::TickBlend* light_blend = paced && !theater && !date_scene ? &tick_state.lights : nullptr;
        if (paced && !theater && !date_scene && !freecam && !pt::game::EndingOutroInputBlocked(game.Controller().Step()) &&
            polled.prompts.device == pt::PromptDevice::Keyboard && !scripted_camera &&
            !game.Demos().ControlsPlayer() && !game.Demos().HasActiveCamera() && !game.Paused() && !(game.GetPlayer().locks.Mask('B') & 2)) {
            const pt::Camera unturned = camera;
            camera = pt::game::RenderMouseLook(camera, game.ViewCamera(), pending_input.mouse_look,
                                              game.Options().invert_x, game.Options().invert_y);
            if (const glm::mat4 turn = game.ThirdPersonBodyTurn(unturned, camera); turn != glm::mat4(1.0f) && !ViewAttachOff()) {
                for (pt::DrawItem& item : draw_items) {
                    if (item.turns_with_view) item.transform = turn * item.transform;
                }
            }
            camera = game.ThirdPersonTurn(unturned, camera);
            camera = game.PeepholeTurn(camera);
        }
        OntoView(draw_items, camera);
        if (static const bool attach_trace = std::getenv("PT_VIEW_ATTACH_TRACE") != nullptr; attach_trace) {
            const bool any_attached = std::any_of(draw_items.begin(), draw_items.end(), [](const pt::DrawItem& i) { return i.view_attached; });
            for (const pt::DrawItem& item : draw_items) {
                if (any_attached ? !item.view_attached : !item.turns_with_view) continue;
                const glm::vec3 f = glm::normalize(glm::vec3(item.transform[2]));
                const float right = glm::degrees(std::asin(std::clamp(glm::dot(f, camera.Right()), -1.0f, 1.0f)));
                const float up = glm::degrees(std::asin(std::clamp(glm::dot(f, camera.Up()), -1.0f, 1.0f)));
                pt::LogInfo("attach trace: frame {} blend {:.3f} camera yaw {:.4f} gun off right {:.3f} up {:.3f}", frame, blend, camera.yaw, right, up);
                break;
            }
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
            app.renderer.ImGuiNewFrame();
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
        VkExtent2D base_render_extent{};
        if (!vr && app.window) {
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(app.window, &width, &height);
            const int mode = WindowMode(app);
            const glm::ivec2 setting(app.settings.display.width, app.settings.display.height);
            const bool minimized = (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED) != 0;
            const glm::ivec2 size = pt::display::RenderSize(mode, {width, height}, setting, minimized);
            if (size.x > 0 && size.y > 0) base_render_extent = {static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)};
            static glm::ivec4 mismatch_logged(0);
            const glm::ivec4 mismatch = mode == pt::display::kExclusive && !minimized && size != setting ? glm::ivec4(size, setting) : glm::ivec4(0);
            if (mismatch != mismatch_logged) {
                mismatch_logged = mismatch;
                if (mismatch != glm::ivec4(0)) {
                    pt::LogInfo("display: the window did not get the exclusive mode {}x{} ({}x{} pixels{}): frames render at the window's size",
                                setting.x, setting.y, size.x, size.y,
                                (SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN) ? "" : ", no fullscreen flag");
                }
            }
        } else if (!vr && options.headless) {
            base_render_extent = {options.width, options.height};
        }
        app.renderer.SetRenderExtent(base_render_extent);
        if (photo_mode) {
            const int language = std::clamp(game.Options().subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1);
            photo_status_time = std::max(0.0f, photo_status_time - dt);
            if (photo_status_time <= 0.0f) photo_status.clear();
            const VkExtent2D preview = base_render_extent.width ? base_render_extent : pt::gpu::vk::Native(app.renderer.RenderExtent());
            photo_view = photo_panel.View(language, photo_status, static_cast<float>(preview.width) / std::max(1u, preview.height));
            photo_view.panel = photo_overlay && !photo_shot;
            if (photo_shot) photo_view.grid = 0;
            if (photo_shot) {
                const std::filesystem::path path = PhotoPath();
                game.RequestScreenshot(pt::os::PathToUtf8(path));
                std::string saved(pt::game::PcText("pc_photo_saved", language));
                if (const size_t at = saved.find("{file}"); at != std::string::npos) saved.replace(at, 6, pt::os::PathToUtf8(path.filename()));
                photo_status = saved;
                photo_status_time = 4.0f;
                photo_shot = false;
            }
        }
        std::vector<std::string> shots = game.TakeScreenshotRequests();
        static const auto drawn_shots = [] {
            struct Range { uint64_t first = 1; uint64_t last = 0; std::string folder; } range;
            if (const char* v = std::getenv("PT_DRAWN_SHOTS")) {
                char folder[1024] = {};
                unsigned long long first = 0, last = 0;
                if (std::sscanf(v, "%llu %llu %1023[^\n]", &first, &last, folder) == 3) range = {first, last, folder};
            }
            return range;
        }();
        if (frame >= drawn_shots.first && frame <= drawn_shots.last) {
            shots.push_back(std::format("{}/drawn_{:05}.png", drawn_shots.folder, frame));
        }
        std::string lisa_photo;
        glm::vec4 lisa_photo_crop(0.0f, 0.0f, 1.0f, 1.0f);
        if (lisa_date) {
            if (const std::optional<glm::vec4> crop = lisa_date->TakePhotoRequest()) {
                lisa_photo = pt::os::PathToUtf8(PhotoPath());
                lisa_photo_crop = *crop;
                shots.push_back(lisa_photo);
            }
        }
        auto lisa_photo_saved = [&](const std::string& shot, bool ok) {
            if (!lisa_photo.empty() && shot == lisa_photo && lisa_date) {
                lisa_date->PhotoSaved(pt::os::PathToUtf8(pt::os::PathFromUtf8(shot).filename()), ok);
                lisa_photo.clear();
            }
        };
        app.renderer.photo_filter = photo_mode ? static_cast<int>(photo_panel.Settings().filter) : 0;
        {
            int pattern = 0;
            if (const pt::game::GameUi* ui = pt::game::GameUi::Active();
                ui && ui->Menu().CurrentState() == pt::game::OptionsMenu::State::Active && ui->Menu().CurrentPage() == pt::game::OptionsMenu::Page::Pc &&
                ui->Menu().PcPage().ScreenShown()) {
                pattern = ui->Menu().PcPage().Screen().pattern;
            }
            app.renderer.SetHdrPattern(pattern);
        }
        const bool photo_capture = photo_mode && !shots.empty() && !vr;
        const bool photo_four_k = photo_capture && photo_panel.Settings().resolution == pt::game::PhotoResolution::FourK;
        const VkExtent2D preview_extent = base_render_extent.width ? base_render_extent : pt::gpu::vk::Native(app.renderer.RenderExtent());
        const float preview_aspect = static_cast<float>(preview_extent.width) / std::max(1u, preview_extent.height);
        const auto photo_crop = photo_capture ? pt::game::PhotoCropForAspect(preview_aspect, photo_panel.Settings().AspectRatio(preview_aspect)) : pt::game::PhotoCropRect{};
        struct RestorePhotoRender {
            pt::Renderer& renderer;
            pt::UpscaleSettings& current;
            pt::UpscaleSettings saved;
            VkExtent2D base;
            bool active;
            ~RestorePhotoRender() { if (active) { renderer.SetRenderExtent(base); current = saved; } }
        } restore_photo{app.renderer, app.scene.upscale, app.scene.upscale, base_render_extent, photo_four_k};
        if (photo_four_k) {
            const auto extent = pt::game::PhotoCaptureExtent(preview_extent.width, preview_extent.height,
                static_cast<pt::game::PhotoAspectPreset>(photo_panel.Settings().aspect), pt::game::PhotoResolution::FourK);
            app.renderer.SetRenderExtent({extent.width, extent.height});
            app.scene.upscale.kind = pt::UpscalerKind::Off;
            app.scene.upscale.frame_generation = pt::FrameGenKind::Off;
            camera.fov_y = 2.0f * std::atan(std::tan(camera.fov_y * 0.5f) * photo_crop.height);
            photo_view.crop = {};
        }
        const bool bug_still = ui_ready && ui.DemoGraphs().BugScreenStill();
        if (static bool logged_still = false; bug_still != logged_still) {
            logged_still = bug_still;
            pt::LogInfo("ui: bug screen {} at frame {}", bug_still ? "still, presenting stops" : "released", frame);
        }
        const bool still_screen = app.window && !vr && bug_still;
        const bool render = !still_screen && (app.window || options.render_all || frame + 1 >= static_cast<uint64_t>(options.frames) || !shots.empty());
        if (still_screen) {
            SDL_Delay(4);
        }
        ++frame;
        pt::LogSetTick(frame);
        app.scene.resumed_threshold_artifact = view.ResumedThresholdArtifact();
        app.scene.AdvanceTime(dt);
        bool warmed = false;
        if (vr) {
            const bool screen = vr->ScreenMode(game);
            const bool menu_open = ui_ready && ui.MenuOpen();
            const float scene_dt = static_cast<float>(game.Time() - last_scene_time);
            auto begin = [&](bool present) { return app.renderer.BeginFrame(present && app.window) || (present && app.renderer.BeginFrame(false)); };
            auto save_shots = [&](const char* suffix) {
                for (const std::string& shot : shots) {
                    if (shot == lisa_photo) {
                        if (!*suffix) lisa_photo_saved(shot, app.renderer.SaveScreenshot(pt::os::PathFromUtf8(shot), lisa_photo_crop));
                        continue;
                    }
                    std::filesystem::path path = pt::os::PathFromUtf8(shot);
                    if (*suffix) path = path.parent_path() / pt::os::PathFromUtf8(pt::os::PathToUtf8(path.stem()) + suffix + pt::os::PathToUtf8(path.extension()));
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
                    if (date_scene) {
                        static pt::vfx::RenderList no_effects;
                        vfx_pass.Submit(no_effects);
                        lisa_date->ApplyLighting(lighting);
                    } else {
                        vfx_scene.Prepare(game, camera, 16.0f / 9.0f, lighting, vfx_pass, blend);
                    }
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
        {
            static std::string adapted_floor;
            const std::string& floor_name = game.Floor().CurrentFloorName();
            if (floor_name != adapted_floor && game.Controller().Step() == 15) {
                if (!adapted_floor.empty()) app.scene.SettleExposureClock();
                adapted_floor = floor_name;
            }
        }
        if (render && !app.window && !shots.empty() && !vr) {
            if (options.shot_settle && last_render_frame + 1 != frame) {
                app.scene.ResetExposure();
            }
            for (int i = 0; i < options.shot_warmup && app.renderer.BeginFrame(); ++i) {
                warmed = true;
                view_builder.Build(view, camera, 0.0f, lighting, light_blend);
                const VkExtent2D warm_extent = pt::gpu::vk::Native(app.renderer.RenderExtent());
                const float warm_aspect = static_cast<float>(warm_extent.width) / static_cast<float>(std::max(1u, warm_extent.height));
                if (date_scene) {
                    static pt::vfx::RenderList no_effects;
                    vfx_pass.Submit(no_effects);
                    lisa_date->ApplyLighting(lighting);
                } else {
                    view_vfx.Prepare(view, camera, warm_aspect, lighting, vfx_pass, blend);
                }
                if (theater) theater->theater->ApplyModelLighting(lighting);
                const bool armory_studio = ui_ready && ui.MenuOpen() && ui.Menu().CurrentPage() == pt::game::OptionsMenu::Page::Pc && pc_settings.ArmoryPreviewActive();
                if (armory_studio) {
                    static pt::vfx::RenderList no_effects;
                    vfx_pass.Submit(no_effects);
                    pt::Camera studio_cam;
                    std::vector<pt::DrawItem> studio_draws;
                    pt::SceneLighting studio_lighting;
                    pc_settings.BuildArmoryStudio(0.0f, studio_cam, studio_draws, studio_lighting);
                    app.scene.SetMotionReference(studio_cam, 1.0f / options.tick_rate, i > 0 || last_render_frame + 1 == frame);
                    app.scene.Render(studio_cam, studio_draws, studio_lighting, dt);
                } else {
                    app.scene.SetMotionReference(camera, 1.0f / options.tick_rate, i > 0 || last_render_frame + 1 == frame);
                    app.scene.Render(camera, draw_items, lighting, dt);
                }
                take_reflection_readback(view);
                app.renderer.EndFrame(false);
                last_render_frame = frame - 1;
            }
        }
        if (render && !vr) {
            static const char* parts_csv = std::getenv("PT_PARTS_CSV");
            using parts_ms = std::chrono::duration<float, std::milli>;
            const auto part_t0 = std::chrono::steady_clock::now();
            if (frozen_since.time_since_epoch().count() != 0) {
                frozen_seconds += std::chrono::duration<double>(part_t0 - frozen_since).count();
                frozen_since = {};
            }
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
            const VkExtent2D vfx_extent = pt::gpu::vk::Native(app.renderer.RenderExtent());
            if (date_scene) {
                static pt::vfx::RenderList no_effects;
                vfx_pass.Submit(no_effects);
                lisa_date->ApplyLighting(lighting);
            } else {
                view_vfx.Prepare(view, camera, static_cast<float>(vfx_extent.width) / static_cast<float>(std::max(1u, vfx_extent.height)), lighting,
                                 vfx_pass, blend);
            }
            if (theater) theater->theater->ApplyModelLighting(lighting);
            const bool view_distortion = view_vfx.ViewDistortion();
            static bool was_distorting = false;
            if (view_distortion != was_distorting) {
                was_distorting = view_distortion;
                pt::LogInfo("upscale: view distortion {} (history reset, frame reactive while on)", view_distortion ? "on" : "off");
                app.scene.ResetUpscaleHistory();
            }
            app.scene.SetUpscaleReactiveFrame(view_distortion);
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
                const bool snapped = scripted_camera && (glm::dot(motion_camera_from.Forward(), tick_camera_from.Forward()) < 0.98481f ||
                                                         glm::distance(motion_camera_from.position, tick_camera_from.position) > 0.25f);
                const pt::Camera& reference =
                    warmed ? camera : options.render_all ? (snapped ? camera : tick_camera_from) : scripted_camera ? camera : motion_camera_from;
                app.scene.SetMotionReference(reference, demo_scale / options.tick_rate, last_render_frame + 1 == frame);
            }
            if (const char* path = std::getenv("PT_WALK_MAP"); path && !shots.empty()) {
                static bool walked = false;
                if (!walked) {
                    walked = true;
                    pt::game::DumpStreetWalk(game, path);
                }
            }
            if (const char* path = std::getenv("PT_DRAW_DUMP"); path && !shots.empty()) {
                app.scene.DumpDraws(draw_items, std::string(path) + std::to_string(frame) + ".bin");
            }
            static const bool dump_targets = std::getenv("PT_TARGET_DUMP") != nullptr;
            if (dump_targets && !shots.empty()) {
                app.scene.RequestTargetDump();
            }
            const auto part_t4 = std::chrono::steady_clock::now();
            const bool armory_studio = ui_ready && ui.MenuOpen() && ui.Menu().CurrentPage() == pt::game::OptionsMenu::Page::Pc && pc_settings.ArmoryPreviewActive();
            if (armory_studio) {
                static pt::vfx::RenderList no_effects;
                vfx_pass.Submit(no_effects);
                pt::Camera studio_cam;
                std::vector<pt::DrawItem> studio_draws;
                pt::SceneLighting studio_lighting;
                pc_settings.BuildArmoryStudio(static_cast<float>(dt), studio_cam, studio_draws, studio_lighting);
                app.scene.SetMotionReference(studio_cam, 1.0f / options.tick_rate, last_render_frame + 1 == frame);
                app.scene.Render(studio_cam, studio_draws, studio_lighting, dt);
            } else {
                app.scene.Render(camera, draw_items, lighting, dt);
            }
            take_reflection_readback(view);
            last_render_frame = frame;
            const auto part_t5 = std::chrono::steady_clock::now();
            app.renderer.EndFrame(app.window != nullptr);
            if (!first_frame_shown) {
                first_frame_shown = true;
                pt::boot::Phase("first frame", ui_ready && ui.ShaderScreenShown()     ? "the shader preparation screen"
                                               : ui_ready && ui.BootSplashShown()     ? "the title picture"
                                                                                      : "the loading screen");
            }
#ifdef _WIN32
            static bool present_filter_logged = false;
            if (!present_filter_logged && app.window && (frame % 600 == 0 || frame < 2) && GetModuleHandleW(L"NvPresent64.dll")) {
                present_filter_logged = true;
                pt::LogInfo("nvidia: NvPresent64.dll is in the process (frame {}): a driver present filter is on (RTX HDR, RTX Dynamic "
                            "Vibrance or Smooth Motion); with RTX HDR the driver tone maps the game's SDR picture again. The game's own "
                            "HDR output is Display > HDR",
                            frame);
            }
#endif
            {
                const pt::Renderer::CpuWaits& waits = app.renderer.LastCpuWaits();
                auto& c = frame_phases.current;
                c[FramePhases::kFence] = waits.fence_ms;
                c[FramePhases::kSleep] = waits.sleep_ms;
                c[FramePhases::kAcquire] = waits.acquire_ms;
                c[FramePhases::kBuild] = parts_ms(part_t2 - part_t1).count();
                c[FramePhases::kVfx] = parts_ms(part_t3 - part_t2).count();
                c[FramePhases::kRecord] = parts_ms(part_t5 - part_t4).count();
                c[FramePhases::kEndRecord] = waits.end_record_ms;
                c[FramePhases::kSubmit] = waits.submit_ms;
                c[FramePhases::kPresent] = waits.present_ms;
                static const bool phase_log = app.window || std::getenv("PT_STATUS_LOG") != nullptr;
                frame_phases.EndFrame(frame, app.scene.Stats().gpu_ms, phase_log);
            }
            if (parts_csv) {
                parts_row[3] = parts_ms(part_t5 - part_t4).count();
                parts_last_end = std::chrono::steady_clock::now();
                parts_row[4] = parts_ms(parts_last_end - part_t5).count();
                parts_trace.rows.push_back(parts_row);
            }
            for (const std::string& shot : shots) {
                if (shot == lisa_photo) {
                    lisa_photo_saved(shot, app.renderer.SaveScreenshot(pt::os::PathFromUtf8(shot), lisa_photo_crop));
                    continue;
                }
                const glm::vec4 crop = photo_capture && !photo_four_k ? glm::vec4(photo_crop.x,photo_crop.y,photo_crop.width,photo_crop.height) : glm::vec4(0,0,1,1);
                const std::string f9_prefix = f9_folder.empty() ? std::string() : pt::os::PathToUtf8(f9_folder);
                const bool f9_frame = !f9_prefix.empty() && shot.starts_with(f9_prefix);
                const bool hdr_copy = !f9_frame || shot.ends_with("f00.png");
                if (!app.renderer.SaveScreenshot(pt::os::PathFromUtf8(shot),crop,f9_frame,hdr_copy) && photo_capture) {
                    photo_status = std::string(pt::game::PcText("pc_photo_save_failed", std::clamp(game.Options().subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1)));
                    photo_status_time=4.0f;
                }
                if (dump_targets) {
                    const std::filesystem::path path = pt::os::PathFromUtf8(shot);
                    app.scene.DumpTargets(pt::os::PathToUtf8((path.parent_path() / path.stem())));
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
                    if (l.has_area) {
                        const glm::mat4& m = l.area_to_box;
                        pt::LogInfo("shot light {} area mode {} rows ({:.5f} {:.5f} {:.5f} {:.5f}) ({:.5f} {:.5f} {:.5f} {:.5f}) ({:.5f} {:.5f} {:.5f} {:.5f})", i,
                                    static_cast<int>(l.area_clip_mode), m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1],
                                    m[0][2], m[1][2], m[2][2], m[3][2]);
                    }
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
                app.renderer.Gpu().DeviceLocalUsage(guard_vram_used, guard_vram_budget);
                guard_vram_used /= 1048576.0;
                guard_vram_budget /= 1048576.0;
                static bool guard_low = false;
                const bool low = pt::MemoryLow(1536.0, guard_vram_used, guard_vram_budget, 0.95);
                if (low != guard_low) {
                    guard_low = low;
                    game.Stages().SetPrefetchAllowed(!low);
                    if (!low) {
                        pt::LogInfo("memory: room again, backing off ends");
                    } else {
                        app.renderer.Context().LogMemoryReport("memory low");
                    }
                }
            }
        }
        if (ui_ready) {
            static auto fps_start = std::chrono::steady_clock::now();
            static uint64_t fps_frames = 0;
            static int fps_shown = 0;
            static pt::streamline::FrameGenCounters last_dlss_counts;
            static double dlss_ratio = 1.0;
            ++fps_frames;
            const auto fps_now = std::chrono::steady_clock::now();
            const double fps_seconds = std::chrono::duration<double>(fps_now - fps_start).count();
            if (fps_seconds >= 0.5) {
                fps_shown = static_cast<int>(std::lround(static_cast<double>(fps_frames) / fps_seconds));
                fps_frames = 0;
                fps_start = fps_now;
                const auto counts = pt::streamline::FrameGenStatistics();
                const uint64_t rendered = counts.rendered >= last_dlss_counts.rendered ? counts.rendered - last_dlss_counts.rendered : 0;
                const uint64_t presented = counts.presented >= last_dlss_counts.presented ? counts.presented - last_dlss_counts.presented : 0;
                dlss_ratio = rendered ? std::max(1.0, static_cast<double>(presented) / rendered) : 1.0;
                last_dlss_counts = counts;
            }
#if defined(_WIN32)
            bool generating = app.renderer.FrameGeneratingD3D12();
#else
            bool generating = false;
#endif
            for (pt::FrameGeneration* fg : {pt::UpscaleHost::Get().FrameGen(), pt::UpscaleHost::Get().XefgFrameGen(),
                                            pt::UpscaleHost::Get().MetalFxFrameGen(),
                                            pt::streamline::Active() ? pt::UpscaleHost::Get().DlssFrameGenImpl() : nullptr}) {
                generating = generating || (fg && fg->Generating());
            }
            const int presented_fps = !generating ? fps_shown : app.scene.upscale.frame_generation == pt::FrameGenKind::Dlss
                ? static_cast<int>(std::lround(fps_shown * dlss_ratio)) : fps_shown * 2;
            ui.SetFpsCounter(app.settings.accessibility.fps_counter, fps_shown, presented_fps);
            ui.SetSupportPrompts(app.settings.ui.support_prompts);
            if (std::string from; ui.TakeSupportRequest(&from)) OpenPatreonPage(app, from);
        }
        static const bool status_log = app.window || std::getenv("PT_STATUS_LOG") != nullptr;
        if (status_log) {
            static auto status_start = std::chrono::steady_clock::now();
            static uint64_t status_frames = 0;
            static double status_gpu = 0.0;
            static std::array<double, 6> status_pass{};
            static std::array<double, 6> status_light{};
            static std::array<double, 4> status_rt{};
            static std::array<double, 4> status_path{};
            ++status_frames;
            status_gpu += app.scene.Stats().gpu_ms;
            for (size_t i = 0; i < 6; ++i) {
                status_pass[i] += app.scene.Stats().pass_ms[i];
                status_light[i] += app.scene.Stats().light_ms[i];
            }
            for (size_t i = 0; i < 4; ++i) {
                status_rt[i] += app.scene.Stats().rt_ms[i];
                status_path[i] += app.scene.Stats().pt_ms[i];
            }
            const auto status_now = std::chrono::steady_clock::now();
            const double status_seconds = std::chrono::duration<double>(status_now - status_start).count();
            if (status_frames == 1 || status_seconds >= 10.0) {
                LogDisplayPacing(app);
            }
            if (status_seconds >= 10.0) {
                double used = 0.0;
                double budget = 0.0;
                app.renderer.Gpu().DeviceLocalUsage(used, budget);
                static const bool vram_report = std::getenv("PT_VRAM_REPORT") != nullptr;
                if (vram_report) app.renderer.Context().LogMemoryReport(game.Floor().CurrentFloorName().c_str());
                const VkExtent2D render_extent = pt::gpu::vk::Native(app.renderer.RenderExtent());
                const std::string cpu_status = frame_phases.StatusAndReset();
                const double n = static_cast<double>(std::max<uint64_t>(status_frames, 1));
                std::string passes = std::format(" (shadows {:.2f}, mirror {:.2f}, gbuffer {:.2f}, lighting {:.2f} [velocity {:.2f}, occlusion "
                                                       "{:.2f}, rt ao {:.2f}, probes {:.2f}, lights {:.2f}, sss {:.2f}], compose {:.2f}, post {:.2f})",
                                                       status_pass[0] / n, status_pass[1] / n, status_pass[2] / n, status_pass[3] / n,
                                                       status_light[0] / n, status_light[1] / n, status_light[2] / n, status_light[3] / n,
                                                       status_light[4] / n, status_light[5] / n, status_pass[4] / n, status_pass[5] / n);
                std::string traced;
                if (status_rt[0] + status_rt[1] + status_rt[2] + status_rt[3] > 0.0)
                    traced += std::format(", ultra tier [classify {:.2f}, bounce light {:.2f}, glossy {:.2f}, resolve {:.2f}]", status_rt[0] / n,
                                          status_rt[1] / n, status_rt[2] / n, status_rt[3] / n);
                if (status_path[0] + status_path[1] + status_path[2] + status_path[3] > 0.0)
                    traced += std::format(", path traced [direct {:.2f}, indirect {:.2f}, denoise {:.2f}, layers {:.2f}]", status_path[0] / n,
                                          status_path[1] / n, status_path[2] / n, status_path[3] / n);
                if (!traced.empty()) passes.insert(passes.size() - 1, traced);
                const double active_seconds = std::max(status_seconds - frozen_seconds, 0.001);
                const std::string frozen_note = frozen_seconds > 0.05 ? std::format(", {:.1f} s frozen in the background (not counted)", frozen_seconds) : "";
                frozen_seconds = 0.0;
                pt::LogInfo("status: {:.0f} fps, gpu {:.2f} ms{}, vram {:.0f} of {:.0f} MB, {}x{}, upscaler {} {}, frame generation {}, vsync {}, "
                            "fps limit {}, floor {}{}{}{}{}",
                            status_frames / active_seconds, status_gpu / n, passes, used / 1048576.0,
                            budget / 1048576.0, render_extent.width, render_extent.height, app.settings.upscaling.upscaler,
                            app.settings.upscaling.quality, app.settings.upscaling.frame_generation,
                            app.settings.display.vsync ? "on" : "off", app.settings.display.fps_limit, game.Floor().CurrentFloorName(),
                            ui_ready && ui.MenuOpen() ? ", menu open" : "", frozen_note, cpu_status.empty() ? "" : "; ", cpu_status);
                ScanThirdPartyTools();
                status_start = status_now;
                status_frames = 0;
                status_gpu = 0.0;
                status_pass = {};
                status_light = {};
                status_rt = {};
                status_path = {};
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
                pt::streamline::SetFrameLimit(reflex_paced && cap > 0 ? static_cast<uint32_t>(1000000 / cap) : 0u);
                static uint64_t streamline_frames = 0;
                if (!app.streamline_marker.empty() && ++streamline_frames == 600) {
                    std::error_code ec;
                    std::filesystem::remove(app.streamline_marker, ec);
                    pt::LogInfo("streamline: 600 frames rendered, start marker removed");
                    if (app.settings.upscaling.dlssg_interrupted && !app.settings_path.empty()) {
                        app.settings.upscaling.dlssg_interrupted = 0;
                        pt::SaveAppSettings(app.settings_path, app.settings);
                        pt::LogInfo("streamline: ran after the interrupted start (pt.ini dlssg_interrupted = 0)");
                    }
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
                    const auto cap_started = std::chrono::steady_clock::now();
                    SDL_DelayPrecise(static_cast<Uint64>(wait.count()));
                    frame_phases.current[FramePhases::kCap] += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - cap_started).count();
                }
            } else {
                frame_deadline = {};
            }
        }
        if (headless_fps_limit > 0) {
            const auto period = std::chrono::nanoseconds(1000000000LL / headless_fps_limit);
            const auto now = std::chrono::steady_clock::now();
            if (headless_frame_deadline.time_since_epoch().count() == 0 || now - headless_frame_deadline > period) {
                headless_frame_deadline = now;
            }
            headless_frame_deadline += period;
            std::this_thread::sleep_until(headless_frame_deadline);
        }
        if (!boot_shots.empty() && app.window) {
            const double seconds = static_cast<double>(SDL_GetTicksNS() - boot_wait_start) * 1e-9;
            auto shoot = [&](const char* name, bool& taken) {
                if (!taken && app.renderer.SaveScreenshot(boot_shots / name)) taken = true;
            };
            static bool shader_taken = false, splash_taken = false, loading_taken = false, play_taken = false;
            static int shader_frames = 0;
            if (shader_screen && ++shader_frames >= 2) shoot("shader_screen.png", shader_taken);
            if (!shader_screen && ui.BootSplashShown() && seconds > 1.0) shoot("title_picture.png", splash_taken);
            if (!shader_screen && !ui.BootSplashShown() && boot_wait && seconds > 0.5 && splash_taken) shoot("loading_screen.png", loading_taken);
            if (!boot_wait && game.Controller().Step() > 5 && frame > 30) {
                static uint64_t play_frame = 0;
                if (!play_frame) play_frame = frame;
                if (frame - play_frame == 120) shoot("first_play.png", play_taken);
                if (frame - play_frame > 600) game.RequestQuit();
            }
        }
        if (app.d3d12_marker && frame == 5) {
            if (const char* test = std::getenv("PT_TEST_CRASH"); test && std::string_view(test) == "d3d12frames") {
                pt::LogInfo("crash test: d3d12frames");
                *static_cast<volatile int*>(nullptr) = 1;
            }
        }
        if (app.d3d12_marker && frame >= 120) {
            app.d3d12_marker = false;
            ClearD3D12Marker();
            pt::LogInfo("rendering api: DirectX 12 drew 120 frames, start marker removed");
            auto& display = app.settings.display;
            if ((display.d3d12_crashed || display.d3d12_interrupted || display.d3d12_auto_fallback) && !app.settings_path.empty()) {
                if (display.d3d12_crashed)
                    pt::LogInfo("rendering api: DirectX 12 ran, the earlier crash no longer keeps it off (pt.ini d3d12_crashed = 0)");
                if (display.d3d12_interrupted)
                    pt::LogInfo("rendering api: DirectX 12 ran after the interrupted start (pt.ini d3d12_interrupted = 0)");
                if (display.d3d12_auto_fallback) {
                    display.rendering_api = std::string(pt::RenderingApiKey(pt::RenderingApi::D3D12));
                    pt::LogInfo("rendering api: the automatic switch to Vulkan is undone, pt.ini rendering_api = d3d12 again");
                }
                display.d3d12_crashed = 0;
                display.d3d12_interrupted = 0;
                display.d3d12_auto_fallback = false;
                pt::SaveAppSettings(app.settings_path, app.settings);
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
    app.fg_job.Cancel();
    controller_speaker.Close();
    input.Shutdown();
    if (script.Expectations() > 0) {
        pt::LogInfo("input script: {} expectations, {} failed", script.Expectations(), script.Failures());
    }
    if (!options.audio_capture.empty()) {
        const char* capture_channels = std::getenv("PT_CAPTURE_CHANNELS");
        const int channels = capture_channels ? std::atoi(capture_channels) : 2;
        WriteWav(options.audio_capture, captured, 48000, static_cast<uint16_t>(channels == 6 || channels == 8 ? channels : 2));
    }
    game.SetAudio(nullptr);
    sound.Shutdown();
    app.renderer.Gpu().WaitIdle();
    app.scene.vfx_forward = nullptr;
    app.scene.vfx_liquid_depth = nullptr;
    app.scene.vfx_filter = nullptr;
    app.scene.vfx_list = nullptr;
    pipeline_warmup.Join();
    vfx_pass.Shutdown();
    app.renderer.overlay = nullptr;
    app.renderer.overlay_top_pending = nullptr;
    ui.Shutdown();
    return script.Failures() > 0 ? 3 : 0;
}

}

bool NoStartMessages() {
    const char* e = std::getenv("PT_NO_START_MESSAGES");
    return e && e[0] == '1';
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
    if (const int probe = pt::VoiceCpuProbe(argc, argv); probe >= 0) return probe;
#ifdef _WIN32
    const HANDLE inherited = GetStdHandle(STD_ERROR_HANDLE);
    if ((inherited == nullptr || inherited == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif
    pt::IoSetMainThread();
    App app;
    app.options = ParseOptions(argc, argv);
    if (!app.options.make_loop_previews.empty()) {
        SetUpLoopPreviewRun(app.options);
    }
    if (!app.options.make_museum_previews.empty()) {
        SetUpMuseumPreviewRun(app.options);
    }
    if (!app.options.make_survival_visions.empty()) {
        SetUpSurvivalVisionRun(app.options);
    }
    const Options& options = app.options;
    const bool headless_user_data = options.headless && !pt::os::GetEnv("PT_HEADLESS_USER_DATA").empty();
    const bool tool = (options.headless && !headless_user_data) || options.script_test || options.anim_test ||
                      !options.voice_test.empty() || options.voice_listen > 0.0f || !options.texture_test.empty() || options.list_pads;
    std::filesystem::path log_path = options.log_path;
    if (log_path.empty()) {
        if (tool) {
            log_path = "pt.log";
        } else {
            std::error_code ec;
            std::filesystem::create_directories(UserDataDir(), ec);
            log_path = UserDataDir() / "pt.log";
        }
    }
    g_output_dir = log_path.parent_path();
    g_log_path = log_path;
    if (!tool) pt::PreserveCrashLog(log_path);
    if (!tool) {
        std::error_code ec;
        if (std::filesystem::exists(log_path, ec))
            std::filesystem::copy_file(log_path, log_path.parent_path() / "pt-previous.log", std::filesystem::copy_options::overwrite_existing, ec);
    }
    pt::LogSetFile(log_path);
    if (UpdateJournalAtStart(options) == JournalAtStart::Relaunch) {
        pt::LogExit(0, "restarted after an interrupted update was undone");
        return 0;
    }
#ifdef _WIN32
    pt::nr_native::Startup(NrDataDir(options.settings_path));
    if (!options.dlss5_install.empty()) {
        std::string key, detail;
        int last = -1;
        const bool ok = pt::nr_native::InstallTo(options.dlss5_install, options.dlss5_arch, [&](int p) {
            if (p / 10 != last) { last = p / 10; pt::LogInfo("dlss5 install: {}%", p); }
        }, {}, &key, &detail);
        pt::LogInfo("dlss5 install: {}{}{}", ok ? "done" : "failed", ok ? "" : " - " + key + ": ", detail);
        return ok ? 0 : 1;
    }
    if (kCommunityRouteRetired) {
        const std::filesystem::path game_root = pt::ExecutableDir();
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
        pt::nr_unlock::RetireRouteFiles(game_root, game_root / "data" / "retired-dlss5-route" / stamp);
    }
    {
        const std::filesystem::path game_root = pt::ExecutableDir();
        const std::filesystem::path archive = game_root / "data" / std::string(pt::fg_unlock::kRetiredDir);
        std::vector<std::filesystem::path> sources;
        if (!g_update_backup.empty())
            for (const char* name : pt::fg_unlock::kProxyCandidates) sources.push_back(g_update_backup / name);
        const pt::fg_unlock::ProxyRepair repair = pt::fg_unlock::RepairRecordedProxy(game_root, NrDataDir(options.settings_path) / "dlssg-unlock", sources, archive);
        using R = pt::fg_unlock::ProxyRepair::Result;
        if (repair.result == R::Restored) {
            g_fg_proxy_restored = true;
            pt::LogWarn("frame generation: the unlock's proxy {}", repair.detail);
        } else if (repair.result == R::NeedsReinstall) {
            g_fg_proxy_reinstall = true;
            pt::LogWarn("frame generation: the unlock's proxy {}; the unlock is reinstalled", repair.detail);
        } else if (repair.result == R::Fine) {
            pt::LogInfo("frame generation: the unlock's record is there, {}", repair.detail);
        }
        const std::string unrecorded = pt::fg_unlock::DescribeUnrecorded(pt::fg_unlock::UnrecordedProxies(game_root));
        if (!unrecorded.empty()) {
            pt::LogWarn("frame generation: {}", unrecorded);
            pt::SetCrashNote(unrecorded);
        }
        if (const std::optional<pt::fg_unlock::Install> unlock = pt::fg_unlock::ReadInstall(game_root)) {
            g_own_proxies = unlock->files;
            g_own_proxies.push_back(unlock->proxy);
            for (std::string& f : g_own_proxies) f = pt::os::PathToUtf8(std::filesystem::path(f).filename());
        }
        if (const std::string retired = pt::fg_unlock::RetireShippedProxy(game_root, archive); !retired.empty())
            pt::LogWarn("frame generation: {}", retired);
    }
    if (!tool) {
        const std::filesystem::path game_root = pt::ExecutableDir();
        const std::optional<pt::nr_unlock::Install> install = pt::nr_unlock::ReadInstall(game_root);
        const bool route_usable = install && pt::nr_unlock::Usable(*install);
        bool route_on = false;
        bool route_load = false;
        bool addon_written = false;
        if (install) {
            const std::filesystem::path ini = options.settings_path.empty() ? UserDataDir() / "pt.ini" : options.settings_path;
            pt::AppSettings peek;
            if (pt::LoadAppSettings(ini, peek)) {
                pt::NeuralRenderingKind kind = pt::NeuralRenderingKind::Off;
                pt::ParseNeuralRendering(peek.upscaling.neural_rendering, kind);
                if (const char* env = std::getenv("PT_NEURAL_RENDERING")) pt::ParseNeuralRendering(env, kind);
                pt::RenderingApi api = pt::RenderingApi::Vulkan;
                pt::ParseRenderingApi(peek.display.rendering_api, api);
                if (options.backend == "d3d12") api = pt::RenderingApi::D3D12;
                else if (options.backend == "vulkan") api = pt::RenderingApi::Vulkan;
                route_load = !kCommunityRouteRetired && !pt::nr_native::Enabled() && route_usable && api == pt::RenderingApi::D3D12 && !peek.upscaling.neural_route_armed;
                route_on = route_load && kind == pt::NeuralRenderingKind::Dlss5;
                app.nr_marker_read = true;
                app.nr_marker_at_start = peek.upscaling.neural_route_armed;
                if (route_on) {
                    peek.upscaling.neural_route_armed = true;
                    if (pt::SaveAppSettings(ini, peek)) {
                        app.nr_prearmed = true;
                        pt::LogInfo("neural rendering: route crash marker set before ReShade loads (DLSS 5 on at start)");
                    }
                }
                if (route_usable) {
                    pt::NeuralStyle style = pt::NeuralStyle::Default;
                    pt::ParseNeuralStyle(peek.upscaling.neural_style, style);
                    pt::nr_unlock::AddonSettings addon;
                    addon.on = route_on;
                    addon.style = static_cast<int>(style);
                    addon.intensity = std::clamp(peek.upscaling.neural_intensity, 0.0f, 1.0f);
                    addon.parked = !route_load;
                    app.nr_addon_parked = addon.parked;
                    if (const char* keys = std::getenv("PT_NR_ADDON_KEYS")) addon.extra = pt::nr_unlock::ParseAddonKeys(keys);
                    addon_written = pt::nr_unlock::WriteAddonSettings(game_root, addon);
                    if (addon_written && addon.parked) {
                        pt::LogInfo("neural rendering: the route's add-on parked this start (no ReShade loads it)");
                    }
                }
            }
        }
        if (!route_load && !addon_written) {
            std::error_code ec;
            if (install || std::filesystem::exists(game_root / L"renodx-dlss5.addon64", ec)) {
                pt::nr_unlock::AddonSettings addon;
                addon.parked = true;
                app.nr_addon_parked = true;
                addon_written = pt::nr_unlock::WriteAddonSettings(game_root, addon);
                if (addon_written) pt::LogInfo("neural rendering: the route's add-on parked this start (no ReShade loads it)");
            }
        }
        if (!route_load && app.nr_addon_parked && !addon_written) {
            SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
            pt::LogWarn("neural rendering: ReShade.ini could not be written; ReShade's Vulkan layer is kept out of this start");
        }
        if (const char* preload = std::getenv("PT_TEST_PRELOAD_DXGI"); preload && *preload == '1') LoadLibraryW((game_root / L"dxgi.dll").c_str());
        app.nr_reshade_loaded = pt::nr_unlock::LoadDxgiForRoute(game_root, install.has_value(), route_load);
        app.nr_addon_boot_on = route_on;
    }
#endif
    auto startup_failure = [&](const std::string& what, const std::string& advice) {
        pt::LogError("startup failed: {}", what);
        if (options.headless) {
            return;
        }
        const std::string text = what + "\n\n" + advice + "\n\nDetails are in " + pt::os::PathToUtf8(std::filesystem::absolute(log_path)) +
                                 "; please attach that file when you report this.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "P.T. could not start", text.c_str(), nullptr);
    };
#ifdef _WIN32
    const std::string driver_advice = "Install the latest graphics driver from NVIDIA, AMD or Intel (a clean reinstall fixed this for other "
                                      "players), and close overlays such as RivaTuner or MSI Afterburner. P.T. needs a GPU with Vulkan 1.3.";
#else
    const std::string driver_advice = "Make sure your graphics driver provides Vulkan 1.3 (Mesa 23 or newer, or the NVIDIA driver) and that "
                                      "P.T. runs inside your desktop session.";
#endif
    if (HeadlessOnly() && WouldOpenWindow(options)) {
        pt::LogError("refused: PT_HEADLESS_ONLY set, run would open a window");
        std::fprintf(stderr, "refused: PT_HEADLESS_ONLY set and this run would open a window\n");
        return 3;
    }
    if (!tool) {
#ifdef PT_RELEASE_LOCKS
        const bool move_old_saves = !options.headless || SaveMoveTest();
#else
        const bool move_old_saves = SaveMoveTest();
#endif
        const std::string test_answer = pt::os::GetEnv("PT_TEST_SAVE_MOVE");
        const auto save_move_language = [](const std::filesystem::path& legacy) {
            for (const auto& dir : {legacy, UserDataDir()}) {
                pt::game::SaveStore store;
                store.SetDirectory(dir, "PT_Save_Data");
                if (const auto save = store.Load()) {
                    return std::clamp(save->options.subtitle_language, 0, pt::game::UiAssets::kLanguageCount - 1);
                }
            }
            return std::clamp(pt::game::DefaultOptionsForLocale(pt::game::SystemLanguageTag()).subtitle_language, 0,
                              pt::game::UiAssets::kLanguageCount - 1);
        };
        const auto save_move_text = [](std::string_view key, int language, const std::filesystem::path& legacy) {
            std::string text(pt::game::PcText(key, language));
            const std::pair<std::string, std::filesystem::path> fills[] = {{"{1}", legacy}, {"{2}", UserDataDir()}};
            for (const auto& [token, path] : fills) {
                for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at)) {
                    const std::string value = pt::os::PathToUtf8(path);
                    text.replace(at, token.size(), value);
                    at += value.size();
                }
            }
            return text;
        };
        int save_move_lang = 0;
        const auto ask_legacy = [&](pt::platform::LegacyQuestion question, const std::filesystem::path& legacy) -> std::optional<bool> {
            save_move_lang = save_move_language(legacy);
            const char* key = question == pt::platform::LegacyQuestion::Move ? "pc_save_move_prompt" : "pc_save_move_newer";
            const std::string text = save_move_text(key, save_move_lang, legacy);
            if (!test_answer.empty()) {
                pt::LogInfo("user data: question {} (language {}) answered '{}' by PT_TEST_SAVE_MOVE:\n{}", key, save_move_lang, test_answer, text);
                return test_answer == "yes";
            }
            if (options.headless || !options.input_script.empty()) return std::nullopt;
            const std::string title(pt::game::PcText("pc_save_move_title", save_move_lang));
            const std::string yes(pt::game::PcText("pc_save_move_yes", save_move_lang));
            const std::string no(pt::game::PcText("pc_save_move_no", save_move_lang));
            const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes.c_str()},
                                                        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no.c_str()}};
            SDL_MessageBoxData box{};
            box.flags = SDL_MESSAGEBOX_INFORMATION;
            box.title = title.c_str();
            box.message = text.c_str();
            box.numbuttons = 2;
            box.buttons = buttons;
            int pressed = 0;
            if (!SDL_ShowMessageBox(&box, &pressed)) {
                pt::LogWarn("user data: the move question could not be shown ({}); asking again next start", SDL_GetError());
                return std::nullopt;
            }
            pt::LogInfo("user data: the player answered {} to {}", pressed == 1 ? "Yes" : "No", key);
            return pressed == 1;
        };
        pt::platform::UserDataHooks save_move_hooks;
        if (const int hold = std::atoi(pt::os::GetEnv("PT_TEST_SAVE_MOVE_HOLD_MS").c_str()); hold > 0 && SaveMoveTest()) {
            save_move_hooks.after_lock = [hold] {
                pt::LogInfo("user data: holding the old folder's lock for {} ms (PT_TEST_SAVE_MOVE_HOLD_MS)", hold);
                std::this_thread::sleep_for(std::chrono::milliseconds(hold));
            };
        }
        pt::platform::UserDataReport user_data =
            pt::platform::PrepareUserDataDirectory(UserDataDir(), move_old_saves ? LegacyUserDataDir() : std::filesystem::path(),
                                                   move_old_saves, ask_legacy, save_move_hooks);
        if (!user_data.success) {
            std::string text = "P.T. cannot prepare its data folder:\n" + pt::os::PathToUtf8(UserDataDir());
            for (const auto& error : user_data.errors) text += "\n" + error;
            text += "\nChoose a writable installation folder. Your existing settings and saves have not been removed.";
            pt::LogError("{}", text);
            std::fprintf(stderr, "%s\n", text.c_str());
            if (!options.headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "P.T. data folder", text.c_str(), nullptr);
            return 1;
        }
        pt::LogInfo("user data: {}", pt::os::PathToUtf8(UserDataDir()));
        for (const auto& note : user_data.notes) pt::LogInfo("user data: {}", note);
        if (user_data.migrated_legacy || user_data.cleaned_legacy) {
            pt::LogInfo("user data: {} the old profile folder: {} files copied and verified, {} older copies kept as .bak, {} old files "
                        "deleted{}",
                        user_data.cleaned_legacy ? "cleaned up (an earlier version had copied it)" : "moved", user_data.files_copied,
                        user_data.files_backed_up, user_data.files_removed, user_data.removed_legacy ? "; the old folder is gone" : "");
        }
        if (user_data.declined_legacy) pt::LogInfo("user data: the old profile folder stays where it is (the player chose No)");
        if (user_data.legacy_pending) pt::LogInfo("user data: the old profile folder waits for a start that can ask");
        for (const auto& warning : user_data.warnings) pt::LogWarn("user data: {}", warning);
        if (user_data.legacy_failed && !user_data.asked) {
            for (const auto& error : user_data.legacy_errors) {
                pt::LogWarn("user data: the old saves stay where they are this start (a later start tries again): {}", error);
            }
        }
        if (user_data.legacy_failed && user_data.asked) {
            for (const auto& error : user_data.legacy_errors) pt::LogError("user data: moving the old saves failed: {}", error);
            const auto legacy = LegacyUserDataDir();
            const int language = save_move_language(legacy);
            const std::string text = save_move_text("pc_save_move_failed", language, legacy);
            const std::string title(pt::game::PcText("pc_save_move_title", language));
            if (!options.headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
            else pt::LogInfo("user data: error dialog (language {}):\n{}", language, text);
        }
    }
    pt::LogInfo("pt-port version {} ({}), Hallway Engine", pt::update::CurrentVersion(), pt::update::Platform());
    pt::boot::Phase("data folder");
    pt::InstallCrashReporting(g_output_dir, PT_BUILD_ID);
#ifdef _WIN32
    if (std::getenv("PT_CRASH_UPLOAD_TEST")) {
        pt::OfferCrashUpload(g_output_dir.empty() ? std::filesystem::path(".") : g_output_dir, std::string(pt::update::CurrentVersion()), false);
    }
    if (const char* test = std::getenv("PT_TEST_CRASH")) {
        const std::string kind = test;
        pt::LogInfo("crash test: {}", kind);
        if (kind == "access") {
            *static_cast<volatile int*>(nullptr) = 1;
        } else if (kind == "throw") {
            throw std::system_error(std::error_code(1113, std::system_category()), "crash test");
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

    pt::boot::Phase("crash reporting");
    pt::Vfs vfs;
    std::string game_dir_source;
    bool first_run_quit = false;
    const std::filesystem::path game_dir = FindGameDir(options, game_dir_source, &first_run_quit);
    if (game_dir != options.game_dir) {
        pt::LogInfo("game files found at {}", pt::os::PathToUtf8(game_dir));
    }
    pt::boot::Phase("game folder", game_dir_source);
    const bool mounted = vfs.Mount(game_dir);
    pt::boot::Phase("game archives");
    if (const int missing = mounted ? vfs.MissingCorePackages() : 0; missing > 0 && !options.headless) {
        pt::LogError("startup failed: the game files in {} are not a complete P.T.: {} of the 12 core packages are missing from chunk1.psarc",
                     pt::os::PathToUtf8(std::filesystem::absolute(game_dir)), missing);
        const std::string text = pt::first_run::Text("pc_game_incomplete", pt::first_run::PlayerLanguage(UserDataDir()),
                                                     {pt::os::PathToUtf8(std::filesystem::absolute(game_dir)), std::to_string(missing),
                                                      pt::os::PathToUtf8(std::filesystem::absolute(g_log_path))});
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "P.T.", text.c_str(), nullptr);
        return 1;
    }
    if (!mounted) {
        if (!options.headless && !first_run_quit) {
#ifdef __APPLE__
            const char* beside = "P.T. PC Port.app";
            const char* program = "pt";
#elif defined(_WIN32)
            const char* beside = "pt.exe";
            const char* program = "pt.exe";
#else
            const char* beside = "pt";
            const char* program = "pt";
#endif
            const std::string text = pt::first_run::Text("pc_game_not_found", pt::first_run::PlayerLanguage(UserDataDir()),
                                                         {pt::os::PathToUtf8(std::filesystem::absolute(game_dir)), beside, program});
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

    pt::FilePrefetch file_prefetch;
    if (const char* prefetch = std::getenv("PT_PREFETCH"); !prefetch || std::string_view(prefetch) != "0") {
        file_prefetch.Start({game_dir / "chunk1.psarc", game_dir / "texture.qar"});
    }

    if (!SDL_Init(options.headless ? 0 : SDL_INIT_VIDEO)) {
        pt::LogError("SDL_Init: {}", SDL_GetError());
        pt::LogError("SDL_Init: {}", pt::SdlVideoDiagnostics());
        startup_failure(std::string("No window can be opened: ") + SDL_GetError() + ".", driver_advice);
        return 1;
    }
    pt::boot::Phase("video");
    pt::devices::Start({.gamepad = !options.headless || options.virtual_pads,
                        .audio = !options.headless,
                        .background = !options.headless && !options.virtual_pads,
                        .virtual_only = options.virtual_pads});
    if (!options.headless) {
        pt::LogSdlVideoInUse();
        pt::OfferCrashUpload(g_output_dir.empty() ? std::filesystem::path(".") : g_output_dir, std::string(pt::update::CurrentVersion()), true);
    }
    app.settings_path = !options.settings_path.empty() ? options.settings_path
                        : (options.headless && !headless_user_data) ? std::filesystem::path()
                                                                    : UserDataDir() / "pt.ini";
    const bool settings_loaded = !app.settings_path.empty() && pt::LoadAppSettings(app.settings_path, app.settings);
    if (pt::nr_native::Enabled()) pt::ConfigureNativeNeuralRuntime(app.settings.upscaling.neural_runtime);
    if (settings_loaded && app.settings.upscaling.frame_generation != "off") {
        std::ifstream previous_log(log_path.parent_path() / "pt-previous.log");
        const pt::FgStartLoss fg_loss = pt::FindFrameGenStartLoss(previous_log);
        if (fg_loss.found) {
            pt::LogWarn("frame generation: the last run lost the graphics device {:.1f} s after {} made its swapchain (pt-previous.log): frame "
                        "generation is off now (pt.ini frame_generation = off); the player may turn it on again",
                        fg_loss.seconds, fg_loss.owner);
            app.settings.upscaling.frame_generation = "off";
            pt::SaveAppSettings(app.settings_path, app.settings);
            app.pending_notices.push_back("pc_notice_fg_start_lost");
        }
    }
#ifdef PT_D3D12_GAME
    pt::SetD3D12RunsGame(true);
    pt::SetD3D12Probe(&ProbeD3D12);
    g_d3d12_marker = app.settings_path.empty() ? std::filesystem::path() : app.settings_path.parent_path() / "d3d12-starting.txt";
    if (std::error_code ec; !g_d3d12_marker.empty() && std::filesystem::exists(g_d3d12_marker, ec)) {
        const int stage = ReadD3D12Marker();
        const StartEvidence evidence = EvidenceOfEarlierStart(g_d3d12_marker, log_path.parent_path() / "pt-previous.log");
        ClearD3D12Marker();
        auto& display = app.settings.display;
        if (!evidence.crashed && evidence.exited) {
            display.d3d12_interrupted = 0;
            pt::LogInfo("rendering api: the DirectX 12 start marker (stage {}) was left by a start that ended with its own exit; "
                        "not a crash, DirectX 12 stays as pt.ini has it", stage);
        } else if (!evidence.crashed && display.d3d12_interrupted < 1) {
            display.d3d12_interrupted = 1;
            pt::LogWarn("rendering api: the last start ended while DirectX 12 started (stage {}) with no sign of a crash, evidence: {}; "
                        "it was closed or ended from outside: this start tries DirectX 12 again, a second such start in a row "
                        "switches to Vulkan (pt.ini d3d12_interrupted = 1)", stage, evidence.what);
        } else {
            const bool was_d3d12 = display.rendering_api == pt::RenderingApiKey(pt::RenderingApi::D3D12);
            if (evidence.crashed) {
                pt::LogWarn("rendering api: the last start ended while DirectX 12 started (stage {}), evidence of a crash: {}", stage, evidence.what);
            } else {
                pt::LogWarn("rendering api: the second start in a row ended while DirectX 12 started (stage {}) with no sign of a crash, "
                            "evidence: {}; treated as a crash", stage, evidence.what);
            }
            display.d3d12_interrupted = 0;
            display.d3d12_crashed = stage;
            display.rendering_api = std::string(pt::RenderingApiKey(pt::RenderingApi::Vulkan));
            display.rendering_api_asked = true;
            if (was_d3d12) display.d3d12_auto_fallback = true;
            pt::LogWarn("rendering api: {}: pt.ini is set to Vulkan{}, and DirectX 12 stays off until the player chooses it in the settings or --backend d3d12 runs", D3D12CrashReason(stage),
                        was_d3d12 ? " (d3d12_auto_fallback = 1: back to DirectX 12 once it runs again)" : "");
            app.pending_notices.push_back("pc_notice_d3d12_crashed");
        }
        pt::SaveAppSettings(app.settings_path, app.settings);
    }
    {
        pt::RenderingApi cli_api = pt::RenderingApi::Vulkan;
        const bool cli_d3d12 = pt::ParseRenderingApi(options.backend, cli_api) && cli_api == pt::RenderingApi::D3D12;
        if (app.settings.display.d3d12_crashed && !cli_d3d12) {
            pt::BlockD3D12(D3D12CrashReason(app.settings.display.d3d12_crashed));
            app.d3d12_crash_stage = app.settings.display.d3d12_crashed;
            pt::LogInfo("rendering api: DirectX 12 off, pt.ini [display] d3d12_crashed = {}", app.settings.display.d3d12_crashed);
        }
    }
    if (app.nr_reshade_loaded) {
        pt::SkipD3D12Probe();
        pt::LogInfo("rendering api: DirectX 12 probe skipped (the DLSS 5 route's ReShade is loaded; the game's own device is the check)");
    }
#endif
    std::string d3d12_reason;
    if (!options.headless && options.backend.empty() && pt::RenderingApiSelectable() && !app.settings.display.rendering_api_asked &&
        !(std::getenv("PT_NO_RENDERING_API_QUESTION") && std::getenv("PT_NO_RENDERING_API_QUESTION")[0] == '1') &&
        pt::D3D12Usable(d3d12_reason)) {
        const int language = std::clamp(pt::game::DefaultOptionsForLocale(pt::game::SystemLanguageTag()).subtitle_language, 0,
                                        pt::game::UiAssets::kLanguageCount - 1);
        pt::RenderingApi chosen = pt::RenderingApi::Vulkan;
        const pt::AskAnswer answer = pt::AskRenderingApi(pt::game::PcText("pc_rendering_api", language),
                                                         pt::game::PcText("pc_rendering_api_prompt", language), chosen);
        if (answer != pt::AskAnswer::NotShown) {
            if (answer == pt::AskAnswer::Chosen) {
                app.settings.display.rendering_api = std::string(pt::RenderingApiKey(chosen));
            }
            app.settings.display.rendering_api_asked = true;
            pt::SaveAppSettings(app.settings_path, app.settings);
            pt::LogInfo("rendering api: first start question {} ({})", answer == pt::AskAnswer::Chosen ? "answered" : "closed",
                        app.settings.display.rendering_api);
        }
    }
    {
        std::string note;
        app.rendering_api = pt::ChooseRenderingApi(options.backend, app.settings.display.rendering_api, note);
        if (!note.empty()) {
            pt::LogWarn("rendering api: {}", note);
        }
        pt::LogInfo("rendering api: {}", pt::RenderingApiName(app.rendering_api));
    }
    if (!options.headless) {
        DetectDisplayEnvironment(app);
        if (!settings_loaded) {
            SDL_Rect usable{};
            SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
#ifdef __linux__
            const bool fits = pt::display::WindowFits({app.settings.display.width, app.settings.display.height}, {usable.w, usable.h});
#else
            const bool fits = true;
#endif
            if (!options.size_set && (app.display_env.gamescope || !fits)) {
                app.settings.display.fullscreen = pt::display::kBorderless;
                pt::LogInfo("display: first start borderless ({}; usable area {}x{})",
                            app.display_env.gamescope ? "gamescope" : "the default window does not fit the display", usable.w, usable.h);
            }
            pt::SaveAppSettings(app.settings_path, app.settings);
        }
        if (app.settings.network.check_updates && !options.no_update_check) {
            app.updates.Start(pt::ExecutableDir());
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
        pt::boot::Phase("settings");
        const bool vulkan_start = app.rendering_api != pt::RenderingApi::D3D12;
        const std::filesystem::path start_marker =
            app.settings_path.empty() ? std::filesystem::path() : app.settings_path.parent_path() / "graphics-starting.txt";
        const pt::start_watch::Previous previous = pt::start_watch::TakePrevious(start_marker);
        bool layers_off = app.settings.display.vulkan_implicit_layers == 0;
        bool layers_retry = false;
        if (previous.found) {
            pt::LogWarn("start: the last start (pid {}, {}, {}{}) ended inside '{}' before its graphics device was ready, without the "
                        "game's own exit; its log is pt-previous.log",
                        previous.pid, previous.time, previous.api, previous.layers_off ? ", implicit Vulkan layers off" : "", previous.step);
            for (const std::string& line : pt::start_watch::RecentErrorReports(48.0, 4)) pt::LogWarn("start: Windows error report {}", line);
            if (previous.api == "vulkan" && pt::start_watch::LoaderStep(previous.step)) {
                if (!previous.layers_off && !layers_off) {
                    layers_off = layers_retry = true;
                    pt::LogWarn("start: the Vulkan loader loads the drivers and the implicit layers in '{}': this start runs without the "
                                "implicit layers; pt.ini keeps them off if it gets its graphics device", previous.step);
                } else if (previous.layers_off) {
                    pt::LogWarn("start: it ended there without the implicit layers too: they are not the cause; the graphics driver is "
                                "the next suspect");
                    if (app.settings.display.vulkan_implicit_layers == 0) {
                        app.settings.display.vulkan_implicit_layers = 1;
                        layers_off = false;
                        pt::SaveAppSettings(app.settings_path, app.settings);
                        pt::LogInfo("start: pt.ini vulkan_implicit_layers = 1 again");
                    }
                }
            }
            if (previous.api == "vulkan" && !NoStartMessages()) {
                const int language = pt::first_run::PlayerLanguage(UserDataDir());
                std::string text = pt::first_run::Text("pc_start_died", language,
                                                       {previous.step, pt::os::PathToUtf8(std::filesystem::absolute(log_path.parent_path() / "pt-previous.log"))});
                if (layers_retry) text += "\n\n" + pt::first_run::Text("pc_start_retry_layers", language);
                pt::LogInfo("start: message about the last start shown");
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, pt::first_run::Text("pc_start_died_title", language).c_str(), text.c_str(), nullptr);
            }
        }
#ifdef _WIN32
        LogGraphicsAdapters();
#endif
        if (vulkan_start) {
            if (layers_off) {
                std::string filter = pt::os::GetEnv("VK_LOADER_LAYERS_DISABLE");
                filter += std::string(filter.empty() ? "" : ",") + "~implicit~";
#ifdef _WIN32
                _putenv_s("VK_LOADER_LAYERS_DISABLE", filter.c_str());
#else
                setenv("VK_LOADER_LAYERS_DISABLE", filter.c_str(), 1);
#endif
                pt::LogInfo("vulkan: VK_LOADER_LAYERS_DISABLE={} for this process ({})", filter,
                            layers_retry ? "the last start ended inside the Vulkan loader" : "pt.ini vulkan_implicit_layers = 0");
            }
            pt::vk::LogImplicitLayers(pt::vk::PrepareImplicitLayers());
        }
        app.start_layers_retry = layers_retry;
        pt::start_watch::Arm({.marker = start_marker,
                              .api = vulkan_start ? "vulkan" : "d3d12",
                              .layers_off = layers_off,
                              .log_path = log_path,
                              .hang_title = NoStartMessages() ? std::function<std::string()>() : [] { return pt::first_run::Text("pc_start_died_title", pt::first_run::PlayerLanguage(UserDataDir())); },
                              .hang_text = [log_path](int seconds, const std::string& step) {
                                  return pt::first_run::Text("pc_start_hang", pt::first_run::PlayerLanguage(UserDataDir()),
                                                             {std::to_string(seconds), step, pt::os::PathToUtf8(std::filesystem::absolute(log_path))});
                              }});
        pt::start_watch::Step("window", vulkan_start ? "SDL_CreateWindow; with Vulkan, SDL loads the Vulkan loader here and asks it for "
                                                       "the instance extensions: the loader loads every driver and the pre-instance layers"
                                                     : "SDL_CreateWindow");
        const SDL_WindowFlags api_flag = app.rendering_api == pt::RenderingApi::D3D12 ? SDL_WindowFlags(0) : SDL_WINDOW_VULKAN;
        const SDL_WindowFlags start_fullscreen = WindowMode(app) != pt::display::kWindow ? SDL_WINDOW_FULLSCREEN : SDL_WindowFlags(0);
        app.window = SDL_CreateWindow("P.T.", app.settings.display.width, app.settings.display.height,
                                      api_flag | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN | start_fullscreen);
        pt::start_watch::Done(app.window ? "created" : "failed");
        if (!app.window) {
            pt::start_watch::Finish();
            pt::LogError("SDL_CreateWindow: {}", SDL_GetError());
            startup_failure(std::string("The game window could not be created: ") + SDL_GetError() + ".", driver_advice);
            return 1;
        }
        ApplyFullscreen(app);
        SDL_ShowWindow(app.window);
        pt::os::ClearWindow(WindowHandle(app.window));
        LogWindowState(app, "window shown");
        LogDisplays();
        LogGamescopeScreen(app);
        if (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(app.window);
        SDL_RaiseWindow(app.window);
        const bool in_front = pt::os::ActivateWindow(WindowHandle(app.window));
        pt::LogInfo("window: raised at start, {} ({})", in_front ? "in front" : "not the foreground window",
                    DescribeForeground(pt::os::GetForegroundWindowInfo()));
        pt::boot::Phase("window");
    }
    MountMods(app);
    pt::RendererSettings settings;
    settings.headless = options.headless;
    settings.validation = options.validation;
    settings.vsync = options.vsync;
    settings.hdr = app.settings.display.hdr;
    settings.hdr_paper_white = app.settings.display.hdr_paper_white;
    settings.hdr_peak = app.settings.display.hdr_peak;
    settings.hdr_ui_white = app.settings.display.hdr_ui_white;
    settings.hdr_reach = app.settings.display.hdr_reach;
    settings.hdr_look = HdrLookOf(app.settings);
    if (!options.headless) {
        settings.pipeline_cache_dir = UserDataDir() / "pipeline-cache";
    } else if (!options.settings_path.empty()) {
        settings.pipeline_cache_dir = std::filesystem::absolute(options.settings_path).parent_path() / "pipeline-cache";
    }
    settings.width = options.width;
    settings.height = options.height;
    const bool d3d12 = app.rendering_api == pt::RenderingApi::D3D12;
#ifdef PT_D3D12_GAME
    if (d3d12) {
        settings.backend = pt::gpu::Backend::D3D12;
        settings.create_d3d12 = [](std::string& error) {
            pt::gpu::d3d12::CreateOptions o;
            return pt::gpu::d3d12::Create(o, error);
        };
        pt::UpscaleHost::Get().SetD3D12Factory(&pt::gpu::d3d12::CreateUpscaleBackend);
        pt::SetD3D12RayTracingFactory(&pt::gpu::d3d12::CreateRayTracing);
    }
#endif
    if ((app.settings.vr.enabled || options.vr) && !options.no_vr) {
        app.xr = std::make_unique<pt::xr::Host>();
        if (app.xr->Init("P.T. (pt-port)", d3d12)) {
#ifdef PT_D3D12_GAME
            if (d3d12) {
                pt::gpu::d3d12::CreateOptions o;
                int min_feature_level = 0;
                app.xr->D3D12Adapter(o.adapter_luid, min_feature_level);
                o.use_adapter_luid = true;
                settings.create_d3d12 = [o](std::string& error) { return pt::gpu::d3d12::Create(o, error); };
            }
#endif
            if (!d3d12) app.renderer.Context().creator = app.xr.get();
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
    app.streamline_marker = StartStreamline(app, d3d12);
    int result = 1;
    const bool test_device_lost = std::getenv("PT_TEST_CRASH") && std::string(std::getenv("PT_TEST_CRASH")) == "devicelost";
    pt::boot::Phase("mods and graphics settings");
    if (d3d12) {
        ArmD3D12Marker(2);
        app.d3d12_marker = true;
        if (const char* end = std::getenv("PT_TEST_START_END")) {
            pt::LogInfo("start test: {} while DirectX 12 starts", end);
#ifdef _WIN32
            if (std::string_view(end) == "kill") TerminateProcess(GetCurrentProcess(), 1);
#endif
            if (std::string_view(end) == "close") {
                SDL_Event close{};
                close.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&close);
            }
        }
    }
    const bool renderer_ready = app.renderer.Init(app.window, settings);
    pt::start_watch::Finish();
    if (renderer_ready && app.start_layers_retry && !app.settings_path.empty()) {
        app.settings.display.vulkan_implicit_layers = 0;
        pt::SaveAppSettings(app.settings_path, app.settings);
        pt::LogWarn("start: the graphics device started without the implicit Vulkan layers after the last start ended inside the "
                    "loader; pt.ini vulkan_implicit_layers = 0 keeps them off (1 loads them again)");
    }
    if (d3d12 && renderer_ready) {
        if (const char* test = std::getenv("PT_TEST_CRASH"); test && std::string_view(test) == "d3d12device") {
            pt::LogInfo("crash test: d3d12device");
            *static_cast<volatile int*>(nullptr) = 1;
        }
        ArmD3D12Marker(3);
    }
    if (!renderer_ready) {
        ClearD3D12Marker();
        app.d3d12_marker = false;
        const bool back_to_vulkan = d3d12 && options.backend.empty() && !app.settings_path.empty();
        if (back_to_vulkan) {
            app.settings.display.rendering_api = std::string(pt::RenderingApiKey(pt::RenderingApi::Vulkan));
            pt::SaveAppSettings(app.settings_path, app.settings);
            pt::LogWarn("rendering api: DirectX 12 did not start; pt.ini is set to Vulkan for the next start");
        }
        if (!d3d12 && app.settings.display.d3d12_crashed && !options.headless) {
            pt::LogError("startup failed: no graphics API starts here: Vulkan could not set up its device and DirectX 12 is off "
                         "(pt.ini d3d12_crashed = {})", app.settings.display.d3d12_crashed);
            if (!NoStartMessages()) {
                const int language = pt::first_run::PlayerLanguage(UserDataDir());
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, pt::first_run::Text("pc_start_died_title", language).c_str(),
                                         pt::first_run::Text("pc_start_no_api", language, {pt::os::PathToUtf8(std::filesystem::absolute(log_path))}).c_str(),
                                         nullptr);
            }
        } else {
#ifdef _WIN32
            if (g_no_gpu_driver) {
                startup_failure("No graphics driver is installed: Windows draws the screen with its Microsoft Basic Render Driver, which "
                                "cannot run the game.",
                                driver_advice);
            } else
#endif
            startup_failure(d3d12 ? (back_to_vulkan ? "The graphics device could not be set up (DirectX 12). The next start uses Vulkan."
                                                    : "The graphics device could not be set up (DirectX 12).")
                                  : "The graphics device could not be set up (Vulkan).",
                            driver_advice);
        }
    }
    pt::boot::Phase("graphics device");
    ScanThirdPartyTools();
#ifdef _WIN32
    if (renderer_ready && !d3d12 && !options.headless && app.renderer.Context().intel_first_frame_driver) {
        const std::string driver = app.renderer.Context().intel_driver_name;
        const std::filesystem::path marker =
            app.settings_path.empty() ? std::filesystem::path() : app.settings_path.parent_path() / "intel-driver-notice.txt";
        std::string shown;
        if (!marker.empty()) std::ifstream(marker) >> shown;
        if (shown == driver) {
            pt::LogInfo("vulkan: the Intel driver notice was shown for {} at an earlier start", driver);
        } else {
            const int language = std::clamp(pt::game::DefaultOptionsForLocale(pt::game::SystemLanguageTag()).subtitle_language, 0,
                                            pt::game::UiAssets::kLanguageCount - 1);
            std::string text(pt::game::PcText("pc_intel_driver_prompt", language));
            if (const size_t at = text.find("{1}"); at != std::string::npos) text.replace(at, 3, driver);
            const std::string title(pt::game::PcText("pc_intel_driver_title", language));
            const bool box = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title.c_str(), text.c_str(), app.window);
            if (box && !marker.empty()) std::ofstream(marker) << driver << std::endl;
            pt::LogInfo("vulkan: Intel driver notice for {} {}", driver, box ? "shown" : std::string("not shown: ") + SDL_GetError());
        }
    }
#endif
    if (renderer_ready) try {
        pt::LogInfo("startup: DLSS 5 and frame generation routes");
        const pt::gpu::DeviceInfo& info = app.renderer.Gpu().Info();
        app.nr_gpu = pt::nr_unlock::ClassifyGpu(info.vendor_id, info.device_id, info.adapter);
        app.nr_install = pt::nr_unlock::ReadInstall(pt::ExecutableDir());
        app.nr_arch = pt::nr_native::CodeArchFor(info.vendor_id, info.device_id, info.adapter);
        app.nr_install_usable = pt::nr_native::UseCard(app.nr_arch) ||
                                (!kCommunityRouteRetired && app.nr_install && pt::nr_unlock::Usable(*app.nr_install));
        if (app.nr_install && !pt::nr_native::Active()) {
            pt::LogInfo("neural rendering: an old community DLSS 5 install is next to pt.exe; it is no longer used (Install or Unlock sets up DLSS 5)");
        }
        if (const char* fake = std::getenv("PT_TEST_NR_INSTALLED"); fake && *fake == '1') {
            app.nr_gpu = pt::nr_unlock::GpuClass::NeedsUnlock;
            app.nr_install_usable = true;
            app.nr_reshade_loaded = true;
            app.nr_addon_boot_on = app.settings.upscaling.neural_rendering == "dlss5";
            if (const char* state = std::getenv("PT_TEST_ROUTE_STATE"); state && std::string_view(state) == "nr=rtx50") {
                app.nr_gpu = pt::nr_unlock::GpuClass::Native;
            } else if (state && std::string_view(state) == "nr=toggle-unreachable") {
                app.nr_hook_failed = true;
            }
        }
        if (NeuralRouteNeedsRestart(app)) {
            pt::LogInfo("neural rendering: DLSS 5 on and off apply at the next start (the add-on's live toggle is unavailable)");
        }
        if (app.nr_install) {
            pt::LogInfo("neural rendering: community DLSS 5 route found ({}), {} (ReShade {}, add-on {}, runtime {}){}",
                        app.nr_install->route, app.nr_install_usable ? "usable" : "not the route this build drives",
                        app.nr_install->reshade, app.nr_install->addon, app.nr_install->runtime,
                        app.nr_reshade_loaded ? ", ReShade loaded this session" : "");
            if (app.nr_install_usable && app.nr_reshade_loaded) {
                const pt::nr_unlock::RouteReport rep = pt::nr_unlock::ReadRouteReport(pt::ExecutableDir());
                if (rep.state == pt::nr_unlock::RouteReport::State::Running) {
                    pt::LogInfo("neural rendering: the route's add-on ran feature 18 ({})", rep.line);
                } else if (rep.state == pt::nr_unlock::RouteReport::State::Failed) {
                    pt::LogWarn("neural rendering: the route's add-on reported a failure{}{} ({})", rep.code.empty() ? "" : " code ", rep.code, rep.line);
                }
            }
        }
        pt::NeuralRenderingKind wanted_kind = pt::NeuralRenderingKind::Off;
        pt::ParseNeuralRendering(app.settings.upscaling.neural_rendering, wanted_kind);
        if (const char* env = std::getenv("PT_NEURAL_RENDERING")) pt::ParseNeuralRendering(env, wanted_kind);
        const bool marker_at_start = app.nr_marker_read ? app.nr_marker_at_start : app.settings.upscaling.neural_route_armed;
        if (app.nr_prearmed) app.nr_route_armed = true;
        const bool prearmed_live = app.nr_prearmed && NeuralRouteActive(app) && app.nr_reshade_loaded && wanted_kind == pt::NeuralRenderingKind::Dlss5;
        if (marker_at_start && app.nr_install_usable && wanted_kind == pt::NeuralRenderingKind::Dlss5) {
            if (pt::nr_native::Active()) {
                pt::LogWarn("neural rendering: DLSS 5 held off for this session: the last run ended with it on (crash marker); pt.ini keeps it on");
                app.nr_crash_hold = true;
                app.nr_row_note = "pc_note_nr_crash";
                ArmNeuralRoute(app, false);
                ApplyNeuralToScene(app);
            } else {
                DisableNeuralRoute(app, "pc_note_nr_crash", "the crash marker was set at start: the last run crashed or lost the device with neural rendering on");
            }
        } else if (prearmed_live) {
        } else if (app.settings.upscaling.neural_route_armed && !(app.nr_prearmed && NeuralRouteActive(app) && wanted_kind == pt::NeuralRenderingKind::Dlss5)) {
            ArmNeuralRoute(app, false);
        } else if (NeuralRouteActive(app) && wanted_kind == pt::NeuralRenderingKind::Dlss5 && !app.nr_reshade_loaded) {
            DisableNeuralRoute(app, "pc_note_nr_addon", "ReShade (the route's dxgi.dll) did not load, so the add-on is not in the game");
        }
        std::string wanted = app.settings.upscaling.neural_rendering;
        if (const char* env = std::getenv("PT_NEURAL_RENDERING")) wanted = env;
        pt::NeuralRenderingKind kind = pt::NeuralRenderingKind::Off;
        if (!pt::ParseNeuralRendering(wanted, kind)) pt::LogWarn("neural rendering: unknown value {}, read as off", wanted);
        if (kind != pt::NeuralRenderingKind::Off) {
            const pt::NeuralRenderingSupport nr = NeuralSupport(app);
            if (nr.available) {
                pt::LogInfo("neural rendering: DLSS 5 available");
            } else {
                pt::LogInfo("neural rendering: DLSS 5 unavailable: {}", nr.reason);
                pt::LogWarn("neural rendering: {} asked for, rendering without it", pt::NeuralRenderingKey(kind));
            }
        }
        ApplyNeuralToScene(app);

        if (app.renderer.Backend() == pt::gpu::Backend::D3D12) {
            pt::UpscaleHost::Get().CheckDlssFrameGenD3D12(info.vendor_id, info.device_id, info.luid);
        }
        const pt::DlssFrameGenSupport& fg = pt::UpscaleHost::Get().DlssFrameGen();
        app.fg_install = pt::fg_unlock::ReadInstall(pt::ExecutableDir());
        app.fg_install_usable = app.fg_install && pt::fg_unlock::Usable(*app.fg_install);
        app.fg_applicable = pt::fg_unlock::Applicable(app.nr_arch, fg.note, app.fg_install_usable);
        if (const char* fake = std::getenv("PT_TEST_FG_INSTALLED"); fake && *fake == '1') {
            app.fg_applicable = true;
            app.fg_install_usable = true;
        }
        if (const char* arch = std::getenv("PT_TEST_FG_ARCH")) {
            const std::string_view value = arch;
            const int code = value == "75" ? 75 : value == "86" ? 86 : value == "89" ? 89 : value == "0" ? 0 : -1;
            app.fg_applicable = pt::fg_unlock::Applicable(code, "pc_note_dlssg_gpu", app.fg_install_usable);
        }
        {
            const std::filesystem::path data = app.settings_path.empty() ? UserDataDir() : app.settings_path.parent_path();
            auto route_result = [&](const std::filesystem::path& file, const char* installed, const char* removed, const char* failed,
                                    std::string& row_note, const char* row_failed, const char* log_name) {
                std::ifstream in(file);
                if (!in) return;
                std::string kind, detail, line;
                int code = -1;
                while (std::getline(in, line)) {
                    if (line.rfind("kind ", 0) == 0) kind = line.substr(5);
                    else if (line.rfind("exit ", 0) == 0) code = std::atoi(line.c_str() + 5);
                    else if (line.rfind("detail ", 0) == 0) detail = line.substr(7);
                }
                in.close();
                std::error_code ec;
                std::filesystem::remove(file, ec);
                app.pending_notices.push_back(code == 0 ? (kind == "remove" ? removed : installed) : failed);
                if (code != 0) row_note = row_failed;
                pt::LogInfo("{}: pt-updater's {} {} ({})", log_name, kind.empty() ? "step" : kind, code == 0 ? "finished" : "failed", detail);
            };
            route_result(data / "dlss5-unlock" / std::string(pt::nr_unlock::kResultFile), "pc_notice_nr_installed", "pc_notice_nr_removed",
                         "pc_notice_nr_route_failed", app.nr_row_note, "pc_note_nr_route_failed", "neural rendering");
            route_result(data / "dlssg-unlock" / std::string(pt::fg_unlock::kResultFile), "pc_notice_fg_installed", "pc_notice_fg_removed",
                         "pc_notice_fg_route_failed", app.fg_row_note, "pc_note_fg_route_failed", "frame generation");
        }
        if (const char* test = std::getenv("PT_TEST_ROUTE_STATE")) {
            app.route_state_test = true;
            const std::string spec = test;
            auto state_of = [&](const char* route) -> std::string {
                const std::string key = std::string(route) + "=";
                const size_t at = spec.find(key);
                if (at == std::string::npos) return {};
                return spec.substr(at + key.size(), spec.find(',', at) == std::string::npos ? std::string::npos : spec.find(',', at) - at - key.size());
            };
            const std::string fg = state_of("fg"), nr = state_of("nr");
            if (fg == "downloading") app.fg_test_percent = 42;
            else if (fg == "verifying") app.fg_test_percent = 100;
            else if (fg == "failed") app.fg_row_note = "pc_note_fg_unlock_download";
            else if (fg == "guard") DisableFgRoute(app, "pc_note_fg_fallback", "PT_TEST_ROUTE_STATE");
            if (nr == "downloading") app.nr_test_percent = 42;
            else if (nr == "verifying") app.nr_test_percent = 100;
            else if (nr == "failed") app.nr_row_note = "pc_note_nr_unlock_download";
            else if (nr == "guard") DisableNeuralRoute(app, "pc_note_nr_create", "PT_TEST_ROUTE_STATE");
            using pt::route_queue::Op;
            using pt::route_queue::Route;
            auto pending = [&](Route route, const std::string& state) {
                if (state == "pending-install") app.routes.Request(route, Op::Install);
                else if (state == "pending-remove") app.routes.Request(route, Op::Remove);
                else if (state == "pending-reinstall") app.routes.Request(route, Op::Reinstall);
                else if (state == "pending-cancelled") {
                    app.routes.Request(route, Op::Install);
                    app.routes.Request(route, Op::Remove);
                }
            };
            if (spec.find("nr=") < spec.find("fg=")) {
                pending(Route::Nr, nr);
                pending(Route::Fg, fg);
            } else {
                pending(Route::Fg, fg);
                pending(Route::Nr, nr);
            }
            pt::LogInfo("route state test: frame generation {}, neural rendering {}", fg.empty() ? "as is" : fg, nr.empty() ? "as is" : nr);
        }
        if (g_fg_proxy_reinstall && app.fg_install && !options.headless && !app.route_state_test && !app.fg_job.Busy()) {
            const std::filesystem::path work = (app.settings_path.empty() ? UserDataDir() : app.settings_path.parent_path()) / "dlssg-unlock";
            if (app.fg_job.Start(pt::fg_unlock::Job::Kind::Reinstall, work)) {
                app.fg_job_serial = app.fg_job.Get().serial;
                app.fg_job_request = 3;
                pt::LogInfo("frame generation: community reinstall started for the missing or wrong proxy (downloading {} {})", pt::fg_unlock::kToolName,
                            pt::fg_unlock::kToolVersion);
            }
        }
        const bool fg_proxy = pt::fg_unlock::ProxyPresent(pt::ExecutableDir());
        if (app.fg_install || fg_proxy) {
            pt::LogInfo("frame generation: community unlock {} (proxy {}, DLSS-G {}){}",
                        app.fg_install_usable ? "installed" : app.fg_install ? "recorded but incomplete" : "proxy present without our record, not treated as installed",
                        app.fg_install ? app.fg_install->proxy : std::string("?"),
                        fg.hardware ? "enabled" : std::format("still greyed: {}", fg.reason),
                        app.fg_install ? std::format(", runtime {}", app.fg_install->runtime) : std::string());
        }
        pt::FrameGenKind fg_kind = pt::FrameGenKind::Off;
        pt::ParseFrameGen(app.settings.upscaling.frame_generation, fg_kind);
        if (app.settings.upscaling.dlssg_route_armed && app.fg_install_usable && fg_kind == pt::FrameGenKind::Dlss) {
            DisableFgRoute(app, "pc_note_fg_crash", "the crash marker was set at start: the last run crashed or lost the device with frame generation on");
            app.scene.upscale.frame_generation = pt::FrameGenKind::Off;
        } else if (app.settings.upscaling.dlssg_route_armed) {
            ArmFgRoute(app, false);
        } else if (app.fg_install_usable && fg_kind == pt::FrameGenKind::Dlss && !fg.hardware && g_fg_proxy_restored) {
            pt::LogInfo("frame generation: off for this session, the unlock's proxy that was put back loads at the next start");
            app.scene.upscale.frame_generation = pt::FrameGenKind::Off;
        } else if (app.fg_install_usable && fg_kind == pt::FrameGenKind::Dlss && !fg.hardware) {
            DisableFgRoute(app, "pc_note_fg_proxy", std::format("the unlock is installed but DLSS-G is still unavailable ({})", fg.reason));
            app.scene.upscale.frame_generation = pt::FrameGenKind::Off;
        } else if (FgRouteActive(app)) {
            ArmFgRoute(app, true);
        }
    } catch (const std::exception& e) {
        pt::LogWarn("startup: the DLSS 5 and frame generation route checks failed ({}); they stay off this start", e.what());
        app.nr_install.reset();
        app.nr_install_usable = false;
        app.fg_install.reset();
        app.fg_install_usable = false;
    }
    if (renderer_ready && app.window && !pt::devices::GamepadsReady()) {
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    }
    if (renderer_ready && test_device_lost && !d3d12) {
        app.renderer.Context().CheckDeviceLost(VK_ERROR_DEVICE_LOST, "PT_TEST_CRASH");
    }
    if (renderer_ready) pt::LogInfo("startup: texture manager and scene renderer");
    if (renderer_ready && app.textures.Init(app.renderer.Gpu()) && app.scene.Init(app.renderer, app.textures)) {
        pt::LogInfo("startup: scene renderer ready");
        app.textures.SetAnisotropy(AnisotropyFromApp(app.settings));
        app.vfs = &vfs;
        static std::string imgui_ini;
        if (app.window && !g_output_dir.empty()) {
            imgui_ini = pt::os::PathToUtf8((g_output_dir / "pt_imgui.ini"));
            if (ImGui::GetCurrentContext()) ImGui::GetIO().IniFilename = imgui_ini.c_str();
        }
        pt::LogInfo("startup: model cache");
        app.models = std::make_unique<pt::ModelCache>(vfs, app.scene, app.textures);
        bool xr_started = true;
#ifdef PT_D3D12_GAME
        if (app.xr && app.renderer.Backend() == pt::gpu::Backend::D3D12) {
            auto& device = static_cast<pt::gpu::d3d12::D3D12Device&>(app.renderer.Gpu());
            xr_started = app.xr->StartSessionD3D12(device.NativeDevice(), device.NativeQueue(), app.settings.vr.resolution_scale);
        } else
#endif
        if (app.xr) {
            xr_started = app.xr->StartSession(app.renderer.Context(), app.settings.vr.resolution_scale);
        }
        if (app.xr && !xr_started) {
            pt::LogWarn("vr: off for this run: {}", app.xr->Error());
            app.xr->Shutdown();
            app.xr.reset();
        }
        const bool viewer = !options.stage.empty() || !options.texture_test.empty();
        pt::LogInfo("startup: {}", viewer ? "viewer" : "game");
        result = viewer ? RunViewer(app, vfs) : RunGame(app, vfs);
        if (!app.options.make_survival_visions.empty()) pt::game::MakeSurvivalVisionCards(std::filesystem::absolute(app.options.make_survival_visions));
        app.renderer.Gpu().WaitIdle();
        if (app.xr) {
            app.xr->Shutdown();
        }
        app.models->Clear();
        app.models.reset();
        app.scene.Shutdown();
        app.textures.Shutdown();
    }
    app.renderer.Shutdown();
    if (app.d3d12_marker) {
        ClearD3D12Marker();
    }
    if (!app.streamline_marker.empty()) {
        std::error_code ec;
        std::filesystem::remove(app.streamline_marker, ec);
    }
    pt::PresenceStop();
    if (app.window) {
        SDL_DestroyWindow(app.window);
    }
    if (pt::devices::Stop()) SDL_Quit();
    if (app.settings.upscaling.neural_route_armed) ArmNeuralRoute(app, false);
    if (app.settings.upscaling.dlssg_route_armed) ArmFgRoute(app, false);
    if (app.update_requested) StartUpdater(app, app.update_language);
    else if (!app.routes.Empty()) StartRouteQueueUpdater(app, app.update_language);
    pt::LogExit(result, "normal end");
    return result;
}
