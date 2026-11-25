#include "engine/platform/input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "engine/core/log.h"
#include "engine/platform/controller_feedback.h"

namespace pt {

namespace {

#ifdef _WIN32
struct CaptureHotKey {
    std::atomic<bool> pressed{false};
    std::atomic<DWORD> thread_id{0};
    std::thread thread;
    ~CaptureHotKey() {
        if (const DWORD id = thread_id.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
        if (thread.joinable()) thread.join();
    }
};
CaptureHotKey g_capture_hot_key;
#endif

}

void KeyPressLatch::ProcessEvent(const SDL_Event& event, uint32_t scancode) {
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == scancode) {
        pressed_ = true;
    }
}

namespace {

constexpr float kTouchSelectX = 768.0f / 1920.0f;
constexpr float kTouchStartX = 1152.0f / 1920.0f;
constexpr uint64_t kRumbleRefreshMs = 400;
constexpr uint32_t kRumbleDurationMs = 1000;
constexpr int kPlayerRawBits[11] = {11, 6, 7, 3, 10, 1, 9, 12, 13, 14, 15};
constexpr int16_t kPromptAxis = 16384;
constexpr float kPromptMouseTravel = 12.0f;
/* Switching between look and pointer warps the cursor; the two frames after it would otherwise read as mouse use and flip the prompts. */
constexpr int kPromptMouseSettle = 2;

constexpr KeyBinding kDefaultKeyBindings[] = {
    {KeyAction::WalkForward, SDL_SCANCODE_W, 0},
    {KeyAction::WalkBack, SDL_SCANCODE_S, 0},
    {KeyAction::WalkLeft, SDL_SCANCODE_A, 0},
    {KeyAction::WalkRight, SDL_SCANCODE_D, 0},
    {KeyAction::PadUp, SDL_SCANCODE_UP, 0},
    {KeyAction::PadDown, SDL_SCANCODE_DOWN, 0},
    {KeyAction::PadLeft, SDL_SCANCODE_LEFT, 0},
    {KeyAction::PadRight, SDL_SCANCODE_RIGHT, 0},
    {KeyAction::Zoom, 0, SDL_BUTTON_RIGHT},
    {KeyAction::Act, 0, SDL_BUTTON_LEFT},
    {KeyAction::Act, SDL_SCANCODE_RETURN, 0},
    {KeyAction::Act, SDL_SCANCODE_E, 0},
    {KeyAction::Act, SDL_SCANCODE_SPACE, 0},
    {KeyAction::Menu, SDL_SCANCODE_ESCAPE, 0},
    {KeyAction::Confirm, SDL_SCANCODE_RETURN, 0},
    {KeyAction::Confirm, SDL_SCANCODE_SPACE, 0},
    {KeyAction::Confirm, SDL_SCANCODE_E, 0},
    {KeyAction::Cancel, SDL_SCANCODE_BACKSPACE, 0},
    {KeyAction::PcSettings, SDL_SCANCODE_F10, 0},
    {KeyAction::MenuColumn, SDL_SCANCODE_TAB, 0},
    {KeyAction::MenuQuit, SDL_SCANCODE_Q, 0},
    {KeyAction::MenuHouse, SDL_SCANCODE_H, 0},
    {KeyAction::DateAuto, SDL_SCANCODE_A, 0},
    {KeyAction::DateSkip, SDL_SCANCODE_TAB, 0},
    {KeyAction::PhotoSave, SDL_SCANCODE_P, 0},
    {KeyAction::PhotoHide, SDL_SCANCODE_H, 0},
    {KeyAction::FreeRise, SDL_SCANCODE_SPACE, 0},
    {KeyAction::FreeSink, SDL_SCANCODE_LCTRL, 0},
    {KeyAction::FreeFast, SDL_SCANCODE_LSHIFT, 0},
    {KeyAction::MusicBack, SDL_SCANCODE_J, 0},
    {KeyAction::MusicForward, SDL_SCANCODE_K, 0},
    {KeyAction::MusicRestart, SDL_SCANCODE_R, 0},
    {KeyAction::MusicLoop, SDL_SCANCODE_L, 0},
    {KeyAction::MenuTabPrev, SDL_SCANCODE_Q, 0},
    {KeyAction::MenuTabNext, SDL_SCANCODE_E, 0},
    {KeyAction::MenuDonate, SDL_SCANCODE_P, 0},
    {KeyAction::MenuUpdate, SDL_SCANCODE_U, 0},
    {KeyAction::MenuCrosshair, SDL_SCANCODE_C, 0},
    {KeyAction::MenuSurvival, SDL_SCANCODE_N, 0},
    {KeyAction::SurvivalFire, 0, SDL_BUTTON_LEFT},
    {KeyAction::SurvivalReload, SDL_SCANCODE_R, 0},
    {KeyAction::SurvivalSprint, SDL_SCANCODE_LSHIFT, 0},
    {KeyAction::SurvivalSwap, SDL_SCANCODE_Q, 0},
    {KeyAction::SurvivalMelee, SDL_SCANCODE_V, 0},
    {KeyAction::SurvivalFlare, SDL_SCANCODE_G, 0},
    {KeyAction::SurvivalUse, SDL_SCANCODE_E, 0},
    {KeyAction::SurvivalPick1, SDL_SCANCODE_1, 0},
    {KeyAction::SurvivalPick2, SDL_SCANCODE_2, 0},
    {KeyAction::SurvivalPick3, SDL_SCANCODE_3, 0},
    {KeyAction::SurvivalInfo, SDL_SCANCODE_TAB, 0},
};

std::vector<KeyBinding>& MutableBindings() {
    static std::vector<KeyBinding> bindings(std::begin(kDefaultKeyBindings), std::end(kDefaultKeyBindings));
    return bindings;
}

struct RebindEntry {
    KeyAction action;
    const char* ini_name;
    int group;
};

constexpr RebindEntry kRebindEntries[] = {
    {KeyAction::WalkForward, "walk_forward", 0},
    {KeyAction::WalkBack, "walk_back", 0},
    {KeyAction::WalkLeft, "walk_left", 0},
    {KeyAction::WalkRight, "walk_right", 0},
    {KeyAction::Zoom, "zoom", 0},
    {KeyAction::Act, "act", 0},
    {KeyAction::SurvivalFire, "survival_fire", 1},
    {KeyAction::SurvivalReload, "survival_reload", 1},
    {KeyAction::SurvivalSprint, "survival_sprint", 1},
    {KeyAction::SurvivalSwap, "survival_swap", 1},
    {KeyAction::SurvivalMelee, "survival_melee", 1},
    {KeyAction::SurvivalFlare, "survival_flare", 1},
    {KeyAction::SurvivalUse, "survival_use", 1},
    {KeyAction::SurvivalInfo, "survival_info", 1},
};

KeyLayoutFunction g_key_layout = nullptr;

uint32_t EnvLayoutKey(const char32_t* letters, uint16_t scancode) {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) return static_cast<uint32_t>(letters[scancode - SDL_SCANCODE_A]);
    return SDLK_UNKNOWN;
}
uint32_t EnvRussian(uint16_t s) { return EnvLayoutKey(U"фисвуапршолдьтщзйкыегмцчня", s); }
uint32_t EnvUkrainian(uint16_t s) { return EnvLayoutKey(U"фисвуапршолдьтщзйкіегмцчня", s); }
uint32_t EnvGreek(uint16_t s) { return EnvLayoutKey(U"αβψδεφγηιξκλμνοπ;ρστθωςχυζ", s); }
uint32_t EnvArabic(uint16_t s) { return EnvLayoutKey(U"شلؤيثبلاهتنمةىخحضقسفعرصءغئ", s); }
uint32_t EnvHebrew(uint16_t s) { return EnvLayoutKey(U"שנבגקכעיןחלךצמםפ/רדאוה'סטז", s); }
uint32_t EnvGlyph(uint16_t s) { return s == SDL_SCANCODE_Q ? 0x016F : EnvLayoutKey(U"abcdefghijklmnopqrstuvwxyz", s); }

KeyLayoutFunction EnvKeyLayout() {
    static const KeyLayoutFunction layout = [] {
        const char* v = SDL_getenv("PT_TEST_KEY_LAYOUT");
        if (!v || !*v) return KeyLayoutFunction{};
        const std::string_view name(v);
        const KeyLayoutFunction f = name == "ru" ? EnvRussian : name == "uk" ? EnvUkrainian : name == "el" ? EnvGreek : name == "ar" ? EnvArabic
                                  : name == "he" ? EnvHebrew : name == "glyph" ? EnvGlyph : nullptr;
        LogInfo("input: keycaps read keyboard layout '{}' (PT_TEST_KEY_LAYOUT){}", name, f ? "" : ": unknown, the system's is used");
        return f;
    }();
    return layout;
}

bool LatinCode(uint32_t c) {
    return c < 0x250 || (c >= 0x1E00 && c < 0x1F00);
}

bool LatinText(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t code = c;
        size_t n = 1;
        if (c >= 0xF0) { code = c & 0x07; n = 4; }
        else if (c >= 0xE0) { code = c & 0x0F; n = 3; }
        else if (c >= 0xC0) { code = c & 0x1F; n = 2; }
        for (size_t k = 1; k < n && i + k < text.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += n;
        if (!LatinCode(code)) return false;
    }
    return true;
}

PromptStyle StyleOf(SDL_Gamepad* pad) {
    PromptStyle style;
    if (IsSteamGamepadId(SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad))) {
        style.device = PromptDevice::Steam;
        style.faces = {'A', 'B', 'X', 'Y'};
        return style;
    }
    const SDL_GamepadType type = SDL_GetGamepadType(pad);
    switch (type) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: style.device = PromptDevice::PlayStation; break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: style.device = PromptDevice::Nintendo; break;
        default: style.device = PromptDevice::Xbox; break;
    }
    constexpr SDL_GamepadButton kFaces[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
    for (int i = 0; i < 4; ++i) {
        switch (SDL_GetGamepadButtonLabelForType(type, kFaces[i])) {
            case SDL_GAMEPAD_BUTTON_LABEL_A: style.faces[i] = 'A'; break;
            case SDL_GAMEPAD_BUTTON_LABEL_B: style.faces[i] = 'B'; break;
            case SDL_GAMEPAD_BUTTON_LABEL_X: style.faces[i] = 'X'; break;
            case SDL_GAMEPAD_BUTTON_LABEL_Y: style.faces[i] = 'Y'; break;
            default: break;
        }
    }
    return style;
}

