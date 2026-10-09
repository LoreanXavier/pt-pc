#include "game/render_scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
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

namespace pt::game {
namespace {

constexpr float kCameraAperture = 1.0f;
constexpr float kCameraShutter = 0.008333333f;

constexpr float kPi = 3.14159265358979f;

struct HandyPose {
    float view_pitch;
    glm::vec3 pivot;
};

// The light is the flash point of the flashlight held in the left hand of the arm pose FC517D57B3A9F1EE. The hand turns as a
// rigid body about a pivot (the wrist) to aim the light at its look target (Game::UpdateHandyTarget): the flash point stands
// kHandOffset from the pivot in the frame of the aim d (right of d about the camera's up, up of d, d). The Spot_Mask constants
// (m_lightParams[3] position and [5] direction in view space) give that offset and the pivots: over flashlight_dynamics
// 1550-2900 (holds at view pitch +39.45, 0, -10.98, -20.09, -39.75 and -55 with targets from 0.6 to 11.5 m), boot_dof
// 2900-2978, mirror_f060 4935-5120 and 5325-5337, f050a_walls_2955 and floor_f060 3585-3595 the offset (0.0377 right, 0.0603
// up, 0.1264 along the aim) makes the pivot one point per view pitch within 2.4 mm rms, and the light lands within 6.4 mm of
// every hold (the flashlight's own axis offset, 0.1044 m along the aim, left the light 2.2 cm too far forward facing a wall
// at 0.607 m). Pivots in the port's camera frame (x right; the original's view space x points to the left of the screen:
// m_view[0] = up x forward, m_projectionParam.x = -1/f): the flashlight_dynamics holds, -2.57 from mirror_f060 4935-5120, -55
// averaged with the sink (floor_f060 3585-3595) and +50.5 from lisa_balcony_f070 3875-3885 draw 593 (the light 0.208 m left,
// 0.079 down, 0.165 forward, aimed 2.26 right)
constexpr glm::vec3 kHandOffset{0.0377f, 0.0603f, 0.1264f};
constexpr HandyPose kHandyPoses[] = {{-55.0f, {-0.2402f, 0.0431f, 0.2678f}}, {-39.75f, {-0.2466f, -0.0074f, 0.2563f}},
                                    {-20.09f, {-0.2482f, -0.0671f, 0.2221f}}, {-10.98f, {-0.2488f, -0.0905f, 0.2022f}},
                                    {-2.57f, {-0.2503f, -0.1142f, 0.1773f}},  {0.0f, {-0.2495f, -0.1130f, 0.1740f}},
                                    {39.45f, {-0.2554f, -0.1395f, 0.0666f}},
                                    {50.5f, {-0.2507f, -0.1393f, 0.0402f}}};
// the pivots after the flashlight pickup (Game::HandyPickupHold): floor_f060 3075-3085 at the view pitch -22.17 (the light
// 0.191 m left, 0.057 down and 0.155 forward aimed 9.15 right and 1.99 up, 0.19 m further back and 6 cm lower than in play
// there), and the still frames of tub_f060_light 3400-3510 (the medians of 26, 14 and 29 frames within 2 mm, from the
// Spot_Mask constants as above)
constexpr HandyPose kPickupHoldPoses[] = {{-55.0f, {-0.2446f, -0.0958f, 0.1462f}}, {-45.8f, {-0.2442f, -0.1141f, 0.1072f}},
                                          {-38.15f, {-0.2459f, -0.1183f, 0.0859f}}, {-22.17f, {-0.2484f, -0.1213f, 0.0380f}}};

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

}  // namespace

void HandyLightPose(const Camera& camera, const glm::vec3& aim_point, float pickup_hold, float demo_pose, glm::vec3& position,
                    glm::vec3& direction, glm::vec3* right_out, glm::vec3* up_out) {
    const glm::vec3 forward = camera.Forward();
    const glm::vec3 right = camera.Right();
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));
    const float view_pitch = glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
    HandyPose pose = PoseAt(kHandyPoses, view_pitch);
    if (pickup_hold > 0.0f) {
        pose.pivot = glm::mix(pose.pivot, PoseAt(kPickupHoldPoses, view_pitch).pivot, std::min(pickup_hold, 1.0f));
    }
    // the hand turns about the pivot until the flash point aims at the target: d = normalize(target - light(d)), solved by
    // iteration in the camera frame (the offset moves the light by at most 0.15 m, so it converges at once for targets beyond
    // 0.4 m)
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
    position = camera.position + right * light_cam.x + up * light_cam.y + forward * light_cam.z;
    direction = glm::normalize(right * d_cam.x + up * d_cam.y + forward * d_cam.z);
    // the hand's frame (right of the aim about the camera's up, up of the aim), in the world
    if (right_out) {
        *right_out = glm::normalize(right * r_cam.x + up * r_cam.y + forward * r_cam.z);
    }
    if (up_out) {
        *up_out = glm::normalize(right * u_cam.x + up * u_cam.y + forward * u_cam.z);
    }
    // under a demo camera (0x12810C0's branch for the camera object of 0x970DE0 +0xC0) the light stands at a fixed offset from
    // that camera, 0.165 m left, 0.13 m down and 0.185 m ahead, aimed at the look target (pickup_release_f060 2639-2720)
    if (demo_pose > 0.0f) {
        const glm::vec3 fixed = camera.position - right * 0.165f - up * 0.13f + forward * 0.185f;
        position = glm::mix(position, fixed, std::min(demo_pose, 1.0f));
        const glm::vec3 to_target = aim_point - position;
        if (glm::dot(to_target, to_target) > 1.0e-6f) {
            direction = glm::normalize(to_target);
        }
    }
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
    // 0xE149F0: spot lights whose name ends in these (0xE18230) take lumen 40 over their data
    const std::string_view last = std::string_view(name).substr(name.rfind('|') == std::string::npos ? 0 : name.rfind('|') + 1);
    if (spot && (last == "8SL_stairs_down0000" || last == "8SL_stairs_down0002")) {
        lumen = 40.0f;
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

// 0xE07C70: isEnable (+0x130), isOneSideMode (+0x131, runtime flag 2), numVertices (+0x138) points of positions (+0x140) in the
// entity's frame
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
    // localFlags bit 4 (16): a dark probe. Every such probe of the hallway sets (LP_0031, LP_0034, LP_0042, LP_DarkProbe0000 and
    // 0001) is left out of its .lpsh and every other probe is in it, so the bake skipped them; they take their box weight at their
    // low priority with no light, which darkens what the later probes would light there (ceillamp_f100 3355: the pillar side
    // under the lamp, black in the capture)
    // PT_DARK_PROBES=0 leaves them out as before (comparisons)
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
    // The plain real SH expansion (rendering.md 5): SH_LIGHTING (0xC81D60) multiplies each coefficient by its basis constant
    // (0xC82D50) and a band multiplier of 1, with no lobe weights and no scale. The weights (+0x10..+0x30) and scale (+0xC)
    // that 0xCED2F0 reads go to 0xC82990 for the per-model REFLECTION_MAP variant, not to the probe ambient.
    // PT_SH_LEGACY=1 brings back the port's former stand-in, the cosine lobe weights (1, 2/3, 1/4) per band, for A/B shots only.
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
    const glm::vec3 target = fx.handy_light_color;
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
    // PT_RENDER_OFF=handy drops the handy light alone, so a shot minus the same shot without it measures its share
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
    l.name = "PlayerHandyLight";
    l.type = LightType::Spot;
    const glm::vec3 up = glm::normalize(glm::cross(camera.Right(), camera.Forward()));
    HandyLightPose(camera, aim_point, game.HandyPickupHold(), game.HandyDemoPose(), l.position, l.direction);
    // VR: the flashlight in the tracked controller's hand
    if (const auto& pose = game.HandyPoseOverride()) {
        l.position = pose->first;
        l.direction = pose->second;
    }
    // the free camera, the photo mode and the third person view show the flashlight in the hand: the beam leaves its lens
    // (Game::AddMirrorBody)
    if (glm::vec3 lens; game.HandyLens(lens)) {
        // The housing is drawn between ticks too. Its lens and emitting point must use the same frame.
        if (blend && blend->handy_lens_valid && glm::distance(blend->handy_lens, lens) <= 1.0f) {
            lens = glm::mix(blend->handy_lens, lens, blend->t);
        }
        if (game.ThirdPerson() && !game.FreeView()) {
            const float weight = game.ThirdPersonWeight();
            const float pose_weight = weight * weight * (3.0f - 2.0f * weight);
            l.position = glm::mix(l.position, lens, pose_weight);
        } else if (game.DetachedView()) {
            l.position = lens;
        }
    }
    l.up = glm::normalize(up - l.direction * glm::dot(up, l.direction));
    const glm::vec3 color = glm::vec3(p.color[0], p.color[1], p.color[2]) * handy_color_;
    const glm::vec3 rgb = light_math::ColorFromTemperature(p.temperature, 0.0f, p.lumen, color);
    const float omega = light_math::SpotSolidAngle(p.umbra_angle, p.penumbra_angle, p.attenuation_exponent);
    l.intensity = rgb / omega * p.power_scale / kPi;
    l.source_radius = light_math::SourceRadius(p.light_size);
    l.inner_range = p.inner_range;
    l.outer_range = p.outer_range;
    l.dimmer = p.dimmer;
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
    out.lights.push_back(l);
    AddHandyReflection(game, l, out);
}

