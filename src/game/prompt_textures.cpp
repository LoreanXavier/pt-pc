#include "game/prompt_textures.h"

#include <glm/glm.hpp>

#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>

#include "engine/core/log.h"
#include "engine/fs/vfs.h"
#include "engine/render/texture_manager.h"

namespace pt::game {
namespace {

constexpr const char* kFramePath = "/Assets/sh/environ/object/shsb/label/shsb_labl001/sourceimages/shsb_labl001_p1_bsm_alp";

using Box = PromptBox;

constexpr Box kR3Box{51, 256, 553, 597};
constexpr int kThreeFrom = 290;
constexpr float kChalkWidth = 25.0f;

enum class Style { Paint, Chalk };

float Hash(int64_t i, uint32_t seed) {
    uint32_t v = static_cast<uint32_t>(i) * 374761393u + seed * 668265263u;
    v = (v ^ (v >> 13)) * 1274126177u;
    return static_cast<float>((v ^ (v >> 16)) & 0xFFFFu) / 65535.0f;
}

float Noise(float x, uint32_t seed) {
    const float fl = std::floor(x);
    const int64_t i = static_cast<int64_t>(fl);
    const float f = x - fl;
    const float t = f * f * (3.0f - 2.0f * f);
    const float a = Hash(i, seed);
    return a + (Hash(i + 1, seed) - a) * t;
}

std::vector<glm::vec2> CatmullRom(std::initializer_list<glm::vec2> points) {
    std::vector<glm::vec2> p(points);
    p.insert(p.begin(), p[0] * 2.0f - p[1]);
    p.push_back(p[p.size() - 1] * 2.0f - p[p.size() - 2]);
    std::vector<glm::vec2> out;
    constexpr int kSteps = 24;
    for (size_t i = 1; i + 2 < p.size(); ++i) {
        for (int k = 0; k < kSteps; ++k) {
            const float t = static_cast<float>(k) / kSteps;
            const float t2 = t * t;
            const float t3 = t2 * t;
            out.push_back(0.5f * (2.0f * p[i] + (p[i + 1] - p[i - 1]) * t + (2.0f * p[i - 1] - 5.0f * p[i] + 4.0f * p[i + 1] - p[i + 2]) * t2 +
                                  (3.0f * p[i] - p[i - 1] - 3.0f * p[i + 1] + p[i + 2]) * t3));
        }
    }
    out.push_back(p[p.size() - 2]);
    return out;
}

struct Layer {
    Box box;
    std::vector<float> a;

