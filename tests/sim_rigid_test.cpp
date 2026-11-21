
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/anim/sim_physics.h"
#include "engine/anim/sim_rigid.h"
#include "engine/anim/skeleton.h"
#include "engine/fs/vfs.h"

#ifndef PT_BABY_SIM_FIXTURE
#define PT_BABY_SIM_FIXTURE "tests/fixtures/baby_sim_ps4.txt"
#endif

namespace {

using namespace pt;

struct Frame {
    int number = 0;
    float dt = 0.0f;
    glm::mat4 model{1.0f};
    glm::mat4 bones[5];
};

glm::mat4 FromRows(const float* r, int rows) {
    glm::mat4 m(1.0f);
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[j][i] = r[i * 4 + j];
        }
    }
    return m;
}

std::vector<Frame> ReadFixture(const char* path) {
    std::vector<Frame> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream s(line);
        std::vector<float> v;
        float x;
        while (s >> x) {
            v.push_back(x);
        }
        if (v.size() != 2 + 16 + 5 * 12) {
            continue;
        }
        Frame f;
        f.number = static_cast<int>(v[0]);
        f.dt = v[1];
        f.model = FromRows(&v[2], 4);
        for (int b = 0; b < 5; ++b) {
            f.bones[b] = FromRows(&v[18 + b * 12], 3);
        }
        out.push_back(f);
    }
    return out;
}

float TurnDegrees(const glm::mat4& parent, const glm::mat4& child) {
    const glm::mat3 r = glm::transpose(glm::mat3(parent)) * glm::mat3(child);
    const float c = std::clamp((r[0][0] + r[1][1] + r[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
    return glm::degrees(std::acos(c));
}

glm::quat RotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        r[c] = glm::normalize(r[c]);
    }
    return glm::normalize(glm::quat_cast(r));
}

}