// 0x9359D0 drives four GrLights from the handy light (Game::UpdateHandyReflection): reflectLightPoint1 and reflectLight0 at the
// beam's hit, reflectLightPoint2 and reflectLight02 at the hit they fade out from. Each takes the handy light's intensity (its
// GrLight value, +0x40 of the module, written by 0x124FE00 and 0x124F980) times its weight, the attenuation of its depth in the
// view, the colour read back from the module's 64x64 view of the beam (always white, see below) and 2.5: the points times
// 1 - the spot share,
// the spots times the share and 1.5. Ranges are 0.4 of the handy light's (0x9359D0 sets them every frame; the constructor
// 0x933C20 starts them at 0.5), the dimmer 4 (0xCDB160 and 0xCDBD70 with 4.0), no shadow and no specular (the points draw
// with SSLighting2_Point_NoSpec_NoShadow); the spots have an umbra of 100 degrees (0xCDBDD0 with 1.7453 rad), no penumbra and
// exponent 1 and turn +Z to the module's eased normal (+0x90). On by default; PT_REFLECT=0 leaves the lights and the sample out.
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
    // The read back pixel (column 0, row 32) is written last by Draw2D_ShSpotLightReflection3 over (0, 0, 64, 64), whose output is
    // saturate(saturate(0.175 (t1 + t2 + t3 + t5) + 0.3 t4 + 2.0) x inColor): the + 2.0 saturates every channel whatever the
    // beam sees, so the lights take white x the shape colour. The captures agree: f050a_walls_2955, lisa_balcony_f070 3785-3795
    // and bath_producer_4280 (reflectLightPoint1 (74.93, 62.94, 47.50) = handy (48.34, 40.60, 30.64) x 2.5 x attenuation 0.6205
    // x 1.000, beam on the dark bath hole). Sampling the scene instead dimmed the lights over dark surfaces 7-12 times, which kept
    // the flashlight-lit dust in front of the f060 bath hole from glinting. PT_REFLECT_SAMPLE=1 brings the scene sample back for A/B.
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
                l.cos_outer = CosHalf(100.0f);
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
                            // 0x95ADA0 hands 0x538970 a box at the locator's world position (0x531CA0, the stage's placement
                            // included), `size` times the locator's scale wide (0x531D70), turned by the rotation 0x531530
                            // gathers from the entity parents, which leaves the stage's placement locator out
                            // (`ShRelativeStageLocator`, -90 degrees for the hallway): the box keeps the file's axes, so in
                            // the hallway its 1.759 m side runs along the world x axis. mirror_f060 fixes it: the MirrorLight sits
                            // at x -5.089 (0.531 m behind the mirror, the box's near face) at 4935 to 5130 and at z 24.445 (the
                            // box's side) at 5325 to 5337, centre (-5.969, 23.771) with half sides 0.879 (x) and 0.674 (z); with
                            // the stage's turn the box stood 0.206 m further back and its z face 0.2 m off, which put the
                            // bounce 0.2 m too far from the door and the light inside the box where the original clamps it.
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

