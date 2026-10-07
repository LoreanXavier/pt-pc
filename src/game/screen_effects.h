#pragma once

#include <glm/glm.hpp>

#include <string>

namespace pt::game {

struct ScreenEffects {
    static constexpr int kNormalPriority = 192;
    static constexpr int kStrongPriority = 250;

    glm::vec4 fade_color{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 fade_current{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 fade_shown{0.0f, 0.0f, 0.0f, 1.0f};
    float fade_shown_hold = 0.0f;
    bool fade_shown_called = false;
    int fade_state = 3;
    float fade_remaining = 0.0f;
    float fade_default_time = 1.0f;
    int fade_priority = kNormalPriority;
    int fade_strong_priority = kStrongPriority;
    bool fade_strong = false;
    bool fade_ignore = false;
    bool lut_control = false;
    std::string lut = "common_saturation_a";
    bool full_screen_blur = false;
    float blur_blend_rate = 0.85f;
    float blur_fetch_band = 2.5f;
    bool film_grain = true;
    float film_grain_strength = 1.0f;
    glm::vec2 grain_offset{0.0f};
    bool colour_banding_canceller = false;
    bool ev_pinned = false;
    float pinned_ev = 0.0f;
    bool screen_distortion = true;
    float reflect_scale = 1.0f;
    float reflect_bias = 0.0f;
    bool reflect_edge = false;
    bool subsurface_scatter = false;
    glm::vec3 handy_light_color{1.0f};
    bool handy_light_color_fade = false;
    bool mirror_capture = false;
    bool maze_viewport = false;
    bool subtitles_enabled = false;
    bool subtitles_visible = true;
    std::string subtitle_message_id;
    int subliminal_image = -1;
    bool subliminal_flag = false;
    bool subliminal_no_string = false;
    float subliminal_time = 0.0f;
    std::string overlay_texture;
    uint32_t caption_id = 0;
    float caption_time = 0.0f;

    float FadeAlpha() const { return fade_current.a; }
    const glm::vec4& FadeShown() const { return fade_shown; }
    void SetFadeColor(int r, int g, int b, int a);
    void FadeCustomSetting(int normal, int strong);
    void CallFadeOut(float seconds);
    void CallStrongFadeOut(float seconds);
    void CallFadeIn(float seconds);
    bool IsFadeProcessing() const { return fade_state == 1 || fade_state == 2; }
    bool IsFadeOut() const { return fade_state == 3; }
    void ShowSubliminalImage(int index, bool flag = false, bool no_string = false);
    void Update(float dt);

private:
    void HoldShownFade();
};

}
