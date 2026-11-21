#include "game/survival/survival_acoustics.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "engine/audio/dsp.h"
#include "engine/audio/wem.h"

namespace pt::game::survival_acoustics {

namespace {

constexpr float kPi = 3.14159265f;
constexpr uint32_t kRate = 48000;
constexpr float kSound = 343.0f;
constexpr float kEchoSeconds = 0.2f;
constexpr std::array<float, 4> kDarkHz = {7000.0f, 4200.0f, 2400.0f, 1100.0f};
constexpr std::array<float, 3> kBandTime = {1.3f, 1.0f, 0.5f};
constexpr float kNoiseSeconds = 2.0f;
constexpr float kReach = 120.0f;
constexpr float kNear = 80.0f;
constexpr float kReflect = 0.82f;
constexpr float kScatter = 0.35f;
constexpr float kReference = 1.0f;
constexpr float kEchoLift = 7.0f;
constexpr float kLateLift = 1.7f;
constexpr float kHeadDelay = 0.00063f;
constexpr size_t kMaxEchoes = 120;

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

struct NoiseBank {
    std::array<std::array<std::vector<float>, 2>, 3> band;
};

const NoiseBank& Noise() {
    static const NoiseBank bank = [] {
        NoiseBank b;
        const size_t length = static_cast<size_t>(kNoiseSeconds * kRate);
        const size_t fade = kRate / 20;
        const size_t warm = kRate / 10;
        uint32_t seed = 0x7A11u;
        for (size_t k = 0; k < 3; ++k) {
            for (size_t c = 0; c < 2; ++c) {
                std::array<audio::Biquad, 3> f;
                if (k == 0) {
                    f[0].Set(audio::Biquad::Type::LowPass, 250.0f, 0.707f, 0.0f);
                    f[1].Set(audio::Biquad::Type::LowPass, 250.0f, 0.707f, 0.0f);
                    f[2].Set(audio::Biquad::Type::HighPass, 30.0f, 0.707f, 0.0f);
                } else if (k == 1) {
                    f[0].Set(audio::Biquad::Type::HighPass, 250.0f, 0.707f, 0.0f);
                    f[1].Set(audio::Biquad::Type::LowPass, 2500.0f, 0.707f, 0.0f);
                    f[2].Set(audio::Biquad::Type::LowPass, 2500.0f, 0.707f, 0.0f);
                } else {
                    f[0].Set(audio::Biquad::Type::HighPass, 2500.0f, 0.707f, 0.0f);
                    f[1].Set(audio::Biquad::Type::HighPass, 2500.0f, 0.707f, 0.0f);
                    f[2].Set(audio::Biquad::Type::LowPass, 9000.0f, 0.707f, 0.0f);
                }
                std::vector<float> raw(warm + length + fade);
                for (float& v : raw) {
                    seed = seed * 1664525u + 1013904223u;
                    float x = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
                    for (audio::Biquad& q : f) x = q.Process(x);
                    v = x;
                }
                std::vector<float>& out = b.band[k][c];
                out.assign(raw.begin() + static_cast<std::ptrdiff_t>(warm), raw.begin() + static_cast<std::ptrdiff_t>(warm + length));
                for (size_t i = 0; i < fade; ++i) {
                    const float a = std::sin(0.5f * kPi * static_cast<float>(i) / static_cast<float>(fade));
                    out[i] = out[i] * a + raw[warm + length + i] * std::sqrt(std::max(0.0f, 1.0f - a * a));
                }
                double sum = 0.0;
                for (const float v : out) sum += static_cast<double>(v) * v;
                const float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(out.size())));
                for (float& v : out) v /= std::max(rms, 1e-9f);
            }
        }
        return b;
    }();
    return bank;
}

