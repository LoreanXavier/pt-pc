#include "game/survival/survival_models.h"
#include "game/survival/survival_skins.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/assets/gltf_model.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/resource_path.h"
#include "engine/fs/mods.h"
#include "engine/render/gpu_types.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/texture_manager.h"

namespace pt::game {

namespace {

constexpr float kPi = 3.14159265f;

using Profile = std::vector<glm::vec2>;

glm::vec3 P(const glm::vec2& zy, float x) {
    return {x, zy.y, zy.x};
}

float Cross(const glm::vec2& a, const glm::vec2& b) {
    return a.x * b.y - a.y * b.x;
}

float SignedArea(const Profile& p) {
    float a = 0.0f;
    for (size_t i = 0; i < p.size(); ++i) a += Cross(p[i], p[(i + 1) % p.size()]);
    return 0.5f * a;
}

bool InTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    const float d1 = Cross(b - a, p - a), d2 = Cross(c - b, p - b), d3 = Cross(a - c, p - c);
    return d1 > 1e-9f && d2 > 1e-9f && d3 > 1e-9f;
}

std::vector<uint32_t> Triangulate(const Profile& poly) {
    std::vector<uint32_t> out;
    std::vector<uint32_t> v(poly.size());
    for (uint32_t i = 0; i < v.size(); ++i) v[i] = i;
    while (v.size() > 3) {
        bool clipped = false;
        for (size_t i = 0; i < v.size(); ++i) {
            const uint32_t a = v[(i + v.size() - 1) % v.size()], b = v[i], c = v[(i + 1) % v.size()];
            if (Cross(poly[b] - poly[a], poly[c] - poly[b]) <= 1e-10f) continue;
            bool empty = true;
            for (const uint32_t j : v) {
                if (j == a || j == b || j == c) continue;
                if (InTriangle(poly[j], poly[a], poly[b], poly[c])) {
                    empty = false;
                    break;
                }
            }
            if (!empty) continue;
            out.insert(out.end(), {a, b, c});
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            for (size_t i = 1; i + 1 < v.size(); ++i) out.insert(out.end(), {v[0], v[i], v[i + 1]});
            return out;
        }
    }
    out.insert(out.end(), {v[0], v[1], v[2]});
    return out;
}

Profile Inset(const Profile& p, float d) {
    Profile out(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        const glm::vec2 prev = p[(i + p.size() - 1) % p.size()], cur = p[i], next = p[(i + 1) % p.size()];
        auto inward = [](glm::vec2 e) {
            const float l = glm::length(e);
            return l > 1e-9f ? glm::vec2(-e.y, e.x) / l : glm::vec2(0.0f);
        };
        const glm::vec2 n1 = inward(cur - prev), n2 = inward(next - cur);
        glm::vec2 m = n1 + n2;
        const float l = glm::length(m);
        m = l > 1e-6f ? m / l : n1;
        out[i] = cur + m * (d / std::max(glm::dot(m, n1), 0.35f));
    }
    return out;
}

struct Kit {
    MeshData data;
    std::vector<std::vector<uint32_t>> parts;
    std::vector<uint32_t> materials;
    std::vector<bool> glows;

    size_t Part(uint32_t material, bool glow = false) {
        parts.emplace_back();
        materials.push_back(material);
        glows.push_back(glow);
        return parts.size() - 1;
    }

    uint32_t Vert(const glm::vec3& p, const glm::vec3& n, const glm::vec3& t, const glm::vec2& uv) {
        Vertex v;
        v.position = p;
        v.normal = n;
        v.tangent = glm::vec4(t, 1.0f);
        v.uv0 = uv;
        data.vertices.push_back(v);
        return static_cast<uint32_t>(data.vertices.size() - 1);
    }

    void Tri(size_t part, glm::vec3 a, glm::vec3 b, glm::vec3 c, const glm::vec3& n, const glm::vec3& t, glm::vec2 ua, glm::vec2 ub, glm::vec2 uc) {
        if (glm::dot(glm::cross(b - a, c - a), n) < 0.0f) {
            std::swap(b, c);
            std::swap(ub, uc);
        }
        const uint32_t i = Vert(a, n, t, ua), j = Vert(b, n, t, ub), k = Vert(c, n, t, uc);
        parts[part].insert(parts[part].end(), {i, j, k});
    }

    void Quad(size_t part, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, glm::vec3 n, const glm::vec2& uv0 = {0, 0},
              const glm::vec2& uv1 = {1, 1}) {
        const float l = glm::length(n);
        n = l > 1e-9f ? n / l : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 t = glm::length(b - a) > 1e-9f ? glm::normalize(b - a) : glm::vec3(1.0f, 0.0f, 0.0f);
        Tri(part, a, b, c, n, t, {uv0.x, uv0.y}, {uv1.x, uv0.y}, {uv1.x, uv1.y});
        Tri(part, a, c, d, n, t, {uv0.x, uv0.y}, {uv1.x, uv1.y}, {uv0.x, uv1.y});
    }

    void Extrude(size_t part, const glm::mat4& m, Profile poly, float half, float bevel, float uv = 20.0f) {
        if (poly.size() < 3) return;
        if (SignedArea(poly) < 0.0f) std::reverse(poly.begin(), poly.end());
        bevel = std::clamp(bevel, 0.0f, half * 0.45f);
        const Profile cap = bevel > 0.0f ? Inset(poly, bevel) : poly;
        const float xin = half - bevel;
        const glm::mat3 r(m);
        auto at = [&](const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); };
        auto dir = [&](const glm::vec3& d) { return glm::normalize(r * d); };
        const std::vector<uint32_t> tris = Triangulate(cap);
        for (const float s : {1.0f, -1.0f}) {
            const glm::vec3 n = dir({s, 0.0f, 0.0f});
            for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                const glm::vec2 a = cap[tris[i]], b = cap[tris[i + 1]], c = cap[tris[i + 2]];
                Tri(part, at(P(a, s * half)), at(P(b, s * half)), at(P(c, s * half)), n, dir({0.0f, 0.0f, 1.0f}), a * uv, b * uv, c * uv);
            }
        }
        float run = 0.0f;
        for (size_t i = 0; i < poly.size(); ++i) {
            const size_t j = (i + 1) % poly.size();
            const glm::vec2 a = poly[i], b = poly[j];
            const glm::vec2 d = b - a;
            const float length = glm::length(d);
            if (length < 1e-7f) continue;
            const glm::vec2 out2(d.y / length, -d.x / length);
            const glm::vec3 out = glm::vec3(0.0f, out2.y, out2.x);
            Quad(part, at(P(a, xin)), at(P(b, xin)), at(P(b, -xin)), at(P(a, -xin)), dir(out), {run * uv, -xin * uv}, {(run + length) * uv, xin * uv});
            if (bevel > 0.0f) {
                for (const float s : {1.0f, -1.0f}) {
                    Quad(part, at(P(a, s * xin)), at(P(b, s * xin)), at(P(cap[j], s * half)), at(P(cap[i], s * half)),
                         dir(out + glm::vec3(s, 0.0f, 0.0f)), {run * uv, 0.0f}, {(run + length) * uv, bevel * uv});
                }
            }
            run += length;
        }
    }

    void Box(size_t part, const glm::mat4& m, const glm::vec3& half, float bevel = 0.0f, float uv = 20.0f) {
        const Profile rect = {{-half.z, -half.y}, {half.z, -half.y}, {half.z, half.y}, {-half.z, half.y}};
        Extrude(part, m, rect, half.x, bevel, uv);
    }

    void Cylinder(size_t part, const glm::mat4& m, float radius, float length, int segments = 16, bool caps = true, float uv = 20.0f) {
        const glm::mat3 r(m);
        auto at = [&](const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); };
        for (int i = 0; i < segments; ++i) {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / segments, a1 = 2.0f * kPi * static_cast<float>(i + 1) / segments;
            const glm::vec3 d0(std::cos(a0), std::sin(a0), 0.0f), d1(std::cos(a1), std::sin(a1), 0.0f);
            const glm::vec3 n0 = glm::normalize(r * d0), n1 = glm::normalize(r * d1);
            const glm::vec3 t = glm::normalize(r * glm::vec3(0.0f, 0.0f, 1.0f));
            const float u0 = a0 * radius * uv, u1 = a1 * radius * uv;
            const uint32_t v0 = Vert(at(d0 * radius), n0, t, {u0, 0.0f}), v1 = Vert(at(d1 * radius), n1, t, {u1, 0.0f});
            const uint32_t v2 = Vert(at(d1 * radius + glm::vec3(0, 0, length)), n1, t, {u1, length * uv});
            const uint32_t v3 = Vert(at(d0 * radius + glm::vec3(0, 0, length)), n0, t, {u0, length * uv});
            parts[part].insert(parts[part].end(), {v0, v1, v2, v0, v2, v3});
            if (caps) {
                Tri(part, at(glm::vec3(0, 0, length)), at(d0 * radius + glm::vec3(0, 0, length)), at(d1 * radius + glm::vec3(0, 0, length)),
                    glm::normalize(r * glm::vec3(0, 0, 1)), glm::normalize(r * glm::vec3(1, 0, 0)), {0, 0}, glm::vec2(d0) * radius * uv,
                    glm::vec2(d1) * radius * uv);
                Tri(part, at(glm::vec3(0.0f)), at(d1 * radius), at(d0 * radius), glm::normalize(r * glm::vec3(0, 0, -1)),
                    glm::normalize(r * glm::vec3(1, 0, 0)), {0, 0}, glm::vec2(d1) * radius * uv, glm::vec2(d0) * radius * uv);
            }
        }
    }

    void Lathe(size_t part, const glm::mat4& m, const std::vector<glm::vec2>& profile, int segments = 12) {
        const glm::mat3 r(m);
        const size_t n = profile.size();
        if (n < 2) return;
        const size_t row = static_cast<size_t>(segments + 1);
        std::vector<uint32_t> ring(n * row);
        for (size_t i = 0; i < n; ++i) {
            const glm::vec2 slope = profile[std::min(i + 1, n - 1)] - profile[i == 0 ? 0 : i - 1];
            const glm::vec2 out = glm::length(slope) > 1e-9f ? glm::normalize(glm::vec2(-slope.y, slope.x)) : glm::vec2(1.0f, 0.0f);
            for (int j = 0; j <= segments; ++j) {
                const float a = 2.0f * kPi * static_cast<float>(j) / segments;
                const glm::vec3 d(std::cos(a), std::sin(a), 0.0f);
                const glm::vec3 p = d * profile[i].x + glm::vec3(0.0f, 0.0f, profile[i].y);
                const glm::vec3 normal = glm::normalize(r * (d * out.x + glm::vec3(0.0f, 0.0f, out.y)));
                const glm::vec3 tangent = glm::normalize(r * glm::vec3(-d.y, d.x, 0.0f));
                ring[i * row + static_cast<size_t>(j)] =
                    Vert(glm::vec3(m * glm::vec4(p, 1.0f)), normal, tangent, {static_cast<float>(j) / segments, static_cast<float>(i) / (n - 1)});
            }
        }
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            const Vertex& va = data.vertices[a];
            const Vertex& vb = data.vertices[b];
            const Vertex& vc = data.vertices[c];
            const glm::vec3 face = glm::cross(vb.position - va.position, vc.position - va.position);
            if (glm::dot(face, face) < 1e-24f) return;
            if (glm::dot(face, va.normal + vb.normal + vc.normal) < 0.0f) std::swap(b, c);
            parts[part].insert(parts[part].end(), {a, b, c});
        };
        for (size_t i = 0; i + 1 < n; ++i) {
            for (size_t j = 0; j < static_cast<size_t>(segments); ++j) {
                const uint32_t a = ring[i * row + j], b = ring[i * row + j + 1], c = ring[(i + 1) * row + j + 1], d = ring[(i + 1) * row + j];
                tri(a, b, c);
                tri(a, c, d);
            }
        }
    }

    void Disc(size_t part, const glm::mat4& m, float radius, int segments = 12) {
        const glm::mat3 r(m);
        auto at = [&](const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); };
        for (int i = 0; i < segments; ++i) {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / segments, a1 = 2.0f * kPi * static_cast<float>(i + 1) / segments;
            Tri(part, at(glm::vec3(0.0f)), at(glm::vec3(std::cos(a0), std::sin(a0), 0.0f) * radius), at(glm::vec3(std::cos(a1), std::sin(a1), 0.0f) * radius),
                glm::normalize(r * glm::vec3(0, 0, 1)), glm::normalize(r * glm::vec3(1, 0, 0)), {0.5f, 0.5f}, {0, 0}, {1, 0});
        }
    }

    void Ring(size_t part, const glm::mat4& m, float r0, float r1, int segments = 32) {
        const glm::mat3 r(m);
        auto at = [&](const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); };
        for (int i = 0; i < segments; ++i) {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / segments, a1 = 2.0f * kPi * static_cast<float>(i + 1) / segments;
            const glm::vec3 d0(std::cos(a0), 0.0f, std::sin(a0)), d1(std::cos(a1), 0.0f, std::sin(a1));
            Quad(part, at(d0 * r0), at(d1 * r0), at(d1 * r1), at(d0 * r1), r * glm::vec3(0, 1, 0));
        }
    }

    std::unique_ptr<GpuMesh> Upload(ModelCache& models, const char* name) {
        data.name = name;
        glm::vec3 lo(1e9f), hi(-1e9f);
        for (const Vertex& v : data.vertices) {
            lo = glm::min(lo, v.position);
            hi = glm::max(hi, v.position);
        }
        data.bounds_min = lo;
        data.bounds_max = hi;
        for (size_t p = 0; p < parts.size(); ++p) {
            if (parts[p].empty()) continue;
            SubMesh sub;
            sub.first_index = static_cast<uint32_t>(data.indices.size());
            sub.index_count = static_cast<uint32_t>(parts[p].size());
            sub.material = materials[p];
            sub.pass = glows[p] ? RenderPass::Unlit : RenderPass::Opaque;
            sub.kind = static_cast<uint8_t>(glows[p] ? gpu::kKindConstant : gpu::kKindDeferred);
            sub.shadow = !glows[p];
            data.indices.insert(data.indices.end(), parts[p].begin(), parts[p].end());
            data.submeshes.push_back(sub);
        }
        return models.Scene().Upload(data);
    }
};

using Owned = std::vector<std::unique_ptr<GpuMesh>>;

const GpuMesh* Keep(Owned& owned, std::unique_ptr<GpuMesh> mesh) {
    owned.push_back(std::move(mesh));
    return owned.back().get();
}

glm::mat4 T(const glm::vec3& p) {
    return glm::translate(glm::mat4(1.0f), p);
}

glm::mat4 TR(const glm::vec3& p, float angle, const glm::vec3& axis) {
    return glm::rotate(glm::translate(glm::mat4(1.0f), p), angle, axis);
}

glm::mat4 Along(float x, float y, float z0) {
    return T({x, y, z0});
}

struct Materials {
    uint32_t polymer = 0, grip = 0, nitride = 0, steel = 0, stainless = 0, brass = 0, rubber = 0, olive = 0, white_case = 0, hull = 0,
             blood = 0, dark = 0, bore = 0, copper = 0, facade = 0, backdrop_ground = 0, fresh_blood = 0, wound = 0, flying_blood = 0;
    std::array<uint32_t, 3> pool{};
    uint32_t rust = 0, tape = 0, flare_paper = 0, flare_fire = 0;
    uint32_t box_paint = 0, label = 0, photo = 0, iron = 0, red_glass = 0, dead_glass = 0, flame = 0, wood = 0, wood_panel = 0, beyond = 0,
             switch_on = 0;
    uint32_t tritium = 0, orange_dot = 0, cross = 0, flash = 0, spark = 0, tracer = 0, lens = 0;
    uint32_t flash_front = 0, flash_side = 0;
    uint32_t mist_texture = 0;
    uint32_t flare_glow_texture = 0;
    uint32_t car_contact_texture = 0;
    uint32_t contact_texture = 0;
};

std::deque<std::vector<uint8_t>>& Pixels() {
    static std::deque<std::vector<uint8_t>> store;
    return store;
}

uint32_t SolidTexture(TextureManager& textures, const std::string& name, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (const uint32_t found = textures.Find(name); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    std::vector<uint8_t>& px = Pixels().emplace_back(std::vector<uint8_t>{r, g, b, a});
    const TextureMip mip{1, 1, std::span<const uint8_t>(px)};
    return textures.Create(name, gpu::Format::R8G8B8A8Unorm, std::span<const TextureMip>(&mip, 1));
}

float Hash2(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFFFu) / 16777215.0f;
}

float ValueNoise(float x, float y, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi), fy = y - static_cast<float>(yi);
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = Hash2(xi, yi, seed), b = Hash2(xi + 1, yi, seed), c = Hash2(xi, yi + 1, seed), d = Hash2(xi + 1, yi + 1, seed);
    return glm::mix(glm::mix(a, b, sx), glm::mix(c, d, sx), sy);
}

float Fbm(float x, float y, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, total = 0.0f;
    for (int o = 0; o < 4; ++o) {
        sum += amp * ValueNoise(x, y, seed + static_cast<uint32_t>(o) * 101u);
        total += amp;
        x *= 2.03f;
        y *= 2.03f;
        amp *= 0.5f;
    }
    return sum / total;
}