constexpr SDL_GamepadButton kMappedButtons[] = {
    SDL_GAMEPAD_BUTTON_SOUTH,      SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,   SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_START,      SDL_GAMEPAD_BUTTON_DPAD_UP,       SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};

uint32_t RawFromButton(int button) {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return kRawCross;
        case SDL_GAMEPAD_BUTTON_EAST: return kRawCircle;
        case SDL_GAMEPAD_BUTTON_WEST: return kRawSquare;
        case SDL_GAMEPAD_BUTTON_NORTH: return kRawTriangle;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return kRawL3;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return kRawR3;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return kRawL1;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return kRawR1;
        case SDL_GAMEPAD_BUTTON_START: return kRawStart;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return kRawUp;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return kRawDown;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return kRawLeft;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return kRawRight;
        default: return 0;
    }
}

float Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis) {
    return std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, axis)) / 32767.0f, -1.0f, 1.0f);
}

float DeadZone(float value, float dead) {
    return std::abs(value) <= dead ? 0.0f : value;
}

uint32_t TouchHalves(SDL_Gamepad* pad) {
    if (SDL_GetNumGamepadTouchpads(pad) < 1) {
        return 0;
    }
    uint32_t raw = 0;
    const int fingers = SDL_GetNumGamepadTouchpadFingers(pad, 0);
    for (int finger = 0; finger < fingers; ++finger) {
        bool down = false;
        float x = 0.0f;
        float y = 0.0f;
        float pressure = 0.0f;
        if (SDL_GetGamepadTouchpadFinger(pad, 0, finger, &down, &x, &y, &pressure) && down) {
            if (x < kTouchSelectX) {
                raw |= kRawSelect;
            }
            if (x > kTouchStartX) {
                raw |= kRawStart;
            }
        }
    }
    return raw;
}

const char* ConnectionName(SDL_JoystickConnectionState state) {
    switch (state) {
        case SDL_JOYSTICK_CONNECTION_WIRED: return "wired";
        case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
        case SDL_JOYSTICK_CONNECTION_UNKNOWN: return "connection unknown";
        default: return "connection invalid";
    }
}

std::string PowerName(SDL_Gamepad* pad) {
    int percent = -1;
    const SDL_PowerState state = SDL_GetGamepadPowerInfo(pad, &percent);
    const char* name = "unknown";
    switch (state) {
        case SDL_POWERSTATE_ON_BATTERY: name = "battery"; break;
        case SDL_POWERSTATE_NO_BATTERY: name = "no battery"; break;
        case SDL_POWERSTATE_CHARGING: name = "charging"; break;
        case SDL_POWERSTATE_CHARGED: name = "charged"; break;
        default: break;
    }
    return percent >= 0 ? std::format("{} {}%", name, percent) : std::string(name);
}

GamepadInfo Describe(SDL_Gamepad* pad) {
    GamepadInfo info;
    info.id = SDL_GetGamepadID(pad);
    info.is_gamepad = true;
    info.opened = true;
    info.is_virtual = SDL_IsJoystickVirtual(info.id);
    const char* name = SDL_GetGamepadName(pad);
    info.name = name ? name : "";
    const char* type = SDL_GetGamepadStringForType(SDL_GetGamepadType(pad));
    info.type = type ? type : "unknown";
    const char* path = SDL_GetGamepadPath(pad);
    info.path = path ? path : "";
    info.connection = ConnectionName(SDL_GetGamepadConnectionState(pad));
    const char* serial = SDL_GetGamepadSerial(pad);
    info.serial = serial ? serial : "";
    info.power = PowerName(pad);
    if (char* mapping = SDL_GetGamepadMapping(pad)) {
        info.mapping = mapping;
        SDL_free(mapping);
    }
    info.vendor = SDL_GetGamepadVendor(pad);
    info.product = SDL_GetGamepadProduct(pad);
    info.player_index = SDL_GetGamepadPlayerIndex(pad);
    info.touchpads = SDL_GetNumGamepadTouchpads(pad);
    const SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    info.rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    info.trigger_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
    info.gyro = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
    return info;
}

}

uint32_t ScancodeFromName(std::string_view name) {
    std::string text(name);
    if (text.size() == 7 && text.rfind("Digit_", 0) == 0) text = text.substr(6);
    std::replace(text.begin(), text.end(), '_', ' ');
    const SDL_Scancode code = SDL_GetScancodeFromName(text.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? 0u : static_cast<uint32_t>(code);
}

uint32_t MouseButtonFromName(std::string_view name) {
    return name == "left" ? SDL_BUTTON_LEFT : name == "middle" ? SDL_BUTTON_MIDDLE : name == "right" ? SDL_BUTTON_RIGHT : 0u;
}

std::span<const KeyBinding> KeyBindings() {
    return MutableBindings();
}

const KeyBinding* FirstBinding(KeyAction action, bool keys_only) {
    for (const KeyBinding& binding : MutableBindings()) {
        if (binding.action == action && (!keys_only || binding.scancode != 0)) {
            return &binding;
        }
    }
    return nullptr;
}

const char* KeyActionIniName(KeyAction action) {
    for (const auto& entry : kRebindEntries) {
        if (entry.action == action) return entry.ini_name;
    }
    return nullptr;
}

bool KeyActionFromIniName(std::string_view name, KeyAction& out) {
    for (const auto& entry : kRebindEntries) {
        if (name == entry.ini_name) {
            out = entry.action;
            return true;
        }
    }
    return false;
}

std::string KeyBindingToken(const KeyBinding& binding) {
    if (binding.mouse != 0) return std::format("mouse:{}", static_cast<int>(binding.mouse));
    if (binding.scancode != 0) {
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.scancode));
        if (name && *name) return std::format("key:{}", name);
    }
    return {};
}

