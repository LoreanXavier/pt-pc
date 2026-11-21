#include "game/survival/survival_sounds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/audio/wem.h"
#include "engine/core/log.h"
#include "engine/core/resource_path.h"
#include "engine/fs/mods.h"

namespace pt::game::survival_sounds {

namespace {

constexpr float kPi = 3.14159265f;
constexpr uint32_t kRate = 48000;

class PcmReader final : public audio::MediaReader {
public:
    PcmReader(std::shared_ptr<const std::vector<float>> pcm, uint32_t channels) : pcm_(std::move(pcm)), channels_(channels) {}
    uint32_t Read(float* out, uint32_t frames) override {
        const uint64_t left = pcm_->size() / channels_ - pos_;
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(frames, left));
        std::copy_n(pcm_->data() + pos_ * channels_, static_cast<size_t>(n) * channels_, out);
        pos_ += n;
        return n;
    }
    bool Seek(uint64_t frame) override {
        pos_ = std::min<uint64_t>(frame, pcm_->size() / channels_);
        return true;
    }
    uint64_t Position() const override { return pos_; }

private:
    std::shared_ptr<const std::vector<float>> pcm_;
    uint32_t channels_;
    uint64_t pos_ = 0;
};

class VariedReader final : public audio::MediaReader {
public:
    VariedReader(std::shared_ptr<const std::vector<float>> pcm, uint32_t channels, float gain, double speed)
        : pcm_(std::move(pcm)), channels_(channels), gain_(gain), speed_(speed) {}
    uint32_t Read(float* out, uint32_t frames) override {
        const uint64_t total = pcm_->size() / channels_;
        const float* p = pcm_->data();
        uint32_t n = 0;
        for (; n < frames; ++n, ++pos_) {
            const double at = static_cast<double>(pos_) * speed_;
            const uint64_t i = static_cast<uint64_t>(at);
            if (i + 1 >= total) break;
            const float f = static_cast<float>(at - static_cast<double>(i));
            for (uint32_t c = 0; c < channels_; ++c) {
                out[n * channels_ + c] = gain_ * ((1.0f - f) * p[i * channels_ + c] + f * p[(i + 1) * channels_ + c]);
            }
        }
        return n;
    }
    bool Seek(uint64_t frame) override {
        pos_ = frame;
        return true;
    }
    uint64_t Position() const override { return pos_; }

private:
    std::shared_ptr<const std::vector<float>> pcm_;
    uint32_t channels_;
    float gain_;
    double speed_;
    uint64_t pos_ = 0;
};

class GainReader final : public audio::MediaReader {
public:
    GainReader(std::unique_ptr<audio::MediaReader> inner, uint32_t channels, float gain) : inner_(std::move(inner)), channels_(channels), gain_(gain) {}
    uint32_t Read(float* out, uint32_t frames) override {
        const uint32_t n = inner_ ? inner_->Read(out, frames) : 0;
        for (size_t i = 0; i < static_cast<size_t>(n) * channels_; ++i) out[i] *= gain_;
        return n;
    }
    bool Seek(uint64_t frame) override { return inner_ && inner_->Seek(frame); }
    uint64_t Position() const override { return inner_ ? inner_->Position() : 0; }

private:
    std::unique_ptr<audio::MediaReader> inner_;
    uint32_t channels_;
    float gain_;
};

Sound Make(std::vector<float> samples, uint32_t channels = 1) {
    auto pcm = std::make_shared<const std::vector<float>>(std::move(samples));
    const uint64_t frames = pcm->size() / channels;
    return audio::Media::CreateGenerated(channels, kRate, frames, [pcm, channels] { return std::make_unique<PcmReader>(pcm, channels); });
}

struct Noise {
    uint32_t seed;
    float operator()() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
    }
};