uint32_t MuzzleFlashTexture(TextureManager& textures, bool side) {
    const std::string name = side ? "survival_flash_side" : "survival_flash_front";
    if (const uint32_t found = textures.Find(name); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    const int w = 128, h = side ? 64 : 128;
    std::vector<float> rgba(static_cast<size_t>(w * h * 4), 0.0f);
    constexpr int kPetals = 7;
    float heading[kPetals], length[kPetals], width[kPetals];
    for (int i = 0; i < kPetals; ++i) {
        heading[i] = (static_cast<float>(i) + 0.6f * Hash2(i, 1, 77u)) / kPetals * 6.2831853f;
        length[i] = 0.55f + 0.45f * Hash2(i, 2, 77u);
        width[i] = 0.16f + 0.14f * Hash2(i, 3, 77u);
    }
    auto colour = [](float heat) {
        const glm::vec3 white(1.0f, 0.96f, 0.86f), yellow(1.0f, 0.78f, 0.38f), orange(1.0f, 0.48f, 0.14f), red(0.75f, 0.16f, 0.04f);
        if (heat > 0.66f) return glm::mix(yellow, white, (heat - 0.66f) / 0.34f);
        if (heat > 0.33f) return glm::mix(orange, yellow, (heat - 0.33f) / 0.33f);
        return glm::mix(red, orange, heat / 0.33f);
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float cover = 0.0f, heat = 0.0f;
            if (!side) {
                const float u = (x + 0.5f) / w * 2.0f - 1.0f, v = (y + 0.5f) / h * 2.0f - 1.0f;
                const float r = std::sqrt(u * u + v * v), a = std::atan2(v, u);
                float edge = 0.22f;
                for (int i = 0; i < kPetals; ++i) {
                    float d = std::remainder(a - heading[i], 6.2831853f) / width[i];
                    edge = std::max(edge, length[i] * std::exp(-d * d));
                }
                edge *= 0.8f + 0.4f * Fbm(std::cos(a) * 2.5f + 3.0f, std::sin(a) * 2.5f + 3.0f, 11u);
                const float body = glm::clamp((edge - r) / (edge * 0.75f), 0.0f, 1.0f);
                const float streak = 0.65f + 0.35f * Fbm(r * 9.0f, a * 4.0f, 23u);
                const float core = std::exp(-r * r * 40.0f);
                cover = glm::clamp(body * body * streak + core, 0.0f, 1.0f);
                heat = glm::clamp(core + 0.85f * (1.0f - r / std::max(edge, 1e-3f)) * streak, 0.0f, 1.0f);
            } else {
                const float t = (x + 0.5f) / w, s = std::abs((y + 0.5f) / h * 2.0f - 1.0f);
                float wide = 1.75f * std::pow(t, 0.38f) * std::pow(1.0f - t, 0.95f);
                wide *= 0.7f + 0.6f * Fbm(t * 5.0f, 0.5f, 31u);
                const float body = glm::clamp((wide - s) / std::max(wide * 0.7f, 1e-3f), 0.0f, 1.0f);
                const float torn = 0.6f + 0.4f * Fbm(t * 12.0f, s * 3.0f, 47u);
                const float core = std::exp(-std::pow(t * 5.0f, 2.0f)) * std::exp(-s * s * 12.0f);
                cover = glm::clamp(body * torn * std::pow(1.0f - t, 0.5f) + core, 0.0f, 1.0f);
                heat = glm::clamp(core + (1.0f - t) * 0.9f * (1.0f - s / std::max(wide, 1e-3f)) * torn, 0.0f, 1.0f);
            }
            const glm::vec3 c = colour(heat);
            float* px = &rgba[static_cast<size_t>((y * w + x) * 4)];
            px[0] = c.r;
            px[1] = c.g;
            px[2] = c.b;
            px[3] = cover;
        }
    }
    std::vector<TextureMip> mips;
    int mw = w, mh = h;
    std::vector<float> level = rgba;
    while (true) {
        std::vector<uint8_t>& px = Pixels().emplace_back(static_cast<size_t>(mw * mh * 4));
        for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>(glm::clamp(level[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        mips.push_back(TextureMip{static_cast<uint32_t>(mw), static_cast<uint32_t>(mh), std::span<const uint8_t>(px)});
        if (mw == 1 && mh == 1) break;
        const int nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
        std::vector<float> next(static_cast<size_t>(nw * nh * 4), 0.0f);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    float sum = 0.0f;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(mw - 1, x * 2 + dx), sy = std::min(mh - 1, y * 2 + dy);
                            sum += level[static_cast<size_t>((sy * mw + sx) * 4 + c)];
                        }
                    }
                    next[static_cast<size_t>((y * nw + x) * 4 + c)] = sum * 0.25f;
                }
            }
        }
        level.swap(next);
        mw = nw;
        mh = nh;
    }
    return textures.Create(name, gpu::Format::R8G8B8A8Unorm, std::span<const TextureMip>(mips));
}

uint32_t CreateWithMips(TextureManager& textures, const std::string& name, int w, int h, std::vector<float> rgba) {
    std::vector<TextureMip> mips;
    int mw = w, mh = h;
    std::vector<float> level = std::move(rgba);
    while (true) {
        std::vector<uint8_t>& px = Pixels().emplace_back(static_cast<size_t>(mw * mh * 4));
        for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>(glm::clamp(level[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        mips.push_back(TextureMip{static_cast<uint32_t>(mw), static_cast<uint32_t>(mh), std::span<const uint8_t>(px)});
        if (mw == 1 && mh == 1) break;
        const int nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
        std::vector<float> next(static_cast<size_t>(nw * nh * 4), 0.0f);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    float sum = 0.0f;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            sum += level[static_cast<size_t>((std::min(mh - 1, y * 2 + dy) * mw + std::min(mw - 1, x * 2 + dx)) * 4 + c)];
                        }
                    }
                    next[static_cast<size_t>((y * nw + x) * 4 + c)] = sum * 0.25f;
                }
            }
        }
        level.swap(next);
        mw = nw;
        mh = nh;
    }
    return textures.Create(name, gpu::Format::R8G8B8A8Unorm, std::span<const TextureMip>(mips));
}

uint32_t ContactTexture(TextureManager& textures) {
    const std::string name = "survival_contact";
    if (const uint32_t found = textures.Find(name); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    const int w = 64, h = 64;
    std::vector<float> rgba(static_cast<size_t>(w * h * 4), 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w * 2.0f - 1.0f, v = (y + 0.5f) / h * 2.0f - 1.0f;
            const float r = std::min(1.0f, std::sqrt(u * u + v * v));
            const float s = r * r * (3.0f - 2.0f * r);
            float* px = &rgba[static_cast<size_t>((y * w + x) * 4)];
            px[3] = std::pow(1.0f - s, 1.6f);
        }
    }
    return CreateWithMips(textures, name, w, h, std::move(rgba));
}

uint32_t CarContactTexture(TextureManager& textures) {
    const std::string name = "survival_car_contact";
    if (const uint32_t found = textures.Find(name); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    const int w = 64, h = 64;
    std::vector<float> rgba(static_cast<size_t>(w * h * 4), 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = std::abs((x + 0.5f) / w * 2.0f - 1.0f), v = std::abs((y + 0.5f) / h * 2.0f - 1.0f);
            const float d = std::clamp((std::max(u, v) - 0.85f) / 0.15f, 0.0f, 1.0f);
            float* px = &rgba[static_cast<size_t>((y * w + x) * 4)];
            px[0] = px[1] = px[2] = 1.0f;
            px[3] = 1.0f - d * d * (3.0f - 2.0f * d);
        }
    }
    return CreateWithMips(textures, name, w, h, std::move(rgba));
}

uint32_t MistTexture(TextureManager& textures) {
    const std::string name = "survival_blood_mist";
    if (const uint32_t found = textures.Find(name); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    const int w = 128, h = 64;
    std::vector<float> rgba(static_cast<size_t>(w * h * 4), 0.0f);
    constexpr int kLumps = 7;
    glm::vec2 lump[kLumps];
    float size[kLumps];
    for (int i = 0; i < kLumps; ++i) {
        const float along = static_cast<float>(i) / (kLumps - 1);
        lump[i] = {-0.62f + 1.3f * along + 0.12f * (Hash2(i, 1, 33u) - 0.5f), (0.15f + 0.45f * along) * (Hash2(i, 2, 33u) - 0.5f)};
        size[i] = 0.42f - 0.24f * along + 0.08f * Hash2(i, 3, 33u);
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w * 2.0f - 1.0f, v = (y + 0.5f) / h * 2.0f - 1.0f;
            float dense = 0.0f;
            for (int i = 0; i < kLumps; ++i) {
                const glm::vec2 d = (glm::vec2(u, v) - lump[i]) / size[i];
                dense += std::exp(-glm::dot(d, d) * 1.6f) * (1.0f - 0.45f * static_cast<float>(i) / kLumps);
            }
            dense *= 0.45f + 0.55f * Fbm(u * 3.0f + 7.0f, v * 3.0f + 7.0f, 91u);
            dense *= 0.7f + 0.3f * Fbm(u * 9.0f, v * 9.0f, 57u);
            const float ru = u + 0.8f, rv = v;
            const float angle = std::atan2(rv, std::max(ru, 1e-3f));
            const float ray = Fbm(angle * 22.0f + 3.0f, ru * 1.5f, 13u);
            const float fan = std::exp(-angle * angle * 6.0f) * glm::clamp(ru / 0.3f, 0.0f, 1.0f) * glm::clamp((1.9f - ru) / 0.6f, 0.0f, 1.0f);
            const float streak = std::pow(glm::clamp((ray - 0.55f) * 3.0f, 0.0f, 1.0f), 2.0f) * fan;
            const float edge = glm::clamp((1.0f - std::abs(v)) / 0.2f, 0.0f, 1.0f) * glm::clamp((1.0f - std::abs(u)) / 0.12f, 0.0f, 1.0f);
            const float cover = glm::clamp((dense * 0.95f + streak * 0.55f) * edge, 0.0f, 1.0f);
            const float shade = 0.75f + 0.25f * Fbm(u * 6.0f + 2.0f, v * 6.0f, 21u);
            float* px = &rgba[static_cast<size_t>((y * w + x) * 4)];
            px[0] = shade;
            px[1] = shade;
            px[2] = shade;
            px[3] = cover;
        }
    }
    return CreateWithMips(textures, name, w, h, std::move(rgba));
}

uint32_t FlareGlowTexture(TextureManager& textures) {
    const std::string name = "survival_flare_glow";
    if (const uint32_t found = textures.Find(name); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    constexpr int kSize = 128;
    std::vector<float> rgba(static_cast<size_t>(kSize * kSize * 4), 0.0f);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float u = (x + 0.5f) / kSize * 2.0f - 1.0f;
            const float v = (y + 0.5f) / kSize * 2.0f - 1.0f;
            const float r = std::sqrt(u * u + v * v);
            const float edge = glm::clamp((1.0f - r) / 0.18f, 0.0f, 1.0f);
            const float smooth_edge = edge * edge * (3.0f - 2.0f * edge);
            const float core = std::exp(-r * r * 18.0f);
            const float halo = std::exp(-r * r * 3.8f) * 0.65f + std::exp(-r * 2.4f) * 0.25f;
            const float val = std::clamp((core + halo) * smooth_edge, 0.0f, 1.0f);
            float* px = &rgba[static_cast<size_t>((y * kSize + x) * 4)];
            px[0] = val;
            px[1] = val;
            px[2] = val;
            px[3] = val;
        }
    }
    return CreateWithMips(textures, name, kSize, kSize, std::move(rgba));
}

uint32_t BloodTexture(TextureManager& textures, int variant) {
    const std::string name = std::format("survival_blood_pool_{}", variant);
    if (const uint32_t found = textures.Find(name); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    constexpr int kSize = 512;
    const uint32_t salt = 71u + 13u * static_cast<uint32_t>(variant);
    std::vector<float> field(static_cast<size_t>(kSize * kSize));
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float u = (x + 0.5f) / kSize * 2.0f - 1.0f, v = (y + 0.5f) / kSize * 2.0f - 1.0f;
            const float bu = u + 0.34f * (Fbm(u * 1.7f + 3.0f, v * 1.7f, salt) - 0.5f), bv = v + 0.34f * (Fbm(u * 1.7f, v * 1.7f + 7.0f, salt + 1u) - 0.5f);
            const float r = std::sqrt(bu * bu + bv * bv);
            const float angle = std::atan2(bv, bu);
            const float lobes = 0.5f + 0.34f * (Fbm(std::cos(angle) * 2.3f + 5.0f, std::sin(angle) * 2.3f, salt + 2u) - 0.5f) * 2.0f;
            const float rim = 0.16f * (Fbm(u * 11.0f, v * 11.0f, salt + 3u) - 0.5f) + 0.05f * (Fbm(u * 37.0f, v * 37.0f, salt + 4u) - 0.5f);
            field[static_cast<size_t>(y * kSize + x)] = (lobes - r) * 1.6f + rim;
        }
    }
    uint32_t seed = 0xB100Du + salt;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (int i = 0; i < 46; ++i) {
        const float a = 6.2831853f * rnd(), d = 0.55f + 0.38f * std::sqrt(rnd());
        const float size = (0.006f + 0.026f * rnd() * rnd()) * (1.1f - 0.6f * d);
        const float length = 1.0f + 2.6f * rnd();
        const glm::vec2 at(std::cos(a) * d, std::sin(a) * d), away(std::cos(a), std::sin(a));
        const int cx = static_cast<int>((at.x * 0.5f + 0.5f) * kSize), cy = static_cast<int>((at.y * 0.5f + 0.5f) * kSize);
        const int reach = static_cast<int>(size * length * kSize) + 3;
        for (int y = std::max(0, cy - reach); y < std::min(kSize, cy + reach); ++y) {
            for (int x = std::max(0, cx - reach); x < std::min(kSize, cx + reach); ++x) {
                const glm::vec2 p = glm::vec2((x + 0.5f) / kSize * 2.0f - 1.0f, (y + 0.5f) / kSize * 2.0f - 1.0f) - at;
                const float l = glm::dot(p, away), w = glm::dot(p, glm::vec2(-away.y, away.x));
                const float inside = 1.0f - std::sqrt(l * l / (length * length) + w * w) / (size * 2.0f);
                float& f = field[static_cast<size_t>(y * kSize + x)];
                f = std::max(f, inside * 0.25f);
            }
        }
    }
    std::vector<float> rgba(static_cast<size_t>(kSize * kSize * 4));
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float f = field[static_cast<size_t>(y * kSize + x)];
            const float u = (x + 0.5f) / kSize, v = (y + 0.5f) / kSize;
            const float frame = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
            const float deep = glm::clamp(f * 2.4f, 0.0f, 1.0f);
            const float red = glm::mix(0.11f, 0.03f, deep) * (0.82f + 0.36f * Fbm(u * 24.0f, v * 24.0f, salt + 5u));
            float* px = &rgba[static_cast<size_t>((y * kSize + x) * 4)];
            px[0] = red;
            px[1] = red * 0.07f;
            px[2] = red * 0.06f;
            px[3] = frame < 0.012f ? 0.0f : glm::clamp(0.5f + f * 90.0f, 0.0f, 1.0f);
        }
    }
    return CreateWithMips(textures, name, kSize, kSize, std::move(rgba));
}

uint32_t StippleTexture(TextureManager& textures) {
    if (const uint32_t found = textures.Find("survival_stipple"); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    constexpr int kSize = 64;
    std::vector<float> h(kSize * kSize, 0.0f);
    uint32_t seed = 0x57199u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (int k = 0; k < 140; ++k) {
        const float cx = rnd() * kSize, cy = rnd() * kSize, radius = 1.4f + rnd() * 1.2f;
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                float dx = std::abs(x + 0.5f - cx), dy = std::abs(y + 0.5f - cy);
                dx = std::min(dx, kSize - dx);
                dy = std::min(dy, kSize - dy);
                const float d = std::sqrt(dx * dx + dy * dy) / radius;
                if (d < 1.0f) h[y * kSize + x] = std::max(h[y * kSize + x], std::sqrt(1.0f - d * d));
            }
        }
    }
    std::vector<uint8_t>& px = Pixels().emplace_back(kSize * kSize * 4);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float hx = h[y * kSize + (x + 1) % kSize] - h[y * kSize + (x + kSize - 1) % kSize];
            const float hy = h[((y + 1) % kSize) * kSize + x] - h[((y + kSize - 1) % kSize) * kSize + x];
            const glm::vec3 n = glm::normalize(glm::vec3(-hx * 1.6f, -hy * 1.6f, 1.0f));
            uint8_t* p = &px[(y * kSize + x) * 4];
            p[0] = 128;
            p[1] = static_cast<uint8_t>(std::lround((n.y * 0.5f + 0.5f) * 255.0f));
            p[2] = 128;
            p[3] = static_cast<uint8_t>(std::lround((n.x * 0.5f + 0.5f) * 255.0f));
        }
    }
    const TextureMip mip{kSize, kSize, std::span<const uint8_t>(px)};
    return textures.Create("survival_stipple", gpu::Format::R8G8B8A8Unorm, std::span<const TextureMip>(&mip, 1));
}

uint32_t FacadeTexture(TextureManager& textures) {
    if (const uint32_t found = textures.Find("survival_facade"); found != TextureManager::kWhite && found != 0xFFFFFFFFu) return found;
    constexpr int kSize = 512;
    constexpr float kMetres = 6.0f;
    std::vector<glm::vec3> base(kSize * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float mx = (x + 0.5f) / kSize * kMetres, my = (y + 0.5f) / kSize * kMetres;
            const int course = static_cast<int>(my / 0.075f);
            const float along = mx + (course % 2 ? 0.11f : 0.0f);
            const bool mortar = std::fmod(my, 0.075f) < 0.01f || std::fmod(along, 0.22f) < 0.01f;
            const float soot = 0.75f + 0.5f * Fbm(mx * 0.6f, my * 0.6f, 7u);
            const float brick = 0.85f + 0.3f * Hash2(static_cast<int>(along / 0.22f), course, 11u);
            glm::vec3 c = mortar ? glm::vec3(0.060f, 0.055f, 0.050f) : glm::vec3(0.060f, 0.036f, 0.028f) * brick;
            c *= soot;
            const float bx = std::fmod(mx, 3.0f), by = std::fmod(my, 3.0f);
            if (bx > 0.9f && bx < 2.1f && by > 1.0f && by < 2.6f) {
                c = glm::vec3(0.006f, 0.006f, 0.007f) * (0.8f + 0.4f * Fbm(mx * 3.0f, my * 3.0f, 3u));
            } else if (bx > 0.8f && bx < 2.2f && by > 0.88f && by <= 1.0f) {
                c = glm::vec3(0.09f, 0.085f, 0.08f) * soot;
            }
            base[y * kSize + x] = c;
        }
    }
    std::vector<TextureMip> mips;
    std::vector<glm::vec3> level = base;
    for (int size = kSize; size >= 1; size /= 2) {
        std::vector<uint8_t>& px = Pixels().emplace_back(static_cast<size_t>(size) * size * 4);
        for (int i = 0; i < size * size; ++i) {
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(level[static_cast<size_t>(i)][c], 0.0f, 1.0f);
                const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                px[static_cast<size_t>(i) * 4 + c] = static_cast<uint8_t>(std::lround(s * 255.0f));
            }
            px[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        mips.push_back({static_cast<uint32_t>(size), static_cast<uint32_t>(size), std::span<const uint8_t>(px)});
        if (size == 1) break;
        std::vector<glm::vec3> next(static_cast<size_t>(size / 2) * (size / 2));
        for (int y = 0; y < size / 2; ++y) {
            for (int x = 0; x < size / 2; ++x) {
                next[static_cast<size_t>(y) * (size / 2) + x] = 0.25f * (level[static_cast<size_t>(2 * y) * size + 2 * x] + level[static_cast<size_t>(2 * y) * size + 2 * x + 1] +
                                                                         level[static_cast<size_t>(2 * y + 1) * size + 2 * x] +
                                                                         level[static_cast<size_t>(2 * y + 1) * size + 2 * x + 1]);
            }
        }
        level = std::move(next);
    }
    return textures.Create("survival_facade", gpu::Format::R8G8B8A8Srgb, std::span<const TextureMip>(mips));
}

