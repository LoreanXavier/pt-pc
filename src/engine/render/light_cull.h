#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/scene_lighting.h"

namespace pt::lightcull {

struct OccluderLimits {
    float min_fan_area = 0.4f;
    float max_distance = 100.0f;
    float max_distance_flag4 = 32.0f;
    uint32_t total = 6;
    uint32_t nearest = 4;
};

enum class OccluderVerdict : uint8_t {
    Kept,
    Disabled,
    OverCapacity,
    OutsideView,
    SmallArea,
    BackFacing,
    TooFar,
    HiddenByOther,
    OverLimit,
};

const char* VerdictName(OccluderVerdict v);

struct OccluderVolume {
    std::array<glm::vec4, 8> planes{};
    uint32_t plane_count = 0;
    uint32_t source = 0;
    float area = 0.0f;
    float distance = 0.0f;
};

struct OccluderSet {
    std::vector<OccluderVolume> volumes;
    std::vector<OccluderVerdict> verdicts;
};

OccluderSet BuildOccluderVolumes(std::span<const SceneOccluder> occluders, const glm::mat4& view_projection, const glm::vec3& eye,
                                 const OccluderLimits& limits);

bool BoxInVolume(const glm::vec3& lo, const glm::vec3& hi, const OccluderVolume& volume);

int FindOccludingVolume(const glm::vec3& lo, const glm::vec3& hi, const OccluderSet& set);

const OccluderLimits& LimitsFromEnv();

struct LightGrid {
    glm::vec3 origin{0.0f};
    float cell = 0.0f;
    bool valid = false;
};
const LightGrid& GridFromEnv();

bool GridBoxInFrustum(const glm::vec3& lo, const glm::vec3& hi, const glm::mat4& view_projection, const LightGrid& grid);

}
