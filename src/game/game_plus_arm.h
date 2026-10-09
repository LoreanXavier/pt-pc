#pragma once

#include <glm/glm.hpp>

#include <deque>
#include <span>
#include <string>
#include <vector>

namespace pt {
class ModelCache;
struct GpuMesh;
struct DrawItem;
}

namespace pt::game {

// Game+ tier 2 (docs/gameplay.md, Game+): Lisa's left side takes the unused creature hsh0's (resident.fpk hsh0_main0_def,
// 74 bones with the same SKL_ names as Lisa's och0_main0_def): her left shoulder, chest and flank down to the hip, arm and hand
// are hsh0's skin and its long arm. Draw only: her skeleton, motions, logic and collision are unchanged. Each Lisa draw (the
// gimmick and her demos, any item with her mesh and a skin) is replaced by
//  - her mesh without its left side (ModelCache::GetSubset: the triangles whose vertices lie left of her middle, above her hip
//    joints and below her shoulder line, and her left arm and hand by their bones), skinned as before, and
//  - hsh0's left side (left of its middle line, between its hip and neck) retargeted into her bind space (its torso's lean
//    taken out, scaled to her size about the waist: s = her waist to neck height over its, 0.60) and skinned with its own
//    weights: its torso bones by her own skin matrices (so it follows every bone of hers exactly), its arm from her real upper
//    arm joint as a chain of her bone rotations over hsh0's bone offsets at kArmLength, so the long arm hangs from her shoulder.
// The cuts overlap at her middle and shoulder line. Both bind poses are rotation free T poses, the left arm along +x.
class GamePlusArm {
public:
    // loads both models once (false when one is missing)
    bool Prepare(ModelCache& models);
    // replaces the Lisa items of `out` and adds the hsh0 draws; the skins live until the next call
    void Apply(std::vector<DrawItem>& out);

private:
    bool ready_ = false;
    bool tried_ = false;
    const GpuMesh* lisa_mesh_ = nullptr;
    const GpuMesh* lisa_cut_ = nullptr;
    const GpuMesh* side_mesh_ = nullptr;
    std::vector<glm::vec3> lisa_bind_;
    std::vector<glm::vec3> side_bind_;
    // hsh0's joints retargeted into her bind space (Retarget)
    std::vector<glm::vec3> side_retarget_;
    std::vector<int> side_parent_;
    // per hsh0 bone: the Lisa bone whose pose it takes (its own name, else its nearest named ancestor's), and whether it is part
    // of the arm chain (upper arm down)
    std::vector<int> side_source_;
    std::vector<bool> side_arm_;
    float scale_ = 0.6f;
    std::deque<std::vector<glm::mat4>> skins_;
};

}