struct LowPass {
    float a;
    float y = 0.0f;
    explicit LowPass(float hz) : a(std::exp(-2.0f * kPi * hz / static_cast<float>(kRate))) {}
    float operator()(float x) { return y = a * y + (1.0f - a) * x; }
};

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

std::vector<float> Samples(float seconds) {
    return std::vector<float>(static_cast<size_t>(seconds * static_cast<float>(kRate)), 0.0f);
}

void Normalize(std::vector<float>& s, float peak) {
    float m = 0.0f;
    for (float v : s) m = std::max(m, std::abs(v));
    if (m <= 0.0f) return;
    for (float& v : s) v = std::tanh(v / m * 1.4f) / std::tanh(1.4f) * peak;
}

void AddClick(std::vector<float>& s, float at, float hz, float ring, float gain, Noise& noise) {
    const size_t from = static_cast<size_t>(at * kRate);
    for (size_t i = from; i < s.size(); ++i) {
        const float u = static_cast<float>(i - from) / kRate;
        if (u > 0.2f) break;
        s[i] += gain * (noise() * std::exp(-u / 0.0013f) + 0.55f * std::sin(2.0f * kPi * hz * u) * std::exp(-u / ring));
    }
}

void AddScrape(std::vector<float>& s, float at, float length, float hz, float gain, Noise& noise) {
    LowPass lp(hz);
    const size_t from = static_cast<size_t>(at * kRate);
    const size_t to = std::min(s.size(), static_cast<size_t>((at + length) * kRate));
    for (size_t i = from; i < to; ++i) {
        const float u = static_cast<float>(i - from) / kRate;
        s[i] += gain * lp(noise()) * std::sin(kPi * u / length);
    }
}

}

Sound FromFile(std::string_view name) {
    const std::filesystem::path path = ExecutableDir() / "survival" / "sounds" / (std::string(name) + ".wav");
    const std::optional<std::vector<uint8_t>> bytes = mods::ReadDiskFile(path);
    if (!bytes) return nullptr;
    const std::vector<uint8_t>& b = *bytes;
    auto u16 = [&](size_t at) { return static_cast<uint32_t>(b[at] | b[at + 1] << 8); };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        LogWarn("survival: sounds/{}.wav is not a WAVE file", name);
        return nullptr;
    }
    uint32_t format = 0, channels = 0, rate = 0, bits = 0;
    size_t data = 0, data_size = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t size = u32(at + 4);
        if (std::memcmp(b.data() + at, "fmt ", 4) == 0 && size >= 16 && at + 8 + size <= b.size()) {
            format = u16(at + 8);
            channels = u16(at + 10);
            rate = u32(at + 12);
            bits = u16(at + 22);
            if (format == 0xFFFE && size >= 26) format = u16(at + 32);
        } else if (std::memcmp(b.data() + at, "data", 4) == 0) {
            data = at + 8;
            data_size = std::min<size_t>(size, b.size() - data);
        }
        at += 8 + size + (size & 1);
    }
    const bool pcm16 = format == 1 && bits == 16, float32 = format == 3 && bits == 32;
    if (!data || (!pcm16 && !float32) || rate != kRate || channels < 1 || channels > 2) {
        LogWarn("survival: sounds/{}.wav is format {} {} bit {} Hz {} channels (16 bit or float PCM at 48 kHz, mono or stereo)", name, format,
                bits, rate, channels);
        return nullptr;
    }
    std::vector<float> samples(data_size / (bits / 8));
    for (size_t i = 0; i < samples.size(); ++i) {
        if (pcm16) {
            samples[i] = static_cast<float>(static_cast<int16_t>(u16(data + i * 2))) / 32768.0f;
        } else {
            std::memcpy(&samples[i], b.data() + data + i * 4, 4);
        }
    }
    samples.resize(samples.size() - samples.size() % channels);
    LogInfo("survival: sound {} recorded ({:.2f} s, {} channel{})", name, static_cast<float>(samples.size() / channels) / kRate, channels,
            channels > 1 ? "s" : "");
    return Make(std::move(samples), channels);
}