template <typename Kit_, typename Block_>
void AddBackdrop(Kit_& k, size_t wall, size_t ground, std::vector<Block_>* blocks) {
    for (const glm::vec4 r : {glm::vec4(12.0f, 70.0f, -100.0f, 300.0f), glm::vec4(-80.0f, -18.0f, -100.0f, 300.0f),
                              glm::vec4(-18.0f, 12.0f, 190.0f, 300.0f)}) {
        k.Quad(ground, {r.x, -0.03f, r.z}, {r.y, -0.03f, r.z}, {r.y, -0.03f, r.w}, {r.x, -0.03f, r.w}, {0.0f, 1.0f, 0.0f});
    }
    uint32_t seed = 0x5EEDu;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (const float side : {1.0f, -1.0f}) {
        const float line = side > 0.0f ? 48.0f : -50.0f;
        for (float z = -80.0f; z < 280.0f;) {
            const float length = 16.0f + 12.0f * rnd(), height = 12.0f + 12.0f * rnd(), step = side * 4.0f * rnd();
            const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(line + step + side * 6.0f, height * 0.5f, z + length * 0.5f));
            const glm::vec3 half(6.0f, height * 0.5f, length * 0.5f);
            k.Box(wall, m, half, 0.0f, 1.0f / 6.0f);
            if (blocks) blocks->push_back({glm::inverse(m), half});
            z += length;
        }
    }
    for (const float end : {-80.0f, 280.0f}) {
        for (float x = -62.0f; x < 60.0f;) {
            const float length = 16.0f + 12.0f * rnd(), height = 12.0f + 12.0f * rnd();
            const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(x + length * 0.5f, height * 0.5f, end + (end < 0.0f ? -6.0f : 6.0f))) *
                                glm::rotate(glm::mat4(1.0f), kPi * 0.5f, glm::vec3(0, 1, 0));
            const glm::vec3 half(6.0f, height * 0.5f, length * 0.5f);
            k.Box(wall, m, half, 0.0f, 1.0f / 6.0f);
            if (blocks) blocks->push_back({glm::inverse(m), half});
            x += length;
        }
    }
}

Materials MakeMaterials(TextureManager& textures) {
    auto srm = [&](const char* name, float s, float r, float refl) {
        return SolidTexture(textures, std::string("survival_srm_") + name, static_cast<uint8_t>(s * 255.0f), static_cast<uint8_t>(r * 255.0f),
                            static_cast<uint8_t>(refl * 255.0f), 255);
    };
    const uint32_t stipple = StippleTexture(textures);
    auto lit = [&](glm::vec3 albedo, uint32_t specular, uint32_t normal = TextureManager::kFlatNormal) {
        MaterialGpu m;
        m.kind = gpu::kKindDeferred;
        m.albedo = TextureManager::kWhite;
        m.normal = normal;
        m.specular = specular;
        m.flags = gpu::kMatConstantColor | (normal != TextureManager::kFlatNormal ? gpu::kMatNormalMap : 0u);
        m.albedo_factor = glm::vec4(albedo, 1.0f);
        return textures.AddMaterial(m);
    };
    auto glow = [&](glm::vec3 color, float level) {
        MaterialGpu m;
        m.kind = gpu::kKindConstant;
        m.albedo = TextureManager::kWhite;
        m.albedo_factor = glm::vec4(color, 1.0f);
        m.params = glm::vec4(0.0f, level, 0.0f, 0.0f);
        return textures.AddMaterial(m);
    };
    Materials m;
    m.polymer = lit({0.030f, 0.030f, 0.032f}, srm("polymer", 0.22f, 0.70f, 0.0f));
    m.grip = lit({0.032f, 0.032f, 0.034f}, srm("grip", 0.18f, 0.82f, 0.0f), stipple);
    m.nitride = lit({0.04f, 0.04f, 0.043f}, srm("nitride", 0.45f, 0.4f, 0.03f));
    m.steel = lit({0.16f, 0.16f, 0.17f}, srm("steel", 0.75f, 0.32f, 0.08f));
    m.stainless = lit({0.13f, 0.13f, 0.14f}, srm("stainless", 0.8f, 0.3f, 0.06f));
    m.brass = lit({0.50f, 0.35f, 0.13f}, srm("brass", 0.8f, 0.3f, 0.08f));
    m.copper = lit({0.45f, 0.22f, 0.11f}, srm("copper", 0.75f, 0.34f, 0.06f));
    m.rubber = lit({0.022f, 0.022f, 0.022f}, srm("rubber", 0.08f, 0.9f, 0.0f), stipple);
    m.olive = lit({0.12f, 0.13f, 0.075f}, srm("olive", 0.3f, 0.6f, 0.02f));
    m.white_case = lit({0.62f, 0.62f, 0.60f}, srm("case", 0.35f, 0.45f, 0.05f));
    m.hull = lit({0.40f, 0.05f, 0.04f}, srm("hull", 0.35f, 0.45f, 0.02f));
    m.rust = lit({0.12f, 0.06f, 0.035f}, srm("rust", 0.22f, 0.86f, 0.0f), stipple);
    m.tape = lit({0.018f, 0.018f, 0.02f}, srm("tape", 0.3f, 0.55f, 0.0f));
    m.flare_paper = lit({0.55f, 0.05f, 0.04f}, srm("flare", 0.2f, 0.7f, 0.0f));
    m.flare_fire = glow({1.0f, 0.46f, 0.30f}, 1800.0f);
    {
        MaterialGpu tip_mat = *textures.Material(m.flare_fire);
        tip_mat.params.w = 2.5f;
        textures.UpdateMaterial(m.flare_fire, tip_mat);
    }
    m.box_paint = lit({0.19f, 0.2f, 0.19f}, srm("box_paint", 0.35f, 0.55f, 0.03f));
    m.label = lit({0.62f, 0.48f, 0.06f}, srm("label", 0.3f, 0.5f, 0.0f));
    m.photo = lit({0.36f, 0.04f, 0.035f}, srm("photo", 0.55f, 0.25f, 0.06f));
    m.iron = lit({0.03f, 0.028f, 0.026f}, srm("iron", 0.4f, 0.6f, 0.02f));
    m.red_glass = glow({1.0f, 0.07f, 0.045f}, 40.0f);
    m.dead_glass = lit({0.05f, 0.004f, 0.003f}, srm("dead_glass", 0.9f, 0.08f, 0.2f));
    m.flame = glow({1.0f, 0.45f, 0.18f}, 260.0f);
    m.wood = lit({0.055f, 0.032f, 0.02f}, srm("wood", 0.2f, 0.62f, 0.02f));
    m.wood_panel = lit({0.045f, 0.026f, 0.016f}, srm("wood_panel", 0.25f, 0.55f, 0.03f));
    m.beyond = glow({0.32f, 0.02f, 0.015f}, 3.0f);
    m.switch_on = glow({0.3f, 1.0f, 0.4f}, 60.0f);
    m.blood = lit({0.045f, 0.003f, 0.002f}, srm("blood", 0.75f, 0.12f, 0.15f));
    for (int variant = 0; variant < 3; ++variant) {
        MaterialGpu pool;
        pool.kind = gpu::kKindDeferred;
        pool.albedo = BloodTexture(textures, variant);
        pool.normal = TextureManager::kFlatNormal;
        pool.specular = srm("blood", 0.75f, 0.12f, 0.15f);
        pool.flags = gpu::kMatAlphaTest | gpu::kMatTwoSided;
        pool.params.x = 1.0f;
        pool.albedo_factor = glm::vec4(1.0f);
        m.pool[static_cast<size_t>(variant)] = textures.AddMaterial(pool);
    }
    m.fresh_blood = lit({0.085f, 0.0055f, 0.0045f}, srm("fresh_blood", 0.85f, 0.1f, 0.2f));
    m.wound = lit({0.035f, 0.002f, 0.002f}, srm("wound", 0.8f, 0.08f, 0.2f));
    m.flying_blood = glow({0.62f, 0.03f, 0.02f}, 30.0f);
    m.dark = lit({0.012f, 0.012f, 0.013f}, srm("dark", 0.2f, 0.7f, 0.0f));
    {
        MaterialGpu f;
        f.kind = gpu::kKindDeferred;
        f.albedo = FacadeTexture(textures);
        f.normal = TextureManager::kFlatNormal;
        f.specular = srm("facade", 0.05f, 0.92f, 0.0f);
        f.albedo_factor = glm::vec4(1.0f);
        m.facade = textures.AddMaterial(f);
    }
    m.backdrop_ground = lit({0.022f, 0.021f, 0.020f}, srm("backdrop_ground", 0.1f, 0.9f, 0.0f));
    m.bore = lit({0.0f, 0.0f, 0.0f}, srm("bore", 0.0f, 1.0f, 0.0f));
    m.tritium = glow({0.35f, 1.0f, 0.45f}, 55.0f);
    m.orange_dot = glow({1.0f, 0.45f, 0.08f}, 45.0f);
    m.cross = glow({1.0f, 0.08f, 0.06f}, 70.0f);
    m.flash = glow({1.0f, 0.62f, 0.26f}, 110.0f);
    m.spark = glow({1.0f, 0.42f, 0.14f}, 260.0f);
    MaterialGpu hot = *textures.Material(m.spark);
    hot.params.w = 2.5f;
    textures.UpdateMaterial(m.spark, hot);
    m.tracer = glow({1.0f, 0.85f, 0.6f}, 500.0f);
    m.lens = glow({1.0f, 0.96f, 0.88f}, 60.0f);
    for (const bool side : {false, true}) {
        MaterialGpu f;
        f.kind = gpu::kKindConstant;
        f.flags = gpu::kMatGlowClip;
        f.albedo = MuzzleFlashTexture(textures, side);
        f.albedo_factor = glm::vec4(1.0f);
        f.params = glm::vec4(0.0f, 420.0f, 0.0f, 3.0f);
        (side ? m.flash_side : m.flash_front) = textures.AddMaterial(f);
    }
    m.mist_texture = MistTexture(textures);
    m.flare_glow_texture = FlareGlowTexture(textures);
    m.contact_texture = ContactTexture(textures);
    m.car_contact_texture = CarContactTexture(textures);
    return m;
}

void BuildHandgun(ModelCache& models, const Materials& mt, SurvivalGunModel& out, Owned& owned) {
    {
        Kit k;
        const size_t frame = k.Part(mt.polymer), grip = k.Part(mt.grip), steel = k.Part(mt.steel), dark = k.Part(mt.dark);
        k.Extrude(frame, glm::mat4(1.0f),
                  {{-0.024f, 0.030f}, {-0.031f, 0.020f}, {-0.030f, 0.008f}, {0.010f, 0.006f}, {0.068f, 0.008f}, {0.074f, 0.013f}, {0.152f, 0.015f},
                   {0.153f, 0.030f}},
                  0.0118f, 0.0016f);
        k.Extrude(grip, glm::mat4(1.0f),
                  {{-0.030f, 0.010f}, {-0.036f, -0.020f}, {-0.042f, -0.060f}, {-0.047f, -0.100f}, {-0.012f, -0.103f}, {-0.006f, -0.075f},
                   {-0.002f, -0.050f}, {0.004f, -0.030f}, {0.012f, -0.012f}, {0.012f, 0.008f}},
                  0.0128f, 0.0022f, 260.0f);
        k.Extrude(frame, glm::mat4(1.0f),
                  {{0.066f, 0.008f}, {0.071f, -0.010f}, {0.064f, -0.026f}, {0.048f, -0.031f}, {0.018f, -0.031f}, {0.008f, -0.024f}, {0.012f, -0.016f},
                   {0.020f, -0.024f}, {0.046f, -0.024f}, {0.058f, -0.020f}, {0.063f, -0.008f}, {0.060f, 0.008f}},
                  0.0082f, 0.001f);
        for (int i = 0; i < 4; ++i) k.Box(frame, T({0.0f, 0.0128f, 0.093f + 0.014f * i}), {0.0105f, 0.0016f, 0.0045f}, 0.0006f);
        k.Extrude(steel, glm::mat4(1.0f), {{0.028f, 0.006f}, {0.033f, 0.006f}, {0.035f, -0.006f}, {0.032f, -0.017f}, {0.028f, -0.019f}, {0.030f, -0.008f}},
                  0.0032f, 0.0006f);
        k.Box(dark, T({0.0f, -0.006f, 0.0322f}), {0.0012f, 0.006f, 0.0012f});
        k.Box(steel, T({0.0128f, 0.024f, 0.048f}), {0.0012f, 0.0026f, 0.008f}, 0.0004f);
        k.Box(steel, T({0.0124f, 0.019f, 0.074f}), {0.0010f, 0.0020f, 0.004f}, 0.0003f);
        k.Cylinder(steel, Along(0.0f, 0.036f, 0.150f), 0.0028f, 0.017f, 10);
        out.body = Keep(owned, k.Upload(models, "survival_handgun_frame"));
    }
    {
        Kit k;
        const size_t slide = k.Part(mt.nitride), steel = k.Part(mt.steel), dark = k.Part(mt.dark), bore = k.Part(mt.bore), glow = k.Part(mt.tritium, true);
        k.Extrude(slide, glm::mat4(1.0f),
                  {{-0.022f, 0.030f}, {0.166f, 0.030f}, {0.170f, 0.034f}, {0.170f, 0.056f}, {0.164f, 0.064f}, {-0.016f, 0.064f}, {-0.022f, 0.058f}},
                  0.0126f, 0.0022f);
        for (int i = 0; i < 8; ++i) {
            for (const float s : {1.0f, -1.0f}) {
                k.Box(dark, T({s * 0.0124f, 0.047f, -0.017f + 0.0026f * i}), {0.0006f, 0.0115f, 0.0006f});
                if (i < 5) k.Box(dark, T({s * 0.0124f, 0.047f, 0.128f + 0.0026f * i}), {0.0006f, 0.0105f, 0.0006f});
            }
        }
        k.Box(dark, T({-0.0055f, 0.0635f, 0.052f}), {0.0072f, 0.0012f, 0.022f});
        k.Box(steel, T({-0.0042f, 0.0625f, 0.052f}), {0.0058f, 0.0012f, 0.0205f}, 0.0005f);
        k.Cylinder(steel, Along(0.0f, 0.045f, 0.1665f), 0.0068f, 0.0045f, 16);
        k.Disc(bore, T({0.0f, 0.045f, 0.1712f}), 0.0045f, 16);
        for (const float s : {1.0f, -1.0f}) {
            k.Box(slide, T({s * 0.0068f, 0.0675f, -0.007f}), {0.0042f, 0.0036f, 0.0052f}, 0.0006f);
            k.Box(glow, T({s * 0.0068f, 0.0682f, -0.0018f}), {0.0011f, 0.0011f, 0.0003f});
        }
        k.Box(slide, T({0.0f, 0.0672f, 0.154f}), {0.0017f, 0.0035f, 0.0045f}, 0.0005f);
        k.Box(glow, T({0.0f, 0.0684f, 0.1495f}), {0.0011f, 0.0011f, 0.0003f});
        out.moving = Keep(owned, k.Upload(models, "survival_handgun_slide"));
    }
    {
        Kit k;
        const size_t body = k.Part(mt.polymer), steel = k.Part(mt.steel), brass = k.Part(mt.brass);
        k.Extrude(steel, glm::mat4(1.0f), {{-0.040f, -0.098f}, {-0.012f, -0.098f}, {-0.002f, 0.018f}, {-0.028f, 0.018f}}, 0.0105f, 0.001f);
        k.Box(body, T({0.0f, -0.106f, -0.029f}), {0.0136f, 0.0042f, 0.0205f}, 0.0018f);
        k.Cylinder(brass, glm::rotate(T({0.0f, 0.021f, -0.026f}), 0.0f, glm::vec3(0, 1, 0)), 0.0046f, 0.018f, 10);
        out.magazine = Keep(owned, k.Upload(models, "survival_handgun_magazine"));
    }
    out.muzzle = {0.0f, 0.045f, 0.172f};
    out.ejection = {-0.012f, 0.062f, 0.050f};
    out.sight_rear = {0.0f, 0.0683f, -0.002f};
    out.sight_front = {0.0f, 0.0684f, 0.1495f};
    out.wrist = {-0.025f, -0.03f, -0.074f};
    out.rake = 0.26f;
}

