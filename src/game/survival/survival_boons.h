#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace pt::game {

enum class SurvivalBoon : uint8_t {
    QuickHands,
    DeepPockets,
    SteadyHands,
    Buckshot,
    HollowPoints,
    HeavyRounds,
    SecondWind,
    Bloodletting,
    LongLegs,
    SearingLight,
    StoppingPower,
    Scavenger,
    Headhunter,
    LampsGrace,
    FlarePouch,
    ColdBlood,
    LastBreath,
    QuickPump,
    FanTheHammer,
    IronGrip,
    Count
};

struct SurvivalBoonSpec {
    const char* name;
    int ranks;
};

const SurvivalBoonSpec& BoonSpec(SurvivalBoon boon);

struct SurvivalBoons {
    std::array<uint8_t, static_cast<size_t>(SurvivalBoon::Count)> rank{};

    int operator[](SurvivalBoon b) const { return rank[static_cast<size_t>(b)]; }
    bool Maxed(SurvivalBoon b) const { return rank[static_cast<size_t>(b)] >= BoonSpec(b).ranks; }
    int Taken() const;
    std::string Save() const;
    void Load(const std::string& text);
};

}
