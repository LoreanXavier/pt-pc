#include "game/render_scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/log.h"
#include "engine/data/fox2.h"
#include "engine/fs/vfs.h"
#include "engine/physics/collision_world.h"
#include "engine/render/mesh.h"
#include "engine/core/strcode.h"
#include "game/demo_system.h"
#include "game/game.h"
#include "game/stage_data.h"
#include "game/stage_manager.h"
#include "game/survival/survival.h"

namespace pt::game {
namespace {

constexpr float kCameraAperture = 1.0f;
constexpr float kCameraShutter = 0.008333333f;

constexpr float kPi = 3.14159265358979f;

struct HandyPose {
    float view_pitch;
    glm::vec3 pivot;
};

constexpr glm::vec3 kHandOffset{0.0377f, 0.0603f, 0.1264f};
constexpr HandyPose kHandyPoses[] = {{-55.0f, {-0.2402f, 0.0431f, 0.2678f}}, {-39.75f, {-0.2466f, -0.0074f, 0.2563f}},
                                    {-20.09f, {-0.2482f, -0.0671f, 0.2221f}}, {-10.98f, {-0.2488f, -0.0905f, 0.2022f}},
                                    {-2.57f, {-0.2503f, -0.1142f, 0.1773f}},  {0.0f, {-0.2495f, -0.1130f, 0.1740f}},
                                    {39.45f, {-0.2554f, -0.1395f, 0.0666f}},
                                    {50.5f, {-0.2507f, -0.1393f, 0.0402f}}};
constexpr HandyPose kPickupHoldPoses[] = {{-55.0f, {-0.2446f, -0.0958f, 0.1462f}}, {-45.8f, {-0.2442f, -0.1141f, 0.1072f}},
                                          {-38.15f, {-0.2459f, -0.1183f, 0.0859f}}, {-22.17f, {-0.2484f, -0.1213f, 0.0380f}}};

struct BreathLoop {
    float seconds;
    float c[5][6];
};
constexpr BreathLoop kPlayBreath{50.0f / 30.0f,
                                 {{-0.2049f, 0.3639f, 0.0040f, 0.0025f, 0.0133f, 0.0059f},
                                  {0.1032f, 0.5280f, -0.0113f, 0.0153f, -0.0011f, 0.0100f},
                                  {0.8702f, 0.0298f, -0.0437f, 0.1596f, -0.0671f, -0.0205f},
                                  {0.3388f, -1.9833f, -0.0440f, -0.1835f, 0.0302f, -0.0566f},
                                  {-3.1988f, -0.0074f, -0.1765f, 0.0261f, -0.0551f, 0.0004f}}};
constexpr BreathLoop kHoldBreath{36.0f / 30.0f,
                                 {{-0.1091f, 0.0006f, 0.0042f, 0.0074f, -0.0082f, 0.0047f},
                                  {-0.0811f, 0.1397f, -0.0016f, -0.0074f, -0.0037f, -0.0005f},
                                  {1.6042f, 0.5580f, -0.1545f, -0.1505f, 0.1551f, -0.0670f},
                                  {1.6803f, -2.3867f, 0.0950f, 0.1597f, 0.1054f, 0.0317f},
                                  {-4.0313f, 1.9254f, -0.5143f, -0.1750f, -0.1193f, 0.0645f}}};

std::array<float, 5> BreathAt(const BreathLoop& loop, float time) {
    const float phase = 6.2831853f * (time / loop.seconds - std::floor(time / loop.seconds));
    std::array<float, 5> out{};
    for (size_t k = 0; k < out.size(); ++k) {
        for (int harmonic = 0; harmonic < 3; ++harmonic) {
            const float a = static_cast<float>(harmonic + 1) * phase;
            out[k] += loop.c[k][2 * harmonic] * std::cos(a) + loop.c[k][2 * harmonic + 1] * std::sin(a);
        }
    }
    return out;
}

template <size_t N>
HandyPose PoseAt(const HandyPose (&poses)[N], float view_pitch) {
    if (view_pitch <= poses[0].view_pitch) {
        return poses[0];
    }
    for (size_t i = 1; i < N; ++i) {
        const HandyPose& a = poses[i - 1];
        const HandyPose& b = poses[i];
        if (view_pitch <= b.view_pitch) {
            const float t = (view_pitch - a.view_pitch) / (b.view_pitch - a.view_pitch);
            return {view_pitch, glm::mix(a.pivot, b.pivot, t)};
        }
    }
    return poses[N - 1];
}

}

void HandyLightPose(const Camera& camera, const glm::vec3& aim_point, float pickup_hold, float demo_pose, glm::vec3& position,
                    glm::vec3& direction, glm::vec3* right_out, glm::vec3* up_out, glm::vec2 breath) {
    const glm::vec3 forward = camera.Forward();
    const glm::vec3 right = camera.Right();
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));
    const float view_pitch = glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
    HandyPose pose = PoseAt(kHandyPoses, view_pitch);
    if (pickup_hold > 0.0f) {
        pose.pivot = glm::mix(pose.pivot, PoseAt(kPickupHoldPoses, view_pitch).pivot, std::min(pickup_hold, 1.0f));
    }
    const glm::vec3 target_cam(glm::dot(aim_point - camera.position, right), glm::dot(aim_point - camera.position, up),
                               glm::dot(aim_point - camera.position, forward));
    glm::vec3 light_cam = pose.pivot + kHandOffset;
    glm::vec3 d_cam(0.0f, 0.0f, 1.0f);
    glm::vec3 r_cam(1.0f, 0.0f, 0.0f);
    glm::vec3 u_cam(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 6; ++i) {
        const glm::vec3 to_target = target_cam - light_cam;
        if (glm::dot(to_target, to_target) < 1.0e-6f) {
            break;
        }
        d_cam = glm::normalize(to_target);
        r_cam = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), d_cam);
        r_cam = glm::dot(r_cam, r_cam) > 1.0e-8f ? glm::normalize(r_cam) : glm::vec3(1.0f, 0.0f, 0.0f);
        u_cam = glm::cross(d_cam, r_cam);
        light_cam = pose.pivot + r_cam * kHandOffset.x + u_cam * kHandOffset.y + d_cam * kHandOffset.z;
    }
    if (breath.y > 0.0f) {
        const std::array<float, 5> play = BreathAt(kPlayBreath, breath.x), hold = BreathAt(kHoldBreath, breath.x);
        const float w = std::min(pickup_hold, 1.0f);
        std::array<float, 5> v{};
        for (size_t k = 0; k < v.size(); ++k) v[k] = (play[k] + (hold[k] - play[k]) * w) * breath.y;
        light_cam += glm::vec3(v[2], v[3], v[4]) * 0.001f;
        const glm::quat turn = glm::angleAxis(glm::radians(-v[1]), r_cam) * glm::angleAxis(glm::radians(v[0]), glm::vec3(0.0f, 1.0f, 0.0f));
        d_cam = glm::normalize(turn * d_cam);
        r_cam = glm::normalize(turn * r_cam);
        u_cam = glm::normalize(turn * u_cam);
    }
    position = camera.position + right * light_cam.x + up * light_cam.y + forward * light_cam.z;
    direction = glm::normalize(right * d_cam.x + up * d_cam.y + forward * d_cam.z);
    if (right_out) {
        *right_out = glm::normalize(right * r_cam.x + up * r_cam.y + forward * r_cam.z);
    }
    if (up_out) {
        *up_out = glm::normalize(right * u_cam.x + up * u_cam.y + forward * u_cam.z);
    }
    if (demo_pose > 0.0f) {
        const glm::vec3 fixed = camera.position - right * 0.165f - up * 0.13f + forward * 0.185f;
        position = glm::mix(position, fixed, std::min(demo_pose, 1.0f));
        const glm::vec3 to_target = aim_point - position;
        if (glm::dot(to_target, to_target) > 1.0e-6f) {
            direction = glm::normalize(to_target);
        }
    }
}

glm::vec3 HandyMaskUp(const Camera& camera, const glm::vec3& direction, float clip_roll_degrees, glm::vec2 pickup) {
    const float kRestYaw = glm::radians(-0.023f);
    const float kRestPitch = glm::radians(25.828f);
    const float kRestRoll = glm::radians(-57.311f);
    const glm::vec3 y(0.0f, 1.0f, 0.0f);
    const glm::vec3 heading(-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw));
    const glm::vec3 side = glm::cross(heading, y);
    const glm::vec3 rest = side * (std::sin(kRestYaw) * std::cos(kRestPitch)) + y * std::sin(kRestPitch) + heading * (std::cos(kRestYaw) * std::cos(kRestPitch));
    const glm::vec3 rest_ref = glm::normalize(y - rest * glm::dot(y, rest));
    const glm::vec3 rest_up = rest_ref * std::cos(kRestRoll) + glm::cross(rest, rest_ref) * std::sin(kRestRoll);
    const glm::vec3 d = glm::normalize(direction);
    const glm::vec3 hand_up = glm::quat(rest, d) * rest_up;
    const auto square = [&](const glm::vec3& v) {
        const glm::vec3 s = v - d * glm::dot(v, d);
        return glm::length(s) > 1.0e-5f ? glm::normalize(s) : glm::vec3(0.0f);
    };
    Camera level = camera;
    level.roll = 0.0f;
    const glm::vec3 level_up = square(level.Up());
    const glm::vec3 view_up = square(camera.Up());
    const glm::vec3 up = square(hand_up);
    if (level_up == glm::vec3(0.0f) || view_up == glm::vec3(0.0f) || up == glm::vec3(0.0f)) {
        return up == glm::vec3(0.0f) ? view_up : up;
    }
    const float hand_angle = std::atan2(glm::dot(glm::cross(level_up, up), d), glm::dot(level_up, up));
    float angle = hand_angle + glm::radians(clip_roll_degrees);
    if (pickup.y > 0.0f) {
        angle += std::remainder(glm::radians(pickup.x) - angle, glm::two_pi<float>()) * std::min(pickup.y, 1.0f);
    }
    return glm::normalize(view_up * std::cos(angle) + glm::cross(d, view_up) * std::sin(angle));
}

