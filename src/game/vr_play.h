#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/platform/input.h"
#include "engine/platform/settings.h"
#include "engine/render/camera.h"
#include "engine/render/renderer.h"
#include "engine/xr/xr_host.h"
#include "engine/xr/xr_view.h"

namespace pt::game {

class Game;

class VrPlay {
public:
    VrPlay(xr::Host& host, const AppSettings::Vr& settings) : host_(host), settings_(settings) {}

    xr::Host& Host() { return host_; }

    void BeginLoop(bool& running);
    bool FrameWaited() const { return waited_; }

    bool ScreenMode(Game& game);

    void ApplyControls(Game& game, InputState& state, bool menu_open, bool screen, float dt);

    struct Stereo {
        Camera head;
        Camera eyes[2];
        XrTarget targets[2];
        XrTarget hud;
        VkExtent2D render{};
        xr::ViewPose poses[2];
    };
    bool PrepareStereo(Game& game, const Camera& logic, float dt, bool menu_open, Stereo& out);
    void FinishStereo(const Stereo& stereo);
    bool PrepareScreen(XrTarget& out);
    void FinishScreen();
    void EndEmpty();

    void Rumble(uint8_t large_motor, uint8_t small_motor);
    VkExtent2D EyeRenderSize() const { return {render_size_.x, render_size_.y}; }

private:
    void Place(const glm::vec3& head_local, float head_yaw_local, bool menu_open, float dt);

    xr::Host& host_;
    const AppSettings::Vr& settings_;
    xr::Rig rig_;
    bool waited_ = false;
    bool centered_ = false;
    bool screen_ = false;
    bool screen_placed_ = false;
    glm::quat screen_orientation_{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 screen_position_{0.0f};
    bool hud_placed_ = false;
    bool menu_was_open_ = false;
    float hud_yaw_ = 0.0f;
    glm::vec3 hud_position_{0.0f};
    float eye_height_ = -1.0f;
    glm::vec3 last_anchor_{0.0f};
    glm::vec3 last_correction_{0.0f};
    glm::uvec2 render_size_{0, 0};
    xr::EyeFrustum frusta_[2];
    uint32_t raw_previous_ = 0;
    bool snap_armed_ = true;
    bool settings_previous_ = false;
    int game_turns_ = 0;
    uint64_t frames_ = 0;
};

}