void BuildShotgun(ModelCache& models, const Materials& mt, SurvivalGunModel& out, Owned& owned) {
    {
        Kit k;
        const size_t receiver = k.Part(mt.nitride), polymer = k.Part(mt.polymer), grip = k.Part(mt.grip), steel = k.Part(mt.steel),
                     dark = k.Part(mt.dark), bore = k.Part(mt.bore), dot = k.Part(mt.orange_dot, true), rubber = k.Part(mt.rubber);
        k.Extrude(receiver, glm::mat4(1.0f),
                  {{-0.075f, 0.030f}, {0.130f, 0.026f}, {0.132f, 0.074f}, {0.120f, 0.082f}, {-0.060f, 0.082f}, {-0.078f, 0.070f}}, 0.0175f, 0.003f);
        k.Box(receiver, T({0.0f, 0.0855f, 0.030f}), {0.0105f, 0.0035f, 0.085f}, 0.001f);
        for (int i = 0; i < 9; ++i) k.Box(dark, T({0.0f, 0.0893f, -0.045f + 0.019f * i}), {0.0107f, 0.0006f, 0.0035f});
        k.Box(dark, T({0.0f, 0.0262f, 0.070f}), {0.0115f, 0.0012f, 0.045f});
        k.Box(dark, T({-0.0176f, 0.064f, 0.050f}), {0.0006f, 0.0095f, 0.032f});
        k.Cylinder(steel, Along(0.0f, 0.058f, 0.128f), 0.0118f, 0.50f, 18);
        k.Disc(bore, T({0.0f, 0.058f, 0.6285f}), 0.0092f, 16);
        k.Cylinder(receiver, Along(0.0f, 0.030f, 0.128f), 0.0110f, 0.43f, 16);
        k.Cylinder(receiver, Along(0.0f, 0.030f, 0.556f), 0.0125f, 0.022f, 16);
        k.Box(receiver, T({0.0f, 0.044f, 0.565f}), {0.008f, 0.012f, 0.008f}, 0.002f);
        k.Cylinder(dot, Along(0.0f, 0.0715f, 0.612f), 0.0024f, 0.004f, 10);
        for (const float s : {1.0f, -1.0f}) k.Box(receiver, T({s * 0.0075f, 0.0975f, 0.098f}), {0.0025f, 0.0085f, 0.004f}, 0.0008f);
        k.Box(receiver, T({0.0f, 0.1075f, 0.098f}), {0.010f, 0.0015f, 0.004f}, 0.0005f);
        k.Box(receiver, T({0.0f, 0.0905f, 0.098f}), {0.010f, 0.0012f, 0.006f}, 0.0005f);
        k.Extrude(receiver, glm::mat4(1.0f),
                  {{0.042f, 0.030f}, {0.046f, 0.008f}, {0.036f, -0.006f}, {-0.012f, -0.006f}, {-0.020f, 0.006f}, {-0.012f, 0.008f}, {-0.008f, 0.001f},
                   {0.034f, 0.001f}, {0.039f, 0.010f}, {0.036f, 0.030f}},
                  0.009f, 0.001f);
        k.Extrude(steel, glm::mat4(1.0f), {{0.012f, 0.030f}, {0.017f, 0.030f}, {0.019f, 0.016f}, {0.015f, 0.006f}, {0.011f, 0.006f}, {0.014f, 0.016f}}, 0.0035f,
                  0.0006f);
        k.Extrude(grip, glm::mat4(1.0f),
                  {{-0.050f, 0.032f}, {-0.012f, 0.032f}, {-0.016f, 0.000f}, {-0.034f, -0.080f}, {-0.040f, -0.096f}, {-0.074f, -0.092f}, {-0.072f, -0.070f},
                   {-0.060f, -0.010f}},
                  0.0148f, 0.0035f, 240.0f);
        k.Cylinder(receiver, Along(0.0f, 0.052f, -0.330f), 0.0155f, 0.26f, 16);
        k.Extrude(polymer, glm::mat4(1.0f),
                  {{-0.380f, -0.040f}, {-0.240f, 0.020f}, {-0.230f, 0.048f}, {-0.250f, 0.074f}, {-0.380f, 0.078f}}, 0.019f, 0.004f);
        k.Extrude(rubber, glm::mat4(1.0f), {{-0.398f, -0.044f}, {-0.380f, -0.040f}, {-0.380f, 0.078f}, {-0.398f, 0.081f}}, 0.0215f, 0.003f, 260.0f);
        out.body = Keep(owned, k.Upload(models, "survival_shotgun_body"));
    }
    {
        Kit k;
        const size_t pump = k.Part(mt.polymer), dark = k.Part(mt.dark);
        k.Extrude(pump, glm::mat4(1.0f), {{0.200f, 0.012f}, {0.380f, 0.012f}, {0.384f, 0.020f}, {0.384f, 0.052f}, {0.376f, 0.056f}, {0.208f, 0.056f}, {0.200f, 0.050f}},
                  0.0215f, 0.005f);
        for (int i = 0; i < 9; ++i) {
            for (const float s : {1.0f, -1.0f}) k.Box(dark, T({s * 0.0212f, 0.032f, 0.222f + 0.0175f * i}), {0.0008f, 0.015f, 0.0028f});
        }
        out.moving = Keep(owned, k.Upload(models, "survival_shotgun_pump"));
    }
    out.muzzle = {0.0f, 0.058f, 0.630f};
    out.ejection = {-0.018f, 0.064f, 0.050f};
    out.sight_rear = {0.0f, 0.0990f, 0.098f};
    out.sight_front = {0.0f, 0.0740f, 0.612f};
    out.wrist = {-0.027f, -0.008f, -0.097f};
    out.rake = 0.22f;
    out.support = {0.045f, -0.045f, -0.05f};
    out.support_turn = {0.0f, -1.0f, 0.35f};
}

void BuildRevolver(ModelCache& models, const Materials& mt, SurvivalGunModel& out, Owned& owned) {
    constexpr float kAxisY = 0.046f;
    {
        Kit k;
        const size_t frame = k.Part(mt.stainless), rubber = k.Part(mt.rubber), steel = k.Part(mt.steel), dark = k.Part(mt.dark),
                     bore = k.Part(mt.bore), dot = k.Part(mt.orange_dot, true);
        k.Extrude(frame, glm::mat4(1.0f),
                  {{-0.040f, 0.018f}, {-0.013f, 0.018f}, {-0.013f, 0.066f}, {-0.020f, 0.071f}, {-0.031f, 0.0715f}, {-0.038f, 0.066f}, {-0.041f, 0.056f}},
                  0.0128f, 0.0035f);
        k.Extrude(frame, glm::mat4(1.0f), {{-0.022f, 0.0655f}, {0.062f, 0.0655f}, {0.062f, 0.0725f}, {0.056f, 0.0745f}, {-0.016f, 0.0745f}, {-0.024f, 0.0715f}},
                  0.0086f, 0.0026f);
        k.Extrude(frame, glm::mat4(1.0f), {{0.0455f, 0.020f}, {0.062f, 0.020f}, {0.064f, 0.026f}, {0.064f, 0.068f}, {0.0455f, 0.0665f}}, 0.0104f, 0.0028f);
        k.Extrude(frame, glm::mat4(1.0f), {{-0.013f, 0.0175f}, {0.046f, 0.0175f}, {0.046f, 0.0255f}, {-0.013f, 0.0255f}}, 0.0098f, 0.002f);
        k.Cylinder(frame, Along(0.0f, kAxisY + 0.004f, 0.062f), 0.0098f, 0.103f, 22);
        k.Cylinder(steel, Along(0.0f, kAxisY + 0.004f, 0.163f), 0.0085f, 0.0035f, 22);
        k.Extrude(frame, glm::mat4(1.0f), {{0.062f, 0.021f}, {0.150f, 0.024f}, {0.162f, 0.030f}, {0.165f, 0.040f}, {0.165f, 0.050f}, {0.062f, 0.050f}}, 0.0074f, 0.0032f);
        k.Box(frame, T({0.0f, 0.0615f, 0.112f}), {0.0032f, 0.0028f, 0.05f}, 0.0012f);
        for (int i = 0; i < 6; ++i) k.Box(dark, T({0.0f, 0.0615f, 0.075f + 0.015f * i}), {0.0033f, 0.0018f, 0.0035f});
        k.Disc(bore, T({0.0f, kAxisY + 0.004f, 0.1667f}), 0.0046f, 16);
        k.Extrude(frame, glm::mat4(1.0f), {{0.146f, 0.059f}, {0.162f, 0.059f}, {0.162f, 0.0745f}, {0.152f, 0.0745f}}, 0.0018f, 0.0004f);
        k.Box(dot, T({0.0f, 0.0715f, 0.1548f}), {0.0019f, 0.0022f, 0.0036f});
        for (const float s : {1.0f, -1.0f}) k.Box(steel, T({s * 0.0045f, 0.0755f, -0.006f}), {0.0028f, 0.0028f, 0.0065f}, 0.0006f);
        k.Extrude(frame, glm::mat4(1.0f),
                  {{0.032f, 0.018f}, {0.034f, -0.004f}, {0.024f, -0.018f}, {-0.006f, -0.018f}, {-0.012f, -0.008f}, {-0.006f, -0.004f}, {-0.002f, -0.011f},
                   {0.022f, -0.011f}, {0.027f, -0.002f}, {0.026f, 0.018f}},
                  0.0078f, 0.001f);
        k.Extrude(steel, glm::mat4(1.0f), {{0.006f, 0.018f}, {0.012f, 0.018f}, {0.014f, 0.004f}, {0.010f, -0.008f}, {0.006f, -0.009f}, {0.008f, 0.004f}}, 0.0034f,
                  0.0006f);
        k.Box(steel, T({0.0134f, 0.046f, -0.024f}), {0.0012f, 0.0035f, 0.006f}, 0.0005f);
        k.Extrude(rubber, glm::mat4(1.0f),
                  {{-0.036f, 0.020f}, {-0.058f, -0.018f}, {-0.066f, -0.078f}, {-0.054f, -0.100f}, {-0.022f, -0.096f}, {-0.016f, -0.078f}, {-0.010f, -0.066f},
                   {-0.016f, -0.054f}, {-0.010f, -0.040f}, {-0.016f, -0.028f}, {-0.006f, 0.000f}, {-0.004f, 0.018f}},
                  0.0168f, 0.0045f, 240.0f);
        out.body = Keep(owned, k.Upload(models, "survival_revolver_frame"));
        (void)dark;
    }
    {
        Kit k;
        const size_t cyl = k.Part(mt.stainless), dark = k.Part(mt.dark), copper = k.Part(mt.copper);
        k.Cylinder(cyl, Along(0.0f, 0.0f, -0.0085f), 0.0205f, 0.0520f, 30);
        k.Cylinder(cyl, Along(0.0f, 0.0f, -0.0105f), 0.0185f, 0.0560f, 30);
        for (int i = 0; i < 6; ++i) {
            const float a = 2.0f * kPi * (static_cast<float>(i) + 0.5f) / 6.0f;
            const glm::vec3 at(std::cos(a) * 0.0196f, std::sin(a) * 0.0196f, 0.0215f);
            k.Box(dark, glm::rotate(T(at), a, glm::vec3(0, 0, 1)), {0.0012f, 0.0034f, 0.0175f});
            const float b = 2.0f * kPi * static_cast<float>(i) / 6.0f;
            const glm::vec3 chamber(std::cos(b) * 0.0122f, std::sin(b) * 0.0122f, 0.0457f);
            k.Disc(dark, T(chamber + glm::vec3(0, 0, 0.0002f)), 0.0046f, 12);
            k.Disc(copper, T(chamber + glm::vec3(0, 0, -0.0012f)), 0.0038f, 12);
        }
        out.moving = Keep(owned, k.Upload(models, "survival_revolver_cylinder"));
    }
    {
        Kit k;
        const size_t steel = k.Part(mt.steel), dark = k.Part(mt.dark);
        k.Extrude(steel, glm::mat4(1.0f), {{-0.004f, -0.008f}, {0.006f, 0.000f}, {0.002f, 0.020f}, {-0.008f, 0.028f}, {-0.016f, 0.026f}, {-0.012f, 0.014f}}, 0.0038f,
                  0.0008f);
        for (int i = 0; i < 4; ++i) k.Box(dark, T({0.0f, 0.0255f - 0.0012f * i, -0.0085f - 0.0018f * i}), {0.0039f, 0.0004f, 0.0006f});
        out.hammer = Keep(owned, k.Upload(models, "survival_revolver_hammer"));
    }
    out.muzzle = {0.0f, kAxisY + 0.004f, 0.167f};
    out.cylinder_axis = {0.0f, kAxisY, 0.0f};
    out.hammer_pivot = {0.0f, 0.052f, -0.030f};
    out.sight_rear = {0.0f, 0.0783f, -0.006f};
    out.sight_front = {0.0f, 0.0745f, 0.157f};
    out.wrist = {-0.025f, -0.025f, -0.09f};
    out.support = {0.031f, -0.045f, -0.075f};
    out.rake = 0.25f;
}

void ReadReloads(const std::filesystem::path& dir, std::array<SurvivalGunModel, 6>& guns, const char* const* names) {
    const std::optional<std::vector<uint8_t>> listing = mods::ReadDiskFile(dir / "reloads.json");
    json::Value doc;
    std::string error;
    if (!listing || !json::Parse(std::string_view(reinterpret_cast<const char*>(listing->data()), listing->size()), doc, &error)) {
        LogWarn("survival: survival/reloads.json {}, the guns reload in place", listing ? error : std::string("missing"));
        return;
    }
    auto vec3 = [](const json::Value& g, const char* key, glm::vec3 fallback) {
        const json::Value* v = g.Find(key);
        if (!v || v->Size() != 3) return fallback;
        return glm::vec3(static_cast<float>(v->At(0)->number), static_cast<float>(v->At(1)->number), static_cast<float>(v->At(2)->number));
    };
    auto shape = [&](const std::string& name, const std::array<glm::vec4, 5>& fallback) {
        const json::Value* shapes = doc.Find("shapes");
        const json::Value* v = shapes ? shapes->Find(name.c_str()) : nullptr;
        if (!v || v->Size() != 5) return fallback;
        std::array<glm::vec4, 5> out = fallback;
        for (size_t f = 0; f < 5; ++f) {
            const json::Value* finger = v->At(f);
            for (size_t k = 0; finger && k < 4 && k < finger->Size(); ++k) out[f][static_cast<glm::length_t>(k)] = static_cast<float>(finger->At(k)->number);
        }
        return out;
    };
    for (size_t w = 1; w < guns.size(); ++w) {
        const json::Value* g = doc.Find(names[w]);
        const json::Value* keys = g ? g->Find("keys") : nullptr;
        if (!keys) continue;
        SurvivalReload& reload = guns[w].reload;
        reload.item_at = vec3(*g, "item_at", reload.item_at);
        reload.item_turn = vec3(*g, "item_turn", reload.item_turn);
        reload.eject = static_cast<float>(g->Number("eject", reload.eject));
        SurvivalReloadKey key;
        key.fingers = guns[w].support_fingers;
        for (size_t i = 0; i < keys->Size(); ++i) {
            const json::Value& k = *keys->At(i);
            key.t = static_cast<float>(k.Number("t", key.t));
            key.at = vec3(k, "at", key.at);
            key.turn = vec3(k, "turn", key.turn);
            key.hand = static_cast<float>(k.Number("hand", key.hand));
            key.left = vec3(k, "left", key.left);
            key.away = static_cast<float>(k.Number("away", key.away));
            key.elbow = static_cast<float>(k.Number("elbow", key.elbow));
            key.thumb = static_cast<float>(k.Number("thumb", key.thumb));
            if (const json::Value* v = k.Find("right_thumb"); v && v->Size() == 4) {
                for (size_t c = 0; c < 4; ++c) key.right_thumb[static_cast<glm::length_t>(c)] = static_cast<float>(v->At(c)->number);
            }
            key.item = static_cast<float>(k.Number("item", key.item));
            key.seat = static_cast<float>(k.Number("seat", key.seat));
            key.spent = static_cast<float>(k.Number("spent", key.spent));
            key.open = static_cast<float>(k.Number("open", key.open));
            if (const std::string name = k.String("fingers"); !name.empty()) key.fingers = shape(name, key.fingers);
            if (k.Find("fingers_to") && k.Find("palm_to")) {
                const glm::vec3 to = vec3(k, "fingers_to", glm::vec3(0.0f, 0.0f, 1.0f));
                const glm::vec3 x = glm::length(to) > 1.0e-4f ? glm::normalize(to) : glm::vec3(0.0f, 0.0f, 1.0f);
                glm::vec3 palm = vec3(k, "palm_to", glm::vec3(0.0f, 1.0f, 0.0f));
                palm -= x * glm::dot(palm, x);
                if (glm::length(palm) > 1.0e-4f) {
                    palm = glm::normalize(palm);
                    key.left_turn = glm::quat_cast(glm::mat3(x, -palm, glm::cross(x, -palm)));
                }
            }
            if (k.Find("item_to")) key.left = vec3(k, "item_to", glm::vec3(0.0f)) - glm::mat3_cast(key.left_turn) * reload.item_at;
            reload.keys.push_back(key);
        }
        LogInfo("survival: the {}'s reload in {} keys", names[w], reload.keys.size());
    }
}

