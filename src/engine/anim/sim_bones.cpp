#include "engine/anim/sim_bones.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace pt::anim {
namespace {

struct SimChainDef {
    int b0, b1, b2, b3;
    glm::vec3 rest[4];
};

// 16 strips of 4 bones authored in /Assets/sh/chara/plr/Fox_Files/plr0_main0_def_s00.sim
// Bones 112..175 (SKL_600_JAKET_SIM .. SKL_663_JAKET_SIM)
const SimChainDef kJacketDefs[16] = {
    // Chain 0: bones 112..115 (SKL_600..SKL_603, front-left flap at zipper)
    { 112, 113, 114, 115,
      {glm::vec3(0.06f, 0.204f, 0.151f), glm::vec3(0.064f, 0.12f, 0.158f), glm::vec3(0.068f, 0.053f, 0.158f), glm::vec3(0.073f, -0.034f, 0.161f)} },
    // Chain 1: bones 116..119 (SKL_604..SKL_607)
    { 116, 117, 118, 119,
      {glm::vec3(0.117f, 0.195f, 0.144f), glm::vec3(0.109f, 0.115f, 0.139f), glm::vec3(0.109f, 0.041f, 0.139f), glm::vec3(0.118f, -0.03f, 0.144f)} },
    // Chain 2: bones 120..123 (SKL_608..SKL_611)
    { 120, 121, 122, 123,
      {glm::vec3(0.157f, 0.187f, 0.1f), glm::vec3(0.166f, 0.109f, 0.115f), glm::vec3(0.17f, 0.034f, 0.108f), glm::vec3(0.181f, -0.034f, 0.103f)} },
    // Chain 3: bones 124..127 (SKL_612..SKL_615)
    { 124, 125, 126, 127,
      {glm::vec3(0.181f, 0.179f, 0.044f), glm::vec3(0.181f, 0.103f, 0.051f), glm::vec3(0.194f, 0.025f, 0.051f), glm::vec3(0.185f, -0.046f, 0.04f)} },
    // Chain 4: bones 128..131 (SKL_616..SKL_619)
    { 128, 129, 130, 131,
      {glm::vec3(0.21f, 0.176f, -0.008f), glm::vec3(0.188f, 0.103f, -0.015f), glm::vec3(0.173f, 0.023f, -0.009f), glm::vec3(0.188f, -0.043f, -0.009f)} },
    // Chain 5: bones 132..135 (SKL_620..SKL_623)
    { 132, 133, 134, 135,
      {glm::vec3(0.176f, 0.183f, -0.082f), glm::vec3(0.166f, 0.11f, -0.073f), glm::vec3(0.17f, 0.029f, -0.075f), glm::vec3(0.168f, -0.039f, -0.072f)} },
    // Chain 6: bones 136..139 (SKL_624..SKL_627)
    { 136, 137, 138, 139,
      {glm::vec3(0.097f, 0.191f, -0.139f), glm::vec3(0.09f, 0.115f, -0.128f), glm::vec3(0.084f, 0.034f, -0.111f), glm::vec3(0.082f, -0.036f, -0.113f)} },
    // Chain 7: bones 140..143 (SKL_628..SKL_631)
    { 140, 141, 142, 143,
      {glm::vec3(0.024f, 0.203f, -0.121f), glm::vec3(0.016f, 0.122f, -0.107f), glm::vec3(0.006f, 0.03f, -0.122f), glm::vec3(0.007f, -0.039f, -0.127f)} },
    // Chain 8: bones 144..147 (SKL_632..SKL_635)
    { 144, 145, 146, 147,
      {glm::vec3(-0.046f, 0.208f, -0.124f), glm::vec3(-0.053f, 0.126f, -0.128f), glm::vec3(-0.059f, 0.034f, -0.136f), glm::vec3(-0.058f, -0.038f, -0.137f)} },
    // Chain 9: bones 148..151 (SKL_636..SKL_639)
    { 148, 149, 150, 151,
      {glm::vec3(-0.11f, 0.217f, -0.13f), glm::vec3(-0.114f, 0.137f, -0.133f), glm::vec3(-0.135f, 0.049f, -0.136f), glm::vec3(-0.154f, -0.034f, -0.124f)} },
    // Chain 10: bones 152..155 (SKL_640..SKL_643)
    { 152, 153, 154, 155,
      {glm::vec3(-0.172f, 0.192f, -0.102f), glm::vec3(-0.15f, 0.12f, -0.081f), glm::vec3(-0.157f, 0.034f, -0.068f), glm::vec3(-0.181f, -0.061f, -0.064f)} },
    // Chain 11: bones 156..159 (SKL_644..SKL_647)
    { 156, 157, 158, 159,
      {glm::vec3(-0.188f, 0.195f, -0.052f), glm::vec3(-0.17f, 0.133f, -0.036f), glm::vec3(-0.193f, 0.044f, -0.044f), glm::vec3(-0.193f, -0.055f, -0.03f)} },
    // Chain 12: bones 160..163 (SKL_648..SKL_651)
    { 160, 161, 162, 163,
      {glm::vec3(-0.21f, 0.217f, 0.015f), glm::vec3(-0.206f, 0.14f, 0.02f), glm::vec3(-0.191f, 0.051f, 0.023f), glm::vec3(-0.195f, -0.043f, 0.03f)} },
    // Chain 13: bones 164..167 (SKL_652..SKL_655)
    { 164, 165, 166, 167,
      {glm::vec3(-0.184f, 0.21f, 0.077f), glm::vec3(-0.169f, 0.138f, 0.074f), glm::vec3(-0.181f, 0.05f, 0.083f), glm::vec3(-0.185f, -0.035f, 0.084f)} },
    // Chain 14: bones 168..171 (SKL_656..SKL_659)
    { 168, 169, 170, 171,
      {glm::vec3(-0.133f, 0.201f, 0.135f), glm::vec3(-0.128f, 0.125f, 0.133f), glm::vec3(-0.132f, 0.048f, 0.141f), glm::vec3(-0.133f, -0.033f, 0.144f)} },
    // Chain 15: bones 172..175 (SKL_660..SKL_663, front-right flap at zipper)
    { 172, 173, 174, 175,
      {glm::vec3(-0.067f, 0.196f, 0.151f), glm::vec3(-0.068f, 0.125f, 0.156f), glm::vec3(-0.068f, 0.047f, 0.165f), glm::vec3(-0.073f, -0.047f, 0.169f)} },
};

glm::quat SafeRotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 u = glm::normalize(from);
    const glm::vec3 v = glm::normalize(to);
    const float cos_theta = glm::dot(u, v);
    if (cos_theta > 0.99999f) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    if (cos_theta < -0.99999f) {
        glm::vec3 axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), u);
        if (glm::length2(axis) < 0.01f) {
            axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), u);
        }
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(u, v);
    const float s = std::sqrt((1.0f + cos_theta) * 2.0f);
    return glm::normalize(glm::quat(s * 0.5f, axis.x / s, axis.y / s, axis.z / s));
}

}

