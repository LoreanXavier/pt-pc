#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pt::game {

struct CaptionRule {
    uint32_t event;
    const char* name;
    const char* key;
    bool positional;
    float hold;
    uint8_t pulse;
    uint8_t priority;
};

enum class CaptionSide : uint8_t { None, Ahead, Left, Right, Behind };

class SoundCaptions {
public:
    static std::span<const CaptionRule> Rules();
    static const CaptionRule* Find(uint32_t event);
    static CaptionSide SideOf(const glm::vec3& listener, const glm::vec3& forward, const glm::vec3& source);
    void SkipNext(uint32_t event) { skip_ = event; }

    struct Line {
        const CaptionRule* rule = nullptr;
        std::optional<glm::vec3> source;
        CaptionSide side = CaptionSide::None;
        float age = 0.0f;
        float Alpha() const;
    };

    void SetEnabled(bool on);
    bool Enabled() const { return enabled_; }
    void Post(uint32_t event, const std::optional<glm::vec3>& source);
    void Update(float dt, const glm::vec3& listener, const glm::vec3& forward);
    void Clear();
    const std::vector<Line>& Lines() const { return lines_; }
    std::pair<uint8_t, uint8_t> Pulse(float dt);
    uint64_t Posted() const { return posted_; }

    static constexpr size_t kMaxLines = 3;
    static constexpr float kPulseSeconds = 0.25f;

private:
    bool enabled_ = false;
    std::vector<Line> lines_;
    float pulse_ = 0.0f;
    float pulse_strength_ = 0.0f;
    uint64_t posted_ = 0;
    bool log_posts_ = false;
    uint32_t skip_ = 0;
};

}