struct Plan {
    struct Tap {
        std::array<uint32_t, 2> delay{};
        std::array<float, 2> gain{};
        uint8_t dark = 0;
    };
    std::shared_ptr<const std::vector<float>> dry;
    std::array<std::shared_ptr<const std::vector<float>>, 4> dark;
    std::vector<Tap> taps;
    float gain = 1.0f;
    double speed = 1.0;
    uint64_t frames = 0;
    uint64_t late_start = 0;
    float late_build = 1.0f;
    std::array<float, 3> late_amp{};
    std::array<float, 3> late_decay{};
    std::array<std::array<uint32_t, 2>, 3> noise_offset{};
};

class ShotReader final : public audio::MediaReader {
public:
    explicit ShotReader(std::shared_ptr<const Plan> plan) : plan_(std::move(plan)) { Seek(0); }

    uint32_t Read(float* out, uint32_t frames) override {
        const Plan& p = *plan_;
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(frames, p.frames - std::min(pos_, p.frames)));
        std::fill(out, out + static_cast<size_t>(n) * 2, 0.0f);
        {
            const std::vector<float>& d = *p.dry;
            const uint64_t total = d.size() / 2;
            for (uint32_t i = 0; i < n; ++i) {
                const double at = static_cast<double>(pos_ + i) * p.speed;
                const uint64_t k = static_cast<uint64_t>(at);
                if (k + 1 >= total) break;
                const float f = static_cast<float>(at - static_cast<double>(k));
                out[i * 2] = p.gain * ((1.0f - f) * d[k * 2] + f * d[k * 2 + 2]);
                out[i * 2 + 1] = p.gain * ((1.0f - f) * d[k * 2 + 1] + f * d[k * 2 + 3]);
            }
        }
        for (const Plan::Tap& tap : p.taps) {
            const std::vector<float>& src = *p.dark[tap.dark];
            for (size_t c = 0; c < 2; ++c) {
                const uint64_t start = tap.delay[c];
                const uint64_t lo = std::max(pos_, start), hi = std::min(pos_ + n, start + src.size());
                const float g = tap.gain[c];
                for (uint64_t j = lo; j < hi; ++j) out[(j - pos_) * 2 + c] += g * src[j - start];
            }
        }
        const NoiseBank& noise = Noise();
        const size_t length = noise.band[0][0].size();
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t t = pos_ + i;
            if (t < p.late_start) continue;
            const float build = Smooth(static_cast<float>(t - p.late_start) / p.late_build);
            for (size_t b = 0; b < 3; ++b) {
                const float e = env_[b] * build;
                out[i * 2] += e * noise.band[b][0][(p.noise_offset[b][0] + t) % length];
                out[i * 2 + 1] += e * noise.band[b][1][(p.noise_offset[b][1] + t) % length];
                env_[b] *= p.late_decay[b];
            }
        }
        pos_ += n;
        return n;
    }

    bool Seek(uint64_t frame) override {
        pos_ = frame;
        const Plan& p = *plan_;
        const double past = static_cast<double>(frame > p.late_start ? frame - p.late_start : 0);
        for (size_t b = 0; b < 3; ++b) env_[b] = p.late_amp[b] * static_cast<float>(std::pow(static_cast<double>(p.late_decay[b]), past));
        return true;
    }

    uint64_t Position() const override { return pos_; }

private:
    std::shared_ptr<const Plan> plan_;
    uint64_t pos_ = 0;
    std::array<float, 3> env_{};
};

std::shared_ptr<const std::vector<float>> Darkened(const std::vector<float>& mono, float hz) {
    std::array<audio::Biquad, 2> f;
    for (audio::Biquad& q : f) q.Set(audio::Biquad::Type::LowPass, hz, 0.707f, 0.0f);
    auto out = std::make_shared<std::vector<float>>(mono.size());
    for (size_t i = 0; i < mono.size(); ++i) {
        float x = mono[i];
        for (audio::Biquad& q : f) x = q.Process(x);
        (*out)[i] = x;
    }
    return out;
}

}

