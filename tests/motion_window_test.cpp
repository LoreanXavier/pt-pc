#include "engine/render/motion_window.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;

void Check(bool ok, const char* what, double got = 0.0, double want = 0.0) {
    std::printf("%s %s (%.5f, expected %.5f)\n", ok ? "PASS" : "FAIL", what, got, want);
    failures += !ok;
}

pt::Camera At(double t, double rate, double speed) {
    pt::Camera c;
    c.yaw = static_cast<float>(rate * t);
    c.position = glm::vec3(0.0f, 1.6f, static_cast<float>(-speed * t));
    return c;
}
}

int main() {
    using pt::render::kOriginalFrameSeconds;
    using pt::render::MotionWindow;
    const double rate = glm::radians(170.0);
    const double speed = 1.5;
    for (const double fps : {29.97, 60.0, 144.0, 240.0, 1000.0}) {
        MotionWindow window;
        double span = 0.0;
        pt::Camera from;
        double t = 0.0;
        for (int i = 0; i < 400; ++i) {
            const pt::Camera previous = At(t, rate, speed);
            t += 1.0 / fps;
            from = window.Advance(previous, At(t, rate, speed), 1.0 / fps, i > 0, &span);
        }
        const pt::Camera now = At(t, rate, speed);
        char what[160];
        std::snprintf(what, sizeof(what), "%.2f frames a second: the window is one original frame", fps);
        Check(std::abs(span - kOriginalFrameSeconds) < 1.0e-9, what, span, kOriginalFrameSeconds);
        std::snprintf(what, sizeof(what), "%.2f frames a second: the turn over it is the original frame's", fps);
        Check(std::abs((now.yaw - from.yaw) - rate * kOriginalFrameSeconds) < 2.0e-4, what, now.yaw - from.yaw, rate * kOriginalFrameSeconds);
        std::snprintf(what, sizeof(what), "%.2f frames a second: the walk over it is the original frame's", fps);
        Check(std::abs((from.position.z - now.position.z) - speed * kOriginalFrameSeconds) < 2.0e-4, what, from.position.z - now.position.z,
              speed * kOriginalFrameSeconds);
    }
    {
        MotionWindow window;
        double span = 0.0;
        pt::Camera from;
        double t = 0.0;
        for (int i = 0; i < 20; ++i) {
            const pt::Camera previous = At(t, rate, speed);
            t += 0.05;
            from = window.Advance(previous, At(t, rate, speed), 0.05, i > 0, &span);
        }
        Check(std::abs(span - kOriginalFrameSeconds) < 1.0e-9, "20 frames a second: the window is one original frame", span, kOriginalFrameSeconds);
        Check(std::abs((At(t, rate, speed).yaw - from.yaw) - rate * kOriginalFrameSeconds) < 2.0e-4, "20 frames a second: the turn over it is the original frame's",
              At(t, rate, speed).yaw - from.yaw, rate * kOriginalFrameSeconds);
    }
    {
        MotionWindow window;
        double span = 0.0;
        double t = 0.0;
        for (int i = 0; i < 50; ++i) {
            const pt::Camera previous = At(t, rate, speed);
            t += 1.0 / 240.0;
            window.Advance(previous, At(t, rate, speed), 1.0 / 240.0, i > 0, &span);
        }
        const pt::Camera jumped = At(t + 100.0, rate, speed);
        pt::Camera from = window.Advance(jumped, jumped, 1.0 / 240.0, false, &span);
        Check(std::abs(span - 1.0 / 240.0) < 1.0e-9 && from.yaw == jumped.yaw && from.position == jumped.position,
              "a cut: the window starts again, with no movement across the cut", span, 1.0 / 240.0);
        from = window.Advance(jumped, At(t + 100.0 + 1.0 / 240.0, rate, speed), 1.0 / 240.0, true, &span);
        Check(std::abs(span - 2.0 / 240.0) < 1.0e-9 && from.yaw == jumped.yaw, "the frame after a cut: two frames of movement", span, 2.0 / 240.0);
    }
    {
        MotionWindow window;
        double span = 0.0;
        pt::Camera camera;
        double low = 1.0e9, high = 0.0;
        for (int i = 0; i < 400; ++i) {
            const pt::Camera previous = camera;
            if (i % 2 == 0) {
                camera.yaw += static_cast<float>(rate * 2.0 / 240.0);
            }
            const pt::Camera from = window.Advance(previous, camera, 1.0 / 240.0, i > 0, &span);
            if (i > 40) {
                low = std::min(low, static_cast<double>(camera.yaw - from.yaw));
                high = std::max(high, static_cast<double>(camera.yaw - from.yaw));
            }
        }
        const double steady = rate * kOriginalFrameSeconds;
        Check(low > 0.74 * steady && high < 1.26 * steady, "uneven mouse movement: the window's turn stays near the steady one", high / steady, 1.0);
    }
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
