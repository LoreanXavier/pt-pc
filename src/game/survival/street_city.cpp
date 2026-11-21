#include "game/survival/street_city.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "engine/physics/collision_world.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/texture_manager.h"
#include "game/game.h"
#include "game/survival/survival_models.h"

namespace pt::game {

namespace {

constexpr uint64_t kCitySource = 0x4349545900000000ull;
constexpr const char* kTenementModel = "/Assets/sh/environ/object/shsb/map/shsb_mapp001/scenes/shsb_mapp001_buld001.fmdl";
constexpr const char* kMapModel = "/Assets/sh/environ/object/shsb/map/shsb_mapp001/scenes/shsb_mapp001.fmdl";

struct Tenement {
    const char* name;
    glm::vec2 block_min, block_max;
    glm::vec2 street_wing_min, street_wing_max;
    float street_wing_faces;
    glm::vec2 side_wing_min, side_wing_max;
};
constexpr std::array<Tenement, 3> kTenements = {{
    {"south", {14.25f, 0.6f}, {45.74f, 33.0f}, {10.0f, -3.0f}, {23.0f, 40.0f}, -1.0f, {10.0f, -3.0f}, {50.0f, 9.0f}},
    {"north", {11.15f, 52.25f}, {42.66f, 84.65f}, {8.0f, 45.0f}, {20.0f, 92.0f}, -1.0f, {8.0f, 45.0f}, {50.0f, 61.0f}},
    {"west", {-51.07f, 56.4f}, {-18.65f, 87.89f}, {-27.0f, 50.0f}, {-15.0f, 92.0f}, 1.0f, {-55.0f, 50.0f}, {-15.0f, 65.0f}},
}};
enum : size_t { kSouth, kNorth, kWest };
constexpr float kSooted = 0.5f;

bool In(const glm::vec3& p, const glm::vec2& lo, const glm::vec2& hi) { return p.x >= lo.x && p.x <= hi.x && p.z >= lo.y && p.z <= hi.y; }

const GpuMesh* CutTenement(ModelCache& models, const std::string& name, const glm::vec2& lo, const glm::vec2& hi, bool bare) {
    const std::string key = std::string(kTenementModel) + "#street_block_" + name;
    const ModelEntry* entry = models.GetRebuilt(kTenementModel, key, [&](MeshData& mesh) {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> shared(mesh.vertices.size(), UINT32_MAX);
        for (size_t s = 0; s < mesh.submeshes.size(); ++s) {
            SubMesh& sub = mesh.submeshes[s];
            const uint32_t first = static_cast<uint32_t>(indices.size());
            const bool dropped = bare && (s == 4 || s == 7 || s == 8);
            for (uint32_t i = 0; !dropped && i + 2 < sub.index_count; i += 3) {
                size_t at[3];
                glm::vec3 middle(0.0f);
                for (uint32_t k = 0; k < 3; ++k) {
                    at[k] = static_cast<size_t>(static_cast<int64_t>(mesh.indices[sub.first_index + i + k]) + sub.vertex_offset);
                    middle += mesh.vertices[at[k]].position / 3.0f;
                }
                if (!In(middle, lo, hi)) continue;
                for (const size_t v : at) {
                    uint32_t& kept = shared[v];
                    if (kept == UINT32_MAX) {
                        kept = static_cast<uint32_t>(vertices.size());
                        vertices.push_back(mesh.vertices[v]);
                    }
                    indices.push_back(kept);
                }
            }
            sub.first_index = first;
            sub.index_count = static_cast<uint32_t>(indices.size()) - first;
            sub.vertex_offset = 0;
        }
        LogInfo("street: {} cut from the street's sheets, {} triangles", name, indices.size() / 3);
        mesh.vertices = std::move(vertices);
        mesh.indices = std::move(indices);
        mesh.name = key;
    });
    if (!entry || !entry->mesh) return nullptr;
    for (SubMesh& sub : entry->mesh->submeshes) {
        sub.double_sided = true;
        sub.shadow = false;
    }
    TextureManager& table = models.Textures();
    for (size_t i = 0; i < entry->materials.size() && i < entry->material_indices.size(); ++i) {
        if (!entry->materials[i].name.ends_with("shsb_mapp001_co004")) continue;
        if (const MaterialGpu* own = table.Material(entry->material_indices[i])) {
            MaterialGpu matt = *own;
            matt.flags |= gpu::kMatDullSpecular;
            table.UpdateMaterial(entry->material_indices[i], matt);
        }
    }
    return entry->mesh.get();
}

struct Front {
    float u0, u1, y0, y1, depth;
};
struct Side {
    float u, y0, y1, d0, d1, faces;
};

const GpuMesh* BuildSides(ModelCache& models, const Tenement& t) {
    const std::string key = std::string(kTenementModel) + "#street_block_sides_" + t.name;
    const ModelEntry* entry = models.GetRebuilt(kTenementModel, key, [&](MeshData& mesh) {
        struct Wing {
            glm::vec3 out, along;
            glm::vec2 lo, hi;
            float wall;
            std::vector<Front> fronts;
            std::vector<Side> sides;
        };
        const float street_wall = (t.street_wing_faces < 0.0f ? t.block_min.x : t.block_max.x) * t.street_wing_faces;
        std::array<Wing, 2> wings = {{{{t.street_wing_faces, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, t.street_wing_min, t.street_wing_max, street_wall, {}, {}},
                                      {{0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, t.side_wing_min, t.side_wing_max, -t.block_min.y, {}, {}}}};
        auto corners = [&](const SubMesh& sub, uint32_t i, std::array<const Vertex*, 3>& v) {
            for (uint32_t k = 0; k < 3; ++k) {
                const size_t at = static_cast<size_t>(static_cast<int64_t>(mesh.indices[sub.first_index + i + k]) + sub.vertex_offset);
                v[k] = &mesh.vertices[at];
            }
        };
        for (const SubMesh& sub : mesh.submeshes) {
            if (sub.pass != RenderPass::Opaque) continue;
            for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                std::array<const Vertex*, 3> v;
                corners(sub, i, v);
                const glm::vec3 middle = (v[0]->position + v[1]->position + v[2]->position) / 3.0f;
                const glm::vec3 cross = glm::cross(v[1]->position - v[0]->position, v[2]->position - v[0]->position);
                const float area = glm::length(cross);
                if (area < 1e-7f) continue;
                const glm::vec3 n = cross / area;
                for (Wing& w : wings) {
                    if (!In(middle, w.lo, w.hi)) continue;
                    float u[3], y[3], d[3];
                    for (int k = 0; k < 3; ++k) {
                        u[k] = glm::dot(v[k]->position, w.along);
                        y[k] = v[k]->position.y;
                        d[k] = glm::dot(v[k]->position, w.out);
                    }
                    if (glm::dot(n, w.out) > 0.98f) {
                        w.fronts.push_back({std::min({u[0], u[1], u[2]}), std::max({u[0], u[1], u[2]}), std::min({y[0], y[1], y[2]}), std::max({y[0], y[1], y[2]}), (d[0] + d[1] + d[2]) / 3.0f});
                    } else if (std::abs(glm::dot(n, w.along)) > 0.9f) {
                        w.sides.push_back({(u[0] + u[1] + u[2]) / 3.0f, std::min({y[0], y[1], y[2]}), std::max({y[0], y[1], y[2]}), std::min({d[0], d[1], d[2]}), std::max({d[0], d[1], d[2]}),
                                           glm::dot(n, w.along) > 0.0f ? 1.0f : -1.0f});
                    }
                }
            }
        }
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        for (SubMesh& sub : mesh.submeshes) {
            const uint32_t first = static_cast<uint32_t>(indices.size());
            if (sub.pass == RenderPass::Opaque) {
                for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                    std::array<const Vertex*, 3> v;
                    corners(sub, i, v);
                    const glm::vec3 e1 = v[1]->position - v[0]->position, e2 = v[2]->position - v[0]->position;
                    const glm::vec3 cross = glm::cross(e1, e2);
                    const float area = glm::length(cross);
                    if (area < 1e-7f) continue;
                    const glm::vec3 middle = (v[0]->position + v[1]->position + v[2]->position) / 3.0f;
                    for (Wing& w : wings) {
                        if (glm::dot(cross / area, w.out) <= 0.98f || !In(middle, w.lo, w.hi)) continue;
                        const float a1 = glm::dot(e1, w.along), a2 = glm::dot(e2, w.along), b1 = e1.y, b2 = e2.y;
                        const float det = a1 * b2 - a2 * b1;
                        if (std::abs(det) < 1e-7f) continue;
                        auto run = [&](const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2) { return ((p1 - p0) * b2 - (p2 - p0) * b1) / det; };
                        const glm::vec2 run0 = run(v[0]->uv0, v[1]->uv0, v[2]->uv0), run1 = run(v[0]->uv1, v[1]->uv1, v[2]->uv1), run2 = run(v[0]->uv2, v[1]->uv2, v[2]->uv2);
                        for (int k = 0; k < 3; ++k) {
                            const Vertex& a = *v[static_cast<size_t>(k)];
                            const Vertex& b = *v[static_cast<size_t>((k + 1) % 3)];
                            const Vertex& c = *v[static_cast<size_t>((k + 2) % 3)];
                            const float u = glm::dot(a.position, w.along), height = std::abs(a.position.y - b.position.y);
                            const float toward_face = glm::dot(c.position, w.along) - u;
                            if (std::abs(u - glm::dot(b.position, w.along)) > 0.002f || std::abs(toward_face) < 0.01f || height < 0.02f) continue;
                            const float faces = toward_face > 0.0f ? -1.0f : 1.0f;
                            const float y0 = std::min(a.position.y, b.position.y), y1 = std::max(a.position.y, b.position.y), depth = glm::dot(a.position, w.out);
                            const float past = u + faces * 0.01f;
                            float back = depth;
                            bool goes_on = false, anything = false;
                            for (const Front& f : w.fronts) {
                                if (past < f.u0 || past > f.u1) continue;
                                const float shared = std::min(f.y1, y1) - std::max(f.y0, y0);
                                if (shared <= 0.01f) continue;
                                anything = true;
                                if (f.depth >= depth - 0.02f) {
                                    if (shared >= 0.6f * height) goes_on = true;
                                } else if (depth - f.depth < 1.2f) {
                                    back = std::min(back, f.depth);
                                }
                            }
                            if (!anything && depth - w.wall > 0.02f && depth - w.wall < 1.2f) back = w.wall;
                            if (goes_on || depth - back < 0.02f) continue;
                            float covered = 0.0f;
                            for (const Side& s : w.sides) {
                                if (s.faces != faces || std::abs(s.u - u) > 0.02f) continue;
                                const float up = std::min(s.y1, y1) - std::max(s.y0, y0), in = std::min(s.d1, depth) - std::max(s.d0, back);
                                if (up > 0.0f && in > 0.0f) covered += 0.5f * up * in;
                            }
                            if (covered >= 0.35f * height * (depth - back)) continue;
                            const float by = depth - back;
                            const glm::vec3 normal = w.along * faces;
                            auto side = [&](const Vertex& from, bool deep) {
                                Vertex s = from;
                                const float in = deep ? by : 0.0f;
                                s.position -= w.out * in;
                                s.normal = normal;
                                const glm::vec3 tangent(from.tangent);
                                s.tangent = glm::vec4(w.out * (faces * glm::dot(tangent, w.along)) + glm::vec3(0.0f, tangent.y, 0.0f), -from.tangent.w);
                                s.uv0 -= run0 * (faces * in);
                                s.uv1 -= run1 * (faces * in);
                                s.uv2 -= run2 * (faces * in);
                                return s;
                            };
                            std::array<Vertex, 4> quad = {side(a, false), side(b, false), side(b, true), side(a, true)};
                            if (glm::dot(glm::cross(quad[1].position - quad[0].position, quad[2].position - quad[0].position), normal) < 0.0f) std::reverse(quad.begin(), quad.end());
                            const uint32_t base = static_cast<uint32_t>(vertices.size());
                            vertices.insert(vertices.end(), quad.begin(), quad.end());
                            indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                        }
                    }
                }
            }
            sub.first_index = first;
            sub.index_count = static_cast<uint32_t>(indices.size()) - first;
            sub.vertex_offset = 0;
        }
        LogInfo("street: the {} tenement's piers given {} sides", t.name, indices.size() / 6);
        mesh.vertices = std::move(vertices);
        mesh.indices = std::move(indices);
        mesh.name = key;
    });
    return entry ? entry->mesh.get() : nullptr;
}

glm::mat4 HalfTurn(const glm::vec2& twice_middle) {
    glm::mat4 m(1.0f);
    m[0][0] = -1.0f;
    m[2][2] = -1.0f;
    m[3] = glm::vec4(twice_middle.x, 0.0f, twice_middle.y, 1.0f);
    return m;
}

glm::mat4 Move(float x, float z) {
    glm::mat4 m(1.0f);
    m[3] = glm::vec4(x, 0.0f, z, 1.0f);
    return m;
}

constexpr size_t kFencePosts = 3, kFenceLink = 7;
constexpr float kFenceAt = -17.0f, kFenceFrom = 1.8f, kFencePair = 3.977f;

const GpuMesh* BuildFences(ModelCache& models) {
    const std::string key = std::string(kMapModel) + "#street_fences";
    const ModelEntry* entry = models.GetRebuilt(kMapModel, key, [&](MeshData& mesh) {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        for (size_t s = 0; s < mesh.submeshes.size(); ++s) {
            SubMesh& sub = mesh.submeshes[s];
            const uint32_t first = static_cast<uint32_t>(indices.size());
            if (s == kFencePosts || s == kFenceLink) {
                for (const StreetEdge& edge : StreetEdges()) {
                    if (!edge.fence) continue;
                    const glm::vec2 run = edge.b - edge.a;
                    const float length = glm::length(run);
                    const int pairs = std::max(1, static_cast<int>(std::lround(length / kFencePair)));
                    const float stretch = length / (static_cast<float>(pairs) * kFencePair);
                    const glm::vec2 along = run / length;
                    const glm::vec3 dir(along.x, 0.0f, along.y), across(along.y, 0.0f, -along.x);
                    auto place = [&](Vertex v, float z) {
                        const float out = v.position.x - kFenceAt;
                        const glm::vec3 p(edge.a.x, v.position.y, edge.a.y);
                        v.position = p + dir * (z * stretch) + across * out;
                        auto turn = [&](const glm::vec3& d) { return across * d.x + glm::vec3(0.0f, d.y, 0.0f) + dir * d.z; };
                        v.normal = turn(v.normal);
                        v.tangent = glm::vec4(turn(glm::vec3(v.tangent)), v.tangent.w);
                        return v;
                    };
                    for (int pair = 0; pair <= pairs; ++pair) {
                        for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                            std::array<Vertex, 3> v;
                            float middle = 0.0f, span = 0.0f, low = 1e9f;
                            for (uint32_t k = 0; k < 3; ++k) {
                                v[k] = mesh.vertices[static_cast<size_t>(static_cast<int64_t>(mesh.indices[sub.first_index + i + k]) + sub.vertex_offset)];
                                middle += v[k].position.z / 3.0f;
                                low = std::min(low, v[k].position.z);
                                span = std::max(span, v[k].position.z);
                            }
                            span -= low;
                            if (middle < kFenceFrom || middle >= kFenceFrom + kFencePair) continue;
                            if (pair == pairs && (s != kFencePosts || span > 0.3f || middle > kFenceFrom + 0.3f)) continue;
                            const uint32_t base = static_cast<uint32_t>(vertices.size());
                            for (const Vertex& c : v) vertices.push_back(place(c, c.position.z - kFenceFrom + static_cast<float>(pair) * kFencePair));
                            indices.insert(indices.end(), {base, base + 1, base + 2});
                        }
                    }
                }
            }
            sub.first_index = first;
            sub.index_count = static_cast<uint32_t>(indices.size()) - first;
            sub.vertex_offset = 0;
        }
        LogInfo("street: the night's fences, {} triangles", indices.size() / 3);
        mesh.vertices = std::move(vertices);
        mesh.indices = std::move(indices);
        mesh.name = key;
    });
    return entry ? entry->mesh.get() : nullptr;
}

}

