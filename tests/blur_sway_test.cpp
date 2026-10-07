#include <cstdio>
#include <random>

#include "game/blur_sway.h"

namespace {

struct Run {
    int pulses = 0;
    int rolls = 0;
};

Run Walk(float speed, float tick, float seconds, unsigned seed) {
    pt::game::BlurSway sway;
    std::mt19937 rng(seed);
    Run run;
    auto roll = [&](int max, int min) {
        ++run.rolls;
        return min + static_cast<int>(rng() % static_cast<unsigned>(max - min));
    };
    glm::vec3 position(0.0f);
    const glm::vec3 axis(0.0f, 0.0f, 1.0f);
    sway.Update(tick, position, axis, roll);
    run = {};
    const int ticks = static_cast<int>(seconds / tick + 0.5f);
    for (int i = 0; i < ticks; ++i) {
        position.z += speed * tick;
        const bool was_active = sway.pulse_active;
        sway.Update(tick, position, axis, roll);
        run.pulses += !was_active && sway.pulse_active;
    }
    return run;
}

}

int main() {
    int failures = 0;
    const auto check = [&](const char* name, bool ok) {
        std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    const Run at60 = Walk(1.8f, 1.0f / 60.0f, 600.0f, 1);
    const Run at30 = Walk(1.8f, 1.0f / 30.0f, 600.0f, 1);
    std::printf("1.8 m/s for 600 s: 60 Hz %d pulses %d rolls, 30 Hz %d pulses %d rolls\n", at60.pulses, at60.rolls, at30.pulses, at30.rolls);
    check("a walk of 0.06 m per game frame rolls at 60 Hz", at60.rolls > 0);
    check("the roll runs once per game frame, not per tick", at60.rolls <= 600 * 30 && at30.rolls <= 600 * 30);
    check("pulse count at a 60 Hz tick within 15 percent of 30 Hz", at60.pulses > 0 && at30.pulses > 0 &&
          std::abs(at60.pulses - at30.pulses) * 100 <= 15 * at30.pulses);
    const Run slow = Walk(0.9f, 1.0f / 60.0f, 60.0f, 2);
    check("0.03 m per game frame does not roll", slow.rolls == 0);
    pt::game::BlurSway sway;
    int rolls = 0;
    auto roll = [&](int, int) { ++rolls; return 10000; };
    sway.Update(1.0f / 60.0f, glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 1.0f), roll);
    sway.Update(1.0f / 60.0f, glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 1.0f), roll);
    check("the first game frame compares with the stored camera", rolls == 1);
    for (int i = 0; i < 20; ++i) sway.Update(1.0f / 60.0f, glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 1.0f), roll);
    check("standing still does not roll", rolls == 1);
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