bool ParseKeyBindingToken(std::string_view token, uint16_t& scancode, uint8_t& mouse) {
    scancode = 0;
    mouse = 0;
    if (token.starts_with("mouse:")) {
        const std::string_view num = token.substr(6);
        if (num == "1") { mouse = SDL_BUTTON_LEFT; return true; }
        if (num == "2") { mouse = SDL_BUTTON_MIDDLE; return true; }
        if (num == "3") { mouse = SDL_BUTTON_RIGHT; return true; }
        if (num == "4") { mouse = SDL_BUTTON_X1; return true; }
        if (num == "5") { mouse = SDL_BUTTON_X2; return true; }
        return false;
    }
    if (token.starts_with("key:")) {
        const std::string name(token.substr(4));
        const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
        if (code != SDL_SCANCODE_UNKNOWN) {
            scancode = static_cast<uint16_t>(code);
            return true;
        }
    }
    return false;
}

bool SetKeyBinding(KeyAction action, uint16_t scancode, uint8_t mouse) {
    if ((scancode == 0 && mouse == 0) || (scancode != 0 && mouse != 0)) return false;
    if (scancode == SDL_SCANCODE_ESCAPE) return false;
    int action_group = -1;
    for (const auto& entry : kRebindEntries) {
        if (entry.action == action) {
            action_group = entry.group;
            break;
        }
    }
    auto& bindings = MutableBindings();
    KeyBinding* target = nullptr;
    for (KeyBinding& b : bindings) {
        if (b.action == action) {
            target = &b;
            break;
        }
    }
    if (!target) return false;
    const uint16_t old_sc = target->scancode;
    const uint8_t old_ms = target->mouse;
    if (old_sc == scancode && old_ms == mouse) return true;
    if (action_group >= 0) {
        for (const auto& entry : kRebindEntries) {
            if (entry.action == action || entry.group != action_group) continue;
            for (KeyBinding& other : bindings) {
                if (other.action == entry.action) {
                    if (other.scancode == scancode && other.mouse == mouse) {
                        other.scancode = old_sc;
                        other.mouse = old_ms;
                    }
                    break;
                }
            }
        }
    }
    target->scancode = scancode;
    target->mouse = mouse;
    return true;
}

void ResetKeyBindings() {
    auto& bindings = MutableBindings();
    bindings.assign(std::begin(kDefaultKeyBindings), std::end(kDefaultKeyBindings));
}

void ApplyKeyBindings(const std::map<std::string, std::string>& overrides) {
    ResetKeyBindings();
    for (const auto& [name, token] : overrides) {
        KeyAction action{};
        uint16_t sc = 0;
        uint8_t ms = 0;
        if (KeyActionFromIniName(name, action) && ParseKeyBindingToken(token, sc, ms)) {
            SetKeyBinding(action, sc, ms);
        }
    }
}

std::map<std::string, std::string> ExportKeyBindings() {
    std::map<std::string, std::string> out;
    for (const auto& entry : kRebindEntries) {
        const KeyBinding* cur = FirstBinding(entry.action);
        if (!cur) continue;
        const KeyBinding* def = nullptr;
        for (const KeyBinding& d : kDefaultKeyBindings) {
            if (d.action == entry.action) {
                def = &d;
                break;
            }
        }
        if (def && cur->scancode == def->scancode && cur->mouse == def->mouse) continue;
        const std::string tok = KeyBindingToken(*cur);
        if (!tok.empty()) out[entry.ini_name] = tok;
    }
    return out;
}

void SetKeyLayoutForTest(KeyLayoutFunction layout) {
    g_key_layout = layout;
}

std::string KeyBindingName(const KeyBinding& binding) {
    if (binding.mouse) {
        if (binding.mouse == SDL_BUTTON_LEFT) return "Left mouse button";
        if (binding.mouse == SDL_BUTTON_RIGHT) return "Right mouse button";
        if (binding.mouse == SDL_BUTTON_MIDDLE) return "Middle mouse button";
        if (binding.mouse == SDL_BUTTON_X1) return "Mouse 4";
        if (binding.mouse == SDL_BUTTON_X2) return "Mouse 5";
        return "Mouse button";
    }
    switch (binding.scancode) {
        case SDL_SCANCODE_ESCAPE: return "Esc";
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return "Enter";
        case SDL_SCANCODE_BACKSPACE: return "Backspace";
        case SDL_SCANCODE_SPACE: return "Space";
        case SDL_SCANCODE_TAB: return "Tab";
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT: return "Shift";
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL: return "Ctrl";
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT: return "Alt";
        case SDL_SCANCODE_DELETE: return "Del";
        case SDL_SCANCODE_INSERT: return "Ins";
        case SDL_SCANCODE_PAGEUP: return "PgUp";
        case SDL_SCANCODE_PAGEDOWN: return "PgDn";
        default: break;
    }
    const SDL_Scancode scancode = static_cast<SDL_Scancode>(binding.scancode);
    const KeyLayoutFunction layout = g_key_layout ? g_key_layout : EnvKeyLayout();
    auto key_of = [&](SDL_Scancode code) {
        return layout ? static_cast<SDL_Keycode>(layout(static_cast<uint16_t>(code))) : SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
    };
    bool latin = true;
    for (const SDL_Scancode home : {SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_J, SDL_SCANCODE_K}) {
        const SDL_Keycode k = key_of(home);
        const char* n = k != SDLK_UNKNOWN ? SDL_GetKeyName(k) : nullptr;
        latin = latin && (!n || LatinText(n));
    }
    const SDL_Keycode key = key_of(scancode);
    const char* us = SDL_GetScancodeName(scancode);
    const char* name = key != SDLK_UNKNOWN ? SDL_GetKeyName(key) : us;
    if ((!latin || (name && !LatinText(name))) && us && *us) {
        static bool logged[SDL_SCANCODE_COUNT] = {};
        if (binding.scancode < SDL_SCANCODE_COUNT && !logged[binding.scancode] && name && std::string_view(name) != us) {
            logged[binding.scancode] = true;
            LogInfo("input: key {} writes '{}' in a keyboard layout of another script: its keycap names the physical key {}", us, name, us);
        }
        name = us;
    }
    return name && *name ? std::string(name) : std::format("Key {}", binding.scancode);
}

const char* PromptDeviceName(PromptDevice device) {
    switch (device) {
        case PromptDevice::PlayStation: return "PlayStation";
        case PromptDevice::Xbox: return "Xbox";
        case PromptDevice::Nintendo: return "Nintendo";
        case PromptDevice::Steam: return "Steam";
        default: return "keyboard";
    }
}

bool IsSteamGamepadId(uint16_t vendor, uint16_t product) {
    if (vendor == 0x28DE) {
        switch (product) {
            case 0x1101:
            case 0x1102:
            case 0x1105:
            case 0x1106:
            case 0x1142:
            case 0x11FF:
            case 0x1201:
            case 0x1202:
            case 0x1205:
            case 0x1302:
            case 0x1303:
            case 0x1304:
            case 0x1305:
                return true;
            default: return false;
        }
    }
    return vendor == 0x0F0D && (product == 0x01AB || product == 0x0196);
}

