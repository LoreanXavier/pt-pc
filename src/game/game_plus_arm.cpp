#include "game/game_plus_arm.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "engine/core/log.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_renderer.h"

namespace pt::game {
namespace {

constexpr const char* kLisa = "/Assets/sh/chara/och/Scenes/och0_main0_def.fmdl";
constexpr const char* kCreature = "/Assets/sh/chara/hsh/Scenes/hsh0_main0_def.fmdl";
// the arm's length over hsh0's own: 2.33 m from its upper arm joint to the fingertips becomes 1.35 m (hers with the hand is
// 1.0 m), so hanging from her shoulder (1.35 m up) the fingertips reach the floor instead of passing a metre through it
constexpr float kArmLength = 0.58f;
constexpr float kSkinGain = 1.5f;

// a bone name's part after SKL_nnn_ (LUARM, CHEST, LF10, RHMRS_HLP)
std::string_view Part(std::string_view name) {
    const size_t a = name.find('_');
    const size_t b = a == std::string_view::npos ? a : name.find('_', a + 1);
    return b == std::string_view::npos ? name : name.substr(b + 1);
}

bool LegOrHead(std::string_view part) {
    for (std::string_view p : {"LTHIGH", "LLEG", "LFOOT", "LTOE", "LBUD", "LMRF", "LPTL", "RTHIGH", "RLEG", "RFOOT", "RTOE", "RBUD",
                               "RMRF", "RPTL", "NECK", "HEAD", "EYE", "FACE", "MOUTH"}) {
        if (part.starts_with(p)) return true;
    }
    return false;
}

bool RightSide(std::string_view part) {
    return part.starts_with('R');
}

// the arm chain from the upper arm down: arm bones, fingers and the arm's helpers
bool ArmChain(std::string_view part) {
    // the fingers are LF10 to LF53 (LFOOT is the leg's)
    if (part.size() > 2 && part.starts_with("LF") && part[2] >= '0' && part[2] <= '9') return true;
    for (std::string_view p : {"LUARM", "LFARM", "LHAND", "LHMRS", "LUARL", "LELBW", "LFARL", "LTNB", "LTOW"}) {
        if (part.starts_with(p)) return true;
    }
    return false;
}

int Dominant(const Vertex& v) {
    int best = 0;
    for (int k = 1; k < 4; ++k) {
        if (v.weights[k] > v.weights[best]) best = k;
    }
    return v.joints[best];
}

// hsh0's bind space to Lisa's: its torso leans to its left (its chest joint at x 0.131, z 0.015 over its waist, hers straight
// above hers), so the lean is taken out up to its chest height, then the whole is scaled to her size about the waists (both at
// the origin)
glm::vec3 Retarget(const glm::vec3& v, float scale) {
    const float t = std::clamp(v.y / 0.374f, 0.0f, 1.0f);
    return scale * (v - glm::vec3(0.131f * t, 0.0f, 0.015f * t));
}

float Smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// How far inside the creature's part of her a point of her bind space is (metres; negative outside): all round her left side (she
// faces +z), a torn, rib-following boundary: a wavering line a few centimetres either side of 3.5 cm from her sternum in front
// (her left breast and chest are the creature's) and 7 cm from her spine behind, and a lower edge that runs from her hip in front
// and her waist behind at the flank up her ribs toward her middle, with ragged waves; the upper edge is her shoulder line (y 0.47).
float CreatureSide(const glm::vec3& p) {
    // 1 on her front, 0 on her back, blended over her sides
    const float front = Smooth01((p.z + 0.02f) / 0.06f);
    const float phase = 1.7f * (1.0f - front);  // the front's waves are not the back's mirror
    const float middle = 0.07f - 0.035f * front + 0.045f * std::sin(p.y * 17.0f + 1.0f + phase) +
                         0.02f * std::sin(p.y * 43.0f + 2.0f + phase) + 0.01f * std::sin(p.y * 97.0f);
    const float bottom = 0.02f - 0.08f * front + (0.13f - 0.03f * front) * Smooth01((0.20f - p.x) / 0.16f) +
                         0.025f * std::sin(p.x * 55.0f + p.z * 9.0f) + 0.012f * std::sin(p.x * 131.0f);
    return std::min(std::min(p.x - middle, p.y - bottom), 0.47f - p.y);
}

int Find(const std::vector<std::string>& names, std::string_view name) {
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

}

bool GamePlusArm::Prepare(ModelCache& models) {
    if (tried_) return ready_;
    tried_ = true;
    const ModelEntry* lisa = models.Get(kLisa);
    const ModelEntry* full = models.Get(kCreature);
    if (!lisa || !full || !lisa->skinned || !full->skinned) {
        LogWarn("game+: Lisa's hsh0 side needs {} and {}", kLisa, kCreature);
        return false;
    }
    std::vector<std::string_view> lisa_parts;
    for (const std::string& n : lisa->bone_names) lisa_parts.push_back(Part(n));
    std::vector<std::string_view> side_parts;
    for (const std::string& n : full->bone_names) side_parts.push_back(Part(n));
    // her size: waist to neck, over hsh0's
    const int lisa_neck = Find(lisa->bone_names, "SKL_003_NECK");
    const int side_neck = Find(full->bone_names, "SKL_003_NECK");
    if (lisa_neck >= 0 && side_neck >= 0 && full->bone_bind_world[static_cast<size_t>(side_neck)].y > 0.0f) {
        scale_ = lisa->bone_bind_world[static_cast<size_t>(lisa_neck)].y / full->bone_bind_world[static_cast<size_t>(side_neck)].y;
    }
    // hsh0's: left of its middle line (x 0 at the waist to 0.131 at its chest, which leans left), between its hip and neck; its
    // vertices retargeted into her bind space, so its torso is skinned by her own skin matrices with its own weights
    const float scale = scale_;
    // her own skinning of her left side, for hsh0's torso: each of its torso vertices takes the joints and weights of her nearest
    // vertex (bind space), offset past hsh0's bones (the draw's skin has hsh0's bones, then hers), so it moves exactly as her
    // body there does; its arm keeps its own weights on the arm chain
    struct Donor {
        glm::vec3 position;
        glm::vec3 normal;
        glm::u8vec4 joints;
        glm::u8vec4 weights;
    };
    std::vector<Donor> donors;
    MeshData lisa_data;
    if (models.ReadMesh(kLisa, lisa_data)) {
        for (const Vertex& v : lisa_data.vertices) {
            if (v.position.x > -0.05f && v.position.y > -0.25f && v.position.y < 0.75f && v.position.x < 0.6f) donors.push_back({v.position, glm::normalize(v.normal), v.joints, v.weights});
        }
    }
    const int side_bones = static_cast<int>(full->bone_names.size());
    // around its shoulder (20 cm of its upper arm joint) its arm vertices take her weights too: there her skin and the arm chain
    // meet without the shoulder cap tearing
    const int side_upper_arm = Find(full->bone_names, "SKL_011_LUARM");
    const glm::vec3 shoulder = side_upper_arm >= 0 ? Retarget(full->bone_bind_world[static_cast<size_t>(side_upper_arm)], scale) : glm::vec3(1e9f);
    if (donors.empty() || side_bones + static_cast<int>(lisa->bone_names.size()) > 255) {
        LogWarn("game+: Lisa's hsh0 side: no weights to transfer");
        return false;
    }
    const auto side_keep = [side_parts](const Vertex& v) {
        const int d = Dominant(v);
        const std::string_view part = d < static_cast<int>(side_parts.size()) ? side_parts[static_cast<size_t>(d)] : std::string_view();
        const float middle = 0.131f * std::clamp(v.position.y / 0.374f, 0.0f, 1.0f);
        // its thigh's top (to 25 cm below its hip) stays: under her front cut, which reaches her hip
        const bool thigh_top = (part.starts_with("LTHIGH") || part.starts_with("LBUD") || part.starts_with("LMRF") || part.starts_with("LPTL")) &&
                               v.position.y > -0.25f;
        return v.position.x > middle - 0.06f && v.position.y > -0.30f && v.position.y < 0.90f && (!LegOrHead(part) || thigh_top) &&
               !RightSide(part);
    };
    // hsh0's kept vertices as placed in her bind space, with their texture coordinates: her own surface over its part takes
    // the creature's skin at the nearest one's coordinates
    struct SkinPoint {
        glm::vec3 position;
        glm::vec2 uv;
    };
    std::vector<SkinPoint> cover;
    const ModelEntry* side = models.GetSubset(
        kCreature, std::string(kCreature) + "#left_side_on_lisa",
        side_keep,
        [scale, side_parts, &donors, side_bones, shoulder, &cover, &side_keep](Vertex& v) {
            const int d = Dominant(v);
            const std::string_view part = d < static_cast<int>(side_parts.size()) ? side_parts[static_cast<size_t>(d)] : std::string_view();
            const bool covers = side_keep(v) && !ArmChain(part);
            v.position = Retarget(v.position, scale);
            if (ArmChain(part) && glm::distance(v.position, shoulder) > 0.2f) return;
            const Donor* best = nullptr;
            float best_d = 1e30f;
            for (const Donor& n : donors) {
                const glm::vec3 e = n.position - v.position;
                const float dist = glm::dot(e, e);
                if (dist < best_d) {
                    best_d = dist;
                    best = &n;
                }
            }
            if (!best) return;
            // its torso is slimmer than hers: no deeper than 1.2 cm below her surface (her nearest vertex's plane), so it fills
            // her shape where her body is cut away; under her cloth (outside the cut) exactly 1.2 cm below, so the cloth lies
            // over it and a tear shows it
            {
                const float depth = glm::dot(best->position - v.position, best->normal);
                const float want = CreatureSide(v.position) < 0.0f ? depth : std::max(depth, 0.012f);
                if (want > 0.012f || CreatureSide(v.position) < 0.0f) v.position += best->normal * (want - 0.012f);
            }
            if (covers) cover.push_back({v.position, v.uv0});
            for (int k = 0; k < 4; ++k) {
                v.joints[k] = static_cast<uint8_t>(best->joints[k] + side_bones);
                v.weights[k] = best->weights[k];
            }
        });
    // the creature's skin material: its part's largest submesh
    uint32_t skin_material = 0;
    if (side) {
        uint32_t most = 0;
        for (const SubMesh& sub : side->mesh->submeshes) {
            if (sub.index_count > most) {
                most = sub.index_count;
                skin_material = sub.material;
            }
        }
    }
    // her surface is never cut (only her left arm, replaced by hsh0's), so no gap ever opens onto the room. Over her left side
    // (CreatureSide) it sinks 2.5 cm, under hsh0's part (no deeper than 1.2 cm below her surface), and is drawn with the
    // creature's skin; her cloth within 9 cm of it tears to the creature's skin by a ragged noise (gbuffer.frag TearNoise). The
    // vertex colour carries it: r the creature's skin material, g and b its texture coordinates at the nearest hsh0 vertex,
    // a 1 in her cloth falling to 0 at the cut, negative over the cut
    const ModelEntry* cut = models.GetSubset(
        kLisa, std::string(kLisa) + "#creature_left_side",
        [lisa_parts](const Vertex& v) {
            const int d = Dominant(v);
            const std::string_view part = d < static_cast<int>(lisa_parts.size()) ? lisa_parts[static_cast<size_t>(d)] : std::string_view();
            return !ArmChain(part);
        },
        [&cover, lisa_parts, skin_material](Vertex& v) {
            const float d = CreatureSide(v.position);
            v.color = glm::vec4(static_cast<float>(skin_material), 0.0f, 0.0f, 1.0f);
            if (d <= -0.12f) return;
            const SkinPoint* nearest = nullptr;
            float nearest_d = 1e30f;
            for (const SkinPoint& c : cover) {
                const float e = glm::dot(c.position - v.position, c.position - v.position);
                if (e < nearest_d) {
                    nearest_d = e;
                    nearest = &c;
                }
            }
            if (nearest) {
                v.color.g = nearest->uv.x;
                v.color.b = nearest->uv.y;
            }
            if (d <= -0.09f) return;
            const int bone = Dominant(v);
            const std::string_view part = bone < static_cast<int>(lisa_parts.size()) ? lisa_parts[static_cast<size_t>(bone)] : std::string_view();
            if (d > 0.0f && !LegOrHead(part)) {
                v.position -= glm::normalize(v.normal) * 0.025f;
                v.color.a = -1.0f;
                return;
            }
            v.color.a = std::clamp(-d / 0.09f, 0.0f, 1.0f);
        });
    if (!cut || !side) return false;
    lisa_mesh_ = lisa->mesh.get();
    lisa_cut_ = cut->mesh.get();
    side_mesh_ = side->mesh.get();
    lisa_bind_ = lisa->bone_bind_world;
    side_bind_ = full->bone_bind_world;
    side_parent_.assign(full->bone_parents.begin(), full->bone_parents.end());
    side_source_.resize(full->bone_names.size(), -1);
    side_arm_.resize(full->bone_names.size(), false);
    for (size_t i = 0; i < full->bone_names.size(); ++i) {
        int source = Find(lisa->bone_names, full->bone_names[i]);
        for (int p = side_parent_[i]; source < 0 && p >= 0; p = side_parent_[static_cast<size_t>(p)]) {
            source = Find(lisa->bone_names, full->bone_names[static_cast<size_t>(p)]);
        }
        side_source_[i] = source;
        side_arm_[i] = ArmChain(side_parts[i]);
    }
    side_retarget_.resize(side_bind_.size());
    for (size_t i = 0; i < side_bind_.size(); ++i) side_retarget_[i] = Retarget(side_bind_[i], scale_);
    ready_ = lisa_mesh_ && lisa_cut_ && side_mesh_;
    LogInfo("game+: Lisa's hsh0 left side ready ({} of hsh0's bones mapped, torso scale {:.2f})", side_source_.size(), scale_);
    return ready_;
}

void GamePlusArm::Apply(std::vector<DrawItem>& out) {
    skins_.clear();
    if (!ready_) return;
    const size_t count = out.size();
    for (size_t n = 0; n < count; ++n) {
        if (out[n].mesh != lisa_mesh_ || out[n].skin.size() < lisa_bind_.size()) continue;
        const std::span<const glm::mat4> skin = out[n].skin;
        // her bones' world matrices in model space: skin = world * T(-bind)
        auto lisa_world = [&](int i) { return skin[static_cast<size_t>(i)] * glm::translate(glm::mat4(1.0f), lisa_bind_[static_cast<size_t>(i)]); };
        std::vector<glm::mat4> world(side_bind_.size(), glm::mat4(1.0f));
        // hsh0's bones, then hers (its torso vertices carry her joints, offset past its bones)
        std::vector<glm::mat4>& side_skin = skins_.emplace_back(side_bind_.size() + lisa_bind_.size());
        for (size_t j = 0; j < lisa_bind_.size(); ++j) side_skin[side_bind_.size() + j] = skin[j];
        // the arm's vertices are in her bind space scaled by s; along the arm they take kArmLength of hsh0's length
        const glm::mat4 arm_scale = glm::scale(glm::mat4(1.0f), glm::vec3(kArmLength / scale_, 1.0f, 1.0f));
        for (size_t i = 0; i < side_bind_.size(); ++i) {
            const int source = side_source_[i];
            const int p = side_parent_[i];
            if (side_arm_[i] && p >= 0) {
                // the arm chain: her rotation; from her upper arm joint, then hsh0's offsets at kArmLength
                const glm::mat4 lisa = source >= 0 ? lisa_world(source) : glm::mat4(1.0f);
                world[i] = glm::mat4(glm::mat3(lisa));
                if (side_arm_[static_cast<size_t>(p)]) {
                    const glm::mat4& parent = world[static_cast<size_t>(p)];
                    world[i][3] = glm::vec4(glm::vec3(parent[3]) + glm::mat3(parent) * (kArmLength * (side_bind_[i] - side_bind_[static_cast<size_t>(p)])), 1.0f);
                } else {
                    // the chain's root where her own skin puts hsh0's upper arm joint: the arm and her skinning of the shoulder
                    // around it agree, so the shoulder does not tear when her arm moves
                    world[i][3] = skin[static_cast<size_t>(source)] * glm::vec4(side_retarget_[i], 1.0f);
                }
                side_skin[i] = world[i] * arm_scale * glm::translate(glm::mat4(1.0f), -side_retarget_[i]);
            } else {
                // a torso bone: her own skin matrix (the vertices are in her bind space)
                world[i] = source >= 0 ? lisa_world(source) : glm::mat4(1.0f);
                side_skin[i] = source >= 0 ? glm::mat4(skin[static_cast<size_t>(source)]) : glm::mat4(1.0f);
            }
        }
        // PT_GAMEPLUS_SKIN_DUMP=<file>: her skin matrices of this frame, appended (the offline check, research/proto_side.py)
        static const char* dump = std::getenv("PT_GAMEPLUS_SKIN_DUMP");
        if (dump && *dump) {
            if (FILE* file = std::fopen(dump, "ab")) {
                const uint32_t count = static_cast<uint32_t>(skin.size());
                std::fwrite(&count, sizeof(count), 1, file);
                std::fwrite(skin.data(), sizeof(glm::mat4), skin.size(), file);
                std::fclose(file);
            }
        }
        out[n].mesh = lisa_cut_;
        // her cut's cloth tears near the cut (gbuffer.frag): tint.a above 1.5 turns the tear on, the fade stays 1
        const glm::vec4 lisa_tint = out[n].tint;
        out[n].tint.a = 2.0f;
        DrawItem side = out[n];
        side.mesh = side_mesh_;
        side.skin = side_skin;
        side.hidden_meshes = {};
        // hsh0's skin texture is far darker than hers (median sRGB 0.27 against her skin's 0.68): brightened to read as one body
        side.tint = glm::vec4(glm::vec3(lisa_tint) * kSkinGain, lisa_tint.a);
        out.push_back(side);
    }
}

}
