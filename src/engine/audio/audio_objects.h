#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace pt::audio {

struct AudioObjectBlock {
    uint32_t key = 0;
    glm::vec3 position{0.0f};
    std::vector<float> samples;
};

struct AudioObjectMix {
    std::vector<AudioObjectBlock> objects;
    uint32_t count = 0;
    uint32_t capacity = 0;
};

}
