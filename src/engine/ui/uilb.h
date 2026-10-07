#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pt::ui {

struct UilbAnimation {
    uint64_t name = 0;
    std::string main;
    std::string shader;
    float speed = 1.0f;
};

struct UilbModel {
    std::string path;
    glm::vec3 translate{0.0f};
    std::vector<uint64_t> animations;
};

class UilbLayout {
public:
    bool Parse(std::span<const uint8_t> data, std::string* error = nullptr);

    const std::vector<UilbModel>& Models() const { return models_; }
    const std::vector<UilbAnimation>& Animations() const { return animations_; }
    const UilbAnimation* FindAnimation(uint64_t name) const;
    float CameraDistance() const { return camera_distance_; }

private:
    std::vector<UilbModel> models_;
    std::vector<UilbAnimation> animations_;
    float camera_distance_ = 0.0f;
};

}