std::string Space::Describe() const {
    return std::format("{} echoes, enclosure {:.2f}, free path {:.1f} m, RT60 {:.2f} s, late {:.3f} from {:.0f} ms", echoes.size(), enclosure, free_path,
                       reverb_time, late_level, late_start * 1000.0f);
}

Space Probe(const RayFn& ray, const glm::vec3& ear, const glm::vec3& forward, const glm::vec3& right) {
    Space space;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    glm::vec3 f(forward.x, 0.0f, forward.z);
    f = glm::length(f) > 1e-4f ? glm::normalize(f) : glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 r(right.x, 0.0f, right.z);
    r = glm::length(r) > 1e-4f ? glm::normalize(r) : glm::normalize(glm::cross(f, up));
    struct Ring {
        float elevation;
        int count;
        float share;
    };
    constexpr Ring kRings[] = {{0.0f, 24, 0.17f}, {0.35f, 16, 0.36f}, {0.79f, 12, 0.31f}, {1.22f, 6, 0.14f}, {1.5707963f, 1, 0.02f}};
    struct Hit {
        glm::vec3 dir;
        glm::vec3 normal;
        float distance;
        float weight;
    };
    std::vector<Hit> hits;
    float closed = 0.0f, path = 0.0f, path_weight = 0.0f, first = kNear;
    for (const Ring& ring : kRings) {
        for (int i = 0; i < ring.count; ++i) {
            const float a = 2.0f * kPi * (static_cast<float>(i) + (ring.count > 1 ? 0.5f * (ring.elevation > 0.5f) : 0.0f)) / static_cast<float>(ring.count);
            const glm::vec3 dir = std::cos(ring.elevation) * (std::cos(a) * f + std::sin(a) * r) + std::sin(ring.elevation) * up;
            const float weight = ring.share / static_cast<float>(ring.count);
            float distance = 0.0f;
            glm::vec3 normal(0.0f);
            if (!ray(ear, dir, kReach, distance, normal) || distance < 0.05f) continue;
            if (glm::dot(normal, dir) > 0.0f) normal = -normal;
            hits.push_back({dir, normal, distance, weight});
            if (distance < kNear) {
                closed += weight;
                path += distance * weight;
                path_weight += weight;
                if (ring.elevation < 0.5f) first = std::min(first, distance);
            }
        }
    }
    space.enclosure = closed;
    space.free_path = path_weight > 0.0f ? path / path_weight : kNear;

    auto echo = [&](float distance, float gain, const glm::vec3& from, int order) {
        Echo e;
        e.delay = distance / kSound;
        e.gain = std::min(gain, kReflect * 0.5f);
        e.side = std::clamp(glm::dot(from, r), -1.0f, 1.0f);
        const int by_distance = distance < 12.0f ? 0 : distance < 30.0f ? 1 : distance < 80.0f ? 2 : 3;
        const int by_order = order <= 1 ? 0 : order <= 3 ? 2 : 3;
        e.dark = static_cast<uint8_t>(std::max(by_distance, by_order));
        space.echoes.push_back(e);
    };
    struct Plane {
        glm::vec3 normal;
        float distance;
    };
    std::vector<Plane> planes;
    for (const Hit& h : hits) {
        const float facing = -glm::dot(h.dir, h.normal);
        if (facing < 0.25f || std::abs(h.normal.y) > 0.6f) continue;
        const float p = h.distance * facing;
        bool known = false;
        for (const Plane& q : planes) known = known || (glm::dot(q.normal, h.normal) > 0.97f && std::abs(q.distance - p) < 0.8f);
        if (known) continue;
        float found = 0.0f;
        glm::vec3 n(0.0f);
        if (!ray(ear, -h.normal, p + 1.0f, found, n) || std::abs(found - p) > 0.6f) continue;
        planes.push_back({h.normal, p});
        echo(2.0f * p, kEchoLift * kReflect * kReference / (2.0f * p), -h.normal, 1);
    }
    {
        int best_a = -1, best_b = -1;
        float best = 1e9f;
        for (size_t i = 0; i < planes.size(); ++i) {
            for (size_t j = i + 1; j < planes.size(); ++j) {
                if (glm::dot(planes[i].normal, planes[j].normal) > -0.94f) continue;
                const float w = planes[i].distance + planes[j].distance;
                if (w < best) {
                    best = w;
                    best_a = static_cast<int>(i);
                    best_b = static_cast<int>(j);
                }
            }
        }
        if (best_a >= 0) {
            const Plane& a = planes[static_cast<size_t>(best_a)];
            const Plane& b = planes[static_cast<size_t>(best_b)];
            const float width = a.distance + b.distance;
            for (int k = -5; k <= 5; ++k) {
                for (int odd = 0; odd < 2; ++odd) {
                    const float x = 2.0f * static_cast<float>(k) * width + (odd ? 2.0f * b.distance : 0.0f);
                    const int order = odd ? std::abs(2 * k + 1) : std::abs(2 * k);
                    if (order < 2 || order > 10) continue;
                    const float d = std::abs(x);
                    echo(d, kEchoLift * std::pow(kReflect, static_cast<float>(order)) * kReference / d, x > 0.0f ? -b.normal : -a.normal, order);
                }
            }
        }
    }
    for (const Hit& h : hits) {
        const float facing = std::max(0.05f, -glm::dot(h.dir, h.normal));
        const float solid = 2.0f * kPi * h.weight;
        const float g = kEchoLift * kReflect * kReference * std::sqrt(solid * kScatter * facing / kPi) / h.distance;
        echo(2.0f * h.distance, g, h.dir, 1);
    }
    {
        float energy = 0.0f;
        for (const Echo& e : space.echoes) energy += e.gain * e.gain;
        if (energy > 1.0f) {
            const float k = 1.0f / std::sqrt(energy);
            for (Echo& e : space.echoes) e.gain *= k;
        }
    }
    if (space.echoes.size() > kMaxEchoes) {
        std::nth_element(space.echoes.begin(), space.echoes.begin() + kMaxEchoes, space.echoes.end(),
                         [](const Echo& x, const Echo& y) { return x.gain > y.gain; });
        space.echoes.resize(kMaxEchoes);
    }
    const float alpha = space.enclosure * 0.07f + (1.0f - space.enclosure);
    space.reverb_time = std::clamp(0.04f * 1.3f * space.free_path / alpha, 0.35f, 3.0f);
    space.late_level = kLateLift * std::pow(space.enclosure, 1.5f) * std::clamp(space.free_path / 8.0f, 0.35f, 1.0f);
    space.late_start = std::max(0.01f, 2.0f * first / kSound);
    return space;
}