void LoadGunFiles(ModelCache& models, SurvivalModels& out_models, bool include_guns, bool include_gameplay, int slot_filter = 0) {
    auto& guns = out_models.guns;
    const std::filesystem::path dir = ExecutableDir() / "survival";
    const std::optional<std::vector<uint8_t>> listing = mods::ReadDiskFile(dir / "guns.json");
    if (!listing) {
        LogWarn("survival: no survival/guns.json beside the game, the guns are built");
        return;
    }
    json::Value doc;
    std::string error;
    if (!json::Parse(std::string_view(reinterpret_cast<const char*>(listing->data()), listing->size()), doc, &error)) {
        LogWarn("survival: survival/guns.json unreadable ({}), the guns are built", error);
        return;
    }
    auto vec3 = [](const json::Value& g, const char* key, glm::vec3 fallback) {
        const json::Value* v = g.Find(key);
        if (!v || v->Size() != 3) return fallback;
        return glm::vec3(static_cast<float>(v->At(0)->number), static_cast<float>(v->At(1)->number), static_cast<float>(v->At(2)->number));
    };
    auto fingers = [](const json::Value& g, const char* key, const std::array<glm::vec4, 5>& fallback) {
        const json::Value* v = g.Find(key);
        if (!v || v->Size() != 5) return fallback;
        std::array<glm::vec4, 5> out = fallback;
        for (size_t f = 0; f < 5; ++f) {
            const json::Value* finger = v->At(f);
            if (!finger || finger->Size() != 4) return fallback;
            for (size_t k = 0; k < 4; ++k) out[f][static_cast<glm::length_t>(k)] = static_cast<float>(finger->At(k)->number);
        }
        return out;
    };
    std::unordered_map<std::string, std::optional<std::vector<uint8_t>>> file_cache;
    auto read_cached = [&](const std::string& file) -> const std::vector<uint8_t>* {
        auto it = file_cache.find(file);
        if (it == file_cache.end()) {
            it = file_cache.emplace(file, mods::ReadDiskFile(dir / file)).first;
        }
        return it->second ? &(*it->second) : nullptr;
    };
    auto part = [&](const json::Value& g, const char* key, std::vector<SurvivalGunModel::SkinMaterial>* collect = nullptr) -> const GpuMesh* {
        const json::Value* parts = g.Find("parts");
        const std::string file = g.String("file");
        const std::string node = parts ? parts->String(key) : std::string();
        if (file.empty() || node.empty()) return nullptr;
        const std::vector<uint8_t>* bytes = read_cached(file);
        FmdlModel model;
        std::string why = "missing";
        const std::string asset = "/Assets/pt_port/survival/" + file;
        if (!bytes || !LoadGltfModel(*bytes, asset, nullptr, model, &why, node) || model.mesh.vertices.empty()) {
            LogWarn("survival: survival/{} node {} not read ({})", file, node, why);
            return nullptr;
        }
        std::vector<uint32_t> src_mats;
        std::vector<std::string> src_names;
        std::vector<std::shared_ptr<SkinBakeSource>> sub_sources;
        if (collect) {
            src_mats.reserve(model.mesh.submeshes.size());
            for (const SubMesh& sub : model.mesh.submeshes) {
                src_mats.push_back(sub.material);
                src_names.push_back(sub.material < model.materials.size() ? model.materials[sub.material].name : std::string());
                auto source = std::make_shared<SkinBakeSource>();
                const auto& vertices = model.mesh.vertices;
                const auto& indices = model.mesh.indices;
                for (uint32_t k = 0; k + 2 < sub.index_count && static_cast<size_t>(sub.first_index) + k + 2 < indices.size(); k += 3) {
                    Vertex corner[3];
                    bool ok = true;
                    for (uint32_t c = 0; c < 3; ++c) {
                        const int64_t vi = static_cast<int64_t>(indices[sub.first_index + k + c]) + sub.vertex_offset;
                        if (vi < 0 || static_cast<size_t>(vi) >= vertices.size()) {
                            ok = false;
                            break;
                        }
                        corner[c] = vertices[static_cast<size_t>(vi)];
                    }
                    if (!ok) continue;
                    for (const Vertex& v : corner) {
                        source->position.push_back(v.position);
                        source->normal.push_back(v.normal);
                        source->uv.push_back(v.uv0);
                    }
                }
                if (sub.material < model.materials.size()) source->base_path = model.materials[sub.material].base_color;
                sub_sources.push_back(std::move(source));
            }
        }
        const ModelEntry* entry = models.Get(asset + "#" + node, &model);
        if (entry && entry->mesh && collect) {
            TextureManager& tm = models.Textures();
            const size_t n = std::min(src_mats.size(), entry->mesh->submeshes.size());
            for (size_t i = 0; i < n; ++i) {
                const uint32_t gpu_mat = entry->mesh->submeshes[i].material;
                if (gpu_mat == 0) continue;
                bool dup = false;
                for (auto& existing : *collect) {
                    if (existing.index == gpu_mat) {
                        if (existing.source && i < sub_sources.size()) {
                            const SkinBakeSource& more = *sub_sources[i];
                            existing.source->position.insert(existing.source->position.end(), more.position.begin(), more.position.end());
                            existing.source->normal.insert(existing.source->normal.end(), more.normal.begin(), more.normal.end());
                            existing.source->uv.insert(existing.source->uv.end(), more.uv.begin(), more.uv.end());
                        }
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                const MaterialGpu* orig = tm.Material(gpu_mat);
                if (!orig) continue;
                const std::string mname = i < src_names.size() ? src_names[i] : std::string();
                uint8_t channel = 0;
                if (mname.find("wood") != std::string::npos || mname.find("grip") != std::string::npos) {
                    channel = 1;
                } else if (mname.find("hammer") != std::string::npos || mname.find("plain") != std::string::npos) {
                    channel = 2;
                }
                std::shared_ptr<SkinBakeSource> source = i < sub_sources.size() ? sub_sources[i] : nullptr;
                if (source) {
                    source->channel = channel;
                    source->key = std::format("{}_{}", node, gpu_mat);
                }
                LogInfo("survival: skin surface {} ({}) channel {}: {} triangles, image {}", source ? source->key : std::string("-"), mname, channel,
                        source ? source->uv.size() / 3 : 0, source && !source->base_path.empty() ? source->base_path : std::string("none"));
                collect->push_back({gpu_mat, channel, *orig, std::move(source)});
            }
        }
        return entry ? entry->mesh.get() : nullptr;
    };
    static const char* const kNames[] = {"", "handgun", "shotgun", "revolver", "lever", "double"};
    if (include_guns) {
        if ((slot_filter == 0 || slot_filter == 6) && (out_models.armory_slots_loaded & (1u << 6)) == 0) {
            if (const json::Value* f = doc.Find("flashlight")) {
                out_models.chest_materials.clear();
                out_models.chest_light = part(*f, "body", &out_models.chest_materials);
                out_models.chest_lens = vec3(*f, "lens", out_models.chest_lens);
                out_models.chest_clip = vec3(*f, "clip", out_models.chest_clip);
                out_models.chest_mount = vec3(*f, "mount", out_models.chest_mount);
                out_models.chest_turn = vec3(*f, "mount_turn", out_models.chest_turn);
                LogInfo("survival: the chest flashlight from survival/guns.json ({}, {} mats)", out_models.chest_light ? "read" : "missing",
                        out_models.chest_materials.size());
            }
            out_models.armory_slots_loaded |= (1u << 6);
        }
        json::Value carry_doc;
        bool carry_read = false;
        if (const std::optional<std::vector<uint8_t>> carried = mods::ReadDiskFile(dir / "carry_poses.json")) {
            std::string carry_error;
            carry_read = json::Parse(std::string_view(reinterpret_cast<const char*>(carried->data()), carried->size()), carry_doc, &carry_error);
            if (!carry_read) LogWarn("survival: survival/carry_poses.json unreadable ({})", carry_error);
        }
        const auto read_carry = [&](const char* name, SurvivalGunModel& gun) {
            const json::Value* poses = carry_read ? carry_doc.Find(name) : nullptr;
            if (!poses) return;
            static const char* const kStates[2] = {"ready", "run"};
            SurvivalGunModel::CarryPose carry;
            for (size_t s = 0; s < 2; ++s) {
                const json::Value* state = poses->Find(kStates[s]);
                const json::Value* m = state ? state->Find("gun") : nullptr;
                const json::Value* rp = state ? state->Find("right_pole") : nullptr;
                const json::Value* lp = state ? state->Find("left_pole") : nullptr;
                if (!m || m->Size() != 16 || !rp || rp->Size() != 3 || !lp || lp->Size() != 3) return;
                for (size_t k = 0; k < 16; ++k) carry.gun[s][static_cast<glm::length_t>(k / 4)][static_cast<glm::length_t>(k % 4)] = static_cast<float>(m->At(k)->number);
                for (size_t k = 0; k < 3; ++k) {
                    carry.right_pole[s][static_cast<glm::length_t>(k)] = static_cast<float>(rp->At(k)->number);
                    carry.left_pole[s][static_cast<glm::length_t>(k)] = static_cast<float>(lp->At(k)->number);
                }
            }
            carry.on = true;
            gun.carry = carry;
            LogInfo("survival: the {}'s carried poses from survival/carry_poses.json", name);
        };
        for (size_t w = 1; w < guns.size(); ++w) {
            if (slot_filter != 0 && slot_filter != static_cast<int>(w)) continue;
            if ((out_models.armory_slots_loaded & (1u << static_cast<unsigned>(w))) != 0) continue;
            out_models.armory_slots_loaded |= (1u << static_cast<unsigned>(w));
            const json::Value* g = doc.Find(kNames[w]);
            std::vector<SurvivalGunModel::SkinMaterial> skin_mats;
            const GpuMesh* body = g ? part(*g, "body", &skin_mats) : nullptr;
            if (!body) continue;
            SurvivalGunModel& out = guns[w];
            out.body = body;
            out.moving = part(*g, "moving", &skin_mats);
            out.hammer = part(*g, "hammer", &skin_mats);
            out.magazine = part(*g, "magazine", &skin_mats);
            out.skin_materials = std::move(skin_mats);
            read_carry(kNames[w], out);
            out.muzzle = vec3(*g, "muzzle", out.muzzle);
            out.ejection = vec3(*g, "ejection", out.ejection);
            out.cylinder_axis = vec3(*g, "cylinder_axis", out.cylinder_axis);
            out.cylinder_swing = vec3(*g, "cylinder_swing", out.cylinder_swing);
            out.hammer_pivot = vec3(*g, "hammer_pivot", out.hammer_pivot);
            out.sight_rear = vec3(*g, "sight_rear", out.sight_rear);
            out.sight_front = vec3(*g, "sight_front", out.sight_front);
            out.wrist = vec3(*g, "wrist", out.wrist);
            out.rake = static_cast<float>(g->Number("rake", out.rake));
            out.grip_turn = vec3(*g, "grip_turn", out.grip_turn);
            out.fingers = fingers(*g, "fingers", out.fingers);
            if (const json::Value* t = g->Find("trigger_index"); t && t->Size() == 4) {
                out.trigger_index = glm::vec4(static_cast<float>(t->At(0)->number), static_cast<float>(t->At(1)->number),
                                              static_cast<float>(t->At(2)->number), static_cast<float>(t->At(3)->number));
            }
            out.support = vec3(*g, "support", out.support);
            out.support_turn = vec3(*g, "support_turn", out.support_turn);
            out.support_fingers = fingers(*g, "support_fingers", out.support_fingers);
            out.slide_travel = static_cast<float>(g->Number("slide_travel", out.slide_travel));
            out.magazine_base = vec3(*g, "magazine_base", out.magazine_base);
            if (const glm::vec3 drop = vec3(*g, "magazine_drop", out.magazine_drop); glm::length(drop) > 1.0e-4f) out.magazine_drop = glm::normalize(drop);
            out.pump_grip = vec3(*g, "pump_grip", out.pump_grip);
            out.pump_travel = static_cast<float>(g->Number("pump_travel", out.pump_travel));
            out.loading_port = vec3(*g, "loading_port", out.loading_port);
            out.lever_pivot = vec3(*g, "lever_pivot", out.lever_pivot);
            out.hinge = vec3(*g, "hinge", out.hinge);
            out.barrel_side = static_cast<float>(g->Number("barrel_side", out.barrel_side));
            out.loaded = true;
            LogInfo("survival: the {} from survival/guns.json (moving part {}, hammer {}, magazine {})", kNames[w], out.moving ? "yes" : "no",
                    out.hammer ? "yes" : "no", out.magazine ? "yes" : "no");
        }
    }
    if (!include_gameplay) return;
    if (const json::Value* p = doc.Find("pickups")) {
        SurvivalModels::Drawn& d = out_models.drawn;
        d.ammo[1] = part(*p, "ammo_handgun");
        d.ammo[2] = part(*p, "ammo_shotgun");
        d.ammo[3] = part(*p, "ammo_revolver");
        d.ammo[4] = part(*p, "ammo_lever");
        d.ammo[5] = d.ammo[2];
        d.ammo[0] = d.ammo[1];
        d.medkit = part(*p, "medkit");
        d.pipe = part(*p, "pipe");
        d.pipe_halves = {part(*p, "pipe_a"), part(*p, "pipe_b")};
        d.flare = part(*p, "flare");
        d.casing = part(*p, "casing");
        d.shell = part(*p, "shell");
        LogInfo("survival: the pickups from survival/guns.json ({})", d.medkit ? "read" : "missing");
    }
    ReadReloads(dir, guns, kNames);
    if (const std::optional<std::vector<uint8_t>> pressed = mods::ReadDiskFile(dir / "hand_press.json")) {
        json::Value table;
        if (!json::Parse(std::string_view(reinterpret_cast<const char*>(pressed->data()), pressed->size()), table, &error)) {
            LogWarn("survival: survival/hand_press.json unreadable ({})", error);
            return;
        }
        for (size_t w = 1; w < guns.size(); ++w) {
            const json::Value* gun = table.Find(kNames[w]);
            if (!gun) continue;
            for (size_t side = 0; side < 2; ++side) {
                const json::Value* points = gun->Find(side == 0 ? "right" : "left");
                if (!points) continue;
                auto& out = guns[w].press[side];
                for (size_t i = 0; i < points->Size(); ++i) {
                    const json::Value* p = points->At(i);
                    if (!p || p->Size() != 6) continue;
                    const auto n = [p](size_t k) { return static_cast<float>(p->At(k)->number); };
                    out.push_back({glm::vec3(n(0), n(1), n(2)), glm::vec3(n(3), n(4), n(5))});
                }
                std::sort(out.begin(), out.end(), [](const SurvivalGunModel::PressPoint& a, const SurvivalGunModel::PressPoint& b) { return a.place.x < b.place.x; });
            }
        }
    }
}

}

glm::vec3 SurvivalGunModel::PressAt(size_t side, const glm::vec3& place) const {
    constexpr float kNear = 3.0e-5f;
    const std::vector<PressPoint>& points = press[side];
    auto at = std::lower_bound(points.begin(), points.end(), place.x - kNear, [](const PressPoint& p, float x) { return p.place.x < x; });
    for (; at != points.end() && at->place.x <= place.x + kNear; ++at) {
        if (std::abs(at->place.y - place.y) <= kNear && std::abs(at->place.z - place.z) <= kNear) return at->step;
    }
    return glm::vec3(0.0f);
}

void ApplyWeaponSkin(TextureManager& textures, const SurvivalGunModel& gun, int skin_id) {
    if (gun.skin_materials.empty()) return;
    if (skin_id <= 0) {
        for (const auto& sm : gun.skin_materials) {
            textures.UpdateMaterial(sm.index, sm.original);
            NoteSkinShown(textures, sm.index, nullptr, 0);
        }
        return;
    }
    SkinTextureSet tile{};
    bool tile_made = false;
    for (const auto& sm : gun.skin_materials) {
        MaterialGpu m = sm.original;
        m.flags &= ~(gpu::kMatConstantColor | gpu::kMatIncidence);
        m.albedo_factor = glm::vec4(1.0f);
        const BakedSkinMaterial baked = sm.source ? BakeSkinMaterial(textures, *sm.source, skin_id, false) : BakedSkinMaterial{};
        if (baked.albedo) {
            m.albedo = baked.albedo;
            if (baked.srm) m.specular = baked.srm;
        } else {
            if (!tile_made) {
                tile = EnsureSkinTextures(textures, skin_id);
                tile_made = true;
            }
            if (sm.channel == 1) {
                if (tile.grip_albedo) m.albedo = tile.grip_albedo;
                if (tile.grip_srm) m.specular = tile.grip_srm;
            } else if (sm.channel == 2) {
                if (tile.accent_albedo) m.albedo = tile.accent_albedo;
                if (tile.accent_srm) m.specular = tile.accent_srm;
            } else {
                if (tile.metal_albedo) m.albedo = tile.metal_albedo;
                if (tile.metal_srm) m.specular = tile.metal_srm;
            }
        }
        textures.UpdateMaterial(sm.index, m);
        NoteSkinShown(textures, sm.index, baked.albedo ? sm.source.get() : nullptr, skin_id);
    }
}

void ApplyWeaponSkinAnimated(TextureManager& textures, const SurvivalGunModel& gun, int skin_id, float time_sec) {
    if (gun.skin_materials.empty()) return;
    const SurvivalSkinDef& sk = SurvivalSkin(skin_id);
    if (sk.rarity != SkinRarity::Mythic) {
        ApplyWeaponSkin(textures, gun, skin_id);
        return;
    }
    const float fps = sk.id == 54 ? 14.0f : (sk.id == 56 ? 12.0f : 10.0f);
    const int f = static_cast<int>(std::floor(std::max(0.0f, time_sec) * fps)) % kMythicAnimFrames;
    const glm::vec3 live_col = MythicLiveColor(sk.id, time_sec);
    const float breath = 0.5f + 0.5f * std::sin(time_sec * (sk.id == 54 ? 13.0f : 3.4f));
    MythicAnimSet anim{};
    bool anim_made = false;
    for (const auto& sm : gun.skin_materials) {
        MaterialGpu m = sm.original;
        m.flags &= ~gpu::kMatConstantColor;
        m.flags |= gpu::kMatIncidence;
        m.albedo_factor = glm::vec4(live_col, sm.channel == 1 ? (0.32f + 0.25f * breath) : (0.62f + 0.35f * breath));
        m.params.z = 2.1f;
        const BakedSkinMaterial baked = sm.source ? BakeSkinMaterial(textures, *sm.source, sk.id, true) : BakedSkinMaterial{};
        if (baked.albedo) {
            m.albedo = sm.channel == 0 && static_cast<size_t>(f) < baked.frames.size() ? baked.frames[static_cast<size_t>(f)] : baked.albedo;
            if (baked.srm) m.specular = baked.srm;
        } else {
            if (!anim_made) {
                anim = EnsureMythicAnimTextures(textures, sk.id);
                anim_made = true;
            }
            if (sm.channel == 1) {
                if (anim.grip_albedo) m.albedo = anim.grip_albedo;
                if (anim.grip_srm) m.specular = anim.grip_srm;
            } else if (sm.channel == 2) {
                if (anim.accent_albedo[static_cast<size_t>(f)]) m.albedo = anim.accent_albedo[static_cast<size_t>(f)];
                if (anim.accent_srm) m.specular = anim.accent_srm;
            } else {
                if (anim.metal_albedo[static_cast<size_t>(f)]) m.albedo = anim.metal_albedo[static_cast<size_t>(f)];
                if (anim.metal_srm) m.specular = anim.metal_srm;
            }
        }
        textures.UpdateMaterial(sm.index, m);
        NoteSkinShown(textures, sm.index, baked.albedo ? sm.source.get() : nullptr, sk.id);
    }
}

void ApplyTorchSkin(TextureManager& textures, const SurvivalModels& models, int torch_id, float lens_gain) {
    const int id = std::clamp(torch_id, 0, kSurvivalTorchCount - 1);
    const SurvivalTorchDef& td = SurvivalTorch(id);
    for (const auto& sm : models.chest_materials) {
        if (id == 0) {
            textures.UpdateMaterial(sm.index, sm.original);
        } else {
            const TorchTextureSet ts = EnsureTorchTextures(textures, id);
            MaterialGpu m = sm.original;
            m.flags &= ~gpu::kMatConstantColor;
            m.albedo_factor = glm::vec4(1.0f);
            if (ts.body_albedo) m.albedo = ts.body_albedo;
            if (ts.body_srm) m.specular = ts.body_srm;
            textures.UpdateMaterial(sm.index, m);
        }
    }
    if (models.lens_material != 0) {
        MaterialGpu lm = models.lens_original;
        const float max_c = std::max({td.tint.r, td.tint.g, td.tint.b, 0.001f});
        lm.albedo_factor = glm::vec4(id == 0 ? glm::vec3(1.0f, 0.96f, 0.88f) : (td.tint / max_c), 1.0f);
        lm.params.y = lens_gain;
        textures.UpdateMaterial(models.lens_material, lm);
    }
}

void EmitMythicWeaponVfx(TextureManager& textures, const SurvivalModels& models, const SurvivalGunModel& gun,
                         const glm::mat4& gun_world, int skin_id, float time_sec, uint8_t hidden_views,
                         bool view_attached, bool turns_with_view, float emission_scale, std::vector<DrawItem>& out) {
    const SurvivalSkinDef& sk = SurvivalSkin(skin_id);
    if (sk.rarity != SkinRarity::Mythic || !gun.body || !models.mythic_mote || models.mythic_vfx_material == 0) return;

    const glm::vec3 live_col = MythicLiveColor(sk.id, time_sec);
    const float pulse = 0.78f + 0.22f * std::sin(time_sec * (sk.id == 54 ? 19.0f : 4.2f));
    if (const MaterialGpu* cur = textures.Material(models.mythic_vfx_material)) {
        MaterialGpu m = *cur;
        m.albedo_factor = glm::vec4(live_col, 0.92f);
        m.params.y = emission_scale * pulse;
        m.params.w = 2.2f;
        textures.UpdateMaterial(models.mythic_vfx_material, m);
    }

    const glm::vec3 bmin = gun.body->bounds_min;
    const glm::vec3 bmax = gun.body->bounds_max;
    const glm::vec3 mid = 0.5f * (bmin + bmax);
    const glm::vec3 ext = glm::max(0.5f * (bmax - bmin), glm::vec3(0.025f, 0.03f, 0.08f));
    const bool is_static = sk.id == 54;
    const bool is_rising = (sk.id == 55 || sk.id == 58 || sk.id == 60);
    const GpuMesh* mesh = is_static && models.mythic_bar ? models.mythic_bar.get() : models.mythic_mote.get();

    constexpr int kCount = 16;
    for (int i = 0; i < kCount; ++i) {
        const float fi = static_cast<float>(i);
        const float h1 = std::fmod(std::sin(fi * 12.9898f + static_cast<float>(sk.id) * 7.13f) * 43758.5453f, 1.0f);
        const float s1 = h1 < 0.0f ? h1 + 1.0f : h1;
        const float h2 = std::fmod(std::sin(fi * 78.233f + 1.91f) * 24634.6345f, 1.0f);
        const float s2 = h2 < 0.0f ? h2 + 1.0f : h2;

        glm::vec3 local_pos = mid;
        glm::vec3 scale(1.0f);
        float alpha = 1.0f;
        glm::mat4 rot(1.0f);

        if (is_static) {
            const float step_t = std::floor(time_sec * (7.0f + 5.0f * s1) + fi * 1.37f);
            const float rx = std::sin(step_t * 3.17f + fi * 2.1f);
            const float ry = std::cos(step_t * 4.71f + fi * 1.7f);
            const float rz = std::sin(step_t * 2.39f + fi * 3.3f);
            local_pos = mid + glm::vec3(rx * (ext.x + 0.032f), ry * (ext.y + 0.028f), rz * (ext.z * 0.92f));
            const float blink = std::sin(time_sec * (16.0f + 9.0f * s2) + fi * 2.3f);
            if (blink < -0.15f) continue;
            alpha = std::clamp(0.45f + 0.55f * blink, 0.0f, 1.0f);
            scale = glm::vec3(0.6f + 1.1f * s2, 0.5f + 0.6f * s1, 0.8f + 1.8f * s1);
        } else if (is_rising) {
            const float life = std::fmod(time_sec * (0.55f + 0.35f * s1) + s2, 1.0f);
            const float fade = std::sin(life * kPi);
            const float z_off = (s1 * 2.0f - 1.0f) * ext.z * 0.88f;
            const float x_off = (s2 * 2.0f - 1.0f) * (ext.x + 0.014f) + 0.012f * std::sin(time_sec * 3.5f + fi);
            const float y_off = -ext.y * 0.2f + life * (ext.y * 1.55f + 0.055f);
            local_pos = mid + glm::vec3(x_off, y_off, z_off);
            rot = glm::rotate(glm::mat4(1.0f), time_sec * 3.0f + fi, glm::vec3(0.3f, 1.0f, 0.2f));
            scale = glm::vec3((0.55f + 0.75f * fade) * (0.7f + 0.6f * s1));
            alpha = fade;
        } else {
            const float speed = (sk.id == 56 ? 2.6f : 1.7f) + 0.6f * s1;
            const float ang = time_sec * speed + fi * (2.0f * kPi / static_cast<float>(kCount));
            const float z_wave = std::sin(time_sec * 0.85f + fi * 0.9f) * ext.z * 0.85f;
            const float rx = (ext.x + 0.022f + 0.014f * s2) * std::cos(ang);
            const float ry = (ext.y + 0.020f + 0.014f * s1) * std::sin(ang);
            local_pos = mid + glm::vec3(rx, ry, z_wave);
            rot = glm::rotate(glm::mat4(1.0f), ang * 1.5f, glm::vec3(0.2f, 0.6f, 1.0f));
            const float twinkle = 0.50f + 0.50f * std::sin(time_sec * 5.0f + fi * 1.9f);
            scale = glm::vec3(0.65f + 0.65f * twinkle);
            alpha = 0.55f + 0.45f * twinkle;
        }

        DrawItem item;
        item.mesh = mesh;
        item.transform = gun_world * glm::translate(glm::mat4(1.0f), local_pos) * rot * glm::scale(glm::mat4(1.0f), scale);
        item.tint = glm::vec4(1.0f, 1.0f, 1.0f, alpha);
        item.hidden_views = hidden_views;
        item.handy_shadowless = true;
        item.view_attached = view_attached;
        item.turns_with_view = turns_with_view;
        item.source = 0x4D59544800000000ull + static_cast<uint64_t>(i);
        out.push_back(item);
    }
}

bool SurvivalModels::BuildArmorySlot(ModelCache& models, int slot) {
    if (!lens_glow || !mythic_mote) {
        TextureManager& textures = models.Textures();
        if (!lens_glow) {
            MaterialGpu lm;
            lm.kind = gpu::kKindConstant;
            lm.albedo = TextureManager::kWhite;
            lm.albedo_factor = glm::vec4(1.0f, 0.96f, 0.88f, 1.0f);
            lm.params = glm::vec4(0.0f, 60.0f, 0.0f, 0.0f);
            lens_original = lm;
            lens_material = textures.AddMaterial(lm);
            Kit k;
            k.Disc(k.Part(lens_material, true), glm::mat4(1.0f), 0.0158f, 24);
            lens_glow = k.Upload(models, "survival_lens_glow");
        }
        if (!mythic_mote) {
            MaterialGpu vm;
            vm.kind = gpu::kKindConstant;
            vm.albedo = TextureManager::kWhite;
            vm.albedo_factor = glm::vec4(1.0f, 0.25f, 0.20f, 0.92f);
            vm.params = glm::vec4(0.0f, 8.0f, 0.0f, 2.2f);
            mythic_vfx_material = textures.AddMaterial(vm);
            {
                Kit k;
                const size_t p = k.Part(mythic_vfx_material, true);
                k.Box(p, glm::rotate(glm::mat4(1.0f), 0.78f, glm::vec3(1, 1, 0)), {0.0022f, 0.0022f, 0.0045f});
                mythic_mote = k.Upload(models, "survival_mythic_mote");
            }
            {
                Kit k;
                const size_t p = k.Part(mythic_vfx_material, true);
                k.Box(p, glm::mat4(1.0f), {0.0014f, 0.0011f, 0.012f});
                mythic_bar = k.Upload(models, "survival_mythic_bar");
            }
        }
    }
    const int s = std::clamp(slot, 1, 6);
    if ((armory_slots_loaded & (1u << static_cast<unsigned>(s))) != 0) return true;
    LoadGunFiles(models, *this, /*include_guns=*/true, /*include_gameplay=*/false, s);
    if (s >= 1 && s <= 3 && !guns[static_cast<size_t>(s)].body) {
        const Materials mt = MakeMaterials(models.Textures());
        if (s == 1) BuildHandgun(models, mt, guns[1], owned);
        else if (s == 2) BuildShotgun(models, mt, guns[2], owned);
        else if (s == 3) BuildRevolver(models, mt, guns[3], owned);
    } else if ((s == 4 || s == 5) && !guns[static_cast<size_t>(s)].body) {
        BuildArmorySlot(models, 2);
        if (guns[2].body) {
            guns[static_cast<size_t>(s)] = guns[2];
            LogWarn("survival: gun {} has no model in survival/guns.json, drawn as the shotgun", s);
        }
    }
    return true;
}

bool SurvivalModels::BuildArmory(ModelCache& models) {
    if (armory_built) return true;
    armory_built = true;
    for (int s = 1; s <= 6; ++s) BuildArmorySlot(models, s);
    return true;
}

bool SurvivalModels::Build(ModelCache& models) {
    if (full_built) return true;
    full_built = true;
    if (!armory_built) BuildArmory(models);
    const Materials mt = MakeMaterials(models.Textures());
    LoadGunFiles(models, *this, /*include_guns=*/false, /*include_gameplay=*/true);
    {
        Kit k;
        const size_t can = k.Part(mt.olive), dark = k.Part(mt.dark), steel = k.Part(mt.steel), brass = k.Part(mt.brass);
        k.Box(can, T({0.0f, 0.075f, 0.0f}), {0.135f, 0.075f, 0.065f}, 0.008f);
        k.Box(can, T({0.0f, 0.153f, 0.0f}), {0.139f, 0.006f, 0.069f}, 0.003f);
        k.Box(dark, T({0.0f, 0.146f, 0.0f}), {0.1365f, 0.0012f, 0.0665f});
        for (const float s : {1.0f, -1.0f}) k.Box(steel, T({s * 0.055f, 0.170f, 0.0f}), {0.004f, 0.012f, 0.004f}, 0.001f);
        k.Box(steel, T({0.0f, 0.183f, 0.0f}), {0.059f, 0.004f, 0.006f}, 0.0015f);
        k.Box(steel, T({0.139f, 0.130f, 0.0f}), {0.004f, 0.022f, 0.016f}, 0.0012f);
        for (int i = 0; i < 3; ++i) k.Cylinder(brass, TR({-0.035f + 0.022f * i, 0.163f, -0.035f}, -kPi * 0.5f, glm::vec3(0, 1, 0)) * T({0, 0, 0}), 0.0048f, 0.03f, 10);
        ammo_box = k.Upload(models, "survival_ammo_can");
    }
    {
        Kit k;
        const size_t body = k.Part(mt.white_case), dark = k.Part(mt.dark), cross = k.Part(mt.cross, true);
        k.Box(body, T({0.0f, 0.045f, 0.0f}), {0.125f, 0.045f, 0.08f}, 0.01f);
        k.Box(dark, T({0.0f, 0.045f, 0.0f}), {0.1265f, 0.0015f, 0.0815f});
        k.Box(dark, T({0.0f, 0.096f, 0.0f}), {0.045f, 0.006f, 0.008f}, 0.002f);
        k.Box(cross, T({0.0f, 0.0905f, -0.028f}), {0.03f, 0.0008f, 0.009f});
        k.Box(cross, T({0.0f, 0.0905f, -0.028f}), {0.009f, 0.0008f, 0.03f});
        medkit = k.Upload(models, "survival_medkit");
    }
    {
        Kit k;
        const size_t wall = k.Part(mt.facade), ground = k.Part(mt.backdrop_ground);
        AddBackdrop(k, wall, ground, &backdrop_blocks);
        backdrop = k.Upload(models, "survival_backdrop");
    }
    {
        Kit k;
        const size_t front = k.Part(mt.flash_front, true), plume = k.Part(mt.flash_side, true);
        const float r = 0.075f;
        for (const float n : {1.0f, -1.0f}) {
            k.Quad(front, {-r, -r, 0.002f}, {r, -r, 0.002f}, {r, r, 0.002f}, {-r, r, 0.002f}, {0.0f, 0.0f, n});
        }
        const float wide = 0.045f, along = 0.17f;
        for (int i = 0; i < 2; ++i) {
            const glm::vec3 across = i == 0 ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 normal = i == 0 ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            for (const float n : {1.0f, -1.0f}) {
                k.Quad(plume, -across * wide, glm::vec3(0.0f, 0.0f, along) - across * wide, glm::vec3(0.0f, 0.0f, along) + across * wide,
                       across * wide, normal * n, {0.0f, 0.0f}, {1.0f, 1.0f});
            }
        }
        flash = k.Upload(models, "survival_muzzle_flash");
    }
    {
        Kit k;
        const size_t g = k.Part(mt.tracer, true);
        k.Box(g, T({0.0f, 0.0f, 0.5f}), {0.0018f, 0.0018f, 0.5f});
        tracer = k.Upload(models, "survival_tracer");
    }
    {
        Kit k;
        const size_t brass = k.Part(mt.brass), dark = k.Part(mt.dark);
        k.Cylinder(brass, Along(0.0f, 0.0f, -0.0095f), 0.0047f, 0.019f, 10);
        k.Disc(dark, glm::rotate(T({0.0f, 0.0f, -0.0096f}), kPi, glm::vec3(0, 1, 0)), 0.002f, 8);
        casing = k.Upload(models, "survival_casing");
    }
    {
        Kit k;
        const size_t hull = k.Part(mt.hull), brass = k.Part(mt.brass);
        k.Cylinder(hull, Along(0.0f, 0.0f, -0.025f), 0.0105f, 0.055f, 14);
        k.Cylinder(brass, Along(0.0f, 0.0f, -0.035f), 0.0108f, 0.012f, 14);
        shell = k.Upload(models, "survival_shell");
    }
    {
        Kit k;
        const size_t rust = k.Part(mt.rust), tape = k.Part(mt.tape);
        k.Cylinder(rust, Along(0.0f, 0.0f, -0.08f), 0.0165f, 0.70f, 14, true, 6.0f);
        k.Cylinder(tape, Along(0.0f, 0.0f, -0.07f), 0.0185f, 0.19f, 14, true, 6.0f);
        k.Cylinder(rust, Along(0.0f, 0.0f, 0.565f), 0.0215f, 0.035f, 14, true, 2.0f);
        k.Cylinder(rust, TR({0.0f, -0.05f, 0.615f}, -kPi * 0.5f, glm::vec3(1, 0, 0)), 0.02f, 0.1f, 14, true, 2.0f);
        pipe = k.Upload(models, "survival_pipe");
        Kit a;
        const size_t ra = a.Part(mt.rust), ta = a.Part(mt.tape);
        a.Cylinder(ra, Along(0.0f, 0.0f, -0.08f), 0.0165f, 0.38f, 14, true, 6.0f);
        a.Cylinder(ta, Along(0.0f, 0.0f, -0.07f), 0.0185f, 0.19f, 14, true, 6.0f);
        pipe_halves[0] = a.Upload(models, "survival_pipe_a");
        Kit b;
        const size_t rb = b.Part(mt.rust);
        b.Cylinder(rb, Along(0.0f, 0.0f, 0.30f), 0.0165f, 0.32f, 14, true, 6.0f);
        b.Cylinder(rb, Along(0.0f, 0.0f, 0.565f), 0.0215f, 0.035f, 14, true, 2.0f);
        b.Cylinder(rb, TR({0.0f, -0.05f, 0.615f}, -kPi * 0.5f, glm::vec3(1, 0, 0)), 0.02f, 0.1f, 14, true, 2.0f);
        pipe_halves[1] = b.Upload(models, "survival_pipe_b");
    }
    gun_metals = {mt.steel, mt.nitride, mt.stainless};
    lantern_glow = mt.red_glass;
    {
        Kit k;
        const size_t paint = k.Part(mt.box_paint), dark = k.Part(mt.dark), steel = k.Part(mt.steel), label = k.Part(mt.label);
        k.Box(paint, T({0.0f, 0.0f, 0.075f}), {0.19f, 0.25f, 0.075f}, 0.008f);
        k.Box(paint, T({0.0f, 0.0f, 0.153f}), {0.175f, 0.235f, 0.004f}, 0.002f);
        k.Box(dark, T({0.0f, 0.0f, 0.1495f}), {0.181f, 0.241f, 0.0015f});
        k.Box(steel, T({0.13f, -0.02f, 0.162f}), {0.01f, 0.035f, 0.006f}, 0.002f);
        k.Box(label, T({-0.05f, 0.13f, 0.1575f}), {0.07f, 0.035f, 0.0008f});
        for (const float y : {0.17f, -0.17f}) k.Cylinder(steel, TR({-0.178f, y, 0.15f}, -kPi * 0.5f, glm::vec3(1, 0, 0)) * T({0.0f, 0.0f, -0.025f}), 0.006f, 0.05f, 8);
        k.Cylinder(steel, TR({0.0f, 0.25f, 0.05f}, -kPi * 0.5f, glm::vec3(1, 0, 0)), 0.016f, 1.4f, 12, true, 4.0f);
        k.Box(paint, T({0.205f, 0.0f, 0.09f}), {0.015f, 0.06f, 0.03f}, 0.003f);
        fuse_box = k.Upload(models, "survival_fuse_box");
        Kit l;
        const size_t arm = l.Part(mt.dark), grip = l.Part(mt.hull);
        l.Box(arm, T({0.012f, -0.08f, 0.0f}), {0.007f, 0.08f, 0.009f}, 0.002f);
        l.Cylinder(grip, TR({0.0f, -0.16f, 0.0f}, kPi * 0.5f, glm::vec3(0, 1, 0)), 0.012f, 0.05f, 10);
        fuse_lever = l.Upload(models, "survival_fuse_lever");
        for (int on = 0; on < 2; ++on) {
            Kit b;
            const size_t lamp = b.Part(on ? mt.switch_on : mt.cross, true);
            b.Box(lamp, T({0.12f, 0.19f, 0.159f}), {0.011f, 0.011f, 0.004f}, 0.002f);
            (on ? fuse_lamp_on : fuse_lamp_out) = b.Upload(models, on ? "survival_fuse_lamp_on" : "survival_fuse_lamp_out");
        }
    }
    {
        Kit k;
        const size_t can = k.Part(mt.olive), dark = k.Part(mt.dark), steel = k.Part(mt.steel), rubber = k.Part(mt.rubber), lamp = k.Part(mt.orange_dot, true);
        k.Box(can, T({0.0f, 0.11f, 0.0f}), {0.16f, 0.11f, 0.065f}, 0.01f);
        k.Box(dark, T({0.0f, 0.12f, 0.066f}), {0.14f, 0.08f, 0.003f});
        for (const float x : {-0.095f, -0.04f}) k.Cylinder(steel, T({x, 0.15f, 0.066f}), 0.017f, 0.016f, 14);
        k.Box(rubber, T({0.07f, 0.11f, 0.0695f}), {0.05f, 0.05f, 0.0015f});
        k.Box(lamp, T({-0.11f, 0.08f, 0.0695f}), {0.008f, 0.008f, 0.002f});
        for (const float x : {-0.09f, 0.09f}) k.Box(steel, T({x, 0.228f, 0.0f}), {0.005f, 0.01f, 0.006f});
        k.Box(steel, T({0.0f, 0.24f, 0.0f}), {0.095f, 0.005f, 0.007f}, 0.002f);
        k.Box(rubber, T({-0.19f, 0.05f, 0.0f}), {0.025f, 0.04f, 0.085f}, 0.01f);
        k.Cylinder(steel, TR({0.12f, 0.22f, -0.04f}, -kPi * 0.5f - 0.18f, glm::vec3(1, 0, 0)), 0.003f, 0.85f, 6, true, 2.0f);
        radio = k.Upload(models, "survival_radio");
    }
    {
        Kit k;
        const size_t border = k.Part(mt.white_case), print = k.Part(mt.photo);
        k.Box(border, T({0.0f, 0.0012f, 0.0f}), {0.062f, 0.0011f, 0.078f});
        k.Box(print, T({0.0f, 0.0025f, 0.004f}), {0.052f, 0.0003f, 0.064f});
        photo_piece = k.Upload(models, "survival_photo_piece");
    }
    {
        for (int broken = 0; broken < 2; ++broken) {
            Kit k;
            const size_t iron = k.Part(mt.iron), glass = k.Part(broken ? mt.dead_glass : mt.red_glass, !broken), flame = k.Part(mt.flame, true);
            k.Box(iron, T({0.0f, 0.015f, 0.0f}), {0.09f, 0.015f, 0.09f}, 0.004f);
            for (const float x : {0.075f, -0.075f}) {
                for (const float z : {0.075f, -0.075f}) k.Box(iron, T({x, 0.17f, z}), {0.008f, 0.14f, 0.008f}, 0.002f);
            }
            k.Box(iron, T({0.0f, 0.322f, 0.0f}), {0.095f, 0.012f, 0.095f}, 0.004f);
            k.Box(iron, T({0.0f, 0.352f, 0.0f}), {0.055f, 0.018f, 0.055f}, 0.006f);
            k.Cylinder(iron, TR({-0.035f, 0.395f, 0.0f}, kPi * 0.5f, glm::vec3(0, 1, 0)), 0.005f, 0.07f, 8);
            for (const float x : {0.035f, -0.035f}) k.Box(iron, T({x, 0.382f, 0.0f}), {0.004f, 0.013f, 0.004f});
            const float h = broken ? 0.035f : 0.13f, y = broken ? 0.065f : 0.17f;
            for (const float s : {1.0f, -1.0f}) {
                k.Box(glass, T({0.0f, y, 0.0745f * s}), {0.067f, h, 0.002f});
                k.Box(glass, T({0.0745f * s, y, 0.0f}), {0.002f, h, 0.067f});
            }
            if (!broken) k.Box(flame, T({0.0f, 0.1f, 0.0f}), {0.01f, 0.03f, 0.01f}, 0.004f);
            (broken ? lantern_broken : lantern) = k.Upload(models, broken ? "survival_lantern_broken" : "survival_lantern");
        }
    }
    {
        Kit f;
        const size_t wood = f.Part(mt.wood);
        for (const float x : {0.45f, -0.45f}) f.Box(wood, T({x, 1.05f, 0.0f}), {0.05f, 1.05f, 0.07f}, 0.006f, 1.0f);
        f.Box(wood, T({0.0f, 2.05f, 0.0f}), {0.5f, 0.05f, 0.07f}, 0.006f, 1.0f);
        f.Box(wood, T({0.0f, 0.012f, 0.0f}), {0.4f, 0.012f, 0.07f}, 0.003f, 1.0f);
        door_frame = f.Upload(models, "survival_door_frame");
        Kit d;
        const size_t leaf = d.Part(mt.wood), panel = d.Part(mt.wood_panel), brass = d.Part(mt.brass);
        d.Box(leaf, T({0.4f, 0.995f, 0.0f}), {0.395f, 0.99f, 0.022f}, 0.004f);
        for (const float s : {1.0f, -1.0f}) {
            d.Box(panel, T({0.4f, 1.41f, 0.024f * s}), {0.27f, 0.39f, 0.004f}, 0.004f);
            d.Box(panel, T({0.4f, 0.49f, 0.024f * s}), {0.27f, 0.35f, 0.004f}, 0.004f);
            d.Cylinder(brass, s > 0.0f ? T({0.73f, 0.97f, 0.022f}) : TR({0.73f, 0.97f, -0.022f}, kPi, glm::vec3(0, 1, 0)), 0.006f, 0.04f, 10);
            d.Cylinder(brass, s > 0.0f ? T({0.73f, 0.97f, 0.062f}) : TR({0.73f, 0.97f, -0.062f}, kPi, glm::vec3(0, 1, 0)), 0.026f, 0.022f, 16);
        }
        door_leaf = d.Upload(models, "survival_door_leaf");
        Kit v;
        const size_t beyond = v.Part(mt.beyond, true);
        v.Quad(beyond, {-0.4f, 0.024f, -0.03f}, {0.4f, 0.024f, -0.03f}, {0.4f, 2.0f, -0.03f}, {-0.4f, 2.0f, -0.03f}, {0.0f, 0.0f, 1.0f});
        door_dark = v.Upload(models, "survival_door_dark");
    }
    {
        Kit k;
        const size_t paper = k.Part(mt.flare_paper), cap = k.Part(mt.dark);
        k.Cylinder(paper, Along(0.0f, 0.0f, -0.11f), 0.0145f, 0.2f, 14);
        k.Cylinder(cap, Along(0.0f, 0.0f, -0.115f), 0.015f, 0.012f, 14);
        flare = k.Upload(models, "survival_flare");
        Kit f;
        const size_t fire = f.Part(mt.flare_fire, true);
        f.Cylinder(fire, Along(0.0f, 0.0f, 0.088f), 0.0125f, 0.018f, 12);
        flare_tip = f.Upload(models, "survival_flare_tip");
    }
    {
        for (int variant = 0; variant < 3; ++variant) {
            Kit k;
            const size_t b = k.Part(mt.pool[static_cast<size_t>(variant)]);
            k.Quad(b, {-1.0f, 0.002f, -1.0f}, {-1.0f, 0.002f, 1.0f}, {1.0f, 0.002f, 1.0f}, {1.0f, 0.002f, -1.0f}, {0.0f, 1.0f, 0.0f});
            blood[static_cast<size_t>(variant)] = k.Upload(models, "survival_blood");
        }
    }
    {
        Kit k;
        const size_t b = k.Part(mt.flying_blood, true);
        constexpr float kRadius = 0.0016f, kTail = 2.6f;
        std::vector<glm::vec2> profile;
        for (int i = 0; i <= 4; ++i) {
            const float a = 0.5f * kPi * static_cast<float>(i) / 4.0f;
            profile.push_back({kRadius * std::sin(a), kRadius * std::cos(a)});
        }
        for (int i = 1; i <= 5; ++i) {
            const float t = static_cast<float>(i) / 5.0f;
            profile.push_back({kRadius * std::pow(std::cos(0.5f * kPi * t), 1.3f), -kRadius * kTail * t});
        }
        k.Lathe(b, glm::mat4(1.0f), profile, 10);
        droplet = k.Upload(models, "survival_droplet");
    }
    mist_texture = mt.mist_texture;
    flare_glow_texture = mt.flare_glow_texture;
    contact_texture = mt.contact_texture;
    car_contact_texture = mt.car_contact_texture;
    {
        uint32_t seed = 0x3D11u;
        auto rnd = [&] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        Kit k;
        const size_t b = k.Part(mt.fresh_blood), hole_part = k.Part(mt.wound);
        auto dome = [&](size_t part, const glm::mat4& at, float radius, float height) {
            std::vector<glm::vec2> profile;
            for (int i = 0; i <= 5; ++i) {
                const float a = 0.5f * kPi * static_cast<float>(i) / 5.0f;
                profile.push_back({radius * std::sin(a), height * std::cos(a) - 0.004f});
            }
            k.Lathe(part, at, profile, 12);
        };
        dome(b, glm::mat4(1.0f), 0.027f, 0.011f);
        for (int i = 0; i < 5; ++i) {
            const float a = 2.0f * kPi * (static_cast<float>(i) + 0.6f * rnd()) / 5.0f, d = 0.02f + 0.012f * rnd();
            dome(b, T({std::cos(a) * d, std::sin(a) * d, 0.0f}), 0.009f + 0.008f * rnd(), 0.008f);
        }
        dome(hole_part, glm::mat4(1.0f), 0.0105f, 0.0135f);
        dome(b, T({0.003f, -0.042f, 0.0f}) * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 3.4f, 1.0f)), 0.0075f, 0.0075f);
        wound = k.Upload(models, "survival_wound");
    }
    {
        Kit k;
        const size_t dark = k.Part(mt.bore), rim = k.Part(mt.dark);
        k.Disc(rim, glm::mat4(1.0f), 0.022f, 12);
        k.Disc(dark, T({0.0f, 0.0f, 0.0004f}), 0.009f, 10);
        hole = k.Upload(models, "survival_hole");
    }
    {
        Kit k;
        const size_t g = k.Part(mt.spark, true);
        k.Box(g, glm::mat4(1.0f), {0.0025f, 0.0025f, 0.03f});
        spark = k.Upload(models, "survival_spark");
    }
    {
        Kit k;
        const size_t body = k.Part(mt.polymer), brass = k.Part(mt.brass), copper = k.Part(mt.copper);
        k.Cylinder(body, Along(0.0f, 0.0f, -0.032f), 0.019f, 0.012f, 20);
        k.Cylinder(body, Along(0.0f, 0.0f, -0.062f), 0.007f, 0.03f, 12);
        for (int i = 0; i < 6; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / 6.0f;
            const float x = std::cos(a) * 0.0122f, y = std::sin(a) * 0.0122f;
            k.Cylinder(brass, Along(x, y, -0.020f), 0.0042f, 0.028f, 10);
            k.Cylinder(copper, Along(x, y, 0.008f), 0.0036f, 0.007f, 10);
        }
        loader = k.Upload(models, "survival_speed_loader");
    }
    {
        Kit k;
        const size_t body = k.Part(mt.polymer);
        k.Cylinder(body, Along(0.0f, 0.0f, -0.032f), 0.019f, 0.012f, 20);
        k.Cylinder(body, Along(0.0f, 0.0f, -0.062f), 0.007f, 0.03f, 12);
        loader_empty = k.Upload(models, "survival_speed_loader_empty");
    }
    {
        Kit k;
        const size_t brass = k.Part(mt.brass), dark = k.Part(mt.dark);
        for (int i = 0; i < 6; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / 6.0f;
            const float x = std::cos(a) * 0.0122f, y = std::sin(a) * 0.0122f;
            k.Cylinder(brass, Along(x, y, -0.0112f), 0.0046f, 0.02f, 10);
            k.Disc(dark, glm::rotate(T({x, y, -0.0113f}), kPi, glm::vec3(0, 1, 0)), 0.0016f, 8);
        }
        rounds = k.Upload(models, "survival_cylinder_rounds");
    }
    auto or_built = [](const GpuMesh*& mesh, const std::unique_ptr<GpuMesh>& built) {
        if (!mesh) mesh = built.get();
    };
    for (const GpuMesh*& box : drawn.ammo) or_built(box, ammo_box);
    or_built(drawn.medkit, medkit);
    or_built(drawn.pipe, pipe);
    or_built(drawn.pipe_halves[0], pipe_halves[0]);
    or_built(drawn.pipe_halves[1], pipe_halves[1]);
    or_built(drawn.flare, flare);
    or_built(drawn.casing, casing);
    or_built(drawn.shell, shell);
    LogInfo("survival: models built (guns with their moving parts, pickups, flash, cases, blood)");
    return true;
}