namespace {

template <typename T>
T ReadAt(const std::vector<uint8_t>& data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

std::string ShortName(const std::string& name) {
    const size_t bar = name.find_last_of('|');
    return bar == std::string::npos ? name : name.substr(bar + 1);
}

float CosHalf(float degrees) {
    return std::cos(degrees * kPi / 360.0f);
}

float InverseRange(float cos_inner, float cos_outer) {
    return 1.0f / std::max(cos_inner - cos_outer, 1.0e-5f);
}

float InverseInner(float scale) {
    return scale >= 0.9999f ? 1.0e4f : 1.0f / (1.0f - scale);
}

void SpotAxes(const glm::mat3& world, glm::vec3 reach, glm::vec3& direction, glm::vec3& up) {
    if (glm::dot(reach, reach) < 1.0e-12f) {
        reach = glm::vec3(0.0f, -1.0f, 0.0f);
    }
    const glm::vec3 v = glm::normalize(reach);
    const glm::vec3 z(0.0f, 0.0f, 1.0f);
    glm::quat arc(0.0f, 1.0f, 0.0f, 0.0f);
    if (glm::dot(v + z, v + z) >= 1.0e-5f) {
        const glm::vec3 axis = glm::cross(z, v);
        arc = glm::normalize(glm::quat(1.0f + glm::dot(z, v), axis.x, axis.y, axis.z));
    }
    const glm::mat3 rotation(glm::normalize(world[0]), glm::normalize(world[1]), glm::normalize(world[2]));
    direction = glm::normalize(rotation * v);
    up = glm::normalize(rotation * (arc * glm::vec3(0.0f, 1.0f, 0.0f)));
}

const char* ForcedLightSet() {
    static const char* value = std::getenv("PT_FORCE_LIGHTS");
    return value && *value ? value : nullptr;
}

bool NameListed(const char* list_value, const std::string& name) {
    if (!list_value || !*list_value) {
        return false;
    }
    for (std::string_view list(list_value); !list.empty();) {
        const size_t comma = list.find(',');
        const std::string_view token = list.substr(0, comma);
        if (!token.empty() && name.find(token) != std::string::npos) {
            return true;
        }
        list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
    }
    return false;
}

bool Forced(const fox2::DataSetFile& file, const fox2::Entity& e) {
    const char* set = ForcedLightSet();
    return set && file.EntityName(e).find(set) != std::string::npos;
}

bool EntityEnabled(const Stage* stage, const fox2::DataSetFile& file, const fox2::Entity& e) {
    if (Forced(file, e)) {
        return true;
    }
    if (stage) {
        if (const BodyState* body = stage->FindBody(&e)) {
            return body->enable;
        }
    }
    return file.GetBool(e, "enable", 0, true);
}

}

const RenderSceneBuilder::ProbeFile* RenderSceneBuilder::LoadProbes(Vfs& vfs, const std::string& path) {
    auto it = probe_files_.find(path);
    if (it != probe_files_.end()) {
        return &it->second;
    }
    ProbeFile file;
    auto bytes = vfs.ReadFile(path);
    if (bytes && bytes->size() > 0xB0 && ReadAt<uint32_t>(*bytes, 0) == 3) {
        const std::vector<uint8_t>& d = *bytes;
        const uint32_t count = ReadAt<uint32_t>(d, 0x7C);
        const uint32_t divisions = ReadAt<uint32_t>(d, 0x8C);
        const size_t table = 0xB0 + size_t(divisions) * 4;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t entry = table + size_t(i) * 12;
            const uint32_t name_offset = ReadAt<uint32_t>(d, entry);
            const uint32_t data_offset = ReadAt<uint32_t>(d, entry + 4);
            if (name_offset >= d.size() || data_offset + 72 > d.size()) {
                continue;
            }
            std::string name;
            for (size_t k = name_offset; k < d.size() && d[k] != 0; ++k) {
                name.push_back(static_cast<char>(d[k]));
            }
            std::array<glm::vec3, 9> sh{};
            for (int c = 0; c < 9; ++c) {
                for (int ch = 0; ch < 3; ++ch) {
                    sh[c][ch] = glm::unpackHalf1x16(ReadAt<uint16_t>(d, data_offset + size_t(c) * 8 + size_t(ch) * 2));
                }
            }
            file.probes[name] = sh;
        }
        LogInfo("render scene: {} probes from {}", file.probes.size(), path);
    } else {
        LogWarn("render scene: probe file {} missing or invalid", path);
    }
    return &probe_files_.emplace(path, std::move(file)).first->second;
}

void RenderSceneBuilder::AddLight(const fox2::DataSetFile& f, const fox2::Entity& e, const glm::mat4& file_to_world, uint64_t id,
                                  SceneLighting& out) const {
    static const char* skip = std::getenv("PT_LIGHT_SKIP");
    static const char* no_specular = std::getenv("PT_LIGHT_NOSPEC");
    const std::string name = f.EntityName(e);
    if (NameListed(skip, name)) {
        return;
    }
    SceneLight l;
    l.id = id;
    l.name = name;
    const bool spot = e.class_name == "SpotLight";
    l.type = spot ? LightType::Spot : LightType::Point;
    const glm::mat4 world = file_to_world * f.WorldTransform(e);
    l.position = glm::vec3(world[3]);
    glm::vec3 reach = glm::vec3(f.GetVec4(e, "reachPoint"));
    static const char* axis_mode = std::getenv("PT_SPOT_AXIS");
    if (axis_mode && std::strcmp(axis_mode, "reach") != 0) {
        if (const fox2::Entity* target = f.GetEntity(e, "irradiationPoint")) {
            const glm::vec3 to = glm::vec3(file_to_world * f.WorldTransform(*target)[3]) - l.position;
            reach = std::strcmp(axis_mode, "target") == 0 ? glm::transpose(glm::mat3(world)) * to : to;
        }
    }
    SpotAxes(glm::mat3(world), reach, l.direction, l.up);
    const glm::vec3 color = glm::vec3(f.GetVec4(e, "color"));
    float lumen = f.GetFloat(e, "lumen");
    const std::string_view last = std::string_view(name).substr(name.rfind('|') == std::string::npos ? 0 : name.rfind('|') + 1);
    if (spot && (last == "8SL_stairs_down0000" || last == "8SL_stairs_down0002")) {
        lumen = 40.0f;
        l.stair_area = true;
    }
    const glm::vec3 rgb = light_math::ColorFromTemperature(f.GetFloat(e, "temperature", 0, 6500.0f), f.GetFloat(e, "colorDeflection"), lumen, color);
    l.inner_range = f.GetFloat(e, "innerRange");
    l.outer_range = std::max(f.GetFloat(e, "outerRange"), 0.01f);
    l.dimmer = f.GetFloat(e, "dimmer");
    l.source_radius = light_math::SourceRadius(f.GetFloat(e, "lightSize"));
    l.cast_shadow = f.GetBool(e, "castShadow");
    l.priority = 0xC0;
    l.specular_scale = f.GetBool(e, "hasSpecular", 0, true) && !NameListed(no_specular, name) ? 1.0f : 0.0f;
    l.shadow_strength = f.GetFloat(e, "LodShadowDrawRate", 0, 1.0f);
    l.lod = glm::vec4(f.GetFloat(e, "LodNearSize"), f.GetFloat(e, "LodFarSize"), std::ldexp(0.5f, f.GetInt(e, "lodRadiusLevel") & 31),
                      f.GetInt(e, "lodFadeType") == 1 ? 1.0f : 0.0f);
    l.shadow_bias = f.GetFloat(e, "shadowBias") * 0.001f;
    if (spot) {
        const float umbra = f.GetFloat(e, "umbraAngle");
        const float penumbra = f.GetFloat(e, "penumbraAngle");
        const float exponent = f.GetFloat(e, "attenuationExponent", 0, 1.0f);
        const float omega = light_math::SpotSolidAngle(umbra, penumbra, exponent);
        l.intensity = rgb / omega * f.GetFloat(e, "powerScale", 0, 1.0f) / kPi;
        l.cos_outer = CosHalf(umbra);
        l.inv_cone_range = InverseRange(CosHalf(penumbra), l.cos_outer);
        l.cone_exponent = exponent;
        const float shadow_umbra = f.GetFloat(e, "shadowUmbraAngle", 0, umbra);
        const float shadow_penumbra = f.GetFloat(e, "shadowPenumbraAngle", 0, penumbra);
        l.shadow_cos_outer = CosHalf(shadow_umbra);
        l.shadow_inv_cone_range = InverseRange(CosHalf(shadow_penumbra), l.shadow_cos_outer);
        l.shadow_fov = shadow_umbra * kPi / 180.0f;
        l.view_bias = f.GetFloat(e, "viewBias") * 0.001f;
    } else {
        l.intensity = rgb / (4.0f * kPi) / kPi;
    }
    const fox2::Entity* irradiation = f.GetEntity(e, "irradiationPoint");
    l.rank_point = irradiation ? glm::vec3(file_to_world * f.WorldTransform(*irradiation)[3]) : l.position;
    l.has_rank_point = true;
    if (const fox2::Entity* area = f.GetEntity(e, "lightArea")) {
        const glm::mat4 box = file_to_world * f.WorldTransform(*area) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
        if (std::abs(glm::determinant(glm::mat3(box))) > 1.0e-9f) {
            l.has_area = true;
            l.area_world = box;
            l.area_to_box = glm::inverse(box);
        }
    }
    out.lights.push_back(l);
}

