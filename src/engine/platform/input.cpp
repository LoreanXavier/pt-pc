#include "engine/platform/input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/core/log.h"
#include "engine/platform/controller_feedback.h"

namespace pt {

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
// prompts: a stick or trigger past half its travel is a pad in use, and pointer moves in a menu are the mouse in use
// (smaller moves are desk bumps); the first two frames after the mouse changes between look and pointer ignore moves (cursor warps)
constexpr int16_t kPromptAxis = 16384;
constexpr float kPromptMouseTravel = 12.0f;
/* Switching between look and pointer warps the cursor; the two frames after it would otherwise read as mouse use and flip the prompts. */
constexpr int kPromptMouseSettle = 2;

constexpr KeyBinding kKeyBindings[] = {
    {KeyAction::WalkForward, SDL_SCANCODE_W, 0},
    {KeyAction::WalkBack, SDL_SCANCODE_S, 0},
    {KeyAction::WalkLeft, SDL_SCANCODE_A, 0},
    {KeyAction::WalkRight, SDL_SCANCODE_D, 0},
    {KeyAction::PadUp, SDL_SCANCODE_UP, 0},
    {KeyAction::PadDown, SDL_SCANCODE_DOWN, 0},
    {KeyAction::PadLeft, SDL_SCANCODE_LEFT, 0},
    {KeyAction::PadRight, SDL_SCANCODE_RIGHT, 0},
    // the zoom is the right button alone: the player holds it and presses the action with the left one (the XMark photo's gouge is the
    // action, cross, in the original; the X key gouges too)
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
};

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
    return info;
}

}

uint32_t ScancodeFromName(std::string_view name) {
    std::string text(name);
    std::replace(text.begin(), text.end(), '_', ' ');
    const SDL_Scancode code = SDL_GetScancodeFromName(text.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? 0u : static_cast<uint32_t>(code);
}

uint32_t MouseButtonFromName(std::string_view name) {
    return name == "left" ? SDL_BUTTON_LEFT : name == "middle" ? SDL_BUTTON_MIDDLE : name == "right" ? SDL_BUTTON_RIGHT : 0u;
}

std::span<const KeyBinding> KeyBindings() {
    return kKeyBindings;
}

const KeyBinding* FirstBinding(KeyAction action, bool keys_only) {
    for (const KeyBinding& binding : kKeyBindings) {
        if (binding.action == action && (!keys_only || binding.scancode != 0)) {
            return &binding;
        }
    }
    return nullptr;
}

std::string KeyBindingName(const KeyBinding& binding) {
    if (binding.mouse) {
        return binding.mouse == SDL_BUTTON_LEFT ? "Left mouse button" : binding.mouse == SDL_BUTTON_RIGHT ? "Right mouse button" : "Middle mouse button";
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
    // letters, digits and punctuation as the current layout prints them (Z for the W key on AZERTY), the rest by SDL's key name
    const SDL_Keycode key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(binding.scancode), SDL_KMOD_NONE, false);
    const char* name = key != SDLK_UNKNOWN ? SDL_GetKeyName(key) : SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.scancode));
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
            case 0x1101: // legacy Steam Controller
            case 0x1102: // wired Steam Controller
            case 0x1105: // Bluetooth Steam Controller
            case 0x1106: // Bluetooth Steam Controller
            case 0x1142: // wireless Steam Controller
            case 0x11FF: // Steam Virtual Gamepad
            case 0x1201: // Steam Controller v2, wired
            case 0x1202: // Steam Controller v2, Bluetooth
            case 0x1205: // Steam Deck built-in controller
            case 0x1302: // Steam Controller (Triton)
            case 0x1303: // Steam Controller (Triton), Bluetooth
            case 0x1304: // Steam Controller (Proteus dongle)
            case 0x1305: // Steam Controller (Nereid dongle)
                return true;
            default: return false;
        }
    }
    return vendor == 0x0F0D && (product == 0x01AB || product == 0x0196); // HORIPAD for Steam, USB or Bluetooth
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

