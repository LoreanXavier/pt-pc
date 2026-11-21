#include "game/survival/survival_visions.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <vector>

#include "engine/core/log.h"
#include "engine/platform/os.h"

namespace pt::game {

namespace {

constexpr int kCardWidth = 768, kCardHeight = 1024;
constexpr int kPrintWidth = 640, kPrintHeight = 880;

struct Rng {
    uint32_t state;
    float Next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
    float Normal(float sigma) { return (Next() + Next() + Next() + Next() - 2.0f) * 1.7320508f * sigma; }
};

float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

std::vector<float> TornProfile(int count, float depth, Rng& rng) {
    std::vector<float> slow(static_cast<size_t>(count));
    float walk = 0.0f;
    for (float& v : slow) v = walk += rng.Normal(1.0f);
    const auto [lo, hi] = std::minmax_element(slow.begin(), slow.end());
    const float low = *lo, span = std::max(1e-6f, *hi - *lo);
    for (float& v : slow) v = (v - low) / span;
    std::vector<float> out(slow.size());
    for (int i = 0; i < count; ++i) {
        float sum = 0.0f;
        for (int j = -4; j <= 4; ++j) sum += slow[static_cast<size_t>(std::clamp(i + j, 0, count - 1))];
        const float fray = std::pow(rng.Next(), 3.0f);
        out[static_cast<size_t>(i)] = depth * (0.25f + 0.6f * sum / 9.0f + 0.15f * fray);
    }
    return out;
}

float Sample(const std::vector<float>& profile, float t) {
    const float at = Clamp01(t) * static_cast<float>(profile.size() - 1);
    const size_t i = static_cast<size_t>(at);
    const size_t j = std::min(profile.size() - 1, i + 1);
    return profile[i] + (profile[j] - profile[i]) * (at - static_cast<float>(i));
}

bool MakeCard(const std::filesystem::path& shot_path, const std::filesystem::path& card_path, uint32_t seed) {
    int width = 0, height = 0, channels = 0;
    uint8_t* shot = stbi_load(os::PathToUtf8(shot_path).c_str(), &width, &height, &channels, 3);
    if (!shot) return false;
    Rng rng{seed * 2654435761u + 17u};
    std::vector<float> print(size_t(kPrintWidth) * kPrintHeight * 3);
    const float scale_fit = std::min(static_cast<float>(width) / kPrintWidth, static_cast<float>(height) / kPrintHeight);
    const float ox = 0.5f * (static_cast<float>(width) - kPrintWidth * scale_fit), oy = 0.5f * (static_cast<float>(height) - kPrintHeight * scale_fit);
    for (int y = 0; y < kPrintHeight; ++y) {
        for (int x = 0; x < kPrintWidth; ++x) {
            const float sx = std::clamp(ox + (static_cast<float>(x) + 0.5f) * scale_fit - 0.5f, 0.0f, static_cast<float>(width - 1));
            const float sy = std::clamp(oy + (static_cast<float>(y) + 0.5f) * scale_fit - 0.5f, 0.0f, static_cast<float>(height - 1));
            const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy), x1 = std::min(width - 1, x0 + 1), y1 = std::min(height - 1, y0 + 1);
            const float fx = sx - static_cast<float>(x0), fy = sy - static_cast<float>(y0);
            for (int c = 0; c < 3; ++c) {
                auto at = [&](int px, int py) { return static_cast<float>(shot[(size_t(py) * width + px) * 3 + c]) / 255.0f; };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx, bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
                print[(size_t(y) * kPrintWidth + x) * 3 + c] = top + (bottom - top) * fy;
            }
        }
    }
    stbi_image_free(shot);
    std::vector<float> values(print);
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() * 985 / 1000), values.end());
    const float white = std::max(1e-3f, values[values.size() * 985 / 1000]);
    for (int y = 0; y < kPrintHeight; ++y) {
        for (int x = 0; x < kPrintWidth; ++x) {
            const float dx = (static_cast<float>(x) - kPrintWidth * 0.5f) / (kPrintWidth * 0.62f);
            const float dy = (static_cast<float>(y) - kPrintHeight * 0.42f) / (kPrintHeight * 0.66f);
            const float spot = std::pow(Clamp01(1.2f - std::sqrt(dx * dx + dy * dy)), 1.5f);
            float* p = &print[(size_t(y) * kPrintWidth + x) * 3];
            float rgb[3];
            for (int c = 0; c < 3; ++c) rgb[c] = std::pow(Clamp01((Clamp01(p[c] / white) * spot - 0.04f) / 0.86f), 1.2f);
            const float grey = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
            static constexpr float kWarm[3] = {1.0f, 0.93f, 0.83f}, kLift[3] = {0.02f, 0.018f, 0.015f};
            for (int c = 0; c < 3; ++c) p[c] = (rgb[c] * 0.45f + grey * 0.55f) * kWarm[c] + kLift[c];
        }
    }
    std::vector<float> soft(print.size());
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<float>& from = pass == 0 ? print : soft;
        std::vector<float>& to = pass == 0 ? soft : print;
        for (int y = 0; y < kPrintHeight; ++y) {
            for (int x = 0; x < kPrintWidth; ++x) {
                for (int c = 0; c < 3; ++c) {
                    float sum = 0.0f;
                    static constexpr float kTaps[5] = {0.06f, 0.24f, 0.4f, 0.24f, 0.06f};
                    for (int t = -2; t <= 2; ++t) {
                        const int sx = pass == 0 ? std::clamp(x + t, 0, kPrintWidth - 1) : x, sy = pass == 1 ? std::clamp(y + t, 0, kPrintHeight - 1) : y;
                        sum += kTaps[t + 2] * from[(size_t(sy) * kPrintWidth + sx) * 3 + c];
                    }
                    to[(size_t(y) * kPrintWidth + x) * 3 + c] = sum;
                }
            }
        }
    }
    for (size_t i = 0; i < print.size(); i += 3) {
        const float g = rng.Normal(0.045f);
        for (int c = 0; c < 3; ++c) print[i + c] = Clamp01(print[i + c] + g);
    }
    std::vector<float> card(size_t(kCardWidth) * kCardHeight * 3);
    static constexpr float kPaper[3] = {0.5f, 0.48f, 0.43f};
    for (int y = 0; y < kCardHeight; ++y) {
        for (int x = 0; x < kCardWidth; ++x) {
            const float edge = static_cast<float>(std::min({x, kCardWidth - 1 - x, y, kCardHeight - 1 - y}));
            const float dirt = 0.72f + 0.28f * Clamp01(edge / 60.0f), g = rng.Normal(0.04f);
            for (int c = 0; c < 3; ++c) card[(size_t(y) * kCardWidth + x) * 3 + c] = Clamp01(kPaper[c] * dirt + g);
        }
    }
    const int px0 = (kCardWidth - kPrintWidth) / 2, py0 = (kCardHeight - kPrintHeight) / 2 - 10;
    for (int y = 0; y < kPrintHeight; ++y) {
        std::copy_n(&print[size_t(y) * kPrintWidth * 3], size_t(kPrintWidth) * 3, &card[(size_t(py0 + y) * kCardWidth + px0) * 3]);
    }
    for (int s = 0; s < 3; ++s) {
        const int x = 40 + static_cast<int>(rng.Next() * 680.0f);
        for (int y = 0; y < kCardHeight; ++y) {
            for (int c = 0; c < 3; ++c) {
                float& v = card[(size_t(y) * kCardWidth + x) * 3 + c];
                v = Clamp01(v + 0.06f);
            }
        }
    }
    std::array<std::vector<float>, 4> sides;
    const int heavy_a = static_cast<int>(rng.Next() * 4.0f) % 4;
    const int heavy_b = (heavy_a + 1 + static_cast<int>(rng.Next() * 3.0f) % 3) % 4;
    for (int i = 0; i < 4; ++i) {
        sides[static_cast<size_t>(i)] = TornProfile(160, (i == heavy_a || i == heavy_b ? 0.06f : 0.012f) * kCardWidth, rng);
    }
    std::vector<uint8_t> rgba(size_t(kCardWidth) * kCardHeight * 4);
    for (int y = 0; y < kCardHeight; ++y) {
        for (int x = 0; x < kCardWidth; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / kCardWidth, fy = (static_cast<float>(y) + 0.5f) / kCardHeight;
            const float inside = std::min({static_cast<float>(y) + 0.5f - Sample(sides[0], fx), kCardWidth - (static_cast<float>(x) + 0.5f) - Sample(sides[1], fy),
                                           kCardHeight - (static_cast<float>(y) + 0.5f) - Sample(sides[2], 1.0f - fx),
                                           static_cast<float>(x) + 0.5f - Sample(sides[3], 1.0f - fy)});
            uint8_t* out = &rgba[(size_t(y) * kCardWidth + x) * 4];
            for (int c = 0; c < 3; ++c) out[c] = static_cast<uint8_t>(std::lround(card[(size_t(y) * kCardWidth + x) * 3 + c] * 255.0f));
            out[3] = static_cast<uint8_t>(std::lround(Clamp01(inside / 1.5f) * 255.0f));
        }
    }
    auto part = card_path;
    part.replace_extension(".part");
    if (!stbi_write_png(os::PathToUtf8(part).c_str(), kCardWidth, kCardHeight, 4, rgba.data(), kCardWidth * 4)) return false;
    std::error_code ec;
    std::filesystem::rename(part, card_path, ec);
    return !ec;
}

}

