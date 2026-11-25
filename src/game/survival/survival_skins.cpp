#include "game/survival/survival_skins.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <stb_image.h>

#include "engine/core/log.h"
#include "engine/core/parallel_for.h"
#include "engine/fs/mods.h"
#include "engine/render/texture_manager.h"

namespace pt::game {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kSkinTexSize = 128;
constexpr uint32_t kMythicTexSize = 96;

inline uint32_t Hash2(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float HashF(int x, int y, uint32_t seed) {
    return static_cast<float>(Hash2(x, y, seed) & 0x00FFFFFFu) / 16777216.0f;
}

inline float Smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float TileNoise(float u, float v, int period, uint32_t seed) {
    const float fx = u * static_cast<float>(period);
    const float fy = v * static_cast<float>(period);
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = Smooth(fx - static_cast<float>(x0));
    const float ty = Smooth(fy - static_cast<float>(y0));
    auto wrap = [period](int i) {
        int m = i % period;
        return m < 0 ? m + period : m;
    };
    const int ix0 = wrap(x0), ix1 = wrap(x0 + 1);
    const int iy0 = wrap(y0), iy1 = wrap(y0 + 1);
    const float a = HashF(ix0, iy0, seed);
    const float b = HashF(ix1, iy0, seed);
    const float c = HashF(ix0, iy1, seed);
    const float d = HashF(ix1, iy1, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

float TileFbm(float u, float v, int base_period, int octaves, uint32_t seed) {
    float sum = 0.0f;
    float amp = 0.5f;
    float norm = 0.0f;
    int p = base_period;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * TileNoise(u, v, p, seed + static_cast<uint32_t>(i) * 97u);
        norm += amp;
        amp *= 0.5f;
        p *= 2;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

float TileVoronoiEdge(float u, float v, int cells, uint32_t seed) {
    const float fx = u * static_cast<float>(cells);
    const float fy = v * static_cast<float>(cells);
    const int cx = static_cast<int>(std::floor(fx));
    const int cy = static_cast<int>(std::floor(fy));
    auto wrap = [cells](int i) {
        int m = i % cells;
        return m < 0 ? m + cells : m;
    };
    float d1 = 10.0f, d2 = 10.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = wrap(cx + dx);
            const int ny = wrap(cy + dy);
            const float px = static_cast<float>(cx + dx) + 0.15f + 0.70f * HashF(nx, ny, seed);
            const float py = static_cast<float>(cy + dy) + 0.15f + 0.70f * HashF(nx, ny, seed + 53u);
            const float dist = std::hypot(px - fx, py - fy);
            if (dist < d1) {
                d2 = d1;
                d1 = dist;
            } else if (dist < d2) {
                d2 = dist;
            }
        }
    }
    return d2 - d1;
}

inline uint8_t ToU8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

inline glm::vec3 Lerp3(const glm::vec3& a, const glm::vec3& b, float t) {
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

struct SampleOut {
    glm::vec3 rgb;
    glm::vec3 srm;
};

SampleOut EvalMetalPattern(const SurvivalSkinDef& skin, float u, float v) {
    const uint32_t seed = static_cast<uint32_t>(skin.id) * 131u + 17u;
    const glm::vec3 A = skin.metal_a;
    const glm::vec3 B = skin.metal_b;
    const glm::vec3 C = skin.accent;
    const glm::vec3 S = skin.metal_srm;
    const glm::vec3 SA = skin.accent_srm;

    switch (skin.pattern) {
        case SkinPattern::FactoryOriginal:
        case SkinPattern::BrushedMetal: {
            const float grain = TileNoise(u * 0.25f, v * 4.0f, 16, seed) * 0.7f + TileNoise(u, v * 8.0f, 32, seed + 11u) * 0.3f;
            const float micro = (grain - 0.5f) * 0.14f;
            const glm::vec3 col = Lerp3(B, A, 0.5f + micro * 3.0f);
            return {col, glm::vec3(S.x, std::clamp(S.y + micro * 0.4f, 0.05f, 0.95f), S.z)};
        }
        case SkinPattern::CarbonWeave: {
            const float gx = u * 32.0f;
            const float gy = v * 32.0f;
            const int ix = static_cast<int>(std::floor(gx));
            const int iy = static_cast<int>(std::floor(gy));
            const float lx = gx - static_cast<float>(ix);
            const float ly = gy - static_cast<float>(iy);
            const bool horiz = ((ix + iy) & 1) == 0;
            const float fiber = horiz ? std::sin(ly * kPi) : std::sin(lx * kPi);
            const float bevel = horiz ? (0.35f + 0.65f * std::sin(lx * kPi)) : (0.35f + 0.65f * std::sin(ly * kPi));
            const float sheen = fiber * bevel;
            const glm::vec3 col = Lerp3(B, A, sheen);
            const float rough = std::clamp(S.y + (horiz ? -0.09f : 0.09f) + (1.0f - sheen) * 0.15f, 0.08f, 0.92f);
            return {col, glm::vec3(S.x * (0.7f + 0.35f * sheen), rough, S.z * (0.6f + 0.6f * sheen))};
        }
        case SkinPattern::DigitalCamo: {
            const float q1 = std::floor(u * 16.0f) / 16.0f;
            const float r1 = std::floor(v * 16.0f) / 16.0f;
            const float n1 = TileNoise(q1, r1, 8, seed);
            const float q2 = std::floor(u * 32.0f) / 32.0f;
            const float r2 = std::floor(v * 32.0f) / 32.0f;
            const float n2 = TileNoise(q2, r2, 16, seed + 7u);
            const float m = n1 * 0.72f + n2 * 0.28f;
            const glm::vec3 col = m < 0.36f ? B : (m < 0.68f ? A : C);
            return {col, glm::vec3(S.x, m < 0.36f ? S.y + 0.08f : S.y, S.z)};
        }
        case SkinPattern::OrganicCamo: {
            const float w1 = TileFbm(u, v, 4, 3, seed);
            const float w2 = TileFbm(u + w1 * 0.18f, v - w1 * 0.14f, 4, 3, seed + 29u);
            const glm::vec3 col = w2 < 0.38f ? B : (w2 < 0.68f ? A : C);
            return {col, S};
        }
        case SkinPattern::TigerStripe: {
            const float warp = TileFbm(u, v, 4, 3, seed) * 0.35f;
            const float band = std::sin((u * 4.0f + v * 2.0f + warp * 3.0f) * 2.0f * kPi);
            const float stripe = Smooth((band - 0.18f) / 0.24f);
            const float accent_band = Smooth((std::sin((u * 4.0f + v * 2.0f + warp * 3.0f + 0.35f) * 2.0f * kPi) - 0.78f) / 0.12f);
            glm::vec3 col = Lerp3(B, A, stripe);
            col = Lerp3(col, C, accent_band * 0.85f);
            return {col, Lerp3(S, SA, accent_band * 0.7f)};
        }
        case SkinPattern::HexGrid: {
            const float sx = u * 18.0f;
            const float sy = v * 18.0f * 1.1547f;
            const float row = std::floor(sy);
            const float ox = sx + (static_cast<int>(row) & 1 ? 0.5f : 0.0f);
            const float fx = ox - std::floor(ox) - 0.5f;
            const float fy = sy - row - 0.5f;
            const float hex_d = std::max(std::abs(fx) * 1.732f + std::abs(fy), std::abs(fy) * 2.0f);
            const float border = Smooth((hex_d - 0.80f) / 0.10f);
            const float cell_n = HashF(static_cast<int>(std::floor(ox)) % 18, static_cast<int>(row) % 21, seed);
            const glm::vec3 base = Lerp3(B, A, 0.4f + 0.6f * cell_n);
            const glm::vec3 col = Lerp3(base, C, border * 0.85f);
            return {col, Lerp3(S, SA, border)};
        }
        case SkinPattern::SplinterCamo: {
            const float s1 = std::fmod(u * 3.0f + v * 1.7f + 10.0f, 1.0f);
            const float s2 = std::fmod(u * 2.1f - v * 3.2f + 10.0f, 1.0f);
            const float s3 = TileNoise(std::floor(u * 5.0f) / 5.0f, std::floor(v * 5.0f) / 5.0f, 5, seed);
            const bool p1 = s1 > 0.52f;
            const bool p2 = s2 > 0.58f;
            const glm::vec3 col = (p1 && p2) ? C : (p1 ? A : (s3 > 0.5f ? B : Lerp3(A, B, 0.5f)));
            return {col, S};
        }
        case SkinPattern::HazardStripe: {
            const float diag = std::fmod((u + v) * 8.0f, 1.0f);
            const float stripe = diag < 0.45f ? 1.0f : 0.0f;
            const float wear = TileFbm(u, v, 8, 3, seed);
            const float chip = Smooth((wear - 0.64f) / 0.12f);
            glm::vec3 col = Lerp3(B, C, stripe);
            col = Lerp3(col, A, chip * 0.75f);
            return {col, Lerp3(S, glm::vec3(0.65f, 0.35f, 0.15f), chip)};
        }
        case SkinPattern::Damascus: {
            const float n1 = TileFbm(u, v, 4, 4, seed);
            const float n2 = TileFbm(u + n1 * 0.45f, v - n1 * 0.35f, 4, 3, seed + 41u);
            const float rings = 0.5f + 0.5f * std::sin((u * 5.0f + v * 2.0f + n2 * 5.5f) * 2.0f * kPi);
            const float crisp = Smooth((rings - 0.30f) / 0.40f);
            const glm::vec3 col = Lerp3(B, A, crisp);
            const glm::vec3 srm = Lerp3(glm::vec3(S.x * 0.78f, std::min(0.85f, S.y + 0.16f), S.z * 0.5f), S, crisp);
            return {col, srm};
        }
        case SkinPattern::CaseHardened: {
            const float n1 = TileFbm(u, v, 3, 4, seed);
            const float n2 = TileFbm(u + n1 * 0.38f, v + n1 * 0.25f, 3, 4, seed + 61u);
            const glm::vec3 blue{0.10f, 0.26f, 0.72f};
            const glm::vec3 violet{0.46f, 0.14f, 0.54f};
            const glm::vec3 gold{0.76f, 0.56f, 0.20f};
            const glm::vec3 silver{0.58f, 0.60f, 0.62f};
            glm::vec3 col = silver;
            if (n2 < 0.36f) col = Lerp3(blue, violet, n2 / 0.36f);
            else if (n2 < 0.58f) col = Lerp3(violet, gold, (n2 - 0.36f) / 0.22f);
            else if (n2 < 0.76f) col = Lerp3(gold, silver, (n2 - 0.58f) / 0.18f);
            else col = Lerp3(silver, blue, (n2 - 0.76f) / 0.24f);
            return {col, glm::vec3(0.88f, 0.18f + 0.10f * n1, 0.38f)};
        }
        case SkinPattern::Filigree: {
            const float a1 = std::sin(u * 8.0f * kPi) * std::cos(v * 8.0f * kPi);
            const float a2 = std::sin((u + v) * 6.0f * kPi + a1 * 1.4f);
            const float scroll = Smooth((std::abs(a2) - 0.88f) / 0.07f);
            const float fine = Smooth((std::abs(std::sin((u - v) * 12.0f * kPi + a1 * 1.2f)) - 0.93f) / 0.05f);
            const float eng = std::max(scroll, fine * 0.55f);
            const glm::vec3 col = Lerp3(A, Lerp3(A, C, 0.8f), eng);
            return {col, Lerp3(S, SA, eng)};
        }
        case SkinPattern::Marble: {
            const float f = TileFbm(u, v, 4, 4, seed);
            const float vein = 1.0f - Smooth(std::abs(std::sin((u * 3.0f + v * 2.0f + f * 3.2f) * kPi)) / 0.16f);
            const float sub = 1.0f - Smooth(std::abs(std::sin((u * 5.0f - v * 4.0f + f * 4.5f) * kPi)) / 0.12f);
            glm::vec3 col = Lerp3(A, B, f * 0.45f);
            col = Lerp3(col, C, std::max(vein, sub * 0.55f));
            return {col, Lerp3(S, SA, vein)};
        }
        case SkinPattern::BloodSplatter: {
            const float base_n = TileFbm(u, v, 6, 3, seed);
            const float pool = TileFbm(u, v, 5, 4, seed + 33u);
            const float spray = TileNoise(u, v, 32, seed + 77u);
            const float blood = std::max(Smooth((pool - 0.56f) / 0.08f), Smooth((spray - 0.86f) / 0.05f) * Smooth((pool - 0.38f) / 0.18f));
            const glm::vec3 steel = Lerp3(B, A, base_n);
            const glm::vec3 wet_blood = Lerp3(C * 0.45f, C, pool);
            const glm::vec3 col = Lerp3(steel, wet_blood, blood);
            const glm::vec3 srm = Lerp3(S, glm::vec3(0.82f, 0.12f, 0.22f), blood);
            return {col, srm};
        }
        case SkinPattern::SilentRust: {
            const float n = TileFbm(u, v, 5, 4, seed);
            const float flake = TileVoronoiEdge(u, v, 10, seed + 19u);
            const float rust_mask = Smooth((n - 0.34f) / 0.30f);
            const float crack = 1.0f - Smooth(flake / 0.07f);
            glm::vec3 col = Lerp3(B, A, rust_mask);
            col = Lerp3(col, C, crack * 0.7f);
            const glm::vec3 srm = Lerp3(S, glm::vec3(0.18f, 0.88f, 0.01f), std::max(rust_mask, crack));
            return {col, srm};
        }
        case SkinPattern::AsylumTile: {
            const float gx = std::fmod(u * 10.0f, 1.0f);
            const float gy = std::fmod(v * 10.0f, 1.0f);
            const float grout = (gx < 0.07f || gy < 0.07f) ? 1.0f : 0.0f;
            const float grime = TileFbm(u, v, 5, 4, seed);
            const float crack = 1.0f - Smooth(TileVoronoiEdge(u, v, 8, seed + 91u) / 0.035f);
            glm::vec3 col = Lerp3(A, B, grime * 0.55f);
            col = Lerp3(col, C, std::max(grout, crack * 0.8f));
            return {col, Lerp3(S, glm::vec3(0.16f, 0.85f, 0.02f), std::max(grout, grime * 0.6f))};
        }
        case SkinPattern::Glitch204863: {
            const float row = std::floor(v * 48.0f) / 48.0f;
            const float scan = 0.75f + 0.25f * std::sin(v * 96.0f * kPi);
            const float band_h = HashF(0, static_cast<int>(v * 48.0f), seed);
            const float shift = (band_h > 0.78f) ? (band_h - 0.78f) * 2.5f : 0.0f;
            const float block = TileNoise(std::floor((u + shift) * 16.0f) / 16.0f, row, 16, seed + 13u);
            const float hot = Smooth((block - 0.72f) / 0.12f) * (band_h > 0.62f ? 1.0f : 0.35f);
            const float static_n = HashF(static_cast<int>(u * 128.0f), static_cast<int>(v * 128.0f), seed + 5u);
            glm::vec3 col = Lerp3(B, A, scan * (0.45f + 0.55f * static_n));
            col = Lerp3(col, C, hot);
            return {col, Lerp3(S, SA, hot)};
        }
        case SkinPattern::CrackedMagma: {
            const float edge = TileVoronoiEdge(u, v, 8, seed);
            const float f = TileFbm(u, v, 6, 3, seed + 17u);
            const float crack = 1.0f - Smooth((edge + (f - 0.5f) * 0.05f) / 0.09f);
            const float core = 1.0f - Smooth(edge / 0.035f);
            glm::vec3 col = Lerp3(B, A, f * 0.5f);
            col = Lerp3(col, C, crack * 0.9f);
            col = Lerp3(col, glm::vec3(1.0f, 0.92f, 0.72f), core * 0.55f);
            return {col, Lerp3(S, SA, crack)};
        }
        case SkinPattern::AnodizedFade: {
            const float t = std::fmod(u * 0.85f + v * 0.45f + 0.1f * TileFbm(u, v, 3, 2, seed), 1.0f);
            const glm::vec3 col = t < 0.5f ? Lerp3(A, B, t * 2.0f) : Lerp3(B, C, (t - 0.5f) * 2.0f);
            return {col, S};
        }
        case SkinPattern::BoneCrack: {
            const float grain = TileFbm(u * 0.5f, v * 2.5f, 6, 3, seed);
            const float crack = 1.0f - Smooth(TileVoronoiEdge(u, v, 7, seed + 23u) / 0.045f);
            glm::vec3 col = Lerp3(A, B, grain * 0.45f);
            col = Lerp3(col, C, crack * 0.85f);
            return {col, Lerp3(S, glm::vec3(0.20f, 0.82f, 0.02f), crack)};
        }
    }
    return {A, S};
}

SampleOut EvalGripPattern(const SurvivalSkinDef& skin, float u, float v) {
    const uint32_t seed = static_cast<uint32_t>(skin.id) * 257u + 91u;
    const glm::vec3 A = skin.grip_a;
    const glm::vec3 B = skin.grip_b;
    const glm::vec3 S = skin.grip_srm;
    if (skin.rarity == SkinRarity::Tactical || skin.rarity == SkinRarity::Mythic) {
        const float gx = u * 28.0f;
        const float gy = v * 28.0f;
        const float d = std::abs(std::fmod(gx + gy, 1.0f) - 0.5f) + std::abs(std::fmod(gx - gy + 32.0f, 1.0f) - 0.5f);
        const float knurl = Smooth((0.55f - d) / 0.25f);
        const glm::vec3 col = Lerp3(B, A, 0.35f + 0.65f * knurl);
        return {col, glm::vec3(S.x, std::clamp(S.y + (1.0f - knurl) * 0.14f, 0.12f, 0.94f), S.z)};
    }
    const float f = TileFbm(u * 0.5f, v * 3.0f, 4, 4, seed);
    const float ring = 0.5f + 0.5f * std::sin((u * 3.0f + f * 3.8f) * 2.0f * kPi);
    const glm::vec3 col = Lerp3(B, A, 0.25f + 0.75f * ring);
    return {col, glm::vec3(S.x, std::clamp(S.y + (0.5f - ring) * 0.12f, 0.10f, 0.94f), S.z)};
}

SurvivalSkinDef MakeSkin(int id, const char* name, SkinRarity rarity, ReloadStyle reload, SkinPattern pattern, int price, int milestone_wave,
                         int daily_wins, glm::vec3 ma, glm::vec3 mb, glm::vec3 ga, glm::vec3 gb, glm::vec3 acc, glm::vec3 msrm, glm::vec3 gsrm,
                         glm::vec3 asrm, glm::vec3 aura = {0.95f, 0.18f, 0.14f}) {
    SurvivalSkinDef s;
    s.id = id;
    s.name = name;
    s.rarity = rarity;
    s.reload = reload;
    s.pattern = pattern;
    s.price = price;
    s.milestone_wave = milestone_wave;
    s.daily_wins_need = daily_wins;
    s.metal_a = ma;
    s.metal_b = mb;
    s.grip_a = ga;
    s.grip_b = gb;
    s.accent = acc;
    s.metal_srm = msrm;
    s.grip_srm = gsrm;
    s.accent_srm = asrm;
    s.aura_color = aura;
    return s;
}

const std::array<SurvivalSkinDef, kSurvivalSkinCount>& BuildSkinTable() {
    using R = SkinRarity;
    using L = ReloadStyle;
    using P = SkinPattern;
    static const std::array<SurvivalSkinDef, kSurvivalSkinCount> kTable = {{
        MakeSkin(0, "Factory Standard", R::Factory, L::Standard, P::FactoryOriginal, 0, 0, 0,
                 {0.28f, 0.28f, 0.30f}, {0.18f, 0.18f, 0.20f}, {0.32f, 0.20f, 0.13f}, {0.20f, 0.12f, 0.07f}, {0.50f, 0.44f, 0.32f},
                 {0.74f, 0.34f, 0.14f}, {0.26f, 0.68f, 0.03f}, {0.82f, 0.28f, 0.18f}),
        MakeSkin(1, "Blued Steel", R::Factory, L::Standard, P::BrushedMetal, 300, 25, 0,
                 {0.26f, 0.40f, 0.66f}, {0.16f, 0.26f, 0.46f}, {0.36f, 0.21f, 0.13f}, {0.22f, 0.12f, 0.07f}, {0.56f, 0.68f, 0.86f},
                 {0.78f, 0.36f, 0.24f}, {0.28f, 0.62f, 0.04f}, {0.84f, 0.28f, 0.28f}),
        MakeSkin(2, "Brushed Nickel", R::Factory, L::Standard, P::BrushedMetal, 400, 75, 0,
                 {0.72f, 0.72f, 0.70f}, {0.52f, 0.52f, 0.50f}, {0.20f, 0.19f, 0.20f}, {0.13f, 0.12f, 0.13f}, {0.86f, 0.84f, 0.80f},
                 {0.88f, 0.22f, 0.36f}, {0.24f, 0.68f, 0.03f}, {0.92f, 0.16f, 0.44f}),
        MakeSkin(3, "Matte Obsidian", R::Factory, L::Standard, P::BrushedMetal, 200, 0, 0,
                 {0.22f, 0.23f, 0.26f}, {0.14f, 0.15f, 0.17f}, {0.18f, 0.18f, 0.20f}, {0.12f, 0.12f, 0.14f}, {0.42f, 0.44f, 0.48f},
                 {0.62f, 0.44f, 0.12f}, {0.24f, 0.74f, 0.03f}, {0.76f, 0.32f, 0.18f}),
        MakeSkin(4, "Parkered Olive", R::Factory, L::Standard, P::BrushedMetal, 250, 0, 0,
                 {0.32f, 0.38f, 0.25f}, {0.20f, 0.25f, 0.16f}, {0.38f, 0.30f, 0.20f}, {0.24f, 0.19f, 0.12f}, {0.48f, 0.46f, 0.32f},
                 {0.52f, 0.52f, 0.08f}, {0.22f, 0.74f, 0.02f}, {0.68f, 0.38f, 0.12f}),
        MakeSkin(5, "Desert Sand", R::Factory, L::Standard, P::BrushedMetal, 250, 0, 0,
                 {0.62f, 0.52f, 0.38f}, {0.44f, 0.36f, 0.25f}, {0.32f, 0.25f, 0.18f}, {0.20f, 0.15f, 0.10f}, {0.74f, 0.62f, 0.44f},
                 {0.44f, 0.58f, 0.05f}, {0.22f, 0.78f, 0.02f}, {0.64f, 0.40f, 0.10f}),
        MakeSkin(6, "Slate Titanium", R::Factory, L::Standard, P::BrushedMetal, 300, 0, 0,
                 {0.42f, 0.46f, 0.52f}, {0.28f, 0.31f, 0.36f}, {0.24f, 0.25f, 0.28f}, {0.15f, 0.16f, 0.18f}, {0.64f, 0.68f, 0.74f},
                 {0.82f, 0.28f, 0.24f}, {0.26f, 0.68f, 0.03f}, {0.86f, 0.20f, 0.30f}),
        MakeSkin(7, "Gunmetal Ash", R::Factory, L::Standard, P::BrushedMetal, 250, 0, 0,
                 {0.36f, 0.35f, 0.34f}, {0.23f, 0.23f, 0.22f}, {0.42f, 0.30f, 0.20f}, {0.26f, 0.18f, 0.11f}, {0.56f, 0.54f, 0.50f},
                 {0.70f, 0.36f, 0.14f}, {0.26f, 0.64f, 0.03f}, {0.78f, 0.28f, 0.20f}),
        MakeSkin(8, "Midnight Navy", R::Factory, L::Standard, P::BrushedMetal, 300, 0, 0,
                 {0.20f, 0.30f, 0.52f}, {0.12f, 0.19f, 0.36f}, {0.19f, 0.22f, 0.28f}, {0.12f, 0.14f, 0.18f}, {0.52f, 0.62f, 0.80f},
                 {0.76f, 0.32f, 0.20f}, {0.24f, 0.72f, 0.03f}, {0.84f, 0.24f, 0.26f}),
        MakeSkin(9, "Forest Ranger", R::Factory, L::Standard, P::BrushedMetal, 300, 0, 0,
                 {0.20f, 0.38f, 0.26f}, {0.12f, 0.24f, 0.16f}, {0.44f, 0.30f, 0.18f}, {0.28f, 0.18f, 0.10f}, {0.60f, 0.52f, 0.32f},
                 {0.62f, 0.44f, 0.10f}, {0.26f, 0.64f, 0.03f}, {0.80f, 0.26f, 0.20f}),
        MakeSkin(10, "Cold Iron", R::Factory, L::Standard, P::BrushedMetal, 220, 0, 0,
                 {0.40f, 0.43f, 0.46f}, {0.25f, 0.27f, 0.30f}, {0.28f, 0.18f, 0.13f}, {0.17f, 0.11f, 0.08f}, {0.58f, 0.60f, 0.64f},
                 {0.78f, 0.38f, 0.18f}, {0.24f, 0.74f, 0.03f}, {0.82f, 0.28f, 0.22f}),
        MakeSkin(11, "Police Service", R::Factory, L::Standard, P::BrushedMetal, 350, 0, 0,
                 {0.22f, 0.26f, 0.34f}, {0.14f, 0.17f, 0.23f}, {0.86f, 0.84f, 0.78f}, {0.70f, 0.68f, 0.62f}, {0.82f, 0.82f, 0.85f},
                 {0.84f, 0.24f, 0.26f}, {0.44f, 0.34f, 0.12f}, {0.90f, 0.18f, 0.36f}),

        MakeSkin(12, "Carbon Fiber", R::Tactical, L::Tactical, P::CarbonWeave, 650, 0, 0,
                 {0.34f, 0.36f, 0.40f}, {0.14f, 0.15f, 0.18f}, {0.24f, 0.25f, 0.28f}, {0.13f, 0.14f, 0.16f}, {0.68f, 0.70f, 0.76f},
                 {0.80f, 0.26f, 0.24f}, {0.54f, 0.40f, 0.10f}, {0.88f, 0.20f, 0.32f}),
        MakeSkin(13, "Urban Digital", R::Tactical, L::Standard, P::DigitalCamo, 450, 0, 0,
                 {0.42f, 0.44f, 0.47f}, {0.20f, 0.22f, 0.25f}, {0.26f, 0.27f, 0.30f}, {0.15f, 0.16f, 0.18f}, {0.66f, 0.68f, 0.71f},
                 {0.48f, 0.54f, 0.07f}, {0.22f, 0.74f, 0.03f}, {0.68f, 0.36f, 0.12f}),
        MakeSkin(14, "Woodland Camo", R::Tactical, L::Standard, P::OrganicCamo, 450, 0, 0,
                 {0.28f, 0.40f, 0.22f}, {0.16f, 0.20f, 0.12f}, {0.24f, 0.21f, 0.15f}, {0.15f, 0.13f, 0.09f}, {0.46f, 0.36f, 0.22f},
                 {0.42f, 0.62f, 0.05f}, {0.20f, 0.78f, 0.02f}, {0.56f, 0.46f, 0.08f}),
        MakeSkin(15, "Arctic Ghost", R::Tactical, L::Tactical, P::OrganicCamo, 600, 0, 0,
                 {0.84f, 0.87f, 0.90f}, {0.52f, 0.58f, 0.64f}, {0.74f, 0.78f, 0.82f}, {0.46f, 0.50f, 0.56f}, {0.28f, 0.32f, 0.38f},
                 {0.60f, 0.40f, 0.14f}, {0.34f, 0.56f, 0.05f}, {0.78f, 0.28f, 0.22f}),
        MakeSkin(16, "Night Tiger", R::Tactical, L::Tactical, P::TigerStripe, 650, 0, 0,
                 {0.36f, 0.40f, 0.46f}, {0.14f, 0.16f, 0.20f}, {0.22f, 0.24f, 0.28f}, {0.13f, 0.14f, 0.17f}, {0.58f, 0.62f, 0.70f},
                 {0.66f, 0.38f, 0.15f}, {0.26f, 0.70f, 0.03f}, {0.84f, 0.24f, 0.26f}),
        MakeSkin(17, "Crimson Tiger", R::Tactical, L::Tactical, P::TigerStripe, 800, 0, 0,
                 {0.30f, 0.30f, 0.34f}, {0.14f, 0.14f, 0.16f}, {0.26f, 0.12f, 0.12f}, {0.14f, 0.07f, 0.07f}, {0.84f, 0.18f, 0.16f},
                 {0.72f, 0.32f, 0.16f}, {0.28f, 0.68f, 0.03f}, {0.86f, 0.20f, 0.26f}),
        MakeSkin(18, "Desert Storm", R::Tactical, L::Standard, P::OrganicCamo, 480, 0, 0,
                 {0.66f, 0.56f, 0.40f}, {0.40f, 0.32f, 0.22f}, {0.34f, 0.27f, 0.19f}, {0.20f, 0.16f, 0.11f}, {0.82f, 0.72f, 0.52f},
                 {0.40f, 0.62f, 0.04f}, {0.20f, 0.78f, 0.02f}, {0.58f, 0.44f, 0.08f}),
        MakeSkin(19, "Hex Spec-Ops", R::Tactical, L::Tactical, P::HexGrid, 850, 0, 0,
                 {0.26f, 0.30f, 0.38f}, {0.14f, 0.17f, 0.22f}, {0.20f, 0.22f, 0.28f}, {0.12f, 0.14f, 0.18f}, {0.26f, 0.66f, 0.96f},
                 {0.76f, 0.28f, 0.22f}, {0.30f, 0.60f, 0.05f}, {0.90f, 0.16f, 0.34f}),
        MakeSkin(20, "Hazard Stripe", R::Tactical, L::Standard, P::HazardStripe, 550, 0, 0,
                 {0.32f, 0.32f, 0.36f}, {0.16f, 0.16f, 0.18f}, {0.24f, 0.24f, 0.26f}, {0.14f, 0.14f, 0.15f}, {0.94f, 0.74f, 0.14f},
                 {0.62f, 0.42f, 0.12f}, {0.22f, 0.74f, 0.03f}, {0.74f, 0.28f, 0.16f}),
        MakeSkin(21, "Stealth Splinter", R::Tactical, L::Tactical, P::SplinterCamo, 750, 0, 0,
                 {0.34f, 0.38f, 0.44f}, {0.16f, 0.19f, 0.23f}, {0.22f, 0.25f, 0.28f}, {0.13f, 0.15f, 0.17f}, {0.58f, 0.64f, 0.70f},
                 {0.60f, 0.40f, 0.12f}, {0.24f, 0.70f, 0.03f}, {0.80f, 0.26f, 0.20f}),
        MakeSkin(22, "Moss Swamp", R::Tactical, L::Standard, P::OrganicCamo, 500, 0, 0,
                 {0.26f, 0.36f, 0.24f}, {0.15f, 0.20f, 0.14f}, {0.26f, 0.23f, 0.16f}, {0.16f, 0.14f, 0.09f}, {0.40f, 0.50f, 0.28f},
                 {0.40f, 0.62f, 0.04f}, {0.20f, 0.78f, 0.02f}, {0.54f, 0.46f, 0.08f}),
        MakeSkin(23, "Copper Tactical", R::Tactical, L::Tactical, P::TigerStripe, 780, 0, 0,
                 {0.28f, 0.27f, 0.30f}, {0.15f, 0.15f, 0.17f}, {0.26f, 0.18f, 0.14f}, {0.15f, 0.10f, 0.08f}, {0.84f, 0.48f, 0.26f},
                 {0.74f, 0.30f, 0.18f}, {0.24f, 0.70f, 0.03f}, {0.88f, 0.20f, 0.34f}),
        MakeSkin(24, "Phosphor Night", R::Tactical, L::Tactical, P::HexGrid, 880, 0, 0,
                 {0.22f, 0.28f, 0.26f}, {0.13f, 0.17f, 0.16f}, {0.18f, 0.24f, 0.20f}, {0.11f, 0.15f, 0.12f}, {0.34f, 0.96f, 0.48f},
                 {0.72f, 0.32f, 0.18f}, {0.26f, 0.68f, 0.04f}, {0.90f, 0.18f, 0.30f}),
        MakeSkin(25, "Ballistic Kevlar", R::Tactical, L::Tactical, P::CarbonWeave, 900, 0, 0,
                 {0.42f, 0.38f, 0.28f}, {0.20f, 0.18f, 0.13f}, {0.26f, 0.23f, 0.17f}, {0.15f, 0.13f, 0.10f}, {0.78f, 0.70f, 0.50f},
                 {0.70f, 0.32f, 0.16f}, {0.38f, 0.54f, 0.06f}, {0.84f, 0.22f, 0.26f}),

        MakeSkin(26, "Damascus Steel", R::Artisan, L::Gunslinger, P::Damascus, 1500, 0, 0,
                 {0.64f, 0.66f, 0.70f}, {0.24f, 0.26f, 0.30f}, {0.40f, 0.20f, 0.14f}, {0.22f, 0.10f, 0.07f}, {0.82f, 0.82f, 0.85f},
                 {0.88f, 0.20f, 0.36f}, {0.32f, 0.52f, 0.05f}, {0.92f, 0.14f, 0.44f}),
        MakeSkin(27, "Case Hardened", R::Artisan, L::Gunslinger, P::CaseHardened, 1800, 0, 0,
                 {0.28f, 0.46f, 0.78f}, {0.72f, 0.54f, 0.26f}, {0.36f, 0.22f, 0.13f}, {0.22f, 0.12f, 0.07f}, {0.86f, 0.70f, 0.32f},
                 {0.90f, 0.18f, 0.40f}, {0.28f, 0.58f, 0.04f}, {0.92f, 0.16f, 0.42f}),
        MakeSkin(28, "Solid Gold", R::Artisan, L::Gunslinger, P::BrushedMetal, 2200, 100, 0,
                 {0.88f, 0.70f, 0.26f}, {0.68f, 0.52f, 0.18f}, {0.88f, 0.86f, 0.78f}, {0.70f, 0.66f, 0.56f}, {0.98f, 0.84f, 0.38f},
                 {0.94f, 0.14f, 0.52f}, {0.42f, 0.34f, 0.10f}, {0.96f, 0.10f, 0.58f}),
        MakeSkin(29, "Mirror Chrome", R::Artisan, L::Gunslinger, P::BrushedMetal, 1700, 0, 0,
                 {0.86f, 0.88f, 0.90f}, {0.70f, 0.72f, 0.76f}, {0.18f, 0.18f, 0.21f}, {0.11f, 0.11f, 0.13f}, {0.95f, 0.97f, 0.99f},
                 {0.96f, 0.10f, 0.62f}, {0.50f, 0.26f, 0.14f}, {0.98f, 0.08f, 0.68f}),
        MakeSkin(30, "Rose Gold Filigree", R::Artisan, L::Gunslinger, P::Filigree, 2000, 0, 0,
                 {0.84f, 0.56f, 0.48f}, {0.62f, 0.36f, 0.30f}, {0.88f, 0.84f, 0.82f}, {0.70f, 0.64f, 0.62f}, {0.96f, 0.76f, 0.66f},
                 {0.90f, 0.18f, 0.44f}, {0.46f, 0.30f, 0.12f}, {0.95f, 0.12f, 0.52f}),
        MakeSkin(31, "Silver Engraving", R::Artisan, L::Gunslinger, P::Filigree, 1650, 0, 0,
                 {0.78f, 0.80f, 0.82f}, {0.56f, 0.58f, 0.60f}, {0.36f, 0.16f, 0.11f}, {0.22f, 0.09f, 0.06f}, {0.26f, 0.26f, 0.30f},
                 {0.90f, 0.18f, 0.42f}, {0.32f, 0.48f, 0.05f}, {0.64f, 0.42f, 0.16f}),
        MakeSkin(32, "Royal Ivory", R::Artisan, L::Standard, P::Marble, 1250, 0, 0,
                 {0.88f, 0.85f, 0.78f}, {0.72f, 0.68f, 0.60f}, {0.38f, 0.22f, 0.13f}, {0.24f, 0.13f, 0.08f}, {0.84f, 0.66f, 0.28f},
                 {0.58f, 0.28f, 0.14f}, {0.28f, 0.58f, 0.04f}, {0.92f, 0.16f, 0.46f}),
        MakeSkin(33, "Emerald Marble", R::Artisan, L::Standard, P::Marble, 1400, 0, 0,
                 {0.18f, 0.50f, 0.34f}, {0.10f, 0.28f, 0.19f}, {0.16f, 0.32f, 0.22f}, {0.09f, 0.18f, 0.13f}, {0.88f, 0.72f, 0.30f},
                 {0.78f, 0.22f, 0.26f}, {0.42f, 0.36f, 0.08f}, {0.94f, 0.14f, 0.48f}),
        MakeSkin(34, "Obsidian & Gold", R::Artisan, L::Gunslinger, P::Filigree, 2400, 0, 0,
                 {0.20f, 0.20f, 0.24f}, {0.12f, 0.12f, 0.15f}, {0.18f, 0.18f, 0.21f}, {0.11f, 0.11f, 0.13f}, {0.94f, 0.76f, 0.26f},
                 {0.78f, 0.26f, 0.24f}, {0.36f, 0.48f, 0.06f}, {0.96f, 0.12f, 0.56f}),
        MakeSkin(35, "Cobalt Anodized", R::Artisan, L::Tactical, P::BrushedMetal, 1200, 0, 0,
                 {0.18f, 0.42f, 0.88f}, {0.10f, 0.26f, 0.58f}, {0.20f, 0.24f, 0.32f}, {0.12f, 0.14f, 0.20f}, {0.62f, 0.78f, 0.98f},
                 {0.88f, 0.20f, 0.38f}, {0.28f, 0.62f, 0.04f}, {0.92f, 0.16f, 0.44f}),
        MakeSkin(36, "Amethyst Anodized", R::Artisan, L::Tactical, P::BrushedMetal, 1250, 0, 0,
                 {0.54f, 0.24f, 0.80f}, {0.34f, 0.14f, 0.52f}, {0.24f, 0.17f, 0.30f}, {0.14f, 0.10f, 0.19f}, {0.84f, 0.66f, 0.96f},
                 {0.88f, 0.20f, 0.38f}, {0.28f, 0.62f, 0.04f}, {0.92f, 0.16f, 0.44f}),
        MakeSkin(37, "Ruby Lacquer", R::Artisan, L::Gunslinger, P::BrushedMetal, 1450, 0, 0,
                 {0.76f, 0.14f, 0.18f}, {0.48f, 0.08f, 0.11f}, {0.20f, 0.16f, 0.17f}, {0.12f, 0.09f, 0.10f}, {0.90f, 0.74f, 0.36f},
                 {0.92f, 0.16f, 0.42f}, {0.42f, 0.34f, 0.08f}, {0.94f, 0.14f, 0.50f}),
        MakeSkin(38, "Bronze Patina", R::Artisan, L::Standard, P::SilentRust, 1150, 0, 0,
                 {0.60f, 0.42f, 0.24f}, {0.38f, 0.26f, 0.15f}, {0.34f, 0.22f, 0.14f}, {0.20f, 0.12f, 0.08f}, {0.28f, 0.62f, 0.52f},
                 {0.78f, 0.32f, 0.24f}, {0.26f, 0.64f, 0.03f}, {0.46f, 0.58f, 0.10f}),
        MakeSkin(39, "Copper Damascus", R::Artisan, L::Gunslinger, P::Damascus, 1950, 0, 0,
                 {0.82f, 0.48f, 0.26f}, {0.26f, 0.19f, 0.16f}, {0.36f, 0.19f, 0.12f}, {0.20f, 0.10f, 0.06f}, {0.92f, 0.58f, 0.32f},
                 {0.90f, 0.18f, 0.40f}, {0.28f, 0.58f, 0.04f}, {0.94f, 0.14f, 0.48f}),

        MakeSkin(40, "Bloodstained", R::Horror, L::Tactical, P::BloodSplatter, 1600, 0, 1,
                 {0.38f, 0.38f, 0.42f}, {0.22f, 0.22f, 0.25f}, {0.34f, 0.22f, 0.16f}, {0.20f, 0.12f, 0.09f}, {0.68f, 0.08f, 0.08f},
                 {0.74f, 0.34f, 0.18f}, {0.28f, 0.64f, 0.03f}, {0.86f, 0.12f, 0.28f}),
        MakeSkin(41, "Silent Rust", R::Horror, L::Gunslinger, P::SilentRust, 2000, 0, 3,
                 {0.52f, 0.25f, 0.13f}, {0.26f, 0.15f, 0.11f}, {0.28f, 0.18f, 0.12f}, {0.16f, 0.10f, 0.07f}, {0.68f, 0.32f, 0.14f},
                 {0.40f, 0.72f, 0.06f}, {0.22f, 0.80f, 0.02f}, {0.48f, 0.66f, 0.07f}),
        MakeSkin(42, "Fog & Ash", R::Horror, L::Standard, P::BoneCrack, 1650, 0, 0,
                 {0.72f, 0.74f, 0.76f}, {0.48f, 0.50f, 0.52f}, {0.22f, 0.22f, 0.24f}, {0.14f, 0.14f, 0.16f}, {0.34f, 0.35f, 0.38f},
                 {0.44f, 0.64f, 0.06f}, {0.22f, 0.78f, 0.02f}, {0.56f, 0.52f, 0.08f}),
        MakeSkin(43, "Red Hallway", R::Horror, L::Tactical, P::Marble, 2400, 0, 0,
                 {0.58f, 0.10f, 0.10f}, {0.26f, 0.05f, 0.05f}, {0.28f, 0.08f, 0.08f}, {0.15f, 0.04f, 0.04f}, {0.94f, 0.18f, 0.16f},
                 {0.84f, 0.20f, 0.28f}, {0.38f, 0.46f, 0.06f}, {0.92f, 0.14f, 0.38f}),
        MakeSkin(44, "Lisa's Dress", R::Horror, L::Tactical, P::BloodSplatter, 2200, 0, 0,
                 {0.76f, 0.74f, 0.70f}, {0.50f, 0.48f, 0.46f}, {0.26f, 0.24f, 0.22f}, {0.16f, 0.14f, 0.13f}, {0.64f, 0.08f, 0.08f},
                 {0.46f, 0.62f, 0.05f}, {0.22f, 0.76f, 0.02f}, {0.84f, 0.14f, 0.24f}),
        MakeSkin(45, "Torn Photograph", R::Horror, L::Standard, P::SilentRust, 1750, 0, 0,
                 {0.60f, 0.50f, 0.36f}, {0.32f, 0.25f, 0.17f}, {0.32f, 0.23f, 0.15f}, {0.18f, 0.12f, 0.08f}, {0.74f, 0.16f, 0.14f},
                 {0.48f, 0.58f, 0.07f}, {0.24f, 0.70f, 0.02f}, {0.74f, 0.28f, 0.16f}),
        MakeSkin(46, "Fetus Sink", R::Horror, L::Standard, P::BloodSplatter, 1900, 0, 0,
                 {0.86f, 0.84f, 0.80f}, {0.64f, 0.62f, 0.58f}, {0.68f, 0.66f, 0.62f}, {0.48f, 0.46f, 0.42f}, {0.66f, 0.10f, 0.10f},
                 {0.78f, 0.22f, 0.20f}, {0.66f, 0.28f, 0.14f}, {0.86f, 0.12f, 0.28f}),
        MakeSkin(47, "Asylum Tile", R::Horror, L::Standard, P::AsylumTile, 1850, 0, 0,
                 {0.66f, 0.72f, 0.64f}, {0.40f, 0.44f, 0.38f}, {0.34f, 0.36f, 0.32f}, {0.20f, 0.22f, 0.19f}, {0.38f, 0.22f, 0.14f},
                 {0.68f, 0.34f, 0.14f}, {0.24f, 0.74f, 0.02f}, {0.40f, 0.74f, 0.05f}),
        MakeSkin(48, "Hanging Lamp", R::Horror, L::Gunslinger, P::Damascus, 2300, 0, 0,
                 {0.74f, 0.50f, 0.22f}, {0.24f, 0.17f, 0.12f}, {0.28f, 0.19f, 0.12f}, {0.16f, 0.10f, 0.06f}, {0.94f, 0.70f, 0.30f},
                 {0.84f, 0.24f, 0.28f}, {0.26f, 0.66f, 0.03f}, {0.92f, 0.16f, 0.44f}),
        MakeSkin(49, "Paper Bag", R::Horror, L::Standard, P::BoneCrack, 1700, 0, 0,
                 {0.58f, 0.44f, 0.28f}, {0.40f, 0.30f, 0.18f}, {0.42f, 0.31f, 0.19f}, {0.26f, 0.18f, 0.11f}, {0.22f, 0.18f, 0.14f},
                 {0.28f, 0.80f, 0.02f}, {0.22f, 0.82f, 0.02f}, {0.36f, 0.68f, 0.03f}),
        MakeSkin(50, "Chandelier Bone", R::Horror, L::Tactical, P::BoneCrack, 2100, 0, 0,
                 {0.86f, 0.82f, 0.72f}, {0.62f, 0.57f, 0.48f}, {0.74f, 0.70f, 0.60f}, {0.50f, 0.46f, 0.38f}, {0.30f, 0.22f, 0.16f},
                 {0.50f, 0.48f, 0.08f}, {0.38f, 0.58f, 0.04f}, {0.60f, 0.42f, 0.12f}),
        MakeSkin(51, "Plague Copper", R::Horror, L::Standard, P::SilentRust, 1950, 0, 0,
                 {0.32f, 0.56f, 0.40f}, {0.18f, 0.30f, 0.22f}, {0.26f, 0.28f, 0.20f}, {0.15f, 0.17f, 0.12f}, {0.62f, 0.38f, 0.20f},
                 {0.66f, 0.40f, 0.18f}, {0.24f, 0.72f, 0.03f}, {0.80f, 0.26f, 0.24f}),
        MakeSkin(52, "Noir Detective", R::Horror, L::Gunslinger, P::Filigree, 2600, 0, 0,
                 {0.80f, 0.80f, 0.82f}, {0.22f, 0.22f, 0.25f}, {0.20f, 0.20f, 0.22f}, {0.12f, 0.12f, 0.14f}, {0.16f, 0.16f, 0.20f},
                 {0.88f, 0.20f, 0.36f}, {0.36f, 0.48f, 0.06f}, {0.76f, 0.28f, 0.20f}),
        MakeSkin(53, "Siren Warning", R::Horror, L::Tactical, P::HazardStripe, 2500, 0, 0,
                 {0.30f, 0.28f, 0.30f}, {0.16f, 0.15f, 0.16f}, {0.24f, 0.13f, 0.13f}, {0.14f, 0.07f, 0.07f}, {0.92f, 0.18f, 0.12f},
                 {0.72f, 0.34f, 0.18f}, {0.24f, 0.70f, 0.03f}, {0.90f, 0.18f, 0.30f}),

        MakeSkin(54, "204863 Static", R::Mythic, L::Telekinetic, P::Glitch204863, 5000, 0, 7,
                 {0.62f, 0.64f, 0.68f}, {0.16f, 0.16f, 0.20f}, {0.28f, 0.11f, 0.12f}, {0.14f, 0.06f, 0.07f}, {1.00f, 0.16f, 0.14f},
                 {0.88f, 0.18f, 0.40f}, {0.44f, 0.42f, 0.12f}, {0.96f, 0.10f, 0.54f}, {1.00f, 0.16f, 0.14f}),
        MakeSkin(55, "Eclipse", R::Mythic, L::Telekinetic, P::CrackedMagma, 5200, 0, 0,
                 {0.20f, 0.20f, 0.26f}, {0.11f, 0.11f, 0.15f}, {0.18f, 0.16f, 0.14f}, {0.10f, 0.09f, 0.08f}, {1.00f, 0.78f, 0.22f},
                 {0.88f, 0.20f, 0.38f}, {0.38f, 0.48f, 0.08f}, {0.98f, 0.10f, 0.58f}, {1.00f, 0.76f, 0.22f}),
        MakeSkin(56, "Crimson Void", R::Mythic, L::Telekinetic, P::CrackedMagma, 5500, 0, 0,
                 {0.22f, 0.11f, 0.14f}, {0.11f, 0.05f, 0.07f}, {0.24f, 0.08f, 0.10f}, {0.13f, 0.04f, 0.06f}, {1.00f, 0.12f, 0.18f},
                 {0.92f, 0.16f, 0.44f}, {0.44f, 0.38f, 0.12f}, {0.98f, 0.08f, 0.56f}, {1.00f, 0.12f, 0.20f}),
        MakeSkin(57, "Spectral UV", R::Mythic, L::Telekinetic, P::AnodizedFade, 4800, 0, 0,
                 {0.14f, 0.84f, 0.98f}, {0.30f, 0.24f, 0.82f}, {0.16f, 0.26f, 0.40f}, {0.10f, 0.14f, 0.24f}, {0.74f, 0.28f, 1.00f},
                 {0.92f, 0.14f, 0.46f}, {0.42f, 0.40f, 0.12f}, {0.96f, 0.10f, 0.56f}, {0.26f, 0.86f, 1.00f}),
        MakeSkin(58, "Magma Core", R::Mythic, L::Telekinetic, P::CrackedMagma, 5400, 0, 0,
                 {0.26f, 0.21f, 0.20f}, {0.15f, 0.12f, 0.12f}, {0.24f, 0.14f, 0.10f}, {0.14f, 0.08f, 0.06f}, {1.00f, 0.44f, 0.08f},
                 {0.68f, 0.44f, 0.16f}, {0.34f, 0.58f, 0.05f}, {0.96f, 0.12f, 0.44f}, {1.00f, 0.46f, 0.10f}),
        MakeSkin(59, "Prism Fade", R::Mythic, L::Telekinetic, P::AnodizedFade, 6000, 0, 0,
                 {0.48f, 0.22f, 0.96f}, {0.96f, 0.26f, 0.62f}, {0.26f, 0.20f, 0.34f}, {0.15f, 0.11f, 0.22f}, {1.00f, 0.82f, 0.24f},
                 {0.94f, 0.12f, 0.52f}, {0.46f, 0.34f, 0.14f}, {0.96f, 0.10f, 0.56f}, {0.90f, 0.32f, 0.98f}),
        MakeSkin(60, "Radioactive", R::Mythic, L::Telekinetic, P::CrackedMagma, 4600, 0, 0,
                 {0.22f, 0.26f, 0.22f}, {0.13f, 0.16f, 0.13f}, {0.18f, 0.26f, 0.18f}, {0.11f, 0.15f, 0.11f}, {0.42f, 1.00f, 0.20f},
                 {0.80f, 0.26f, 0.26f}, {0.34f, 0.56f, 0.06f}, {0.94f, 0.12f, 0.44f}, {0.40f, 1.00f, 0.22f}),
        MakeSkin(61, "Astral Gold", R::Mythic, L::Telekinetic, P::Filigree, 6500, 0, 0,
                 {0.18f, 0.22f, 0.48f}, {0.10f, 0.13f, 0.30f}, {0.18f, 0.18f, 0.32f}, {0.11f, 0.11f, 0.20f}, {1.00f, 0.84f, 0.32f},
                 {0.90f, 0.16f, 0.44f}, {0.42f, 0.40f, 0.12f}, {0.98f, 0.10f, 0.58f}, {0.98f, 0.84f, 0.36f}),
        MakeSkin(62, "Blood Moon", R::Mythic, L::Telekinetic, P::Damascus, 6200, 0, 0,
                 {0.82f, 0.14f, 0.16f}, {0.24f, 0.06f, 0.08f}, {0.26f, 0.09f, 0.10f}, {0.14f, 0.05f, 0.06f}, {0.94f, 0.92f, 0.96f},
                 {0.92f, 0.14f, 0.46f}, {0.44f, 0.36f, 0.12f}, {0.96f, 0.10f, 0.54f}, {0.98f, 0.18f, 0.20f}),
        MakeSkin(63, "The Dawn (204863)", R::Mythic, L::Telekinetic, P::Filigree, 8500, 0, 0,
                 {0.94f, 0.92f, 0.88f}, {0.74f, 0.70f, 0.64f}, {0.88f, 0.84f, 0.78f}, {0.68f, 0.62f, 0.56f}, {1.00f, 0.78f, 0.26f},
                 {0.96f, 0.10f, 0.58f}, {0.58f, 0.26f, 0.18f}, {0.98f, 0.08f, 0.64f}, {1.00f, 0.86f, 0.46f}),
    }};
    return kTable;
}

}

const std::array<SurvivalSkinDef, kSurvivalSkinCount>& SurvivalSkins() {
    return BuildSkinTable();
}

const SurvivalSkinDef& SurvivalSkin(int id) {
    const auto& table = BuildSkinTable();
    return table[static_cast<size_t>(std::clamp(id, 0, kSurvivalSkinCount - 1))];
}

const std::array<SurvivalTorchDef, kSurvivalTorchCount>& SurvivalTorches() {
    static const std::array<SurvivalTorchDef, kSurvivalTorchCount> kTorches = {{
        {0, "pc_torch_white", 0, 0, 0, {1.0f, 1.0f, 1.0f}},
        {1, "pc_torch_amber", 800, 50, 0, {1.15f, 0.86f, 0.55f}},
        {2, "pc_torch_red", 1500, 100, 0, {1.70f, 0.32f, 0.26f}},
        {3, "pc_torch_uv", 1000, 0, 2, {0.52f, 0.84f, 1.48f}},
        {4, "pc_torch_emerald", 1200, 0, 5, {0.48f, 1.32f, 0.62f}},
    }};
    return kTorches;
}

const SurvivalTorchDef& SurvivalTorch(int id) {
    const auto& table = SurvivalTorches();
    return table[static_cast<size_t>(std::clamp(id, 0, kSurvivalTorchCount - 1))];
}

const std::array<DailyMutatorDef, kSurvivalDailyMutatorCount>& DailyMutators() {
    static const std::array<DailyMutatorDef, kSurvivalDailyMutatorCount> kMutators = {{
        {0, "pc_daily_mut_bloodmoon", "pc_daily_mut_bloodmoon_note", 1.5f, 1.20f, 1.00f, 1.05f, 1.00f, 1.00f, 2.00f, 1.00f, 1.00f, 1.00f, 1.00f,
         true, false, false, false, false, 2},
        {1, "pc_daily_mut_blackout", "pc_daily_mut_blackout_note", 1.4f, 1.05f, 0.95f, 1.00f, 1.00f, 1.00f, 1.15f, 1.00f, 1.00f, 1.00f, 2.00f,
         false, true, false, false, false, 6},
        {2, "pc_daily_mut_glass", "pc_daily_mut_glass_note", 1.5f, 1.05f, 1.00f, 1.00f, 0.60f, 1.50f, 1.25f, 1.25f, 1.05f, 1.00f, 1.00f,
         false, false, false, false, false, 0},
        {3, "pc_daily_mut_iron", "pc_daily_mut_iron_note", 1.4f, 1.05f, 1.15f, 1.05f, 1.00f, 1.35f, 1.15f, 1.00f, 1.00f, 0.85f, 1.00f,
         false, false, false, true, false, 3},
        {4, "pc_daily_mut_lisa", "pc_daily_mut_lisa_note", 1.6f, 1.08f, 1.00f, 1.00f, 1.00f, 1.10f, 1.10f, 1.08f, 1.00f, 1.15f, 1.15f,
         false, false, false, false, true, 7},
        {5, "pc_daily_mut_fog", "pc_daily_mut_fog_note", 1.4f, 1.12f, 0.75f, 1.15f, 1.00f, 1.00f, 1.15f, 1.00f, 1.00f, 1.10f, 1.20f,
         false, false, true, false, false, 1},
        {6, "pc_daily_mut_trigger", "pc_daily_mut_trigger_note", 1.4f, 1.08f, 1.00f, 1.30f, 1.00f, 1.00f, 1.00f, 1.15f, 1.35f, 1.40f, 1.00f,
         false, false, false, false, false, 1},
        {7, "pc_daily_mut_echo", "pc_daily_mut_echo_note", 2.0f, 1.22f, 1.12f, 1.20f, 0.85f, 1.25f, 1.35f, 1.15f, 1.10f, 1.15f, 1.20f,
         true, false, true, false, true, 12},
    }};
    return kMutators;
}

const DailyMutatorDef& DailyMutatorForDate(int yyyymmdd) {
    uint32_t h = static_cast<uint32_t>(std::max(0, yyyymmdd)) * 2654435761u;
    h ^= h >> 16;
    return DailyMutators()[h % static_cast<uint32_t>(kSurvivalDailyMutatorCount)];
}

const DailyMutatorDef& DailyMutatorById(int id) {
    return DailyMutators()[static_cast<size_t>(std::clamp(id, 0, kSurvivalDailyMutatorCount - 1))];
}

const char* SkinRarityKey(SkinRarity rarity) {
    switch (rarity) {
        case SkinRarity::Factory: return "pc_skin_rarity_factory";
        case SkinRarity::Tactical: return "pc_skin_rarity_tactical";
        case SkinRarity::Artisan: return "pc_skin_rarity_artisan";
        case SkinRarity::Horror: return "pc_skin_rarity_horror";
        case SkinRarity::Mythic: return "pc_skin_rarity_mythic";
    }
    return "pc_skin_rarity_factory";
}

const char* ReloadStyleKey(ReloadStyle style) {
    switch (style) {
        case ReloadStyle::Standard: return "pc_reload_standard";
        case ReloadStyle::Tactical: return "pc_reload_tactical";
        case ReloadStyle::Gunslinger: return "pc_reload_gunslinger";
        case ReloadStyle::Telekinetic: return "pc_reload_telekinetic";
    }
    return "pc_reload_standard";
}

SurvivalPayout ComputeSurvivalCredits(const SurvivalPayoutInput& in) {
    SurvivalPayout out;
    const int waves = std::max(0, in.waves_cleared);
    for (int w = 1; w <= waves; ++w) {
        out.wave_credits += 35 + 12 * w;
    }
    out.combat_credits = std::max(0, in.kills) * 4
                       + std::max(0, in.headshots) * 8
                       + std::max(0, in.best_chain) * 15
                       + std::max(0, in.missions_done) * 120
                       + std::max(0, in.bosses_down) * 350;
    if (in.daily) {
        out.multiplier = DailyMutatorById(in.daily_mutator).credits_mult;
    } else {
        static constexpr float kDiffMult[] = {0.8f, 1.0f, 1.4f, 2.0f, 3.0f};
        out.multiplier = kDiffMult[static_cast<size_t>(std::clamp(in.difficulty, 0, 4))];
    }
    const int scaled = static_cast<int>(std::lround(static_cast<float>(out.wave_credits + out.combat_credits) * out.multiplier));
    if (in.daily && in.daily_goal_fresh) {
        out.daily_bonus = std::min(2000, 1000 + std::max(0, in.daily_streak - 1) * 200);
    }
    out.total = std::max(0, scaled + out.daily_bonus);
    return out;
}

SkinTextureSet EnsureSkinTextures(TextureManager& textures, int skin_id) {
    const SurvivalSkinDef& skin = SurvivalSkin(skin_id);
    SkinTextureSet set;
    if (skin.id == 0) return set;

    const std::string prefix = std::format("survival_skin_{}_", skin.id);
    const std::string n_ma = prefix + "ma";
    const std::string n_ms = prefix + "ms";
    const std::string n_ga = prefix + "ga";
    const std::string n_gs = prefix + "gs";
    const std::string n_aa = prefix + "aa";
    const std::string n_as = prefix + "as";

    if (const uint32_t existing = textures.Find(n_ma); existing != TextureManager::kWhite) {
        set.metal_albedo = existing;
        set.metal_srm = textures.Find(n_ms);
        set.grip_albedo = textures.Find(n_ga);
        set.grip_srm = textures.Find(n_gs);
        set.accent_albedo = textures.Find(n_aa);
        set.accent_srm = textures.Find(n_as);
        return set;
    }

    const size_t pixels = static_cast<size_t>(kSkinTexSize) * static_cast<size_t>(kSkinTexSize);
    std::vector<uint8_t> ma(pixels * 4u), ms(pixels * 4u);
    std::vector<uint8_t> ga(pixels * 4u), gs(pixels * 4u);

    for (uint32_t y = 0; y < kSkinTexSize; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSkinTexSize);
        for (uint32_t x = 0; x < kSkinTexSize; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSkinTexSize);
            const size_t idx = (static_cast<size_t>(y) * kSkinTexSize + x) * 4u;

            const SampleOut m = EvalMetalPattern(skin, u, v);
            ma[idx + 0] = ToU8(m.rgb.r);
            ma[idx + 1] = ToU8(m.rgb.g);
            ma[idx + 2] = ToU8(m.rgb.b);
            ma[idx + 3] = 255;
            ms[idx + 0] = ToU8(m.srm.x);
            ms[idx + 1] = ToU8(m.srm.y);
            ms[idx + 2] = ToU8(m.srm.z);
            ms[idx + 3] = 255;

            const SampleOut g = EvalGripPattern(skin, u, v);
            ga[idx + 0] = ToU8(g.rgb.r);
            ga[idx + 1] = ToU8(g.rgb.g);
            ga[idx + 2] = ToU8(g.rgb.b);
            ga[idx + 3] = 255;
            gs[idx + 0] = ToU8(g.srm.x);
            gs[idx + 1] = ToU8(g.srm.y);
            gs[idx + 2] = ToU8(g.srm.z);
            gs[idx + 3] = 255;
        }
    }

    auto upload = [&](const std::string& name, gpu::Format fmt, uint32_t size, const std::vector<uint8_t>& buf) {
        const TextureMip mip{size, size, buf};
        const std::array<TextureMip, 1> mips{mip};
        return textures.Create(name, fmt, mips);
    };

    std::vector<uint8_t> aa(16u * 4u), as(16u * 4u);
    for (size_t i = 0; i < 16u; ++i) {
        const float micro = (static_cast<float>((i * 7u) % 5u) - 2.0f) * 0.015f;
        aa[i * 4 + 0] = ToU8(skin.accent.r + micro);
        aa[i * 4 + 1] = ToU8(skin.accent.g + micro);
        aa[i * 4 + 2] = ToU8(skin.accent.b + micro);
        aa[i * 4 + 3] = 255;
        as[i * 4 + 0] = ToU8(skin.accent_srm.x);
        as[i * 4 + 1] = ToU8(skin.accent_srm.y);
        as[i * 4 + 2] = ToU8(skin.accent_srm.z);
        as[i * 4 + 3] = 255;
    }

    set.metal_albedo = upload(n_ma, gpu::Format::R8G8B8A8Srgb, kSkinTexSize, ma);
    set.metal_srm = upload(n_ms, gpu::Format::R8G8B8A8Unorm, kSkinTexSize, ms);
    set.grip_albedo = upload(n_ga, gpu::Format::R8G8B8A8Srgb, kSkinTexSize, ga);
    set.grip_srm = upload(n_gs, gpu::Format::R8G8B8A8Unorm, kSkinTexSize, gs);
    set.accent_albedo = upload(n_aa, gpu::Format::R8G8B8A8Srgb, 4u, aa);
    set.accent_srm = upload(n_as, gpu::Format::R8G8B8A8Unorm, 4u, as);
    return set;
}

glm::vec3 EvalMythicMetal(const SurvivalSkinDef& skin, float u, float v, int f, int x, int y) {
    const uint32_t seed = static_cast<uint32_t>(skin.id) * 193u + 37u;
    const glm::vec3 A = skin.metal_a;
    const glm::vec3 B = skin.metal_b;
    const glm::vec3 C = skin.accent;
    const float phase = static_cast<float>(f) / static_cast<float>(kMythicAnimFrames);
    const float ang = phase * 2.0f * kPi;
    const float breath = 0.5f + 0.5f * std::sin(ang);
    glm::vec3 col = A;

    switch (skin.id) {
        case 54: {
            const float vs = std::fmod(v + phase, 1.0f);
            const int row_i = static_cast<int>(v * 32.0f);
            const float band = HashF(f * 3 + 1, row_i, seed);
            const float shift = band > 0.72f ? (band - 0.72f) * 1.8f : 0.0f;
            const float us = std::fmod(u + shift, 1.0f);
            const float sweep = std::exp(-std::pow(std::sin((vs - 0.5f) * kPi) * 3.8f, 2.0f));
            const float scan = 0.65f + 0.35f * std::sin((v * 48.0f + phase * 4.0f) * 2.0f * kPi);
            const float blk = TileNoise(std::floor(us * 12.0f) / 12.0f, std::floor(v * 24.0f) / 24.0f, 12, seed + static_cast<uint32_t>(f) * 19u);
            const float hot = std::clamp(Smooth((blk - 0.62f) / 0.16f) * (band > 0.55f ? 1.0f : 0.45f) + sweep * 0.75f, 0.0f, 1.0f);
            const float grain = HashF(static_cast<int>(x) + f * 17, static_cast<int>(y) + f * 31, seed + 5u);
            col = Lerp3(B, A, scan * (0.45f + 0.55f * grain));
            col = Lerp3(col, C, hot);
            col = Lerp3(col, glm::vec3(1.0f, 0.94f, 0.92f), Smooth((hot - 0.80f) / 0.18f) * 0.75f);
            break;
        }
        case 55:
        case 56:
        case 58:
        case 60: {
            const float edge = TileVoronoiEdge(u, v, 7, seed);
            const float flow = TileFbm(std::fmod(u + phase, 1.0f), std::fmod(v + phase * (skin.id == 58 ? 1.0f : 0.0f), 1.0f), 4, 3, seed + 17u);
            const float wave = 0.5f + 0.5f * std::sin((u * 2.0f + v * 1.0f - phase * 2.0f) * 2.0f * kPi);
            const float surge = 0.45f + 0.55f * (skin.id == 56 ? std::pow(wave, 2.2f) : (0.5f * breath + 0.5f * wave));
            const float crack = 1.0f - Smooth((edge + (flow - 0.5f) * 0.06f) / (0.09f + 0.06f * surge));
            const float core = 1.0f - Smooth(edge / (0.030f + 0.035f * surge));
            col = Lerp3(B, A, flow * 0.65f);
            col = Lerp3(col, C, std::clamp(crack * (0.70f + 0.45f * surge), 0.0f, 1.0f));
            const glm::vec3 hot_core = skin.id == 60 ? glm::vec3(0.88f, 1.00f, 0.68f)
                                      : skin.id == 56 ? glm::vec3(1.00f, 0.72f, 0.78f)
                                                      : glm::vec3(1.00f, 0.95f, 0.76f);
            col = Lerp3(col, hot_core, core * (0.45f + 0.50f * surge));
            break;
        }
        case 57: {
            const float n = TileFbm(std::fmod(u + phase, 1.0f), std::fmod(v - phase, 1.0f), 4, 3, seed);
            const float band = 0.5f + 0.5f * std::sin((u * 2.0f + v * 2.0f - phase * 2.0f + n * 1.4f) * 2.0f * kPi);
            const float arc = 1.0f - Smooth(std::abs(std::sin((u * 3.0f - v * 2.0f + phase * 2.0f + n * 2.4f) * kPi)) / 0.14f);
            col = Lerp3(B, A, band);
            col = Lerp3(col, C, Smooth((band - 0.45f) / 0.45f) * 0.75f);
            col = Lerp3(col, glm::vec3(0.86f, 0.98f, 1.00f), arc * 0.85f);
            break;
        }
        case 59: {
            const float n = TileFbm(u, v, 3, 2, seed) * 0.25f;
            const float h = std::fmod(u * 1.0f + v * 0.6f - phase + n + 4.0f, 1.0f) * 6.0f;
            const float r = std::clamp(std::abs(h - 3.0f) - 1.0f, 0.0f, 1.0f);
            const float g = std::clamp(2.0f - std::abs(h - 2.0f), 0.0f, 1.0f);
            const float b = std::clamp(2.0f - std::abs(h - 4.0f), 0.0f, 1.0f);
            const float sheen = 0.5f + 0.5f * std::sin((u * 3.0f - v * 2.0f + phase * 2.0f) * 2.0f * kPi);
            col = glm::vec3(0.18f) + 0.82f * glm::vec3(r, g, b);
            col = Lerp3(col, glm::vec3(1.0f, 0.96f, 0.90f), Smooth((sheen - 0.78f) / 0.22f) * 0.45f);
            break;
        }
        case 61:
        case 63: {
            const float a1 = std::sin(u * 8.0f * kPi) * std::cos(v * 8.0f * kPi);
            const float a2 = std::sin((u + v) * 6.0f * kPi + a1 * 1.4f);
            const float eng = Smooth((std::abs(a2) - 0.68f) / 0.16f);
            const float wave = 0.5f + 0.5f * std::sin((u * 2.0f + v - phase * 2.0f) * 2.0f * kPi);
            const float sparkle = Smooth((HashF(static_cast<int>(u * 48.0f), static_cast<int>(v * 48.0f), seed) - 0.90f) / 0.09f) *
                                  (0.35f + 0.65f * std::sin(ang + u * 12.0f + v * 9.0f));
            col = Lerp3(B, A, 0.45f + 0.55f * wave);
            col = Lerp3(col, C, std::clamp(eng * (0.65f + 0.45f * wave), 0.0f, 1.0f));
            col = Lerp3(col, glm::vec3(1.0f, 0.98f, 0.88f), std::clamp(sparkle + eng * std::pow(wave, 3.0f) * 0.65f, 0.0f, 1.0f));
            break;
        }
        case 62: {
            const float n1 = TileFbm(std::fmod(u + phase, 1.0f), v, 4, 3, seed);
            const float rings = 0.5f + 0.5f * std::sin((u * 4.0f + v * 2.0f - phase * 2.0f + n1 * 4.2f) * 2.0f * kPi);
            const float crest = Smooth((rings - 0.72f) / 0.22f);
            col = Lerp3(B, A, Smooth((rings - 0.25f) / 0.45f));
            col = Lerp3(col, C, crest * (0.55f + 0.45f * breath));
            break;
        }
        default:
            col = Lerp3(B, A, breath);
            break;
    }

    return col;
}

glm::vec3 MythicLiveColor(int skin_id, float time_sec) {
    const SurvivalSkinDef& sk = SurvivalSkin(skin_id);
    if (sk.rarity != SkinRarity::Mythic) return sk.aura_color;
    switch (sk.id) {
        case 54: {
            const float f = 0.5f + 0.5f * std::sin(time_sec * 17.0f) * std::cos(time_sec * 11.3f);
            return Lerp3({1.00f, 0.10f, 0.08f}, {1.00f, 0.78f, 0.74f}, Smooth((f - 0.76f) / 0.22f));
        }
        case 55: {
            const float p = 0.5f + 0.5f * std::sin(time_sec * 2.8f);
            return Lerp3({0.92f, 0.42f, 0.08f}, {1.00f, 0.88f, 0.36f}, p);
        }
        case 56: {
            const float cyc = std::fmod(std::max(0.0f, time_sec * 1.15f), 1.0f);
            const float b1 = std::exp(-std::pow((cyc - 0.14f) / 0.055f, 2.0f));
            const float b2 = 0.75f * std::exp(-std::pow((cyc - 0.29f) / 0.065f, 2.0f));
            return Lerp3({0.58f, 0.04f, 0.10f}, {1.00f, 0.22f, 0.28f}, std::clamp(b1 + b2, 0.0f, 1.0f));
        }
        case 57: {
            const float p = 0.5f + 0.5f * std::sin(time_sec * 3.2f);
            return Lerp3({0.14f, 0.88f, 1.00f}, {0.72f, 0.24f, 1.00f}, p);
        }
        case 58: {
            const float p = 0.5f + 0.5f * std::sin(time_sec * 3.6f + 0.4f * std::sin(time_sec * 7.1f));
            return Lerp3({0.95f, 0.24f, 0.04f}, {1.00f, 0.76f, 0.24f}, p);
        }
        case 59: {
            const float h = std::fmod(std::max(0.0f, time_sec * 0.42f), 1.0f) * 6.0f;
            const float r = std::clamp(std::abs(h - 3.0f) - 1.0f, 0.0f, 1.0f);
            const float g = std::clamp(2.0f - std::abs(h - 2.0f), 0.0f, 1.0f);
            const float b = std::clamp(2.0f - std::abs(h - 4.0f), 0.0f, 1.0f);
            return glm::vec3(0.22f) + 0.78f * glm::vec3(r, g, b);
        }
        case 60: {
            const float p = 0.5f + 0.5f * std::sin(time_sec * 4.4f);
            return Lerp3({0.16f, 0.85f, 0.14f}, {0.72f, 1.00f, 0.34f}, p);
        }
        case 61: {
            const float p = 0.5f + 0.5f * std::sin(time_sec * 2.5f);
            return Lerp3({0.46f, 0.62f, 1.00f}, {1.00f, 0.86f, 0.36f}, p);
        }
        case 62: {
            const float p = 0.5f + 0.5f * std::sin(time_sec * 2.4f);
            return Lerp3({0.95f, 0.12f, 0.16f}, {1.00f, 0.84f, 0.88f}, p * 0.72f);
        }
        case 63: {
            const float p = 0.5f + 0.5f * std::sin(time_sec * 2.2f);
            return Lerp3({1.00f, 0.74f, 0.24f}, {1.00f, 0.96f, 0.82f}, p);
        }
        default:
            return sk.aura_color;
    }
}

MythicAnimSet EnsureMythicAnimTextures(TextureManager& textures, int skin_id) {
    const SurvivalSkinDef& skin = SurvivalSkin(skin_id);
    MythicAnimSet out;
    if (skin.rarity != SkinRarity::Mythic) return out;

    const SkinTextureSet base = EnsureSkinTextures(textures, skin.id);
    out.metal_srm = base.metal_srm;
    out.grip_albedo = base.grip_albedo;
    out.grip_srm = base.grip_srm;
    out.accent_srm = base.accent_srm;

    const std::string prefix = std::format("survival_mythic_{}_", skin.id);
    if (const uint32_t existing = textures.Find(prefix + "m0"); existing != TextureManager::kWhite) {
        for (int f = 0; f < kMythicAnimFrames; ++f) {
            out.metal_albedo[static_cast<size_t>(f)] = textures.Find(std::format("{}m{}", prefix, f));
            out.accent_albedo[static_cast<size_t>(f)] = textures.Find(std::format("{}a{}", prefix, f));
        }
        return out;
    }

    const size_t pixels = static_cast<size_t>(kMythicTexSize) * static_cast<size_t>(kMythicTexSize);
    std::vector<uint8_t> buf(pixels * 4u);
    std::vector<uint8_t> abuf(16u * 4u);

    auto upload = [&](const std::string& name, uint32_t size, const std::vector<uint8_t>& data) {
        const TextureMip mip{size, size, data};
        const std::array<TextureMip, 1> mips{mip};
        return textures.Create(name, gpu::Format::R8G8B8A8Srgb, mips);
    };

    for (int f = 0; f < kMythicAnimFrames; ++f) {
        const float phase = static_cast<float>(f) / static_cast<float>(kMythicAnimFrames);

        for (uint32_t y = 0; y < kMythicTexSize; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kMythicTexSize);
            for (uint32_t x = 0; x < kMythicTexSize; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kMythicTexSize);
                const size_t idx = (static_cast<size_t>(y) * kMythicTexSize + x) * 4u;
                const glm::vec3 col = EvalMythicMetal(skin, u, v, f, static_cast<int>(x), static_cast<int>(y));

                buf[idx + 0] = ToU8(col.r);
                buf[idx + 1] = ToU8(col.g);
                buf[idx + 2] = ToU8(col.b);
                buf[idx + 3] = 255;
            }
        }