    explicit Layer(Box b) : box(b), a(static_cast<size_t>(b.Width()) * b.Height(), 0.0f) {}
    float& At(int x, int y) { return a[static_cast<size_t>(y - box.y0) * box.Width() + (x - box.x0)]; }
    void Over(int x, int y, float v) {
        float& d = At(x, y);
        d = d + v * (1.0f - d);
    }
};

void Stroke(Layer& layer, const std::vector<glm::vec2>& path, float width, uint32_t seed, Style style) {
    if (path.size() < 2) {
        return;
    }
    const float half = width * 0.5f;
    const float reach = half * 1.3f + 2.0f;
    std::vector<float> cumulative(path.size(), 0.0f);
    for (size_t i = 1; i < path.size(); ++i) {
        cumulative[i] = cumulative[i - 1] + glm::length(path[i] - path[i - 1]);
    }
    const float total = cumulative.back();
    const float dry_length = std::min(width * 1.6f, total * 0.25f);
    const int w = layer.box.Width();
    const int h = layer.box.Height();
    std::vector<float> dist(static_cast<size_t>(w) * h, 1e9f);
    std::vector<float> along(dist.size(), 0.0f);
    std::vector<float> side(dist.size(), 0.0f);
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const glm::vec2 a = path[i];
        const glm::vec2 b = path[i + 1];
        const float len = glm::length(b - a);
        if (len < 1e-4f) {
            continue;
        }
        const glm::vec2 dir = (b - a) / len;
        const int x0 = std::max(layer.box.x0, static_cast<int>(std::floor(std::min(a.x, b.x) - reach)));
        const int x1 = std::min(layer.box.x1, static_cast<int>(std::ceil(std::max(a.x, b.x) + reach)));
        const int y0 = std::max(layer.box.y0, static_cast<int>(std::floor(std::min(a.y, b.y) - reach)));
        const int y1 = std::min(layer.box.y1, static_cast<int>(std::ceil(std::max(a.y, b.y) + reach)));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const glm::vec2 q(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                const float u = std::clamp(glm::dot(q - a, dir), 0.0f, len);
                const float d = glm::length(q - (a + dir * u));
                const size_t k = static_cast<size_t>(y - layer.box.y0) * w + (x - layer.box.x0);
                if (d < dist[k]) {
                    dist[k] = d;
                    along[k] = cumulative[i] + u;
                    side[k] = dir.x * (q.y - a.y) - dir.y * (q.x - a.x);
                }
            }
        }
    }
    for (int y = layer.box.y0; y < layer.box.y1; ++y) {
        for (int x = layer.box.x0; x < layer.box.x1; ++x) {
            const size_t k = static_cast<size_t>(y - layer.box.y0) * w + (x - layer.box.x0);
            if (dist[k] > reach) {
                continue;
            }
            const float t = along[k];
            const float end = std::min(t, total - t);
            const float taper = std::sqrt(std::clamp(end / (width * 0.5f), 0.0f, 1.0f));
            const float wobble = 1.0f + 0.2f * (Noise(t / 45.0f, seed) - 0.5f);
            const float half_t = half * wobble * (0.7f + 0.3f * taper);
            const float edge = (Noise(t / 5.0f, seed + 3) - 0.5f) * 0.1f * half;
            float alpha = std::clamp((half_t + edge - dist[k]) / 1.3f + 0.5f, 0.0f, 1.0f);
            if (alpha <= 0.0f) {
                continue;
            }
            const float across = std::clamp((side[k] / std::max(half_t, 1e-3f) + 1.0f) * 0.5f, 0.0f, 1.0f);
            if (style == Style::Paint) {
                const float bristle = Noise(across * 23.0f, seed + 11) * 0.65f + Noise(across * 57.0f, seed + 17) * 0.35f;
                const float dry = std::clamp(1.0f - end / dry_length, 0.0f, 1.0f);
                const float speck = Noise(t / 2.5f + across * 31.0f, seed + 23);
                const float body = 0.9f + 0.08f * Noise(t / 30.0f + across * 3.0f, seed + 29);
                alpha *= bristle + 0.2f * (speck - 0.5f) > 0.12f + 0.7f * dry ? body : 0.25f * body * (1.0f - dry);
            } else {
                const float bristle = Noise(across * 9.0f, seed + 11);
                const float start = std::clamp(1.0f - t / (width * 0.6f), 0.0f, 1.0f);
                alpha *= bristle > 0.25f + 0.5f * start ? 1.0f : 1.0f - start;
            }
            layer.Over(x, y, alpha);
        }
    }
}

