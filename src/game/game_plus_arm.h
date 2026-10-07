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

class GamePlusArm {
public:
    bool Prepare(ModelCache& models);
    void Apply(std::vector<DrawItem>& out);

private:
    bool ready_ = false;
    bool tried_ = false;
    const GpuMesh* lisa_mesh_ = nullptr;
    const GpuMesh* lisa_cut_ = nullptr;
    const GpuMesh* side_mesh_ = nullptr;
    std::vector<glm::vec3> lisa_bind_;
    std::vector<glm::vec3> side_bind_;
    std::vector<glm::vec3> side_retarget_;
    std::vector<int> side_parent_;
    std::vector<int> side_source_;
    std::vector<bool> side_arm_;
    float scale_ = 0.6f;
    std::deque<std::vector<glm::mat4>> skins_;
};

}
