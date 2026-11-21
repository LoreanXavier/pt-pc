#pragma once

#include <deque>

#include "engine/render/camera.h"

namespace pt::render {

inline constexpr double kOriginalFrameSeconds = 1001.0 / 30000.0;

class MotionWindow {
public:
    void Reset() { samples_.clear(); }

    Camera Advance(const Camera& previous, const Camera& camera, double interval, bool connected, double* span) {
        if (!connected) {
            samples_.clear();
        }
        if (samples_.empty()) {
            samples_.push_back({clock_, previous});
        } else {
            samples_.back().camera = previous;
        }
        clock_ += interval;
        samples_.push_back({clock_, camera});
        const double target = clock_ - kOriginalFrameSeconds;
        while (samples_.size() > 2 && samples_[1].time <= target) {
            samples_.pop_front();
        }
        if (samples_.front().time >= target) {
            *span = clock_ - samples_.front().time;
            return samples_.front().camera;
        }
        const Sample& a = samples_[0];
        const Sample& b = samples_[1];
        *span = kOriginalFrameSeconds;
        const double length = b.time - a.time;
        return length > 0.0 ? BlendCameras(a.camera, b.camera, static_cast<float>((target - a.time) / length)) : b.camera;
    }

private:
    struct Sample {
        double time;
        Camera camera;
    };
    std::deque<Sample> samples_;
    double clock_ = 0.0;
};

}