uint32_t PlayerButtonsFromRaw(uint32_t raw) {
    uint32_t buttons = 0;
    for (int i = 0; i < 11; ++i) {
        if (raw >> kPlayerRawBits[i] & 1u) {
            buttons |= 1u << i;
        }
    }
    return buttons;
}

void MergeFrameButtons(InputState& pending, const InputState& polled) {
    pending.pressed |= polled.pressed;
    pending.music_pressed |= polled.music_pressed;
    pending.survival_pressed |= polled.survival_pressed;
    pending.survival_held = polled.survival_held | (pending.survival_pressed & kSurvivalFire);
    pending.menu_keys_pressed |= polled.menu_keys_pressed;
    pending.capture_pose_pressed |= polled.capture_pose_pressed;
    pending.support_pressed |= polled.support_pressed;
    pending.raw_pressed |= polled.raw_pressed;
    pending.held = polled.held | (pending.pressed & (kPadGouge - 1u));
    pending.raw_held = polled.raw_held | pending.raw_pressed;
}

void SpendTickInput(InputState& pending) {
    pending.mouse_look = glm::vec2(0.0f);
    pending.gyro_turn = glm::vec2(0.0f);
    pending.pressed = 0;
    pending.raw_pressed = 0;
    pending.pause = pending.confirm = pending.cancel = pending.any_button = false;
    pending.click = pending.right_click = pending.house_pressed = false;
    pending.wheel = 0.0f;
    pending.menu_keys_pressed = 0;
    pending.music_pressed = 0;
    pending.survival_pressed = 0;
    pending.support_pressed = false;
}

std::string DescribeGamepad(const GamepadInfo& info) {
    if (!info.is_gamepad) {
        return std::format("{} '{}' joystick without a gamepad mapping, VID {:04X} PID {:04X}, path {}", info.id, info.name, info.vendor,
                           info.product, info.path);
    }
    return std::format("{} '{}' type {}, {}, VID {:04X} PID {:04X}, path {}, serial '{}', player {}, rumble {}{}, touchpads {}, power {}{}", info.id,
                       info.name, info.type, info.connection, info.vendor, info.product, info.path, info.serial, info.player_index,
                       info.rumble ? "yes" : "no", info.trigger_rumble ? " (trigger motors too)" : "", info.touchpads, info.power,
                       info.is_virtual ? ", virtual" : "") + (info.gyro ? ", gyro" : "");
}

std::vector<GamepadInfo> DescribeJoysticks() {
    std::vector<GamepadInfo> out;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; ++i) {
        const SDL_JoystickID id = ids[i];
        GamepadInfo info;
        if (SDL_IsGamepad(id)) {
            if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
                info = Describe(pad);
                SDL_CloseGamepad(pad);
                out.push_back(std::move(info));
                continue;
            }
            info.is_gamepad = true;
        }
        info.id = id;
        const char* name = SDL_GetJoystickNameForID(id);
        info.name = name ? name : "";
        const char* path = SDL_GetJoystickPathForID(id);
        info.path = path ? path : "";
        info.vendor = SDL_GetJoystickVendorForID(id);
        info.product = SDL_GetJoystickProductForID(id);
        info.is_virtual = SDL_IsJoystickVirtual(id);
        out.push_back(std::move(info));
    }
    SDL_free(ids);
    return out;
}

void LogJoysticks(uint32_t wait_ms) {
    const uint64_t until = SDL_GetTicks() + wait_ms;
    while (SDL_GetTicks() < until) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
        }
        SDL_Delay(20);
    }
    const std::vector<GamepadInfo> pads = DescribeJoysticks();
    const int version = SDL_GetVersion();
    LogInfo("pads: SDL {}.{}.{} sees {} joysticks", SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version),
            pads.size());
    for (const GamepadInfo& info : pads) {
        LogInfo("pads: {}{}", DescribeGamepad(info), info.is_gamepad && !info.opened ? " (open failed)" : "");
        if (!info.mapping.empty()) {
            LogInfo("pads: mapping {}", info.mapping);
        }
    }
}

void InputDevice::Init() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        Open(ids[i]);
    }
    SDL_free(ids);
    if (pads_.empty()) {
        LogInfo("input: no gamepad connected");
    } else {
        UsePrompts(pads_.front().style, pads_.front().id);
    }
}

void InputDevice::UsePrompts(const PromptStyle& style, uint32_t pad) {
    if (style.device != PromptDevice::Keyboard) {
        mouse_travel_ = 0.0f;
    }
    if (style == prompts_ && pad == prompt_pad_) {
        return;
    }
    const bool changed = style != prompts_;
    prompts_ = style;
    prompt_pad_ = pad;
    if (changed) {
        const Pad* p = Find(pad);
        const char* name = p ? SDL_GetGamepadName(p->handle) : nullptr;
        LogInfo("input: button prompts follow {}{}", PromptDeviceName(style.device), name ? std::format(" ('{}')", name) : std::string());
    }
}

PromptStyle InputDevice::ShownPrompts() const {
    if (settings.prompt_style <= 0 || prompts_.device == PromptDevice::Keyboard) return prompts_;
    PromptStyle style;
    switch (settings.prompt_style) {
        case 1: style.device = PromptDevice::PlayStation; break;
        case 3:
            style.device = PromptDevice::Nintendo;
            style.faces = {'B', 'A', 'Y', 'X'};
            break;
        case 4:
            style.device = PromptDevice::Steam;
            style.faces = {'A', 'B', 'X', 'Y'};
            break;
        default:
            style.device = PromptDevice::Xbox;
            style.faces = {'A', 'B', 'X', 'Y'};
            break;
    }
    return style;
}

void InputDevice::Shutdown() {
    if (Pad* pad = Find(rumble_pad_); pad) {
        if (rumble_sent_[0] || rumble_sent_[1]) {
            SDL_RumbleGamepad(pad->handle, 0, 0, 0);
        }
        if (trigger_rumble_sent_[0] || trigger_rumble_sent_[1]) {
            SDL_RumbleGamepadTriggers(pad->handle, 0, 0, 0);
        }
    }
    for (Pad& pad : pads_) {
        SDL_CloseGamepad(pad.handle);
    }
    pads_.clear();
    rumble_pad_ = 0;
    rumble_sent_[0] = rumble_sent_[1] = 0;
    trigger_rumble_sent_[0] = trigger_rumble_sent_[1] = 0;
    trigger_rumble_sent_at_ = 0;
}

SDL_Gamepad* InputDevice::LastUsedGamepad() const {
    const Pad* target = nullptr;
    for (const Pad& pad : pads_) {
        if (!target || pad.last_used > target->last_used) {
            target = &pad;
        }
    }
    return target ? target->handle : nullptr;
}

InputDevice::Pad* InputDevice::Find(uint32_t id) {
    for (Pad& pad : pads_) {
        if (pad.id == id) {
            return &pad;
        }
    }
    return nullptr;
}

void InputDevice::Open(uint32_t id) {
    if (Find(id)) {
        return;
    }
    SDL_Gamepad* handle = SDL_OpenGamepad(id);
    if (!handle) {
        LogWarn("input: cannot open gamepad {}: {}", id, SDL_GetError());
        return;
    }
    Pad pad;
    pad.handle = handle;
    pad.id = id;
    pad.style = StyleOf(handle);
    pad.trigger_rumble_supported = Describe(handle).trigger_rumble;
    pad.has_gyro = SDL_GamepadHasSensor(handle, SDL_SENSOR_GYRO);
    {
        char guid[64] = {};
        SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(handle)), guid, sizeof(guid));
        pad.guid = guid;
    }
    pads_.push_back(std::move(pad));
    LogInfo("input: gamepad opened: {} ({} connected)", DescribeGamepad(Describe(handle)), pads_.size());
}