std::vector<Sound> FromFilePool(std::string_view name) {
    std::vector<Sound> pool;
    for (int k = 1;; ++k) {
        const std::string take = std::string(name) + "_" + std::to_string(k);
        if (!std::filesystem::exists(ExecutableDir() / "survival" / "sounds" / (take + ".wav"))) break;
        if (Sound sound = FromFile(take)) pool.push_back(std::move(sound));
    }
    if (pool.empty()) {
        if (Sound sound = FromFile(name)) pool.push_back(std::move(sound));
    }
    return pool;
}

std::map<std::string, Sound, std::less<>> FromFolder(std::string_view folder) {
    std::map<std::string, Sound, std::less<>> sounds;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(ExecutableDir() / "survival" / "sounds" / folder, error)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".wav") continue;
        const std::string name = entry.path().stem().string();
        if (Sound sound = FromFile(std::string(folder) + "/" + name)) sounds.emplace(name, std::move(sound));
    }
    return sounds;
}

Take Hold(Sound media, bool decode) {
    Take take;
    if (!media) return take;
    take.channels = std::max<uint32_t>(media->Channels(), 1);
    if (decode && media->SampleRate() == kRate) {
        std::vector<float> pcm(static_cast<size_t>(media->Frames()) * take.channels);
        std::unique_ptr<audio::MediaReader> reader = media->OpenReader();
        size_t frames = 0;
        while (reader && frames < media->Frames()) {
            const uint32_t n = reader->Read(pcm.data() + frames * take.channels, static_cast<uint32_t>(std::min<uint64_t>(4096, media->Frames() - frames)));
            if (n == 0) break;
            frames += n;
        }
        pcm.resize(frames * take.channels);
        take.pcm = std::make_shared<const std::vector<float>>(std::move(pcm));
    }
    take.media = std::move(media);
    return take;
}

Sound Varied(const Take& take, float gain, float speed) {
    if (take.pcm && !take.pcm->empty()) {
        const uint64_t total = take.pcm->size() / take.channels;
        const double rate = std::clamp(static_cast<double>(speed), 0.25, 4.0);
        const uint64_t frames = static_cast<uint64_t>(static_cast<double>(total - 1) / rate);
        return audio::Media::CreateGenerated(take.channels, kRate, frames, [pcm = take.pcm, channels = take.channels, gain, rate] {
            return std::make_unique<VariedReader>(pcm, channels, gain, rate);
        });
    }
    if (!take.media) return nullptr;
    if (gain == 1.0f) return take.media;
    return audio::Media::CreateGenerated(take.channels, take.media->SampleRate(), take.media->Frames(), [media = take.media, channels = take.channels, gain] {
        return std::make_unique<GainReader>(media->OpenReader(), channels, gain);
    });
}

Sound Gunshot(float thump_hz, float thump_decay, float blast_decay, float blast_hz, float tail, float peak, unsigned seed) {
    std::vector<float> s = Samples(blast_decay * 6.0f + tail * 4.0f);
    Noise noise{seed};
    LowPass blast(blast_hz), echo(blast_hz * 0.25f), echo2(blast_hz * 0.12f);
    float phase = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        phase += 2.0f * kPi * thump_hz * (1.0f + 1.6f * std::exp(-t / 0.02f)) / kRate;
        const float crack = n * std::exp(-t / 0.0035f);
        const float body = blast(n) * std::exp(-t / blast_decay) * 2.2f;
        const float thump = std::sin(phase) * std::exp(-t / thump_decay);
        const float slap = (t > 0.045f && t < 0.06f ? 0.6f : 0.0f) + (t > 0.09f && t < 0.11f ? 0.4f : 0.0f);
        const float echo_env = std::min(1.0f, t / 0.04f) * std::exp(-t / tail);
        s[i] = 0.55f * crack + 0.9f * body + 0.85f * thump + 1.4f * echo(n) * echo_env + slap * echo2(n) * 2.0f;
    }
    Normalize(s, peak);
    return Make(std::move(s));
}