        const glm::vec3 live_acc = MythicLiveColor(skin.id, phase * 2.4f);
        for (size_t i = 0; i < 16u; ++i) {
            abuf[i * 4 + 0] = ToU8(live_acc.r);
            abuf[i * 4 + 1] = ToU8(live_acc.g);
            abuf[i * 4 + 2] = ToU8(live_acc.b);
            abuf[i * 4 + 3] = 255;
        }
        out.metal_albedo[static_cast<size_t>(f)] = upload(std::format("{}m{}", prefix, f), kMythicTexSize, buf);
        out.accent_albedo[static_cast<size_t>(f)] = upload(std::format("{}a{}", prefix, f), 4u, abuf);
    }
    return out;
}

TorchTextureSet EnsureTorchTextures(TextureManager& textures, int torch_id) {
    TorchTextureSet out;
    if (torch_id <= 0 || torch_id >= kSurvivalTorchCount) return out;
    const std::string n_alb = std::format("survival_torch_{}_alb", torch_id);
    const std::string n_srm = std::format("survival_torch_{}_srm", torch_id);
    if (const uint32_t existing = textures.Find(n_alb); existing != TextureManager::kWhite) {
        out.body_albedo = existing;
        out.body_srm = textures.Find(n_srm);
        return out;
    }

    glm::vec3 col_a{0.48f, 0.30f, 0.11f}, col_b{0.26f, 0.15f, 0.05f}, srm{0.72f, 0.36f, 0.20f};
    switch (torch_id) {
        case 1:
            col_a = {0.50f, 0.32f, 0.12f};
            col_b = {0.28f, 0.17f, 0.06f};
            srm = {0.72f, 0.36f, 0.22f};
            break;
        case 2:
            col_a = {0.46f, 0.08f, 0.08f};
            col_b = {0.24f, 0.04f, 0.04f};
            srm = {0.70f, 0.35f, 0.20f};
            break;
        case 3:
            col_a = {0.11f, 0.30f, 0.60f};
            col_b = {0.05f, 0.16f, 0.36f};
            srm = {0.74f, 0.34f, 0.22f};
            break;
        case 4:
            col_a = {0.10f, 0.42f, 0.22f};
            col_b = {0.05f, 0.22f, 0.11f};
            srm = {0.72f, 0.36f, 0.20f};
            break;
        default:
            break;
    }

    const uint32_t sz = 128;
    const size_t pixels = static_cast<size_t>(sz) * static_cast<size_t>(sz);
    std::vector<uint8_t> alb(pixels * 4u), spec(pixels * 4u);
    const uint32_t seed = static_cast<uint32_t>(torch_id) * 97u + 13u;
    for (uint32_t y = 0; y < sz; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(sz);
        for (uint32_t x = 0; x < sz; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(sz);
            const size_t idx = (static_cast<size_t>(y) * sz + x) * 4u;
            const float grain = TileNoise(u * 0.5f, v * 4.0f, 16, seed) * 0.65f + TileFbm(u, v, 6, 3, seed + 29u) * 0.35f;
            const glm::vec3 c = Lerp3(col_b, col_a, 0.35f + 0.65f * grain);
            alb[idx + 0] = ToU8(c.r);
            alb[idx + 1] = ToU8(c.g);
            alb[idx + 2] = ToU8(c.b);
            alb[idx + 3] = 255;
            spec[idx + 0] = ToU8(srm.x);
            spec[idx + 1] = ToU8(std::clamp(srm.y + (0.5f - grain) * 0.10f, 0.14f, 0.72f));
            spec[idx + 2] = ToU8(srm.z);
            spec[idx + 3] = 255;
        }
    }
    const TextureMip m_alb{sz, sz, alb};
    const TextureMip m_srm{sz, sz, spec};
    const std::array<TextureMip, 1> a_mips{m_alb};
    const std::array<TextureMip, 1> s_mips{m_srm};
    out.body_albedo = textures.Create(n_alb, gpu::Format::R8G8B8A8Srgb, a_mips);
    out.body_srm = textures.Create(n_srm, gpu::Format::R8G8B8A8Unorm, s_mips);
    return out;
}