std::optional<SceneOccluder> RenderSceneBuilder::BuildOccluder(const fox2::DataSetFile& f, const fox2::Entity& e, const glm::mat4& file_to_world) {
    if (!f.GetBool(e, "isEnable", 0, true)) {
        return std::nullopt;
    }
    SceneOccluder o;
    o.count = static_cast<uint32_t>(std::clamp(f.GetInt(e, "numVertices", 0, 4), 0, 7));
    if (o.count < 3) {
        return std::nullopt;
    }
    o.one_sided = f.GetBool(e, "isOneSideMode");
    const glm::mat4 world = file_to_world * f.WorldTransform(e);
    for (uint32_t i = 0; i < o.count; ++i) {
        o.points[i] = glm::vec3(world * glm::vec4(glm::vec3(f.GetVec4(e, "positions", i)), 1.0f));
    }
    return o;
}

std::optional<SceneProbe> RenderSceneBuilder::BuildProbe(Vfs& vfs, const fox2::DataSetFile& f, const fox2::Entity& e, const glm::mat4& file_to_world) {
    const fox2::Entity* area = f.GetEntity(e, "lightArea");
    const fox2::Entity* coefficients = f.GetEntity(e, "shCoefficientsData");
    if (!area || !coefficients) {
        return std::nullopt;
    }
    std::string path = f.GetString(*coefficients, "lpshFile");
    if (path.empty()) {
        path = f.GetString(*coefficients, "filePath");
    }
    if (path.empty()) {
        return std::nullopt;
    }
    const ProbeFile* file = LoadProbes(vfs, path);
    auto it = file->probes.find(ShortName(f.EntityName(e)));
    static const bool dark_probes = [] {
        const char* env = std::getenv("PT_DARK_PROBES");
        return !env || std::atoi(env) != 0;
    }();
    const bool dark = dark_probes && (f.GetUInt(e, "localFlags") & 16u) != 0;
    if (it == file->probes.end() && !dark) {
        return std::nullopt;
    }
    SceneProbe p;
    p.box_world = file_to_world * f.WorldTransform(*area) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
    if (std::abs(glm::determinant(glm::mat3(p.box_world))) < 1.0e-12f) {
        return std::nullopt;
    }
    p.world_to_box = glm::inverse(p.box_world);
    auto inner = [&](const char* name) { return InverseInner(f.GetFloat(e, name, 0, 1.0f)); };
    p.positive_scale = glm::vec3(inner("innerScaleXPositive"), inner("innerScaleYPositive"), inner("innerScaleZPositive"));
    p.negative_scale = glm::vec3(inner("innerScaleXNegative"), inner("innerScaleYNegative"), inner("innerScaleZNegative"));
    p.priority = f.GetInt(e, "priority");
    p.weight = 1.0f;
    p.name = f.EntityName(e);
    static const float kBasis[9] = {0.2820948f, 0.4886025f, 0.4886025f, 0.4886025f, 1.0925484f, 1.0925484f, 0.3153916f, 1.0925484f, 0.5462742f};
    static const bool sh_legacy = [] {
        const char* env = std::getenv("PT_SH_LEGACY");
        return env && std::atoi(env) != 0;
    }();
    static const float kLegacyLobe[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    for (int i = 0; i < 9; ++i) {
        const float basis = sh_legacy ? kBasis[i] * kLegacyLobe[i] : kBasis[i];
        p.sh[i] = dark || it == file->probes.end() ? glm::vec3(0.0f) : it->second[i] * basis;
    }
    return p;
}

void RenderSceneBuilder::LoadResidentSettings(Game& game) {
    if (resident_.loaded) {
        return;
    }
    Stage* resident = game.Stages().Resident();
    if (!resident) {
        return;
    }
    resident_.loaded = true;
    for (const auto& data : resident->files) {
        const fox2::DataSetFile& f = *data->file;
        for (const fox2::Entity& e : f.Entities()) {
            if (e.class_name == "GrPluginSettings") {
                resident_.tonemap_speed = f.GetFloat(e, "tonemapSpeed", 0, 1.0f);
                ExposureSettings& s = resident_.defaults;
                s.min_ev = f.GetFloat(e, "minExposure", 0, s.min_ev);
                s.max_ev = f.GetFloat(e, "maxExposure", 0, s.max_ev);
                s.compensation = f.GetFloat(e, "exposureCompensation", 0, s.compensation);
                s.key = f.GetFloat(e, "keyValue", 0, s.key);
                s.bloom_weight = f.GetFloat(e, "bloomWeight", 0, s.bloom_weight);
                s.bloom_extraction = f.GetFloat(e, "bloomBrightnessExtraction", 0, s.bloom_extraction);
                s.bloom_size = f.GetFloat(e, "bloomSize", 0, s.bloom_size);
                resident_.plugin_flags = static_cast<uint32_t>(f.GetInt(e, "flags", 0, static_cast<int32_t>(resident_.plugin_flags)));
            } else if (e.class_name == "GrReflectionSetting") {
                if (const fox2::Property* p = f.FindProperty(e, "reflectionTexturePath"); p && p->Count() > 0) {
                    resident_.reflection_texture = f.ElementString(*p, 0);
                }
            } else if (e.class_name == "TppTonemap") {
                resident_.tpp_tonemap = f.GetBool(e, "enable", 0, true);
                resident_.tpp_threshold = f.GetFloat(e, "threshold", 0, resident_.tpp_threshold);
                resident_.tpp_range = f.GetFloat(e, "range", 0, resident_.tpp_range);
            } else if (e.class_name == "ColorCorrectionData") {
                resident_.color_scale = glm::vec3(f.GetVec4(e, "colorScale"));
                resident_.start_slope = f.GetFloat(e, "startSlope", 0, resident_.start_slope);
                resident_.end_slope = f.GetFloat(e, "endSlope", 0, resident_.end_slope);
            } else if (e.class_name == "ShTextureLoader") {
                if (const fox2::Property* p = f.FindProperty(e, "textures")) {
                    for (size_t i = 0; i < p->Count(); ++i) {
                        std::string path = f.ElementString(*p, i);
                        if (const size_t dot = path.find_last_of('.'); dot != std::string::npos && dot > path.find_last_of('/')) {
                            path = path.substr(0, dot);
                        }
                        resident_.luts[f.KeyString(*p, i)] = path;
                    }
                }
            }
        }
    }
    resident_.defaults.speed = resident_.tonemap_speed;
    LogInfo("render scene: resident settings, tonemap speed {}, color scale ({} {} {}), {} LUTs, plugin flags {:#x}, reflection texture {}",
            resident_.tonemap_speed, resident_.color_scale.x, resident_.color_scale.y, resident_.color_scale.z, resident_.luts.size(),
            resident_.plugin_flags, resident_.reflection_texture);
}

void RenderSceneBuilder::AddHandyLight(Game& game, const Camera& camera, const glm::vec3& aim_point, float dt, SceneLighting& out, const TickBlend* blend) {
    const ScreenEffects& fx = game.Effects();
    glm::vec3 target = fx.handy_light_color;
    if (target == glm::vec3(1.0f)) target = game.LisaHintLightColour();
    if (!handy_initialized_) {
        handy_color_ = handy_from_ = handy_target_ = target;
        handy_initialized_ = true;
    }
    if (target != handy_target_) {
        handy_from_ = handy_color_;
        handy_target_ = target;
        handy_fade_ = fx.handy_light_color_fade ? 2.0f : 0.0f;
        if (handy_fade_ <= 0.0f) {
            handy_color_ = target;
        }
    }
    if (handy_fade_ > 0.0f) {
        handy_fade_ = std::max(0.0f, handy_fade_ - dt);
        const float t = handy_fade_ * 0.5f;
        handy_color_ = handy_target_ * (1.0f - t) + handy_from_ * t;
    }
    const Player& player = game.GetPlayer();
    static const bool handy_off = [] {
        const char* off = std::getenv("PT_RENDER_OFF");
        return off && std::strstr(off, "handy") != nullptr;
    }();
    if (!player.handy_light.enable || handy_off) {
        return;
    }
    const HandyLightParameters& p = game.Parameters().handy_light;
    SceneLight l;
    l.id = kHandyLightId;
    l.light_fitting = kHandyLightFitting;
    l.name = "PlayerHandyLight";
    l.type = LightType::Spot;
    const glm::vec3 up = glm::normalize(glm::cross(camera.Right(), camera.Forward()));
    HandyLightPose(camera, aim_point, game.HandyPickupHold(), game.HandyDemoPose(), l.position, l.direction, nullptr, nullptr, game.HandyBreath());
    if (static const bool pose_log = std::getenv("PT_HANDY_POSE_LOG") != nullptr; pose_log) {
        const glm::vec3 rel = l.position - camera.position, f = camera.Forward(), r = camera.Right(), u = glm::normalize(glm::cross(r, f));
        const glm::vec3 d(glm::dot(l.direction, r), glm::dot(l.direction, u), glm::dot(l.direction, f));
        LogInfo("handy pose: frame {} at ({:.1f} {:.1f} {:.1f}) aim {:+.3f} {:+.3f} breath {:.2f}", game.Frame(), 1000.0f * glm::dot(rel, r),
                1000.0f * glm::dot(rel, u), 1000.0f * glm::dot(rel, f), glm::degrees(std::atan2(d.x, d.z)), glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f))),
                game.HandyBreath().y);
    }
    if (const auto& pose = game.HandyPoseOverride()) {
        l.position = pose->first;
        l.direction = pose->second;
    }
    if (glm::vec3 lens; game.HandyLens(lens)) {
        if (blend && blend->handy_lens_valid && glm::distance(blend->handy_lens, lens) <= 1.0f) {
            lens = glm::mix(blend->handy_lens, lens, blend->t);
        }
        if (game.ThirdPerson() && !game.FreeView()) {
            if (game.BodyShown()) {
                l.position = lens;
            } else {
                const float weight = std::clamp(game.ThirdPersonWeight() / 0.25f, 0.0f, 1.0f);
                const float pose_weight = weight * weight * (3.0f - 2.0f * weight);
                l.position = glm::mix(l.position, lens, pose_weight);
            }
        } else if (game.DetachedView()) {
            l.position = lens;
        }
    }
    if (game.HandyPoseOverride() || game.SurvivalActive()) {
        l.up = glm::normalize(up - l.direction * glm::dot(up, l.direction));
    } else {
        l.up = HandyMaskUp(camera, l.direction, blend ? glm::mix(blend->handy_mask_roll, game.HandyMaskRoll(), blend->t) : game.HandyMaskRoll(),
                           blend ? glm::mix(blend->handy_pickup_roll, game.HandyPickupMaskRoll(), blend->t) : game.HandyPickupMaskRoll());
    }
    if (static const bool mask_log = std::getenv("PT_HANDY_POSE_LOG") != nullptr; mask_log) {
        const glm::vec3 d = glm::normalize(l.direction);
        glm::vec3 view_up = camera.Up() - d * glm::dot(camera.Up(), d);
        view_up = glm::normalize(view_up);
        LogInfo("handy mask: frame {} roll {:+.3f} clip roll {:+.3f}", game.Frame(),
                glm::degrees(std::atan2(glm::dot(glm::cross(view_up, l.up), d), glm::dot(view_up, l.up))), game.HandyMaskRoll());
    }
    const glm::vec3 color = glm::vec3(p.color[0], p.color[1], p.color[2]) * handy_color_;
    const glm::vec3 rgb = light_math::ColorFromTemperature(p.temperature, 0.0f, p.lumen, color);
    const float omega = light_math::SpotSolidAngle(p.umbra_angle, p.penumbra_angle, p.attenuation_exponent);
    l.intensity = rgb / omega * p.power_scale / kPi;
    static const float street_gain = [] { const char* g = std::getenv("PT_STREET_HANDY_GAIN"); return g ? static_cast<float>(std::atof(g)) : 16.0f; }();
    if (game.StreetWalkActive()) l.intensity *= street_gain;
    game.SetHandyLensRadiance(l.intensity / (kPi * 0.020f * 0.020f));
    l.source_radius = light_math::SourceRadius(p.light_size);
    l.inner_range = p.inner_range;
    l.outer_range = p.outer_range;
    l.dimmer = p.dimmer;
    if (game.StreetWalkActive()) {
        l.outer_range = std::max(l.outer_range, 14.0f);
    }
    l.cos_outer = CosHalf(p.umbra_angle);
    l.inv_cone_range = InverseRange(CosHalf(p.penumbra_angle), l.cos_outer);
    l.cone_exponent = p.attenuation_exponent;
    l.shadow_cos_outer = CosHalf(78.0f);
    l.shadow_inv_cone_range = InverseRange(CosHalf(78.0f), l.shadow_cos_outer);
    l.shadow_fov = 78.0f * kPi / 180.0f;
    l.shadow_bias = -10.0f * 0.001f;
    l.view_bias = -5.0f * 0.001f;
    l.cast_shadow = true;
    l.masked = true;
    l.mask_fov = p.umbra_angle * kPi / 180.0f;
    l.mask_soft = game.StreetWalkActive();
    out.lights.push_back(l);
    AddHandyReflection(game, l, out);
}