void InputDevice::Close(uint32_t id) {
    for (auto it = pads_.begin(); it != pads_.end(); ++it) {
        if (it->id != id) {
            continue;
        }
        const char* name = SDL_GetGamepadName(it->handle);
        LogInfo("input: gamepad {} removed ('{}'), {} left", id, name ? name : "", pads_.size() - 1);
        if (it->id == rumble_pad_) {
            if (rumble_sent_[0] || rumble_sent_[1]) {
                SDL_RumbleGamepad(it->handle, 0, 0, 0);
            }
            if (trigger_rumble_sent_[0] || trigger_rumble_sent_[1]) {
                SDL_RumbleGamepadTriggers(it->handle, 0, 0, 0);
            }
        }
        SDL_CloseGamepad(it->handle);
        if (gyro_pad_ == id) {
            gyro_pad_ = 0;
            gyro_.Reset();
        }
        pads_.erase(it);
        if (rumble_pad_ == id) {
            rumble_pad_ = 0;
            rumble_sent_[0] = rumble_sent_[1] = 0;
            trigger_rumble_sent_[0] = trigger_rumble_sent_[1] = 0;
            trigger_rumble_sent_at_ = 0;
        }
        if (prompt_pad_ == id) {
            const Pad* next = nullptr;
            for (const Pad& pad : pads_) {
                if (!next || pad.last_used > next->last_used) {
                    next = &pad;
                }
            }
            if (next) {
                UsePrompts(next->style, next->id);
            } else {
                UsePrompts(PromptStyle{}, 0);
            }
        }
        return;
    }
}

uint32_t InputDevice::TouchRaw(const Pad& pad) const {
    return SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_TOUCHPAD) ? TouchHalves(pad.handle) : 0;
}

uint32_t InputDevice::HeldRaw(const Pad& pad) const {
    uint32_t raw = TouchRaw(pad);
    for (SDL_GamepadButton button : kMappedButtons) {
        if (SDL_GetGamepadButton(pad.handle, button)) {
            raw |= RawFromButton(button);
        }
    }
    const auto trigger = [&](SDL_GamepadAxis axis, bool& down) {
        const Sint16 value = SDL_GetGamepadAxis(pad.handle, axis);
        down = down ? value > 5461 : value > 10922;
        return down;
    };
    if (trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, pad.l2_down)) {
        raw |= kRawL2;
    }
    if (trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, pad.r2_down)) {
        raw |= kRawR2;
    }
    return raw;
}

glm::vec2 InputDevice::Stick(const Pad& pad, int x_axis, int y_axis) const {
    const float dead = settings.stick_dead_zone;
    return glm::vec2(DeadZone(Axis(pad.handle, static_cast<SDL_GamepadAxis>(x_axis)), dead),
                     DeadZone(Axis(pad.handle, static_cast<SDL_GamepadAxis>(y_axis)), dead));
}