namespace {

constexpr uint32_t kBakeSize = 1024;
constexpr uint32_t kBakeFrameSize = 512;
constexpr float kDetailShare = 0.85f;
constexpr int kBakePadding = 6;
constexpr uint64_t kBakeBudget = 384ull << 20;

float Fract(float x) {
    return x - std::floor(x);
}

float SkinTile(const SurvivalSkinDef& skin, uint8_t channel) {
    if (channel == 1) return skin.rarity == SkinRarity::Tactical || skin.rarity == SkinRarity::Mythic ? 0.06f : 0.25f;
    switch (skin.pattern) {
        case SkinPattern::Filigree: return 0.11f;
        case SkinPattern::CarbonWeave: return 0.12f;
        case SkinPattern::HexGrid: return 0.20f;
        case SkinPattern::Damascus:
        case SkinPattern::AsylumTile: return 0.15f;
        case SkinPattern::BrushedMetal:
        case SkinPattern::FactoryOriginal: return 0.20f;
        default: return 0.30f;
    }
}

bool OriginalLuminance(const std::string& base_path, uint32_t size, std::vector<float>& out) {
    if (base_path.empty()) return false;
    const auto bytes = mods::ReadOverride(base_path + ".png");
    if (!bytes) return false;
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    out.assign(static_cast<size_t>(size) * size, 0.0f);
    const int bx = std::max(1, w / static_cast<int>(size)), by = std::max(1, h / static_cast<int>(size));
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const int sx = static_cast<int>(static_cast<uint64_t>(x) * static_cast<uint64_t>(w) / size);
            const int sy = static_cast<int>(static_cast<uint64_t>(y) * static_cast<uint64_t>(h) / size);
            float sum = 0.0f;
            int n = 0;
            for (int j = 0; j < by && sy + j < h; ++j) {
                for (int i = 0; i < bx && sx + i < w; ++i) {
                    const stbi_uc* p = px + (static_cast<size_t>(sy + j) * static_cast<size_t>(w) + static_cast<size_t>(sx + i)) * 4;
                    sum += (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
                    ++n;
                }
            }
            out[static_cast<size_t>(y) * size + x] = n > 0 ? sum / static_cast<float>(n) : 0.0f;
        }
    }
    stbi_image_free(px);
    return true;
}

