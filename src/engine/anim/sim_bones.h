#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

#include "engine/anim/skeleton.h"

namespace pt::anim {

class SimBones {
public:
    SimBones();

    // Reset simulation state
    void Reset();

    // Evaluate simulation for bones 112..175 (SKL_600..SKL_663)
    // Runs after rig and help bones, before ComputeSkin
    void Apply(const Skeleton& skeleton, std::vector<glm::mat4>& world, float dt = 1.0f / 60.0f);

private:
    void Init(const Skeleton& skeleton);

    bool initialized_ = false;
    bool is_player_ = false;
    bool first_run_ = true;

    struct Chain {
        int bones[4]{-1, -1, -1, -1};
        glm::vec3 rest_pos[4]{};
        glm::vec3 rest_local_to_chest[4]{};
        float lengths[3]{0.0f, 0.0f, 0.0f};
        glm::vec3 pos[4]{};
        glm::vec3 prev_pos[4]{};
    };

    std::vector<Chain> chains_;
    int chest_bone_ = -1;
    int waist_bone_ = -1;
};

// PT_SIM_BONES_OFF=1 disables simulation bones for comparisons
bool SimBonesEnabled();

}
