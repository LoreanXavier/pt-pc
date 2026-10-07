#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pt::ui {

struct UiaTrack {
    uint32_t unit = 0;
    int components = 1;
    std::vector<int> frames;
    std::vector<glm::vec4> values;
};

struct UiaNode {
    uint32_t node = 0;
    std::vector<UiaTrack> tracks;
};

class UiaAnimation {
public:
    static constexpr uint32_t kTranslate = 0xA34AEDBA;
    static constexpr uint32_t kColor = 0x6318D107;
    static constexpr uint32_t kScale = 0x8550FCEE;
    static constexpr uint32_t kRotate = 0x10DFD233;
    static constexpr float kFramesPerSecond = 60.0f;

    bool Parse(std::span<const uint8_t> data, std::string* error = nullptr);
    int Frames() const { return frames_; }
    const std::vector<UiaNode>& Nodes() const { return nodes_; }

    static glm::vec4 Sample(const UiaTrack& track, float frame);

private:
    bool ParseUnits(std::span<const uint8_t> data, size_t at, uint32_t node_hash);

    int frames_ = 0;
    std::vector<UiaNode> nodes_;
};

}
