#include <algorithm>
#include <cmath>

#include "engine/core/log.h"
#include "engine/render/camera.h"
#include "game/game.h"
#include "game/player.h"
#include "game/survival/survival.h"

namespace pt::game {

namespace {

glm::vec2 Flat(const glm::vec3& v) {
    return {v.x, v.z};
}

}

void SurvivalNight::SpawnBoss() {
    const glm::vec3 feet = game_.GetPlayer().Feet();
    glm::vec3 at;
    if (!SpawnPoint(at, 16.0f, 24.0f)) at = feet + glm::vec3(0.0f, 0.0f, 18.0f);
    const bool lisa = plan_.boss != SurvivalBoss::None;
    const int count = !lisa && wave_ >= 70 ? 2 : 1;
    for (int i = 0; i < count; ++i) {
        glm::vec3 p = at + glm::vec3(2.5f * static_cast<float>(i), 0.0f, 0.0f);
        if (i > 0 && !SpawnPoint(p, 16.0f, 24.0f)) continue;
        Enemy* e = SpawnEnemyAt(lisa ? SurvivalEnemyKind::Lisa : SurvivalEnemyKind::Giant, p, lisa ? 1.0f : 1.4f, 0, false);
        if (!e) continue;
        boss_due_ = false;
        e->boss = true;
        const float over = plan_.boss == SurvivalBoss::LongDark ? 3.5f : plan_.boss == SurvivalBoss::Final ? 5.0f : 2.0f;
        e->max_health = e->health = e->health * over;
        e->damage *= lisa ? 1.0f : 1.3f;
        LogInfo("survival: the boss comes ({}, {:.0f} health)", lisa ? "Lisa" : "a giant", e->max_health);
    }
    if (boss_due_) {
        boss_wait_ = 0.5f;
        if (++boss_tries_ >= 40) {
            boss_due_ = false;
            LogWarn("survival: the boss of wave {} found no body in 20 s, the wave goes on without it", wave_);
        }
        return;
    }
    boss_tries_ = 0;
    hud_.boss_key = plan_.boss == SurvivalBoss::LongDark ? "pc_survival_boss_dark"
                    : plan_.boss == SurvivalBoss::Final  ? "pc_survival_boss_final"
                                                         : "pc_survival_boss_giant";
    Banner(hud_.boss_key, 0, 3.5f);
    shake_ = std::min(1.0f, shake_ + 0.4f);
    Play(rise_, &at, 6.0f, 0.7f);
    if (lisa) Play(lisa_laugh_, &at, 4.0f, 0.9f);
}

void SurvivalNight::Blink(Enemy& e) {
    const Player& player = game_.GetPlayer();
    const glm::vec3 feet = player.Feet();
    const glm::vec3 ahead(std::sin(player.FoxYaw()), 0.0f, std::cos(player.FoxYaw()));
    const glm::vec3 side(-ahead.z, 0.0f, ahead.x);
    auto puff = [&](const glm::vec3& p) {
        for (int i = 0; i < 6; ++i) {
            Debris d;
            d.kind = Debris::Kind::Mist;
            d.id = next_id_++;
            d.position = p + glm::vec3(0.0f, 0.4f + 0.25f * static_cast<float>(i), 0.0f);
            d.velocity = glm::vec3(Rand() - 0.5f, 0.3f, Rand() - 0.5f) * 1.5f;
            d.spin = d.velocity;
            d.life = 0.8f + 0.5f * Rand();
            d.size = 0.8f + 0.5f * Rand();
            debris_.push_back(d);
        }
    };
    puff(e.position);
    for (int attempt = 0; attempt < 12; ++attempt) {
        const float back = Rand() < 0.65f ? -1.0f : 0.2f;
        const float s = Rand() < 0.5f ? 1.0f : -1.0f;
        glm::vec3 to = feet + ahead * (back * (9.0f + 4.0f * Rand())) + side * (s * (2.0f + 5.0f * Rand()));
        to.x = std::clamp(to.x, -6.5f, 4.0f);
        if (glm::distance(Flat(to), Flat(feet)) < 7.0f) continue;
        to.y = GroundAt(to + glm::vec3(0.0f, 1.0f, 0.0f), feet.y);
        if (std::abs(to.y - feet.y) > 1.0f || Blocked(to + glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.3f)) continue;
        e.position = to;
        e.state = Enemy::State::Chasing;
        e.timer = 0.0f;
        e.knock = glm::vec3(0.0f);
        const glm::vec3 look = feet - to;
        e.yaw = std::atan2(look.x, look.z);
        break;
    }
    puff(e.position);
    Play(lisa_laugh_, &e.position, 2.0f, 1.0f);
    Frighten(0.08f);
    LogInfo("survival: Lisa vanishes and comes back {:.1f} m off", glm::distance(Flat(e.position), Flat(feet)));
}

void SurvivalNight::UpdateBosses(float dt) {
    boss_wait_ = std::max(0.0f, boss_wait_ - dt);
    if (!boss_due_) boss_tries_ = 0;
    if (boss_due_ && phase_ == SurvivalPhase::Wave && wave_time_ > 4.0f && boss_wait_ <= 0.0f) SpawnBoss();
    hud_.boss = false;
    std::vector<std::pair<SurvivalEnemyKind, glm::vec3>> summons;
    for (Enemy& e : enemies_) {
        if (!e.boss || e.state == Enemy::State::Dying) continue;
        hud_.boss = true;
        const float share = e.health / std::max(1.0f, e.max_health);
        const int phase = share > 2.0f / 3.0f ? 0 : share > 1.0f / 3.0f ? 1 : 2;
        if (phase > e.phase) {
            e.phase = phase;
            e.stagger = std::max(e.stagger, 1.1f);
            e.boss_timer = 0.0f;
            Play(e.kind == SurvivalEnemyKind::Lisa ? lisa_laugh_ : rise_, &e.position, 6.0f, e.kind == SurvivalEnemyKind::Lisa ? 0.75f : 0.6f);
            shake_ = std::min(1.0f, shake_ + 0.35f);
            Omen(phase == 1 ? "pc_survival_boss_rage" : "pc_survival_boss_last", 3.5f);
            Frighten(0.15f);
            LogInfo("survival: the boss in phase {} ({:.0f} %)", phase + 1, share * 100.0f);
            if (plan_.boss == SurvivalBoss::Final && phase == 1) {
                for (const float s : {1.0f, -1.0f}) summons.emplace_back(SurvivalEnemyKind::Mother, e.position + glm::vec3(2.2f * s, 0.0f, 1.0f));
            }
            if (plan_.boss == SurvivalBoss::Final && phase == 2) {
                events_ |= kEventSiren;
                PlaySound(siren_sound_, nullptr);
            }
            if (e.kind == SurvivalEnemyKind::Lisa && phase >= 1) Blink(e);
        }
        e.boss_timer += dt;
        if (e.kind == SurvivalEnemyKind::Lisa) {
            e.beam_held = InBeam(e) ? e.beam_held + dt : std::max(0.0f, e.beam_held - dt * 0.5f);
            if (e.beam_held > 1.1f - 0.25f * static_cast<float>(e.phase) && e.state == Enemy::State::Chasing) {
                e.beam_held = 0.0f;
                Blink(e);
            }
            const float every = plan_.boss == SurvivalBoss::Final ? 8.0f : 9.0f;
            if (e.phase >= 1 && e.boss_timer >= every) {
                e.boss_timer = 0.0f;
                if (plan_.boss == SurvivalBoss::Final && e.phase >= 2) {
                    summons.emplace_back(SurvivalEnemyKind::Count, glm::vec3(0.0f));
                    summons.emplace_back(SurvivalEnemyKind::Count, glm::vec3(0.0f));
                } else {
                    for (int i = 0; i < 3; ++i) {
                        const float a = Rand() * 6.28f;
                        summons.emplace_back(SurvivalEnemyKind::Crawler, e.position + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * 2.5f);
                    }
                    Play(lisa_laugh_, &e.position, 4.0f, 0.7f);
                    boss_flash_ = 0.35f;
                }
            }
        }
    }
    for (const auto& [kind, at] : summons) {
        if (kind == SurvivalEnemyKind::Count) {
            FindPerches();
            const glm::vec3 feet = game_.GetPlayer().Feet();
            std::vector<glm::vec3> free;
            for (const glm::vec3& p : perches_) {
                const float d = glm::distance(Flat(p), Flat(feet));
                if (d > 5.0f && d < 16.0f) free.push_back(p);
            }
            if (!free.empty()) SpawnEnemyAt(SurvivalEnemyKind::Crawler, free[static_cast<size_t>(Rand() * static_cast<float>(free.size())) % free.size()], 1.0f, kAffixPerched, false);
            continue;
        }
        glm::vec3 p = at;
        for (int k = 0; k < 8 && Crowded(p); ++k) {
            const float a = 0.785f * static_cast<float>(k) + Rand();
            p = at + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * (1.2f + 0.1f * static_cast<float>(k));
        }
        p.y = GroundAt(p + glm::vec3(0.0f, 1.0f, 0.0f), at.y);
        if (Enemy* s = SpawnEnemyAt(kind, p, 0.8f, 0, false); s && kind == SurvivalEnemyKind::Mother) s->max_health = s->health = s->health * 0.2f;
    }
    boss_flash_ = std::max(0.0f, boss_flash_ - dt);
}

}
