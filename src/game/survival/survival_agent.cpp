#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sstream>
#include <string>
#include <thread>

#include "engine/core/log.h"
#include "engine/physics/collision_world.h"
#include "engine/platform/input.h"
#include "engine/render/camera.h"
#include "game/game.h"
#include "game/player.h"
#include "game/survival/survival.h"

namespace pt::game {

namespace {

struct AgentLink {
    std::FILE* state = nullptr;
    std::FILE* commands = nullptr;
    int every = 6;
    int tick = 0;
    uint64_t serial = 0;
    glm::vec2 stick{0.0f};
    uint32_t held = 0;
    float yaw = NAN;
    float pitch = NAN;
};

std::string Vec(const glm::vec3& v) {
    return std::format("[{:.3f},{:.3f},{:.3f}]", v.x, v.y, v.z);
}

}

std::string SurvivalNight::AgentState(uint64_t serial) const {
    Player& player = game_.GetPlayer();
    const Camera camera = game_.ThirdPersonCameraActive() ? game_.GetCamera() : game_.ViewCamera(true);
    const WeaponState& gun = weapons_[static_cast<size_t>(weapon_)];
    std::string s = std::format("{{\"s\":{},\"t\":{:.3f},\"ph\":{},\"w\":{},\"left\":{},\"cd\":{:.2f},\"wt\":{:.2f},\"hp\":{:.2f},\"maxhp\":{:.1f},\"st\":{:.3f},\"winded\":{},",
                                serial, time_, static_cast<int>(phase_), wave_, hud_.enemies_left, countdown_, wave_time_, health_, max_health_, stamina_, winded_ ? 1 : 0);
    s += std::format("\"dead\":{},\"deadt\":{:.2f},\"retry\":{},\"caught\":{},\"wpn\":{},\"clip\":{},\"res\":{},\"rel\":{},\"swap\":{:.2f},\"shot\":{:.3f},\"aim\":{:.2f},",
                     hud_.dead ? 1 : 0, hud_.dead_time, hud_.can_retry ? 1 : 0, caught_ ? 1 : 0, static_cast<int>(weapon_), gun.clip, gun.reserve,
                     reload_timer_ > 0.0f ? 1 : 0, swap_, shot_timer_, aim_);
    s += std::format("\"sights\":{},\"spr\":{},\"burst\":{:.2f},\"flares\":{},\"pipe\":{},\"kills\":{},\"score\":{},\"ev\":{},\"blk\":{:.2f},\"fog\":{:.2f},\"fear\":{:.2f},",
                     sights_ ? 1 : 0, sprinting_ ? 1 : 0, burst_timer_, flares_, pipe_hits_, kills_, score_, events_, blackout_time_, fog_level_, hud_.fear);
    s += std::format("\"night\":{},\"mid\":{:.2f},\"north\":{:.1f},\"lampsout\":{},\"tospawn\":{},\"boss\":{},\"feet\":{},\"eye\":{},\"yaw\":{:.4f},\"pitch\":{:.4f},\"foxyaw\":{:.4f},\"tp\":{},\"own\":[",
                     SurvivalNightOf(std::max(1, wave_)), midnight_, north_end_, lamps_out_, to_spawn_, hud_.boss ? 1 : 0, Vec(player.Feet()), Vec(camera.position),
                     player.yaw, player.pitch, player.FoxYaw(), game_.ThirdPersonCameraActive() ? 1 : 0);
    for (size_t i = 0; i < weapons_.size(); ++i) {
        s += std::format("{}[{},{},{}]", i ? "," : "", weapons_[i].owned ? 1 : 0, weapons_[i].clip, weapons_[i].reserve);
    }
    s += "],\"en\":[";
    bool first = true;
    for (const Enemy& e : enemies_) {
        const glm::vec3 chest = e.position + glm::vec3(0.0f, e.kind == SurvivalEnemyKind::Crawler ? 0.4f : 1.1f, 0.0f) - camera.position;
        const float away = glm::length(chest);
        RayHit wall;
        const bool seen = away < 0.1f || !game_.Collision().Raycast(camera.position, chest / away, away - 0.2f, wall);
        s += std::format("{}[{},{},{},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.1f},{:.1f},{},{},{},{}]", first ? "" : ",", e.id, static_cast<int>(e.kind),
                         static_cast<int>(e.state), e.position.x, e.position.y, e.position.z, e.head.x, e.head.y, e.head.z, e.health, e.max_health, e.affix,
                         e.perched ? 1 : 0, e.latched ? 1 : 0, seen ? 1 : 0);
        first = false;
    }
    s += "],\"pk\":[";
    first = true;
    for (const Pickup& p : pickups_) {
        s += std::format("{}[{},{},{},{:.2f},{:.2f},{:.2f},{},{:.1f}]", first ? "" : ",", p.id, static_cast<int>(p.kind), static_cast<int>(p.weapon), p.position.x,
                         p.position.y, p.position.z, p.settled ? 1 : 0, p.life - p.age);
        first = false;
    }
    s += std::format("],\"task\":{{\"k\":{},\"done\":{},\"lost\":{},\"hold\":{:.2f},\"tuned\":{},\"spoken\":{:.2f},\"house\":{},\"age\":{:.2f},\"glass\":{:.2f},\"rlift\":{:.2f},\"face\":{},\"use\":\"{}\",\"prog\":{:.2f},\"key\":\"{}\",\"n\":{},\"need\":{},\"spots\":[",
                     static_cast<int>(task_.kind), task_.done ? 1 : 0, task_.lost ? 1 : 0, task_.hold, task_.tuned ? 1 : 0, task_.spoken,
                     task_.at_house ? 1 : 0, task_.age, LanternGlass(), RadioLift(), Vec(task_.facing), hud_.use_key, hud_.use_progress,
                     hud_.task_key, hud_.task_count, hud_.task_need);
    for (size_t i = 0; i < task_.spots.size(); ++i) {
        s += std::format("{}[{:.2f},{:.2f},{:.2f},{}]", i ? "," : "", task_.spots[i].x, task_.spots[i].y, task_.spots[i].z,
                         i < task_.taken.size() && task_.taken[i] ? 1 : 0);
    }
    const SurvivalProfile& prof = game_.SurvivalProfileData();
    s += std::format("]}},\"diff\":{},\"cause\":{},\"hs\":{},\"skin\":{},\"torch\":{},\"dark\":{{\"cnt\":{},\"t\":{:.2f},\"dist\":{:.2f}}},\"lamps\":[",
                     static_cast<int>(difficulty_), hud_.cause, headshots_, prof.EquippedSkin(static_cast<int>(weapon_)), prof.EquippedTorch(),
                     hud_.dark_count, dark_time_, ToTheLight(player.Feet()));
    const std::vector<glm::vec2> lamps = LitLamps();
    for (size_t i = 0; i < lamps.size(); ++i) {
        s += std::format("{}[{:.2f},{:.2f}]", i ? "," : "", lamps[i].x, lamps[i].y);
    }
    s += "],\"flares_lit\":[";
    for (size_t i = 0; i < flares_out_.size(); ++i) {
        const BurningFlare& f = flares_out_[i];
        s += std::format("{}[{:.2f},{:.2f},{:.2f},{:.2f},{}]", i ? "," : "", f.position.x, f.position.y, f.position.z, f.life - f.age,
                         f.landed ? 1 : 0);
    }
    s += std::format("],\"sup\":{{\"on\":{},\"at\":{}}},\"offers\":[", supply_on_ ? 1 : 0, Vec(supply_at_));
    for (int i = 0; i < offer_count_; ++i) {
        s += std::format("{}{}", i ? "," : "", static_cast<int>(offer_[static_cast<size_t>(i)]));
    }
    s += std::format("],\"offer\":{},\"banner\":\"{}\",\"omen\":\"{}\",\"notice\":\"{}\"}}\n", offer_count_, hud_.banner > 0.0f ? hud_.banner_key : "",
                     hud_.omen > 0.0f ? hud_.omen_key : "", hud_.notice_age >= 0.0f ? hud_.notice_key : "");
    return s;
}

void SurvivalNight::AgentInput(InputState& input) {
    static const char* const folder = std::getenv("PT_SURVIVAL_AGENT");
    if (!folder) return;
    static AgentLink link;
    if (!link.state) {
        link.state = std::fopen(std::format("{}/state.jsonl", folder).c_str(), "ab");
        link.commands = std::fopen(std::format("{}/commands.txt", folder).c_str(), "rb");
        if (const char* every = std::getenv("PT_SURVIVAL_AGENT_EVERY")) link.every = std::max(1, std::atoi(every));
        if (!link.state || !link.commands) {
            LogWarn("survival agent: no link in {}", folder);
            return;
        }
        LogInfo("survival agent: linked in {}, every {} ticks", folder, link.every);
    }
    if (!link.commands) return;
    uint32_t pressed = 0;
    if (link.tick++ % link.every == 0) {
        const std::string line = AgentState(++link.serial);
        std::fwrite(line.data(), 1, line.size(), link.state);
        std::fflush(link.state);
        const auto since = std::chrono::steady_clock::now();
        char text[4096];
        for (;;) {
            std::clearerr(link.commands);
            const long at = std::ftell(link.commands);
            if (std::fgets(text, sizeof text, link.commands) && std::strchr(text, '\n')) {
                std::istringstream words(text);
                uint64_t serial = 0;
                std::string yaw, pitch, steps;
                glm::vec2 stick{0.0f};
                uint32_t held = 0;
                words >> serial >> stick.x >> stick.y >> yaw >> pitch >> held >> pressed >> steps;
                if (serial < link.serial) continue;
                link.stick = stick;
                link.held = held;
                link.yaw = yaw == "-" ? NAN : std::strtof(yaw.c_str(), nullptr);
                link.pitch = pitch == "-" ? NAN : std::strtof(pitch.c_str(), nullptr);
                while (steps != "-" && !steps.empty()) {
                    const size_t semi = steps.find(';');
                    const std::string step = steps.substr(0, semi);
                    steps = semi == std::string::npos ? "" : steps.substr(semi + 1);
                    if (step.rfind("shot=", 0) == 0) {
                        game_.RequestScreenshot(step.substr(5));
                    } else if (step == "quit") {
                        game_.RequestQuit();
                    } else if (step == "mortal") {
                        unhurt_ = false;
                    } else if (step.rfind("map=", 0) == 0) {
                        DumpWays(step.substr(4));
                    } else if (step.rfind("probe=", 0) == 0) {
                        glm::vec3 from, dir;
                        float length = 2.0f;
                        std::sscanf(step.c_str() + 6, "%f,%f,%f,%f,%f,%f,%f", &from.x, &from.y, &from.z, &dir.x, &dir.y, &dir.z, &length);
                        RayHit any, front;
                        const bool hit = game_.Collision().Raycast(from, dir, length, any);
                        const bool hit_front = game_.Collision().Raycast(from, dir, length, front, true);
                        LogInfo("survival agent: probe ({:.2f} {:.2f} {:.2f}) along ({:.2f} {:.2f} {:.2f}): {} at {:.2f} ({:.2f} {:.2f} {:.2f}) normal ({:.2f} {:.2f} {:.2f}) triangle {}, front {} at {:.2f}",
                                from.x, from.y, from.z, dir.x, dir.y, dir.z, hit ? "hit" : "clear", any.distance, any.point.x, any.point.y, any.point.z, any.normal.x,
                                any.normal.y, any.normal.z, any.triangle, hit_front ? "hit" : "clear", front.distance);
                    } else if (step.rfind("skin=", 0) == 0) {
                        int w = 0, id = 0;
                        if (std::sscanf(step.c_str() + 5, "%d,%d", &w, &id) == 2 && id >= 0 && id < kSurvivalSkinCount) {
                            SurvivalProfile& prof = game_.SurvivalProfileData();
                            prof.GrantSkin(id);
                            if (w == 0) {
                                for (size_t i = 1; i < prof.weapon_skins.size(); ++i) prof.weapon_skins[i] = id;
                            } else if (w >= 1 && w < static_cast<int>(prof.weapon_skins.size())) {
                                prof.weapon_skins[static_cast<size_t>(w)] = id;
                            }
                            ApplyLooks(true);
                        }
                    } else if (step.rfind("torch=", 0) == 0) {
                        const int id = std::clamp(std::atoi(step.c_str() + 6), 0, kSurvivalTorchCount - 1);
                        SurvivalProfile& prof = game_.SurvivalProfileData();
                        prof.GrantTorch(id);
                        prof.torch = id;
                        ApplyLooks(true);
                    } else {
                        Act(step);
                    }
                }
                break;
            }
            std::fseek(link.commands, at, SEEK_SET);
            if (std::chrono::steady_clock::now() - since > std::chrono::seconds(60)) {
                LogWarn("survival agent: no answer to {}, playing on", link.serial);
                link.stick = glm::vec2(0.0f);
                link.held = 0;
                break;
            }
            std::this_thread::yield();
        }
    }
    input.left_stick = link.stick;
    input.left_stick_from_pad = true;
    input.survival_held = link.held;
    input.survival_pressed = pressed;
    Player& player = game_.GetPlayer();
    if (!std::isnan(link.yaw)) player.yaw = player.target_yaw = link.yaw;
    if (!std::isnan(link.pitch)) player.pitch = player.target_pitch = Player::ClampPitch(link.pitch);
}

}
