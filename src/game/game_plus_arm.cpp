#include "game/game_plus_arm.h"

#include <glm/gtc/matrix_transform.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "engine/core/log.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_renderer.h"

namespace pt::game {
namespace {

constexpr const char* kLisa = "/Assets/sh/chara/och/Scenes/och0_main0_def.fmdl";
constexpr const char* kCreature = "/Assets/sh/chara/hsh/Scenes/hsh0_main0_def.fmdl";
constexpr const char* kStaticLisa = "/Assets/sh/environ/object/shsb/house/shsb_hous001/scenes/shsb_hous001_ocho001.fmdl";
constexpr float kSkinGain = 1.5f;
constexpr float kShoulderCap = 0.11f;
constexpr float kShoulderBand = 0.20f;
constexpr float kShiftInner = 0.08f;
constexpr float kShiftOuter = 0.22f;
constexpr int kDonors = 6;

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

bool ArmChain(std::string_view part) {
    if (part.size() > 2 && part.starts_with("LF") && part[2] >= '0' && part[2] <= '9') return true;
    for (std::string_view p : {"LUARM", "LFARM", "LHAND", "LHMRS", "LUARL", "LELBW", "LFARL", "LTNB", "LTOW"}) {
        if (part.starts_with(p)) return true;
    }
    return false;
}

bool NeckOrHead(std::string_view part) {
    for (std::string_view p : {"NECK", "HEAD", "EYE", "FACE", "MOUTH"}) {
        if (part.starts_with(p)) return true;
    }
    return false;
}

int ArmRegion(std::string_view part) {
    for (std::string_view p : {"LUARM", "LHMRS", "LUARL"}) {
        if (part.starts_with(p)) return 1;
    }
    for (std::string_view p : {"LFARM", "LELBW", "LFARL", "LTNB"}) {
        if (part.starts_with(p)) return 2;
    }
    return 3;
}

int Dominant(const Vertex& v) {
    int best = 0;
    for (int k = 1; k < 4; ++k) {
        if (v.weights[k] > v.weights[best]) best = k;
    }
    return v.joints[best];
}

glm::vec3 Retarget(const glm::vec3& v, float scale) {
    const float t = std::clamp(v.y / 0.374f, 0.0f, 1.0f);
    return scale * (v - glm::vec3(0.131f * t, 0.0f, 0.015f * t));
}

float Smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float CreatureSide(const glm::vec3& p) {
    const float front = Smooth01((p.z + 0.02f) / 0.06f);
    const float phase = 1.7f * (1.0f - front);
    const float middle = 0.07f - 0.035f * front + 0.045f * std::sin(p.y * 17.0f + 1.0f + phase) +
                         0.02f * std::sin(p.y * 43.0f + 2.0f + phase) + 0.01f * std::sin(p.y * 97.0f);
    const float bottom = 0.02f - 0.08f * front + (0.13f - 0.03f * front) * Smooth01((0.20f - p.x) / 0.16f) +
                         0.025f * std::sin(p.x * 55.0f + p.z * 9.0f) + 0.012f * std::sin(p.x * 131.0f);
    return std::min(std::min(p.x - middle, p.y - bottom), 0.47f - p.y);
}

float CreatureSkin(const glm::vec3& p) {
    const float edge = 0.125f + 0.012f * std::sin(p.z * 70.0f + 0.5f) + 0.008f * std::sin(p.y * 110.0f);
    return std::max(CreatureSide(p), std::min(p.x - edge, p.y - 0.38f));
}

int Find(const std::vector<std::string>& names, std::string_view name) {
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

glm::mat4 FitRigid(const std::vector<glm::vec3>& a, const std::vector<glm::vec3>& b) {
    glm::dvec3 ca(0.0), cb(0.0);
    for (size_t i = 0; i < a.size(); ++i) {
        ca += glm::dvec3(a[i]);
        cb += glm::dvec3(b[i]);
    }
    ca /= static_cast<double>(a.size());
    cb /= static_cast<double>(b.size());
    double s[3][3] = {};
    for (size_t i = 0; i < a.size(); ++i) {
        const glm::dvec3 p = glm::dvec3(a[i]) - ca;
        const glm::dvec3 q = glm::dvec3(b[i]) - cb;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) s[r][c] += p[r] * q[c];
    }
    double n[4][4] = {{s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0]},
                      {s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2]},
                      {s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1]},
                      {s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2]}};
    double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    for (int sweep = 0; sweep < 32; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q) off += n[p][q] * n[p][q];
        if (off < 1e-20) break;
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                if (std::abs(n[p][q]) < 1e-30) continue;
                const double theta = (n[q][q] - n[p][p]) / (2.0 * n[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double sn = t * c;
                for (int k = 0; k < 4; ++k) {
                    const double kp = n[k][p], kq = n[k][q];
                    n[k][p] = c * kp - sn * kq;
                    n[k][q] = sn * kp + c * kq;
                }
                for (int k = 0; k < 4; ++k) {
                    const double pk = n[p][k], qk = n[q][k];
                    n[p][k] = c * pk - sn * qk;
                    n[q][k] = sn * pk + c * qk;
                }
                for (int k = 0; k < 4; ++k) {
                    const double kp = v[k][p], kq = v[k][q];
                    v[k][p] = c * kp - sn * kq;
                    v[k][q] = sn * kp + c * kq;
                }
            }
        }
    }
    int best = 0;
    for (int k = 1; k < 4; ++k) {
        if (n[k][k] > n[best][best]) best = k;
    }
    const glm::dquat rotation = glm::normalize(glm::dquat(v[0][best], v[1][best], v[2][best], v[3][best]));
    glm::dmat4 m = glm::mat4_cast(rotation);
    m[3] = glm::dvec4(cb - glm::dmat3(m) * ca, 1.0);
    return glm::mat4(m);
}

}