std::span<const StreetEdge> StreetEdges() {
    constexpr glm::vec2 kEast(1.0f, 0.0f), kWest(-1.0f, 0.0f), kNorth(0.0f, 1.0f), kSouthward(0.0f, -1.0f);
    static constexpr StreetEdge kEdges[] = {
        {{14.3f, 33.0f}, {14.3f, 52.25f}, kWest, true},
        {{14.3f, 85.45f}, {14.3f, 97.6f}, kWest, true},
        {{14.3f, 130.65f}, {14.3f, 149.9f}, kWest, true},
        {{11.5f, 183.1f}, {9.16f, 184.85f}, glm::vec2(-0.6f, -0.8f), true},
        {{9.16f, 184.85f}, {3.7f, 190.0f}, glm::vec2(-0.69f, -0.73f), false},
        {{3.7f, 190.0f}, {-19.4f, 190.0f}, kSouthward, true},
        {{-19.4f, 190.0f}, {-18.3f, 185.6f}, kEast, true},
        {{-17.0f, 101.75f}, {-17.0f, 137.6f}, kEast, false},
        {{-17.0f, 137.6f}, {-18.1f, 154.05f}, kEast, true},
        {{-17.0f, 101.75f}, {-18.6f, 88.0f}, kEast, true},
        {{-18.9f, 56.4f}, {-18.9f, 24.3f}, kEast, true},
        {{11.15f, 85.45f}, {14.3f, 85.45f}, kNorth, false},
        {{11.5f, 183.1f}, {14.3f, 183.1f}, kNorth, false},
        {{14.25f, 20.0f}, {14.25f, 33.0f}, kWest, false},
        {{11.15f, 52.25f}, {11.15f, 85.45f}, kWest, false},
        {{14.76f, 97.6f}, {14.76f, 130.65f}, kWest, false},
        {{11.66f, 149.9f}, {11.66f, 183.1f}, kWest, false},
        {{-18.65f, 56.4f}, {-18.65f, 88.0f}, kEast, false},
        {{-18.14f, 154.05f}, {-18.14f, 185.6f}, kEast, false},
        {{11.15f, 52.25f}, {14.3f, 52.25f}, kSouthward, false},
        {{11.66f, 149.9f}, {14.3f, 149.9f}, kSouthward, false},
        {{14.3f, 97.6f}, {14.76f, 97.6f}, kNorth, false},
    };
    return kEdges;
}

