#include "game/survival/survival_profile.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <sstream>
#include <vector>

namespace pt::game {

namespace {

std::vector<std::string> Split(const std::string& s, char by) {
    std::vector<std::string> out;
    std::string part;
    std::istringstream in(s);
    while (std::getline(in, part, by)) out.push_back(part);
    return out;
}

int Int(const std::string& s) {
    return std::atoi(s.c_str());
}

std::string SaveCheckpoint(const SurvivalCheckpoint& c) {
    if (c.wave <= 0) return {};
    std::string w;
    for (size_t i = 0; i < c.weapons.size(); ++i) {
        w += std::format("{}{}:{}:{}", i ? "/" : "", c.weapons[i][0], c.weapons[i][1], c.weapons[i][2]);
    }
    return std::format("{},{},{:.0f},{},{},{},{},{},{},{},{},{:.0f}", c.wave, w, c.max_health, c.boons.empty() ? "-" : c.boons, c.flares, c.score,
                       c.kills, c.headshots, c.shots, c.hits, c.best_chain, c.run_time);
}

SurvivalCheckpoint LoadCheckpoint(const std::string& s) {
    SurvivalCheckpoint c;
    const std::vector<std::string> f = Split(s, ',');
    if (f.size() < 12) return c;
    c.wave = Int(f[0]);
    const std::vector<std::string> weapons = Split(f[1], '/');
    for (size_t i = 0; i < weapons.size() && i < c.weapons.size(); ++i) {
        const std::vector<std::string> v = Split(weapons[i], ':');
        for (size_t k = 0; k < v.size() && k < 3; ++k) c.weapons[i][k] = Int(v[k]);
    }
    c.max_health = static_cast<float>(std::atof(f[2].c_str()));
    c.boons = f[3] == "-" ? std::string() : f[3];
    c.flares = Int(f[4]);
    c.score = Int(f[5]);
    c.kills = Int(f[6]);
    c.headshots = Int(f[7]);
    c.shots = Int(f[8]);
    c.hits = Int(f[9]);
    c.best_chain = Int(f[10]);
    c.run_time = static_cast<float>(std::atof(f[11].c_str()));
    if (c.wave < 2 || c.max_health < 1.0f) c = SurvivalCheckpoint{};
    return c;
}

}

std::string SurvivalProfile::Save() const {
    std::string s = std::format(
        "score={}:{}:{}:{}:{};daily={}:{}:{};dwins={}:{}:{};unlocks={};finish={};torch={};credits={};skins={:x};torches={};wskins={}:{}:{}:{}:{}:{};reloads={};wreloads={}:{}:{}:{}:{}:{}",
        best_score[0], best_score[1], best_score[2], best_score[3], best_score[4], daily_date, daily_wave, daily_score, daily_wins, daily_streak,
        daily_last_win_date, unlocks, finish, torch, credits, static_cast<unsigned long long>(owned_skins | 1ull), owned_torches | 1u,
        weapon_skins[0], weapon_skins[1], weapon_skins[2], weapon_skins[3], weapon_skins[4], weapon_skins[5], owned_reloads | 1u,
        weapon_reloads[0], weapon_reloads[1], weapon_reloads[2], weapon_reloads[3], weapon_reloads[4], weapon_reloads[5]);
    for (size_t d = 0; d < checkpoints.size(); ++d) {
        if (checkpoints[d].wave > 0) s += std::format(";cp{}={}", d, SaveCheckpoint(checkpoints[d]));
    }
    return s;
}

void SurvivalProfile::Load(const std::string& text) {
    *this = SurvivalProfile{};
    bool had_wskins = false;
    for (const std::string& item : Split(text, ';')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
        if (key == "score") {
            const std::vector<std::string> v = Split(value, ':');
            for (size_t i = 0; i < v.size() && i < best_score.size(); ++i) best_score[i] = Int(v[i]);
        } else if (key == "daily") {
            const std::vector<std::string> v = Split(value, ':');
            if (v.size() >= 3) {
                daily_date = Int(v[0]);
                daily_wave = Int(v[1]);
                daily_score = Int(v[2]);
            }
        } else if (key == "dwins") {
            const std::vector<std::string> v = Split(value, ':');
            if (v.size() >= 3) {
                daily_wins = std::max(0, Int(v[0]));
                daily_streak = std::max(0, Int(v[1]));
                daily_last_win_date = std::max(0, Int(v[2]));
            }
        } else if (key == "unlocks") {
            unlocks = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "finish") {
            finish = Int(value);
        } else if (key == "torch") {
            torch = std::clamp(Int(value), 0, kSurvivalTorchCount - 1);
        } else if (key == "credits") {
            credits = std::max(0, Int(value));
        } else if (key == "skins") {
            owned_skins = static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 16)) | 1ull;
        } else if (key == "torches") {
            owned_torches = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10)) | 1u;
        } else if (key == "wskins") {
            had_wskins = true;
            const std::vector<std::string> v = Split(value, ':');
            for (size_t i = 0; i < v.size() && i < weapon_skins.size(); ++i) {
                weapon_skins[i] = std::clamp(Int(v[i]), 0, kSurvivalSkinCount - 1);
            }
        } else if (key == "reloads") {
            owned_reloads = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10)) | 1u;
        } else if (key == "wreloads") {
            const std::vector<std::string> v = Split(value, ':');
            for (size_t i = 0; i < v.size() && i < weapon_reloads.size(); ++i) {
                weapon_reloads[i] = std::clamp(Int(v[i]), 0, 4);
            }
        } else if (key.size() == 3 && key.compare(0, 2, "cp") == 0) {
            const int d = key[2] - '0';
            if (d >= 0 && d < static_cast<int>(checkpoints.size())) checkpoints[static_cast<size_t>(d)] = LoadCheckpoint(value);
        }
    }
    owned_skins |= 1ull;
    owned_torches |= 1u;
    if (unlocks & kUnlockBlued) GrantSkin(1);
    if (unlocks & kUnlockNickel) GrantSkin(2);
    if (unlocks & kUnlockGold) GrantSkin(28);
    if (unlocks & kUnlockAmber) GrantTorch(1);
    if (unlocks & kUnlockRed) GrantTorch(2);
    if (!had_wskins && finish > 0) {
        const int mapped = finish == 1 ? 1 : finish == 2 ? 2 : finish == 3 ? 28 : 0;
        if (mapped > 0 && OwnsSkin(mapped)) {
            for (size_t i = 1; i < weapon_skins.size(); ++i) weapon_skins[i] = mapped;
        }
    }
}

int SurvivalToday() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

int SurvivalYesterday(int yyyymmdd) {
    if (yyyymmdd < 19700102) return 0;
    std::tm t{};
    t.tm_year = yyyymmdd / 10000 - 1900;
    t.tm_mon = (yyyymmdd / 100) % 100 - 1;
    t.tm_mday = (yyyymmdd % 100) - 1;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    const std::time_t prev = std::mktime(&t);
    if (prev == static_cast<std::time_t>(-1)) return yyyymmdd - 1;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &prev);
#else
    localtime_r(&prev, &local);
#endif
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

}