void RenderSceneBuilder::AddLisaGlint(Game& game, SceneLighting& out) const {
    glm::vec3 at, normal;
    float strength = 0.0f;
    if (!game.LisaHintGlint(at, normal, strength)) return;
    static const glm::vec3 tune = [] {
        glm::vec3 t(0.6f, 0.05f, 0.3f);
        if (const char* v = std::getenv("PT_LISA_HINT_GLINT")) std::sscanf(v, "%f,%f,%f", &t.x, &t.y, &t.z);
        return t;
    }();
    SceneLight l;
    l.id = 0x4C495341474C4E54ull;
    l.name = "LisaHintGlint";
    l.type = LightType::Point;
    l.position = at + normal * tune.y;
    l.intensity = glm::vec3(1.0f, 0.42f, 0.70f) * (tune.x * strength);
    l.inner_range = 0.0f;
    l.outer_range = tune.z;
    l.dimmer = 0.05f;
    l.diffuse_scale = 0.35f;
    l.cast_shadow = false;
    out.lights.push_back(l);
}

void RenderSceneBuilder::AddIntercomLight(Game& game, SceneLighting& out) const {
    glm::vec3 at, direction;
    glm::mat4 area(1.0f);
    bool has_area = false;
    float level = 0.0f;
    if (!game.GamePlusIntercomLight(at, direction, area, has_area, level)) return;
    struct Tune {
        float lumen = 500.0f;
        float kelvin = 9500.0f;
        float umbra = 150.0f;
        float penumbra = 80.0f;
        float range = 5.0f;
    };
    static const Tune tune = [] {
        Tune t;
        if (const char* v = std::getenv("PT_INTERCOM_LIGHT")) std::sscanf(v, "%f,%f,%f,%f,%f", &t.lumen, &t.kelvin, &t.umbra, &t.penumbra, &t.range);
        return t;
    }();
    SceneLight l;
    l.id = 0x494E54434F4D4C54ull;
    l.name = "IntercomLight";
    l.type = LightType::Spot;
    l.position = at;
    l.direction = direction;
    const glm::vec3 side = glm::cross(direction, glm::vec3(0.0f, 1.0f, 0.0f));
    l.up = glm::length(side) > 1.0e-4f ? glm::normalize(glm::cross(side, direction)) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 rgb = light_math::ColorFromTemperature(tune.kelvin, 0.0f, tune.lumen * level, glm::vec3(1.0f));
    l.intensity = rgb / light_math::SpotSolidAngle(tune.umbra, tune.penumbra, 1.0f) / kPi;
    l.source_radius = light_math::SourceRadius(0.12f);
    l.inner_range = 0.0f;
    l.outer_range = tune.range;
    l.cos_outer = CosHalf(tune.umbra);
    l.inv_cone_range = InverseRange(CosHalf(tune.penumbra), l.cos_outer);
    l.cone_exponent = 1.0f;
    l.cast_shadow = false;
    l.priority = 0xC0;
    l.rank_point = at;
    l.has_rank_point = true;
    if (has_area) {
        l.has_area = true;
        l.area_world = area;
        l.area_to_box = glm::inverse(area);
    }
    out.lights.push_back(l);
}

