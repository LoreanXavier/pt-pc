#include "game/survival/survival_waves.h"

#include <algorithm>
#include <cmath>

#include "game/survival/survival.h"

namespace pt::game {

namespace {

float Hash(uint32_t seed, int wave, uint32_t salt) {
    uint32_t x = seed * 0x9E3779B1u ^ static_cast<uint32_t>(wave) * 0x85EBCA77u ^ salt * 0xC2B2AE3Du;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<float>(x >> 8) / 16777216.0f;
}

SurvivalWavePlan PlanLaterWave(int wave, uint32_t seed);

}

uint8_t SurvivalAffixesBy(int wave) {
    uint8_t a = 0;
    if (wave >= 16) a |= kAffixSplitter;
    if (wave >= 19) a |= kAffixArmored;
    if (wave >= 22) a |= kAffixShy;
    if (wave >= 26) a |= kAffixPerched;
    if (wave >= 33) a |= kAffixScreamer;
    return a;
}

SurvivalWavePlan PlanSurvivalWave(int wave, uint32_t seed) {
    SurvivalWavePlan p;
    if (wave > kSurvivalWaves) return PlanLaterWave(wave, seed);
    wave = std::max(wave, 1);
    p.wave = wave;
    p.chapter = (wave - 1) / 10;
    const float w = static_cast<float>(wave);
    p.count = std::min(60, static_cast<int>(std::lround(5.0f + 1.7f * (w - 1.0f))));
    p.cap = wave <= 8 ? 4 + wave : std::min(12 + (wave - 8) / 2, 14 + wave / 20);
    p.interval = std::max(0.55f, 1.8f - 0.055f * w);
    p.health = 1.0f + 0.05f * std::min(w - 1.0f, 19.0f) + 0.02f * std::max(0.0f, w - 20.0f);
    p.speed = 1.0f + 0.01f * std::min(w - 1.0f, 24.0f) + (p.chapter >= 8 ? 0.05f : 0.0f);
    p.damage = 1.0f + 0.008f * std::max(0.0f, w - 20.0f);
    p.weights = {1.0f, wave >= 2 ? 0.25f + 0.025f * std::min(w, 30.0f) : 0.0f, wave >= 3 ? 0.1f + 0.012f * std::min(w, 30.0f) : 0.0f,
                 wave >= 6 ? 0.08f + 0.01f * std::min(w, 30.0f) : 0.0f};
    p.giants = wave >= 6 ? std::min(4, 1 + (wave - 6) / 8) : 0;
    p.giant_first = wave >= 12 && wave % 3 == 0;
    p.lisa_last = wave % 5 == 0;
    p.lisa_chance = wave > 12 ? (wave >= 50 ? 0.06f : 0.04f) : 0.0f;
    p.boon = wave % 3 == 0;
    p.checkpoint = wave % 10 == 0 && wave < kSurvivalWaves;
    p.giant_boss = wave == 25 || wave == 75;
    if (p.giant_boss) p.more_events = false;
    p.affixes = SurvivalAffixesBy(wave);
    p.affix_share = wave < 16 ? 0.0f : wave <= 30 ? 0.12f + 0.01f * (w - 16.0f) : wave <= 60 ? 0.3f : 0.3f + 0.005f * (w - 60.0f);
    if (wave % 10 == 0) p.events |= kEventSiren;
    if (wave >= 80) p.events |= kEventHell;
    if (wave > 10 && wave % 10 == 1) {
        p.breather = true;
        p.count = static_cast<int>(std::lround(p.count * 0.65f));
        p.more_events = false;
        p.theme = "pc_survival_theme_breather";
    }
    auto debut = [&](uint8_t affix, const char* theme) {
        p.affixes = affix;
        p.affix_share = 0.35f;
        p.more_events = false;
        p.theme = theme;
    };
    switch (wave) {
    case 3: p.events |= kEventBlackout; p.more_events = false; break;
    case 6: p.mission = SurvivalMission::Radio; p.more_events = false; break;
    case 7:
        p.weights = {0.25f, 1.0f, 0.0f, 0.0f};
        p.count = static_cast<int>(std::lround(p.count * 1.3f));
        p.interval *= 0.7f;
        p.more_events = false;
        p.theme = "pc_survival_theme_crawl";
        break;
    case 8: p.events |= kEventFog; p.more_events = false; break;
    case 12: p.mission = SurvivalMission::Fuse; p.events |= kEventBlackout; p.more_events = false; break;
    case 13:
        p.weights = {0.6f, 0.0f, 1.0f, 0.0f};
        p.giants = 0;
        p.count = static_cast<int>(std::lround(p.count * 0.8f));
        p.more_events = false;
        p.theme = "pc_survival_theme_procession";
        break;
    case 14: p.events |= kEventFlank; p.more_events = false; break;
    case 15: p.events |= kEventHorde; p.more_events = false; break;
    case 18: p.mission = SurvivalMission::Fragments; p.more_events = false; break;
    case 29: p.mission = SurvivalMission::Lamps; p.events |= kEventHell; p.more_events = false; break;
    case 36: p.mission = SurvivalMission::Door; p.more_events = false; break;
    case 16: debut(kAffixSplitter, "pc_survival_theme_splitter"); break;
    case 17:
        p.weights = {0.5f, 0.2f, 0.0f, 1.0f};
        p.more_events = false;
        p.theme = "pc_survival_theme_mothers";
        break;
    case 19: debut(kAffixArmored, "pc_survival_theme_armored"); break;
    case 20:
        p.giants = 3;
        p.giant_first = true;
        p.weights = {1.0f, 0.3f, 0.0f, 0.0f};
        p.more_events = false;
        p.theme = "pc_survival_theme_giants";
        break;
    case 22: debut(kAffixShy, "pc_survival_theme_shy"); break;
    case 24:
        p.giants = 3;
        p.giant_first = true;
        p.weights = {1.0f, 0.2f, 0.1f, 0.0f};
        p.more_events = false;
        p.theme = "pc_survival_theme_giants";
        break;
    case 23: p.events |= kEventDrought; p.more_events = false; break;
    case 26: debut(kAffixPerched, "pc_survival_theme_perched"); break;
    case 27:
        p.weights = {0.2f, 1.0f, 0.0f, 0.0f};
        p.count = static_cast<int>(std::lround(p.count * 1.3f));
        p.interval *= 0.7f;
        p.theme = "pc_survival_theme_crawl";
        break;
    case 28:
        p.count = static_cast<int>(std::lround(p.count * 0.45f));
        p.events |= kEventFog | kEventBlackout;
        p.affixes = kAffixShy;
        p.affix_share = 1.0f;
        p.giants = 0;
        p.more_events = false;
        p.theme = "pc_survival_theme_quiet";
        break;
    case 30: p.events |= kEventHell; break;
    case 33: debut(kAffixScreamer, "pc_survival_theme_screamer"); break;
    case 35: p.events |= kEventHunt; p.more_events = false; break;
    case 37:
        p.weights = {0.4f, 0.0f, 1.0f, 0.0f};
        p.affixes = kAffixArmored;
        p.affix_share = 0.5f;
        p.more_events = false;
        p.theme = "pc_survival_theme_procession";
        break;
    case 48:
        p.giants = 3;
        p.giant_first = true;
        p.events |= kEventHell;
        p.theme = "pc_survival_theme_giants";
        break;
    case 50:
        p.boss = SurvivalBoss::LongDark;
        p.events = kEventBlackout;
        p.lisa_last = false;
        p.lisa_chance = 0.0f;
        p.count = 16;
        p.giants = 0;
        p.weights = {1.0f, 0.6f, 0.0f, 0.0f};
        p.more_events = false;
        p.boon = true;
        p.theme = "pc_survival_theme_boss_dark";
        break;
    case 51: p.count = static_cast<int>(std::lround(p.count * 0.6f)); break;
    case 53:
        p.weights = {0.2f, 1.0f, 0.0f, 0.0f};
        p.affixes = kAffixShy;
        p.affix_share = 0.6f;
        p.interval *= 0.7f;
        p.theme = "pc_survival_theme_crawl";
        break;
    case 67:
        p.weights = {0.4f, 0.0f, 1.0f, 0.0f};
        p.affixes = kAffixArmored | kAffixScreamer;
        p.affix_share = 0.6f;
        p.theme = "pc_survival_theme_procession";
        break;
    case 75:
        p.giants = 4;
        p.giant_first = true;
        p.weights = {0.6f, 0.2f, 0.0f, 1.0f};
        p.more_events = false;
        p.theme = "pc_survival_theme_giants";
        break;
    case 85: p.events |= kEventHunt; break;
    case 95:
        p.giants = 4;
        p.giant_first = true;
        p.lisa_last = true;
        p.events |= kEventFog;
        p.theme = "pc_survival_theme_giants";
        break;
    case kSurvivalWaves:
        p.boss = SurvivalBoss::Final;
        p.events = kEventBlackout | kEventSiren | kEventFog | kEventHell;
        p.lisa_last = false;
        p.lisa_chance = 0.0f;
        p.count = 40;
        p.more_events = false;
        p.boon = false;
        p.theme = "pc_survival_theme_boss_final";
        break;
    default: break;
    }
    if (p.chapter >= 5 && wave % 10 != 0 && wave % 10 != 1 && Hash(seed, wave, 7u) < 0.25f) p.events |= kEventHell;
    if (wave > 12 && p.mission == SurvivalMission::None && p.boss == SurvivalBoss::None && !p.giant_boss && !p.breather && p.theme == nullptr &&
        wave % 10 != 0) {
        const float chance = wave < 50 ? 0.25f : wave < 70 ? 0.35f : 0.45f;
        if (Hash(seed, wave, 11u) < chance) {
            SurvivalMission pool[5];
            int n = 0;
            pool[n++] = SurvivalMission::Radio;
            pool[n++] = SurvivalMission::Fuse;
            if (wave > 18) pool[n++] = SurvivalMission::Fragments;
            if (wave > 29) pool[n++] = SurvivalMission::Lamps;
            if (wave > 36) pool[n++] = SurvivalMission::Door;
            p.mission = pool[std::min(n - 1, static_cast<int>(Hash(seed, wave, 13u) * static_cast<float>(n)))];
            if (p.mission == SurvivalMission::Fuse) p.events |= kEventBlackout;
            if (p.mission == SurvivalMission::Lamps) p.events |= kEventHell;
        }
    }
    return p;
}

int SurvivalNightOf(int wave) {
    return wave <= kSurvivalWaves ? 1 : 2 + (wave - kSurvivalWaves - 1) / kSurvivalLaterWaves;
}

int SurvivalWaveInNight(int wave) {
    return wave <= kSurvivalWaves ? wave : (wave - kSurvivalWaves - 1) % kSurvivalLaterWaves + 1;
}

bool SurvivalNightEnds(int wave) {
    return wave == kSurvivalWaves || (wave > kSurvivalWaves && SurvivalWaveInNight(wave) == kSurvivalLaterWaves);
}

int SurvivalLampsOut(int wave) {
    const int night = SurvivalNightOf(wave);
    return night < 2 ? 0 : night > 2 ? kSurvivalLamps : (SurvivalWaveInNight(wave) - 1) / 5;
}

namespace {

SurvivalWavePlan PlanLaterWave(int wave, uint32_t seed) {
    const int night = SurvivalNightOf(wave);
    const int in = SurvivalWaveInNight(wave);
    SurvivalWavePlan p = PlanSurvivalWave(kSurvivalWaves - kSurvivalLaterWaves + in, seed ^ static_cast<uint32_t>(night) * 0x9E3779B9u);
    p.wave = wave;
    if (p.mission != SurvivalMission::Lamps && in != kSurvivalLaterWaves && Hash(seed, wave, 17u) >= 0.33f) p.events &= ~kEventHell;
    if (night >= 3 && p.mission == SurvivalMission::Fuse) {
        p.mission = SurvivalMission::None;
        p.events &= ~kEventBlackout;
    }
    if (night >= 4 && p.mission == SurvivalMission::Door) p.mission = SurvivalMission::None;
    if (night >= 5) p.events |= kEventHunt;
    p.giant_boss = in == 13;
    p.checkpoint = false;
    const float k = static_cast<float>(std::min(night, 6) - 1);
    const float beyond = static_cast<float>(std::max(0, night - 6));
    p.count = static_cast<int>(std::lround(static_cast<float>(p.count) * (1.0f + 0.14f * k) * std::pow(1.25f, beyond)));
    p.cap = std::min(30, p.cap + 2 * (night - 1));
    p.interval = std::max(0.25f, p.interval * std::pow(0.93f, static_cast<float>(night - 1)));
    p.health *= (1.0f + 0.15f * k) * std::pow(1.2f, beyond);
    p.speed = std::min(2.0f, p.speed * (1.0f + 0.03f * k) * std::pow(1.05f, beyond));
    p.damage *= (1.0f + 0.1f * k) * std::pow(1.15f, beyond);
    p.affix_share = std::min(0.9f, p.affix_share + 0.05f * static_cast<float>(night - 1));
    p.lisa_chance = std::min(0.15f, p.lisa_chance + 0.01f * static_cast<float>(night - 1));
    return p;
}

}

}
