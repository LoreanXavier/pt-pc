#include <cstdio>
#include <cmath>
#include <limits>
#include <random>

#include <glm/gtc/matrix_transform.hpp>

#include "game/vr_play.h"
#include "engine/xr/xr_view.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what, double value = 0.0) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s (%g)\n", what, value);
    }
}

float Distance(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b); }

}

int main() {
    const glm::vec3 logic_position(1.0f, 2.0f, 3.0f);
    const glm::vec3 feet(1.0f, 0.0f, 3.0f);
    const glm::vec3 eye(1.0f, 1.6f, 3.0f);
    const glm::vec3 original_anchor = logic_position + (feet - eye) + glm::vec3(0.0f, 1.6f, 0.0f);
    const glm::vec3 default_anchor = pt::game::VrEyeAnchor(logic_position, feet, eye, 1.6f, 0.0f);
    Expect(Distance(default_anchor, original_anchor) < 1e-6f, "zero VR height offset preserves anchor");
    const glm::vec3 raised_anchor = pt::game::VrEyeAnchor(logic_position, feet, eye, 1.6f, 0.25f);
    Expect(std::abs(raised_anchor.y - default_anchor.y - 0.25f) < 1e-6f, "VR height offset raises anchor");
    Expect(std::abs(pt::game::ClampVrHeightOffset(-2.0f) + 0.5f) < 1e-6f &&
               std::abs(pt::game::ClampVrHeightOffset(2.0f) - 0.5f) < 1e-6f &&
               pt::game::ClampVrHeightOffset(std::numeric_limits<float>::quiet_NaN()) == 0.0f,
           "VR height offset clamps to supported range");
    const glm::vec3 tracked_head_offset(0.1f, 0.2f, -0.3f);
    Expect(Distance(pt::game::ScaleVrTrackedOffset(tracked_head_offset, 1.0f), tracked_head_offset) < 1e-6f,
           "default VR world scale preserves tracked translation");
    Expect(Distance(pt::game::ScaleVrTrackedOffset(tracked_head_offset, 2.0f), tracked_head_offset * 2.0f) < 1e-6f &&
               std::abs(pt::game::ScaleVrTrackedOffset(tracked_head_offset, 2.0f).y - tracked_head_offset.y * 2.0f) < 1e-6f,
           "VR world scale affects vertical and horizontal head translation");
    const glm::vec3 eye_relative_offset(-0.032f, 0.0f, 0.0f);
    const glm::vec3 mapped_eye = pt::game::MapVrEyeOffset(tracked_head_offset, eye_relative_offset, 2.0f);
    Expect(Distance(mapped_eye, tracked_head_offset * 2.0f + eye_relative_offset) < 1e-6f,
           "world scale keeps eye offset relative to the head unscaled");
    const glm::vec3 mapped_left_eye = pt::game::MapVrEyeOffset(tracked_head_offset, glm::vec3(-0.032f, 0.0f, 0.0f), 2.0f);
    const glm::vec3 mapped_right_eye = pt::game::MapVrEyeOffset(tracked_head_offset, glm::vec3(0.032f, 0.0f, 0.0f), 2.0f);
    Expect(std::abs(Distance(mapped_left_eye, mapped_right_eye) - 0.064f) < 1e-6f,
           "world scale preserves 64 mm interpupillary distance");
    Expect(std::abs(pt::game::ClampVrWorldScale(0.1f) - 0.5f) < 1e-6f &&
               std::abs(pt::game::ClampVrWorldScale(5.0f) - 2.0f) < 1e-6f &&
               pt::game::ClampVrWorldScale(std::numeric_limits<float>::quiet_NaN()) == 1.0f,
           "VR world scale clamps to supported range");

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    float worst = 0.0f;
    for (int i = 0; i < 20000; ++i) {
        glm::quat q = glm::normalize(glm::quat(unit(rng), unit(rng), unit(rng), unit(rng)));
        if (i < 4) {
            q = glm::angleAxis(unit(rng) * 3.0f, glm::vec3(0, 1, 0)) * glm::angleAxis((i % 2 ? 1.0f : -1.0f) * 1.5707963f, glm::vec3(1, 0, 0)) *
                glm::angleAxis(unit(rng), glm::vec3(0, 0, 1));
        }
        const pt::xr::EyeFrustum frustum;
        const pt::Camera c = pt::xr::EyeCamera(glm::vec3(0.0f), q, frustum, 0.05f);
        worst = std::max({worst, Distance(c.Forward(), q * glm::vec3(0, 0, -1)), Distance(c.Up(), q * glm::vec3(0, 1, 0))});
    }
    Expect(worst < 2.0e-3f, "camera from orientation", worst);
    std::printf("orientation round trip: worst %.2e\n", worst);

    const glm::vec4 left_eye(std::tan(-0.9425f), std::tan(0.7330f), std::tan(0.8203f), std::tan(-0.8901f));
    const glm::vec4 right_eye(-left_eye.y, -left_eye.x, left_eye.z, left_eye.w);
    const glm::vec4 eyes[2] = {left_eye, right_eye};
    const glm::uvec2 size = pt::xr::StereoRenderSize(eyes, glm::uvec2(1024, 1104));
    std::printf("render size for 1024x1104 views: %ux%u\n", size.x, size.y);
    float corner_error = 0.0f;
    for (int e = 0; e < 2; ++e) {
        const pt::xr::EyeFrustum f = pt::xr::FrustumFor(eyes[e], size, eyes);
        pt::Camera c;
        c.position = glm::vec3(0.0f);
        c.fov_y = f.fov_y;
        const glm::mat4 vp = c.Projection(static_cast<float>(size.x) / static_cast<float>(size.y)) * c.View();
        const float tangents[2][2] = {{eyes[e].x, eyes[e].z}, {eyes[e].y, eyes[e].w}};
        const float uvs[2][2] = {{f.rect.x, f.rect.y}, {f.rect.x + f.rect.z, f.rect.y + f.rect.w}};
        for (int k = 0; k < 2; ++k) {
            const glm::vec4 clip = vp * glm::vec4(tangents[k][0], tangents[k][1], -1.0f, 1.0f);
            const glm::vec2 uv = glm::vec2(clip.x, clip.y) / clip.w * 0.5f + 0.5f;
            corner_error = std::max({corner_error, std::abs(uv.x - uvs[k][0]), std::abs(uv.y - uvs[k][1])});
        }
        Expect(f.rect.z * size.x > 1023.0f && f.rect.w * size.y > 1103.0f, "crop density", f.rect.w * size.y);
        std::printf("eye %d: part %.1f x %.1f pixels of %ux%u for a 1024x1104 image\n", e, f.rect.z * size.x, f.rect.w * size.y, size.x, size.y);
        Expect(f.rect.x >= -1e-5f && f.rect.y >= -1e-5f && f.rect.x + f.rect.z <= 1.0f + 1e-5f && f.rect.y + f.rect.w <= 1.0f + 1e-5f, "crop inside");
    }
    Expect(corner_error < 1.0e-4f, "crop corners", corner_error);
    std::printf("crop corners: worst %.2e uv\n", corner_error);

    pt::xr::Rig rig;
    const glm::quat head = glm::angleAxis(0.4f, glm::vec3(0, 1, 0)) * glm::angleAxis(-0.2f, glm::vec3(1, 0, 0));
    const glm::vec3 head_position(0.1f, 0.05f, -0.2f);
    rig.Recenter(head_position, head, 1.2f);
    const pt::xr::Angles start = pt::xr::AnglesOf(rig.ToWorld(head));
    Expect(std::abs(pt::xr::WrapAngle(start.yaw - 1.2f)) < 1e-4f, "recentre yaw", start.yaw);
    Expect(std::abs(start.pitch + 0.2f) < 1e-4f, "recentre pitch", start.pitch);
    Expect(glm::length(rig.Offset(head_position)) < 1e-6f, "recentre position");
    const glm::vec3 moved(0.3f, 0.0f, 0.1f);
    const glm::vec3 before = rig.Offset(moved);
    const glm::vec3 moved_up = moved + glm::vec3(0.0f, 0.2f, 0.0f);
    const glm::vec3 tracked_up = pt::game::ScaleVrTrackedOffset(rig.Offset(moved_up), 1.0f);
    const glm::vec3 tracked_level = pt::game::ScaleVrTrackedOffset(rig.Offset(moved), 1.0f);
    Expect(std::abs((tracked_up.y - tracked_level.y) - 0.2f) < 1e-5f,
           "tracked head height reaches the rendered eye position");
    rig.Turn(0.5236f, moved);
    Expect(Distance(rig.Offset(moved), before) < 1e-5f, "turn keeps the head", Distance(rig.Offset(moved), before));
    const pt::xr::Angles turned = pt::xr::AnglesOf(rig.ToWorld(head));
    Expect(std::abs(pt::xr::WrapAngle(turned.yaw - start.yaw - 0.5236f)) < 1e-4f, "turn angle", turned.yaw - start.yaw);
    const glm::vec3 left = head * glm::vec3(-0.032f, 0, 0) + moved;
    const glm::vec3 right = head * glm::vec3(0.032f, 0, 0) + moved;
    const glm::vec3 world_left = rig.Offset(left);
    const glm::vec3 world_right = rig.Offset(right);
    Expect(std::abs(Distance(world_left, world_right) - 0.064f) < 1e-5f, "ipd", Distance(world_left, world_right));
    const pt::Camera eye_camera = pt::xr::EyeCamera(world_left, rig.ToWorld(head), pt::xr::EyeFrustum{}, 0.05f);
    Expect(glm::dot(glm::normalize(world_right - world_left), eye_camera.Right()) > 0.9999f, "right eye to the right");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