const SkinBakeSource::Raster& Rasterize(SkinBakeSource& source, uint32_t size) {
    for (const auto& r : source.rasters) {
        if (r.size == size) return r;
    }
    SkinBakeSource::Raster r;
    r.size = size;
    const size_t texels = static_cast<size_t>(size) * size;
    r.tri.assign(texels, -1);
    r.weights.assign(texels, glm::vec3(0.0f));
    const float fs = static_cast<float>(size);
    const size_t tris = source.uv.size() / 3;
    for (size_t t = 0; t < tris; ++t) {
        const glm::vec2 a = source.uv[t * 3] * fs, b = source.uv[t * 3 + 1] * fs, c = source.uv[t * 3 + 2] * fs;
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (std::abs(area) < 1.0e-8f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(static_cast<int>(size) - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(static_cast<int>(size) - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 p(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                const float w0 = ((b.x - p.x) * (c.y - p.y) - (c.x - p.x) * (b.y - p.y)) / area;
                const float w1 = ((c.x - p.x) * (a.y - p.y) - (a.x - p.x) * (c.y - p.y)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -1.0e-4f || w1 < -1.0e-4f || w2 < -1.0e-4f) continue;
                const size_t i = static_cast<size_t>(y) * size + static_cast<size_t>(x);
                r.tri[i] = static_cast<int32_t>(t);
                r.weights[i] = glm::vec3(w0, w1, w2);
            }
        }
    }
    for (int pass = 0; pass < kBakePadding; ++pass) {
        std::vector<int32_t> tri = r.tri;
        std::vector<glm::vec3> wts = r.weights;
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const size_t i = static_cast<size_t>(y) * size + x;
                if (r.tri[i] >= 0) continue;
                for (const glm::ivec2 d : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)}) {
                    const int nx = static_cast<int>(x) + d.x, ny = static_cast<int>(y) + d.y;
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(size) || ny >= static_cast<int>(size)) continue;
                    const size_t j = static_cast<size_t>(ny) * size + static_cast<size_t>(nx);
                    if (r.tri[j] < 0) continue;
                    tri[i] = r.tri[j];
                    wts[i] = r.weights[j];
                    break;
                }
            }
        }
        r.tri.swap(tri);
        r.weights.swap(wts);
    }
    std::vector<float> luma;
    r.detail.assign(texels, 1.0f);
    if (OriginalLuminance(source.base_path, size, luma)) {
        double sum = 0.0;
        size_t n = 0;
        for (size_t i = 0; i < texels; ++i) {
            if (r.tri[i] >= 0) {
                sum += luma[i];
                ++n;
            }
        }
        const float mean = n > 0 ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
        if (mean > 0.01f) {
            for (size_t i = 0; i < texels; ++i) r.detail[i] = std::clamp(luma[i] / mean, 0.2f, 2.0f);
        }
    }
    source.rasters.push_back(std::move(r));
    return source.rasters.back();
}

