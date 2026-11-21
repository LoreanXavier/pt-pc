#include "engine/platform/device_start.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "engine/core/crash_report.h"
#include "engine/core/log.h"
#include "engine/platform/start_watch.h"
#include "engine/platform/boot_timing.h"
#include "engine/platform/sdl_diag.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif

namespace pt::devices {
namespace {

using Clock = std::chrono::steady_clock;

enum : int { kIdle, kRunning, kDone };

std::atomic<int> g_audio{kIdle};
std::atomic<int> g_pads{kIdle};
std::atomic<bool> g_handed_over{true};
std::mutex g_mutex;
std::condition_variable g_changed;
bool g_pad_thread_over = false;
bool g_pad_thread_started = false;
#ifdef _WIN32
std::atomic<unsigned long> g_pad_thread_id{0};
#endif

double Since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

uint32_t TestDelay(const char* name) {
    const char* value = std::getenv(name);
    return value ? static_cast<uint32_t>(std::max(0, std::atoi(value))) : 0u;
}

void SetDone(std::atomic<int>& state) {
    {
        std::lock_guard lock(g_mutex);
        state = kDone;
    }
    g_changed.notify_all();
}

bool StartAudioHere(std::string_view where) {
    const auto started = Clock::now();
    const bool ok = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (const uint32_t delay = TestDelay("PT_TEST_SLOW_AUDIO_MS")) {
        LogInfo("boot: the audio start is held {} ms (PT_TEST_SLOW_AUDIO_MS)", delay);
        SDL_Delay(delay);
    }
    const double seconds = Since(started);
    if (ok) {
        LogInfo("boot: {}", boot::PhaseText("audio devices", seconds, std::format("{}driver {}", where, SDL_GetCurrentAudioDriver())));
    } else {
        LogWarn("boot: {}", boot::PhaseText("audio devices", seconds,
                                            std::format("{}none: {}; audio drivers built in: {}; the game runs without sound", where,
                                                        SDL_GetError(), SdlCompiledAudioDrivers())));
    }
    if (seconds > 2.0) LogWarn("boot: the audio devices took {:.1f} s to start", seconds);
    return ok;
}

bool StartGamepadsHere(std::string_view where) {
    const auto started = Clock::now();
    const bool ok = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (ok) {
        if (const uint32_t delay = TestDelay("PT_TEST_SLOW_GAMEPAD_MS")) {
            LogInfo("boot: the controller start is held {} ms with the joystick lock (PT_TEST_SLOW_GAMEPAD_MS)", delay);
            SDL_LockJoysticks();
            SDL_Delay(delay);
            SDL_UnlockJoysticks();
        }
    }
    int count = 0;
    if (ok) SDL_free(SDL_GetGamepads(&count));
    const double seconds = Since(started);
    if (ok) {
        LogInfo("boot: {}", boot::PhaseText("controllers", seconds, std::format("{}{} gamepad{} connected", where, count, count == 1 ? "" : "s")));
    } else {
        LogWarn("boot: {}", boot::PhaseText("controllers", seconds, std::format("{}none: {}; the game runs with the keyboard and mouse", where, SDL_GetError())));
    }
    if (seconds > 2.0) LogWarn("boot: the controllers took {:.1f} s to start", seconds);
    return ok;
}

#ifdef _WIN32
constexpr auto kDirectInputBudget = std::chrono::seconds(2);
constexpr GUID kIidDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

struct DirectInputProbe {
    std::mutex mutex;
    std::condition_variable over_changed;
    bool over = false;
    bool late = false;
    int controllers = 0;
    HRESULT result = E_FAIL;
    double seconds = 0.0;
};

BOOL CALLBACK CountController(LPCDIDEVICEINSTANCEW, LPVOID context) {
    ++*static_cast<int*>(context);
    return DIENUM_CONTINUE;
}

bool DirectInputSlow() {
    auto probe = std::make_shared<DirectInputProbe>();
    std::thread([probe] {
        const auto started = Clock::now();
        if (const uint32_t delay = TestDelay("PT_TEST_SLOW_DIRECTINPUT_MS")) Sleep(delay);
        int controllers = 0;
        HRESULT result = E_FAIL;
        if (HMODULE dll = LoadLibraryW(L"dinput8.dll")) {
            using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
            if (const auto create = reinterpret_cast<Create>(GetProcAddress(dll, "DirectInput8Create"))) {
                IDirectInput8W* input = nullptr;
                result = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, kIidDirectInput8W, reinterpret_cast<void**>(&input), nullptr);
                if (SUCCEEDED(result) && input) {
                    result = input->EnumDevices(DI8DEVCLASS_GAMECTRL, CountController, &controllers, DIEDFL_ATTACHEDONLY);
                    input->Release();
                }
            }
        }
        bool late = false;
        double seconds = Since(started);
        {
            std::lock_guard lock(probe->mutex);
            probe->over = true;
            probe->controllers = controllers;
            probe->result = result;
            probe->seconds = seconds;
            late = probe->late;
        }
        probe->over_changed.notify_all();
        if (late) LogInfo("boot: DirectInput enumeration over after {:.1f} s ({} game controllers)", seconds, controllers);
    }).detach();
    std::unique_lock lock(probe->mutex);
    if (probe->over_changed.wait_for(lock, kDirectInputBudget, [&] { return probe->over; })) {
        LogInfo("boot: {}", boot::PhaseText("DirectInput enumeration", probe->seconds,
                                            SUCCEEDED(probe->result) ? std::format("{} game controllers", probe->controllers)
                                                                     : std::format("failed, 0x{:08X}", static_cast<uint32_t>(probe->result))));
        return false;
    }
    probe->late = true;
    LogWarn("boot: DirectInput enumeration not over after {} s: the controllers start without DirectInput this time (XInput, HIDAPI "
            "and the others as usual; a pad only DirectInput knows is missing until a start where it answers in time)",
            kDirectInputBudget.count());
    return true;
}

void PadThread() {
    g_pad_thread_id = GetCurrentThreadId();
    ReserveCrashStack();
    LogInfo("boot: controller thread running");
    start_watch::TestFault("pads");
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const auto waited = Clock::now();
    while (g_audio == kRunning && !SDL_WasInit(SDL_INIT_AUDIO) && Since(waited) < 1.0) SDL_Delay(1);
    if (SDL_GetHintBoolean(SDL_HINT_JOYSTICK_DIRECTINPUT, true) && DirectInputSlow()) SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    const bool ok = StartGamepadsHere("on its own thread, ");
    SetDone(g_pads);
    if (ok) {
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    {
        std::lock_guard lock(g_mutex);
        g_pad_thread_over = true;
    }
    g_changed.notify_all();
}
#endif

}

void Start(const StartOptions& options) {
    if (options.virtual_only) {
        for (const char* hint : {SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_XINPUT_ENABLED, SDL_HINT_JOYSTICK_DIRECTINPUT, SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_JOYSTICK_WGI}) {
            SDL_SetHintWithPriority(hint, "0", SDL_HINT_OVERRIDE);
        }
    }
#ifdef _WIN32
    const bool background = options.background;
#else
    const bool background = false;
#endif
    if (!background) {
        if (options.gamepad) StartGamepadsHere("");
        if (options.audio) StartAudioHere("");
        return;
    }
#ifdef _WIN32
    LogInfo("boot: the controllers and the audio devices start on threads of their own; the window does not wait for them");
    if (options.gamepad) {
        g_handed_over = false;
        g_pads = kRunning;
        SDL_SetHintWithPriority(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0", SDL_HINT_OVERRIDE);
    }
    if (options.audio) {
        g_audio = kRunning;
        std::thread([] {
            ReserveCrashStack();
            LogInfo("boot: audio thread running");
            start_watch::TestFault("audio");
            StartAudioHere("on its own thread, ");
            SetDone(g_audio);
        }).detach();
    }
    if (options.gamepad) {
        g_pad_thread_started = true;
        std::thread(PadThread).detach();
    }
#endif
}

bool TakeGamepadsReady() {
    if (g_handed_over || g_pads != kDone) return false;
    SDL_SetHintWithPriority(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "1", SDL_HINT_OVERRIDE);
    g_handed_over = true;
    LogInfo("input: the controller start is over, the pads are read from this frame");
    return true;
}

bool GamepadsReady() {
    return g_handed_over;
}

bool AudioStarting() {
    return g_audio == kRunning;
}

bool WaitAudio(double seconds) {
    std::unique_lock lock(g_mutex);
    return g_changed.wait_for(lock, std::chrono::duration<double>(seconds), [] { return g_audio != kRunning; });
}

bool EnsureAudio() {
    if (AudioStarting()) return false;
    return SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO);
}

bool Stop() {
    bool clean = true;
    if (g_audio == kRunning) {
        clean = false;
        LogWarn("boot: the audio start still runs at the end; SDL is left to the process's end");
    }
    if (g_pad_thread_started) {
        g_pad_thread_started = false;
        if (g_pads != kDone) {
            clean = false;
            LogWarn("boot: the controller start still runs at the end; SDL is left to the process's end");
        } else {
#ifdef _WIN32
            PostThreadMessageW(g_pad_thread_id, WM_QUIT, 0, 0);
#endif
            std::unique_lock lock(g_mutex);
            if (!g_changed.wait_for(lock, std::chrono::seconds(5), [] { return g_pad_thread_over; })) {
                clean = false;
                LogWarn("boot: the controller thread did not end within 5 s; SDL is left to the process's end");
            }
        }
    }
    return clean;
}

}