std::string DescribeGamepad(const GamepadInfo& info) {
    if (!info.is_gamepad) {
        return std::format("{} '{}' joystick without a gamepad mapping, VID {:04X} PID {:04X}, path {}", info.id, info.name, info.vendor,
                           info.product, info.path);
    }
    return std::format("{} '{}' type {}, {}, VID {:04X} PID {:04X}, path {}, serial '{}', player {}, rumble {}{}, touchpads {}, power {}{}", info.id,
                       info.name, info.type, info.connection, info.vendor, info.product, info.path, info.serial, info.player_index,
                       info.rumble ? "yes" : "no", info.trigger_rumble ? " (trigger motors too)" : "", info.touchpads, info.power,
                       info.is_virtual ? ", virtual" : "");
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

// The prompts follow the device used last at once; a pad connected counts as used, and a pad removed hands them to the pad used last
// before it, or to the keyboard
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
    if (SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0) {
        raw |= kRawL2;
    }
    if (SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0) {
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
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) {
            break;
        }
        any_edge_ = true;
        last_from_gamepad_ = false;
        key_pressed_since_poll_ = true;
        UsePrompts(PromptStyle{}, 0);
        for (const KeyBinding& binding : kKeyBindings) {
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
        any_edge_ = true;
        last_from_gamepad_ = false;
        UsePrompts(PromptStyle{}, 0);
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

InputState InputDevice::Poll(bool keyboard_free, MouseUse mouse, bool pads_free) {
    InputState state;
    state.gamepad_sensitivity = settings.gamepad_sensitivity;
    uint32_t raw = 0;
    const bool video = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    const bool* sdl_keys = keyboard_free && video ? SDL_GetKeyboardState(nullptr) : nullptr;
    auto key = [&](SDL_Scancode code) {
        return (sdl_keys && sdl_keys[code]) || (static_cast<size_t>(code) < injected_keys_.size() && injected_keys_[code]);
    };
    const SDL_MouseButtonFlags buttons = (mouse != MouseUse::None && video ? SDL_GetMouseState(nullptr, nullptr) : 0u) | injected_mouse_;
    // Mouse bindings count in play; on the option screen only the zoom's (the photo option, 0x1285F80 reads R3, follows the right button
    // as in play) and a left click stays a click
    auto held = [&](KeyAction action) {
        for (const KeyBinding& binding : kKeyBindings) {
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
    if (held(KeyAction::PadUp)) raw |= kRawUp;
    if (held(KeyAction::PadDown)) raw |= kRawDown;
    if (held(KeyAction::PadLeft)) raw |= kRawLeft;
    if (held(KeyAction::PadRight)) raw |= kRawRight;
    if (mouse == MouseUse::Menu && held(KeyAction::MenuColumn)) raw |= kRawR1;
    if (mouse == MouseUse::Menu && held(KeyAction::MenuQuit)) raw |= kRawR1;
    // zoom: the right mouse button (no key: Shift was too easy to press while walking with WASD, and the user wants the right button only)
    if (held(KeyAction::Zoom)) raw |= kRawR3;
    if (held(KeyAction::Act)) raw |= kRawCross;
    const bool gouge_key = key(SDL_SCANCODE_X);
    state.gouge_pressed = gouge_key && !previous_gouge_key_;
    previous_gouge_key_ = gouge_key;
    // the option screen's "Return to the house" in the street walk (L1 on a pad)
    const bool house_key = mouse == MouseUse::Menu && held(KeyAction::MenuHouse);
    state.house_pressed = house_key && !previous_house_key_;
    previous_house_key_ = house_key;
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
        // the XMark gouge is X on every device: cross on a PlayStation pad, else the face button SDL labels X (west on Xbox and
        // generic pads, north on Switch pads); square (west) when a pad labels none
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
    // the stronger of the keys and the sticks moves: an idle pad whose stick drifts past the dead zone (a tester's charging
    // Xbox One pad) took over every frame and WASD did nothing
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
    state.raw_held = raw;
    state.raw_pressed = raw & ~previous_raw_;
    previous_raw_ = raw;
    state.held = PlayerButtonsFromRaw(raw);
    state.pressed = state.held & ~previous_held_;
    if(state.gouge_pressed)state.pressed |= kPadGouge;
    previous_held_ = state.held;
    const uint32_t pad_pressed = state.raw_pressed & pad_raw;
    state.pause = pause_edge_ || ((pad_pressed & kRawStart) && !(pad_pressed & kRawSelect));
    state.confirm = confirm_edge_;
    state.cancel = cancel_edge_;
    state.click = click_edge_;
    state.right_click = right_click_edge_;
    state.pc_settings = settings_edge_;
    state.any_button = any_edge_;
    state.from_gamepad = state.from_gamepad || last_from_gamepad_;
    state.prompts = prompts_;
    pause_edge_ = confirm_edge_ = cancel_edge_ = click_edge_ = right_click_edge_ = settings_edge_ = any_edge_ = false;
    return state;
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