bool GamePlusArm::Prepare(ModelCache& models) {
    if (tried_) {
        if (ready_ && !static_fitted_ && ++static_probe_ % 120 == 0 && models.Paths().contains(kStaticLisa)) {
            static_fitted_ = true;
            FitStaticPose(models);
        }
        return ready_;
    }
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
    const int lisa_neck = Find(lisa->bone_names, "SKL_003_NECK");
    const int side_neck = Find(full->bone_names, "SKL_003_NECK");
    if (lisa_neck >= 0 && side_neck >= 0 && full->bone_bind_world[static_cast<size_t>(side_neck)].y > 0.0f) {
        scale_ = lisa->bone_bind_world[static_cast<size_t>(lisa_neck)].y / full->bone_bind_world[static_cast<size_t>(side_neck)].y;
    }
    const float scale = scale_;
    struct Donor {
        glm::vec3 position;
        glm::vec3 normal;
        glm::u8vec4 joints;
        glm::u8vec4 weights;
    };
    const int lisa_upper_arm = Find(lisa->bone_names, "SKL_011_LUARM");
    const float shoulder_cap = lisa_upper_arm >= 0 ? lisa->bone_bind_world[static_cast<size_t>(lisa_upper_arm)].x + kShoulderCap : -1e9f;
    const auto lisa_part = [lisa_parts](const Vertex& v) {
        const int d = Dominant(v);
        return d < static_cast<int>(lisa_parts.size()) ? lisa_parts[static_cast<size_t>(d)] : std::string_view();
    };
    const auto lisa_kept = [lisa_part, shoulder_cap](const Vertex& v) {
        const std::string_view part = lisa_part(v);
        const bool cap = (part.starts_with("LUARM") || part.starts_with("LHMRS") || part.starts_with("LUARL")) && v.position.x < shoulder_cap;
        return !ArmChain(part) || cap;
    };
    std::vector<Donor> donors;
    MeshData lisa_data;
    if (models.ReadMesh(kLisa, lisa_data)) {
        for (const Vertex& v : lisa_data.vertices) {
            if (v.position.x > -0.05f && v.position.y > -0.25f && v.position.y < 0.75f && v.position.x < 0.6f && lisa_kept(v) && !NeckOrHead(lisa_part(v))) {
                donors.push_back({v.position, glm::normalize(v.normal), v.joints, v.weights});
            }
        }
    }
    const int side_bones = static_cast<int>(full->bone_names.size());
    const int side_upper_arm = Find(full->bone_names, "SKL_011_LUARM");
    const glm::vec3 side_shoulder = side_upper_arm >= 0 ? Retarget(full->bone_bind_world[static_cast<size_t>(side_upper_arm)], scale) : glm::vec3(1e9f);
    shift_ = side_upper_arm >= 0 && lisa_upper_arm >= 0 ? lisa->bone_bind_world[static_cast<size_t>(lisa_upper_arm)] - side_shoulder : glm::vec3(0.0f);
    const glm::vec3 shoulder = side_shoulder + shift_;
    const glm::vec3 shift = shift_;
    if (donors.empty() || side_bones + static_cast<int>(lisa->bone_names.size()) > 255) {
        LogWarn("game+: Lisa's hsh0 side: no weights to transfer");
        return false;
    }
    const auto side_keep = [side_parts](const Vertex& v) {
        const int d = Dominant(v);
        const std::string_view part = d < static_cast<int>(side_parts.size()) ? side_parts[static_cast<size_t>(d)] : std::string_view();
        const float middle = 0.131f * std::clamp(v.position.y / 0.374f, 0.0f, 1.0f);
        const bool thigh_top = (part.starts_with("LTHIGH") || part.starts_with("LBUD") || part.starts_with("LMRF") || part.starts_with("LPTL")) &&
                               v.position.y > -0.25f;
        return v.position.x > middle - 0.06f && v.position.y > -0.30f && (ArmChain(part) || v.position.y < 0.90f) && (!LegOrHead(part) || thigh_top) &&
               !RightSide(part);
    };
    const ModelEntry* side = models.GetSubset(
        kCreature, std::string(kCreature) + "#left_side_on_lisa",
        side_keep,
        [scale, side_parts, &donors, side_bones, side_shoulder, shoulder, shift](Vertex& v) {
            const int d = Dominant(v);
            const std::string_view part = d < static_cast<int>(side_parts.size()) ? side_parts[static_cast<size_t>(d)] : std::string_view();
            const bool arm = ArmChain(part);
            v.position = Retarget(v.position, scale);
            const float moved = arm ? 1.0f : 1.0f - Smooth01((glm::distance(v.position, side_shoulder) - kShiftInner) / (kShiftOuter - kShiftInner));
            v.position += shift * moved;
            const float t = arm ? Smooth01((v.position.x - shoulder.x) / kShoulderBand) : 0.0f;
            if (t >= 1.0f) return;
            std::array<const Donor*, kDonors> nearest{};
            std::array<float, kDonors> nearest_d;
            nearest_d.fill(1e30f);
            for (const Donor& n : donors) {
                const glm::vec3 e = n.position - v.position;
                const float dist = glm::dot(e, e);
                if (dist >= nearest_d[kDonors - 1]) continue;
                int k = kDonors - 1;
                for (; k > 0 && nearest_d[k - 1] > dist; --k) {
                    nearest_d[k] = nearest_d[k - 1];
                    nearest[k] = nearest[k - 1];
                }
                nearest_d[k] = dist;
                nearest[k] = &n;
            }
            if (!nearest[0]) return;
            const Donor& best = *nearest[0];
            {
                const float depth = glm::dot(best.position - v.position, best.normal);
                const float side = CreatureSide(v.position);
                const float want = side < 0.0f ? depth : std::max(depth, 0.012f);
                if (want > 0.012f || side < 0.0f) v.position += best.normal * ((want - 0.012f) * (1.0f - t));
            }
            std::array<float, 256> acc{};
            float total = 0.0f;
            std::array<float, kDonors> share{};
            for (int k = 0; k < kDonors && nearest[k]; ++k) {
                const float dist = std::max(std::sqrt(nearest_d[k]), 0.004f);
                share[k] = 1.0f / (dist * dist);
                total += share[k];
            }
            for (int k = 0; k < kDonors && nearest[k]; ++k) {
                for (int j = 0; j < 4; ++j) {
                    acc[static_cast<size_t>(nearest[k]->joints[j] + side_bones)] += share[k] / total * (1.0f - t) * nearest[k]->weights[j];
                }
            }
            for (int j = 0; j < 4; ++j) acc[v.joints[j]] += t * v.weights[j];
            std::array<int, 4> top{0, 0, 0, 0};
            std::array<float, 4> top_w{0.0f, 0.0f, 0.0f, 0.0f};
            for (int j = 0; j < 256; ++j) {
                const float w = acc[static_cast<size_t>(j)];
                if (w <= top_w[3]) continue;
                int k = 3;
                for (; k > 0 && top_w[k - 1] < w; --k) {
                    top_w[k] = top_w[k - 1];
                    top[k] = top[k - 1];
                }
                top_w[k] = w;
                top[k] = j;
            }
            const float sum = top_w[0] + top_w[1] + top_w[2] + top_w[3];
            int given = 0;
            for (int k = 0; k < 4; ++k) {
                const int w = sum > 0.0f ? static_cast<int>(255.0f * top_w[k] / sum) : 0;
                v.joints[k] = static_cast<uint8_t>(top[k]);
                v.weights[k] = static_cast<uint8_t>(w);
                given += w;
            }
            v.weights[0] = static_cast<uint8_t>(v.weights[0] + (255 - given));
        });
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
    const auto key = [](const Vertex& v) {
        uint32_t bits[5];
        std::memcpy(bits, &v.position, sizeof(float) * 3);
        std::memcpy(bits + 3, &v.uv0, sizeof(float) * 2);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t b : bits) h = (h ^ b) * 1099511628211ull;
        return h;
    };
    std::unordered_map<uint64_t, float> uv_scale;
    {
        const auto areas = [](const Vertex& a, const Vertex& b, const Vertex& c, double& uv, double& space, float size) {
            space += glm::length(glm::cross(b.position - a.position, c.position - a.position)) * size * size;
            const glm::vec2 e = b.uv0 - a.uv0, f = c.uv0 - a.uv0;
            uv += std::abs(e.x * f.y - e.y * f.x);
        };
        const auto triangles = [](const MeshData& m, const std::function<void(const SubMesh&, const Vertex&, const Vertex&, const Vertex&)>& each) {
            for (const SubMesh& sub : m.submeshes) {
                for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                    const auto at = [&](uint32_t k) -> const Vertex& {
                        return m.vertices[static_cast<size_t>(static_cast<int64_t>(m.indices[sub.first_index + i + k]) + sub.vertex_offset)];
                    };
                    each(sub, at(0), at(1), at(2));
                }
            }
        };
        double side_uv = 0.0, side_space = 0.0;
        MeshData side_data;
        if (models.ReadMesh(kCreature, side_data)) {
            triangles(side_data, [&](const SubMesh&, const Vertex& a, const Vertex& b, const Vertex& c) {
                if (side_keep(a) && side_keep(b) && side_keep(c)) areas(a, b, c, side_uv, side_space, scale);
            });
        }
        std::map<uint32_t, std::pair<double, double>> hers;
        triangles(lisa_data, [&](const SubMesh& sub, const Vertex& a, const Vertex& b, const Vertex& c) {
            if (std::max({CreatureSkin(a.position), CreatureSkin(b.position), CreatureSkin(c.position)}) > -0.15f) {
                auto& [uv, space] = hers[sub.material];
                areas(a, b, c, uv, space, 1.0f);
            }
        });
        const double side_density = side_space > 0.0 ? std::sqrt(side_uv / side_space) : 0.0;
        for (const SubMesh& sub : lisa_data.submeshes) {
            const auto it = hers.find(sub.material);
            const double density = it != hers.end() && it->second.second > 0.0 ? std::sqrt(it->second.first / it->second.second) : 0.0;
            const float factor = density > 0.0 && side_density > 0.0 ? static_cast<float>(side_density / density) : 1.0f;
            for (uint32_t i = 0; i < sub.index_count; ++i) {
                uv_scale.emplace(key(lisa_data.vertices[static_cast<size_t>(static_cast<int64_t>(lisa_data.indices[sub.first_index + i]) + sub.vertex_offset)]),
                                 factor);
            }
        }
        LogInfo("game+: Lisa's creature skin: hsh0's texture at {:.3f} per metre on her", side_density);
    }
    const ModelEntry* cut = models.GetSubset(
        kLisa, std::string(kLisa) + "#creature_left_side", lisa_kept,
        [lisa_parts, skin_material, &uv_scale, &key](Vertex& v) {
            const auto scaled = uv_scale.find(key(v));
            const glm::vec2 uv = v.uv0 * (scaled != uv_scale.end() ? scaled->second : 1.0f) / glm::vec2(0.46f, 0.48f);
            v.color = glm::vec4(static_cast<float>(skin_material), uv.x, uv.y, 1.0f);
            const int bone = Dominant(v);
            const std::string_view part = bone < static_cast<int>(lisa_parts.size()) ? lisa_parts[static_cast<size_t>(bone)] : std::string_view();
            const bool head = LegOrHead(part);
            const float d = CreatureSide(v.position);
            if (d > 0.0f && !head) {
                v.position -= glm::normalize(v.normal) * 0.025f;
                v.color.a = -1.0f;
                return;
            }
            const float skin = head ? d : CreatureSkin(v.position);
            v.color.a = std::clamp(-skin / 0.09f, 0.0f, 1.0f);
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
    for (size_t i = 0; i < side_bind_.size(); ++i) side_retarget_[i] = Retarget(side_bind_[i], scale_) + (side_arm_[i] ? shift_ : glm::vec3(0.0f));
    const auto segment = [&](const char* a, const char* b) {
        const int la = Find(lisa->bone_names, a), lb = Find(lisa->bone_names, b);
        const int ha = Find(full->bone_names, a), hb = Find(full->bone_names, b);
        if (la < 0 || lb < 0 || ha < 0 || hb < 0) return 0.0f;
        const float theirs = glm::distance(full->bone_bind_world[static_cast<size_t>(ha)], full->bone_bind_world[static_cast<size_t>(hb)]);
        return theirs > 0.0f ? glm::distance(lisa->bone_bind_world[static_cast<size_t>(la)], lisa->bone_bind_world[static_cast<size_t>(lb)]) / theirs : 0.0f;
    };
    const float lengths[3] = {segment("SKL_011_LUARM", "SKL_012_LFARM"), segment("SKL_012_LFARM", "SKL_013_LHAND"),
                              segment("SKL_013_LHAND", "SKL_109_LF33")};
    if (lengths[0] <= 0.0f || lengths[1] <= 0.0f || lengths[2] <= 0.0f) {
        LogWarn("game+: Lisa's hsh0 side: arm bones missing");
        return false;
    }
    side_region_.assign(full->bone_names.size(), 0);
    for (size_t i = 0; i < full->bone_names.size(); ++i) {
        if (side_arm_[i]) side_region_[i] = ArmRegion(side_parts[i]);
    }
    std::copy(std::begin(lengths), std::end(lengths), arm_lengths_);
    ready_ = lisa_mesh_ && lisa_cut_ && side_mesh_;
    LogInfo("game+: Lisa's hsh0 left side ready ({} of hsh0's bones mapped, torso scale {:.2f}, arm {:.3f} {:.3f} {:.3f} of hsh0's)", side_source_.size(),
            scale_, lengths[0], lengths[1], lengths[2]);
    return ready_;
}

LisaPoseFit FitLisaStaticPose(ModelCache& models, std::string_view baked_path,
                              std::span<const std::pair<std::string_view, std::string_view>> materials, double min_share, double max_mean,
                              std::string_view what) {
    LisaPoseFit result;
    const std::string still_path(baked_path);
    const ModelEntry* lisa = models.Get(kLisa);
    const ModelEntry* still = models.Get(still_path);
    MeshData lisa_data, still_data;
    if (!lisa || !still || !still->mesh || !models.ReadMesh(kLisa, lisa_data) || !models.ReadMesh(still_path, still_data)) {
        LogWarn("game+: {}'s pose: {} not readable", what, still_path);
        return result;
    }
    const auto material_name = [](const ModelEntry& e, const SubMesh& s) -> std::string_view {
        return s.material < e.materials.size() ? std::string_view(e.materials[s.material].name) : std::string_view();
    };
    std::map<std::string_view, std::multimap<std::pair<int, int>, uint32_t>> grid;
    for (const SubMesh& s : still_data.submeshes) {
        auto& cells = grid[material_name(*still, s)];
        std::vector<bool> seen(still_data.vertices.size(), false);
        for (uint32_t i = 0; i < s.index_count; ++i) {
            const uint32_t v = still_data.indices[s.first_index + i] + static_cast<uint32_t>(s.vertex_offset);
            if (v >= still_data.vertices.size() || seen[v]) continue;
            seen[v] = true;
            const glm::vec2 uv = still_data.vertices[v].uv0 * 1024.0f;
            cells.emplace(std::make_pair(static_cast<int>(std::lround(uv.x)), static_cast<int>(std::lround(uv.y))), v);
        }
    }
    const size_t count = lisa_data.vertices.size();
    std::vector<std::vector<glm::vec3>> candidates(count);
    for (const SubMesh& s : lisa_data.submeshes) {
        const std::string_view mine = material_name(*lisa, s);
        std::string_view theirs;
        for (const auto& [a, b] : materials) {
            if (a == mine) theirs = b;
        }
        if (materials.empty()) {
            size_t best = 0;
            for (const auto& [name, cells] : grid) {
                size_t hits = 0;
                for (uint32_t i = 0; i < s.index_count; i += 3) {
                    const uint32_t v = lisa_data.indices[s.first_index + i] + static_cast<uint32_t>(s.vertex_offset);
                    if (v >= count) continue;
                    const glm::vec2 uv = lisa_data.vertices[v].uv0 * 1024.0f;
                    const auto [lo, hi] = cells.equal_range({static_cast<int>(std::lround(uv.x)), static_cast<int>(std::lround(uv.y))});
                    for (auto it = lo; it != hi; ++it) {
                        if (glm::distance(still_data.vertices[it->second].uv0, lisa_data.vertices[v].uv0) < 6.0e-4f) {
                            ++hits;
                            break;
                        }
                    }
                }
                if (hits > best) {
                    best = hits;
                    theirs = name;
                }
            }
            static thread_local std::string logged;
            const std::string line = std::format("{}/{}/{}", what, mine, theirs);
            if (!theirs.empty() && logged.find(line) == std::string::npos) {
                logged += line + ";";
                LogInfo("game+: {}'s pose: her material {} pairs with {}", what, mine, theirs);
            }
        }
        const auto cells = grid.find(theirs);
        if (cells == grid.end()) continue;
        for (uint32_t i = 0; i < s.index_count; ++i) {
            const uint32_t v = lisa_data.indices[s.first_index + i] + static_cast<uint32_t>(s.vertex_offset);
            if (v >= count || !candidates[v].empty()) continue;
            const glm::vec2 uv = lisa_data.vertices[v].uv0 * 1024.0f;
            const int x = static_cast<int>(std::lround(uv.x)), y = static_cast<int>(std::lround(uv.y));
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    const auto [lo, hi] = cells->second.equal_range({x + dx, y + dy});
                    for (auto it = lo; it != hi; ++it) {
                        if (glm::distance(still_data.vertices[it->second].uv0, lisa_data.vertices[v].uv0) < 6.0e-4f) {
                            candidates[v].push_back(still_data.vertices[it->second].position);
                        }
                    }
                }
            }
        }
    }
    const size_t bones = lisa->bone_names.size();
    std::vector<std::vector<size_t>> members(bones);
    for (size_t v = 0; v < count; ++v) {
        const Vertex& x = lisa_data.vertices[v];
        const int d = Dominant(x);
        if (*std::max_element(&x.weights[0], &x.weights[0] + 4) >= 128 && !candidates[v].empty() && d >= 0 && static_cast<size_t>(d) < bones) {
            members[static_cast<size_t>(d)].push_back(v);
        }
    }
    std::vector<glm::mat4> fit(bones, glm::mat4(1.0f));
    std::vector<bool> done(bones, false);
    {
        std::vector<glm::vec3> a, b;
        for (size_t root : {size_t{0}, size_t{1}}) {
            for (size_t v : root < bones ? members[root] : std::vector<size_t>{}) {
                if (candidates[v].size() != 1) continue;
                a.push_back(lisa_data.vertices[v].position);
                b.push_back(candidates[v][0]);
            }
        }
        if (a.size() < 20 || bones < 2) {
            LogWarn("game+: {}'s pose: {} trunk points", what, a.size());
            return result;
        }
        fit[0] = fit[1] = FitRigid(a, b);
        done[0] = done[1] = true;
    }
    const std::function<void(size_t)> solve = [&](size_t b) {
        if (done[b]) return;
        done[b] = true;
        const int parent = b < lisa->bone_parents.size() ? lisa->bone_parents[b] : -1;
        const bool has_parent = parent >= 0 && static_cast<size_t>(parent) < bones;
        if (has_parent) solve(static_cast<size_t>(parent));
        glm::mat4 m = has_parent ? fit[static_cast<size_t>(parent)] : fit[0];
        if (members[b].size() >= 6) {
            std::vector<glm::vec3> a, c;
            for (int round = 0; round < 4; ++round) {
                a.clear();
                c.clear();
                for (size_t v : members[b]) {
                    const glm::vec3 p = lisa_data.vertices[v].position;
                    const glm::vec3 guess(m * glm::vec4(p, 1.0f));
                    const glm::vec3* nearest = &candidates[v][0];
                    for (const glm::vec3& q : candidates[v]) {
                        if (glm::distance(q, guess) < glm::distance(*nearest, guess)) nearest = &q;
                    }
                    a.push_back(p);
                    c.push_back(*nearest);
                }
                m = FitRigid(a, c);
            }
        }
        fit[b] = m;
    };
    for (size_t b = 0; b < bones; ++b) solve(b);
    double error = 0.0;
    size_t matched = 0;
    for (size_t v = 0; v < count; ++v) {
        if (candidates[v].empty()) continue;
        const Vertex& x = lisa_data.vertices[v];
        glm::vec3 p(0.0f);
        for (int k = 0; k < 4; ++k) {
            if (x.joints[k] < bones) p += (x.weights[k] / 255.0f) * glm::vec3(fit[x.joints[k]] * glm::vec4(x.position, 1.0f));
        }
        float best = 1e9f;
        for (const glm::vec3& q : candidates[v]) best = std::min(best, glm::distance(p, q));
        error += best;
        ++matched;
    }
    const double mean = matched ? error / static_cast<double>(matched) : 1.0;
    LogInfo("game+: {}'s pose fitted: {} of {} vertices matched, {:.1f} mm mean", what, matched, count, mean * 1000.0);
    result.matched = matched;
    result.count = count;
    result.mean = mean;
    if (static_cast<double>(matched) < static_cast<double>(count) * min_share || mean > max_mean) return result;
    result.skin = std::move(fit);
    result.ok = true;
    return result;
}