std::string SurvivalVisionRoute(const std::filesystem::path& dir) {
    std::string route = "20 sstep 15\n20 swait 30\n20 sloop 17\n20 sstep 31\n20 swait 400\n20 ssurvival\n20 swait 90\n20 shandylight 0\n20 swait 10\n";
    for (size_t kind = 0; kind < std::size(kSurvivalVisionNames); ++kind) {
        if (!*kSurvivalVisionNames[kind]) continue;
        route += std::format("20 sact studio={}\n20 swait 45\n20 sshot {}\n20 swait 2\n", kind,
                             os::PathToUtf8((dir / std::format("{}-shot.png", kSurvivalVisionNames[kind])).generic_u8string()));
    }
    route += "20 squit\n";
    return route;
}

bool MakeSurvivalVisionCards(const std::filesystem::path& dir) {
    bool all = true;
    for (size_t kind = 0; kind < std::size(kSurvivalVisionNames); ++kind) {
        const char* name = kSurvivalVisionNames[kind];
        if (!*name) continue;
        const auto shot = dir / std::format("{}-shot.png", name);
        const bool made = MakeCard(shot, dir / std::format("{}.png", name), static_cast<uint32_t>(kind) + 5u);
        LogInfo("survival visions: {} {}", name, made ? "made" : "not shot");
        std::error_code ec;
        if (made) std::filesystem::remove(shot, ec);
        all = all && made;
    }
    return all;
}

bool SurvivalVisionsReady(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const char* name : kSurvivalVisionNames) {
        if (*name && !std::filesystem::exists(dir / std::format("{}.png", name), ec)) return false;
    }
    return true;
}

}