namespace {

constexpr const char* kTenementModel = "/Assets/sh/environ/object/shsb/map/shsb_mapp001/scenes/shsb_mapp001_buld001.fmdl";
constexpr glm::vec3 kTenementMin{11.0f, -2.0f, -2.6f}, kTenementMax{19.6f, 40.0f, 33.9f};
constexpr float kTenementDoorwayZ = 18.49f, kTenementDoorwayBack = 14.49f;
constexpr glm::vec3 kPaintedDoorMin{14.44f, 0.17f, 17.91f}, kPaintedDoorMax{14.6f, 2.49f, 19.07f};
constexpr float kStoreyFrom = 10.0f, kStoreyHeight = 3.93f;

enum class TowerPart { Base, Storey, Top };

Vertex Turned(Vertex v) {
    auto turn = [](const glm::vec3& d) { return glm::vec3(d.z, d.y, -d.x); };
    v.position = glm::vec3(v.position.z - kTenementDoorwayZ + kHouseDoorX, v.position.y, kHouseFront + kTenementDoorwayBack - v.position.x);
    v.normal = turn(v.normal);
    v.tangent = glm::vec4(turn(glm::vec3(v.tangent)), v.tangent.w);
    return v;
}

Vertex Mix(const Vertex& a, const Vertex& b, float t) {
    Vertex v = a;
    v.position = glm::mix(a.position, b.position, t);
    const glm::vec3 n = glm::mix(a.normal, b.normal, t), tangent = glm::mix(glm::vec3(a.tangent), glm::vec3(b.tangent), t);
    v.normal = glm::dot(n, n) > 1e-12f ? glm::normalize(n) : a.normal;
    v.tangent = glm::vec4(glm::dot(tangent, tangent) > 1e-12f ? glm::normalize(tangent) : glm::vec3(a.tangent), a.tangent.w);
    v.uv0 = glm::mix(a.uv0, b.uv0, t);
    v.uv1 = glm::mix(a.uv1, b.uv1, t);
    v.uv2 = glm::mix(a.uv2, b.uv2, t);
    v.color = glm::mix(a.color, b.color, t);
    return v;
}

using Polygon = std::vector<Vertex>;

Polygon Cut(const Polygon& poly, int axis, float value, bool below) {
    Polygon out;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vertex& a = poly[i];
        const Vertex& b = poly[(i + 1) % poly.size()];
        const float da = a.position[axis] - value, db = b.position[axis] - value;
        const bool in_a = below ? da <= 0.0f : da >= 0.0f, in_b = below ? db <= 0.0f : db >= 0.0f;
        if (in_a) out.push_back(a);
        if (in_a != in_b) out.push_back(Mix(a, b, da / (da - db)));
    }
    return out.size() >= 3 ? out : Polygon{};
}

