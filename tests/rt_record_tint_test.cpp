#include <cmath>
#include <cstdint>
#include <cstdio>

#include "engine/render/rt_caster.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++g_failures;
    }
}

glm::vec3 DecodeTint(uint32_t flags) {
    return glm::vec3(static_cast<float>(((flags >> 12u) & 63u) ^ 32u),
                     static_cast<float>(((flags >> 18u) & 63u) ^ 32u),
                     static_cast<float>(((flags >> 24u) & 63u) ^ 32u)) /
           32.0f;
}

}

int main() {
    {
        pt::RtCaster c;
        Check(pt::RtRecordFlags(c) == 0u, "default caster flags are 0");
        const glm::vec3 decoded = DecodeTint(0u);
        Check(decoded == glm::vec3(1.0f), "zero flags decode to tint (1, 1, 1)");
    }
    {
        pt::RtCaster c;
        c.tint = glm::vec3(1.0f);
        c.alpha_test = true;
        c.light_fitting = 5;
        c.mirror_hidden = true;
        const uint32_t expected = 8u | (5u << 4) | 1024u;
        Check(pt::RtRecordFlags(c) == expected, "untinted caster preserves legacy flags");
    }
    {
        const float samples[] = {0.0f, 0.3f, 0.62f, 1.0f, 1.7f};
        for (float r : samples) {
            for (float g : samples) {
                for (float b : samples) {
                    pt::RtCaster c;
                    c.tint = glm::vec3(r, g, b);
                    const uint32_t flags = pt::RtRecordFlags(c);
                    Check((flags & 0xC0000FFFu) == 0u, "tint bits stay in bits 12..29");
                    const glm::vec3 dec = DecodeTint(flags);
                    Check(std::fabs(dec.r - r) <= 1.0f / 64.0f + 1e-6f, "red channel round trip");
                    Check(std::fabs(dec.g - g) <= 1.0f / 64.0f + 1e-6f, "green channel round trip");
                    Check(std::fabs(dec.b - b) <= 1.0f / 64.0f + 1e-6f, "blue channel round trip");
                }
            }
        }
    }
    {
        pt::RtCaster c;
        c.tint = glm::vec3(5.0f, -1.0f, 1.96875f);
        const glm::vec3 dec = DecodeTint(pt::RtRecordFlags(c));
        Check(std::fabs(dec.r - 1.96875f) < 1e-6f, "high clamp to 63/32");
        Check(std::fabs(dec.g - 0.0f) < 1e-6f, "low clamp to 0");
        Check(std::fabs(dec.b - 1.96875f) < 1e-6f, "max exact value");
    }
    std::printf("%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
