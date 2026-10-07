#include "engine/vfx/vfx_lensflare.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/data/fox2.h"

namespace pt::vfx {

namespace {

LensFlareField ReadField(const fox2::DataSetFile& f, const fox2::Entity& shape, const char* prop) {
    LensFlareField field;
    const auto link = f.GetLink(shape, prop);
    const fox2::Entity* e = link ? f.ResolveLink(*link) : nullptr;
    if (!e) {
        return field;
    }
    field.valid = true;
    field.shape_type = f.GetInt(*e, "shapeType");
    field.interp_type = f.GetInt(*e, "interpType");
    field.scales[0] = f.GetFloat(*e, "innerScale");
    field.scales[1] = f.GetFloat(*e, "centerScale", 0, 0.5f);
    field.scales[2] = f.GetFloat(*e, "outerScale", 0, 1.0f);
    field.values[0] = f.GetFloat(*e, "innerValue", 0, 1.0f);
    field.values[1] = f.GetFloat(*e, "centerValue", 0, 1.0f);
    field.values[2] = f.GetFloat(*e, "outerValue", 0, 1.0f);
    return field;
}

LensFlareGraph ReadGraph(const fox2::DataSetFile& f, const fox2::Entity& shape, const char* prop) {
    LensFlareGraph graph;
    const auto link = f.GetLink(shape, prop);
    const fox2::Entity* e = link ? f.ResolveLink(*link) : nullptr;
    if (!e) {
        return graph;
    }
    graph.valid = true;
    for (int i = 0; i < 10; ++i) {
        graph.values[i] = f.GetFloat(*e, std::format("value0_{}", i), 0, 1.0f);
    }
    graph.values[10] = f.GetFloat(*e, "value1_0", 0, 1.0f);
    return graph;
}

float Interp(int32_t type, float d, float s0, float s1, float v0, float v1) {
    const float t = (d - s0) / (s1 - s0);
    switch (type) {
    case 0: return v0 + (v1 - v0) * t;
    case 1: return v0 + (v1 - v0) * 0.5f * (1.0f - std::cos(3.1415927f * t));
    case 2: return v0 + (v1 - v0) * t * t;
    default: return v0 + (v1 - v0) * (1.0f - (1.0f - t) * (1.0f - t));
    }
}

}

float LensFlareField::Eval(const glm::vec2& p, float inv_aspect) const {
    if (!valid) {
        return 1.0f;
    }
    const float y = p.y * inv_aspect;
    const float d = shape_type == 0 ? std::max(std::fabs(p.x), std::fabs(p.y)) : std::sqrt(p.x * p.x + y * y);
    if (d < scales[0]) {
        return values[0];
    }
    if (d < scales[1]) {
        return std::fabs(scales[0] - scales[1]) >= 1e-4f ? Interp(interp_type, d, scales[0], scales[1], values[0], values[1]) : values[1];
    }
    if (d < scales[2] && std::fabs(scales[1] - scales[2]) >= 1e-4f) {
        return Interp(interp_type, d, scales[1], scales[2], values[1], values[2]);
    }
    return values[2];
}

float LensFlareGraph::Eval(float t) const {
    if (!valid) {
        return 1.0f;
    }
    if (t <= 0.0f) {
        return values[0];
    }
    if (t >= 1.0f) {
        return values[10];
    }
    const float x = t * 10.0f;
    const int i = std::min(9, static_cast<int>(x));
    return values[i] + (x - static_cast<float>(i)) * (values[i + 1] - values[i]);
}

bool LoadLensFlare(std::span<const uint8_t> data, const std::string& name, LensFlareDef& out) {
    fox2::DataSetFile f;
    if (!f.Load(name, data)) {
        return false;
    }
    out = LensFlareDef{};
    out.name = name;
    const fox2::Entity* root = nullptr;
    for (const fox2::Entity& e : f.Entities()) {
        if (e.class_name.find("LensFlareRoot") != std::string::npos) {
            root = &e;
            break;
        }
    }
    if (!root || (f.GetUInt(*root, "flags") & 1u) == 0) {
        return false;
    }
    out.exposure_blend = f.GetFloat(*root, "exposureBlend");
    out.collision_check = f.GetBool(*root, "needCollisionCheck");
    const fox2::Property* shapes = f.FindProperty(*root, "shapes");
    const size_t count = shapes ? shapes->Count() : 0;
    for (size_t i = 0; i < count; ++i) {
        const auto link = f.GetLink(*root, "shapes", i);
        const fox2::Entity* shape = link ? f.ResolveLink(*link) : nullptr;
        if (!shape || shape->class_name != "TppLensFlareShape" || (f.GetUInt(*shape, "flags") & 1u) == 0) {
            continue;
        }
        LensFlareElement el;
        el.width = f.GetFloat(*shape, "width", 0, 1.0f);
        el.height = f.GetFloat(*shape, "height", 0, 1.0f);
        el.color = f.GetVec4(*shape, "baseColor");
        el.offset_type = f.GetInt(*shape, "offsetType", 0, 1);
        el.offset_scale = f.GetFloat(*shape, "offsetScale");
        el.base_offset = glm::vec2(f.GetFloat(*shape, "baseOffsetX"), f.GetFloat(*shape, "baseOffsetY"));
        el.rotate_type = f.GetInt(*shape, "rotateType");
        el.base_rotate = f.GetFloat(*shape, "baseRotate");
        el.scale_x = ReadField(f, *shape, "scaleFieldX");
        el.scale_y = ReadField(f, *shape, "scaleFieldY");
        el.alpha = ReadField(f, *shape, "alphaField");
        el.scale_at_light = f.GetBool(*shape, "scaleFieldPickSunPositionFlag");
        el.alpha_at_light = f.GetBool(*shape, "alphaFieldPickSunPositionFlag");
        el.fade_out = f.GetFloat(*shape, "shieldFadeOutTime", 0, 0.2f);
        el.fade_in = f.GetFloat(*shape, "shieldFadeInTime", 0, 0.2f);
        el.angle_scale_x = ReadGraph(f, *shape, "angleScaleGraphX");
        el.angle_scale_y = ReadGraph(f, *shape, "angleScaleGraphY");
        el.angle_alpha = ReadGraph(f, *shape, "angleAlphaGraph");
        el.distance_scaling = f.GetInt(*shape, "distanceScaling");
        el.limit_distance = f.GetFloat(*shape, "limitDistance", 0, 100.0f);
        if (const auto mat_link = f.GetLink(*shape, "material")) {
            if (const fox2::Entity* material = f.ResolveLink(*mat_link)) {
                el.texture = f.GetString(*material, "texture");
            }
        }
        if (!el.texture.empty()) {
            out.elements.push_back(std::move(el));
        }
    }
    return !out.elements.empty();
}

}
