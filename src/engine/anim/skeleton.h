#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pt::anim {

struct Skeleton {
    std::vector<std::string> names;
    std::vector<uint32_t> hashes;
    std::vector<int16_t> parents;
    std::vector<glm::vec3> bind_world;
    std::vector<glm::vec3> bind_local;

    size_t Size() const { return names.size(); }
    bool Empty() const { return names.empty(); }
    int Find(uint32_t hash) const;
    int FindName(std::string_view name) const;
};

bool ReadFmdlSkeleton(std::span<const uint8_t> fmdl, Skeleton& out);

struct Pose {
    std::vector<glm::quat> rotation;
    std::vector<glm::vec3> offset;

    void Reset(size_t bones);
    size_t Size() const { return rotation.size(); }
};

void ComputeBoneWorld(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& world);
void ComputeSkin(const Skeleton& skeleton, const std::vector<glm::mat4>& world, std::vector<glm::mat4>& skin);
void BlendPose(const Pose& from, const Pose& to, float weight, Pose& out);

}