void GamePlusArm::FitStaticPose(ModelCache& models) {
    static constexpr std::pair<std::string_view, std::string_view> kMaterials[] = {
        {"heel", "urethane_coat_a_nr"}, {"fabric_mid_glossy", "fabric_mid_glossy2"}, {"hair", "skin_white_male2"}, {"mout", "skin_white_male3"},
        {"ANIME_EYE", "eye_lens_a1"},   {"ocho_body", "skin_white_male"},           {"ocho_face", "skin_white_male1"}};
    LisaPoseFit fit = FitLisaStaticPose(models, kStaticLisa, kMaterials, 0.9, 0.01, "the static Lisa");
    if (!fit.ok) return;
    static_mesh_ = models.Get(kStaticLisa)->mesh.get();
    static_skin_ = std::move(fit.skin);
}

void GamePlusArm::Apply(std::vector<DrawItem>& out) {
    skins_.clear();
    if (!ready_) return;
    if (static_mesh_) {
        for (DrawItem& item : out) {
            if (item.mesh != static_mesh_) continue;
            if (static bool logged = false; !logged) {
                logged = true;
                const glm::vec3 at(item.transform * glm::vec4((static_mesh_->bounds_min + static_mesh_->bounds_max) * 0.5f, 1.0f));
                const glm::vec3 facing = glm::normalize(glm::vec3(item.transform[2]));
                LogInfo("game+: the static Lisa at ({:.3f} {:.3f} {:.3f}) facing ({:.3f} {:.3f} {:.3f})", at.x, at.y, at.z, facing.x, facing.y, facing.z);
            }
            item.mesh = lisa_mesh_;
            item.skin = static_skin_;
            item.hidden_meshes = {};
        }
    }
    const size_t count = out.size();
    for (size_t n = 0; n < count; ++n) {
        if (out[n].mesh != lisa_mesh_ || out[n].skin.size() < lisa_bind_.size() || out[n].original_look) continue;
        const std::span<const glm::mat4> skin = out[n].skin;
        auto lisa_world = [&](int i) { return skin[static_cast<size_t>(i)] * glm::translate(glm::mat4(1.0f), lisa_bind_[static_cast<size_t>(i)]); };
        std::vector<glm::mat4> world(side_bind_.size(), glm::mat4(1.0f));
        std::vector<glm::mat4>& side_skin = skins_.emplace_back(side_bind_.size() + lisa_bind_.size());
        for (size_t j = 0; j < lisa_bind_.size(); ++j) side_skin[side_bind_.size() + j] = skin[j];
        for (size_t i = 0; i < side_bind_.size(); ++i) {
            const int source = side_source_[i];
            const int p = side_parent_[i];
            if (side_arm_[i] && p >= 0) {
                const glm::mat4 lisa = source >= 0 ? lisa_world(source) : glm::mat4(1.0f);
                world[i] = glm::mat4(glm::mat3(lisa));
                if (side_arm_[static_cast<size_t>(p)]) {
                    const glm::mat4& parent = world[static_cast<size_t>(p)];
                    const float length = arm_lengths_[side_region_[static_cast<size_t>(p)] - 1];
                    world[i][3] = glm::vec4(glm::vec3(parent[3]) + glm::mat3(parent) * (length * (side_bind_[i] - side_bind_[static_cast<size_t>(p)])), 1.0f);
                } else {
                    world[i][3] = skin[static_cast<size_t>(source)] * glm::vec4(side_retarget_[i], 1.0f);
                }
                const float length = arm_lengths_[side_region_[i] - 1] / scale_;
                const glm::vec3 arm_scale = side_region_[i] == 3 ? glm::vec3(length) : glm::vec3(length, 1.0f, 1.0f);
                side_skin[i] = world[i] * glm::scale(glm::mat4(1.0f), arm_scale) * glm::translate(glm::mat4(1.0f), -side_retarget_[i]);
            } else {
                world[i] = source >= 0 ? lisa_world(source) : glm::mat4(1.0f);
                side_skin[i] = source >= 0 ? glm::mat4(skin[static_cast<size_t>(source)]) : glm::mat4(1.0f);
            }
        }
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
        const glm::vec4 lisa_tint = out[n].tint;
        out[n].tint.a = 2.0f;
        DrawItem side = out[n];
        side.mesh = side_mesh_;
        side.skin = side_skin;
        side.hidden_meshes = {};
        side.tint = glm::vec4(glm::vec3(lisa_tint) * kSkinGain, lisa_tint.a);
        out.push_back(side);
    }
}

}