void RenderSceneBuilder::AddHandyReflection(Game& game, const SceneLight& handy, SceneLighting& out) const {
    static const bool trace_log = std::getenv("PT_VFX_TRACE_LOG") != nullptr;
    static const bool on = [] {
        const char* e = std::getenv("PT_REFLECT");
        return !e || std::atoi(e) != 0;
    }();
    const HandyReflection& r = game.HandyReflectionState();
    if (!on || !r.active) {
        return;
    }
    static const bool sample_scene = std::getenv("PT_REFLECT_SAMPLE") != nullptr;
    out.reflection_sample.active = sample_scene && !game.FreeView();
    std::copy(std::begin(r.samples), std::end(r.samples), std::begin(out.reflection_sample.points));
    const HandyLightParameters& p = game.Parameters().handy_light;
    constexpr float kScale = 2.5f;
    constexpr float kSpotScale = 1.5f;
    constexpr uint32_t kStaleUpdates = 8;
    const glm::vec3 colour = sample_scene && r.has_readback && r.readback_age <= kStaleUpdates ? r.readback : glm::vec3(1.0f);
    const float weights[2] = {r.first_weight, r.second_weight};
    const float attenuations[2] = {r.first_attenuation, r.second_attenuation};
    const glm::vec3 positions[2] = {r.position, r.second};
    static constexpr const char* kPointNames[2] = {"reflectLightPoint1", "reflectLightPoint2"};
    static constexpr const char* kSpotNames[2] = {"reflectLight0", "reflectLight02"};
    if (trace_log) {
        LogInfo("reflect trace: hit ({:.4f} {:.4f} {:.4f}), handy RGB ({:.3f} {:.3f} {:.3f}), readback {} age {} ({:.3f} {:.3f} {:.3f}), "
                "used colour ({:.3f} {:.3f} {:.3f}), weights ({:.3f} {:.3f}), attenuation ({:.5f} {:.5f}), spot share {:.3f}",
                r.position.x, r.position.y, r.position.z, handy.intensity.r, handy.intensity.g, handy.intensity.b, r.has_readback,
                r.readback_age, r.readback.r, r.readback.g, r.readback.b, colour.r, colour.g, colour.b, r.first_weight, r.second_weight,
                r.first_attenuation, r.second_attenuation, r.spot_share);
    }
    for (int i = 0; i < 2; ++i) {
        const float base = weights[i] * attenuations[i] * kScale;
        for (int spot = 0; spot < 2; ++spot) {
            const float share = spot ? r.spot_share * kSpotScale : 1.0f - r.spot_share;
            const bool enabled = weights[i] > 0.0f && (spot ? r.spot_share > 0.0f : r.spot_share < 1.0f);
            if (!enabled || base * share <= 0.0f) {
                continue;
            }
            SceneLight l;
            l.id = kHandyReflectionId + static_cast<uint64_t>(i * 2 + spot);
            l.name = spot ? kSpotNames[i] : kPointNames[i];
            l.type = spot ? LightType::Spot : LightType::Point;
            l.position = positions[i];
            l.intensity = handy.intensity * colour * (base * share);
            l.inner_range = 0.4f * p.inner_range;
            l.outer_range = 0.4f * p.outer_range;
            l.dimmer = 4.0f;
            l.specular_scale = 0.0f;
            l.cast_shadow = false;
            if (spot) {
                l.direction = r.normal;
                l.up = std::abs(r.normal.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
                l.cos_outer = CosHalf(200.0f);
                l.inv_cone_range = InverseRange(1.0f, l.cos_outer);
                l.cone_exponent = 1.0f;
            }
            out.lights.push_back(l);
            if (trace_log && !spot) {
                LogInfo("reflect trace: {} position ({:.4f} {:.4f} {:.4f}) RGB ({:.3f} {:.3f} {:.3f}) range {:.3f}", l.name,
                        l.position.x, l.position.y, l.position.z, l.intensity.r, l.intensity.g, l.intensity.b, l.outer_range + l.dimmer);
            }
        }
    }
}

void RenderSceneBuilder::AddMirrors(Game& game, SceneLighting& out) const {
    game.Stages().ForEachStage(
        [&](Stage& stage) {
            for (const auto& data : stage.files) {
                const fox2::DataSetFile& f = *data->file;
                for (const fox2::Entity& e : f.Entities()) {
                    if (e.class_name != "Mirror") {
                        continue;
                    }
                    const fox2::Entity* model = f.GetEntity(e, "staticModelHandle");
                    if (!model) {
                        continue;
                    }
                    const fox2::Entity* area = f.GetEntity(e, "lightAreaLocatorHandle");
                    for (const Stage::Draw& draw : stage.draws) {
                        if (draw.entity != model || !draw.mesh) {
                            continue;
                        }
                        const BodyState* body = stage.FindBody(draw.entity);
                        if (body && !body->visible) {
                            continue;
                        }
                        SceneMirror mirror{draw.mesh, stage.file_to_world * draw.file_transform};
                        if (area) {
                            const float size = f.GetFloat(*area, "size", 0, 1.0f);
                            const glm::mat4 local = f.WorldTransform(*area);
                            const glm::mat4 placement = stage.file_to_world * local;
                            glm::mat3 axes(local);
                            for (const fox2::Entity& root : f.Entities()) {
                                if (root.class_name == "ShRelativeStageLocator") {
                                    axes = glm::mat3(glm::inverse(f.WorldTransform(root)) * local);
                                    break;
                                }
                            }
                            mirror.light_area = glm::translate(glm::mat4(1.0f), glm::vec3(placement[3])) * glm::mat4(axes) *
                                                glm::scale(glm::mat4(1.0f), glm::vec3(size));
                            mirror.has_light_area = true;
                        }
                        out.mirrors.push_back(mirror);
                    }
                }
            }
        },
        false);
}

namespace {

bool RayEntersBox(const glm::mat4& box, const glm::vec3& o, const glm::vec3& d, glm::vec3& at) {
    const glm::mat4 inverse = glm::inverse(box);
    const glm::vec3 lo(inverse * glm::vec4(o, 1.0f));
    const glm::vec3 ld(inverse * glm::vec4(d, 0.0f));
    float enter = 0.0f;
    float leave = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        if (std::abs(ld[i]) < 1.0e-8f) {
            if (lo[i] < -0.5f || lo[i] > 0.5f) {
                return false;
            }
            continue;
        }
        float a = (-0.5f - lo[i]) / ld[i];
        float b = (0.5f - lo[i]) / ld[i];
        if (a > b) {
            std::swap(a, b);
        }
        enter = std::max(enter, a);
        leave = std::min(leave, b);
        if (enter > leave) {
            return false;
        }
    }
    at = o + d * enter;
    return true;
}

bool MirrorApertureProjection(const SceneMirror& mirror, const glm::vec3& source, const glm::vec3& room_normal, glm::mat4& projection) {
    if (!mirror.mesh) {
        return false;
    }
    const glm::vec3 extent = mirror.mesh->bounds_max - mirror.mesh->bounds_min;
    int plane_axis = 0;
    if (extent.y < extent[plane_axis]) plane_axis = 1;
    if (extent.z < extent[plane_axis]) plane_axis = 2;
    const int u_axis = (plane_axis + 1) % 3;
    const int v_axis = (plane_axis + 2) % 3;
    if (extent[plane_axis] < 0.0f || extent[u_axis] <= 1.0e-5f || extent[v_axis] <= 1.0e-5f) {
        return false;
    }

    const glm::vec3 local_center = 0.5f * (mirror.mesh->bounds_min + mirror.mesh->bounds_max);
    const glm::vec3 center = glm::vec3(mirror.transform * glm::vec4(local_center, 1.0f));
    glm::vec3 n = glm::normalize(glm::vec3(mirror.transform[plane_axis]));
    if (glm::dot(n, room_normal) < 0.0f) n = -n;
    const glm::vec3 tu = glm::normalize(glm::vec3(mirror.transform[u_axis]));
    const glm::vec3 tv = glm::normalize(glm::vec3(mirror.transform[v_axis]));
    const float width = extent[u_axis] * glm::length(glm::vec3(mirror.transform[u_axis]));
    const float height = extent[v_axis] * glm::length(glm::vec3(mirror.transform[v_axis]));
    const float d = glm::dot(n, center - source);
    if (d <= 1.0e-5f || width <= 1.0e-5f || height <= 1.0e-5f) {
        return false;
    }

    const float su = glm::dot(tu, source - center);
    const float sv = glm::dot(tv, source - center);
    const float sn = glm::dot(n, source);
    const float u_constant = -d * glm::dot(tu, source) - su * sn;
    const float v_constant = -d * glm::dot(tv, source) - sv * sn;
    const glm::vec4 row_u((d * tu + su * n) / width, u_constant / width);
    const glm::vec4 row_v((d * tv + sv * n) / height, v_constant / height);
    const glm::vec4 row_z(-0.5f * n, d + 0.5f * sn);
    const glm::vec4 row_w(n, -sn);
    projection = glm::mat4(0.0f);
    for (int column = 0; column < 4; ++column) {
        projection[column][0] = row_u[column];
        projection[column][1] = row_v[column];
        projection[column][2] = row_z[column];
        projection[column][3] = row_w[column];
    }
    return true;
}

}

void RenderSceneBuilder::AddMirrorLight(Game& game, const Camera& camera, SceneLighting& out) const {
    const SceneMirror* mirror = nullptr;
    float nearest = 0.0f;
    for (const SceneMirror& m : out.mirrors) {
        const float distance = glm::length(glm::vec3(m.transform[3]) - camera.position);
        if (!mirror || distance < nearest) {
            mirror = &m;
            nearest = distance;
        }
    }
    const auto handy = std::find_if(out.lights.begin(), out.lights.end(), [](const SceneLight& l) { return l.id == kHandyLightId; });
    static const bool trace = std::getenv("PT_MIRROR_TRACE") != nullptr;
    auto trace_line = [&](const char* why) {
        if (trace) {
            LogInfo("mirror trace: off ({}) handy {} mirror {} area {}", why, handy != out.lights.end(), mirror != nullptr,
                    mirror ? mirror->has_light_area : false);
        }
    };
    if (!mirror || !mirror->has_light_area || handy == out.lights.end()) {
        trace_line("missing");
        return;
    }
    const glm::vec3 n = glm::normalize(-glm::vec3(mirror->transform[2]));
    const glm::vec3 m = glm::vec3(mirror->transform[3]);
    const glm::vec3 p = handy->position;
    const glm::vec3 a = glm::normalize(handy->direction);
    const float na = glm::dot(n, a);
    if (na >= 0.0f) {
        trace_line("axis away from the plane");
        return;
    }
    const glm::vec3 hit = p + a * (glm::dot(n, m - p) / na);
    const glm::vec3 mirrored = p - 2.0f * std::abs(glm::dot(n, p - m)) * n;
    glm::vec3 at;
    const glm::vec3 to_hit = hit - mirrored;
    if (glm::length(to_hit) < 1.0e-6f) {
        trace_line("no beam");
        return;
    }
    const bool from_mirrored = RayEntersBox(mirror->light_area, mirrored, glm::normalize(to_hit), at);
    if (!from_mirrored && !RayEntersBox(mirror->light_area, hit, glm::normalize(-to_hit), at)) {
        if (trace) {
            const glm::vec3 lo(glm::inverse(mirror->light_area) * glm::vec4(mirrored, 1.0f));
            const glm::vec3 lh(glm::inverse(mirror->light_area) * glm::vec4(hit, 1.0f));
            LogInfo("mirror trace: off (beam misses the area) p ({:.3f} {:.3f} {:.3f}) a ({:.3f} {:.3f} {:.3f}) mirrored ({:.3f} {:.3f} {:.3f}) hit ({:.3f} "
                    "{:.3f} {:.3f}) local mirrored ({:.3f} {:.3f} {:.3f}) local hit ({:.3f} {:.3f} {:.3f})",
                    p.x, p.y, p.z, a.x, a.y, a.z, mirrored.x, mirrored.y, mirrored.z, hit.x, hit.y, hit.z, lo.x, lo.y, lo.z, lh.x, lh.y, lh.z);
        }
        return;
    }
    const HandyLightParameters& params = game.Parameters().handy_light;
    SceneLight l = *handy;
    l.id = kMirrorLightId;
    l.name = "MirrorLight";
    l.position = at;
    l.direction = glm::normalize(a - 2.0f * na * n);
    l.up = glm::normalize(handy->up - 2.0f * glm::dot(handy->up, n) * n);
    l.intensity *= 4.0f;
    l.source_radius *= 0.5f;
    l.outer_range += 2.0f;
    l.cos_outer = CosHalf(params.umbra_angle * 0.6f);
    l.inv_cone_range = InverseRange(CosHalf(params.penumbra_angle), l.cos_outer);
    l.mask_fov = params.umbra_angle * 0.6f * kPi / 180.0f;
    static const bool handy_cone = [] {
        const char* value = std::getenv("PT_MIRROR_SHADOW_CONE");
        return value && std::string_view(value) == "handy";
    }();
    if (!handy_cone) {
        const float full = handy->shadow_cos_outer + 1.0f / std::max(handy->shadow_inv_cone_range, 1.0e-6f);
        l.shadow_cos_outer = CosHalf(params.umbra_angle * 0.6f);
        l.shadow_inv_cone_range = 1.0f / std::max(full - l.shadow_cos_outer, 1.0e-6f);
    }
    static const bool light_shadow = [] {
        const char* value = std::getenv("PT_MIRROR_LIGHT_SHADOW");
        return !value || std::atoi(value) != 0;
    }();
    l.cast_shadow = light_shadow;
    l.priority = 0x40;
    static const bool camera_hidden = [] {
        const char* value = std::getenv("PT_MIRROR_LIGHT_CAMERA");
        return value && std::atoi(value) == 0;
    }();
    l.hidden_views = camera_hidden ? 1u : 0u;
    static const bool clip_front = [] {
        const char* value = std::getenv("PT_MIRROR_LIGHT_CLIP");
        return !value || std::atoi(value) != 0;
    }();
    if (clip_front) {
        constexpr float kHalf = 15.0f;
        const glm::vec3 up = std::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 t1 = glm::normalize(glm::cross(n, up));
        const glm::vec3 t2 = glm::cross(n, t1);
        l.has_area = true;
        l.area_world = glm::mat4(glm::vec4(n * kHalf, 0.0f), glm::vec4(t1 * kHalf, 0.0f), glm::vec4(t2 * kHalf, 0.0f), glm::vec4(m + n * kHalf, 1.0f));
        l.area_to_box = glm::inverse(l.area_world);
        if (MirrorApertureProjection(*mirror, at, n, l.area_to_box)) {
            l.area_clip_mode = AreaClipMode::ProjectiveAperture;
        }
    }
    if (trace) {
        LogInfo("mirror trace: on at ({:.3f} {:.3f} {:.3f}) dir ({:.3f} {:.3f} {:.3f}) hit ({:.3f} {:.3f} {:.3f}) from_mirrored {} beam {:.3f} p ({:.3f} "
                "{:.3f} {:.3f}) camera view {}",
                at.x, at.y, at.z, l.direction.x, l.direction.y, l.direction.z, hit.x, hit.y, hit.z, from_mirrored, glm::length(hit - mirrored), p.x, p.y, p.z,
                l.hidden_views == 0);
        LogInfo("mirror trace: light area centre ({:.3f} {:.3f} {:.3f}) half axes x ({:.3f} {:.3f} {:.3f}) y ({:.3f} {:.3f} {:.3f}) z ({:.3f} {:.3f} {:.3f})",
                mirror->light_area[3].x, mirror->light_area[3].y, mirror->light_area[3].z, 0.5f * mirror->light_area[0].x, 0.5f * mirror->light_area[0].y,
                0.5f * mirror->light_area[0].z, 0.5f * mirror->light_area[1].x, 0.5f * mirror->light_area[1].y, 0.5f * mirror->light_area[1].z,
                0.5f * mirror->light_area[2].x, 0.5f * mirror->light_area[2].y, 0.5f * mirror->light_area[2].z);
        LogInfo("mirror trace: plane point ({:.3f} {:.3f} {:.3f}) normal ({:.3f} {:.3f} {:.3f}); handy dist {:.3f} dir ({:.3f} {:.3f} {:.3f}); P' ({:.3f} {:.3f} "
                "{:.3f}) r ({:.3f} {:.3f} {:.3f}); light dist {:.3f} (negative: behind the plane), r.n {:.3f}",
                m.x, m.y, m.z, n.x, n.y, n.z, glm::dot(p - m, n), a.x, a.y, a.z, mirrored.x, mirrored.y, mirrored.z, l.direction.x, l.direction.y, l.direction.z,
                glm::dot(at - m, n), glm::dot(l.direction, n));
    }
    static const bool mirror_light_off = [] {
        const char* off = std::getenv("PT_MIRROR_LIGHT_OFF");
        return off && *off == '1';
    }();
    if (mirror_light_off) {
        return;
    }
    out.lights.push_back(l);
}