Sound DryFire() {
    std::vector<float> s = Samples(0.12f);
    Noise noise{41};
    AddClick(s, 0.0f, 3100.0f, 0.006f, 1.0f, noise);
    Normalize(s, 0.3f);
    return Make(std::move(s));
}

Sound Reload(bool empty) {
    std::vector<float> s = Samples(empty ? 1.35f : 1.1f);
    Noise noise{empty ? 77u : 78u};
    AddClick(s, 0.0f, 2100.0f, 0.01f, 0.7f, noise);
    AddScrape(s, 0.04f, 0.16f, 2600.0f, 0.5f, noise);
    const float seat = empty ? 0.62f : 0.6f;
    AddScrape(s, seat - 0.14f, 0.12f, 2200.0f, 0.45f, noise);
    AddClick(s, seat, 1800.0f, 0.014f, 1.0f, noise);
    AddClick(s, seat + 0.012f, 900.0f, 0.03f, 0.5f, noise);
    if (empty) {
        AddScrape(s, 1.06f, 0.06f, 3200.0f, 0.4f, noise);
        AddClick(s, 1.12f, 2500.0f, 0.018f, 1.1f, noise);
    }
    Normalize(s, 0.42f);
    return Make(std::move(s));
}

Sound Pump() {
    std::vector<float> s = Samples(0.5f);
    Noise noise{91};
    AddScrape(s, 0.0f, 0.12f, 1500.0f, 0.8f, noise);
    AddClick(s, 0.12f, 1200.0f, 0.02f, 1.1f, noise);
    AddScrape(s, 0.2f, 0.1f, 1700.0f, 0.7f, noise);
    AddClick(s, 0.3f, 1500.0f, 0.02f, 1.2f, noise);
    Normalize(s, 0.5f);
    return Make(std::move(s));
}

Sound ShellIn() {
    std::vector<float> s = Samples(0.18f);
    Noise noise{93};
    AddScrape(s, 0.0f, 0.06f, 1800.0f, 0.6f, noise);
    AddClick(s, 0.06f, 1400.0f, 0.012f, 0.9f, noise);
    Normalize(s, 0.35f);
    return Make(std::move(s));
}

Sound RevolverReload() {
    std::vector<float> s = Samples(2.2f);
    Noise noise{95};
    AddClick(s, 0.0f, 2600.0f, 0.012f, 0.9f, noise);
    for (int i = 0; i < 6; ++i) AddClick(s, 0.32f + 0.035f * i + 0.01f * (i % 2), 4200.0f + 300.0f * i, 0.03f, 0.35f, noise);
    AddScrape(s, 1.0f, 0.2f, 2400.0f, 0.5f, noise);
    AddClick(s, 1.25f, 1900.0f, 0.015f, 0.8f, noise);
    AddClick(s, 1.85f, 2200.0f, 0.02f, 1.1f, noise);
    Normalize(s, 0.42f);
    return Make(std::move(s));
}

Sound Casing(bool shell) {
    std::vector<float> s = Samples(0.24f);
    Noise noise{shell ? 101u : 103u};
    LowPass lp(shell ? 1100.0f : 2200.0f), hp_ref(320.0f);
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        float v = 0.0f;
        for (const float at : {0.0f, 0.095f}) {
            if (t < at) continue;
            const float u = t - at;
            const float k = at == 0.0f ? 1.0f : 0.35f;
            const float n = noise();
            const float grit = lp(n) - hp_ref(n);
            v += k * grit * std::exp(-u / (shell ? 0.008f : 0.006f));
        }
        s[i] = v;
    }
    Normalize(s, 0.14f);
    return Make(std::move(s));
}