void DumpStreetWalk(Game& game, const std::string& path) {
    const CollisionWorld& world = game.Collision();
    CharacterController c;
    c.shape = game.GetPlayer().controller.shape;
    const bool fine = std::getenv("PT_WALK_FINE") != nullptr;
    const float kStep = fine ? 0.15f : 0.3f;
    const int directions = fine ? 16 : 8;
    std::unordered_map<int64_t, glm::vec3> seen;
    std::deque<glm::vec3> queue;
    std::vector<glm::vec3> falls, outs;
    const glm::vec3 start = game.GetPlayer().Feet();
    auto key = [&](const glm::vec3& p) {
        const int64_t x = static_cast<int64_t>(std::floor(p.x / kStep)) + 2000, z = static_cast<int64_t>(std::floor(p.z / kStep)) + 2000;
        return (x << 32) | (z << 8) | (static_cast<int64_t>(std::floor(p.y / 0.8f)) + 64);
    };
    seen[key(start)] = start;
    queue.push_back(start);
    size_t moves = 0;
    while (!queue.empty() && seen.size() < 1500000) {
        const glm::vec3 p = queue.front();
        queue.pop_front();
        for (int d = 0; d < directions; ++d) {
            const float a = static_cast<float>(d) * 6.2831853f / static_cast<float>(directions);
            const glm::vec3 dir(std::sin(a), 0.0f, std::cos(a));
            c.position = p + glm::vec3(0.0f, 0.02f, 0.0f);
            c.Reset();
            for (int k = 0; k < 4; ++k) c.Move(world, dir * (kStep * 1.1f / 4.0f), 1.0f / 60.0f, 1.0f / 30.0f);
            ++moves;
            for (int k = 0; k < 240; ++k) {
                const float before = c.position.y;
                c.Move(world, glm::vec3(0.0f), 1.0f / 60.0f, 1.0f / 30.0f);
                if ((c.grounded && std::abs(c.position.y - before) < 1.0e-4f) || c.position.y < -6.0f) break;
            }
            glm::vec3 q = c.position;
            if (glm::length(glm::vec2(q.x - p.x, q.z - p.z)) < kStep * 0.6f) continue;
            if (q.y < -0.1f) {
                falls.push_back(q);
                if (q.y <= -5.0f) continue;
                q.y = -0.02f;
            }
            if (q.x < -19.5f || q.x > 14.6f || q.z < -12.5f || q.z > 190.0f) {
                outs.push_back(q);
                continue;
            }
            if (seen.emplace(key(q), q).second) queue.push_back(q);
        }
    }
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "start %.3f %.3f %.3f moves %zu\n", start.x, start.y, start.z, moves);
    for (const auto& [k, q] : seen) std::fprintf(f, "w %.3f %.3f %.3f\n", q.x, q.y, q.z);
    for (const glm::vec3& q : falls) std::fprintf(f, "f %.3f %.3f %.3f\n", q.x, q.y, q.z);
    for (const glm::vec3& q : outs) std::fprintf(f, "o %.3f %.3f %.3f\n", q.x, q.y, q.z);
    std::vector<bool> owners;
    for (const CollisionTriangle& t : world.Triangles()) {
        std::fprintf(f, "t %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %u %llu\n", t.a.x, t.a.y, t.a.z, t.b.x, t.b.y, t.b.z, t.c.x, t.c.y, t.c.z, t.owner,
                     static_cast<unsigned long long>(t.tags));
        if (t.owner >= owners.size()) owners.resize(t.owner + 1);
        owners[t.owner] = true;
    }
    for (uint32_t o = 0; o < owners.size(); ++o) {
        if (owners[o]) std::fprintf(f, "n %u %s\n", o, world.OwnerName(o).c_str());
    }
    std::fclose(f);
    LogInfo("walk map: {} places, {} falls, {} past the box, {} moves", seen.size(), falls.size(), outs.size(), moves);
}

