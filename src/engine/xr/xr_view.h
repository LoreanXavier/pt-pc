#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

#include "engine/render/camera.h"

namespace pt::xr {

inline float WrapAngle(float a) {
    constexpr float kTwoPi = 6.28318530718f;
    a = std::fmod(a + 3.14159265359f, kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a - 3.14159265359f;
}

inline glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

struct Angles {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

inline Angles AnglesOf(const glm::quat& q) {
    const glm::vec3 f = q * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 u = q * glm::vec3(0.0f, 1.0f, 0.0f);
    Angles a;
    a.pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
    const float horizontal = std::sqrt(f.x * f.x + f.z * f.z);
    a.yaw = horizontal > 1.0e-4f ? std::atan2(-f.x, -f.z) : std::atan2(f.y > 0.0f ? u.x : -u.x, f.y > 0.0f ? u.z : -u.z);
    const glm::vec3 up0(std::sin(a.yaw) * std::sin(a.pitch), std::cos(a.pitch), std::cos(a.yaw) * std::sin(a.pitch));
    a.roll = std::atan2(glm::dot(f, glm::cross(up0, u)), glm::dot(up0, u));
    return a;
}

struct EyeFrustum {
    float fov_y = 1.0f;
    float tan_x = 1.0f;
    float tan_y = 1.0f;
    glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
};

inline glm::uvec2 StereoRenderSize(const glm::vec4 tangents[2], glm::uvec2 swapchain) {
    float tx = 0.0f;
    float ty = 0.0f;
    float density_x = 0.0f;
    float density_y = 0.0f;
    for (int i = 0; i < 2; ++i) {
        tx = std::max({tx, -tangents[i].x, tangents[i].y});
        ty = std::max({ty, tangents[i].z, -tangents[i].w});
        density_x = std::max(density_x, static_cast<float>(swapchain.x) / std::max(tangents[i].y - tangents[i].x, 1.0e-3f));
        density_y = std::max(density_y, static_cast<float>(swapchain.y) / std::max(tangents[i].z - tangents[i].w, 1.0e-3f));
    }
    const float density = std::max(density_x, density_y);
    return {std::clamp(static_cast<uint32_t>(std::ceil(2.0f * tx * density)), 16u, 8192u),
            std::clamp(static_cast<uint32_t>(std::ceil(2.0f * ty * density)), 16u, 8192u)};
}

inline EyeFrustum FrustumFor(const glm::vec4& tangents, glm::uvec2 render_size, const glm::vec4 both[2]) {
    EyeFrustum f;
    float ty = 0.0f;
    float tx_needed = 0.0f;
    for (int i = 0; i < 2; ++i) {
        ty = std::max({ty, both[i].z, -both[i].w});
        tx_needed = std::max({tx_needed, -both[i].x, both[i].y});
    }
    const float aspect = static_cast<float>(render_size.x) / static_cast<float>(std::max(render_size.y, 1u));
    f.tan_y = std::max(ty, tx_needed / aspect);
    f.tan_x = f.tan_y * aspect;
    f.fov_y = 2.0f * std::atan(f.tan_y);
    f.rect.x = (tangents.x + f.tan_x) / (2.0f * f.tan_x);
    f.rect.z = (tangents.y - tangents.x) / (2.0f * f.tan_x);
    f.rect.y = (f.tan_y - tangents.z) / (2.0f * f.tan_y);
    f.rect.w = (tangents.z - tangents.w) / (2.0f * f.tan_y);
    return f;
}

class Rig {
public:
    void Recenter(const glm::vec3& head_position, const glm::quat& head_orientation, float world_yaw) {
        center_ = head_position;
        center_yaw_ = AnglesOf(head_orientation).yaw;
        base_yaw_ = world_yaw;
    }
    void Turn(float radians, const glm::vec3& head_position) {
        const glm::quat old_rotation = Rotation();
        base_yaw_ = WrapAngle(base_yaw_ + radians);
        center_ = head_position - glm::inverse(Rotation()) * (old_rotation * (head_position - center_));
    }
    float BaseYaw() const { return base_yaw_; }
    glm::quat Rotation() const { return YawRotation(WrapAngle(base_yaw_ - center_yaw_)); }
    glm::quat ToWorld(const glm::quat& local) const { return glm::normalize(Rotation() * local); }
    glm::vec3 Offset(const glm::vec3& local) const { return Rotation() * (local - center_); }
    glm::quat ToLocal(const glm::quat& world) const { return glm::normalize(glm::inverse(Rotation()) * world); }

private:
    glm::vec3 center_{0.0f};
    float center_yaw_ = 0.0f;
    float base_yaw_ = 0.0f;
};

inline Camera EyeCamera(const glm::vec3& position, const glm::quat& orientation, const EyeFrustum& frustum, float near_plane) {
    Camera c;
    const Angles a = AnglesOf(orientation);
    c.position = position;
    c.yaw = a.yaw;
    c.pitch = a.pitch;
    c.roll = a.roll;
    c.fov_y = frustum.fov_y;
    c.near_plane = near_plane;
    return c;
}

}