struct PatternOut {
    glm::vec3 rgb{0.0f};
    glm::vec3 srm{0.0f};
};

PatternOut PatternAt(const SurvivalSkinDef& skin, uint8_t channel, int frame, float u, float v, uint32_t size) {
    if (channel == 1) {
        const SampleOut g = EvalGripPattern(skin, u, v);
        return {g.rgb, g.srm};
    }
    if (channel == 2) {
        const glm::vec3 acc = frame >= 0 ? MythicLiveColor(skin.id, static_cast<float>(frame) / static_cast<float>(kMythicAnimFrames) * 2.4f) : skin.accent;
        return {acc, skin.accent_srm};
    }
    if (frame >= 0) {
        return {EvalMythicMetal(skin, u, v, frame, static_cast<int>(u * static_cast<float>(size)), static_cast<int>(v * static_cast<float>(size))), skin.metal_srm};
    }
    const SampleOut m = EvalMetalPattern(skin, u, v);
    return {m.rgb, m.srm};
}

void BakePixels(const SurvivalSkinDef& skin, const SkinBakeSource& source, const SkinBakeSource::Raster& r, int frame, std::vector<uint8_t>& albedo,
                std::vector<uint8_t>* srm) {
    const uint32_t size = r.size;
    albedo.assign(static_cast<size_t>(size) * size * 4, 0);
    if (srm) srm->assign(static_cast<size_t>(size) * size * 4, 0);
    const bool panels = skin.pattern == SkinPattern::Filigree && source.channel == 0 && frame < 0;
    int lateral = 0;
    if (panels && source.position.size() >= 3) {
        glm::vec3 facing(0.0f);
        for (size_t k = 0; k + 2 < source.position.size(); k += 3) {
            facing += glm::abs(glm::cross(source.position[k + 1] - source.position[k], source.position[k + 2] - source.position[k]));
        }
        lateral = facing.x >= facing.y && facing.x >= facing.z ? 0 : facing.y >= facing.z ? 1 : 2;
    }
    ParallelChunks(size, 32, [&](size_t, size_t begin, size_t end) {
        for (size_t y = begin; y < end; ++y) {
            for (size_t x = 0; x < size; ++x) {
                const size_t i = y * size + x;
                const int32_t t = r.tri[i];
                PatternOut out;
                if (t < 0) {
                    out = PatternAt(skin, source.channel, frame, 0.5f, 0.5f, size);
                } else {
                    const glm::vec3 w = r.weights[i];
                    const size_t k = static_cast<size_t>(t) * 3;
                    const glm::vec3 p = source.position[k] * w.x + source.position[k + 1] * w.y + source.position[k + 2] * w.z;
                    glm::vec3 n = source.normal[k] * w.x + source.normal[k + 1] * w.y + source.normal[k + 2] * w.z;
                    n = glm::length(n) > 1.0e-6f ? glm::normalize(n) : glm::vec3(0.0f, 1.0f, 0.0f);
                    glm::vec3 side = glm::abs(n);
                    side = side * side * side * side;
                    side /= std::max(side.x + side.y + side.z, 1.0e-6f);
                    const glm::vec3 q = p / SkinTile(skin, source.channel);
                    const glm::vec2 planes[3] = {{q.z, q.y}, {q.x, q.z}, {q.x, q.y}};
                    float total = 0.0f;
                    for (int a = 0; a < 3; ++a) {
                        if (side[a] < 0.02f) continue;
                        const PatternOut o = PatternAt(skin, source.channel, frame, Fract(planes[a].x), Fract(planes[a].y), size);
                        out.rgb += o.rgb * side[a];
                        out.srm += o.srm * side[a];
                        total += side[a];
                    }
                    if (total > 0.0f) {
                        out.rgb /= total;
                        out.srm /= total;
                    }
                    if (panels) {
                        const float on_side = Smooth((std::abs(n[lateral]) - 0.62f) / 0.25f);
                        out.rgb = Lerp3(skin.metal_a, out.rgb, on_side);
                        out.srm = Lerp3(skin.metal_srm, out.srm, on_side);
                    }
                    const float d = glm::mix(1.0f, std::clamp(r.detail[i], 0.3f, 1.06f), kDetailShare);
                    out.rgb *= d;
                    out.srm.y = std::clamp(out.srm.y + (1.0f - r.detail[i]) * 0.12f, 0.05f, 0.95f);
                }
                uint8_t* a = &albedo[i * 4];
                a[0] = ToU8(out.rgb.r);
                a[1] = ToU8(out.rgb.g);
                a[2] = ToU8(out.rgb.b);
                a[3] = 255;
                if (srm) {
                    uint8_t* s2 = &(*srm)[i * 4];
                    s2[0] = ToU8(out.srm.x);
                    s2[1] = ToU8(out.srm.y);
                    s2[2] = ToU8(out.srm.z);
                    s2[3] = 255;
                }
            }
        }
    });
}