std::span<const StreetBody> StreetBodies() {
    constexpr float kSaloon = 1.3f, kSaloonBack = -2.46f, kSaloonFront = 2.83f;
    static constexpr StreetBody kBodies[] = {
        {{-6.254f, 98.931f}, {0.997f, -0.075f}, kSaloon, kSaloonBack, kSaloonFront},
        {{9.902f, 36.506f}, {-0.115f, 0.993f}, kSaloon, kSaloonBack, kSaloonFront},
        {{-11.609f, 35.193f}, {0.08f, 0.997f}, kSaloon, kSaloonBack, kSaloonFront},
        {{-13.562f, 65.791f}, {0.995f, 0.096f}, kSaloon, kSaloonBack, kSaloonFront},
        {{6.959f, 82.962f}, {-0.966f, -0.259f}, kSaloon, kSaloonBack, kSaloonFront},
        {{8.493f, 45.809f}, {-0.174f, -0.985f}, kSaloon, kSaloonBack, kSaloonFront},
        {{9.264f, 40.513f}, {0.259f, -0.966f}, 1.34f, -2.75f, 2.63f},
        {{-11.97f, 119.2f}, {1.0f, 0.0f}, 0.5f, -0.5f, 0.5f},
        {{6.6f, 28.5f}, {1.0f, 0.0f}, 0.5f, -0.5f, 0.5f},
        {{-12.87f, 76.4f}, {1.0f, 0.0f}, 0.74f, -1.09f, 1.09f},
    };
    return kBodies;
}