// 0x538970 (slab test 0x538150): where the ray o + t d, t from 0, enters the box [-0.5, 0.5]^3 of `box`; a ray that starts
// inside enters at its origin
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

    // These homogeneous coordinates locate the source-to-receiver segment's mirror-plane intersection in aperture units.
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

// The MirrorLight (created by 0x95ABB0 and updated every frame by 0x95ADA0 while a capture runs), the handy light as the
// mirror returns it. It copies the handy light with the intensity times 4, the outer range plus 2 m and the umbra angle
// times 0.6 (the mask texture spans the narrower umbra, 0xD4E040); the inner range, dimmer, penumbra, cone exponent, shadow
// cone, light size, bias and mask are the handy light's, castShadow is on and its priority byte 0x40 (no priority light).
// With the handy light on and its axis toward the mirror, the light turns with the handy light mirrored at the mirror plane
// (0x95A5A0) and sits on the returned beam, the line through the mirrored light position P' and the point H where the axis
// crosses the plane (0x543DF0): at the point where the ray P' -> H enters the mirror's light-area box (MirrorLightAreaLocator,
// 0.74 to 2.09 m behind the hallway mirror; P' itself when it lies inside), else where the ray H -> P' enters it. It is off
// when the axis points away from the mirror or both rays miss. The capture serves the Mirror nearest to the camera (0x956D10).
// The light stands behind the wall, in the hallway: with the camera view's casters the mirror's glass_a (3.8 mm behind the
// mirror, where the bathroom's tile wall has its opening) and the wall around it would hide the bathroom from it. The capture
// views render their own shadow maps (0xD3F0C0), in which the mirror model, hidden in those views (0x958530), casts nothing:
// the light reaches the room through the mirror's opening. The PS4 capture draws it in the camera view too, as the same
// SSLighting2_Spot_Mask light (mirror_f060 5330: draw 1348 at (-5.342, 1.597, 24.445) as the capture view's draw 614), so the
// port also evaluates it in the camera view with the mirror view's shadow casters (SceneRenderer::BuildShadowViews). A PS4
// reference shows a finite patch on the short wall beside the bathroom door. The port bounds this light by the transformed
// glass mesh; that finite-aperture test is an implementation inference from the glass and wall geometry, not a claim about the
// original shadow-map contents. Whether the camera draw shares the capture shadow map is also an inference; PT_MIRROR_LIGHT_CAMERA=0 keeps it out of the camera view.
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
    const bool trace = std::getenv("PT_MIRROR_TRACE") != nullptr;
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
    // mirror_f060: the MirrorLight's m_lightParams[4].w (the source radius the highlight's roughness uses) is 0.0125 against the handy
    // light's 0.025 in every frame of 4935 to 5342
    l.source_radius *= 0.5f;
    l.outer_range += 2.0f;
    l.cos_outer = CosHalf(params.umbra_angle * 0.6f);
    l.inv_cone_range = InverseRange(CosHalf(params.penumbra_angle), l.cos_outer);
    l.mask_fov = params.umbra_angle * 0.6f * kPi / 180.0f;
    // The shadow cone shrinks with the umbra: mirror_f060's MirrorLight draws carry m_lightParams[7] = (0.9178, 20.759) = cos(23.4
    // degrees) and 1 / (cos 15 degrees - cos 23.4 degrees), the handy light's (0.777, 5.297) with the same full-shadow edge at 15
    // degrees. So the shadow only reaches full strength within 15 degrees of the axis and fades out by 23.4 degrees; outside that
    // the light is unshadowed (rendering.md 4.x, shadowCone). PT_MIRROR_SHADOW_CONE=handy keeps the handy light's cone.
    static const bool handy_cone = [] {
        const char* value = std::getenv("PT_MIRROR_SHADOW_CONE");
        return value && std::string_view(value) == "handy";
    }();
    if (!handy_cone) {
        const float full = handy->shadow_cos_outer + 1.0f / std::max(handy->shadow_inv_cone_range, 1.0e-6f);
        l.shadow_cos_outer = CosHalf(params.umbra_angle * 0.6f);
        l.shadow_inv_cone_range = 1.0f / std::max(full - l.shadow_cos_outer, 1.0e-6f);
    }
    // The original gives the MirrorLight castShadow on (rendering.md 12.20). It stands just behind the mirror with its
    // shadow cone around it, so a bias that suits the lamp and door lights leaves its own surface in shadow, and the
    // mirror's dirt then gets no light at all (measured: the MirrorLight alone lights the mirror region by 2.4 levels of
    // 255 against a peak of 40). PT_MIRROR_LIGHT_SHADOW=0 turns the light's shadow off, the one-variable test for that.
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
    // The half-space box keeps the CPU's light-area ranking on the room side; the GPU uses the mirror mesh's finite aperture.
    // Without that aperture, the skipped paper_a1 caster lets the light spill across the short wall. PT_MIRROR_LIGHT_CLIP=0
    // disables both clip tests.
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
    // PT_MIRROR_LIGHT_OFF=1 drops it alone, so a shot with and one without show the light the capture adds
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
    // 0x127AB20: the look target ray starts 0.1 m ahead of the camera (camera component +0x26C, which the P.T. camera
    // plugin's enter 0x982F90 sets to 0.1), so a surface closer than that, such as the bathroom wall behind the
    // peephole camera, does not take the focus; 0x978450 measures the focus from the camera to the hit (+0x274). The ray
    // is cast with mask 0x700, which the detailed surfaces answer and the movement hull does not (Game::LineCollision): in
    // front of the bathroom mirror the hull stands 0.25 m out and the capture's focus is 0.607 m, the mirror (mirror_f060 4950)
    constexpr float kRayStart = 0.1f;
    RayHit hit;
    float target = 1500.0f;
    const bool hit_any = game.LineCollision().Raycast(camera.position + camera.Forward() * kRayStart, camera.Forward(), 1500.0f, hit);
    if (hit_any) {
        target = hit.distance + kRayStart;
    }
    target = std::clamp(target, 0.5f, 1500.0f);
    // after a camera cut (the first frame, a teleport or a scripted camera jump) the focus starts at the target, as the exposure is settled
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
    // PT_BUILD_PROFILE=1: the mean milliseconds of Build's steps over each 120 frames
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
    // lights and probes of a loaded but inactive stage are not in the scene, as its models (StageManager::CollectDraws)
    game.Stages().ForEachStage(
        [&](Stage& stage) {
            if (!stage.active) {
                return;
            }
            for (const auto& data : stage.files) {
                for (const LightPlacement& light : data->lights) {
                    if (!light.entity || !EntityEnabled(&stage, *data->file, *light.entity)) {
                        continue;
                    }
                    const uint64_t id = (uint64_t(stage.id) << 40) ^ reinterpret_cast<uintptr_t>(light.entity);
                    AddLight(*data->file, *light.entity, stage.file_to_world, id, out);
                }
            }
        },
        false);
    stamp();
    // the static caches below hold entity pointers: any change of the loaded stages empties them
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
            // probes and occluders are static once a stage is loaded: the file's entities are scanned for them once and each
            // is built once per stage placement (rebuilding them every frame, with every entity of every active stage
            // compared by class name, took 13.6 ms a frame in the red lamp loop when its next hallway copy was active)
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
    // with the free camera and the third person view (Extras) the light stays in the player's hand: posed from the player's own
    // view, not the rendered one. Keep the third-person pose on this camera throughout its blend, even before the body is shown.
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
    for (const DemoLight& light : game.Demos().Lights()) {
        AddDemoLight(light, blended ? blend : nullptr, out);
    }

    const ScreenEffects& fx = game.Effects();
    // 0x959FD0: the capture runs while the floor's MirrorCapture flag is on and a MirrorSwitch trap has set a viewport bit
    // PT_MIRROR_CAPTURE=0 drops the capture alone, so a shot with and one without measure what the mirror shows
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
    // PT_NO_LOCAL_REFLECTIONS=1 drops the level's screen space reflections alone, so a shot with and one without
    // measures their share (the reflection that goes when the camera looks down, report 2 of the tester notes)
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
    screen.colour_banding_canceller = fx.colour_banding_canceller;
    screen.blur_blend_rate = fx.blur_blend_rate;
    screen.blur_fetch_band = fx.blur_fetch_band;
    screen.zoom = game.GetPlayer().zoom;
    screen.depth_of_field = (resident_.plugin_flags & 0x8u) != 0;
    screen.motion_blur = (resident_.plugin_flags & 0x4u) != 0;
    screen.fixed_shutter = (resident_.plugin_flags & 0x80u) != 0;
    screen.focus_distance = focus;
    screen.focal_length = game.Parameters().player.focal_length;
    screen.aperture = kCameraAperture;
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
                "dof {} motion blur {} zoom {:.2f} distortion {} demo camera {}",
                game.Frame(), game.Floor().CurrentFloorName(), focus, screen.focus_distance, screen.focal_length,
                screen.aperture, screen.full_screen_blur, screen.blur_blend_rate, screen.blur_fetch_band, screen.depth_of_field,
                screen.motion_blur, screen.zoom, screen.screen_distortion, game.Demos().CameraParams() != nullptr);
    }
    stamp();
    AddTppAtmosphere(game, out);
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
                        // 0x906330: m1 = e (selfColor.rgb selfColor.a selfLuminance + sky), m2 = (mie.rgb mie.a, 1.55 g - 0.55 g^3),
                        // m3 = rayleigh.rgb rayleigh.a (0 in P.T., whose rayleighScattering has alpha 0), m0.w = e dirLightGain |dir|.
                        // The sky term (skyAlbedo.rgb skyAlbedo.a skyLightGain times the atmosphere's sky colour) is 0 in P.T.,
                        // whose skyAlbedo has alpha 0
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
    // The fog's directional light (0x8EEC00: FUN_008dff20 over pi, times the fog object's +0xA0, and the negated direction of
    // FUN_008df890) is the atmosphere's moon: ending_fog_spot traces it at 3165, 3560 and 4080 as VolFog m0.w = 0.48971 =
    // e 3.5 |(7.162, 12.732, 20.690)| and, in the area fog colour, as (7.162, 12.732, 20.690) times the area albedo, which is
    // TppAtmosphere moonColor (0.45, 0.8, 1.3) times moonLux 50 over pi; m_localParam[1] = (0.39491, 0.85361, 0.33969) in
    // all three frames. The atmosphere's sun and moon positions are not ported; with P.T.'s mieAnisotropy 0 and rayleigh 0 the
    // direction does not change the fog.
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
                        // TppSky draws Sky_Draw_TppBaked and its dome every frame it is enabled (sh_sky.fox2: enable true)
                        if (e.class_name == "TppSky" && f.GetBool(e, "enable", 0, true)) {
                            tpp.sky = true;
                        }
                    }
                }
            },
            false);
    }
    // DR_VolFog_TppTonemap composes every floor, with a 1x1 fog volume where no fog is set (f010_dumps 1490)
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
    // the street walk's scenery demo lends its camera's exposure, never its lens (DemoSystem::SceneryCameraParams)
    const DemoCameraParams* camera_params = demos.CameraParams() ? demos.CameraParams() : demos.SceneryCameraParams();
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
    // the depth of field reads the lens one original frame late (DemoSystem::DofLens): a demo's first frame still blurs with the
    // game's lens and its last with its own
    if (const DemoCameraParams* lens = demos.DofLens()) {
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
