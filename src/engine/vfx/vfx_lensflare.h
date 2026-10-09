#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pt::vfx {

// 0x8FA8A0
struct LensFlareField {
    bool valid = false;
    int32_t shape_type = 0;
    int32_t interp_type = 0;
    float scales[3] = {0.0f, 0.5f, 1.0f};
    float values[3] = {1.0f, 1.0f, 1.0f};

    float Eval(const glm::vec2& p, float inv_aspect) const;
};

// 0x8F7B00
struct LensFlareGraph {
    bool valid = false;
    float values[11] = {};

    float Eval(float t) const;
};

struct LensFlareElement {
    std::string texture;
    float width = 1.0f;
    float height = 1.0f;
    glm::vec4 color{1.0f};
    int32_t offset_type = 1;
    float offset_scale = 0.0f;
    glm::vec2 base_offset{0.0f};
    int32_t rotate_type = 0;
    float base_rotate = 0.0f;
    LensFlareField scale_x;
    LensFlareField scale_y;
    LensFlareField alpha;
    bool scale_at_light = false;
    bool alpha_at_light = false;
    float fade_out = 0.2f;
    float fade_in = 0.2f;
    LensFlareGraph angle_scale_x;
    LensFlareGraph angle_scale_y;
    LensFlareGraph angle_alpha;
    int32_t distance_scaling = 0;
    float limit_distance = 100.0f;
};

struct LensFlareDef {
    std::string name;
    float exposure_blend = 0.0f;
    bool collision_check = false;
    std::vector<LensFlareElement> elements;
};

bool LoadLensFlare(std::span<const uint8_t> data, const std::string& name, LensFlareDef& out);

}