int main(int argc, char** argv) {
    int failures = 0;
    const auto check = [&](const std::string& name, bool ok) {
        std::printf("%s: %s\n", name.c_str(), ok ? "PASS" : "FAIL");
        failures += ok ? 0 : 1;
    };
    Vfs vfs;
    if (argc != 2 || !vfs.Mount(argv[1]) || !vfs.LoadPackage("/Assets/sh/level/common/resident.fpk") ||
        !vfs.LoadPackage("/Assets/sh/level/common/resident.fpkd")) {
        std::printf("usage: pt_sim_rigid_test <game folder>\n");
        return 2;
    }
    anim::Skeleton skeleton;
    anim::SimRig rig;
    auto fmdl = vfs.ReadFile("/Assets/sh/chara/bab/Scenes/bab0_main0_def.fmdl");
    auto sim = vfs.ReadFile("/Assets/sh/chara/bab/Fox_Files/bab0_main0_def_s00.sim");
    std::string error;
    check("bab0 skeleton and simulation load", fmdl && sim && anim::ReadFmdlSkeleton(*fmdl, skeleton) &&
                                                   rig.Load("bab0_main0_def_s00.sim", *sim, &error));
    if (failures) {
        return 1;
    }
    const char* names[5] = {"SKL_000_ROOT", "SKL_005_HEAD", "SKL_600_WAIST_SIM", "SKL_602_HEADB_SIM", "SKL_603_HEADF_SIM"};
    int bone[5];
    for (int b = 0; b < 5; ++b) {
        bone[b] = skeleton.FindName(names[b]);
    }
    const int parent_of_sim[3] = {0, 1, 1};
    int body[3] = {-1, -1, -1};
    for (size_t i = 0; i < rig.Bodies().size(); ++i) {
        for (int s = 0; s < 3; ++s) {
            if (rig.Bodies()[i].bone == names[2 + s]) {
                body[s] = static_cast<int>(i);
            }
        }
    }
    std::vector<glm::mat4> bind(skeleton.Size());
    for (size_t i = 0; i < skeleton.Size(); ++i) {
        bind[i] = glm::translate(glm::mat4(1.0f), skeleton.bind_world[i]);
    }

    const std::vector<Frame> frames = ReadFixture(PT_BABY_SIM_FIXTURE);
    check("fixture holds 61 frames", frames.size() == 61);
    if (frames.size() == 61) {
        anim::SimRigid solver;
        solver.SetDelay(-1.0f);
        std::vector<glm::mat4> world = bind;
        auto pose = [&](const Frame& f) {
            world = bind;
            world[static_cast<size_t>(bone[0])] = f.bones[0];
            world[static_cast<size_t>(bone[1])] = f.bones[1];
        };
        pose(frames[0]);
        solver.Step(rig, skeleton, world, frames[0].model, -1.0f);
        const float first_dt = frames[0].dt;
        for (int s = 0; s < 3; ++s) {
            const anim::SimBody& b = rig.Bodies()[static_cast<size_t>(body[s])];
            const glm::mat4 offset = glm::translate(glm::mat4(1.0f), b.offset_position) * glm::mat4_cast(b.offset_rotation);
            const glm::mat4 now = frames[1].model * frames[1].bones[2 + s] * offset;
            const glm::mat4 before = frames[0].model * frames[0].bones[2 + s] * offset;
            const glm::quat q = RotationOf(now);
            glm::quat d = q * glm::inverse(RotationOf(before));
            if (d.w < 0.0f) {
                d = -d;
            }
            const float sine = glm::length(glm::vec3(d.x, d.y, d.z));
            const glm::vec3 spin = sine > 1e-7f ? glm::vec3(d.x, d.y, d.z) * (2.0f * std::atan2(sine, d.w) / sine / first_dt) : glm::vec3(0.0f);
            solver.SetBody(static_cast<size_t>(body[s]), glm::vec3(now[3]), q, (glm::vec3(now[3]) - glm::vec3(before[3])) / first_dt, spin);
        }
        double square[3] = {0, 0, 0};
        float worst[3] = {0, 0, 0};
        std::vector<float> cones[3][2];
        std::vector<float> twists[3][2];
        int count = 0;
        for (size_t k = 1; k < frames.size(); ++k) {
            pose(frames[k]);
            solver.Step(rig, skeleton, world, frames[k].model, frames[k].dt);
            if (k < 2) {
                continue;
            }
            for (int s = 0; s < 3; ++s) {
                const glm::mat4& parent = frames[k].bones[parent_of_sim[s]];
                const float mine = TurnDegrees(world[static_cast<size_t>(bone[parent_of_sim[s]])], world[static_cast<size_t>(bone[2 + s])]);
                const float theirs = TurnDegrees(parent, frames[k].bones[2 + s]);
                square[s] += (mine - theirs) * (mine - theirs);
                worst[s] = std::max(worst[s], std::abs(mine - theirs));
                if (std::getenv("PT_SIM_TEST_VERBOSE")) {
                    const anim::SimBody& simulated = rig.Bodies()[static_cast<size_t>(body[s])];
                    const glm::vec3 ref = glm::normalize(rig.Constraints()[static_cast<size_t>(simulated.constraint)].ref_b);
                    auto split = [&](const glm::mat4& p, const glm::mat4& c, float& cone, float& twist) {
                        const glm::mat3 r = glm::transpose(glm::mat3(p)) * glm::mat3(c);
                        const glm::vec3 d = r * glm::vec3(0.0f, -1.0f, 0.0f);
                        cone = glm::degrees(std::acos(std::clamp(glm::dot(d, ref), -1.0f, 1.0f)));
                        const glm::vec3 a0(0.0f, -1.0f, 0.0f);
                        auto arc = [](const glm::vec3& a, const glm::vec3& b) {
                            const glm::vec3 v = glm::cross(a, b);
                            return glm::normalize(glm::quat(1.0f + glm::dot(a, b), v.x, v.y, v.z));
                        };
                        const glm::vec3 axis = glm::normalize(d);
                        const glm::quat zero = arc(ref, axis) * arc(a0, ref);
                        const glm::quat q = glm::quat_cast(r) * glm::inverse(zero);
                        twist = glm::degrees(2.0f * std::atan2(glm::dot(glm::vec3(q.x, q.y, q.z), axis), q.w));
                    };
                    float mc = 0.0f, mt = 0.0f, tc = 0.0f, tt = 0.0f;
                    split(world[static_cast<size_t>(bone[parent_of_sim[s]])], world[static_cast<size_t>(bone[2 + s])], mc, mt);
                    split(parent, frames[k].bones[2 + s], tc, tt);
                    cones[s][0].push_back(mc);
                    cones[s][1].push_back(tc);
                    twists[s][0].push_back(mt);
                    twists[s][1].push_back(tt);
                    const glm::vec3 dm = glm::transpose(glm::mat3(world[static_cast<size_t>(bone[parent_of_sim[s]])])) *
                                         (glm::mat3(world[static_cast<size_t>(bone[2 + s])]) * glm::vec3(0.0f, -1.0f, 0.0f));
                    const glm::vec3 dt = glm::transpose(glm::mat3(parent)) * (glm::mat3(frames[k].bones[2 + s]) * glm::vec3(0.0f, -1.0f, 0.0f));
                    const glm::vec3 down = glm::normalize(glm::transpose(glm::mat3(parent)) *
                                                          (glm::transpose(glm::mat3(frames[k].model)) * glm::vec3(0.0f, -1.0f, 0.0f)));
                    const float gap = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(dm), glm::normalize(dt)), -1.0f, 1.0f)));
                    const float down_mine = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(dm), down), -1.0f, 1.0f)));
                    const float down_theirs = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(dt), down), -1.0f, 1.0f)));
                    std::printf("frame %d %s port %.2f ps4 %.2f cone %.2f %.2f twist %.2f %.2f axis gap %.2f from down %.2f %.2f\n", frames[k].number,
                                names[2 + s], mine, theirs, mc, tc, mt, tt, gap, down_mine, down_theirs);
                }
            }
            ++count;
        }
        auto spread = [](const std::vector<float>& v, float& mean, float& deviation) {
            mean = 0.0f;
            for (float x : v) mean += x;
            mean /= std::max<size_t>(v.size(), 1);
            deviation = 0.0f;
            for (float x : v) deviation += (x - mean) * (x - mean);
            deviation = std::sqrt(deviation / std::max<size_t>(v.size(), 1));
        };
        for (int s = 0; s < 3 && std::getenv("PT_SIM_TEST_VERBOSE"); ++s) {
            float a, b, c, d, e, f, g, h;
            spread(cones[s][0], a, b);
            spread(cones[s][1], c, d);
            spread(twists[s][0], e, f);
            spread(twists[s][1], g, h);
            std::printf("spread %s cone port %.2f +- %.2f ps4 %.2f +- %.2f twist port %.2f +- %.2f ps4 %.2f +- %.2f\n", names[2 + s], a, b, c, d,
                        e, f, g, h);
        }
        for (int s = 0; s < 3; ++s) {
            const double rms = std::sqrt(square[s] / std::max(count, 1));
            std::printf("replay %s: rms %.2f degrees, worst %.2f\n", names[2 + s], rms, worst[s]);
            check(std::string("replay ") + names[2 + s] + " follows the original (rms under 0.75 degrees)", rms < 0.75);
        }
    }

    {
        const glm::mat4 model = frames.empty() ? glm::mat4(1.0f) : frames[0].model;
        anim::SimRigid now_solver;
        anim::SimRigid late_solver;
        late_solver.SetDelay(1.0f / 29.97f);
        std::vector<std::vector<glm::mat4>> drawn;
        float gap = 0.0f;
        float lag = 0.0f;
        for (int i = 0; i < 240; ++i) {
            const float t = static_cast<float>(i) / 60.0f;
            std::vector<glm::mat4> a = bind;
            const size_t root = static_cast<size_t>(bone[0]);
            const size_t head = static_cast<size_t>(bone[1]);
            a[root] = glm::translate(glm::mat4(1.0f), skeleton.bind_world[root]) *
                      glm::rotate(glm::mat4(1.0f), glm::radians(10.0f) * std::sin(t * 2.1f * glm::two_pi<float>()), glm::vec3(0.0f, 0.0f, 1.0f));
            a[head] = glm::translate(glm::mat4(1.0f), skeleton.bind_world[head]) *
                      glm::rotate(glm::mat4(1.0f), glm::radians(20.0f) * std::sin(t * 1.5f * glm::two_pi<float>()), glm::vec3(1.0f, 0.0f, 0.0f));
            std::vector<glm::mat4> b = a;
            now_solver.Step(rig, skeleton, a, model, i == 0 ? -1.0f : 1.0f / 60.0f);
            late_solver.Step(rig, skeleton, b, model, i == 0 ? -1.0f : 1.0f / 60.0f);
            drawn.push_back(a);
            for (int s = 0; s < 3 && i >= 2; ++s) {
                const size_t k = static_cast<size_t>(bone[2 + s]);
                gap = std::max(gap, TurnDegrees(b[k], drawn[static_cast<size_t>(i - 2)][k]));
                gap = std::max(gap, 100.0f * glm::length(glm::vec3(b[k][3]) - glm::vec3(drawn[static_cast<size_t>(i - 2)][k][3])));
                lag = std::max(lag, TurnDegrees(b[k], a[k]));
            }
        }
        std::printf("delay at 60 fps: %.3f degrees from the bodies of two frames before, %.2f degrees from the bodies of the frame\n", gap, lag);
        check("drawn one frame of the original behind at 60 fps", gap < 0.05f && lag > 0.5f);
    }

    const glm::mat4 model = frames.empty() ? glm::mat4(1.0f) : frames[0].model;
    float rest[4][3] = {};
    const float rates[4] = {30.0f, 60.0f, 144.0f, 240.0f};
    for (int r = 0; r < 4; ++r) {
        anim::SimRigid solver;
        std::vector<glm::mat4> world = bind;
        solver.Step(rig, skeleton, world, model, -1.0f);
        const int steps = static_cast<int>(rates[r] * 20.0f);
        for (int i = 0; i < steps; ++i) {
            world = bind;
            solver.Step(rig, skeleton, world, model, 1.0f / rates[r]);
        }
        for (int s = 0; s < 3; ++s) {
            rest[r][s] = TurnDegrees(world[static_cast<size_t>(bone[parent_of_sim[s]])], world[static_cast<size_t>(bone[2 + s])]);
        }
        std::printf("rest at %.0f fps: %.2f %.2f %.2f degrees\n", rates[r], rest[r][0], rest[r][1], rest[r][2]);
    }
    for (int r = 0; r < 4; ++r) {
        bool same = true;
        for (int s = 0; s < 3; ++s) {
            same = same && std::abs(rest[r][s] - rest[1][s]) < 0.5f;
        }
        check(std::format("rest at {:.0f} fps matches 60 fps within half a degree", rates[r]), same);
    }
    const float most[3] = {43.2f + 35.0f, 18.0f + 35.0f, 14.4f + 35.0f};
    for (int s = 0; s < 3; ++s) {
        check(std::string("rest of ") + names[2 + s] + " inside its cone", rest[1][s] <= most[s] + 1.0f);
    }
    return failures ? 1 : 0;
}
