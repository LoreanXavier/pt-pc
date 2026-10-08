#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

union SDL_Event;
struct SDL_Gamepad;

namespace pt {

enum PadRaw : uint32_t {
    kRawSquare = 1u << 0,
    kRawCross = 1u << 1,
    kRawCircle = 1u << 2,
    kRawTriangle = 1u << 3,
    kRawL2 = 1u << 4,
    kRawR2 = 1u << 5,
    kRawL1 = 1u << 6,
    kRawR1 = 1u << 7,
    kRawStart = 1u << 8,
    kRawSelect = 1u << 9,
    kRawL3 = 1u << 10,
    kRawR3 = 1u << 11,
    kRawUp = 1u << 12,
    kRawDown = 1u << 13,
    kRawLeft = 1u << 14,
    kRawRight = 1u << 15,
};

enum PadButton : uint32_t {
    kPadZoom = 1u << 0,
    kPadL1 = 1u << 1,
    kPadR1 = 1u << 2,
    kPadTriangle = 1u << 3,
    kPadL3 = 1u << 4,
    kPadAction = 1u << 5,
    kPadSelect = 1u << 6,
    kPadLookUp = 1u << 7,
    kPadLookDown = 1u << 8,
    kPadLookLeft = 1u << 9,
    kPadLookRight = 1u << 10,
    kPadGouge = 1u << 11,
};

uint32_t PlayerButtonsFromRaw(uint32_t raw);

enum class KeyAction : uint8_t {
    WalkForward, WalkBack, WalkLeft, WalkRight, PadUp, PadDown, PadLeft, PadRight, Zoom, Act, Menu, Confirm, Cancel, PcSettings, MenuColumn,
    MenuQuit, MenuHouse
};

struct KeyBinding {
    KeyAction action;
    uint16_t scancode;
    uint8_t mouse;
};

std::span<const KeyBinding> KeyBindings();
const KeyBinding* FirstBinding(KeyAction action, bool keys_only = false);
std::string KeyBindingName(const KeyBinding& binding);

enum class PromptDevice : uint8_t { Keyboard, PlayStation, Xbox, Nintendo, Steam };
const char* PromptDeviceName(PromptDevice device);

bool IsSteamGamepadId(uint16_t vendor, uint16_t product);

struct PromptStyle {
    PromptDevice device = PromptDevice::Keyboard;
    std::array<char, 4> faces{};

    bool operator==(const PromptStyle&) const = default;
};

struct InputState {
    glm::vec2 left_stick{0.0f};
    glm::vec2 right_stick{0.0f};
    glm::vec2 mouse_look{0.0f};
    uint32_t held = 0;
    uint32_t pressed = 0;
    uint32_t raw_held = 0;
    uint32_t raw_pressed = 0;
    bool pause = false;
    bool confirm = false;
    bool cancel = false;
    bool pc_settings = false;
    bool any_button = false;
    bool from_gamepad = false;
    bool left_stick_from_pad = false;
    bool click = false;
    bool right_click = false;
    bool gouge_pressed = false;
    bool voice_keyword_pressed = false;
    bool house_pressed = false;
    bool pointer_valid = false;
    glm::vec2 pointer{0.0f};
    PromptStyle prompts;
    bool vr_look = false;
    glm::vec2 vr_look_angles{0.0f};
    float gamepad_sensitivity = 1.0f;
};

struct InputSettings {
    float mouse_sensitivity = 0.0015f;
    float gamepad_sensitivity = 1.0f;
    float stick_dead_zone = 26.0f / 255.0f;
    bool rumble = true;
    bool trigger_rumble = false;
};

struct GamepadInfo {
    uint32_t id = 0;
    bool is_gamepad = false;
    bool opened = false;
    bool is_virtual = false;
    std::string name;
    std::string type;
    std::string path;
    std::string connection;
    std::string serial;
    std::string power;
    std::string mapping;
    uint16_t vendor = 0;
    uint16_t product = 0;
    int player_index = -1;
    int touchpads = 0;
    bool rumble = false;
    bool trigger_rumble = false;
};

std::vector<GamepadInfo> DescribeJoysticks();
std::string DescribeGamepad(const GamepadInfo& info);
void LogJoysticks(uint32_t wait_ms);
uint32_t ScancodeFromName(std::string_view name);
uint32_t MouseButtonFromName(std::string_view name);

enum class MouseUse { None, Look, Menu };

class KeyPressLatch {
public:
    void ProcessEvent(const SDL_Event& event, uint32_t scancode);
    bool Consume() { return std::exchange(pressed_, false); }
    void Discard() { pressed_ = false; }

private:
    bool pressed_ = false;
};

class InputDevice {
public:
    void Init();
    void Shutdown();
    void ProcessEvent(const SDL_Event& event);
    InputState Poll(bool keyboard_free, MouseUse mouse, bool pads_free = true);
    void SetRumble(uint8_t large_motor, uint8_t small_motor);
    void SetTriggerRumble(uint8_t left, uint8_t right);
    size_t GamepadCount() const { return pads_.size(); }
    uint32_t RumblePad() const { return rumble_pad_; }
    SDL_Gamepad* LastUsedGamepad() const;
    const PromptStyle& Prompts() const { return prompts_; }
    void InjectKey(uint32_t scancode, bool down);
    void InjectMouseButton(uint8_t button, bool down);

    InputSettings settings;

private:
    struct Pad {
        SDL_Gamepad* handle = nullptr;
        uint32_t id = 0;
        uint32_t event_raw = 0;
        uint64_t last_used = 0;
        PromptStyle style;
        uint32_t previous_gouge = 0;
        bool previous_voice_chord = false;
        bool trigger_rumble_supported = false;
    };

    void Open(uint32_t id);
    void Close(uint32_t id);
    Pad* Find(uint32_t id);
    void UsePrompts(const PromptStyle& style, uint32_t pad);
    uint32_t HeldRaw(const Pad& pad) const;
    uint32_t TouchRaw(const Pad& pad) const;
    glm::vec2 Stick(const Pad& pad, int x_axis, int y_axis) const;
    void UpdateRumble();

    std::vector<Pad> pads_;
    uint64_t use_counter_ = 0;
    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;
    uint32_t previous_held_ = 0;
    uint32_t previous_raw_ = 0;
    bool previous_gouge_key_ = false;
    bool previous_house_key_ = false;
    bool pause_edge_ = false;
    bool confirm_edge_ = false;
    bool cancel_edge_ = false;
    bool click_edge_ = false;
    bool right_click_edge_ = false;
    bool settings_edge_ = false;
    bool any_edge_ = false;
    bool last_from_gamepad_ = false;
    std::vector<bool> injected_keys_;
    uint32_t injected_mouse_ = 0;
    PromptStyle prompts_;
    uint32_t prompt_pad_ = 0;
    float mouse_travel_ = 0.0f;
    bool key_pressed_since_poll_ = false;
    MouseUse last_mouse_use_ = MouseUse::None;
    int mouse_settle_ = 0;
    uint32_t rumble_pad_ = 0;
    uint32_t rumble_logged_pad_ = 0;
    uint8_t rumble_want_[2] = {0, 0};
    uint8_t rumble_sent_[2] = {0, 0};
    uint8_t trigger_rumble_want_[2] = {0, 0};
    uint16_t trigger_rumble_sent_[2] = {0, 0};
    uint64_t trigger_rumble_sent_at_ = 0;
    uint64_t rumble_sent_at_ = 0;
};

}