Source Prepare(const survival_sounds::Take& take) {
    Source s;
    if (!take.pcm || take.pcm->empty()) return s;
    const uint32_t ch = std::max<uint32_t>(take.channels, 1);
    const size_t frames = take.pcm->size() / ch;
    auto dry = std::make_shared<std::vector<float>>(frames * 2);
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        const float l = (*take.pcm)[i * ch], r = (*take.pcm)[i * ch + (ch > 1 ? 1 : 0)];
        (*dry)[i * 2] = l;
        (*dry)[i * 2 + 1] = r;
        mono[i] = 0.5f * (l + r);
    }
    s.dry = dry;
    std::vector<float> head(mono.begin(), mono.begin() + static_cast<std::ptrdiff_t>(std::min(frames, static_cast<size_t>(kEchoSeconds * kRate))));
    const size_t fade = std::min(head.size(), static_cast<size_t>(kRate / 50));
    for (size_t i = 0; i < fade; ++i) head[head.size() - 1 - i] *= static_cast<float>(i) / static_cast<float>(fade);
    for (size_t k = 0; k < 4; ++k) s.dark[k] = Darkened(head, kDarkHz[k]);
    for (size_t b = 0; b < 3; ++b) {
        std::array<audio::Biquad, 2> f;
        if (b == 0) {
            f[0].Set(audio::Biquad::Type::LowPass, 250.0f, 0.707f, 0.0f);
            f[1].Set(audio::Biquad::Type::LowPass, 250.0f, 0.707f, 0.0f);
        } else if (b == 1) {
            f[0].Set(audio::Biquad::Type::HighPass, 250.0f, 0.707f, 0.0f);
            f[1].Set(audio::Biquad::Type::LowPass, 2500.0f, 0.707f, 0.0f);
        } else {
            f[0].Set(audio::Biquad::Type::HighPass, 2500.0f, 0.707f, 0.0f);
            f[1].Set(audio::Biquad::Type::HighPass, 2500.0f, 0.707f, 0.0f);
        }
        double sum = 0.0;
        for (float x : mono) {
            for (audio::Biquad& q : f) x = q.Process(x);
            sum += static_cast<double>(x) * x;
        }
        s.band_energy[b] = static_cast<float>(sum);
    }
    return s;
}