uint32_t UploadMipped(TextureManager& textures, const std::string& name, bool srgb, uint32_t size, std::vector<uint8_t> top) {
    static const std::array<float, 256> to_linear = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    const auto encode = [](float l) {
        l = std::clamp(l, 0.0f, 1.0f);
        const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8_t>(c * 255.0f + 0.5f);
    };
    std::vector<std::vector<uint8_t>> levels;
    levels.push_back(std::move(top));
    uint32_t s = size;
    while (s > 1) {
        const uint32_t h = s / 2;
        const std::vector<uint8_t>& up = levels.back();
        std::vector<uint8_t> down(static_cast<size_t>(h) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < h; ++x) {
                for (size_t c = 0; c < 4; ++c) {
                    const auto at = [&](uint32_t xx, uint32_t yy) { return up[(static_cast<size_t>(yy) * s + xx) * 4 + c]; };
                    const uint8_t q[4] = {at(2 * x, 2 * y), at(2 * x + 1, 2 * y), at(2 * x, 2 * y + 1), at(2 * x + 1, 2 * y + 1)};
                    uint8_t v;
                    if (srgb && c < 3) {
                        v = encode((to_linear[q[0]] + to_linear[q[1]] + to_linear[q[2]] + to_linear[q[3]]) * 0.25f);
                    } else {
                        v = static_cast<uint8_t>((static_cast<int>(q[0]) + q[1] + q[2] + q[3] + 2) / 4);
                    }
                    down[(static_cast<size_t>(y) * h + x) * 4 + c] = v;
                }
            }
        }
        levels.push_back(std::move(down));
        s = h;
    }
    std::vector<TextureMip> mips;
    s = size;
    for (const auto& l : levels) {
        mips.push_back(TextureMip{s, s, l});
        s = std::max(1u, s / 2);
    }
    TextureManager::ModelScope scope(textures);
    const uint32_t index = textures.Create(name, srgb ? gpu::Format::R8G8B8A8Srgb : gpu::Format::R8G8B8A8Unorm, mips);
    textures.AddModelRef(index);
    return index;
}

