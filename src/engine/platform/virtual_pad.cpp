#include "engine/platform/virtual_pad.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

#include "engine/core/log.h"

namespace pt {
namespace {

struct Kind {
    std::string_view key;
    const char* name;
    uint16_t vendor;
    uint16_t product;
    bool touchpad;
};

constexpr Kind kKinds[] = {
    {"xbox", "Xbox Wireless Controller (virtual)", 0x045E, 0x0B13, false},
    {"ps4", "DUALSHOCK 4 Wireless Controller (virtual)", 0x054C, 0x09CC, true},
    {"ps5", "DualSense Wireless Controller (virtual)", 0x054C, 0x0CE6, true},
    {"switch", "Nintendo Switch Pro Controller (virtual)", 0x057E, 0x2009, false},
    {"steam", "Steam Deck controller (virtual)", 0x28DE, 0x1205, false},
    {"steam_virtual", "Steam Virtual Gamepad (virtual)", 0x28DE, 0x11FF, false},
    {"hori_steam", "HORIPAD for Steam (virtual)", 0x0F0D, 0x01AB, false},
    {"generic", "Generic Gamepad (virtual)", 0x0000, 0x0000, false},
};

constexpr std::pair<std::string_view, int> kButtons[] = {
    {"south", SDL_GAMEPAD_BUTTON_SOUTH},        {"cross", SDL_GAMEPAD_BUTTON_SOUTH},         {"a", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", SDL_GAMEPAD_BUTTON_EAST},          {"circle", SDL_GAMEPAD_BUTTON_EAST},         {"b", SDL_GAMEPAD_BUTTON_EAST},
    {"west", SDL_GAMEPAD_BUTTON_WEST},          {"square", SDL_GAMEPAD_BUTTON_WEST},         {"x", SDL_GAMEPAD_BUTTON_WEST},
    {"north", SDL_GAMEPAD_BUTTON_NORTH},        {"triangle", SDL_GAMEPAD_BUTTON_NORTH},      {"y", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", SDL_GAMEPAD_BUTTON_BACK},          {"share", SDL_GAMEPAD_BUTTON_BACK},          {"view", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", SDL_GAMEPAD_BUTTON_GUIDE},        {"start", SDL_GAMEPAD_BUTTON_START},         {"options", SDL_GAMEPAD_BUTTON_START},
    {"menu", SDL_GAMEPAD_BUTTON_START},         {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK},       {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},   {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},    {"r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},  {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},          {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},     {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},    {"touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD},
};

constexpr std::pair<std::string_view, int> kAxes[] = {
    {"lx", SDL_GAMEPAD_AXIS_LEFTX}, {"ly", SDL_GAMEPAD_AXIS_LEFTY}, {"rx", SDL_GAMEPAD_AXIS_RIGHTX}, {"ry", SDL_GAMEPAD_AXIS_RIGHTY},
    {"l2", SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {"lt", SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {"r2", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    {"rt", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
};

constexpr uint32_t kFaceAndDpadMask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1u;
const SDL_VirtualJoystickTouchpadDesc kTouchpad = {2, {0, 0, 0}};

uint32_t ButtonMask(const Kind& kind) {
    return kFaceAndDpadMask | (kind.touchpad ? 1u << SDL_GAMEPAD_BUTTON_TOUCHPAD : 0u);
}

bool SDLCALL OnRumble(void* userdata, Uint16 low, Uint16 high) {
    auto* rumble = static_cast<VirtualRumble*>(userdata);
    rumble->low = low;
    rumble->high = high;
    rumble->peak_low = std::max(rumble->peak_low, low);
    rumble->peak_high = std::max(rumble->peak_high, high);
    ++rumble->calls;
    return true;
}

}

VirtualPads::~VirtualPads() {
    DetachAll();
}

void VirtualPads::UseOnlyVirtualDevices() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

VirtualPads::Slot* VirtualPads::Get(int slot) {
    return slot >= 0 && slot < kSlots ? &slots_[static_cast<size_t>(slot)] : nullptr;
}

bool VirtualPads::Attached(int slot) const {
    return slot >= 0 && slot < kSlots && slots_[static_cast<size_t>(slot)].joystick != nullptr;
}

uint32_t VirtualPads::Id(int slot) const {
    return Attached(slot) ? slots_[static_cast<size_t>(slot)].id : 0;
}

bool VirtualPads::Attach(int slot, std::string_view kind) {
    Slot* s = Get(slot);
    const Kind* k = nullptr;
    for (const Kind& candidate : kKinds) {
        if (candidate.key == kind) {
            k = &candidate;
        }
    }
    if (!s || !k) {
        LogWarn("virtual pad: cannot attach slot {} kind '{}'", slot, kind);
        return false;
    }
    if (s->joystick) {
        Detach(slot);
    }
    s->rumble = std::make_unique<VirtualRumble>();
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = k->vendor;
    desc.product_id = k->product;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.button_mask = ButtonMask(*k);
    desc.nbuttons = static_cast<Uint16>(std::popcount(desc.button_mask));
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    desc.ntouchpads = k->touchpad ? 1 : 0;
    desc.touchpads = k->touchpad ? &kTouchpad : nullptr;
    desc.name = k->name;
    desc.userdata = s->rumble.get();
    desc.Rumble = OnRumble;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id) {
        LogError("virtual pad: attach failed: {}", SDL_GetError());
        s->rumble.reset();
        return false;
    }
    s->joystick = SDL_OpenJoystick(id);
    if (!s->joystick) {
        LogError("virtual pad: open failed: {}", SDL_GetError());
        SDL_DetachVirtualJoystick(id);
        s->rumble.reset();
        return false;
    }
    s->id = id;
    s->kind = std::string(kind);
    LogInfo("virtual pad {}: '{}' attached as joystick {}", slot, k->name, id);
    return true;
}

bool VirtualPads::Detach(int slot) {
    Slot* s = Get(slot);
    if (!s || !s->joystick) {
        return false;
    }
    SDL_CloseJoystick(s->joystick);
    SDL_DetachVirtualJoystick(s->id);
    LogInfo("virtual pad {}: joystick {} detached", slot, s->id);
    *s = Slot{};
    return true;
}

void VirtualPads::DetachAll() {
    for (int slot = 0; slot < kSlots; ++slot) {
        Detach(slot);
    }
}

bool VirtualPads::SetButton(int slot, std::string_view name, bool down) {
    Slot* s = Get(slot);
    if (!s || !s->joystick) {
        return false;
    }
    for (const auto& [key, button] : kButtons) {
        if (key != name) {
            continue;
        }
        const Kind* kind = nullptr;
        for (const Kind& candidate : kKinds) {
            if (candidate.key == s->kind) {
                kind = &candidate;
            }
        }
        const uint32_t mask = kind ? ButtonMask(*kind) : kFaceAndDpadMask;
        if (!(mask & (1u << button))) {
            LogWarn("virtual pad {}: no button '{}' on a {} pad", slot, name, s->kind);
            return false;
        }
        const int index = std::popcount(mask & ((1u << button) - 1u));
        return SDL_SetJoystickVirtualButton(s->joystick, index, down);
    }
    LogWarn("virtual pad {}: unknown button '{}'", slot, name);
    return false;
}

bool VirtualPads::SetAxis(int slot, std::string_view name, float value) {
    Slot* s = Get(slot);
    if (!s || !s->joystick) {
        return false;
    }
    for (const auto& [key, axis] : kAxes) {
        if (key != name) {
            continue;
        }
        long raw = 0;
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            raw = -32768 + std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f);
        } else {
            raw = std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
        }
        return SDL_SetJoystickVirtualAxis(s->joystick, axis, static_cast<Sint16>(std::clamp(raw, -32768L, 32767L)));
    }
    LogWarn("virtual pad {}: unknown axis '{}'", slot, name);
    return false;
}

bool VirtualPads::SetTouch(int slot, bool down, float x, float y) {
    Slot* s = Get(slot);
    if (!s || !s->joystick) {
        return false;
    }
    return SDL_SetJoystickVirtualTouchpad(s->joystick, 0, 0, down, std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f), down ? 1.0f : 0.0f);
}

VirtualRumble VirtualPads::TakeRumble(int slot) {
    Slot* s = Get(slot);
    if (!s || !s->rumble) {
        return {};
    }
    VirtualRumble out = *s->rumble;
    s->rumble->peak_low = s->rumble->low;
    s->rumble->peak_high = s->rumble->high;
    s->rumble->calls = 0;
    return out;
}

}