void InputDevice::ProcessEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        Open(event.gdevice.which);
        if (const Pad* pad = Find(event.gdevice.which)) {
            UsePrompts(pad->style, pad->id);
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        Close(event.gdevice.which);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouse_dx_ += event.motion.xrel;
        mouse_dy_ += event.motion.yrel;
        mouse_travel_ += std::abs(event.motion.xrel) + std::abs(event.motion.yrel);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        UsePrompts(PromptStyle{}, 0);
        wheel_ += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) {
            break;
        }
        if (capturing_key_) {
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                captured_cancel_ = true;
            } else {
                captured_scancode_ = static_cast<uint16_t>(event.key.scancode);
                captured_mouse_ = 0;
            }
            captured_ready_ = true;
            break;
        }
        any_edge_ = true;
        last_from_gamepad_ = false;
        key_pressed_since_poll_ = true;
        if (event.key.scancode == SDL_SCANCODE_F9) capture_pose_edge_ = true;
        UsePrompts(PromptStyle{}, 0);
        for (const KeyBinding& binding : MutableBindings()) {
            if (binding.scancode == 0 || binding.scancode != event.key.scancode) {
                continue;
            }
            switch (binding.action) {
                case KeyAction::Menu: pause_edge_ = true; break;
                case KeyAction::Confirm: confirm_edge_ = true; break;
                case KeyAction::Cancel: cancel_edge_ = true; break;
                case KeyAction::PcSettings: settings_edge_ = true; break;
                default: break;
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (capturing_key_ && event.button.button >= 1 && event.button.button <= 5) {
            captured_scancode_ = 0;
            captured_mouse_ = event.button.button;
            captured_ready_ = true;
            break;
        }
        any_edge_ = true;
        last_from_gamepad_ = false;
        UsePrompts(PromptStyle{}, 0);
        if (event.button.button >= 1 && event.button.button <= 32) {
            mouse_down_latch_ |= SDL_BUTTON_MASK(event.button.button);
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            click_edge_ = true;
        } else if (event.button.button == SDL_BUTTON_RIGHT) {
            right_click_edge_ = true;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (Pad* pad = Find(event.gbutton.which)) {
            any_edge_ = true;
            last_from_gamepad_ = true;
            pad->last_used = ++use_counter_;
            UsePrompts(pad->style, pad->id);
            pad->event_raw |= RawFromButton(event.gbutton.button);
            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
                pad->event_raw |= TouchHalves(pad->handle);
            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                settings_edge_ = true;
                pad_back_edge_ = true;
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (event.gsensor.sensor == SDL_SENSOR_GYRO && settings.gyro != 0) {
            Pad* pad = Find(event.gsensor.which);
            if (pad && (gyro_pad_ == 0 || gyro_pad_ == pad->id)) {
                if (gyro_pad_ != pad->id) {
                    const auto stored = settings.gyro_bias.find(pad->guid);
                    if (stored != settings.gyro_bias.end()) {
                        gyro_.SetOffset({stored->second[0], stored->second[1], stored->second[2]});
                    } else {
                        gyro_.ClearOffset();
                    }
                }
                gyro_pad_ = pad->id;
                gyro_.SetTightening(settings.gyro_tightening * 0.0174532925f);
                const uint64_t time = event.gsensor.sensor_timestamp;
                const float dt = pad->gyro_time != 0 && time > pad->gyro_time ? static_cast<float>(time - pad->gyro_time) * 1.0e-9f : 0.0f;
                pad->gyro_time = time;
                if (dt > 0.0f && dt <= 0.05f) {
                    gyro_.Add(glm::vec3(event.gsensor.data[0], event.gsensor.data[1], event.gsensor.data[2]), dt,
                              std::clamp(settings.gyro_sensitivity, 0.25f, 4.0f));
                    glm::vec3 offset;
                    if (gyro_.TakeCalibrated(offset)) {
                        calibrated_guid_ = pad->guid;
                        calibrated_offset_ = {offset.x, offset.y, offset.z};
                        calibrated_ready_ = true;
                        LogInfo("input: gyro of gamepad {} calibrated, offset {:.4f} {:.4f} {:.4f} rad/s", pad->id, offset.x, offset.y, offset.z);
                    }
                }
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (Pad* pad = Find(event.gaxis.which); pad && (event.gaxis.value > kPromptAxis || event.gaxis.value < -kPromptAxis)) {
            UsePrompts(pad->style, pad->id);
        }
        break;
    default:
        break;
    }
}

void InputDevice::InjectKey(uint32_t scancode, bool down) {
    if (scancode == SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return;
    }
    injected_keys_.resize(SDL_SCANCODE_COUNT, false);
    const bool was = injected_keys_[scancode];
    injected_keys_[scancode] = down;
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = static_cast<SDL_Scancode>(scancode);
    event.key.key = SDL_GetKeyFromScancode(event.key.scancode, SDL_KMOD_NONE, false);
    event.key.down = down;
    event.key.repeat = down && was;
    ProcessEvent(event);
}

void InputDevice::ListenCaptureKeyEverywhere() {
#ifdef _WIN32
    if (g_capture_hot_key.thread.joinable()) return;
    g_capture_hot_key.thread = std::thread([] {
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        g_capture_hot_key.thread_id = GetCurrentThreadId();
        constexpr int kId = 0x7039;
        if (!RegisterHotKey(nullptr, kId, MOD_NOREPEAT, VK_F9)) {
            LogWarn("input: F9 is not free as a Windows hot key (error {}): the capture pose only with the game window in front",
                    GetLastError());
            return;
        }
        LogInfo("input: F9 writes the capture pose wherever the focus is (a Windows hot key)");
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_HOTKEY && msg.wParam == kId) g_capture_hot_key.pressed = true;
        }
        UnregisterHotKey(nullptr, kId);
    });
#endif
}

void InputDevice::InjectMouseButton(uint8_t button, bool down) {
    if (button < 1 || button > 5) {
        return;
    }
    if (down) {
        injected_mouse_ |= SDL_BUTTON_MASK(button);
    } else {
        injected_mouse_ &= ~SDL_BUTTON_MASK(button);
    }
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    ProcessEvent(event);
}

void InputDevice::InjectMouseMotion(float dx, float dy, int polls) {
    injected_motion_ = glm::vec2(dx, dy);
    injected_motion_polls_ = std::max(0, polls);
}

InputState InputDevice::Poll(bool keyboard_free, MouseUse mouse, bool pads_free) {
    InputState state;
    state.gamepad_sensitivity = settings.gamepad_sensitivity;
    uint32_t raw = 0;
    const bool video = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    const bool* sdl_keys = keyboard_free && !capturing_key_ && video ? SDL_GetKeyboardState(nullptr) : nullptr;
    auto raw_key = [&](SDL_Scancode code) {
        return !capturing_key_ && ((sdl_keys && sdl_keys[code]) || (static_cast<size_t>(code) < injected_keys_.size() && injected_keys_[code]));
    };
    if (swallowed_capture_scancode_ != 0 && !raw_key(static_cast<SDL_Scancode>(swallowed_capture_scancode_))) {
        swallowed_capture_scancode_ = 0;
    }
    auto key = [&](SDL_Scancode code) {
        if (swallowed_capture_scancode_ != 0 && static_cast<uint16_t>(code) == swallowed_capture_scancode_) return false;
        return raw_key(code);
    };
    SDL_MouseButtonFlags raw_buttons =
        (!capturing_key_ && mouse != MouseUse::None) ? (video ? SDL_GetMouseState(nullptr, nullptr) : 0u) | injected_mouse_ | mouse_down_latch_ : 0u;
    if (swallowed_capture_mouse_ != 0 && !(raw_buttons & SDL_BUTTON_MASK(swallowed_capture_mouse_))) {
        swallowed_capture_mouse_ = 0;
    }
    const SDL_MouseButtonFlags buttons =
        swallowed_capture_mouse_ != 0 ? (raw_buttons & ~SDL_BUTTON_MASK(swallowed_capture_mouse_)) : raw_buttons;
    mouse_down_latch_ = 0;
    auto held = [&](KeyAction action) {
        for (const KeyBinding& binding : MutableBindings()) {
            if (binding.action != action) {
                continue;
            }
            if (binding.scancode ? key(static_cast<SDL_Scancode>(binding.scancode))
                                 : (mouse == MouseUse::Look || (mouse == MouseUse::Menu && action == KeyAction::Zoom)) &&
                                       (buttons & SDL_BUTTON_MASK(binding.mouse))) {
                return true;
            }
        }
        return false;
    };
    glm::vec2 move(0.0f);
    if (held(KeyAction::WalkForward)) move.y += 1.0f;
    if (held(KeyAction::WalkBack)) move.y -= 1.0f;
    if (held(KeyAction::WalkRight)) move.x += 1.0f;
    if (held(KeyAction::WalkLeft)) move.x -= 1.0f;
    if (glm::length(move) > 1.0f) {
        move = glm::normalize(move);
    }
    state.left_stick = move;
    auto gameplay_owns_pad = [&](KeyAction pad_action) {
        if (mouse != MouseUse::Look) return false;
        const KeyBinding* pb = FirstBinding(pad_action, true);
        if (!pb || pb->scancode == 0) return false;
        for (const auto& entry : kRebindEntries) {
            const KeyBinding* gb = FirstBinding(entry.action, true);
            if (gb && gb->scancode == pb->scancode) return true;
        }
        return false;
    };
    if (!gameplay_owns_pad(KeyAction::PadUp) && held(KeyAction::PadUp)) raw |= kRawUp;
    if (!gameplay_owns_pad(KeyAction::PadDown) && held(KeyAction::PadDown)) raw |= kRawDown;
    if (!gameplay_owns_pad(KeyAction::PadLeft) && held(KeyAction::PadLeft)) raw |= kRawLeft;
    if (!gameplay_owns_pad(KeyAction::PadRight) && held(KeyAction::PadRight)) raw |= kRawRight;
    if (mouse == MouseUse::Menu && held(KeyAction::MenuColumn)) raw |= kRawR1;
    if (mouse == MouseUse::Menu && held(KeyAction::MenuQuit)) raw |= kRawR1;
    const bool zoom_raw = held(KeyAction::Zoom);
    if (zoom_toggle_ && mouse == MouseUse::Look) {
        if (zoom_raw && !prev_zoom_raw_) zoom_toggled_ = !zoom_toggled_;
    } else if (mouse != MouseUse::Look) {
        zoom_toggled_ = false;
    }
    prev_zoom_raw_ = zoom_raw;
    const bool zoom_active = (zoom_toggle_ && mouse == MouseUse::Look) ? zoom_toggled_ : zoom_raw;
    if (zoom_active) raw |= kRawR3;
    if (held(KeyAction::Act)) raw |= kRawCross;
    const bool gouge_key = key(SDL_SCANCODE_X);
    state.gouge_pressed = gouge_key && !previous_gouge_key_;
    previous_gouge_key_ = gouge_key;
    const bool house_key = mouse == MouseUse::Menu && held(KeyAction::MenuHouse);
    state.house_pressed = house_key && !previous_house_key_;
    previous_house_key_ = house_key;
    const bool date_auto_key = mouse == MouseUse::Menu && held(KeyAction::DateAuto);
    const bool date_skip_key = mouse == MouseUse::Menu && held(KeyAction::DateSkip);
    state.date_auto_pressed = date_auto_key && !previous_date_auto_key_;
    state.date_skip_pressed = date_skip_key && !previous_date_skip_key_;
    previous_date_auto_key_ = date_auto_key;
    previous_date_skip_key_ = date_skip_key;
    uint32_t music_keys = 0;
    if (mouse == MouseUse::Menu) {
        if (held(KeyAction::MusicBack)) music_keys |= kMusicPressBack;
        if (held(KeyAction::MusicForward)) music_keys |= kMusicPressForward;
        if (held(KeyAction::MusicRestart)) music_keys |= kMusicPressRestart;
        if (held(KeyAction::MusicLoop)) music_keys |= kMusicPressLoop;
    }
    state.music_pressed = music_keys & ~previous_music_keys_;
    previous_music_keys_ = music_keys;
    uint32_t menu_keys = 0;
    if (mouse == MouseUse::Menu) {
        const bool shift = key(SDL_SCANCODE_LSHIFT) || key(SDL_SCANCODE_RSHIFT);
        const bool tab = key(SDL_SCANCODE_TAB);
        if (held(KeyAction::MenuTabPrev) || (tab && shift)) menu_keys |= kMenuKeyTabPrev;
        if (held(KeyAction::MenuTabNext) || (tab && !shift)) menu_keys |= kMenuKeyTabNext;
        if (key(SDL_SCANCODE_E)) menu_keys |= kMenuKeyFromE;
        if (held(KeyAction::MenuDonate)) menu_keys |= kMenuKeyDonate;
        if (held(KeyAction::MenuUpdate)) menu_keys |= kMenuKeyUpdate;
        if (held(KeyAction::MenuCrosshair)) menu_keys |= kMenuKeyCrosshair;
        if (held(KeyAction::MenuSurvival)) menu_keys |= kMenuKeySurvival;
        if (tab) menu_keys |= kMenuKeyArmoryTab;
        if (held(KeyAction::SurvivalReload)) menu_keys |= kMenuKeyArmoryReplay;
        if (held(KeyAction::SurvivalMelee)) menu_keys |= kMenuKeyArmoryApplyAll;
    }
    state.menu_keys_pressed = menu_keys & ~previous_menu_keys_;
    if (!(state.menu_keys_pressed & kMenuKeyTabNext)) state.menu_keys_pressed &= ~kMenuKeyFromE;
    previous_menu_keys_ = menu_keys;
    state.capture_pose_pressed = capture_pose_edge_;
#ifdef _WIN32
    state.capture_pose_pressed = g_capture_hot_key.pressed.exchange(false) || state.capture_pose_pressed;
#endif
    capture_pose_edge_ = false;
    if (injected_motion_polls_ > 0) {
        --injected_motion_polls_;
        mouse_dx_ += injected_motion_.x;
        mouse_dy_ += injected_motion_.y;
    }
    if (mouse == MouseUse::Look) {
        state.mouse_look = glm::vec2(mouse_dx_, mouse_dy_) * settings.mouse_sensitivity;
    }
    if (mouse != last_mouse_use_) {
        last_mouse_use_ = mouse;
        mouse_settle_ = kPromptMouseSettle;
    }
    if (mouse_settle_ > 0) {
        --mouse_settle_;
    } else if (mouse == MouseUse::Menu && mouse_travel_ >= kPromptMouseTravel && !key_pressed_since_poll_) {
        UsePrompts(PromptStyle{}, 0);
    }
    key_pressed_since_poll_ = false;
    mouse_travel_ = 0.0f;
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    uint32_t pad_raw = 0;
    glm::vec2 left(0.0f);
    glm::vec2 right(0.0f);
    for (Pad& pad : pads_) {
        const uint32_t held = HeldRaw(pad) | pad.event_raw;
        const bool voice_chord = SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16383 &&
                                 SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16383;
        state.voice_keyword_pressed |= pads_free && voice_chord && !pad.previous_voice_chord;
        pad.previous_voice_chord = voice_chord;
        uint32_t gouge_button = kRawSquare;
        if (pad.style.device == PromptDevice::PlayStation) {
            gouge_button = kRawCross;
        } else {
            constexpr uint32_t kFaceBits[4] = {kRawCross, kRawCircle, kRawSquare, kRawTriangle};
            for (int i = 0; i < 4; ++i) {
                if (pad.style.faces[i] == 'X') gouge_button = kFaceBits[i];
            }
        }
        const uint32_t gouge = held & gouge_button;
        state.gouge_pressed |= pads_free && gouge && !pad.previous_gouge;
        pad.previous_gouge = gouge;
        pad.event_raw = 0;
        if (!pads_free) {
            continue;
        }
        glm::vec2 l = Stick(pad, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY);
        l.y = -l.y;
        const glm::vec2 r = Stick(pad, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY);
        if (held != 0 || glm::length(l) > 0.0f || glm::length(r) > 0.0f) {
            pad.last_used = ++use_counter_;
        }
        pad_raw |= held;
        if (glm::length(l) > glm::length(left)) {
            left = l;
        }
        if (glm::length(r) > glm::length(right)) {
            right = r;
        }
    }
    if (glm::length(left) > glm::length(move)) {
        state.left_stick = left;
        state.left_stick_from_pad = true;
        state.from_gamepad = true;
    }
    if (glm::length(right) > 0.0f) {
        state.right_stick = right;
        state.from_gamepad = true;
    }
    if (pad_raw) {
        state.from_gamepad = true;
    }
    raw |= pad_raw;
    UpdateGyroSensors();
    {
        const glm::vec2 turn = gyro_.TakeTurn();
        if (pads_free && settings.gyro == 1) {
            state.gyro_turn = turn * std::clamp(settings.gyro_sensitivity, 0.25f, 4.0f);
        } else if (pads_free && settings.gyro == 2) {
            if (pad_raw & ~previous_raw_ & kRawL3) {
                gyro_.Recenter();
            }
            state.gyro_light = gyro_.AimStick();
        }
    }
    state.raw_held = raw;
    state.raw_pressed = raw & ~previous_raw_;
    previous_raw_ = raw;
    state.held = PlayerButtonsFromRaw(raw);
    state.pressed = state.held & ~previous_held_;
    if(state.gouge_pressed)state.pressed |= kPadGouge;
    previous_held_ = state.held;
    const uint32_t pad_pressed = state.raw_pressed & pad_raw;
    if (pad_pressed & kRawL1) state.music_pressed |= kMusicPressBack;
    if (pad_pressed & kRawR1) state.music_pressed |= kMusicPressForward;
    if (pad_pressed & kRawSquare) state.music_pressed |= kMusicPressRestart;
    if (pad_pressed & kRawTriangle) state.music_pressed |= kMusicPressLoop;
    if (pad_pressed & kRawL1) state.menu_keys_pressed |= kMenuKeyTabPrev;
    if (pad_pressed & kRawR1) state.menu_keys_pressed |= kMenuKeyTabNext;
    if ((pad_pressed & kRawSelect) || pad_back_edge_) state.menu_keys_pressed |= kMenuKeyDonate;
    if (pad_pressed & kRawTriangle) state.menu_keys_pressed |= kMenuKeyUpdate;
    pad_back_edge_ = false;
    uint32_t survival = 0;
    if (mouse == MouseUse::Look) {
        uint32_t buttons = 0;
        if (held(KeyAction::SurvivalFire)) buttons |= kSurvivalFire;
        if (zoom_active) buttons |= kSurvivalAim;
        if (previous_mouse_use_ != MouseUse::Look) swallowed_survival_ = buttons;
        swallowed_survival_ &= buttons;
        survival |= buttons & ~swallowed_survival_;
        if (held(KeyAction::SurvivalReload)) survival |= kSurvivalReload;
        if (held(KeyAction::SurvivalSprint)) survival |= kSurvivalSprint;
        if (held(KeyAction::SurvivalSwap)) survival |= kSurvivalSwap;
        if (held(KeyAction::SurvivalMelee)) survival |= kSurvivalMelee;
        if (held(KeyAction::SurvivalFlare)) survival |= kSurvivalFlare;
        if (held(KeyAction::SurvivalUse)) survival |= kSurvivalUse;
        if (held(KeyAction::SurvivalInfo)) survival |= kSurvivalInfo;
        if (held(KeyAction::SurvivalPick1)) survival |= kSurvivalSlot1;
        if (held(KeyAction::SurvivalPick2)) survival |= kSurvivalSlot2;
        if (held(KeyAction::SurvivalPick3)) survival |= kSurvivalSlot3;
        if (key(SDL_SCANCODE_4)) survival |= kSurvivalSlot4;
        if (key(SDL_SCANCODE_5)) survival |= kSurvivalSlot5;
        if (key(SDL_SCANCODE_6) || key(SDL_SCANCODE_GRAVE)) survival |= kSurvivalSlot0;
    }
    if (mouse != MouseUse::None) {
        if (held(KeyAction::SurvivalPick1)) survival |= kSurvivalPick1;
        if (held(KeyAction::SurvivalPick2)) survival |= kSurvivalPick2;
        if (held(KeyAction::SurvivalPick3)) survival |= kSurvivalPick3;
    }
    if (pad_raw & kRawR2) survival |= kSurvivalFire;
    if (pad_raw & kRawL2) survival |= kSurvivalAim;
    if (pad_raw & kRawSquare) survival |= kSurvivalReload;
    if (pad_raw & kRawL3) survival |= kSurvivalSprint;
    if (pad_raw & kRawTriangle) survival |= kSurvivalSwap;
    if (pad_raw & kRawR1) survival |= kSurvivalMelee;
    if (pad_raw & kRawL1) survival |= kSurvivalFlare;
    if (pad_raw & kRawCross) survival |= kSurvivalUse;
    if (pad_raw & kRawLeft) survival |= kSurvivalPick1 | kSurvivalSwapPrev;
    if (pad_raw & kRawUp) survival |= kSurvivalPick2;
    if (pad_raw & kRawRight) survival |= kSurvivalPick3 | kSurvivalSwap;
    if (pad_raw & kRawSelect) survival |= kSurvivalInfo;
    const bool sprint_raw = (survival & kSurvivalSprint) != 0;
    if (sprint_toggle_ && mouse == MouseUse::Look) {
        if (sprint_raw && !prev_sprint_raw_) sprint_toggled_ = !sprint_toggled_;
        if (glm::length(state.left_stick) < 0.15f || (survival & (kSurvivalAim | kSurvivalReload | kSurvivalFire))) sprint_toggled_ = false;
        if (sprint_toggled_) survival |= kSurvivalSprint;
        else survival &= ~kSurvivalSprint;
    } else {
        sprint_toggled_ = false;
    }
    prev_sprint_raw_ = sprint_raw;
    previous_mouse_use_ = mouse;
    state.survival_held = survival;
    state.survival_pressed = survival & ~previous_survival_;
    if (mouse == MouseUse::Look && wheel_ != 0.0f) state.survival_pressed |= wheel_ > 0.0f ? kSurvivalSwapPrev : kSurvivalSwap;
    previous_survival_ = survival;
    {
        const bool support_key = held(KeyAction::MenuDonate);
        state.support_pressed = (support_key && !previous_support_key_) || (pad_pressed & kRawL3) != 0;
        previous_support_key_ = support_key;
    }
    state.pause = pause_edge_ || ((pad_pressed & kRawStart) && !(pad_pressed & kRawSelect));
    state.pause_held = held(KeyAction::Menu) || (pad_raw & kRawStart) != 0;
    state.confirm = confirm_edge_;
    state.cancel = cancel_edge_;
    state.click = click_edge_;
    state.click_held = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
    state.right_click = right_click_edge_;
    state.wheel = wheel_;
    wheel_ = 0.0f;
    state.pc_settings = settings_edge_;
    state.any_button = any_edge_;
    state.from_gamepad = state.from_gamepad || last_from_gamepad_;
    state.prompts = ShownPrompts();
    pause_edge_ = confirm_edge_ = cancel_edge_ = click_edge_ = right_click_edge_ = settings_edge_ = any_edge_ = false;
    return state;
}

bool InputDevice::AnyGyroPad() const {
    return std::any_of(pads_.begin(), pads_.end(), [](const Pad& pad) { return pad.has_gyro; });
}

bool InputDevice::TakeGyroCalibrated(std::string& guid, std::array<float, 3>& offset) {
    if (!calibrated_ready_) return false;
    calibrated_ready_ = false;
    guid = calibrated_guid_;
    offset = calibrated_offset_;
    return true;
}

void InputDevice::UpdateGyroSensors() {
    const bool want = settings.gyro != 0;
    bool any = false;
    for (Pad& pad : pads_) {
        const bool on = want && pad.has_gyro;
        any |= on;
        if (on == pad.gyro_on) {
            continue;
        }
        if (SDL_SetGamepadSensorEnabled(pad.handle, SDL_SENSOR_GYRO, on)) {
            pad.gyro_on = on;
            pad.gyro_time = 0;
            LogInfo("input: gyro {} on gamepad {}", on ? "enabled" : "disabled", pad.id);
        } else {
            pad.gyro_on = on;
            LogWarn("input: gyro of gamepad {} could not be switched {}: {}", pad.id, on ? "on" : "off", SDL_GetError());
        }
    }
    if (!want) {
        if (gyro_.Samples() != 0 || gyro_pad_ != 0) {
            gyro_ = GyroTracker{};
            gyro_pad_ = 0;
        }
        gyro_missing_logged_ = false;
    } else if (!any && !pads_.empty() && !gyro_missing_logged_) {
        gyro_missing_logged_ = true;
        LogInfo("input: gyro is on but no connected gamepad has a motion sensor");
    }
}

void InputDevice::SetRumble(uint8_t large_motor, uint8_t small_motor) {
    rumble_want_[0] = settings.rumble ? large_motor : 0;
    rumble_want_[1] = settings.rumble ? small_motor : 0;
    if (!settings.rumble || !settings.trigger_rumble) {
        trigger_rumble_want_[0] = trigger_rumble_want_[1] = 0;
    }
    UpdateRumble();
}

void InputDevice::SetTriggerRumble(uint8_t left, uint8_t right) {
    trigger_rumble_want_[0] = settings.rumble && settings.trigger_rumble ? left : 0;
    trigger_rumble_want_[1] = settings.rumble && settings.trigger_rumble ? right : 0;
    UpdateRumble();
}

void InputDevice::UpdateRumble() {
    const Pad* target = nullptr;
    for (const Pad& pad : pads_) {
        if (!target || pad.last_used > target->last_used) {
            target = &pad;
        }
    }
    const uint32_t id = target ? target->id : 0;
    if (id != rumble_pad_) {
        if (Pad* old = Find(rumble_pad_); old && (rumble_sent_[0] || rumble_sent_[1])) {
            SDL_RumbleGamepad(old->handle, 0, 0, 0);
        }
        if (Pad* old = Find(rumble_pad_); old && (trigger_rumble_sent_[0] || trigger_rumble_sent_[1])) {
            SDL_RumbleGamepadTriggers(old->handle, 0, 0, 0);
        }
        rumble_pad_ = id;
        rumble_sent_[0] = rumble_sent_[1] = 0;
        trigger_rumble_sent_[0] = trigger_rumble_sent_[1] = 0;
        trigger_rumble_sent_at_ = 0;
        rumble_sent_at_ = 0;
    }
    if (!target) {
        return;
    }
    const uint64_t now = SDL_GetTicks();
    const bool active = rumble_want_[0] || rumble_want_[1];
    const bool changed = rumble_want_[0] != rumble_sent_[0] || rumble_want_[1] != rumble_sent_[1];
    const TriggerRumbleLevels trigger = BuildTriggerRumble(trigger_rumble_want_[0], trigger_rumble_want_[1], settings.rumble,
                                                           settings.trigger_rumble, target->trigger_rumble_supported);
    const bool trigger_changed = trigger.left != trigger_rumble_sent_[0] || trigger.right != trigger_rumble_sent_[1];
    if (active && rumble_logged_pad_ != id) {
        rumble_logged_pad_ = id;
        const char* name = SDL_GetGamepadName(target->handle);
        LogInfo("input: vibration on gamepad {} ('{}')", id, name ? name : "");
    }
    if (changed || (active && now - rumble_sent_at_ >= kRumbleRefreshMs)) {
        SDL_RumbleGamepad(target->handle, static_cast<Uint16>(rumble_want_[0] * 257), static_cast<Uint16>(rumble_want_[1] * 257),
                          active ? kRumbleDurationMs : 0);
        rumble_sent_[0] = rumble_want_[0];
        rumble_sent_[1] = rumble_want_[1];
        rumble_sent_at_ = now;
    }
    if (trigger_changed || (trigger.Active() && now - trigger_rumble_sent_at_ >= kRumbleRefreshMs)) {
        SDL_RumbleGamepadTriggers(target->handle, trigger.left, trigger.right, trigger.Active() ? kRumbleDurationMs : 0);
        trigger_rumble_sent_[0] = trigger.left;
        trigger_rumble_sent_[1] = trigger.right;
        trigger_rumble_sent_at_ = now;
    }
}

}
