#pragma once

#include <cstdint>
#include <array>
#include <deque>
#include <memory>
#include <vector>

namespace pt::audio {

struct FxObject;

constexpr uint32_t kOutputRate = 48000;

float DbToGain(float db);
float FastPow10(float x);
float FastGainToDb(float gain);
float GainToDb(float gain);
float LpfToCutoffHz(float lpf);
float OnePoleCoefficient(float cutoff_hz);
void ButterworthLowPass(float cutoff_hz, float* coef);

struct Biquad {
    enum class Type { LowPass, HighPass, BandPass, Notch, Peaking, LowShelf, HighShelf };

    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;

    void Set(Type type, float freq, float q, float gain_db);
    float Process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void Reset() { z1 = z2 = 0.0f; }
};

class DelayLine {
public:
    void Resize(size_t length);
    size_t Length() const { return buffer_.size(); }
    float Read(size_t delay) const {
        size_t index = pos_ + buffer_.size() - delay;
        if (index >= buffer_.size()) {
            index -= buffer_.size();
        }
        return buffer_[index];
    }
    void Write(float value) {
        buffer_[pos_] = value;
        if (++pos_ == buffer_.size()) {
            pos_ = 0;
        }
    }
    void Clear();

private:
    std::vector<float> buffer_;
    size_t pos_ = 0;
};

class Effect {
public:
    virtual ~Effect() = default;
    virtual void Process(float* left, float* right, uint32_t frames) = 0;
    virtual void ProcessSurround(float* left, float* right, std::array<float*, 8>& speakers, uint32_t frames) {
        Process(left, right, frames);
    }
    virtual float TailSeconds() const = 0;
    virtual void Reset() = 0;
    virtual void MuteDry() {}
    virtual bool MonoInput() const { return false; }
    virtual bool FrontInput() const { return false; }
    virtual bool SumInput() const { return false; }
    virtual void SetDetector(const float*) {}
};

std::unique_ptr<Effect> CreateEffect(const FxObject& fx);

}
