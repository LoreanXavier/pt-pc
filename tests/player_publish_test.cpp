#include <cmath>
#include <cstdio>

#include "engine/fs/vfs.h"
#include "game/player.h"
#include "game/player_animation.h"

int main(int argc, char** argv) {
    using namespace pt;
    using namespace pt::game;
    int failures = 0;
    const auto check = [&](const char* name, bool ok) {
        std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    auto wrap = [](float a) { return std::remainder(a, 6.2831853f); };
    pt::Vfs vfs;
    if (argc != 2 || !vfs.Mount(argv[1]) || !vfs.LoadPackage("/Assets/sh/level/common/resident.fpk") || !LoadPlayerAnimation(vfs) ||
        !PlayerLocomotionData().loaded) {
        std::puts("needs the game folder");
        return 2;
    }
    Player player;
    CollisionWorld world;
    PlayerFrameContext context;
    InputState input;
    player.Warp(glm::vec3(0.0f), 0.0f);
    check("warp publishes the new yaw", std::abs(player.PublishedBodyFoxYaw()) < 1e-5f);
    input.right_stick = glm::vec2(1.0f, 0.0f);
    input.from_gamepad = true;
    float largest_lag = 0.0f;
    float previous_frame_body = player.BodyFoxYaw();
    bool lags_one_frame = true;
    bool after_publish_lags = true;
    bool changes_on_frames_only = true;
    float last_seen = player.PublishedBodyFoxYaw();
    int frames = 0;
    for (int tick = 0; tick < 20; ++tick) {
        player.Update(1.0f / 60, input, world, context);
        const bool frame_end = player.FrameEnds();
        if (frame_end) {
            ++frames;
            lags_one_frame = lags_one_frame && std::abs(wrap(player.PublishedBodyFoxYaw() - previous_frame_body)) < 1e-5f;
            largest_lag = std::max(largest_lag, std::abs(wrap(player.CameraFoxYaw() - player.PublishedBodyFoxYaw())));
        }
        const float before_end = player.PublishedBodyFoxYaw();
        player.EndFrame();
        if (frame_end) {
            after_publish_lags = after_publish_lags && std::abs(wrap(player.PublishedBodyFoxYaw() - before_end)) < 1e-6f;
            previous_frame_body = player.BodyFoxYaw();
        } else if (std::abs(wrap(player.PublishedBodyFoxYaw() - last_seen)) > 1e-6f) {
            changes_on_frames_only = changes_on_frames_only && tick > 0;
        }
        last_seen = player.PublishedBodyFoxYaw();
    }
    check("after the publishing tick's EndFrame the read buffer still holds the frame before", after_publish_lags);
    check("the read buffer turns over only in the tick after a frame's end", changes_on_frames_only);
    check("ten trap frames in twenty ticks", frames == 10);
    std::printf("largest camera to published body lag in the turn: %.1f degrees\n", largest_lag * 57.29578f);
    check("the trap reads the body yaw of the previous frame", lags_one_frame);
    check("the published body yaw trails the camera in a fast turn", largest_lag > 0.1f);
    check("the body turned", std::abs(wrap(player.BodyFoxYaw())) > 0.1f);
    input = InputState{};
    for (int tick = 0; tick < 600; ++tick) {
        player.Update(1.0f / 60, input, world, context);
        player.EndFrame();
    }
    check("at rest the published body yaw meets the camera yaw", std::abs(wrap(player.CameraFoxYaw() - player.PublishedBodyFoxYaw())) < 0.01f);
    check("standing at rest", player.Standing());
    input.left_stick = glm::vec2(0.0f, 1.0f);
    input.left_stick_from_pad = true;
    int walk_tick = -1;
    bool previous_end = false;
    bool turned_after_end = false;
    for (int tick = 0; tick < 8 && walk_tick < 0; ++tick) {
        player.Update(1.0f / 60, input, world, context);
        const bool frame_end = player.FrameEnds();
        player.EndFrame();
        if (player.Walking()) {
            walk_tick = tick;
            turned_after_end = previous_end;
        }
        previous_end = frame_end;
    }
    std::printf("Walking() from tick %d of the push\n", walk_tick);
    check("the walk flag is read a frame after the frame that set it", walk_tick >= 2 && turned_after_end);
    glm::vec3 frame_before = player.Feet();
    bool feet_lag = true;
    int moved = 0;
    for (int tick = 0; tick < 120; ++tick) {
        player.Update(1.0f / 60, input, world, context);
        const bool frame_end = player.FrameEnds();
        player.EndFrame();
        if (frame_end) {
            feet_lag = feet_lag && glm::length(player.PublishedFeet() - frame_before) < 1e-5f;
            moved += glm::length(player.Feet() - frame_before) > 1e-4f;
            frame_before = player.Feet();
        }
    }
    check("a walk moved the player", moved > 10);
    check("the published feet are the previous frame's after the publish", feet_lag);
    input = InputState{};
    input.right_stick = glm::vec2(0.0f, -1.0f);
    input.from_gamepad = true;
    float pitch_before = player.pitch;
    bool pitch_lag = true;
    bool pitch_moved = false;
    for (int tick = 0; tick < 40; ++tick) {
        player.Update(1.0f / 60, input, world, context);
        const bool frame_end = player.FrameEnds();
        player.EndFrame();
        if (frame_end) {
            pitch_lag = pitch_lag && std::abs(player.PublishedPitch() - pitch_before) < 1e-6f;
            pitch_moved = pitch_moved || std::abs(player.pitch - pitch_before) > 1e-4f;
            pitch_before = player.pitch;
        }
    }
    check("the published pitch is the previous frame's", pitch_lag && pitch_moved);
    Player default_look;
    Player fast_look;
    Player dead_zone_look;
    default_look.Warp(glm::vec3(0.0f), 0.0f);
    fast_look.Warp(glm::vec3(0.0f), 0.0f);
    dead_zone_look.Warp(glm::vec3(0.0f), 0.0f);
    InputState default_input;
    default_input.right_stick = glm::vec2(0.5f, 0.0f);
    default_input.from_gamepad = true;
    default_input.gamepad_sensitivity = 1.0f;
    InputState fast_input = default_input;
    fast_input.gamepad_sensitivity = 2.0f;
    InputState dead_zone_input = default_input;
    dead_zone_input.right_stick = glm::vec2(0.08f, 0.0f);
    dead_zone_input.gamepad_sensitivity = 5.0f;
    for (int tick = 0; tick < 120; ++tick) {
        default_look.Update(1.0f / 60, default_input, world, context);
        fast_look.Update(1.0f / 60, fast_input, world, context);
        dead_zone_look.Update(1.0f / 60, dead_zone_input, world, context);
        default_look.EndFrame();
        fast_look.EndFrame();
        dead_zone_look.EndFrame();
    }
    const float default_turn = std::abs(wrap(default_look.CameraFoxYaw()));
    const float fast_turn = std::abs(wrap(fast_look.CameraFoxYaw()));
    const float dead_zone_turn = std::abs(wrap(dead_zone_look.CameraFoxYaw()));
    check("gamepad sensitivity 1 preserves the original turning response", default_turn > 0.1f);
    check("higher gamepad sensitivity speeds up turning", fast_turn > default_turn);
    check("sensitivity does not move the original stick dead zone", dead_zone_turn < 1e-5f);
    check("sensitivity leaves the flashlight stick response unchanged", glm::length(default_look.LightStick() - fast_look.LightStick()) < 1e-5f);
    player.Warp(glm::vec3(5.0f, 0.0f, 5.0f), 1.0f);
    check("a warp is published at once", glm::length(player.PublishedFeet() - glm::vec3(5.0f, 0.0f, 5.0f)) < 1e-5f);
    return failures ? 1 : 0;
}