bool StreetBody::Holds(const glm::vec2& p) const {
    const glm::vec2 d = p - at;
    const float along = glm::dot(d, x_axis), up = glm::dot(d, glm::vec2(-x_axis.y, x_axis.x));
    return std::abs(along) < half_x && up > back && up < front;
}

bool StreetCity::Ensure(ModelCache& models, uint32_t road_material, float road_tile) {
    if (!tried_) {
        tried_ = true;
        std::array<const GpuMesh*, 3> crossing{}, sides{}, street_wing{}, side_wing{};
        for (size_t i = 0; i < kTenements.size(); ++i) {
            const Tenement& t = kTenements[i];
            crossing[i] = CutTenement(models, std::string(t.name) + "_across", t.side_wing_min, t.side_wing_max, false);
            sides[i] = BuildSides(models, t);
            street_wing[i] = CutTenement(models, std::string(t.name) + "_street_wing", t.street_wing_min, t.street_wing_max, true);
            side_wing[i] = CutTenement(models, std::string(t.name) + "_side_wing", t.side_wing_min, t.side_wing_max, true);
            if (!crossing[i] || !sides[i] || !street_wing[i] || !side_wing[i]) {
                LogWarn("street: the tenements could not be cut from {}", kTenementModel);
                return false;
            }
        }
        glm::mat4 turned(1.0f);
        turned[0] = glm::vec4(0.683050f, 0.0f, 0.730371f, 0.0f);
        turned[2] = glm::vec4(-0.730371f, 0.0f, 0.683050f, 0.0f);
        turned[3] = glm::vec4(24.008266f, 0.0f, 194.618546f, 1.0f);
        const std::array<glm::mat4, 3> sheets = {glm::mat4(1.0f), Move(0.512074f, 97.650002f), turned};
        std::vector<CityBox> insides;
        for (const glm::mat4& sheet : sheets) {
            for (size_t i = 0; i < kTenements.size(); ++i) {
                const Tenement& t = kTenements[i];
                const glm::mat4 across = sheet * HalfTurn(t.block_min + t.block_max);
                placed_.push_back({crossing[i], across, glm::vec4(kSooted, kSooted, kSooted, 1.0f)});
                placed_.push_back({sides[i], across});
                placed_.push_back({sides[i], sheet});
                constexpr float kInset = 1.0f, kTop = 19.5f;
                const glm::vec2 middle = 0.5f * (t.block_min + t.block_max), half = 0.5f * (t.block_max - t.block_min) - glm::vec2(kInset);
                insides.push_back({sheet * glm::translate(glm::mat4(1.0f), glm::vec3(middle.x, 0.5f * kTop, middle.y)), glm::vec3(half.x, 0.5f * kTop, half.y)});
            }
        }
        auto wing = [&](const GpuMesh* mesh, const glm::mat4& place, const glm::vec3& behind, const glm::vec3& half) {
            placed_.push_back({mesh, place});
            insides.push_back({place * glm::translate(glm::mat4(1.0f), behind), half});
        };
        constexpr float kHalfHeight = 9.75f;
        auto east = [&](float x, float z) {
            wing(street_wing[kSouth], Move(x - 14.25f, z - 0.6f), {14.25f + 2.8f, kHalfHeight, 16.8f}, {2.0f, kHalfHeight, 16.5f});
        };
        constexpr float kWestWing = 31.49f;
        auto west = [&](float x, float z, float stretch) {
            glm::mat4 m = Move(x + 18.65f, z - 56.4f * stretch);
            m[2][2] = stretch;
            wing(street_wing[kWest], m, {-18.65f - 2.8f, kHalfHeight, 72.1f}, {2.0f, kHalfHeight, 16.5f});
        };
        auto north = [&](float z, float x) {
            wing(side_wing[kSouth], Move(x - 14.25f, z - 0.6f), {30.0f, kHalfHeight, 0.6f + 2.8f}, {16.5f, kHalfHeight, 2.0f});
        };
        for (const float z : {27.2f, 73.2f, 125.2f, 183.0f, 214.5f}) east(56.0f, z);
        west(-53.0f, 56.4f - kWestWing, 1.0f);
        west(-53.0f, 56.4f - 2.0f * kWestWing, 1.0f);
        west(-53.0f, 87.89f, 1.0505f);
        west(-53.0f, 87.89f + 1.0505f * kWestWing, 1.0505f);
        west(-60.0f, 175.0f, 1.0f);
        west(-60.0f, 206.5f, 1.0f);
        for (const float x : {14.0f, 45.5f, -80.0f, -48.5f}) north(292.0f, x);
        wing(side_wing[kSouth], HalfTurn(glm::vec2(-6.75f, 19.6f)), {30.0f, kHalfHeight, 0.6f + 2.8f}, {16.5f, kHalfHeight, 2.0f});
        shell_ = BuildCityShell(models, insides);
        const GpuMesh* fences = BuildFences(models);
        if (fences) placed_.push_back({fences, glm::mat4(1.0f)});
        ready_ = true;
    }
    if (ready_) {
        std::unique_ptr<GpuMesh>& ground = grounds_[road_material];
        if (!ground) ground = BuildCityGround(models, road_material, road_tile);
        ground_ = ground.get();
    }
    return ready_;
}

