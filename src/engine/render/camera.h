#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace pt {

struct Camera {
    glm::vec3 position{0.0f, 1.6f, 5.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float fov_y = glm::radians(60.0f);
    float near_plane = 0.05f;
    // the far clip: the projection has none (reverse Z to infinity), but the SSAO fade ends at min(far, 250) (0xDA39F0); the
    // game camera holds 100 m in play and in demos (rendering.md 12.17, boot_dof), a demo's farClip functor sets its own
    /* Not a clip distance (the projection is reverse-Z to infinity); it only bounds the SSAO fade, which the original ends at min(far, 250). */
    float far_plane = 100.0f;

    glm::vec3 Forward() const {
        return glm::normalize(glm::vec3(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)));
    }

    glm::vec3 Up() const {
        const glm::vec3 up(std::sin(yaw) * std::sin(pitch), std::cos(pitch), std::cos(yaw) * std::sin(pitch));
        return roll == 0.0f ? up : glm::normalize(glm::angleAxis(roll, Forward()) * up);
    }

    glm::vec3 Right() const {
        return glm::normalize(glm::cross(Forward(), Up()));
    }

    glm::mat4 View() const {
        return glm::lookAt(position, position + Forward(), Up());
    }

    glm::mat4 Projection(float aspect) const {
        const float f = 1.0f / std::tan(fov_y * 0.5f);
        glm::mat4 p(0.0f);
        p[0][0] = f / aspect;
        p[1][1] = -f;
        p[2][3] = -1.0f;
        p[3][2] = near_plane;
        return p;
    }
};

// A transform between two game ticks, for the frames a window draws between them: rotation slerped, scale and translation
// mixed (per element for a mirrored or degenerate transform); a move over 1 m in the tick is a cut and keeps the later one
inline glm::mat4 BlendTransform(const glm::mat4& from, const glm::mat4& to, float t) {
    if (t >= 1.0f || from == to || glm::distance(glm::vec3(from[3]), glm::vec3(to[3])) > 1.0f) {
        return to;
    }
    const glm::vec3 scale_from(glm::length(glm::vec3(from[0])), glm::length(glm::vec3(from[1])), glm::length(glm::vec3(from[2])));
    const glm::vec3 scale_to(glm::length(glm::vec3(to[0])), glm::length(glm::vec3(to[1])), glm::length(glm::vec3(to[2])));
    const float smallest =
        glm::min(glm::min(glm::min(scale_from.x, scale_from.y), glm::min(scale_from.z, scale_to.x)), glm::min(scale_to.y, scale_to.z));
    if (smallest < 1e-6f || glm::determinant(glm::mat3(from)) <= 0.0f || glm::determinant(glm::mat3(to)) <= 0.0f) {
        return from + (to - from) * t;
    }
    const glm::mat3 rotation_from(glm::vec3(from[0]) / scale_from.x, glm::vec3(from[1]) / scale_from.y, glm::vec3(from[2]) / scale_from.z);
    const glm::mat3 rotation_to(glm::vec3(to[0]) / scale_to.x, glm::vec3(to[1]) / scale_to.y, glm::vec3(to[2]) / scale_to.z);
    const glm::quat rotation = glm::slerp(glm::quat_cast(rotation_from), glm::quat_cast(rotation_to), t);
    const glm::vec3 scale = glm::mix(scale_from, scale_to, t);
    glm::mat4 out = glm::mat4_cast(rotation);
    out[0] *= scale.x;
    out[1] *= scale.y;
    out[2] *= scale.z;
    out[3] = glm::mix(from[3], to[3], t);
    return out;
}

// Keep a rotating actor root and its counter-animated bones in the same space.
// inverse_blended_world is computed once per draw, shared by all its bones.
inline glm::mat4 BlendSkinTransform(const glm::mat4& from_world, const glm::mat4& to_world,
                                    const glm::mat4& inverse_blended_world, const glm::mat4& from_skin,
                                    const glm::mat4& to_skin, float t) {
    const glm::mat4 from = from_world * from_skin;
    const glm::mat4 to = to_world * to_skin;
    return inverse_blended_world * (from + (to - from) * t);
}


}