struct BakeEntry {
    BakedSkinMaterial set;
    uint64_t bytes = 0;
    uint64_t last_use = 0;
    int shown = 0;
};
std::unordered_map<std::string, BakeEntry> g_bakes;
std::unordered_map<uint32_t, std::string> g_shown;
struct Retired {
    std::chrono::steady_clock::time_point at;
    std::vector<uint32_t> textures;
};
std::vector<Retired> g_retired;
uint64_t g_bake_clock = 0;
uint64_t g_bake_bytes = 0;

std::string BakeKey(const SkinBakeSource& source, int skin_id) {
    return std::format("survival_bake_{}_{}", skin_id, source.key);
}

void PumpRetired(TextureManager& textures) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_retired.begin(); it != g_retired.end();) {
        if (now - it->at < std::chrono::milliseconds(1500)) {
            ++it;
            continue;
        }
        for (const uint32_t t : it->textures) textures.ReleaseModelRef(t);
        it = g_retired.erase(it);
    }
}

void Evict(const std::string& keep) {
    while (g_bake_bytes > kBakeBudget) {
        auto oldest = g_bakes.end();
        for (auto it = g_bakes.begin(); it != g_bakes.end(); ++it) {
            if (it->second.shown > 0 || it->first == keep) continue;
            if (oldest == g_bakes.end() || it->second.last_use < oldest->second.last_use) oldest = it;
        }
        if (oldest == g_bakes.end()) return;
        Retired r{std::chrono::steady_clock::now(), {}};
        const BakedSkinMaterial& s = oldest->second.set;
        if (s.albedo) r.textures.push_back(s.albedo);
        if (s.srm) r.textures.push_back(s.srm);
        for (const uint32_t f : s.frames) r.textures.push_back(f);
        g_bake_bytes -= oldest->second.bytes;
        LogInfo("survival: skin bake {} let go ({} MB baked)", oldest->first, g_bake_bytes >> 20);
        g_retired.push_back(std::move(r));
        g_bakes.erase(oldest);
    }
}

}

