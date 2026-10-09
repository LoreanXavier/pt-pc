// DominantLightSearch's change over time (0x8D12A0): small changes are taken at once, changes over 22.5 degrees turn with w 1
// and changes over 45 degrees fade out and in, each over 1/4 s of game time.
#include <cmath>
#include <cstdio>

#include "engine/render/dominant_light.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what, const glm::vec4& v) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s: (%.4f %.4f %.4f %.4f)\n", what, v.x, v.y, v.z, v.w);
    }
}

bool Near(const glm::vec4& a, const glm::vec4& b) {
    return glm::length(a - b) < 1.0e-3f;
}

glm::vec3 Dir(float degrees) {
    const float r = glm::radians(degrees);
    return glm::vec3(std::sin(r), 0.0f, std::cos(r));
}

}

int main() {
    const float frame = 1.0f / 30.0f;
    {
        // the first result after the default (0, -1, 0) is 90 degrees away: a fade
        pt::DominantLightState s;
        glm::vec4 v = s.Step(Dir(0.0f), frame);
        Expect(Near(v, glm::vec4(0.0f, -1.0f, 0.0f, 1.0f - 2.0f * 4.0f * frame)), "fade starts from the old direction", v);
        for (int i = 0; i < 3; ++i) {
            v = s.Step(Dir(0.0f), frame);
        }
        Expect(Near(v, glm::vec4(Dir(0.0f), 2.0f * (16.0f * frame - 0.5f))), "fade ends in the new direction", v);
        for (int i = 0; i < 4; ++i) {
            v = s.Step(Dir(0.0f), frame);
        }
        Expect(Near(v, glm::vec4(Dir(0.0f), 1.0f)), "fade settles", v);
        v = s.Step(Dir(10.0f), frame);
        Expect(Near(v, glm::vec4(Dir(10.0f), 1.0f)), "a 10 degree change is taken at once", v);
        v = s.Step(Dir(40.0f), frame);
        Expect(Near(v, glm::vec4(Dir(10.0f + 30.0f * 4.0f * frame), 1.0f)), "a 30 degree change turns", v);
        for (int i = 0; i < 8; ++i) {
            v = s.Step(Dir(40.0f), frame);
        }
        Expect(Near(v, glm::vec4(Dir(40.0f), 1.0f)), "the turn ends", v);
        v = s.Step(Dir(100.0f), frame);
        Expect(Near(v, glm::vec4(Dir(40.0f), 1.0f - 8.0f * frame)), "a 60 degree change fades", v);
    }
    {
        // a render long after the last one: the whole change happens in that step
        pt::DominantLightState s;
        const glm::vec4 v = s.Step(Dir(0.0f), 2.0f);
        Expect(Near(v, glm::vec4(Dir(0.0f), 1.0f)), "a long gap settles at once", v);
    }
    if (failures == 0) {
        std::printf("dominant light: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