float RenderSceneBuilder::FocusDistance(Game& game, const Camera& camera, float dt) {
    constexpr float kRayStart = 0.1f;
    RayHit hit;
    float target = 1500.0f;
    const bool hit_any = game.LineCollision().Raycast(camera.position + camera.Forward() * kRayStart, camera.Forward(), 1500.0f, hit);
    if (hit_any) {
        target = hit.distance + kRayStart;
    }
    target = std::clamp(target, 0.5f, 1500.0f);
    const bool cut = !focus_valid_ || glm::distance(camera.position, focus_eye_) > 3.0f || glm::dot(camera.Forward(), focus_forward_) < 0.7071f;
    focus_eye_ = camera.position;
    focus_forward_ = camera.Forward();
    focus_valid_ = true;
    const float k = cut ? 1.0f : 1.0f - std::exp(-dt * 8.0f);
    focus_ += (target - focus_) * k;
    static const bool trace = std::getenv("PT_TRACE_FOCUS") != nullptr;
    if (trace && (cut || std::abs(target - focus_) > 1.0f)) {
        const CollisionWorld& lines = game.LineCollision();
        LogInfo("focus: eye ({:.3f} {:.3f} {:.3f}) target {:.3f} focus {:.3f} on {} tags {:#x} flags {:#x}", camera.position.x,
                camera.position.y, camera.position.z, target, focus_,
                hit_any ? lines.OwnerName(lines.Triangles()[hit.triangle].owner) : std::string("nothing"),
                hit_any ? lines.Triangles()[hit.triangle].tags : 0, hit_any ? lines.Triangles()[hit.triangle].shape_flags : 0u);
    }
    return focus_;
}