BakedSkinMaterial BakeSkinMaterial(TextureManager& textures, SkinBakeSource& source, int skin_id, bool animated) {
    PumpRetired(textures);
    const SurvivalSkinDef& skin = SurvivalSkin(skin_id);
    if (skin.id == 0 || source.uv.empty()) return {};
    const bool mythic = animated && skin.rarity == SkinRarity::Mythic;
    const std::string key = BakeKey(source, skin.id);
    auto it = g_bakes.find(key);
    if (it != g_bakes.end() && (!mythic || source.channel != 0 || !it->second.set.frames.empty())) {
        it->second.last_use = ++g_bake_clock;
        return it->second.set;
    }
    const auto started = std::chrono::steady_clock::now();
    BakeEntry entry = it != g_bakes.end() ? it->second : BakeEntry{};
    const uint64_t old_bytes = it != g_bakes.end() ? it->second.bytes : 0;
    if (!entry.set.albedo) {
        const SkinBakeSource::Raster& r = Rasterize(source, kBakeSize);
        std::vector<uint8_t> albedo, srm;
        BakePixels(skin, source, r, -1, albedo, &srm);
        entry.set.albedo = UploadMipped(textures, key + "_a", true, kBakeSize, std::move(albedo));
        entry.set.srm = UploadMipped(textures, key + "_s", false, kBakeSize, std::move(srm));
        entry.bytes += 2ull * kBakeSize * kBakeSize * 4 * 4 / 3;
    }
    if (mythic && source.channel == 0 && entry.set.frames.empty()) {
        const SkinBakeSource::Raster& r = Rasterize(source, kBakeFrameSize);
        for (int f = 0; f < kMythicAnimFrames; ++f) {
            std::vector<uint8_t> albedo;
            BakePixels(skin, source, r, f, albedo, nullptr);
            entry.set.frames.push_back(UploadMipped(textures, std::format("{}_f{}", key, f), true, kBakeFrameSize, std::move(albedo)));
        }
        entry.bytes += static_cast<uint64_t>(kMythicAnimFrames) * kBakeFrameSize * kBakeFrameSize * 4 * 4 / 3;
    }
    g_bake_bytes = g_bake_bytes - old_bytes + entry.bytes;
    entry.last_use = ++g_bake_clock;
    const BakedSkinMaterial out = entry.set;
    g_bakes[key] = std::move(entry);
    LogInfo("survival: skin {} baked onto {} ({} triangles{}) in {} ms, {} MB baked", skin.id, source.key, source.uv.size() / 3, mythic ? ", animated" : "",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), g_bake_bytes >> 20);
    Evict(key);
    return out;
}

void NoteSkinShown(TextureManager& textures, uint32_t material_index, const SkinBakeSource* source, int skin_id) {
    PumpRetired(textures);
    const std::string key = source && skin_id > 0 ? BakeKey(*source, skin_id) : std::string();
    std::string& now = g_shown[material_index];
    if (now == key) return;
    if (auto it = g_bakes.find(now); !now.empty() && it != g_bakes.end()) it->second.shown = std::max(0, it->second.shown - 1);
    if (auto it = g_bakes.find(key); !key.empty() && it != g_bakes.end()) it->second.shown += 1;
    now = key;
    Evict(key);
}

}
