#include <cmath>
#include <cstdio>
#include <string>

#include "game/survival/survival_boons.h"
#include "game/survival/survival_profile.h"
#include "game/survival/survival_waves.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++g_failures;
    }
}

}

int main() {
    using namespace pt::game;

    for (int w = 1; w <= 150; ++w) {
        const SurvivalWavePlan a = PlanSurvivalWave(w, 0x20261119u);
        const SurvivalWavePlan b = PlanSurvivalWave(w, 0x20261119u);
        Check(a.count > 0 && a.cap > 0 && a.interval > 0.0f, "wave counts/cap/interval positive");
        Check(a.health > 0.0f && a.speed > 0.0f && a.damage > 0.0f, "wave multipliers positive");
        Check(a.count == b.count && a.mission == b.mission && a.events == b.events && a.boss == b.boss,
              "PlanSurvivalWave deterministic for same seed");
    }
    Check(PlanSurvivalWave(4, 1u).weights[3] == 0.0f && PlanSurvivalWave(5, 1u).lisa_last && PlanSurvivalWave(6, 1u).weights[3] > 0.0f,
          "Mother spawns only after wave 5 Lisa debut");
    Check(PlanSurvivalWave(6, 1u).mission == SurvivalMission::Radio, "wave 6 introduces Radio task");
    Check(PlanSurvivalWave(12, 1u).mission == SurvivalMission::Fuse, "wave 12 introduces Fuse task");
    Check(PlanSurvivalWave(18, 1u).mission == SurvivalMission::Fragments, "wave 18 introduces Fragments task");
    Check(PlanSurvivalWave(25, 1u).giant_boss, "wave 25 is Giant boss");
    Check(PlanSurvivalWave(29, 1u).mission == SurvivalMission::Lamps, "wave 29 introduces Lamps task");
    Check(PlanSurvivalWave(36, 1u).mission == SurvivalMission::Door, "wave 36 introduces Door task");
    Check(PlanSurvivalWave(50, 1u).boss == SurvivalBoss::LongDark, "wave 50 is Lisa LongDark boss");
    Check(PlanSurvivalWave(100, 1u).boss == SurvivalBoss::Final, "wave 100 is Lisa Final boss");
    Check(SurvivalNightOf(1) == 1 && SurvivalNightOf(100) == 1 && SurvivalNightOf(101) == 2, "SurvivalNightOf boundaries");
    Check(SurvivalWaveInNight(101) == 1 && SurvivalNightEnds(100) && SurvivalNightEnds(125), "later night boundaries");
    Check(SurvivalLampsOut(1) == 0 && SurvivalLampsOut(101) == 0 && SurvivalLampsOut(106) == 1 &&
          SurvivalLampsOut(130) == kSurvivalLamps, "lamps out progression");

    Check(kSurvivalWeaponUnlocks.size() == 4, "four weapon unlock waves");
    Check(kSurvivalWeaponUnlocks[0].after_wave == 2 && kSurvivalWeaponUnlocks[1].after_wave == 4 &&
          kSurvivalWeaponUnlocks[2].after_wave == 30 && kSurvivalWeaponUnlocks[3].after_wave == 57,
          "weapon unlock wave schedule");

    {
        SurvivalBoons boons;
        boons.rank[static_cast<size_t>(SurvivalBoon::QuickHands)] = 2;
        boons.rank[static_cast<size_t>(SurvivalBoon::HollowPoints)] = 3;
        const std::string saved = boons.Save();
        SurvivalBoons loaded;
        loaded.Load(saved);
        Check(loaded[SurvivalBoon::QuickHands] == 2, "boon roundtrip QuickHands");
        Check(loaded[SurvivalBoon::HollowPoints] == 3, "boon roundtrip HollowPoints");
        Check(loaded.Taken() == 5, "boon Taken count");
        SurvivalBoons bad;
        bad.Load("!!!xyz;;;@@@---___");
        Check(bad.Taken() == 0, "boon parser ignores non-hex input");
        SurvivalBoons clamped;
        clamped.Load(std::string(static_cast<size_t>(SurvivalBoon::Count), 'f'));
        bool all_clamped = true;
        for (size_t i = 0; i < static_cast<size_t>(SurvivalBoon::Count); ++i) {
            if (clamped.rank[i] != BoonSpec(static_cast<SurvivalBoon>(i)).ranks) all_clamped = false;
        }
        Check(all_clamped, "boon parser clamps hex ranks to BoonSpec max");
    }

    {
        SurvivalProfile p;
        p.best_score = {100, 2500, 4200, 9999};
        p.daily_date = 20261119;
        p.daily_wave = 14;
        p.daily_score = 3300;
        p.unlocks = kUnlockBlued | kUnlockAmber;
        p.finish = 1;
        p.torch = 1;
        p.checkpoints[1].wave = 11;
        p.checkpoints[1].max_health = 120.0f;
        p.checkpoints[1].flares = 3;
        p.checkpoints[1].score = 1800;
        p.checkpoints[1].weapons[1] = {1, 12, 45};
        const std::string line = p.Save();
        SurvivalProfile q;
        q.Load(line);
        Check(q.best_score == p.best_score, "profile best_score roundtrip");
        Check(q.daily_date == 20261119 && q.daily_wave == 14 && q.daily_score == 3300, "profile daily roundtrip");
        Check(q.unlocks == p.unlocks && q.finish == 1 && q.torch == 1, "profile unlocks/looks roundtrip");
        Check(q.checkpoints[1].wave == 11 && std::fabs(q.checkpoints[1].max_health - 120.0f) < 0.01f &&
              q.checkpoints[1].flares == 3 && q.checkpoints[1].weapons[1][2] == 45,
              "profile checkpoint roundtrip");
        SurvivalProfile garbage;
        garbage.Load("!!!corrupt;;;profile=bad|||\x01");
        Check(garbage.checkpoints[0].wave == 0 && garbage.best_score[0] == 0, "profile tolerates corrupt input");
    }

    std::printf("%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