Sound BulletImpact() {
    std::vector<float> s = Samples(0.11f);
    Noise noise{211u};
    LowPass mid(2100.0f), sub(260.0f), dust(3600.0f);
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        const float crack = mid(n) * std::exp(-t / 0.0055f);
        const float thump = sub(n) * std::exp(-t / 0.018f) * 1.4f;
        const float grit = dust(n) * std::min(1.0f, t / 0.008f) * std::exp(-t / 0.028f) * 0.45f;
        s[i] = crack + thump + grit;
    }
    Normalize(s, 0.24f);
    return Make(std::move(s));
}

Sound ArmorDeflect() {
    std::vector<float> s = Samples(0.18f);
    Noise noise{223u};
    LowPass body_lp(1450.0f), low_lp(360.0f);
    float phase = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        phase += 2.0f * kPi * (240.0f + 120.0f * std::exp(-t / 0.012f)) / kRate;
        const float knock = std::sin(phase) * std::exp(-t / 0.024f) * 0.55f;
        const float scrape = body_lp(n) * std::exp(-t / 0.016f) * 0.9f;
        const float weight = low_lp(n) * std::exp(-t / 0.045f) * 0.8f;
        s[i] = knock + scrape + weight;
    }
    Normalize(s, 0.26f);
    return Make(std::move(s));
}

Sound GlassBreak() {
    std::vector<float> s = Samples(0.42f);
    Noise noise{239u};
    LowPass hi(6800.0f), cut(1400.0f);
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        const float shard = hi(n) - cut(n);
        float env = std::exp(-t / 0.022f);
        for (const float at : {0.035f, 0.075f, 0.13f, 0.19f, 0.26f}) {
            if (t >= at) env += 0.35f * std::exp(-(t - at) / 0.014f);
        }
        s[i] = shard * env + cut(n) * std::exp(-t / 0.012f) * 0.45f;
    }
    Normalize(s, 0.34f);
    return Make(std::move(s));
}

Sound BodyFall() {
    std::vector<float> s = Samples(0.32f);
    Noise noise{251u};
    LowPass cloth(340.0f), slap(950.0f), rumble(130.0f);
    float phase = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        const float hz = 42.0f + 50.0f * std::exp(-t / 0.026f);
        phase += 2.0f * kPi * hz / kRate;
        const float weight = std::sin(phase) * std::min(1.0f, t / 0.005f) * std::exp(-t / 0.062f) * 0.95f;
        const float fabric = cloth(n) * std::min(1.0f, t / 0.004f) * std::exp(-t / 0.055f) * 1.45f;
        const float u2 = t - 0.075f;
        const float settle = u2 > 0.0f ? (slap(n) * std::exp(-u2 / 0.018f) * 0.35f + rumble(n) * std::exp(-u2 / 0.045f) * 0.8f) : 0.0f;
        s[i] = weight + fabric + settle;
    }
    Normalize(s, 0.46f);
    return Make(std::move(s));
}

Sound HeavyThud() {
    std::vector<float> s = Samples(0.42f);
    Noise noise{263u};
    LowPass sub_n(180.0f), crunch(680.0f);
    float phase = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        const float hz = 32.0f + 44.0f * std::exp(-t / 0.032f);
        phase += 2.0f * kPi * hz / kRate;
        const float boom = std::sin(phase) * std::min(1.0f, t / 0.004f) * std::exp(-t / 0.095f) * 1.05f;
        const float grit = crunch(n) * std::exp(-t / 0.016f) * 0.65f;
        const float tail = sub_n(n) * std::min(1.0f, t / 0.01f) * std::exp(-t / 0.14f) * 1.3f;
        s[i] = boom + grit + tail;
    }
    Normalize(s, 0.56f);
    return Make(std::move(s));
}