void Fill(Layer& layer, const std::function<float(glm::vec2)>& sdf, Box area, uint32_t seed, float opacity) {
    for (int y = std::max(area.y0, layer.box.y0); y < std::min(area.y1, layer.box.y1); ++y) {
        for (int x = std::max(area.x0, layer.box.x0); x < std::min(area.x1, layer.box.x1); ++x) {
            const glm::vec2 q(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            const float rough = (Noise(q.x / 7.0f + q.y / 11.0f, seed) - 0.5f) * 3.0f;
            const float alpha = std::clamp(0.5f - (sdf(q) + rough) / 1.3f, 0.0f, 1.0f) * opacity;
            if (alpha > 0.0f) {
                layer.Over(x, y, alpha);
            }
        }
    }
}

void DrawMouse(Layer& l, glm::vec2 c, float s, int button, float line, Style style) {
    const glm::vec2 half = glm::vec2(100.0f, 150.0f) * s;
    constexpr float kPower = 2.6f;
    const float split = c.y - 30.0f * s;
    const float sign = button == 1 ? -1.0f : 1.0f;
    const glm::vec2 inner = half - line * 0.3f;
    auto body = [c, inner](glm::vec2 q) {
        const glm::vec2 d = glm::abs(q - c) / inner;
        return (std::pow(std::pow(d.x, kPower) + std::pow(d.y, kPower), 1.0f / kPower) - 1.0f) * std::min(inner.x, inner.y);
    };
    auto pressed = [body, split, sign, c, s](glm::vec2 q) { return std::max({body(q), q.y - split, sign * (c.x - q.x) + 2.0f * s}); };
    const Box area{static_cast<int>(c.x - half.x - 10), static_cast<int>(c.y - half.y - 10), static_cast<int>(c.x + half.x + 10),
                   static_cast<int>(c.y + half.y + 10)};
    Fill(l, pressed, area, 121, style == Style::Paint ? 0.92f : 1.0f);
    std::vector<glm::vec2> ring;
    constexpr int kSegments = 72;
    for (int k = 0; k <= kSegments + 8; ++k) {
        const float a = 2.3561945f + static_cast<float>(k) / kSegments * 6.2831853f;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        ring.push_back(c + glm::vec2(std::copysign(std::pow(std::abs(ca), 2.0f / kPower), ca) * half.x,
                                     std::copysign(std::pow(std::abs(sa), 2.0f / kPower), sa) * half.y));
    }
    Stroke(l, ring, line, 122, style);
    Stroke(l, CatmullRom({{c.x - half.x * 0.98f, split}, {c.x, split + 3.0f * s}, {c.x + half.x * 0.98f, split}}), line * 0.85f, 123, style);
    Stroke(l, CatmullRom({{c.x, c.y - half.y * 0.99f}, {c.x + 2.0f * s, c.y - half.y * 0.6f}, {c.x, split}}), line * 0.8f, 124, style);
}

void ChalkS(Layer& l, glm::vec2 c) {
    Stroke(l, CatmullRom({c + glm::vec2(62, -82), c + glm::vec2(10, -102), c + glm::vec2(-55, -78), c + glm::vec2(-50, -25), c + glm::vec2(10, 2),
                          c + glm::vec2(62, 40), c + glm::vec2(45, 88), c + glm::vec2(-15, 100), c + glm::vec2(-70, 78)}),
           kChalkWidth, 31, Style::Chalk);
}

void ChalkStick(Layer& l, glm::vec2 c) {
    const glm::vec2 ball = c + glm::vec2(0.0f, -58.0f);
    Fill(l, [ball](glm::vec2 q) { return glm::length(q - ball) - 40.0f; },
         {static_cast<int>(c.x - 60), static_cast<int>(c.y - 120), static_cast<int>(c.x + 60), static_cast<int>(c.y)}, 61, 1.0f);
    Stroke(l, CatmullRom({c + glm::vec2(2, -30), c + glm::vec2(0, 20), c + glm::vec2(1, 62)}), kChalkWidth, 62, Style::Chalk);
    Stroke(l, CatmullRom({c + glm::vec2(-72, 66), c + glm::vec2(-30, 80), c + glm::vec2(30, 80), c + glm::vec2(74, 64)}), kChalkWidth, 63, Style::Chalk);
}

bool PatchLevels(FtexTexture& ftex, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, const std::vector<Box>& boxes) {
    const int width = static_cast<int>(ftex.width);
    const bool bc3 = ftex.pixel_format == 4;
    if (ftex.pixel_format != 2 && !bc3) {
        return false;
    }
    const size_t block_size = bc3 ? 16 : 8;
    for (uint32_t level = 0; level < ftex.mip_count && level < ftex.mips.size(); ++level) {
        if (ftex.mips[level].empty()) {
            continue;
        }
        const int lw = static_cast<int>(ftex.MipWidth(level));
        const int lh = static_cast<int>(ftex.MipHeight(level));
        const int scale = 1 << level;
        std::vector<uint8_t> pixels;
        if (level == 0) {
            pixels = before;
        } else if (!DecodeFtexLevel(ftex, level, pixels)) {
            return false;
        }
        const int bw = (lw + 3) / 4;
        for (const Box& box : boxes) {
            const int bx0 = std::max(0, box.x0 / scale / 4);
            const int by0 = std::max(0, box.y0 / scale / 4);
            const int bx1 = std::min(bw, (box.x1 + scale - 1) / scale / 4 + 1);
            const int by1 = std::min((lh + 3) / 4, (box.y1 + scale - 1) / scale / 4 + 1);
            for (int by = by0; by < by1; ++by) {
                for (int bx = bx0; bx < bx1; ++bx) {
                    uint8_t block[16 * 4];
                    bool changed = false;
                    for (int p = 0; p < 16; ++p) {
                        const int x = std::min(bx * 4 + (p & 3), lw - 1);
                        const int y = std::min(by * 4 + (p >> 2), lh - 1);
                        float delta[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        for (int sy = 0; sy < scale; ++sy) {
                            for (int sx = 0; sx < scale; ++sx) {
                                const size_t i = (static_cast<size_t>(y * scale + sy) * width + x * scale + sx) * 4;
                                for (int c = 0; c < 4; ++c) {
                                    delta[c] += static_cast<float>(after[i + c]) - static_cast<float>(before[i + c]);
                                }
                            }
                        }
                        const uint8_t* old = &pixels[(static_cast<size_t>(y) * lw + x) * 4];
                        for (int c = 0; c < 4; ++c) {
                            const float d = delta[c] / static_cast<float>(scale * scale);
                            changed = changed || std::abs(d) >= 0.5f;
                            block[p * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(old[c] + d), 0L, 255L));
                        }
                    }
                    if (!changed) {
                        continue;
                    }
                    uint8_t* out = ftex.mips[level].data() + block_size * (static_cast<size_t>(by) * bw + bx);
                    if (bc3) {
                        EncodeBc3AlphaBlock(block, out);
                    } else {
                        EncodeBc1Block(block, out);
                    }
                }
            }
        }
    }
    return true;
}

void Dump(const char* path, const std::string& variant, const FtexTexture& ftex, const std::vector<uint8_t>& before,
          const std::vector<uint8_t>& after, const std::vector<Box>& boxes) {
    const char* dump = std::getenv("PT_PROMPT_TEXTURE_DUMP");
    if (!dump || !*dump) {
        return;
    }
    const int width = static_cast<int>(ftex.width);
    std::string stem = FtexStem(path);
    stem = stem.substr(stem.find_last_of('/') + 1);
    const std::filesystem::path whole = std::filesystem::path(dump) / std::format("{}_level0.png", stem);
    if (!std::filesystem::exists(whole)) {
        stbi_write_png(whole.string().c_str(), width, static_cast<int>(ftex.height), 4, before.data(), width * 4);
    }
    for (size_t b = 0; b < boxes.size(); ++b) {
        const Box& box = boxes[b];
        std::vector<uint8_t> crop(static_cast<size_t>(box.Width()) * box.Height() * 4);
        for (int y = 0; y < box.Height(); ++y) {
            std::copy_n(&after[(static_cast<size_t>(box.y0 + y) * width + box.x0) * 4], box.Width() * 4, &crop[static_cast<size_t>(y) * box.Width() * 4]);
        }
        const std::filesystem::path file = std::filesystem::path(dump) / std::format("{}_{}_{}.png", stem, variant, b);
        stbi_write_png(file.string().c_str(), box.Width(), box.Height(), 4, crop.data(), box.Width() * 4);
    }
}

const char* TargetName() {
    return "the fallen frame's R3";
}

}