void RenderSceneBuilder::Build(Game& game, const Camera& camera, float dt, SceneLighting& out, const TickBlend* blend) {
    static const bool profile = std::getenv("PT_BUILD_PROFILE") != nullptr;
    using clock = std::chrono::steady_clock;
    clock::time_point marks[8];
    int mark = 0;
    auto stamp = [&] { if (profile && mark < 8) marks[mark++] = clock::now(); };
    stamp();
    out = SceneLighting{};
    out.valid = true;
    LoadResidentSettings(game);
    stamp();
    Vfs& vfs = game.GetVfs();
    game.Stages().ForEachStage(
        [&](Stage& stage) {
            if (!stage.active) {
                return;
            }
            for (const auto& data : stage.files) {
                for (const LightPlacement& light : data->lights) {
                    if (!light.entity || light.mod_off || !EntityEnabled(&stage, *data->file, *light.entity)) {
                        continue;
                    }
                    const uint64_t id = (uint64_t(stage.id) << 40) ^ reinterpret_cast<uintptr_t>(light.entity);
                    const size_t before = out.lights.size();
                    AddLight(*data->file, *light.entity, stage.file_to_world, id, out);
                    if (out.lights.size() > before && (light.mod_tint != glm::vec3(1.0f) || light.mod_brightness != 1.0f)) {
                        out.lights.back().intensity *= light.mod_tint * light.mod_brightness;
                    }
                }
            }
        },
        false);
    stamp();
    {
        std::vector<std::pair<uint32_t, const void*>> loaded;
        game.Stages().ForEachStage([&](Stage& stage) {
            for (const auto& data : stage.files) loaded.emplace_back(static_cast<uint32_t>(stage.id), static_cast<const void*>(data->file.get()));
        }, false);
        if (loaded != loaded_stage_files_) {
            loaded_stage_files_ = std::move(loaded);
            static_entities_.clear();
            probe_cache_.clear();
            occluder_cache_.clear();
        }
    }
    game.Stages().ForEachStage(
        [&](Stage& stage) {
            if (!stage.active) {
                return;
            }
            for (const auto& data : stage.files) {
                const fox2::DataSetFile& f = *data->file;
                auto [scan, fresh] = static_entities_.try_emplace(&f);
                if (fresh) {
                    for (const fox2::Entity& e : f.Entities()) {
                        if (e.class_name == "ShLightProbe") {
                            scan->second.probes.push_back(&e);
                        } else if (e.class_name == "OccluderEx") {
                            scan->second.occluders.push_back(&e);
                        }
                    }
                }
                auto cached = [&](const fox2::Entity* e, auto& cache, auto build) -> const auto* {
                    const uint64_t key = (uint64_t(stage.id) << 40) ^ reinterpret_cast<uintptr_t>(e);
                    auto it = cache.find(key);
                    if (it == cache.end() || it->second.first != stage.file_to_world) {
                        it = cache.insert_or_assign(key, std::make_pair(stage.file_to_world, build())).first;
                    }
                    return &it->second.second;
                };
                for (const fox2::Entity* e : scan->second.probes) {
                    if (!EntityEnabled(&stage, f, *e)) continue;
                    if (const auto* p = cached(e, probe_cache_, [&] { return BuildProbe(vfs, f, *e, stage.file_to_world); }); *p) {
                        out.probes.push_back(**p);
                    }
                }
                for (const fox2::Entity* e : scan->second.occluders) {
                    if (!EntityEnabled(&stage, f, *e)) continue;
                    if (const auto* o = cached(e, occluder_cache_, [&] { return BuildOccluder(f, *e, stage.file_to_world); }); *o) {
                        out.occluders.push_back(**o);
                    }
                }
            }
        },
        false);
    stamp();
    const float focus = FocusDistance(game, camera, dt);
    stamp();
    const bool blended = blend && blend->t < 1.0f;
    const bool player_view = game.DetachedView() || game.ThirdPersonCameraActive();
    Camera handy_view = player_view ? game.GetPlayer().MakeCamera() : camera;
    if (player_view) {
        handy_view.position += game.GetPlayer().DrawnOffset();
        if (blended) {
            const Camera& previous = blend->handy_camera;
            const float yaw_delta = std::remainder(handy_view.yaw - previous.yaw, 2.0f * kPi);
            if (glm::distance(previous.position, handy_view.position) <= 1.0f && std::abs(yaw_delta) <= 0.5f &&
                std::abs(handy_view.pitch - previous.pitch) <= 0.5f) {
                handy_view.position = glm::mix(previous.position, handy_view.position, blend->t);
                handy_view.yaw = previous.yaw + yaw_delta * blend->t;
                handy_view.pitch = glm::mix(previous.pitch, handy_view.pitch, blend->t);
                handy_view.roll = previous.roll + std::remainder(handy_view.roll - previous.roll, 2.0f * kPi) * blend->t;
            }
        }
    }
    AddHandyLight(game, handy_view, blended ? glm::mix(blend->handy_aim, game.HandyAim(), blend->t) : game.HandyAim(), dt, out,
                  blended ? blend : nullptr);
    AddLisaGlint(game, out);
    AddIntercomLight(game, out);
    if (const SurvivalNight* survival = game.Survival()) {
        survival->AddLights(out);
    }
    if (const StreetHouse* house = game.House()) {
        house->AddLights(out, game.SurvivalActive() ? game.Survival()->StreetLight() : 1.0f);
    }
    for (const DemoLight& light : game.Demos().Lights()) {
        AddDemoLight(light, blended ? blend : nullptr, out);
    }

    const ScreenEffects& fx = game.Effects();
    static const bool mirror_capture_off = [] {
        const char* off = std::getenv("PT_MIRROR_CAPTURE");
        return off && *off == '0';
    }();
    out.mirror_capture = fx.mirror_capture && game.MirrorViewportBits() != 0 && !mirror_capture_off;
    out.mirror_high = (game.MirrorViewportBits() & 2u) != 0;
    if (out.mirror_capture) {
        AddMirrors(game, out);
        AddMirrorLight(game, camera, out);
    }

    ExposureSettings exposure = resident_.defaults;
    if (const LightingRow* row = game.Parameters().Lighting(game.FloorLightingRow())) {
        exposure.min_ev = row->min_exposure;
        exposure.max_ev = row->max_exposure;
        exposure.compensation = row->exposure_compensation;
        exposure.key = row->key_value;
        for (int i = 0; i < 3; ++i) {
            exposure.add_comp[i] = row->add_exp_comp[i];
            exposure.add_comp_ev[i] = row->add_exp_comp_ev[i];
        }
        exposure.bloom_weight = row->bloom_weight;
        exposure.bloom_extraction = row->bloom_brightness_extraction;
        exposure.bloom_size = row->bloom_size;
    }
    exposure.pinned = fx.ev_pinned;
    exposure.pinned_ev = fx.pinned_ev;
    out.exposure = exposure;

    ScreenSettings& screen = out.screen;
    auto lut = resident_.luts.find(fx.lut);
    const std::string lut_path = lut != resident_.luts.end() ? lut->second : std::string();
    if (lut_path != lut_path_) {
        previous_lut_path_ = lut_path_;
        lut_path_ = lut_path;
        lut_blend_ = previous_lut_path_.empty() ? 1.0f : 0.0f;
    }
    lut_blend_ = std::min(1.0f, lut_blend_ + dt);
    screen.lut_path = lut_path_;
    screen.previous_lut_path = previous_lut_path_;
    screen.lut_blend = lut_blend_;
    screen.color_scale = resident_.color_scale;
    screen.start_slope = resident_.start_slope;
    screen.end_slope = resident_.end_slope;
    screen.local_reflections = (resident_.plugin_flags & 0x20u) != 0;
    static const bool no_local_reflections = std::getenv("PT_NO_LOCAL_REFLECTIONS") != nullptr;
    if (no_local_reflections) {
        screen.local_reflections = false;
    }
    out.reflection_texture = resident_.reflection_texture;
    const bool ending = game.Floor().IsCurrentFloorName("ending");
    screen.film_grain = fx.film_grain;
    screen.grain_alt = ending;
    screen.grain_strength = fx.film_grain_strength;
    screen.grain_offset = fx.grain_offset;
    screen.screen_distortion = fx.screen_distortion;
    screen.wide_shadow_limit = fx.maze_viewport;
    screen.reflect_scale = fx.reflect_scale;
    screen.reflect_bias = fx.reflect_bias;
    screen.reflect_edge = fx.reflect_edge;
    screen.subsurface_scatter = fx.subsurface_scatter;
    screen.full_screen_blur = fx.full_screen_blur;
    screen.mesh_occlusion = game.Nazo().IsPeepholeTheaterActive();
    screen.colour_banding_canceller = fx.colour_banding_canceller;
    screen.blur_blend_rate = fx.blur_blend_rate;
    screen.blur_fetch_band = fx.blur_fetch_band;
    screen.zoom = game.GetPlayer().zoom;
    screen.depth_of_field = (resident_.plugin_flags & 0x8u) != 0;
    screen.motion_blur = (resident_.plugin_flags & 0x4u) != 0;
    screen.smooth_camera_motion_blur = game.StreetWalkActive();
    screen.fixed_shutter = (resident_.plugin_flags & 0x80u) != 0;
    screen.focus_distance = focus;
    screen.focal_length = game.Parameters().player.focal_length;
    screen.aperture = kCameraAperture;
    if (game.SurvivalActive() && !game.ThirdPersonCameraActive()) screen.dof_flags |= 1u;
    if (game.SurvivalActive()) {
        const SurvivalHud& hud = game.Survival()->Hud();
        const float since = std::max(0.0f, hud.run_seconds - hud.last_beat);
        const float beat = hud.heart_rate > 0.0f ? std::exp(-since * 7.0f) : 0.0f;
        const float low = hud.low_health * (0.75f + 0.25f * beat);
        const float hurt = hud.hurt * 0.5f;
        const float k = std::clamp(low + hurt, 0.0f, 1.0f);
        screen.color_scale *= glm::mix(glm::vec3(1.0f), glm::vec3(1.0f, 0.62f, 0.58f), k);
        screen.grain_strength *= 1.0f + 2.2f * low;
        screen.film_grain = screen.film_grain || low > 0.05f;
    }
    screen.shutter_speed = kCameraShutter;
    if (const LightingRow* row = game.Parameters().Lighting(game.FloorLightingRow())) {
        screen.shutter_speed = row->shutter_speed;
    }
    game.Demos().SetGameLens(screen.focus_distance, screen.aperture, screen.shutter_speed);
    ApplyDemoCamera(game, out);
    static const bool trace_lens = std::getenv("PT_TRACE_LENS") != nullptr;
    if (log_lens_next || (trace_lens && game.Frame() % 30 == 0)) {
        log_lens_next = false;
        LogInfo("lens: frame {} floor {} auto {:.3f} applied {:.3f} focal {:.3f} aperture {:.3f} blur {} blend {:.3f} band {:.2f} "
                "dof {} motion blur {} shutter {:.4f} zoom {:.2f} distortion {} demo camera {}",
                game.Frame(), game.Floor().CurrentFloorName(), focus, screen.focus_distance, screen.focal_length,
                screen.aperture, screen.full_screen_blur, screen.blur_blend_rate, screen.blur_fetch_band, screen.depth_of_field,
                screen.motion_blur, screen.shutter_speed, screen.zoom, screen.screen_distortion, game.Demos().CameraParams() != nullptr);
    }
    stamp();
    AddTppAtmosphere(game, out);
    if (const SurvivalNight* survival = game.Survival()) {
        survival->ApplyStreetEvents(out);
    }
    stamp();
    if (profile && mark == 7) {
        static double sums[6] = {};
        static int frames = 0;
        for (int i = 0; i < 6; ++i) sums[i] += std::chrono::duration<double, std::milli>(marks[i + 1] - marks[i]).count();
        if (++frames == 120) {
            LogInfo("build profile: resident {:.3f} lights {:.3f} probes {:.3f} focus {:.3f} rest {:.3f} atmosphere {:.3f} ms ({} lights, {} probes)",
                    sums[0] / frames, sums[1] / frames, sums[2] / frames, sums[3] / frames, sums[4] / frames, sums[5] / frames,
                    out.lights.size(), out.probes.size());
            frames = 0;
            for (double& v : sums) v = 0.0;
        }
    }
}