bool SimBonesEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("PT_SIM_BONES_OFF");
        return !(env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0));
    }();
    return enabled;
}

SimBones::SimBones() = default;

void SimBones::Reset() {
    initialized_ = false;
    first_run_ = true;
    for (Chain& c : chains_) {
        for (int k = 0; k < 4; ++k) {
            c.pos[k] = c.prev_pos[k] = c.rest_pos[k];
        }
    }
}

void SimBones::Init(const Skeleton& skeleton) {
    chest_bone_ = skeleton.FindName("SKL_002_CHEST");
    waist_bone_ = skeleton.FindName("SKL_000_WAIST");
    const int first_jacket = skeleton.FindName("SKL_600_JAKET_SIM");

    if (chest_bone_ < 0 || waist_bone_ < 0 || first_jacket < 0 || skeleton.Size() < 176) {
        is_player_ = false;
        initialized_ = true;
        return;
    }

    is_player_ = true;
    chains_.resize(16);
    const glm::vec3 chest_bind = skeleton.bind_world[static_cast<size_t>(chest_bone_)];

    for (size_t s = 0; s < 16; ++s) {
        Chain& c = chains_[s];
        const SimChainDef& def = kJacketDefs[s];
        for (int k = 0; k < 4; ++k) {
            c.bones[k] = def.b0 + k;
            c.rest_pos[k] = def.rest[k];
            c.rest_local_to_chest[k] = def.rest[k] - chest_bind;
            c.pos[k] = c.prev_pos[k] = def.rest[k];
        }
        for (int k = 0; k < 3; ++k) {
            c.lengths[k] = glm::length(def.rest[k + 1] - def.rest[k]);
        }
    }
    initialized_ = true;
    first_run_ = true;
}