Sound PickupLand() {
    std::vector<float> s = Samples(0.18f);
    Noise noise{271u};
    LowPass soft(520.0f), sub(190.0f);
    float phase = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        phase += 2.0f * kPi * (88.0f + 62.0f * std::exp(-t / 0.014f)) / kRate;
        const float thump = std::sin(phase) * std::exp(-t / 0.028f) * 0.7f;
        const float tap = soft(n) * std::exp(-t / 0.012f) * 0.9f + sub(n) * std::exp(-t / 0.04f) * 0.7f;
        const float u2 = t - 0.065f;
        const float bounce = u2 > 0.0f ? soft(n) * std::exp(-u2 / 0.010f) * 0.35f : 0.0f;
        s[i] = thump + tap + bounce;
    }
    Normalize(s, 0.30f);
    return Make(std::move(s));
}

Sound PickupTake() {
    std::vector<float> s = Samples(0.20f);
    Noise noise{281u};
    LowPass rustle(1600.0f), low_cut(280.0f), latch(950.0f);
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        const float cloth = (rustle(n) - low_cut(n)) * std::sin(kPi * std::clamp(t / 0.14f, 0.0f, 1.0f)) * 0.75f;
        const float u = t - 0.055f;
        const float click = u > 0.0f ? latch(n) * std::exp(-u / 0.008f) * 0.65f : 0.0f;
        s[i] = cloth + click;
    }
    Normalize(s, 0.30f);
    return Make(std::move(s));
}

Sound HealTake() {
    std::vector<float> s = Samples(0.55f);
    Noise noise{293u};
    LowPass air(900.0f), sub(220.0f);
    float p1 = 0.0f, p2 = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float n = noise();
        p1 += 2.0f * kPi * 330.0f / kRate;
        p2 += 2.0f * kPi * 495.0f / kRate;
        const float env = Smooth(t / 0.06f) * std::exp(-t / 0.22f);
        const float warm = (0.65f * std::sin(p1) + 0.35f * std::sin(p2)) * env * 0.5f;
        const float breath = (air(n) - sub(n)) * Smooth(t / 0.05f) * std::exp(-t / 0.14f) * 0.55f;
        s[i] = warm + breath;
    }
    Normalize(s, 0.32f);
    return Make(std::move(s));
}

Sound Swap() {
    std::vector<float> s = Samples(0.35f);
    Noise noise{42};
    AddScrape(s, 0.0f, 0.12f, 1600.0f, 0.5f, noise);
    AddClick(s, 0.14f, 1100.0f, 0.02f, 0.8f, noise);
    Normalize(s, 0.28f);
    return Make(std::move(s));
}

Sound Swing() {
    std::vector<float> s = Samples(0.32f);
    Noise noise{77};
    float lp1 = 0.0f, lp2 = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float k = std::sin(kPi * std::clamp(t / 0.3f, 0.0f, 1.0f));
        const float a = 1.0f - std::exp(-2.0f * kPi * (300.0f + 1500.0f * k) / kRate);
        lp1 += a * (noise() - lp1);
        lp2 += a * (lp1 - lp2);
        s[i] = (lp1 - lp2) * k * k;
    }
    Normalize(s, 0.35f);
    return Make(std::move(s));
}

Sound Drop() {
    std::vector<float> s = Samples(0.6f);
    Noise noise{61};
    AddClick(s, 0.0f, 520.0f, 0.05f, 1.0f, noise);
    AddClick(s, 0.09f, 610.0f, 0.04f, 0.5f, noise);
    Normalize(s, 0.3f);
    return Make(std::move(s));
}