void RenderSceneBuilder::AddTppAtmosphere(Game& game, SceneLighting& out) const {
    TppAtmosphereSettings& tpp = out.tpp;
    glm::vec3 self_color(0.0f);
    float self_alpha = 1.0f;
    float self_luminance = 0.0f;
    const DemoScreenState* screen = game.Demos().ScreenState();
    auto demo_value = [&](uint64_t functor, uint64_t name, size_t index, float& target) {
        if (!screen) {
            return;
        }
        const std::vector<float>* values = nullptr;
        if (name) {
            auto it = screen->named_values.find({functor, name});
            values = it != screen->named_values.end() ? &it->second : nullptr;
        } else {
            auto it = screen->values.find(functor);
            values = it != screen->values.end() ? &it->second : nullptr;
        }
        if (values && index < values->size()) {
            target = (*values)[index];
        }
    };
    game.Stages().ForEachStage(
        [&](Stage& stage) {
            for (const auto& data : stage.files) {
                const fox2::DataSetFile& f = *data->file;
                for (const fox2::Entity& e : f.Entities()) {
                    if (e.class_name == "TppGlobalVolumetricFogParam") {
                        tpp.enabled = true;
                        const glm::vec4 self = f.GetVec4(e, "selfColor");
                        self_color = glm::vec3(self);
                        self_alpha = self.a;
                        self_luminance = f.GetFloat(e, "selfLuminance");
                        tpp.fog_density = f.GetFloat(e, "density");
                        tpp.fog_falloff = f.GetFloat(e, "falloff");
                        tpp.fog_near = f.GetFloat(e, "near");
                        tpp.fog_far = f.GetFloat(e, "far", 0, 70.0f);
                        const glm::vec4 mie = f.GetVec4(e, "mieScattering");
                        tpp.fog_mie = glm::vec3(mie) * mie.a;
                        const float g = f.GetFloat(e, "mieAnisotropy");
                        tpp.fog_mie_anisotropy = 1.55f * g - 0.55f * g * g * g;
                        const glm::vec4 rayleigh = f.GetVec4(e, "rayleighScattering");
                        tpp.fog_rayleigh = glm::vec3(rayleigh) * rayleigh.a;
                        tpp.dir_gain = f.GetFloat(e, "dirLightGain");
                        for (int i = 0; i < 3; ++i) {
                            tpp.exposure_offset_values[i] = f.GetFloat(e, "exposureOffsetValues", static_cast<size_t>(i));
                            tpp.exposure_offset_targets[i] = f.GetFloat(e, "exposureOffsetTargets", static_cast<size_t>(i));
                        }
                    } else if (e.class_name == "TppAreaVolumetricFog" && !tpp.area) {
                        const fox2::Entity* param = f.GetEntity(e, "param");
                        if (!param) {
                            continue;
                        }
                        const uint64_t name = StrCode64(f.EntityName(e)) & kStrCode64Mask;
                        float enable = f.GetBool(*param, "enable") ? 1.0f : 0.0f;
                        glm::vec3 color = glm::vec3(f.GetVec4(*param, "color"));
                        float density = f.GetFloat(*param, "density");
                        demo_value(0x1C873769CA1FULL, name, 0, enable);
                        demo_value(0xD8C7DBEE04F7ULL, name, 0, density);
                        demo_value(0x66F326FA2CE2ULL, name, 0, color.r);
                        demo_value(0x66F326FA2CE2ULL, name, 1, color.g);
                        demo_value(0x66F326FA2CE2ULL, name, 2, color.b);
                        if (enable == 0.0f) {
                            continue;
                        }
                        const glm::mat4 box = stage.file_to_world * f.WorldTransform(e) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
                        glm::vec3 lo(1.0e9f);
                        glm::vec3 hi(-1.0e9f);
                        for (int corner = 0; corner < 8; ++corner) {
                            const glm::vec4 c((corner & 1) ? 1.0f : -1.0f, (corner & 2) ? 1.0f : -1.0f, (corner & 4) ? 1.0f : -1.0f, 1.0f);
                            const glm::vec3 w = glm::vec3(box * c);
                            lo = glm::min(lo, w);
                            hi = glm::max(hi, w);
                        }
                        tpp.area = true;
                        tpp.area_min = lo;
                        tpp.area_max = hi;
                        tpp.area_color = color * f.GetFloat(*param, "luminance");
                        tpp.area_density = density;
                        tpp.area_near = f.GetFloat(*param, "nearDistance");
                        tpp.area_falloff = f.GetFloat(*param, "falloff");
                        tpp.area_inverse = f.GetBool(*param, "inverseFalloff");
                    }
                }
            }
        },
        false);
    if (tpp.enabled) {
        demo_value(0x899F4232248DULL, 0, 0, tpp.fog_density);
        demo_value(0x5E28E698728CULL, 0, 0, tpp.fog_near);
        demo_value(0xBB4946A517ECULL, 0, 0, self_luminance);
        demo_value(0x232188F03793ULL, 0, 0, tpp.fog_far);
        demo_value(0x10363B1D0048ULL, 0, 0, self_color.r);
        demo_value(0x10363B1D0048ULL, 0, 1, self_color.g);
        demo_value(0x10363B1D0048ULL, 0, 2, self_color.b);
    }
    tpp.fog_self = self_color * self_alpha * self_luminance;
    if (tpp.enabled) {
        game.Stages().ForEachStage(
            [&](Stage& stage) {
                for (const auto& data : stage.files) {
                    const fox2::DataSetFile& f = *data->file;
                    for (const fox2::Entity& e : f.Entities()) {
                        if (e.class_name == "TppAtmosphere") {
                            tpp.dir_color = glm::vec3(f.GetVec4(e, "moonColor")) * f.GetFloat(e, "moonLux") / kPi;
                            tpp.light_dir = glm::vec3(0.39491f, 0.85361f, 0.33969f);
                        }
                        if (e.class_name == "TppSky" && f.GetBool(e, "enable", 0, true)) {
                            tpp.sky = true;
                        }
                    }
                }
            },
            false);
    }
    tpp.tonemap = resident_.tpp_tonemap;
    tpp.threshold = resident_.tpp_threshold;
    tpp.range = resident_.tpp_range;
}

void RenderSceneBuilder::AddDemoLight(const DemoLight& d, const TickBlend* blend, SceneLighting& out) const {
    if (!d.enabled || d.lumen <= 0.0f) {
        return;
    }
    glm::mat4 world = d.world;
    if (blend) {
        for (const DemoLight& previous : blend->demo_lights) {
            if (previous.name == d.name && previous.demo_id == d.demo_id) {
                world = BlendTransform(previous.world, d.world, blend->t);
                break;
            }
        }
    }
    SceneLight l;
    l.id = StrCode64(d.demo_id + "|" + d.name);
    l.name = d.demo_id + "|" + d.name;
    l.type = d.point ? LightType::Point : LightType::Spot;
    l.position = glm::vec3(world[3]);
    SpotAxes(glm::mat3(world), glm::vec3(0.0f, -1.0f, 0.0f), l.direction, l.up);
    const glm::vec3 rgb = light_math::ColorFromTemperature(d.temperature, d.deflection, d.lumen, glm::vec3(d.color));
    l.inner_range = d.inner_range;
    l.outer_range = std::max(d.outer_range, 0.01f);
    l.cast_shadow = d.shadow;
    l.specular_scale = d.specular ? 1.0f : 0.0f;
    l.shadow_bias = d.bias * 0.001f;
    l.shadow_strength = d.shadow_strength;
    l.priority = 0x80;
    l.source_radius = light_math::SourceRadius(d.light_size);
    if (d.point) {
        l.intensity = rgb / (4.0f * kPi) / kPi;
    } else {
        const float omega = light_math::SpotSolidAngle(d.umbra, d.penumbra, d.attenuation_exponent);
        l.intensity = rgb / omega * d.power_scale / kPi;
        l.cos_outer = CosHalf(d.umbra);
        l.inv_cone_range = InverseRange(CosHalf(d.penumbra), l.cos_outer);
        l.cone_exponent = d.attenuation_exponent;
        l.shadow_cos_outer = CosHalf(d.shadow_umbra);
        l.shadow_inv_cone_range = InverseRange(CosHalf(d.shadow_penumbra), l.shadow_cos_outer);
        l.shadow_fov = d.shadow_umbra * kPi / 180.0f;
        l.view_bias = d.view_bias * 0.001f;
    }
    out.lights.push_back(l);
}

void RenderSceneBuilder::ApplyDemoCamera(Game& game, SceneLighting& out) const {
    const DemoSystem& demos = game.Demos();
    const DemoCameraParams* camera_params =
        (game.SurvivalActive() && demos.SceneryCameraParams()) ? demos.SceneryCameraParams()
        : demos.CameraParams()                                 ? demos.CameraParams()
                                                               : demos.SceneryCameraParams();
    if (const DemoCameraParams* cam = camera_params) {
        const uint32_t set = cam->set_mask;
        ExposureSettings& e = out.exposure;
        auto apply = [&](uint32_t bit, float value, float& target) {
            if (set & (1u << bit)) {
                target = value;
            }
        };
        apply(4, cam->exposure_compensation, e.compensation);
        apply(5, cam->min_exposure, e.min_ev);
        apply(6, cam->max_exposure, e.max_ev);
        apply(7, cam->bloom_size, e.bloom_size);
        apply(10, cam->key_value, e.key);
        apply(11, cam->bloom_weight, e.bloom_weight);
        apply(12, cam->bloom_extraction, e.bloom_extraction);
        if (set & (1u << 13)) {
            static const int kEv[3] = {4, 5, 3};
            for (int i = 0; i < 3; ++i) {
                e.add_comp[i] = cam->add_exposure[i];
                e.add_comp_ev[i] = cam->add_exposure[kEv[i]];
            }
        }
        apply(3, cam->shutter_speed, out.screen.shutter_speed);
    }
    const bool skip_dof = game.Survival() && game.Survival()->WakingThirdPerson();
    if (const DemoCameraParams* lens = skip_dof ? nullptr : demos.DofLens()) {
        ScreenSettings& s = out.screen;
        s.focal_length = lens->focal_length;
        if (lens->set_mask & 1u) {
            s.focus_distance = lens->focus_distance;
        }
        if (lens->set_mask & 2u) {
            s.aperture = lens->aperture;
        }
    }
    if (const DemoScreenState* screen = demos.ScreenState(); screen && screen->tone_set) {
        out.screen.color_scale = glm::vec3(screen->color_scale);
        out.screen.start_slope = screen->start_slope;
        out.screen.end_slope = screen->end_slope;
    }
}

void RenderSceneBuilder::BuildFromStage(const StageData& stage, Vfs& vfs, SceneLighting& out) {
    out = SceneLighting{};
    out.valid = true;
    const fox2::DataSetFile& f = *stage.file;
    uint64_t id = 1;
    for (const LightPlacement& light : stage.lights) {
        if (light.entity && light.enable) {
            AddLight(f, *light.entity, glm::mat4(1.0f), id++, out);
        }
    }
    for (const fox2::Entity& e : f.Entities()) {
        if (e.class_name == "ShLightProbe" && EntityEnabled(nullptr, f, e)) {
            if (auto p = BuildProbe(vfs, f, e, glm::mat4(1.0f))) {
                out.probes.push_back(*p);
            }
        }
    }
    out.screen.film_grain = false;
    out.screen.screen_distortion = false;
    out.screen.depth_of_field = false;
}

}