void SimBones::Apply(const Skeleton& skeleton, std::vector<glm::mat4>& world, float dt) {
    if (!SimBonesEnabled()) {
        return;
    }
    if (!initialized_) {
        Init(skeleton);
    }
    if (!is_player_ || chest_bone_ < 0 || waist_bone_ < 0) {
        return;
    }
    const size_t n = world.size();
    if (static_cast<size_t>(chest_bone_) >= n || static_cast<size_t>(waist_bone_) >= n) {
        return;
    }

    const glm::mat4& chest_m = world[static_cast<size_t>(chest_bone_)];
    const glm::mat4& waist_m = world[static_cast<size_t>(waist_bone_)];
    const glm::quat chest_rot = glm::quat_cast(chest_m);

    // Continuous torso hit capsule from plr0_main0_def_s00.sim:
    // Waist hit body (10): offset [0, 0.0015, 0.035], radius 0.12m + 0.032m cloth
    const glm::vec3 waist_center = glm::vec3(waist_m * glm::vec4(0.0f, 0.0015f, 0.035f, 1.0f));
    const float waist_radius = 0.155f;

    // Chest hit body (142): offset [0, 0.1545, 0], radius 0.08m + 0.048m cloth
    const glm::vec3 chest_center = glm::vec3(chest_m * glm::vec4(0.0f, 0.18f, 0.02f, 1.0f));
    const float chest_radius = 0.130f;

    const glm::vec3 torso_axis = chest_center - waist_center;
    const float torso_len_sq = glm::dot(torso_axis, torso_axis);

    const float time_step = std::clamp(dt, 1.0f / 120.0f, 1.0f / 30.0f);
    const glm::vec3 gravity(0.0f, -9.81f, 0.0f);
    const float damp = 0.85f;
    const float spring_k = 240.0f;

    // Seamless first frame initialization into target chest pose
    if (first_run_) {
        for (size_t s = 0; s < chains_.size(); ++s) {
            Chain& c = chains_[s];
            for (int k = 0; k < 4; ++k) {
                c.pos[k] = c.prev_pos[k] = glm::vec3(chest_m * glm::vec4(c.rest_local_to_chest[k], 1.0f));
            }
        }
        first_run_ = false;
    }

    for (size_t s = 0; s < chains_.size(); ++s) {
        Chain& c = chains_[s];
        glm::vec3 target[4];
        for (int k = 0; k < 4; ++k) {
            target[k] = glm::vec3(chest_m * glm::vec4(c.rest_local_to_chest[k], 1.0f));
        }

        // Anchor top segment firmly to chest
        c.pos[0] = target[0];
        c.prev_pos[0] = target[0];

        // Damped integration for dynamic segments 1..3
        for (int k = 1; k < 4; ++k) {
            const glm::vec3 vel = (c.pos[k] - c.prev_pos[k]) * damp;
            c.prev_pos[k] = c.pos[k];
            const glm::vec3 spring = (target[k] - c.pos[k]) * spring_k;
            c.pos[k] += vel + (gravity + spring) * (time_step * time_step);
        }

        // Solve distance constraints and continuous torso collision (4 iterations)
        for (int iter = 0; iter < 4; ++iter) {
            for (int k = 1; k < 4; ++k) {
                const glm::vec3 delta = c.pos[k] - c.pos[k - 1];
                const float d = glm::length(delta);
                if (d > 1e-6f) {
                    const float diff = (d - c.lengths[k - 1]) / d;
                    c.pos[k] -= delta * diff;
                }
            }

            // Torso continuous capsule collision resolution (eliminates any gap between chest and waist)
            for (int k = 1; k < 4; ++k) {
                if (torso_len_sq > 1e-6f) {
                    const float t = glm::clamp(glm::dot(c.pos[k] - waist_center, torso_axis) / torso_len_sq, 0.0f, 1.0f);
                    const glm::vec3 closest = waist_center + t * torso_axis;
                    const float r_torso = glm::mix(waist_radius, chest_radius, t);
                    const glm::vec3 radial = c.pos[k] - closest;
                    const float dist = glm::length(radial);
                    if (dist < r_torso && dist > 1e-5f) {
                        c.pos[k] = closest + (radial / dist) * r_torso;
                    }
                }
            }
        }
    }

    // Zipper closure synchronization across midline:
    // Chain 0 (left flap) and Chain 15 (right flap) meet at zipper.
    // Ensure the front flaps stay smoothly together and forward (+Z)
    for (int k = 1; k < 4; ++k) {
        const float front_z = std::max(chains_[0].pos[k].z, chains_[15].pos[k].z);
        chains_[0].pos[k].z = front_z;
        chains_[15].pos[k].z = front_z;
    }

    // Reconstruct bone matrices for all 64 jacket bones
    for (size_t s = 0; s < chains_.size(); ++s) {
        const Chain& c = chains_[s];
        for (int k = 0; k < 4; ++k) {
            const int bone = c.bones[k];
            if (bone < 0 || static_cast<size_t>(bone) >= n) {
                continue;
            }
            const glm::vec3 dir = (k < 3) ? (c.pos[k + 1] - c.pos[k]) : (c.pos[3] - c.pos[2]);
            const glm::vec3 rest_dir = (k < 3) ? (c.rest_pos[k + 1] - c.rest_pos[k]) : (c.rest_pos[3] - c.rest_pos[2]);
            const glm::quat swing = SafeRotationBetween(rest_dir, dir);
            const glm::quat rot = glm::normalize(swing * chest_rot);

            world[static_cast<size_t>(bone)] = glm::translate(glm::mat4(1.0f), c.pos[k]) * glm::mat4_cast(rot);
        }
    }
}

}
