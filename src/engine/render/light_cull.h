#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/scene_lighting.h"

// The per view occluder stage of the original's culling core 0xD53AC0 (docs/rendering.md 12.23): 0xD29E90 sets up the
// view, 0xD2A100 turns each OccluderEx into an occlusion volume (its plane and the planes through the eye and its edges),
// 0xD2AC10 picks the volumes the view keeps. No Vulkan here, so tests/light_cull_test.cpp builds it alone.
namespace pt::lightcull {

// The values 0xD2A100 and 0xD2AC10 read from the eboot's data (read from the eboot, scratch occluder_values.txt).
// PT_OCCLUDER_LIMITS="min_fan_area max_distance max_distance_flag4 total nearest" overrides them for tests.
struct OccluderLimits {
    // 0x145F330 (0.4): an occluder whose projected polygon (clipped at w = 0) has a fan sum of |cross| (twice its area in
    // normalized device units, the whole screen 8) below this is skipped
    float min_fan_area = 0.4f;
    // 0x1B8CBC0 (100 m): an occluder without runtime flag 4 further from the eye than this is skipped
    float max_distance = 100.0f;
    // 0x1B8CBBC (32 m): the same for an occluder with runtime flag 4
    float max_distance_flag4 = 32.0f;
    // 0x1B8CBB4 (6): the volumes a view keeps (0xD53AC0 has room for 16)
    uint32_t total = 6;
    // 0x1B8CBB8 (4): the flag 4 occluders kept by distance before the others are taken by area
    uint32_t nearest = 4;
};

// what 0xD2A100 and 0xD2AC10 made of an occluder
enum class OccluderVerdict : uint8_t {
    Kept,
    Disabled,       // fewer than 3 points (the port's BuildOccluder leaves out a disabled one)
    OverCapacity,   // 128 volumes, 256 points or 256 planes in use (0xD29E90's buffers)
    OutsideView,    // every point outside the same clip plane or behind the eye
    SmallArea,      // projected fan sum under min_fan_area
    BackFacing,     // one sided, seen from the back
    TooFar,         // further from the eye than max_distance
    HiddenByOther,  // inside the volume of a kept occluder that covers at least 0.4 of the screen (2D units, of 4)
    OverLimit,      // not among the first `total` (by distance for flag 4, then by area)
};

const char* VerdictName(OccluderVerdict v);

struct OccluderVolume {
    // a point is in the volume when every plane gives dot(plane.xyz, p) + plane.w > 0; plane 0 is the occluder's own
    // plane (the eye on its negative side), the others pass through the eye and an edge
    std::array<glm::vec4, 8> planes{};
    uint32_t plane_count = 0;
    // the index of the occluder in the input
    uint32_t source = 0;
    // the projected area in normalized device units (the whole screen 4)
    float area = 0.0f;
    // the eye's distance to the polygon
    float distance = 0.0f;
};

struct OccluderSet {
    std::vector<OccluderVolume> volumes;
    // one per input occluder
    std::vector<OccluderVerdict> verdicts;
};

// view_projection maps the world to clip space with x, y in [-w, w] (any depth convention); eye is the view's position
OccluderSet BuildOccluderVolumes(std::span<const SceneOccluder> occluders, const glm::mat4& view_projection, const glm::vec3& eye,
                                 const OccluderLimits& limits);

// the box lo..hi lies wholly inside the volume
bool BoxInVolume(const glm::vec3& lo, const glm::vec3& hi, const OccluderVolume& volume);

// the index into set.volumes of the first volume that holds the box, or -1
int FindOccludingVolume(const glm::vec3& lo, const glm::vec3& hi, const OccluderSet& set);

// PT_OCCLUDER_LIMITS, read once
const OccluderLimits& LimitsFromEnv();

// The scene grid of 0xCFFED0: world = origin + cell * grid (grid +0xD0, +0xE0, +0xE4 the inverse). Its values are set at
// run time by code not decompiled yet, so the port has none unless PT_LIGHT_GRID="ox oy oz cell" gives them.
struct LightGrid {
    glm::vec3 origin{0.0f};
    float cell = 0.0f;
    bool valid = false;
};
const LightGrid& GridFromEnv();

// 0x1300ED0 files a box in whole cells (min floor((lo - origin) / cell), max floor((hi - origin) / cell + 1), clamped to
// 16 bits); the frustum walkers (0x1305350 for the nodes, 0x13059C0 for the objects of a partly visible cell) keep a box
// unless it lies wholly outside one of the query's six planes w - x, w + x, w - y, w + y, w - z, w + z (in the box's
// grid frame; a plane must be strictly positive somewhere on the box). Every component takes the larger of its two ends,
// so a box whose minimum passes its maximum counts as the box between them.
bool GridBoxInFrustum(const glm::vec3& lo, const glm::vec3& hi, const glm::mat4& view_projection, const LightGrid& grid);

}  // namespace pt::lightcull
