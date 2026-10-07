#pragma once
#include "engine/render/scene_lighting.h"
#include <vector>
#include <algorithm>
#include <utility>
namespace pt::game {
inline void ApplyFollowerShadow(std::vector<SceneLight>& lights, const glm::vec3& position) {
    size_t best = lights.size();
    float score = 0.0f;
    for (size_t i = 0; i < lights.size(); ++i) {
        const auto& light = lights[i];
        if (light.cast_shadow || light.type != LightType::Point || light.hidden_views || light.character_shadow_only) continue;
        const glm::vec3 delta = light.position - position;
        const float distance2 = glm::dot(delta, delta);
        if (distance2 >= light.outer_range * light.outer_range) continue;
        const float energy = (light.intensity.x + light.intensity.y + light.intensity.z) / std::max(distance2, 0.25f);
        if (energy > score) { best = i; score = energy; }
    }
    if (best == lights.size()) return;
    SceneLight shadow = lights[best];
    shadow.diffuse_scale = shadow.specular_scale = 0.0f;
    shadow.outer_range = std::max(shadow.outer_range, 24.0f);
    shadow.dimmer = 0.0f;
    shadow.has_area = false;
    shadow.id ^= 0x4C49534153484457ull;
    shadow.name += " (Lisa shadow)";
    shadow.cast_shadow = true;
    shadow.character_shadow_only = true;
    shadow.shadow_strength = 1.0f;
    shadow.source_radius = std::min(shadow.source_radius, 0.08f);
    shadow.priority = 0x10;
    shadow.lod = glm::vec4(0.0f);
    lights.push_back(std::move(shadow));
}
}
