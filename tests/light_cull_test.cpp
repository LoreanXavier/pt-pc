// The occluder stage of the original's culling core (0xD2A100, 0xD2AC10) as src/engine/render/light_cull.cpp ports it
#include <cmath>
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/render/light_cull.h"

namespace {

using namespace pt;
using namespace pt::lightcull;

// the port's camera projection: reverse-Z, infinite, Vulkan clip y down
glm::mat4 Projection(float fov_y, float aspect, float near_plane) {
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = -f;
    p[2][3] = -1.0f;
    p[3][2] = near_plane;
    return p;
}

SceneOccluder Quad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, bool one_sided = false) {
    SceneOccluder o;
    o.points[0] = a;
    o.points[1] = b;
    o.points[2] = c;
    o.points[3] = d;
    o.count = 4;
    o.one_sided = one_sided;
    return o;
}

}  // namespace

int main() {
    int failures = 0;
    const auto check = [&](const char* name, bool ok) {
        std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    const glm::vec3 eye(0.0f);
    // looking down -z
    const glm::mat4 vp = Projection(glm::radians(60.0f), 1.0f, 0.1f) * glm::lookAt(eye, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    // the geometry checks run without the eboot's thresholds and counts, which have checks of their own below
    OccluderLimits limits;
    limits.min_fan_area = 0.0f;
    limits.max_distance = 1.0e30f;
    limits.max_distance_flag4 = 1.0e30f;
    limits.total = 16;
    limits.nearest = 16;

    // a 4 x 4 m wall 5 m ahead, wound counterclockwise seen from the eye
    const SceneOccluder wall = Quad({-2, -2, -5}, {2, -2, -5}, {2, 2, -5}, {-2, 2, -5});
    {
        const SceneOccluder list[] = {wall};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, limits);
        check("wall in view is kept", set.volumes.size() == 1 && set.verdicts[0] == OccluderVerdict::Kept);
        check("wall volume has its plane and four edge planes", set.volumes.size() == 1 && set.volumes[0].plane_count == 5);
        check("wall distance is the plane distance", set.volumes.size() == 1 && std::abs(set.volumes[0].distance - 5.0f) < 1e-4f);
        // the wall spans 4 / (2 * 5 * tan 30) = 0.693 of the screen's width and height in device units (2 wide)
        const float side = 4.0f / (5.0f * std::tan(glm::radians(30.0f)));
        check("wall area is its projected area", set.volumes.size() == 1 && std::abs(set.volumes[0].area - side * side) < 1e-3f);
        check("box behind the wall is occluded", FindOccludingVolume({-0.5f, -0.5f, -9.0f}, {0.5f, 0.5f, -8.0f}, set) == 0);
        check("box in front of the wall is not", FindOccludingVolume({-0.5f, -0.5f, -4.0f}, {0.5f, 0.5f, -3.0f}, set) < 0);
        check("box straddling the wall's edge is not", FindOccludingVolume({1.0f, -0.5f, -9.0f}, {4.0f, 0.5f, -8.0f}, set) < 0);
        check("box crossing the wall's plane is not", FindOccludingVolume({-0.5f, -0.5f, -6.0f}, {0.5f, 0.5f, -4.0f}, set) < 0);
    }
    {
        // the same wall wound the other way round is the same occluder for a two sided test
        const SceneOccluder list[] = {Quad({-2, 2, -5}, {2, 2, -5}, {2, -2, -5}, {-2, -2, -5})};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, limits);
        check("reversed winding gives the same volume", set.volumes.size() == 1 &&
                                                             FindOccludingVolume({-0.5f, -0.5f, -9.0f}, {0.5f, 0.5f, -8.0f}, set) == 0);
    }
    {
        // one sided: counterclockwise on the screen is the back (0xD2A100), so the wall above is skipped and the reversed
        // one kept
        const SceneOccluder ccw = Quad({-2, -2, -5}, {2, -2, -5}, {2, 2, -5}, {-2, 2, -5}, true);
        const SceneOccluder cw = Quad({-2, 2, -5}, {2, 2, -5}, {2, -2, -5}, {-2, -2, -5}, true);
        const SceneOccluder list[] = {ccw, cw};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, limits);
        check("one sided seen counterclockwise is skipped", set.verdicts[0] == OccluderVerdict::BackFacing);
        check("one sided seen clockwise is kept", set.verdicts[1] == OccluderVerdict::Kept);
    }
    {
        const SceneOccluder list[] = {Quad({-2, -2, 5}, {2, -2, 5}, {2, 2, 5}, {-2, 2, 5}),
                                      Quad({-12, -2, -5}, {-8, -2, -5}, {-8, 2, -5}, {-12, 2, -5})};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, limits);
        check("wall behind the eye is outside the view", set.verdicts[0] == OccluderVerdict::OutsideView);
        check("wall beside the view is outside the view", set.verdicts[1] == OccluderVerdict::OutsideView);
    }
    {
        // a wall that reaches behind the eye on the left: clipped at w = 0, still kept, finite area
        const SceneOccluder list[] = {Quad({-3, -2, 3}, {-3, -2, -6}, {-3, 2, -6}, {-3, 2, 3})};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, limits);
        check("wall crossing the eye plane is kept", set.verdicts[0] == OccluderVerdict::Kept);
        check("its area is finite", set.volumes.size() == 1 && std::isfinite(set.volumes[0].area) && set.volumes[0].area > 0.0f);
        check("its distance is the eye's distance to its plane", set.volumes.size() == 1 && std::abs(set.volumes[0].distance - 3.0f) < 1e-4f);
    }
    {
        // a wall running far out of the view on the left: its left edge plane is left out (both ends outside the left
        // plane, the view axis inside it), so a box beyond that edge but behind the wall's plane is still hidden
        const SceneOccluder list[] = {Quad({-50, -2, -5}, {2, -2, -5}, {2, 2, -5}, {-50, 2, -5})};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, limits);
        check("long wall keeps its plane and three edge planes", set.volumes.size() == 1 && set.volumes[0].plane_count == 4);
        check("box past the off screen edge is hidden", FindOccludingVolume({-90.0f, -1.0f, -9.0f}, {-70.0f, 1.0f, -8.0f}, set) == 0);
        check("its distance is the plane distance", set.volumes.size() == 1 && std::abs(set.volumes[0].distance - 5.0f) < 1e-4f);
    }
    {
        // eye outside the polygon's outline: the distance to the nearest edge
        const SceneOccluder list[] = {Quad({1, -1, -4}, {3, -1, -4}, {3, 1, -4}, {1, 1, -4})};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, limits);
        check("distance to the nearest edge", set.volumes.size() == 1 && std::abs(set.volumes[0].distance - std::sqrt(17.0f)) < 1e-4f);
    }
    {
        // a large wall hides a small wall behind it (0xD2AC10, area 0.4 or more)
        const SceneOccluder list[] = {Quad({-0.5f, -0.5f, -8}, {0.5f, -0.5f, -8}, {0.5f, 0.5f, -8}, {-0.5f, 0.5f, -8}), wall};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, limits);
        check("small wall behind a large one is dropped", set.verdicts[0] == OccluderVerdict::HiddenByOther && set.verdicts[1] == OccluderVerdict::Kept);
        check("one volume left, the large wall's", set.volumes.size() == 1 && set.volumes[0].source == 1);
    }
    {
        // the limits: the largest occluder first, and the area and distance thresholds
        const SceneOccluder small = Quad({0.5f, -0.5f, -5}, {1.5f, -0.5f, -5}, {1.5f, 0.5f, -5}, {0.5f, 0.5f, -5});
        const SceneOccluder list[] = {small, Quad({-2.5f, -0.5f, -5}, {-0.5f, -0.5f, -5}, {-0.5f, 0.5f, -5}, {-2.5f, 0.5f, -5})};
        OccluderLimits one = limits;
        one.total = 1;
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, one);
        check("the limit keeps the larger occluder", set.verdicts[0] == OccluderVerdict::OverLimit && set.verdicts[1] == OccluderVerdict::Kept);
        OccluderLimits area = limits;
        area.min_fan_area = 0.3f;  // the small one 0.24, the large one 0.48
        const OccluderSet by_area = BuildOccluderVolumes(list, vp, eye, area);
        check("the area threshold drops the small one", by_area.verdicts[0] == OccluderVerdict::SmallArea && by_area.verdicts[1] == OccluderVerdict::Kept);
        OccluderLimits near_only = limits;
        near_only.max_distance = 4.0f;
        const OccluderSet by_distance = BuildOccluderVolumes(list, vp, eye, near_only);
        check("the distance threshold drops both", by_distance.volumes.empty() && by_distance.verdicts[0] == OccluderVerdict::TooFar);
    }
    {
        // the eboot's values (0x145F330 0.4, 0x1B8CBC0 100 m, 0x1B8CBB4 6): a 1 m quad 5 m ahead has a fan sum of 0.24 and is
        // skipped, the 4 m wall is kept, a wall 120 m away is too far, and of eight walls the six largest stay
        const OccluderLimits original;
        check("eboot defaults", original.min_fan_area == 0.4f && original.max_distance == 100.0f && original.max_distance_flag4 == 32.0f &&
                                    original.total == 6 && original.nearest == 4);
        const SceneOccluder list[] = {Quad({0.5f, -0.5f, -5}, {1.5f, -0.5f, -5}, {1.5f, 0.5f, -5}, {0.5f, 0.5f, -5}), wall,
                                      Quad({-60, -60, -120}, {60, -60, -120}, {60, 60, -120}, {-60, 60, -120})};
        const OccluderSet set = BuildOccluderVolumes(list, vp, eye, original);
        check("small quad under the area threshold", set.verdicts[0] == OccluderVerdict::SmallArea);
        check("wall kept with the eboot's values", set.verdicts[1] == OccluderVerdict::Kept);
        check("wall beyond 100 m too far", set.verdicts[2] == OccluderVerdict::TooFar);
        // eight side by side strips 6 m ahead, 12 m tall, widths growing, none inside another's volume
        SceneOccluder strips[8];
        float x = -1.9f;
        for (int i = 0; i < 8; ++i) {
            const float w = 0.3f + 0.05f * static_cast<float>(i);
            strips[i] = Quad({x, -6, -6}, {x + w, -6, -6}, {x + w, 6, -6}, {x, 6, -6});
            x += w + 0.02f;
        }
        const OccluderSet six = BuildOccluderVolumes(strips, vp, eye, original);
        check("six volumes kept", six.volumes.size() == 6);
        check("the two smallest over the limit", six.verdicts[0] == OccluderVerdict::OverLimit && six.verdicts[1] == OccluderVerdict::OverLimit &&
                                                    six.verdicts[7] == OccluderVerdict::Kept);
    }
    {
        // the grid test: a box 0.3 m behind the eye is kept when its cells reach in front of the near plane, a box
        // 1.3 m behind is not (1 m cells, cell boundaries at z = 0.5 + k); without the cells both lie behind the near plane
        LightGrid grid;
        grid.origin = glm::vec3(0.0f, 0.0f, 0.5f);
        grid.cell = 1.0f;
        grid.valid = true;
        check("box ending 0.3 m behind the eye kept by its cells",
              GridBoxInFrustum({-0.5f, -0.5f, 0.3f}, {0.5f, 0.5f, 0.8f}, vp, grid));
        check("box ending 1.3 m behind the eye dropped", !GridBoxInFrustum({-0.5f, -0.5f, 1.3f}, {0.5f, 0.5f, 1.8f}, vp, grid));
        check("box ahead kept", GridBoxInFrustum({-0.5f, -0.5f, -6.0f}, {0.5f, 0.5f, -5.0f}, vp, grid));
        check("box beside the view dropped", !GridBoxInFrustum({-40.0f, -0.5f, -6.0f}, {-30.0f, 0.5f, -5.0f}, vp, grid));
        check("inverted box counts as the box between its ends", GridBoxInFrustum({0.5f, 0.5f, -5.0f}, {-0.5f, -0.5f, -6.0f}, vp, grid));
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