survival_sounds::Sound Render(const Source& source, const Space& space, float gain, float speed, uint32_t seed) {
    if (!source.Valid()) return nullptr;
    auto plan = std::make_shared<Plan>();
    plan->dry = source.dry;
    plan->dark = source.dark;
    plan->gain = gain;
    plan->speed = std::clamp(static_cast<double>(speed), 0.5, 2.0);
    uint64_t end = static_cast<uint64_t>(static_cast<double>(source.dry->size() / 2) / plan->speed);
    for (const Echo& e : space.echoes) {
        Plan::Tap tap;
        tap.dark = std::min<uint8_t>(e.dark, 3);
        const float side = std::clamp(e.side, -1.0f, 1.0f);
        const float lag = kHeadDelay * std::abs(side) * kRate;
        tap.delay[0] = static_cast<uint32_t>(e.delay * kRate + (side > 0.0f ? lag : 0.0f));
        tap.delay[1] = static_cast<uint32_t>(e.delay * kRate + (side < 0.0f ? lag : 0.0f));
        tap.gain[0] = gain * e.gain * std::sqrt(0.5f * (1.0f - 0.85f * side));
        tap.gain[1] = gain * e.gain * std::sqrt(0.5f * (1.0f + 0.85f * side));
        end = std::max<uint64_t>(end, std::max(tap.delay[0], tap.delay[1]) + source.dark[tap.dark]->size());
        plan->taps.push_back(tap);
    }
    plan->late_start = static_cast<uint64_t>(space.late_start * kRate);
    plan->late_build = std::max(1.0f, 2.0f * space.free_path / kSound * kRate);
    float longest = 0.0f;
    uint32_t s = seed * 2654435761u + 0x9E37u;
    for (size_t b = 0; b < 3; ++b) {
        const float rt = std::max(0.25f, space.reverb_time * kBandTime[b]);
        const float tau = rt / 6.91f * kRate;
        plan->late_decay[b] = std::exp(-1.0f / tau);
        plan->late_amp[b] = gain * std::sqrt(2.0f * space.late_level * source.band_energy[b] / tau);
        longest = std::max(longest, rt);
        for (size_t c = 0; c < 2; ++c) {
            s = s * 1664525u + 1013904223u;
            plan->noise_offset[b][c] = (s >> 8) % static_cast<uint32_t>(kNoiseSeconds * kRate);
        }
    }
    if (space.late_level > 0.0f) end = std::max<uint64_t>(end, plan->late_start + static_cast<uint64_t>(plan->late_build + longest * kRate));
    plan->frames = std::min<uint64_t>(end, static_cast<uint64_t>(4.5f * kRate));
    const uint64_t frames = plan->frames;
    std::shared_ptr<const Plan> shared = std::move(plan);
    return audio::Media::CreateGenerated(2, kRate, frames, [shared] { return std::make_unique<ShotReader>(shared); });
}

}