PromptTextures::~PromptTextures() {
    for (Target& target : targets_) {
        if (target.pending.valid()) {
            target.pending.wait();
        }
    }
}

void PromptTextures::Init(Vfs& vfs, TextureManager& textures, bool background) {
    vfs_ = &vfs;
    textures_ = &textures;
    const char* worker = std::getenv("PT_PROMPT_TEXTURE_WORKER");
    background_ = background || (worker && *worker == '1');
    targets_.clear();
    targets_.resize(1);
    targets_[0].textures.resize(1);
    targets_[0].textures[0].path = kFramePath;
}

std::string PromptTextures::VariantFor(const PromptStyle& style) const {
    if (style.device == PromptDevice::PlayStation || style.device == PromptDevice::Steam) {
        return {};
    }
    if (style.device == PromptDevice::Keyboard) {
        const KeyBinding* binding = FirstBinding(KeyAction::Zoom);
        return binding && binding->mouse ? std::format("mouse{}", binding->mouse) : std::string();
    }
    return style.device == PromptDevice::Nintendo ? "stick" : "rs";
}

PromptTextures::Painted PromptTextures::Paint(Target& target, const std::string& variant) {
    const auto started = std::chrono::steady_clock::now();
    Painted painted;
    painted.variant = variant;
    painted.textures.resize(target.textures.size());
    painted.ok.assign(target.textures.size(), false);
    {
        Texture& texture = target.textures.front();
        FtexTexture& ftex = painted.textures.front();
        ftex = texture.ftex;
        std::vector<uint8_t> before;
        if (DecodeFtexLevel(ftex, 0, before)) {
            std::vector<uint8_t> after = before;
            const int width = static_cast<int>(ftex.width);
            const bool whole = variant.starts_with("mouse");
            Layer layer(kR3Box);
            const glm::vec2 origin(static_cast<float>(kR3Box.x0), static_cast<float>(kR3Box.y0));
            if (whole) {
                DrawMouse(layer, origin + glm::vec2(250.0f, 172.0f), 0.9f, std::atoi(variant.c_str() + 5), kChalkWidth * 1.2f, Style::Chalk);
            } else if (variant == "stick") {
                ChalkStick(layer, origin + glm::vec2(395.0f, 150.0f));
            } else {
                ChalkS(layer, origin + glm::vec2(395.0f, 142.0f));
            }
            for (int y = kR3Box.y0; y < kR3Box.y1; ++y) {
                for (int x = kR3Box.x0; x < kR3Box.x1; ++x) {
                    uint8_t& a = after[(static_cast<size_t>(y) * width + x) * 4 + 3];
                    const bool rub = whole || x - kR3Box.x0 >= kThreeFrom;
                    const float old = rub ? 0.0f : a / 255.0f;
                    const float v = layer.At(x, y);
                    a = static_cast<uint8_t>(std::lround((old + v * (1.0f - old)) * 255.0f));
                }
            }
            Dump(texture.path, variant, ftex, before, after, {kR3Box});
            painted.ok[0] = PatchLevels(ftex, before, after, {kR3Box});
        }
    }
    painted.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return painted;
}

