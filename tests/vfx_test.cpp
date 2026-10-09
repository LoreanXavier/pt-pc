#include <cmath>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>

#include "engine/vfx/vfx_system.h"

#include <algorithm>

namespace {

using QuadSignature = std::array<float, 48>;

std::vector<QuadSignature> Signatures(const pt::vfx::RenderList& list) {
    std::vector<QuadSignature> result;
    result.reserve(list.quads.size());
    for (const pt::vfx::Quad& quad : list.quads) {
        QuadSignature signature{};
        size_t at = 0;
        auto append = [&](const glm::vec4& value) {
            for (size_t i = 0; i < 4; ++i) {
                signature[at++] = value[i];
            }
        };
        for (const glm::vec4& corner : quad.corner) {
            append(corner);
        }
        append(quad.uv);
        append(quad.uv_next);
        append(quad.color);
        append(quad.params);
        append(quad.luminance);
        append(quad.extra);
        signature[at++] = static_cast<float>(quad.info.x);
        signature[at++] = static_cast<float>(quad.info.y);
        signature[at++] = static_cast<float>(quad.info.z);
        signature[at++] = static_cast<float>(quad.info.w);
        append(quad.rain_rotation);
        result.push_back(signature);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<QuadSignature> RenderSmokeSeeds(const std::vector<uint8_t>& bytes,
                                            const std::array<std::optional<uint32_t>, 3>& seeds) {
    pt::vfx::System system;
    system.SetReader([&](const std::string&) -> std::optional<std::vector<uint8_t>> { return bytes; });
    for (size_t i = 0; i < seeds.size(); ++i) {
        if (!system.Spawn({7, i + 1}, "smoke-seeds.vfx", glm::mat4(1.0f), seeds[i])) {
            return {};
        }
    }
    pt::vfx::ViewInfo view;
    view.forward = glm::normalize(glm::vec3(1.0f));
    for (int tick = 0; tick < 2; ++tick) {
        system.Update(1.0f / 60.0f, view);
    }
    pt::vfx::RenderList list;
    std::vector<pt::vfx::LightOut> lights;
    system.Build(view, [](const std::string&) { return 55u; }, {}, {}, list, lights);
    return Signatures(list);
}

} // namespace

// The extracted original fx_sh_lgthnd01_s5.vfx is supplied by the caller, never bundled with the test.
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: pt_vfx_test <fx_sh_lgthnd01_s5.vfx> [fx_sh_smkgnd03_s5.vfx]\n");
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        return 2;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    pt::vfx::System system;
    system.SetReader([&](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        return path == "handlight.vfx" ? std::optional(bytes) : std::nullopt;
    });
    if (!system.Spawn({1, 1}, "handlight.vfx", glm::mat4(1.0f), 1)) {
        return 2;
    }
    pt::vfx::ViewInfo view;
    view.forward = glm::normalize(glm::vec3(1.0f));
    system.Update(1.0f / 60.0f, view);
    system.Update(1.0f / 60.0f, view);
    pt::vfx::RenderList list;
    std::vector<pt::vfx::LightOut> lights;
    system.Build(view, [](const std::string& path) {
        return path.find("fx_smkcom03_iy_alp") != std::string::npos ? 52u :
               path.find("fx_flrlgt01_iy_alp") != std::string::npos ? 51u : 53u;
    }, {}, {}, list, lights);
    int rain = 0;
    int failures = 0;
    for (const auto& q : list.quads) {
        if (q.info.x != 51) {
            continue;
        }
        ++rain;
        // Original B71F40/B72510/B72840: two screen-space layers, scale (2,2),
        // offset starts at zero and subtracts 0.1 per second, wrapping into [0,1), here after two ticks.
        if ((q.info.y & (1u << 10)) == 0 || q.info.z != 52 ||
            std::abs(q.luminance.x - 2.0f) > 1e-5f || std::abs(q.luminance.y - 2.0f) > 1e-5f ||
            std::abs(q.luminance.z - 0.99666667f) > 1e-5f || std::abs(q.luminance.w - 0.99666667f) > 1e-5f) {
            ++failures;
        }
        if ((q.info.y & 1u) == 0 || std::abs(q.params.x - 1.0f / 0.3f) > 1e-5f) {
            ++failures;
        }
        if (std::abs(glm::dot(glm::vec2(q.rain_rotation), glm::vec2(q.rain_rotation)) - 1.0f) > 1e-5f ||
            std::abs(glm::dot(glm::vec2(q.rain_rotation.z, q.rain_rotation.w), glm::vec2(q.rain_rotation.z, q.rain_rotation.w)) - 1.0f) > 1e-5f) {
            ++failures;
        }
        // B72840 pairs the fields as angle/swing, angle/swing. This asset starts at +20 and -20 degrees.
        if (q.rain_rotation.x < 0.3f || q.rain_rotation.z > -0.3f) {
            ++failures;
        }
    }
    if (rain != 3) {
        ++failures;
    }
    pt::vfx::RenderList repeated;
    system.Build(view, [](const std::string& path) {
        return path.find("fx_smkcom03_iy_alp") != std::string::npos ? 52u :
               path.find("fx_flrlgt01_iy_alp") != std::string::npos ? 51u : 53u;
    }, {}, {}, repeated, lights);
    if (repeated.quads.size() != list.quads.size()) {
        ++failures;
    } else {
        for (size_t i = 0; i < list.quads.size(); ++i) {
            if (repeated.quads[i].rain_rotation != list.quads[i].rain_rotation ||
                repeated.quads[i].luminance != list.quads[i].luminance) {
                ++failures;
            }
        }
    }
    std::printf("handlight: %d rain planes, %d failures\n", rain, failures);
    if (argc == 3) {
        std::ifstream smoke_input(argv[2], std::ios::binary);
        if (!smoke_input) {
            return 2;
        }
        const std::vector<uint8_t> smoke_bytes((std::istreambuf_iterator<char>(smoke_input)), {});
        pt::vfx::System smoke;
        smoke.SetReader([&](const std::string&) -> std::optional<std::vector<uint8_t>> { return smoke_bytes; });
        if (!smoke.Spawn({2, 1}, "smoke.vfx", glm::mat4(1.0f), 1)) {
            return 2;
        }
        for (int tick = 0; tick < 600; ++tick) {
            smoke.Update(1.0f / 60.0f, view);
        }
        pt::vfx::RenderList ambient, directional;
        const auto texture = [](const std::string&) { return 55u; };
        smoke.Build(view, texture, {}, [](const glm::vec3&, bool) {
            return pt::vfx::LightingSample{glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(0.0f)};
        }, ambient, lights);
        smoke.Build(view, texture, {}, [](const glm::vec3&, bool) {
            return pt::vfx::LightingSample{glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(4.0f)};
        }, directional, lights);
        int checked = 0, smoke_failures = 0;
        if (ambient.quads.size() != directional.quads.size()) {
            ++smoke_failures;
        } else {
            for (size_t i = 0; i < ambient.quads.size(); ++i) {
                const glm::vec3 base(ambient.quads[i].color);
                if (glm::length(base) > 1e-5f) {
                    ++checked;
                    // The original smoke asset has ambientRate=1 and directionalLightRate=0.25.
                    if (glm::length(glm::vec3(directional.quads[i].color) - 2.0f * base) > 1e-5f) {
                        ++smoke_failures;
                    }
                }
            }
        }
        if (!checked) {
            ++smoke_failures;
        }
        std::printf("smoke directional: %d quads checked, %d failures\n", checked, smoke_failures);
        failures += smoke_failures;

        const auto counter_seeds = RenderSmokeSeeds(smoke_bytes, {std::nullopt, 42u, std::nullopt});
        const auto explicit_seeds = RenderSmokeSeeds(smoke_bytes, {0u, 42u, 2u});
        int seed_failures = counter_seeds.empty() || counter_seeds != explicit_seeds ? 1 : 0;
        std::printf("smoke creation seeds: %zu quads, %d failures\n", counter_seeds.size(), seed_failures);
        failures += seed_failures;
    }
    return failures ? 1 : 0;
}