void StreetCity::Sheets(ModelCache& models, bool night) {
    if (!night && sheets_own_.empty()) return;
    const ModelEntry* entry = models.Get(kTenementModel);
    if (!entry || !entry->mesh) return;
    std::vector<SubMesh>& subs = entry->mesh->submeshes;
    if (night) {
        if (!sheets_own_.empty()) return;
        for (SubMesh& sub : subs) {
            sheets_own_.push_back(sub.double_sided);
            sub.double_sided = true;
        }
    } else {
        for (size_t i = 0; i < subs.size() && i < sheets_own_.size(); ++i) subs[i].double_sided = sheets_own_[i];
        sheets_own_.clear();
    }
}

void StreetCity::AddDraws(std::vector<DrawItem>& out) const {
    if (!ready_) return;
    DrawItem item;
    for (size_t i = 0; i < placed_.size(); ++i) {
        item.mesh = placed_[i].mesh;
        item.transform = placed_[i].transform;
        item.tint = placed_[i].tint;
        item.source = kCitySource + i;
        out.push_back(item);
    }
    item.transform = glm::mat4(1.0f);
    item.tint = glm::vec4(1.0f);
    if (shell_) {
        item.mesh = shell_.get();
        item.source = kCitySource + 0x1000u;
        out.push_back(item);
    }
    if (ground_) {
        item.mesh = ground_;
        item.source = kCitySource + 0x1001u;
        out.push_back(item);
    }
}

}
