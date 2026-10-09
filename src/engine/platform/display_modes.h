#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace pt {

inline std::vector<glm::ivec2> UniqueDisplaySizes(std::span<const glm::ivec2> modes) {
    std::vector<glm::ivec2> sizes;
    for (const glm::ivec2 size : modes) {
        if (size.x > 0 && size.y > 0 && std::find(sizes.begin(), sizes.end(), size) == sizes.end()) sizes.push_back(size);
    }
    std::sort(sizes.begin(), sizes.end(), [](glm::ivec2 a, glm::ivec2 b) {
        const int64_t area_a = static_cast<int64_t>(a.x) * a.y;
        const int64_t area_b = static_cast<int64_t>(b.x) * b.y;
        return area_a == area_b ? a.x < b.x : area_a < area_b;
    });
    return sizes;
}

inline glm::ivec2 ClosestDisplaySize(std::span<const glm::ivec2> modes, glm::ivec2 requested) {
    if (modes.empty()) return requested;
    const auto score = [requested](glm::ivec2 size) {
        const int64_t dx = static_cast<int64_t>(size.x) - requested.x;
        const int64_t dy = static_cast<int64_t>(size.y) - requested.y;
        return dx * dx + dy * dy;
    };
    return *std::min_element(modes.begin(), modes.end(), [&](glm::ivec2 a, glm::ivec2 b) { return score(a) < score(b); });
}

}