Sound Siren() {
    std::vector<float> s = Samples(12.0f);
    LowPass muffle(2400.0f);
    float phase = 0.0f;
    const float held = 1.0f - std::exp(-3.5f / 1.1f);
    for (size_t i = 0; i < s.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float k = t < 3.5f ? 1.0f - std::exp(-t / 1.1f) : t < 6.0f ? held : held * std::exp(-(t - 6.0f) / 2.2f);
        phase += 2.0f * kPi * (140.0f + 420.0f * k) / kRate;
        const float tone = std::sin(phase) + 0.25f * std::sin(2.0f * phase) + 0.33f * std::sin(3.0f * phase) + 0.18f * std::sin(5.0f * phase);
        const float envelope = Smooth(t / 0.8f) * (t > 10.5f ? std::max(0.0f, 1.0f - (t - 10.5f) / 1.5f) : 1.0f);
        s[i] = muffle(tone) * envelope * (0.35f + 0.65f * k);
    }
    const std::vector<float> dry = s;
    for (const auto& [delay, gain] : {std::pair{0.31f, 0.35f}, std::pair{0.74f, 0.22f}, std::pair{1.3f, 0.12f}}) {
        const size_t lag = static_cast<size_t>(delay * kRate);
        for (size_t i = lag; i < s.size(); ++i) s[i] += gain * dry[i - lag];
    }
    Normalize(s, 0.5f);
    return Make(std::move(s));
}

Sound Tick(bool last) {
    std::vector<float> s = Samples(last ? 0.7f : 0.25f);
    Noise noise{last ? 81u : 82u};
    AddClick(s, 0.0f, last ? 880.0f : 1760.0f, last ? 0.12f : 0.03f, 1.0f, noise);
    if (last) AddClick(s, 0.0f, 440.0f, 0.18f, 0.7f, noise);
    Normalize(s, last ? 0.4f : 0.3f);
    return Make(std::move(s));
}

namespace {

std::vector<float> Mono(const audio::Media& media) {
    const std::vector<float> all = media.DecodeAll();
    const uint32_t channels = std::max(1u, media.Channels());
    std::vector<float> mono(all.size() / channels);
    for (size_t i = 0; i < mono.size(); ++i) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < channels; ++c) sum += all[i * channels + c];
        mono[i] = sum / static_cast<float>(channels);
    }
    if (media.SampleRate() == kRate || media.SampleRate() == 0 || mono.empty()) return mono;
    const double step = static_cast<double>(media.SampleRate()) / kRate;
    std::vector<float> out(static_cast<size_t>(static_cast<double>(mono.size()) / step));
    for (size_t i = 0; i < out.size(); ++i) {
        const double at = static_cast<double>(i) * step;
        const size_t a = std::min(mono.size() - 1, static_cast<size_t>(at));
        const size_t b = std::min(mono.size() - 1, a + 1);
        const float f = static_cast<float>(at - static_cast<double>(a));
        out[i] = mono[a] * (1.0f - f) + mono[b] * f;
    }
    return out;
}

std::vector<float> Cut(const std::vector<float>& x, float t0, float t1) {
    const size_t a = std::min(x.size(), static_cast<size_t>(t0 * kRate));
    const size_t b = std::min(x.size(), static_cast<size_t>(t1 * kRate));
    std::vector<float> s(x.begin() + static_cast<std::ptrdiff_t>(a), x.begin() + static_cast<std::ptrdiff_t>(b));
    const size_t fade = std::min(s.size() / 2, static_cast<size_t>(0.006f * kRate));
    for (size_t i = 0; i < fade; ++i) {
        const float k = static_cast<float>(i) / static_cast<float>(fade);
        s[i] *= k;
        s[s.size() - 1 - i] *= k;
    }
    return s;
}

std::vector<float> Join(std::vector<float> a, const std::vector<float>& b) {
    const size_t cross = std::min({a.size(), b.size(), static_cast<size_t>(0.008f * kRate)});
    for (size_t i = 0; i < cross; ++i) a[a.size() - cross + i] += b[i];
    a.insert(a.end(), b.begin() + static_cast<std::ptrdiff_t>(cross), b.end());
    return a;
}

float Peak(const std::vector<float>& s) {
    float m = 0.0f;
    for (float v : s) m = std::max(m, std::abs(v));
    return m;
}

struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    Biquad(float hz, bool high) {
        const float w = 2.0f * kPi * hz / static_cast<float>(kRate);
        const float alpha = std::sin(w) / (2.0f * 0.7071f);
        const float c = std::cos(w);
        const float a0 = 1.0f + alpha;
        b0 = (high ? (1.0f + c) : (1.0f - c)) / 2.0f / a0;
        b1 = (high ? -(1.0f + c) : (1.0f - c)) / a0;
        b2 = b0;
        a1 = -2.0f * c / a0;
        a2 = (1.0f - alpha) / a0;
    }
    float operator()(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

}

Sound RadioTakeOne(const audio::Media* riddle, const audio::Media* voice08, const audio::Media* noise, int variant) {
    if (!riddle || !voice08 || !noise) return nullptr;
    const std::vector<float> dj = Mono(*riddle);
    const std::vector<float> shout = Mono(*voice08);
    const std::vector<float> hiss = Mono(*noise);
    if (dj.size() < static_cast<size_t>(25.3f * kRate) || shout.size() < static_cast<size_t>(4.7f * kRate) || hiss.size() < kRate * 2) return nullptr;
    std::vector<float> take = Cut(dj, 3.975f, 4.222f);
    std::vector<float> one = variant == 2 ? Cut(shout, 4.31f, 4.61f)
                           : variant == 3 ? Cut(shout, 4.33f, 4.61f)
                                          : Join(Cut(dj, 22.236f, 22.302f), Cut(dj, 25.068f, 25.212f));
    const float level = Peak(take) / std::max(1e-6f, Peak(one));
    for (float& v : one) v *= level;
    std::vector<float> line = take;
    line.resize(line.size() + static_cast<size_t>(0.07f * kRate), 0.0f);
    line.insert(line.end(), one.begin(), one.end());
    Biquad h1(380.0f, true), h2(380.0f, true), l1(3100.0f, false), l2(3100.0f, false);
    for (float& v : line) v = l2(l1(h2(h1(v))));
    const float drive = 1.6f / std::max(1e-6f, Peak(line));
    Noise rng{2203u};
    for (size_t i = 0; i < line.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        float y = std::tanh(line[i] * drive) / std::tanh(1.6f);
        y *= 0.85f + 0.15f * std::sin(2.0f * kPi * 7.3f * t + 1.1f);
        if (t >= 0.43f && t < 0.455f) y *= 0.25f;
        line[i] = y;
    }
    for (int c = 0; c < 40; ++c) {
        const size_t at = static_cast<size_t>((rng() * 0.5f + 0.5f) * static_cast<float>(line.size() - 32));
        const float gain = 0.25f * rng();
        for (size_t k = 0; k < 30; ++k) line[at + k] += gain * 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(k) / 29.0f));
    }
    const size_t pre = static_cast<size_t>(0.55f * kRate), post = static_cast<size_t>(0.5f * kRate);
    std::vector<float> out(pre + line.size() + post, 0.0f);
    const float hiss_level = 0.6f / std::max(1e-6f, Peak(hiss));
    for (size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float in = Smooth(t / 0.15f) * (1.0f - Smooth((t - static_cast<float>(out.size()) / kRate + 0.25f) / 0.25f));
        const bool voice = i >= pre && i < pre + line.size();
        const float burst = t >= 0.37f && t < 0.55f ? 1.6f : 0.0f;
        const float bed = hiss[(static_cast<size_t>(kRate) + i) % hiss.size()] * hiss_level;
        out[i] = bed * (in * (voice ? 0.45f : 1.0f) * 0.5f + burst) + (voice ? line[i - pre] : 0.0f);
    }
    Normalize(out, 0.85f);
    LogInfo("survival: the radio's take one made from the riddle and Play_radio_voice_08 (variant {}, {:.2f} s)", variant,
            static_cast<float>(out.size()) / kRate);
    return Make(std::move(out));
}

}
