#include "game/vfx_scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>

#include "engine/anim/sim_physics.h"
#include "engine/assets/fmdl.h"
#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "engine/data/fox2.h"
#include "engine/core/memory_status.h"
#include "engine/render/vfx_pass.h"
#include "game/demo_system.h"
#include "game/game.h"
#include "game/game_objects.h"
#include "game/game_sound.h"
#include "game/stage_manager.h"

namespace pt::game {

namespace {

constexpr float kPi = 3.14159265f;
constexpr uint64_t kDemoOwner = 0xDE300000000000ull;
constexpr uint64_t kGimmickOwner = 0x61C0000000000ull;
constexpr uint64_t kFunctorWind = 0x5B5B7F07E849ull;
constexpr uint32_t kKeyWindSpeed = 0x39774475;
constexpr uint32_t kKeyWindRotation = 0x4BAA6AFC;

struct GimmickLights {
    GimmickType type;
    bool retrigger;
    std::array<const char*, 3> names;
};

constexpr GimmickLights kGimmickLights[] = {
    {GimmickType::Freezer, true, {"FreezerBloodTop", "FreezerBloodEye", nullptr}},
    {GimmickType::CeilLamp, false, {"CeilLampGlass", "CeilLampFlareLight", "CeilLampFlareLightRed"}},
};

bool IsSoundEffect(const std::string& path) {
    return path.find("/vfx_data/sound/") != std::string::npos || path.find("fxsd_") != std::string::npos;
}

float CosHalf(float degrees) {
    return std::cos(degrees * kPi / 360.0f);
}

float InverseRange(float cos_inner, float cos_outer) {
    return 1.0f / std::max(cos_inner - cos_outer, 1.0e-4f);
}

glm::vec3 ProbeAmbient(const SceneLighting& lighting, const glm::vec3& p) {
    glm::vec3 sum(0.0f);
    float total = 0.0f;
    const SceneProbe* nearest = nullptr;
    float nearest_d = 1e30f;
    for (const SceneProbe& probe : lighting.probes) {
        const glm::vec3 q = glm::vec3(probe.world_to_box * glm::vec4(p, 1.0f));
        const glm::vec3 fp = glm::clamp((glm::vec3(1.0f) - q) * probe.positive_scale, 0.0f, 1.0f);
        const glm::vec3 fn = glm::clamp((glm::vec3(1.0f) + q) * probe.negative_scale, 0.0f, 1.0f);
        const glm::vec3 f = glm::min(fp, fn);
        const float w = probe.weight * f.x * f.y * f.z;
        if (w > 0.0f) {
            sum += probe.sh[0] * w;
            total += w;
        }
        const float d = glm::length(glm::max(glm::abs(q) - glm::vec3(1.0f), glm::vec3(0.0f)));
        if (d < nearest_d) {
            nearest_d = d;
            nearest = &probe;
        }
    }
    if (total > 0.0f) {
        return sum / total;
    }
    return nearest ? nearest->sh[0] : glm::vec3(0.02f);
}

glm::vec3 PointLighting(const SceneLighting& lighting, const glm::vec3& p) {
    glm::vec3 best[3] = {glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)};
    float best_l[3] = {0.0f, 0.0f, 0.0f};
    for (const SceneLight& l : lighting.lights) {
        if (l.type != LightType::Point) {
            continue;
        }
        const glm::vec3 d = l.position - p;
        const float dist = glm::length(d);
        if (dist >= l.outer_range || l.outer_range <= 0.0f) {
            continue;
        }
        const float d2 = std::max(dist * dist, 0.01f);
        const float outer2 = l.outer_range * l.outer_range;
        const float atten = std::max(0.0f, 1.0f / d2 - d2 / (outer2 * outer2));
        const glm::vec3 c = l.intensity * atten;
        const float lum = c.r + c.g + c.b;
        for (int i = 0; i < 3; ++i) {
            if (lum > best_l[i]) {
                for (int k = 2; k > i; --k) {
                    best_l[k] = best_l[k - 1];
                    best[k] = best[k - 1];
                }
                best_l[i] = lum;
                best[i] = c;
                break;
            }
        }
    }
    return best[0] + best[1] + best[2];
}

vfx::LightBlock LightBlockAt(const SceneLighting& lighting, const glm::vec3& center, const glm::vec3& half) {
    vfx::LightBlock block;
    const float radius = std::max({half.x, half.y, half.z});
    struct Candidate {
        const SceneLight* light = nullptr;
        float score = 0.0f;
        float cone = 1.0f;
    };
    std::vector<Candidate> candidates;
    for (const SceneLight& l : lighting.lights) {
        if ((l.type != LightType::Point && l.type != LightType::Spot) || (l.hidden_views & 1u) != 0 || l.outer_range <= 0.0f) {
            continue;
        }
        const glm::vec3 reach(l.outer_range + l.dimmer);
        if (glm::any(glm::greaterThan(l.position - reach, center + half)) || glm::any(glm::lessThan(l.position + reach, center - half))) {
            continue;
        }
        const glm::vec3 to = center - l.position;
        const float dist = glm::length(to);
        float cone = 1.0f;
        if (l.type == LightType::Spot) {
            const float cos_a = dist > 1.0e-6f ? glm::dot(glm::normalize(l.direction), to / dist) : 1.0f;
            cone = std::pow(std::clamp((cos_a - l.cos_outer) * l.inv_cone_range, 0.0f, 1.0f), 2.7182817f);
        }
        const float d = std::max(dist, l.inner_range);
        const float r = l.outer_range + radius;
        const float falloff = 1.0f / std::max(d * d, 1.0e-12f) - d * d / std::max(r * r * r * r, 1.0e-12f);
        candidates.push_back({&l, glm::length(l.intensity) * cone * falloff, cone});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    for (size_t k = 0; k < 3 && k < candidates.size(); ++k) {
        const SceneLight& l = *candidates[k].light;
        const float range = l.outer_range + l.dimmer;
        block.position[block.count] = l.position;
        block.color[block.count] = l.intensity * candidates[k].cone;
        block.inv_r4[block.count] = 1.0f / std::max(range * range * range * range, 1.0e-12f);
        ++block.count;
    }
    std::vector<const SceneProbe*> probes;
    for (const SceneProbe& p : lighting.probes) {
        const glm::vec3 q = glm::vec3(p.world_to_box * glm::vec4(center, 1.0f));
        if (glm::all(glm::lessThanEqual(glm::abs(q), glm::vec3(1.0f + 1.0e-3f)))) {
            probes.push_back(&p);
        }
    }
    std::stable_sort(probes.begin(), probes.end(), [](const SceneProbe* a, const SceneProbe* b) { return a->priority < b->priority; });
    float weights[3] = {0.0f, 0.0f, 0.0f};
    float left = 1.0f;
    float total = 0.0f;
    for (size_t k = 0; k < 3 && k < probes.size(); ++k) {
        const SceneProbe& p = *probes[k];
        const glm::vec3 q = glm::vec3(p.world_to_box * glm::vec4(center, 1.0f));
        const glm::vec3 f = glm::min(glm::clamp((1.0f - q) * p.positive_scale, 0.0f, 1.0f), glm::clamp((1.0f + q) * p.negative_scale, 0.0f, 1.0f));
        weights[k] = probes.size() == 1 ? 1.0f : left * p.weight * f.x * f.y * f.z;
        left -= weights[k];
        total += weights[k];
    }
    if (total >= 1.0e-5f) {
        for (size_t k = 0; k < 3 && k < probes.size(); ++k) {
            const std::array<glm::vec3, 9>& sh = probes[k]->sh;
            block.sky += weights[k] / total * glm::max(sh[0] + sh[2] + 2.0f * sh[6], glm::vec3(0.0f));
        }
    } else {
        block.sky = ProbeAmbient(lighting, center);
    }
    block.directional = glm::vec3(0.0f);
    return block;
}

SceneLight ToSceneLight(const vfx::LightOut& v) {
    SceneLight l;
    l.id = v.id;
    l.type = v.spot ? LightType::Spot : LightType::Point;
    l.position = v.position;
    l.direction = glm::length(v.direction) > 1e-5f ? glm::normalize(v.direction) : glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 up = v.up - l.direction * glm::dot(v.up, l.direction);
    if (glm::dot(up, up) > 1.0e-8f) {
        l.up = glm::normalize(up);
    } else {
        const glm::vec3 side = std::fabs(l.direction.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        l.up = glm::normalize(glm::cross(glm::cross(l.direction, side), l.direction));
    }
    const glm::vec3 rgb = light_math::ColorFromTemperature(v.temperature, 0.0f, v.lumen, v.color);
    l.inner_range = v.inner_range;
    l.outer_range = std::max(v.outer_range, 0.01f);
    l.cast_shadow = v.shadow && v.spot;
    l.specular_scale = v.specular ? 1.0f : 0.0f;
    l.shadow_bias = v.shadow_bias;
    if (v.spot) {
        const float umbra = std::clamp(v.umbra, 1.0f, 179.0f);
        const float penumbra = std::clamp(v.penumbra, 0.0f, umbra);
        const float omega = light_math::SpotSolidAngle(umbra, penumbra, v.attenuation);
        l.intensity = rgb / omega / kPi;
        l.cos_outer = CosHalf(umbra);
        l.inv_cone_range = InverseRange(CosHalf(penumbra), l.cos_outer);
        l.cone_exponent = v.attenuation;
        l.shadow_cos_outer = CosHalf(v.shadow_umbra);
        l.shadow_inv_cone_range = InverseRange(CosHalf(v.shadow_penumbra), l.shadow_cos_outer);
        l.shadow_fov = v.shadow_umbra * kPi / 180.0f;
        l.view_bias = v.view_bias;
        l.masked = v.mask >= 0;
        l.mask_texture = v.mask;
        l.mask_fov = umbra * kPi / 180.0f;
    } else {
        l.intensity = rgb / (4.0f * kPi) / kPi;
    }
    if (v.area && std::abs(glm::determinant(glm::mat3(v.area_world))) > 1.0e-9f) {
        l.has_area = true;
        l.area_world = v.area_world;
        l.area_to_box = glm::inverse(v.area_world);
    }
    return l;
}

}

VfxScene::VfxScene() = default;
VfxScene::~VfxScene() = default;

void VfxScene::Clear() {
    system_.Clear();
    stages_.clear();
    demo_keys_.clear();
    demo_key_demo_.clear();
    gimmicks_ = {};
}

glm::vec3 VfxScene::Wind(Game& game) const {
    glm::vec3 wind(0.0f);
    for (const auto& [id, entry] : stages_) {
        if (entry.has_wind) {
            wind = entry.wind;
            break;
        }
    }
    for (const PlayingDemo& demo : game.Demos().Playing()) {
        if (demo.finished || !demo.stream) {
            continue;
        }
        for (const anim::StreamEvent* e : demo.stream->events) {
            if (e->functor != kFunctorWind || demo.frame < e->Start() || demo.frame >= e->End()) {
                continue;
            }
            const glm::vec4 r = e->Vector(kKeyWindRotation, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            const float length = glm::length(wind);
            const glm::vec3 direction = length > 1e-3f ? wind / length : glm::vec3(0.0f, 0.0f, 1.0f);
            wind = glm::quat(r.w, r.x, r.y, r.z) * direction * e->Float(kKeyWindSpeed);
        }
    }
    return wind;
}

vfx::ViewInfo VfxScene::View(const Camera& camera, float aspect) const {
    vfx::ViewInfo v;
    v.position = camera.position;
    v.forward = camera.Forward();
    v.up = camera.Up();
    v.right = camera.Right();
    v.aspect = aspect > 0.0f ? aspect : 16.0f / 9.0f;
    v.fov_y = camera.fov_y;
    v.near_plane = camera.near_plane;
    v.view_projection = camera.Projection(v.aspect) * camera.View();
    return v;
}

void VfxScene::SyncStages(Game& game) {
    std::set<uint32_t> alive;
    game.Stages().ForEachStage([&](Stage& stage) {
        alive.insert(stage.id);
        auto [it, inserted] = stages_.try_emplace(stage.id);
        StageEntry& entry = it->second;
        if (inserted) {
            for (const auto& data : stage.files) {
                const fox2::DataSetFile& f = *data->file;
                for (const fox2::Entity& e : f.Entities()) {
                    if (e.class_name == "WindGlobal" && !entry.has_wind) {
                        if (const fox2::Entity* parameter = f.GetEntity(e, "parameter")) {
                            entry.wind = glm::mat3(stage.file_to_world) * glm::vec3(f.GetVec4(*parameter, "velocity"));
                            entry.has_wind = true;
                        }
                        continue;
                    }
                    if (e.class_name != "FxLocatorData") {
                        continue;
                    }
                    Locator loc;
                    loc.path = f.GetString(e, "vfxFile");
                    if (loc.path.empty() || IsSoundEffect(loc.path)) {
                        continue;
                    }
                    loc.entity = &e;
                    loc.file = &f;
                    loc.file_transform = f.WorldTransform(e);
                    loc.seed = static_cast<uint32_t>(StrCode64(f.EntityName(e))) | 1u;
                    preload_.push_back(loc.path);
                    entry.locators.push_back(std::move(loc));
                }
            }
            if (!entry.locators.empty()) {
                LogInfo("vfx: stage {} ({}) has {} effect locators", stage.label, stage.id, entry.locators.size());
            }
        }
        for (Locator& loc : entry.locators) {
            const vfx::InstanceKey key{stage.id, reinterpret_cast<uintptr_t>(loc.entity)};
            const BodyState& body = stage.Body(loc.entity);
            const bool visible = stage.active && body.visible && body.enable;
            const bool exists = system_.Exists(key);
            if (!visible) {
                if (exists) {
                    system_.Remove(key);
                }
                continue;
            }
            const glm::mat4 world = stage.ToWorld(loc.file_transform);
            if (!exists) {
                if (loc.failed) {
                    continue;
                }
                if (!system_.Spawn(key, loc.path, world, loc.seed)) {
                    loc.failed = true;
                    LogDebug("vfx: {} has nothing to draw", loc.path);
                    continue;
                }
                LogDebug("vfx: {} on at ({:.2f} {:.2f} {:.2f})", loc.path, world[3].x, world[3].y, world[3].z);
            } else {
                system_.SetTransform(key, world);
            }
        }
        if (entry.active != stage.active) {
            entry.active = stage.active;
            size_t on = 0;
            for (const Locator& loc : entry.locators) {
                on += system_.Exists({stage.id, reinterpret_cast<uintptr_t>(loc.entity)}) ? 1 : 0;
            }
            LogInfo("vfx: stage {} ({}) {} at frame {}, {} of {} effect locators on", stage.label, stage.id, stage.active ? "active" : "inactive",
                    game.Frame(), on, entry.locators.size());
        }
    });
    for (auto it = stages_.begin(); it != stages_.end();) {
        if (!alive.contains(it->first)) {
            system_.RemoveOwner(it->first);
            it = stages_.erase(it);
        } else {
            ++it;
        }
    }
}

void VfxScene::SyncDemos(Game& game) {
    std::set<vfx::InstanceKey> seen;
    for (const DemoEffect& effect : game.Demos().Effects()) {
        if (effect.file_path.empty() || IsSoundEffect(effect.file_path)) {
            continue;
        }
        const vfx::InstanceKey key{kDemoOwner ^ StrCode64(effect.demo_id), effect.instance};
        glm::mat4 world = effect.world;
        if (effect.rotation != glm::vec3(0.0f)) {
            const glm::vec3 r = glm::radians(effect.rotation);
            world = world * glm::rotate(glm::mat4(1.0f), r.y, glm::vec3(0.0f, 1.0f, 0.0f)) *
                    glm::rotate(glm::mat4(1.0f), r.x, glm::vec3(1.0f, 0.0f, 0.0f)) * glm::rotate(glm::mat4(1.0f), r.z, glm::vec3(0.0f, 0.0f, 1.0f));
        }
        if (system_.IsStopped(key) && !demo_keys_.contains(key)) {
            system_.Remove(key);
        }
        if (!system_.Exists(key)) {
            if (demo_keys_.contains(key)) {
                seen.insert(key);
                continue;
            }
            const uint32_t seed = static_cast<uint32_t>(std::max(effect.created_frame, 0));
            if (system_.Spawn(key, effect.file_path, world, seed)) {
                LogInfo("vfx: demo {} effect {} created", effect.demo_id, effect.file_path);
            }
        } else {
            system_.SetTransform(key, world);
        }
        for (const auto& [code, value] : effect.parameters) {
            system_.SetParameterLow16(key, static_cast<uint32_t>(code & 0xFFFFu), value);
        }
        seen.insert(key);
    }
    std::set<std::string_view> playing;
    for (const PlayingDemo& demo : game.Demos().Playing()) {
        playing.insert(demo.demo_id);
    }
    std::map<vfx::InstanceKey, std::string> demo_of;
    for (const DemoEffect& effect : game.Demos().Effects()) {
        demo_of[{kDemoOwner ^ StrCode64(effect.demo_id), effect.instance}] = effect.demo_id;
    }
    for (const vfx::InstanceKey& key : demo_keys_) {
        if (!seen.contains(key)) {
            const auto it = demo_key_demo_.find(key);
            if (it != demo_key_demo_.end() && playing.contains(it->second)) {
                system_.Stop(key);
            } else {
                system_.Remove(key);
            }
        }
    }
    demo_key_demo_ = std::move(demo_of);
    demo_keys_ = std::move(seen);
}

void VfxScene::LoadGimmickParts(Game& game, size_t index, const std::string& parts) {
    GimmickEntry& entry = gimmicks_[index];
    for (size_t i = 0; i < entry.effects.size(); ++i) {
        for (size_t c = 0; c < entry.effects[i].connections.size(); ++c) {
            system_.Remove({kGimmickOwner | index, i * 16 + c});
        }
    }
    entry = GimmickEntry{};
    entry.parts = parts;
    const GimmickLights* table = nullptr;
    for (const GimmickLights& t : kGimmickLights) {
        if (static_cast<size_t>(t.type) == index) {
            table = &t;
        }
    }
    auto bytes = parts.empty() || !table ? std::nullopt : game.GetVfs().ReadFile(parts);
    fox2::DataSetFile file;
    if (!bytes || !file.Load(parts, *bytes)) {
        return;
    }
    entry.retrigger = table->retrigger;
    for (const char* name : table->names) {
        if (name) {
            entry.effects.push_back(PartEffect{name});
        }
    }
    for (const fox2::Entity& e : file.Entities()) {
        if (e.class_name == "ModelDescription") {
            if (auto model = game.GetVfs().ReadFile(file.GetString(e, "modelFile"))) {
                anim::ReadFmdlSkeleton(*model, entry.skeleton);
            }
            const std::string cnp = file.GetString(e, "connectPointFile");
            if (auto cnp_bytes = cnp.empty() ? std::nullopt : game.GetVfs().ReadFile(cnp)) {
                ReadConnectPoints(*cnp_bytes, entry.connect_points);
            }
            continue;
        }
        if (e.class_name != "EffectDescription") {
            continue;
        }
        const std::string name = file.GetString(e, "partName");
        auto it = std::find_if(entry.effects.begin(), entry.effects.end(), [&](const PartEffect& p) { return p.name == name; });
        if (it == entry.effects.end()) {
            continue;
        }
        it->file = file.GetString(e, "effectFileFromFilePtr");
        if (it->file.empty()) {
            it->file = file.GetString(e, "effectFileFromVfxFileLoader");
        }
        it->seed = file.GetUInt(e, "effectRandomSeed");
        vfx::SoundNode node;
        if (auto effect_bytes = it->file.empty() ? std::nullopt : game.GetVfs().ReadFile(it->file); effect_bytes && vfx::ReadSoundNode(*effect_bytes, node)) {
            it->sound = node;
        }
        const uint32_t kind = file.GetUInt(e, "effectKind");
        for (int cnp = 0; cnp < 2; ++cnp) {
            const fox2::Property* targets = file.FindProperty(e, cnp ? "connectDestinationCnpNames" : "connectDestinationSkelNames");
            for (size_t i = 0; targets && i < targets->Count(); ++i) {
                PartConnection c;
                c.cnp = cnp != 0;
                c.target = file.ElementString(*targets, i);
                c.offset = glm::vec3(file.GetVec4(e, cnp ? "offsetCnpPositions" : "offsetSkelPositions", i));
                c.general = file.GetVec4(e, cnp ? "generalCnpParameters" : "generalSkelParameters", i);
                c.kind = kind;
                it->connections.push_back(std::move(c));
            }
        }
        if (!it->file.empty() && !IsSoundEffect(it->file)) {
            preload_.push_back(it->file);
        }
        LogInfo("vfx: gimmick part {} effect {} ({} connections){}", name, it->file, it->connections.size(),
                it->sound ? " sound " + it->sound->play : std::string());
    }
}

bool VfxScene::PartMatrix(Game& game, size_t index, const PartConnection& c, glm::mat4& out) const {
    const GimmickEntry& entry = gimmicks_[index];
    const Gimmick& g = game.Objects().Gimmicks()[index];
    const std::span<const glm::mat4> skin = game.Demos().Gimmicks().Skin(static_cast<GimmickType>(index));
    auto bone_world = [&](std::string_view name, glm::mat4& m) {
        const int bone = entry.skeleton.FindName(name);
        if (bone < 0) {
            m = g.world;
            return name.empty();
        }
        const glm::mat4 bind = glm::translate(glm::mat4(1.0f), entry.skeleton.bind_world[static_cast<size_t>(bone)]);
        m = g.world * (static_cast<size_t>(bone) < skin.size() ? skin[static_cast<size_t>(bone)] * bind : bind);
        return true;
    };
    glm::mat4 base(1.0f);
    if (!c.cnp) {
        if (!bone_world(c.target, base)) {
            return false;
        }
    } else {
        auto it = std::find_if(entry.connect_points.begin(), entry.connect_points.end(), [&](const ConnectPoint& p) { return p.name == c.target; });
        if (it == entry.connect_points.end()) {
            return false;
        }
        bone_world(it->parent, base);
        base = base * glm::translate(glm::mat4(1.0f), it->translation) * glm::mat4_cast(it->rotation);
    }
    out = base * glm::translate(glm::mat4(1.0f), c.offset);
    if (c.kind == 2) {
        out = out * glm::mat4_cast(glm::quat(c.general.w, c.general.x, c.general.y, c.general.z));
    } else if (c.kind == 1) {
        out[3] += glm::vec4(glm::vec3(c.general), 0.0f);
    }
    return true;
}

void VfxScene::SyncGimmicks(Game& game) {
    for (const GimmickLights& table : kGimmickLights) {
        const size_t index = static_cast<size_t>(table.type);
        const Gimmick& g = game.Objects().Gimmicks()[index];
        GimmickEntry& entry = gimmicks_[index];
        if (entry.parts != g.parts_path) {
            LoadGimmickParts(game, index, g.parts_path);
        }
        const bool visible = g.enabled && g.shown && g.placed;
        const bool restarted = entry.retrigger && g.freezer_timer < entry.timer && !g.freezer_strong;
        entry.timer = g.freezer_timer;
        for (size_t i = 0; i < entry.effects.size(); ++i) {
            PartEffect& part = entry.effects[i];
            const bool want = visible && i < 3 && g.lights[i];
            const bool spawn = want && (!part.on || (i == 0 && restarted));
            for (size_t c = 0; c < part.connections.size(); ++c) {
                const vfx::InstanceKey key{kGimmickOwner | index, i * 16 + c};
                glm::mat4 world(1.0f);
                const bool placed = want && PartMatrix(game, index, part.connections[c], world);
                if (!placed || spawn) {
                    system_.Remove(key);
                    if (c == 0) {
                        EndPartSound(game, part);
                    }
                }
                if (!placed) {
                    continue;
                }
                if (spawn) {
                    const uint32_t seed = part.seed ? part.seed + static_cast<uint32_t>(c) : static_cast<uint32_t>(game.Frame() * 2654435761u) | 1u;
                    system_.Spawn(key, part.file, world, seed);
                    if (c == 0 && part.sound) {
                        part.sound_at = glm::vec3(world[3]);
                        part.sound_id = game.PostSound(part.sound->play, part.sound_at, true);
                    }
                } else {
                    system_.SetTransform(key, world);
                }
            }
            if (want && part.on && !spawn && entry.retrigger && !part.connections.empty() &&
                !system_.IsPlaying({kGimmickOwner | index, i * 16})) {
                game.Objects().SetGimmickLight(table.type, i, false);
                EndPartSound(game, part);
                part.on = false;
                continue;
            }
            if (want != part.on) {
                glm::mat4 at = g.world;
                if (!part.connections.empty()) {
                    PartMatrix(game, index, part.connections[0], at);
                }
                LogDebug("vfx: gimmick {} part {} {} at ({:.2f} {:.2f} {:.2f})", g.name, part.name, want ? "on" : "off", at[3].x, at[3].y, at[3].z);
            }
            part.on = want;
        }
    }
}

void VfxScene::EndPartSound(Game& game, PartEffect& part) {
    if (!part.sound_id || !part.sound) {
        part.sound_id = 0;
        return;
    }
    auto* sound = dynamic_cast<GameSound*>(game.Audio());
    if (sound && sound->Ready() && sound->System().IsPlaying(part.sound_id)) {
        if (part.sound->stop_playing) {
            sound->System().StopPlayingId(part.sound_id, part.sound->fade, static_cast<audio::Interp>(std::min(part.sound->curve, 9u)));
        } else if (!part.sound->stop.empty()) {
            game.PostSound(part.sound->stop, part.sound_at, true);
        }
    }
    part.sound_id = 0;
}

void VfxScene::SyncTest(Game& game) {
    static const std::string spec = [] {
        const char* v = std::getenv("PT_VFX_TEST");
        return std::string(v ? v : "");
    }();
    if (spec.empty() || test_spawned_) {
        return;
    }
    const size_t bar = spec.find('|');
    const std::string path = spec.substr(0, bar);
    glm::vec3 pos = game.GetCamera().position + game.GetCamera().Forward() * 2.0f;
    if (bar != std::string::npos) {
        std::sscanf(spec.c_str() + bar + 1, "%f %f %f", &pos.x, &pos.y, &pos.z);
    }
    test_spawned_ = system_.Spawn({0xDEB0, 1}, path, glm::translate(glm::mat4(1.0f), pos), 1u);
    if (test_spawned_) {
        LogInfo("vfx: test effect {} at ({:.2f} {:.2f} {:.2f})", path, pos.x, pos.y, pos.z);
    }
}

void VfxScene::Update(Game& game, float dt) {
    const auto started = std::chrono::steady_clock::now();
    UpdateSystems(game, dt);
    update_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ++updates_;
}

void VfxScene::UpdateSystems(Game& game, float dt) {
    if (!reader_set_) {
        Vfs* vfs = &game.GetVfs();
        system_.SetReader([vfs](const std::string& path) { return vfs->ReadFile(path); });
        system_.SetModelReader([vfs](uint64_t code) -> std::shared_ptr<const vfx::ModelMesh> {
            for (const auto& package : vfs->LoadedPackages()) {
                for (const FoxPackage::Entry& entry : package->Entries()) {
                    if (!entry.path.ends_with(".fmdl") || StrCode64(entry.path) != code) {
                        continue;
                    }
                    FmdlModel model;
                    const std::vector<uint8_t> bytes = package->Read(entry);
                    if (!LoadFmdl(bytes, entry.path, model)) {
                        return nullptr;
                    }
                    auto mesh = std::make_shared<vfx::ModelMesh>();
                    for (const Vertex& v : model.mesh.vertices) {
                        mesh->positions.push_back(v.position);
                        mesh->uvs.push_back(v.uv0);
                    }
                    for (const SubMesh& sub : model.mesh.submeshes) {
                        for (uint32_t i = 0; i < sub.index_count; ++i) {
                            const uint32_t index = static_cast<uint32_t>(sub.vertex_offset) + model.mesh.indices[sub.first_index + i];
                            mesh->indices.push_back(index < mesh->positions.size() ? index : 0u);
                        }
                    }
                    LogDebug("vfx: model {:#x} is {}", code, entry.path);
                    return mesh;
                }
            }
            return nullptr;
        });
        if (const char* only = std::getenv("PT_VFX_ONLY")) {
            system_.SetFilter(only);
        }
        reader_set_ = true;
    }
    SyncStages(game);
    SyncDemos(game);
    SyncGimmicks(game);
    SyncTest(game);
    if (game.Paused()) {
        return;
    }
    const glm::vec3 wind = Wind(game);
    system_.SetWind(wind);
    anim::SetSimWind(wind);
    system_.Update(dt, View(game.ViewCamera(), 16.0f / 9.0f));
    if (game.Frame() >= logged_frame_ + 600) {
        logged_frame_ = game.Frame();
        std::string top;
        for (const auto& [name, count] : system_.Stats()) {
            if (top.size() < 200) {
                top += std::format(" {} {}", name, count);
            }
        }
        LogDebug("vfx: frame {}, {} instances, {} particles, cpu {:.3f} ms per tick, {:.3f} ms per rendered frame:{}", game.Frame(),
                 system_.InstanceCount(), system_.ParticleCount(), updates_ ? update_seconds_ * 1000.0 / static_cast<double>(updates_) : 0.0,
                 prepares_ ? prepare_seconds_ * 1000.0 / static_cast<double>(prepares_) : 0.0, top);
        update_seconds_ = prepare_seconds_ = 0.0;
        updates_ = prepares_ = 0;
    }
}

void VfxScene::Prepare(Game& game, const Camera& camera, float aspect, SceneLighting& lighting, VfxPass& pass, float blend) {
    const auto started = std::chrono::steady_clock::now();
    const ThreadCost cost_before = QueryThreadCost();
    preload_ms_ = build_ms_ = 0.0;
    PrepareList(game, camera, aspect, lighting, pass, blend);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const ThreadCost cost_after = QueryThreadCost();
    prepare_seconds_ += seconds;
    if (seconds > 0.02) {
        LogInfo("vfx: prepare took {:.1f} ms at frame {} ({} instances; preload {:.1f}, build {:.1f}, {} quads; {:.1f} Mcycles, {} page faults)",
                seconds * 1000.0, game.Frame(), system_.InstanceCount(), preload_ms_, build_ms_, pass.List().quads.size(),
                static_cast<double>(cost_after.cycles - cost_before.cycles) * 1e-6, cost_after.page_faults - cost_before.page_faults);
    }
    ++prepares_;
}

void VfxScene::PrepareList(Game& game, const Camera& camera, float aspect, SceneLighting& lighting, VfxPass& pass, float blend) {
    (void)game;
    static const bool disabled = [] {
        const char* v = std::getenv("PT_VFX");
        return v && v[0] == '0';
    }();
    vfx::RenderList& list = render_list_;
    list.Clear();
    if (disabled) {
        preload_.clear();
        pass.Submit(list);
        return;
    }
    const vfx::ViewInfo view = View(camera, aspect);
    std::vector<vfx::LightOut> lights;
    const SceneLighting* scene = &lighting;
    auto probe = [scene](const glm::vec3& p, bool ambient) {
        vfx::LightingSample s;
        if (ambient) {
            s.ambient = ProbeAmbient(*scene, p);
        } else {
            s.point = PointLighting(*scene, p);
            s.directional = scene->tpp.dir_color;
        }
        return s;
    };
    const vfx::TextureResolver textures = [&pass](const std::string& path) { return pass.Texture(path); };
    const vfx::TextureResolver cubes = [&pass](const std::string& path) { return pass.Cube(path); };
    static const bool legacy_lit = [] {
        const char* v = std::getenv("PT_VFX_LIT_LEGACY");
        return v && v[0] == '1';
    }();
    if (legacy_lit) {
        system_.SetLightBlockProbe({});
    } else {
        system_.SetLightBlockProbe([scene](const glm::vec3& center, const glm::vec3& half) { return LightBlockAt(*scene, center, half); });
    }
    const auto preload_started = std::chrono::steady_clock::now();
    if (!preload_.empty()) {
        std::sort(preload_.begin(), preload_.end());
        preload_.erase(std::unique(preload_.begin(), preload_.end()), preload_.end());
        std::vector<std::string> ahead;
        const vfx::TextureResolver collect = [&ahead](const std::string& path) {
            ahead.push_back(path);
            return TextureManager::kWhite;
        };
        for (const std::string& path : preload_) {
            system_.Preload(path, collect, cubes);
        }
        preload_.clear();
        pass.DecodeAhead(ahead);
    }
    const auto build_started = std::chrono::steady_clock::now();
    system_.Build(view, textures, cubes, probe, list, lights, blend);
    const auto build_done = std::chrono::steady_clock::now();
    preload_ms_ = std::chrono::duration<double, std::milli>(build_started - preload_started).count();
    build_ms_ = std::chrono::duration<double, std::milli>(build_done - build_started).count();
    for (const vfx::LightOut& l : lights) {
        lighting.lights.push_back(ToSceneLight(l));
    }
    if (++prepared_ % 300 == 1) {
        size_t layers[3] = {0, 0, 0};
        for (const vfx::Draw& d : list.draws) {
            layers[static_cast<int>(d.layer)] += d.count;
        }
        LogDebug("vfx: frame quads world {} flare {} screen {}, {} lights", layers[0], layers[1], layers[2], lights.size());
        if (std::getenv("PT_VFX_DEBUG")) {
            static const size_t shown = std::getenv("PT_VFX_DEBUG_QUADS") ? std::strtoul(std::getenv("PT_VFX_DEBUG_QUADS"), nullptr, 10) : 8;
            for (size_t i = 0; i < std::min<size_t>(list.quads.size(), shown); ++i) {
                const vfx::Quad& q = list.quads[i];
                LogDebug("vfx: quad {} ({:.2f} {:.2f} {:.2f}) ({:.2f} {:.2f} {:.2f}) color ({:.3f} {:.3f} {:.3f} {:.3f}) tex {} flags {:#x} "
                         "corners 1 2 ({:.2f} {:.2f} {:.2f}) ({:.2f} {:.2f} {:.2f}) uv ({:.4f} {:.4f} {:.4f} {:.4f}) params ({:.3f} {:.3f} {:.3f} {:.3f})",
                         i, q.corner[0].x, q.corner[0].y, q.corner[0].z, q.corner[3].x, q.corner[3].y, q.corner[3].z, q.color.r, q.color.g,
                         q.color.b, q.color.a, q.info.x, q.info.y, q.corner[1].x, q.corner[1].y, q.corner[1].z, q.corner[2].x, q.corner[2].y,
                         q.corner[2].z, q.uv.x, q.uv.y, q.uv.z, q.uv.w, q.params.x, q.params.y, q.params.z, q.params.w);
            }
        }
    }
    pass.Submit(list);
}

}
