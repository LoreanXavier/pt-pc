#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/survival/survival_sounds.h"

namespace pt::game::survival_acoustics {

using RayFn = std::function<bool(const glm::vec3& from, const glm::vec3& dir, float length, float& distance, glm::vec3& normal)>;

struct Echo {
    float delay = 0.0f;
    float gain = 0.0f;
    float side = 0.0f;
    uint8_t dark = 0;
};

struct Space {
    std::vector<Echo> echoes;
    float enclosure = 0.0f;
    float free_path = 0.0f;
    float reverb_time = 0.0f;
    float late_level = 0.0f;
    float late_start = 0.0f;
    std::string Describe() const;
};

Space Probe(const RayFn& ray, const glm::vec3& ear, const glm::vec3& forward, const glm::vec3& right);

struct Source {
    std::shared_ptr<const std::vector<float>> dry;
    std::array<std::shared_ptr<const std::vector<float>>, 4> dark;
    std::array<float, 3> band_energy{};
    bool Valid() const { return dry && !dry->empty(); }
};
Source Prepare(const survival_sounds::Take& take);

survival_sounds::Sound Render(const Source& source, const Space& space, float gain, float speed, uint32_t seed);

}
