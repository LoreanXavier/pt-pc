#pragma once

#include <array>
#include <cstdint>

namespace pt::game {

enum class SurvivalMission : uint8_t { None, Fuse, Radio, Fragments, Lamps, Door };
enum class SurvivalBoss : uint8_t { None, LongDark, Final };
enum SurvivalAffix : uint8_t {
    kAffixSplitter = 1u,
    kAffixArmored = 2u,
    kAffixShy = 4u,
    kAffixScreamer = 8u,
    kAffixPerched = 16u,
};

struct SurvivalWavePlan {
    int wave = 1;
    int chapter = 0;
    int count = 5;
    int cap = 4;
    float interval = 1.8f;
    std::array<float, 4> weights{1.0f, 0.0f, 0.0f, 0.0f};
    int giants = 0;
    bool giant_first = false;
    bool lisa_last = false;
    float lisa_chance = 0.0f;
    uint32_t events = 0;
    bool more_events = true;
    const char* theme = nullptr;
    bool breather = false;
    SurvivalMission mission = SurvivalMission::None;
    SurvivalBoss boss = SurvivalBoss::None;
    bool boon = false;
    bool checkpoint = false;
    bool giant_boss = false;
    float affix_share = 0.0f;
    uint8_t affixes = 0;
    float health = 1.0f;
    float speed = 1.0f;
    float damage = 1.0f;
};

SurvivalWavePlan PlanSurvivalWave(int wave, uint32_t seed);

uint8_t SurvivalAffixesBy(int wave);

inline constexpr int kSurvivalLaterWaves = 25;
int SurvivalNightOf(int wave);
int SurvivalWaveInNight(int wave);
bool SurvivalNightEnds(int wave);
inline constexpr int kSurvivalLamps = 5;
int SurvivalLampsOut(int wave);

struct SurvivalWeaponUnlock {
    int after_wave;
    int weapon_index;
};
inline constexpr std::array<SurvivalWeaponUnlock, 4> kSurvivalWeaponUnlocks = {{
    {2, 2},
    {4, 3},
    {30, 4},
    {57, 5},
}};

}