void PromptTextures::Finish(Target& target, Painted painted) {
    for (size_t t = 0; t < target.textures.size(); ++t) {
        Texture& texture = target.textures[t];
        uint32_t index = texture.original;
        if (t < painted.ok.size() && painted.ok[t]) {
            const FtexTexture& ftex = painted.textures[t];
            uint32_t first = 0;
            while (first < ftex.mip_count && ftex.mips[first].empty()) {
                ++first;
            }
            std::vector<TextureMip> mips;
            for (uint32_t level = first; level < ftex.mip_count; ++level) {
                mips.push_back({std::max(ftex.MipWidth(level), 1u), std::max(ftex.MipHeight(level), 1u), ftex.mips[level]});
            }
            index = textures_->Create(FtexStem(texture.path) + "#prompt:" + painted.variant, ftex.Format(), mips);
        }
        texture.variants[painted.variant] = index;
    }
    LogInfo("prompt textures: {} painted with {} in {:.0f} ms{}", TargetName(), painted.variant, painted.ms,
            background_ ? " (worker)" : "");
}

void PromptTextures::Update(const PromptStyle& style) {
    if (!textures_) {
        return;
    }
    const bool changed = !styled_ || !(style == style_);
    styled_ = true;
    style_ = style;
    for (Target& target : targets_) {
        if (target.failed) {
            continue;
        }
        if (!target.loaded) {
            bool all = true;
            for (Texture& texture : target.textures) {
                texture.original = textures_->Find(FtexStem(texture.path));
                all = all && texture.original != TextureManager::kWhite;
            }
            if (!all) {
                continue;
            }
            for (Texture& texture : target.textures) {
                if (!LoadFtex(vfs_->Textures(), texture.path, texture.ftex)) {
                    target.failed = true;
                    LogWarn("prompt textures: {} not readable", texture.path);
                }
            }
            if (target.failed) {
                continue;
            }
            target.loaded = true;
        } else if (!changed && !target.pending.valid()) {
            continue;
        }
        if (target.pending.valid()) {
            if (target.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                continue;
            }
            Finish(target, target.pending.get());
        }
        const std::string variant = VariantFor(style);
        if (target.applied && variant == target.shown) {
            continue;
        }
        if (!variant.empty() && !target.textures.front().variants.contains(variant)) {
            if (background_) {
                target.pending = std::async(std::launch::async, [&target, variant] { return Paint(target, variant); });
                continue;
            }
            Finish(target, Paint(target, variant));
        }
        for (Texture& texture : target.textures) {
            textures_->RedirectTexture(texture.original, variant.empty() ? texture.original : texture.variants[variant]);
        }
        LogInfo("prompt textures: {} shows {} ({})", TargetName(),
                variant.empty() ? std::string("the data's button") : variant, PromptDeviceName(style.device));
        target.shown = variant;
        target.applied = true;
    }
}

}
