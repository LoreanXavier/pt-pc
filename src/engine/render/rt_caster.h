#pragma once

#include <algorithm>
#include <cmath>

#include "engine/render/gpu_types.h"

namespace pt {

struct GpuMesh;

struct RtCaster {
    const GpuMesh* mesh = nullptr;
    uint32_t submesh = 0;
    glm::mat4 transform{1.0f};
    uint32_t material = 0;
    uint32_t skin_base = gpu::kInvalid;
    uint8_t mask = 3;
    bool alpha_test = false;
    bool double_sided = false;
    bool fitting = false;
    uint32_t light_fitting = 0;
    bool mirror_hidden = false;
    glm::vec3 tint{1.0f};
};

constexpr uint32_t kRtRecordEffect = 1u << 11;

inline uint32_t RtRecordTintBits(const glm::vec3& tint) {
    uint32_t bits = 0;
    for (int i = 0; i < 3; ++i) {
        const uint32_t q = static_cast<uint32_t>(std::clamp(std::lround(tint[i] * 32.0f), 0l, 63l));
        bits |= (q ^ 32u) << (12 + 6 * i);
    }
    return bits;
}

inline uint32_t RtRecordFlags(const RtCaster& c) {
    return (c.fitting ? 1u | (c.alpha_test ? 2u : 0u) : 0u) | (c.skin_base != gpu::kInvalid ? 4u : 0u) |
           (c.alpha_test ? 8u : 0u) | ((c.light_fitting & 63u) << 4) | (c.mirror_hidden ? 1024u : 0u) |
           RtRecordTintBits(c.tint);
}

}