const GpuMesh* BuildTowerPart(ModelCache& models, TowerPart part) {
    const float lo = part == TowerPart::Base ? -1e9f : kStoreyFrom;
    const float hi = part == TowerPart::Top ? 1e9f : part == TowerPart::Base ? kStoreyFrom : kStoreyFrom + kStoreyHeight;
    static constexpr const char* kNames[] = {"base", "storey", "top"};
    const char* name = kNames[static_cast<int>(part)];
    const std::string key = std::string(kTenementModel) + "#street_tower_" + name;
    const ModelEntry* entry = models.GetRebuilt(kTenementModel, key, [&](MeshData& mesh) {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> shared(mesh.vertices.size(), UINT32_MAX);
        auto emit = [&](const Polygon& poly) {
            const uint32_t first = static_cast<uint32_t>(vertices.size());
            for (const Vertex& v : poly) vertices.push_back(Turned(v));
            for (uint32_t k = 1; k + 1 < poly.size(); ++k) indices.insert(indices.end(), {first, first + k, first + k + 1});
        };
        struct Front {
            glm::vec2 x, y;
            float z;
        };
        std::vector<Front> fronts;
        size_t sides = 0;
        for (const SubMesh& sub : mesh.submeshes) {
            for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                Vertex v[3];
                glm::vec3 middle(0.0f);
                for (uint32_t k = 0; k < 3; ++k) {
                    v[k] = mesh.vertices[static_cast<size_t>(static_cast<int64_t>(mesh.indices[sub.first_index + i + k]) + sub.vertex_offset)];
                    middle += v[k].position / 3.0f;
                    v[k] = Turned(v[k]);
                }
                if (v[0].normal.z < 0.98f || glm::any(glm::lessThan(middle, kTenementMin)) || glm::any(glm::greaterThan(middle, kTenementMax))) continue;
                fronts.push_back({{std::min({v[0].position.x, v[1].position.x, v[2].position.x}), std::max({v[0].position.x, v[1].position.x, v[2].position.x})},
                                  {std::min({v[0].position.y, v[1].position.y, v[2].position.y}), std::max({v[0].position.y, v[1].position.y, v[2].position.y})},
                                  v[0].position.z});
            }
        }
        for (SubMesh& sub : mesh.submeshes) {
            const uint32_t first = static_cast<uint32_t>(indices.size());
            for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                size_t at[3];
                glm::vec3 low(1e30f), high(-1e30f), middle(0.0f);
                for (uint32_t k = 0; k < 3; ++k) {
                    at[k] = static_cast<size_t>(static_cast<int64_t>(mesh.indices[sub.first_index + i + k]) + sub.vertex_offset);
                    const glm::vec3& p = mesh.vertices[at[k]].position;
                    low = glm::min(low, p);
                    high = glm::max(high, p);
                    middle += p / 3.0f;
                }
                if (glm::any(glm::lessThan(middle, kTenementMin)) || glm::any(glm::greaterThan(middle, kTenementMax))) continue;
                if (high.y <= lo || low.y >= hi) continue;
                const bool door =
                    part == TowerPart::Base && glm::all(glm::lessThan(low, kPaintedDoorMax)) && glm::all(glm::greaterThan(high, kPaintedDoorMin));
                if (low.y >= lo && high.y <= hi && !door) {
                    for (const size_t v : at) {
                        uint32_t& s = shared[v];
                        if (s == UINT32_MAX) {
                            s = static_cast<uint32_t>(vertices.size());
                            vertices.push_back(Turned(mesh.vertices[v]));
                        }
                        indices.push_back(s);
                    }
                    continue;
                }
                Polygon poly{mesh.vertices[at[0]], mesh.vertices[at[1]], mesh.vertices[at[2]]};
                if (low.y < lo) poly = Cut(poly, 1, lo, false);
                if (!poly.empty() && high.y > hi) poly = Cut(poly, 1, hi, true);
                if (poly.empty()) continue;
                if (!door) {
                    emit(poly);
                    continue;
                }
                for (int axis = 0; axis < 3 && !poly.empty(); ++axis) {
                    for (const bool below : {true, false}) {
                        const float value = below ? kPaintedDoorMin[axis] : kPaintedDoorMax[axis];
                        if (const Polygon out = Cut(poly, axis, value, below); !out.empty()) emit(out);
                        poly = Cut(poly, axis, value, !below);
                        if (poly.empty()) break;
                    }
                }
            }
            const size_t last = indices.size();
            for (size_t i = first; i + 2 < last; i += 3) {
                const Vertex t[3] = {vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]]};
                if (t[0].normal.z < 0.98f) continue;
                const glm::vec3 e1 = t[1].position - t[0].position, e2 = t[2].position - t[0].position;
                const float det = e1.x * e2.y - e2.x * e1.y;
                if (std::abs(det) < 1e-7f) continue;
                auto along_x = [&](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) { return ((b - a) * e2.y - (c - a) * e1.y) / det; };
                const glm::vec2 run0 = along_x(t[0].uv0, t[1].uv0, t[2].uv0), run1 = along_x(t[0].uv1, t[1].uv1, t[2].uv1), run2 = along_x(t[0].uv2, t[1].uv2, t[2].uv2);
                const bool wound = glm::cross(e1, e2).z > 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const Vertex& a = t[k];
                    const Vertex& b = t[(k + 1) % 3];
                    const Vertex& c = t[(k + 2) % 3];
                    const float x = a.position.x, height = std::abs(a.position.y - b.position.y);
                    if (std::abs(x - b.position.x) > 0.002f || x < c.position.x + 0.01f || height < 0.02f) continue;
                    const float y0 = std::min(a.position.y, b.position.y), y1 = std::max(a.position.y, b.position.y), z = a.position.z;
                    float back = z;
                    bool goes_on = false;
                    for (const Front& f : fronts) {
                        if (f.x.x > x + 0.01f || f.x.y < x + 0.01f) continue;
                        const float shared = std::min(f.y.y, y1) - std::max(f.y.x, y0);
                        if (shared <= 0.01f) continue;
                        if (f.z >= z - 0.02f) {
                            if (shared >= 0.6f * height) goes_on = true;
                        } else if (z - f.z < 1.2f) {
                            back = std::min(back, f.z);
                        }
                    }
                    if (goes_on || z - back < 0.02f) continue;
                    auto side = [&](const Vertex& from, bool deep) {
                        Vertex v = from;
                        const float by = deep ? z - back : 0.0f;
                        v.position.z = z - by;
                        v.normal = glm::vec3(1.0f, 0.0f, 0.0f);
                        v.tangent = glm::vec4(0.0f, from.tangent.y, from.tangent.x, -from.tangent.w);
                        v.uv0 -= run0 * by;
                        v.uv1 -= run1 * by;
                        v.uv2 -= run2 * by;
                        return v;
                    };
                    const bool up = b.position.y > a.position.y;
                    Polygon quad{side(a, false), side(b, false), side(b, true), side(a, true)};
                    if (up == wound) std::reverse(quad.begin(), quad.end());
                    const uint32_t base = static_cast<uint32_t>(vertices.size());
                    vertices.insert(vertices.end(), quad.begin(), quad.end());
                    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                    sides += 2;
                }
            }
            sub.first_index = first;
            sub.index_count = static_cast<uint32_t>(indices.size()) - first;
            sub.vertex_offset = 0;
        }
        LogInfo("street: the tower's {} cut from the street's tenement, {} triangles, {} of them the sides its front lacked", name, indices.size() / 3, sides);
        mesh.vertices = std::move(vertices);
        mesh.indices = std::move(indices);
        mesh.name = key;
    });
    if (!entry) return nullptr;
    for (SubMesh& sub : entry->mesh->submeshes) sub.double_sided = true;
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

}

