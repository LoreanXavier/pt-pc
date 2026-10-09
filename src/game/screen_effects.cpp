#include "game/screen_effects.h"

#include <algorithm>

#include "engine/core/log.h"

namespace pt::game {

void ScreenEffects::SetFadeColor(int r, int g, int b, int a) {
    if (fade_ignore) {
        return;
    }
    fade_color = glm::vec4(r, g, b, a) / 255.0f;
}

void ScreenEffects::FadeCustomSetting(int normal, int strong) {
    if (normal >= 11 && normal <= 254 && normal < fade_strong_priority) {
        fade_priority = normal;
    }
    if (strong >= 11 && strong <= 254 && strong > fade_priority) {
        fade_strong_priority = strong;
    }
}

void ScreenEffects::HoldShownFade() {
    if (fade_shown_hold <= 0.0f) {
        fade_shown = fade_current;
    }
    fade_shown_hold = 1.0f / 30.0f;
    fade_shown_called = true;
}

void ScreenEffects::CallFadeOut(float seconds) {
    HoldShownFade();
    fade_strong = false;
    fade_state = 2;
    fade_remaining = std::clamp(seconds, 0.01f, 10.0f);
    // 0x7D2050: from a clear screen the fade takes its colour at once and ramps only the alpha
    if (fade_current.a <= 0.0f) {
        const glm::vec4 target = fade_ignore ? glm::vec4(0.0f) : fade_color;
        fade_current = glm::vec4(glm::vec3(target), fade_current.a);
    }
    LogDebug("fade: out {:.2f} s", fade_remaining);
}

void ScreenEffects::CallStrongFadeOut(float seconds) {
    CallFadeOut(seconds);
    fade_strong = true;
}

void ScreenEffects::CallFadeIn(float seconds) {
    HoldShownFade();
    fade_state = 1;
    fade_remaining = std::clamp(seconds, 0.01f, 10.0f);
    fade_strong = false;
    LogDebug("fade: in {:.2f} s", fade_remaining);
}

void ScreenEffects::ShowSubliminalImage(int index, bool flag, bool no_string) {
    subliminal_image = index;
    subliminal_flag = flag;
    subliminal_no_string = no_string;
    subliminal_time = 0.0f;
}

void ScreenEffects::Update(float dt) {
    if (fade_state == 1 || fade_state == 2) {
        glm::vec4 target = fade_ignore ? glm::vec4(0.0f) : fade_color;
        if (fade_state == 1) {
            target = glm::vec4(glm::vec3(fade_current), 0.0f);
        }
        const float k = fade_remaining > 0.0f ? std::min(dt / fade_remaining, 1.0f) : 1.0f;
        fade_current += (target - fade_current) * k;
        fade_remaining -= dt;
        if (fade_remaining <= 0.0f) {
            fade_current = target;
            fade_state = fade_state == 2 ? 3 : 0;
        }
    }
    if (fade_shown_hold > 0.0f) {
        // the hold counts from the tick after the call, as a demo's first clock advance does from its start (DemoSystem::Update)
        if (fade_shown_called) {
            fade_shown_called = false;
        } else {
            fade_shown_hold -= dt;
        }
        if (fade_shown_hold <= 1e-4f) {
            fade_shown_hold = 0.0f;
            fade_shown = fade_current;
        }
    } else {
        fade_shown = fade_current;
    }
    if (subliminal_image >= 0) {
        subliminal_time += dt;
        const float length = subliminal_no_string ? 0.061f : 3.06f;
        if (subliminal_time > length) {
            subliminal_image = -1;
        }
    }
    if (caption_id != 0) {
        caption_time += dt;
    }
}

}
