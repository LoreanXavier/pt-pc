
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/anim/sim_physics.h"
#include "engine/anim/sim_rigid.h"
#include "engine/anim/skeleton.h"
#include "engine/fs/vfs.h"

#ifndef PT_JACKET_SIM_FIXTURE
#define PT_JACKET_SIM_FIXTURE "tests/fixtures/jacket_sim_ps4.txt"
#endif

namespace {

using namespace pt;

struct Frame {
    int number = 0;
    float dt = 0.0f;
    glm::mat4 model{1.0f};
    std::map<int, glm::mat4> bones;
};

std::vector<Frame> ReadFixture(const char* path) {
    std::vector<Frame> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream s(line);
        Frame f;
        s >> f.number >> f.dt;
        float w[16];
        for (float& x : w) {
            s >> x;
        }
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                f.model[j][i] = w[i * 4 + j];
            }
        }
        std::string token;
        while (s >> token && token.size() > 1 && token[0] == 'b') {
            float v[7];
            for (float& x : v) {
                s >> x;
            }
            f.bones[std::atoi(token.c_str() + 1)] =
                glm::translate(glm::mat4(1.0f), glm::vec3(v[4], v[5], v[6])) * glm::mat4_cast(glm::normalize(glm::quat(v[0], v[1], v[2], v[3])));
        }
        out.push_back(std::move(f));
    }
    return out;
}

float TurnDegrees(const glm::mat4& parent, const glm::mat4& child) {
    const glm::mat3 r = glm::transpose(glm::mat3(parent)) * glm::mat3(child);
    const float c = std::clamp((r[0][0] + r[1][1] + r[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
    return glm::degrees(std::acos(c));
}

}

int main(int argc, char** argv) {
    int failures = 0;
    const auto check = [&](const std::string& name, bool ok) {
        std::printf("%s: %s\n", name.c_str(), ok ? "PASS" : "FAIL");
        failures += ok ? 0 : 1;
    };
    Vfs vfs;
    if (argc != 2 || !vfs.Mount(argv[1]) || !vfs.LoadPackage("/Assets/sh/level/promotion/pt_2014/ending/ending.fpk") ||
        !vfs.LoadPackage("/Assets/sh/level/promotion/pt_2014/ending/ending.fpkd")) {
        std::printf("usage: pt_sim_jacket_test <game folder>\n");
        return 2;
    }
    anim::Skeleton skeleton;
    anim::SimRig rig;
    auto fmdl = vfs.ReadFile("/Assets/sh/chara/plr/Scenes/plr0_main0_def.fmdl");
    auto sim = vfs.ReadFile("/Assets/sh/chara/plr/Fox_Files/plr0_main0_def_s00.sim");
    std::string error;
    check("plr0 skeleton and simulation load",
          fmdl && sim && anim::ReadFmdlSkeleton(*fmdl, skeleton) && rig.Load("plr0_main0_def_s00.sim", *sim, &error));
    const std::vector<Frame> frames = ReadFixture(PT_JACKET_SIM_FIXTURE);
    check("fixture holds 105 frames", frames.size() == 105);
    if (failures) {
        return 1;
    }
    const size_t n = skeleton.Size();
    std::vector<bool> simulated(n, false), below(n, false);
    for (const anim::SimBody& b : rig.Bodies()) {
        if (b.dynamic) {
            if (const int i = skeleton.FindName(b.bone); i >= 0) {
                simulated[static_cast<size_t>(i)] = true;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const int p = skeleton.parents[i];
        below[i] = simulated[i] || (p >= 0 && below[static_cast<size_t>(p)]);
    }
    const bool verbose = std::getenv("PT_SIM_TEST_VERBOSE") != nullptr;
    const char* only = std::getenv("PT_JACKET_BONE");

    anim::SimRigid solver;
    solver.SetDelay(-1.0f);
    std::vector<glm::mat4> world(n, glm::mat4(1.0f));
    struct Stat {
        double sum[2] = {0, 0}, square[2] = {0, 0}, error = 0;
        int count = 0;
    };
    std::map<int, Stat> stats;
    for (size_t k = 0; k < frames.size(); ++k) {
        const Frame& f = frames[k];
        for (size_t i = 0; i < n; ++i) {
            const int p = skeleton.parents[i];
            auto it = f.bones.find(static_cast<int>(i));
            if (!below[i] && it != f.bones.end()) {
                world[i] = it->second;
            } else if (p >= 0) {
                world[i] = world[static_cast<size_t>(p)] * glm::translate(glm::mat4(1.0f), skeleton.bind_local[i]);
            } else {
                world[i] = glm::translate(glm::mat4(1.0f), skeleton.bind_world[i]);
            }
        }
        solver.Step(rig, skeleton, world, f.model, k == 0 ? -1.0f : f.dt);
        if (k < 20) {
            continue;
        }
        for (size_t i = 0; i < n; ++i) {
            const int p = skeleton.parents[i];
            if (!simulated[i] || p < 0) {
                continue;
            }
            auto ps = f.bones.find(static_cast<int>(i));
            auto pp = f.bones.find(p);
            if (ps == f.bones.end() || pp == f.bones.end()) {
                continue;
            }
            const float mine = TurnDegrees(world[static_cast<size_t>(p)], world[i]);
            const float theirs = TurnDegrees(pp->second, ps->second);
            if (only && skeleton.names[i] == only) {
                std::printf("frame %d %s port %6.2f ps4 %6.2f\n", f.number, only, mine, theirs);
            }
            Stat& s = stats[static_cast<int>(i)];
            s.sum[0] += mine;
            s.sum[1] += theirs;
            s.square[0] += mine * mine;
            s.square[1] += theirs * theirs;
            s.error += (mine - theirs) * (mine - theirs);
            ++s.count;
        }
    }
    double total = 0, strips = 0;
    int count = 0, strip_count = 0;
    for (const auto& [bone, s] : stats) {
        const std::string& name = skeleton.names[static_cast<size_t>(bone)];
        if (verbose) {
            const double m0 = s.sum[0] / s.count, m1 = s.sum[1] / s.count;
            std::printf("%-24s port %6.2f +- %5.2f  ps4 %6.2f +- %5.2f  rms %5.2f\n", name.c_str(), m0,
                        std::sqrt(std::max(0.0, s.square[0] / s.count - m0 * m0)), m1, std::sqrt(std::max(0.0, s.square[1] / s.count - m1 * m1)),
                        std::sqrt(s.error / s.count));
        }
        total += s.error;
        count += s.count;
        if (name.find("JAKET") != std::string::npos) {
            strips += s.error;
            strip_count += s.count;
        }
    }
    const double rms = std::sqrt(total / std::max(count, 1));
    const double strip_rms = std::sqrt(strips / std::max(strip_count, 1));
    std::printf("replay: %zu simulated bones, rms %.2f degrees over %d samples, jacket strips %.2f\n", stats.size(), rms, count, strip_rms);
    check("replay follows the original (rms under 8.5 degrees, jacket strips under 6.5)", rms < 8.5 && strip_rms < 6.5);
    return failures ? 1 : 0;
}