bool BuildStreetHouse(ModelCache& models, int floors, StreetHouseMeshes& out) {
    out.tower.base = BuildTowerPart(models, TowerPart::Base);
    out.tower.storey = BuildTowerPart(models, TowerPart::Storey);
    out.tower.top = BuildTowerPart(models, TowerPart::Top);
    out.tower.storeys = std::max(floors - 3, 1);
    out.tower.storey_height = kStoreyHeight;
    if (!out.tower.base || !out.tower.storey || !out.tower.top) {
        LogWarn("street: the tower could not be cut from {}", kTenementModel);
        return false;
    }
    TextureManager& textures = models.Textures();
    const Materials mt = MakeMaterials(textures);
    auto srm = [&](const char* name, float s, float r, float refl) {
        return SolidTexture(textures, std::string("survival_srm_") + name, static_cast<uint8_t>(s * 255.0f), static_cast<uint8_t>(r * 255.0f),
                            static_cast<uint8_t>(refl * 255.0f), 255);
    };
    const uint32_t stipple = StippleTexture(textures);
    auto lit = [&](glm::vec3 albedo, uint32_t specular, uint32_t normal = TextureManager::kFlatNormal) {
        MaterialGpu m;
        m.kind = gpu::kKindDeferred;
        m.albedo = TextureManager::kWhite;
        m.normal = normal;
        m.specular = specular;
        m.flags = gpu::kMatConstantColor | (normal != TextureManager::kFlatNormal ? gpu::kMatNormalMap : 0u);
        m.albedo_factor = glm::vec4(albedo, 1.0f);
        return textures.AddMaterial(m);
    };
    MaterialGpu bulb_material;
    bulb_material.kind = gpu::kKindConstant;
    bulb_material.albedo = TextureManager::kWhite;
    bulb_material.albedo_factor = glm::vec4(1.0f, 0.78f, 0.5f, 1.0f);
    bulb_material.params = glm::vec4(0.0f, 90.0f, 0.0f, 0.0f);
    const uint32_t bulb = textures.AddMaterial(bulb_material);
    const uint32_t glass = lit({0.01f, 0.012f, 0.014f}, srm("window", 0.9f, 0.06f, 0.35f));
    const uint32_t inside_dark = lit({0.0f, 0.0f, 0.0f}, srm("bore", 0.0f, 1.0f, 0.0f));
    const uint32_t door_paint = lit({0.5f, 0.49f, 0.45f}, srm("door_paint", 0.3f, 0.45f, 0.03f));
    const uint32_t mat = lit({0.03f, 0.02f, 0.015f}, srm("mat", 0.05f, 0.95f, 0.0f), stipple);

    constexpr float zf = kHouseFront;
    const float door0 = kHouseDoorX - kHouseDoorHalf, door1 = kHouseDoorX + kHouseDoorHalf;
    const float head = kHouseDoorSill + 2.0f;
    {
        Kit w;
        const size_t wood = w.Part(mt.wood);
        auto board = [&](const glm::vec3& lo, const glm::vec3& hi) {
            const glm::vec3 d = hi - lo;
            auto face = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& e, const glm::vec3& n, float across, float up) {
                w.Quad(wood, a, b, c, e, n, {0.02f, 0.0f}, {0.02f + std::min(0.25f * across, 0.22f), 0.4f * up});
            };
            face({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, {0, 0, 1}, d.x, d.y);
            face({hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {0, 0, -1}, d.x, d.y);
            face({hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {1, 0, 0}, d.z, d.y);
            face({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, {-1, 0, 0}, d.z, d.y);
            face({lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {0, 1, 0}, d.z, d.x);
            face({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {0, -1, 0}, d.z, d.x);
        };
        board({kDoorwayX.x, kHouseDoorSill, zf - 0.06f}, {door0, kDoorwayTop, zf + 0.08f});
        board({door1, kHouseDoorSill, zf - 0.06f}, {kDoorwayX.y, kDoorwayTop, zf + 0.08f});
        board({door0, head, zf - 0.06f}, {door1, head + 0.09f, zf + 0.08f});
        board({door0, kDoorwayTop - 0.06f, zf - 0.06f}, {door1, kDoorwayTop, zf + 0.08f});
        board({kHouseDoorX - 0.02f, head + 0.09f, zf - 0.04f}, {kHouseDoorX + 0.02f, kDoorwayTop - 0.06f, zf + 0.06f});
        board({door0, kHouseDoorSill, zf - 0.07f}, {door1, kHouseDoorSill + 0.016f, zf + 0.11f});
        for (const float x : {kDoorwayX.x, kDoorwayX.y}) board({x - 0.02f, kHouseDoorSill, zf}, {x + 0.02f, kDoorwayTop, kDoorwayMouth});
        board({kDoorwayX.x - 0.02f, kDoorwayTop - 0.01f, zf}, {kDoorwayX.y + 0.02f, kDoorwayTop + 0.03f, kDoorwayMouth});
        out.doorway = w.Upload(models, "street_doorway");
    }
    Kit k;
    const size_t p_glass = k.Part(glass), p_dark = k.Part(inside_dark), p_bulb = k.Part(bulb, true), p_iron = k.Part(mt.iron), p_mat = k.Part(mat);
    k.Quad(p_glass, {door0, head + 0.09f, zf}, {door1, head + 0.09f, zf}, {door1, kDoorwayTop - 0.06f, zf}, {door0, kDoorwayTop - 0.06f, zf}, {0.0f, 0.0f, 1.0f});
    k.Box(p_mat, T({kHouseDoorX, kHouseDoorSill + 0.004f, zf + 0.45f}), {0.42f, 0.003f, 0.26f});
    const glm::vec3 lamp(kHouseDoorX, 2.78f, kDoorwayMouth + 0.2f);
    k.Box(p_iron, T({kHouseDoorX, lamp.y + 0.16f, kDoorwayMouth + 0.1f}), {0.015f, 0.015f, 0.1f}, 0.003f);
    k.Box(p_iron, T(lamp + glm::vec3(0.0f, 0.12f, 0.0f)), {0.08f, 0.015f, 0.08f}, 0.004f);
    k.Box(p_iron, T(lamp), {0.065f, 0.11f, 0.065f}, 0.01f);
    k.Box(p_bulb, T(lamp - glm::vec3(0.0f, 0.02f, 0.0f)), {0.045f, 0.075f, 0.045f}, 0.01f);
    {
        const float x0 = kDoorwayX.x, x1 = kDoorwayX.y, z0 = zf - 1.6f, z1 = zf - 0.06f, bottom = kHouseDoorSill, top = kDoorwayTop;
        k.Quad(p_dark, {x0, bottom, z0}, {x1, bottom, z0}, {x1, top, z0}, {x0, top, z0}, {0.0f, 0.0f, 1.0f});
        k.Quad(p_dark, {x0, bottom, z1}, {x0, bottom, z0}, {x0, top, z0}, {x0, top, z1}, {1.0f, 0.0f, 0.0f});
        k.Quad(p_dark, {x1, bottom, z0}, {x1, bottom, z1}, {x1, top, z1}, {x1, top, z0}, {-1.0f, 0.0f, 0.0f});
        k.Quad(p_dark, {x0, bottom + 0.002f, z1}, {x1, bottom + 0.002f, z1}, {x1, bottom + 0.002f, z0}, {x0, bottom + 0.002f, z0}, {0.0f, 1.0f, 0.0f});
        k.Quad(p_dark, {x0, top, z0}, {x1, top, z0}, {x1, top, z1}, {x0, top, z1}, {0.0f, -1.0f, 0.0f});
    }
    {
        Kit b;
        const size_t backdrop_wall = b.Part(mt.facade), backdrop_ground = b.Part(mt.backdrop_ground);
        AddBackdrop(b, backdrop_wall, backdrop_ground, static_cast<std::vector<SurvivalModels::Block>*>(nullptr));
        out.backdrop = b.Upload(models, "street_backdrop");
    }
    out.house = k.Upload(models, "street_house");
    {
        Kit d;
        const size_t paint = d.Part(door_paint), brass = d.Part(mt.brass);
        const float w = 2.0f * kHouseDoorHalf - 0.01f;
        d.Box(paint, T({0.5f * w, 0.5f * kHouseDoorHeight, 0.0f}), {0.5f * w, 0.5f * kHouseDoorHeight - 0.003f, 0.022f}, 0.004f);
        for (const float s : {1.0f, -1.0f}) {
            for (const float px : {0.25f * w, 0.75f * w}) {
                for (const glm::vec2 row : {glm::vec2(0.18f, 0.8f), glm::vec2(0.95f, 1.55f), glm::vec2(1.7f, 2.0f)}) {
                    d.Box(paint, T({px, 0.5f * (row.x + row.y), 0.025f * s}), {0.17f, 0.5f * (row.y - row.x), 0.005f}, 0.004f);
                }
            }
            const glm::mat4 out_dir = s > 0.0f ? glm::mat4(1.0f) : glm::rotate(glm::mat4(1.0f), kPi, glm::vec3(0, 1, 0));
            d.Cylinder(brass, T({w - 0.08f, 1.0f, 0.022f * s}) * out_dir, 0.006f, 0.035f, 10);
            d.Cylinder(brass, T({w - 0.08f, 1.0f, (0.022f + 0.035f) * s}) * out_dir, 0.027f, 0.02f, 16);
            d.Cylinder(brass, T({w - 0.08f, 1.18f, 0.022f * s}) * out_dir, 0.022f, 0.008f, 14);
        }
        d.Cylinder(brass, T({0.5f * w, 1.55f, 0.022f}), 0.009f, 0.006f, 12);
        out.door = d.Upload(models, "street_house_door");
    }
    LogInfo("street: the tower built, {} storeys, its cornice {:.1f} m up", out.tower.storeys + 3,
            20.1f + kStoreyHeight * static_cast<float>(out.tower.storeys - 1));
    return out.house && out.door && out.doorway;
}

std::unique_ptr<GpuMesh> BuildCityShell(ModelCache& models, const std::vector<CityBox>& boxes) {
    TextureManager& textures = models.Textures();
    const Materials mt = MakeMaterials(textures);
    Kit k;
    const size_t dark = k.Part(mt.dark);
    for (const CityBox& b : boxes) k.Box(dark, b.place, b.half, 0.0f, 1.0f);
    return k.Upload(models, "street_city_shell");
}

std::unique_ptr<GpuMesh> BuildCityGround(ModelCache& models, uint32_t road_material, float road_tile) {
    Kit k;
    const size_t road = k.Part(road_material);
    constexpr float kY = -0.012f, kStep = 60.0f;
    for (float x = -180.0f; x < 180.0f; x += kStep) {
        for (float z = -120.0f; z < 360.0f; z += kStep) {
            const float x1 = x + kStep, z1 = z + kStep;
            k.Quad(road, {x, kY, z}, {x1, kY, z}, {x1, kY, z1}, {x, kY, z1}, {0.0f, 1.0f, 0.0f}, glm::vec2(x, z) / road_tile, glm::vec2(x1, z1) / road_tile);
        }
    }
    return k.Upload(models, "street_city_ground");
}

}
