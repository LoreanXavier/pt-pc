#include <algorithm>
#include <cmath>

#include "engine/core/log.h"
#include "game/game.h"
#include "game/survival/survival.h"

namespace pt::game {

SurvivalNight::WeaponSpec SurvivalNight::Gun(SurvivalWeapon w) const {
    WeaponSpec s = Spec(w);
    const auto r = [this](SurvivalBoon b) { return static_cast<float>(boons_[b]); };
    s.reload *= std::max(0.4f, 1.0f - 0.18f * r(SurvivalBoon::QuickHands));
    s.max_reserve = static_cast<int>(std::lround(static_cast<float>(s.max_reserve) * (1.0f + 0.4f * r(SurvivalBoon::DeepPockets))));
    s.pickup_ammo = static_cast<int>(std::lround(static_cast<float>(s.pickup_ammo) * (1.0f + 0.25f * r(SurvivalBoon::DeepPockets))));
    s.spread *= 1.0f - 0.2f * r(SurvivalBoon::SteadyHands);
    s.kick *= 1.0f - 0.15f * r(SurvivalBoon::SteadyHands);
    switch (w) {
    case SurvivalWeapon::Flashlight:
        s.interval *= 1.0f - 0.25f * r(SurvivalBoon::SearingLight);
        s.damage *= 1.0f + 0.5f * r(SurvivalBoon::SearingLight);
        break;
    case SurvivalWeapon::Handgun: s.damage *= 1.0f + 0.2f * r(SurvivalBoon::HollowPoints); break;
    case SurvivalWeapon::Shotgun:
        s.pellets += 2 * boons_[SurvivalBoon::Buckshot];
        s.interval *= 1.0f - 0.18f * r(SurvivalBoon::QuickPump);
        break;
    case SurvivalWeapon::Magnum:
        s.damage *= 1.0f + 0.15f * r(SurvivalBoon::HeavyRounds);
        s.interval *= 1.0f - 0.18f * r(SurvivalBoon::FanTheHammer);
        break;
    case SurvivalWeapon::Lever:
        s.damage *= 1.0f + 0.15f * r(SurvivalBoon::HeavyRounds);
        s.interval *= 1.0f - 0.18f * r(SurvivalBoon::QuickPump);
        break;
    case SurvivalWeapon::Double: s.pellets += 2 * boons_[SurvivalBoon::Buckshot]; break;
    default: break;
    }
    if (start_ == SurvivalStart::Daily) {
        const DailyMutatorDef& mut = DailyMutatorById(daily_mutator_);
        s.reload /= std::max(0.5f, mut.reload_speed_mult);
        s.interval /= std::max(0.5f, mut.fire_rate_mult);
        s.max_reserve = std::max(s.clip_size, static_cast<int>(std::lround(static_cast<float>(s.max_reserve) * mut.ammo_cap_mult)));
        if (w == SurvivalWeapon::Flashlight) {
            s.interval /= std::max(0.5f, mut.stun_mult);
            s.damage *= mut.stun_mult;
        } else {
            s.damage *= mut.damage_mult;
        }
    }
    return s;
}

void SurvivalNight::OfferBoons() {
    std::vector<SurvivalBoon> pool;
    for (size_t i = 0; i < static_cast<size_t>(SurvivalBoon::Count); ++i) {
        const auto b = static_cast<SurvivalBoon>(i);
        if (boons_.Maxed(b)) continue;
        if (b == SurvivalBoon::IronGrip && wave_ < 6) continue;
        if (b == SurvivalBoon::HeavyRounds || b == SurvivalBoon::FanTheHammer) {
            if (!weapons_[static_cast<size_t>(SurvivalWeapon::Magnum)].owned) continue;
        }
        if ((b == SurvivalBoon::Buckshot || b == SurvivalBoon::QuickPump) && !weapons_[static_cast<size_t>(SurvivalWeapon::Shotgun)].owned) continue;
        pool.push_back(b);
    }
    offer_count_ = 0;
    plan_rng_ = PlanSeed(wave_, 1u);
    while (offer_count_ < 3 && !pool.empty()) {
        const size_t pick = std::min(pool.size() - 1, static_cast<size_t>(PlanRand() * static_cast<float>(pool.size())));
        offer_[static_cast<size_t>(offer_count_++)] = pool[pick];
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(pick));
    }
    if (offer_count_ == 0) return;
    hud_.offer_count = offer_count_;
    hud_.offer_taken = -1;
    hud_.offer_age = 0.0f;
    for (int i = 0; i < offer_count_; ++i) {
        hud_.offer[static_cast<size_t>(i)] = static_cast<uint8_t>(offer_[static_cast<size_t>(i)]);
        hud_.offer_rank[static_cast<size_t>(i)] = static_cast<uint8_t>(boons_[offer_[static_cast<size_t>(i)]] + 1);
    }
    countdown_ = std::max(countdown_, 20.0f);
    last_tick_ = static_cast<int>(std::ceil(countdown_));
    if (!radio_noise_) radio_noise_ = PostEvent("Play_radio_noise_lp", nullptr);
    Speak(take_one_sound_, "take_one");
    LogInfo("survival: the radio offers {} / {} / {}", BoonSpec(offer_[0]).name, offer_count_ > 1 ? BoonSpec(offer_[1]).name : "-",
            offer_count_ > 2 ? BoonSpec(offer_[2]).name : "-");
}

void SurvivalNight::ChooseOffer(int index) {
    if (offer_count_ <= 0 || index < 0 || index >= offer_count_ || hud_.offer_age < 0.35f) return;
    LogInfo("survival: the offer's card {} taken with the pointer", index + 1);
    trigger_dead_ = true;
    TakeBoon(index);
}

void SurvivalNight::TakeBoon(int index) {
    if (offer_count_ <= 0 || index < 0 || index >= offer_count_) return;
    const SurvivalBoon b = offer_[static_cast<size_t>(index)];
    ++boons_.rank[static_cast<size_t>(b)];
    offer_count_ = 0;
    hud_.offer_taken = index;
    hud_.taken_age = 0.0f;
    if (b == SurvivalBoon::SecondWind) {
        max_health_ += 20.0f;
        health_ = max_health_;
        hud_.heal = 0.9f;
        SpawnHealEffect(game_.GetPlayer().Feet());
        Play(heal_cue_, nullptr);
    }
    if (b == SurvivalBoon::FlarePouch && flares_ < MaxFlares()) {
        ++flares_;
        Note("pc_survival_pickup_flare");
    }
    if (radio_noise_ && game_.Audio()) game_.Audio()->StopPlayingId(radio_noise_, 0.4f);
    radio_noise_ = 0;
    PlaySound(swap_sound_, nullptr);
    if (phase_ == SurvivalPhase::Break) countdown_ = std::min(countdown_, 6.0f);
    LogInfo("survival: took {} (rank {}, card {})", BoonSpec(b).name, boons_[b], index + 1);
    if (phase_ == SurvivalPhase::Break && kept_break_) SaveCheckpoint(true);
}

void SurvivalNight::SaveCheckpoint(bool quiet) {
    if (start_ == SurvivalStart::Daily) return;
    SurvivalCheckpoint c;
    c.wave = wave_ + 1;
    for (size_t i = 0; i < weapons_.size() && i < c.weapons.size(); ++i) {
        c.weapons[i] = {weapons_[i].owned ? 1 : 0, weapons_[i].clip, weapons_[i].reserve};
    }
    c.max_health = max_health_;
    c.boons = boons_.Save();
    c.flares = flares_;
    c.score = score_;
    c.kills = kills_;
    c.headshots = headshots_;
    c.shots = shots_;
    c.hits = hits_;
    c.best_chain = best_chain_;
    c.run_time = run_time_;
    game_.SurvivalProfileData().checkpoints[static_cast<size_t>(difficulty_)] = c;
    game_.MarkSurvivalProfile();
    kept_wave_ = c.wave;
    if (!quiet) {
        kept_break_ = true;
        hud_.kept = 4.0f;
        hud_.kept_wave = c.wave;
    }
    LogInfo("survival: the night kept at wave {} (difficulty {}{})", c.wave, static_cast<int>(difficulty_), quiet ? ", again with what the break brought" : "");
}

void SurvivalNight::Unlock(uint32_t bits) {
    SurvivalProfile& profile = game_.SurvivalProfileData();
    const uint32_t fresh = bits & ~profile.unlocks;
    if (!fresh) return;
    profile.unlocks |= fresh;
    if (fresh & kUnlockBlued) profile.GrantSkin(1);
    if (fresh & kUnlockNickel) profile.GrantSkin(2);
    if (fresh & kUnlockGold) profile.GrantSkin(28);
    if (fresh & kUnlockAmber) profile.GrantTorch(1);
    if (fresh & kUnlockRed) profile.GrantTorch(2);
    game_.MarkSurvivalProfile();
    Note(fresh & (kUnlockAmber | kUnlockRed) && !(fresh & (kUnlockBlued | kUnlockNickel | kUnlockGold)) ? "pc_survival_unlock_torch" : "pc_survival_unlock_finish");
    LogInfo("survival: unlocked {:#x}", fresh);
}

ReloadStyle SurvivalNight::ActiveReloadStyle() const {
    const int w = static_cast<int>(weapon_);
    if (w <= 0 || w >= static_cast<int>(SurvivalWeapon::Count)) return ReloadStyle::Standard;
    return game_.SurvivalProfileData().ResolvedReloadStyle(w);
}

void SurvivalNight::ApplyLooks(bool on) {
    TextureManager& table = game_.Models().Textures();
    for (const auto& [index, colour] : metal_colours_) {
        if (const MaterialGpu* cur = table.Material(index)) {
            MaterialGpu m = *cur;
            m.albedo_factor = colour;
            table.UpdateMaterial(index, m);
        }
    }
    metal_colours_.clear();
    torch_tint_ = glm::vec3(1.0f);
    const SurvivalProfile& profile = game_.SurvivalProfileData();
    for (size_t w = 1; w < models_.guns.size(); ++w) {
        const int skin_id = on ? profile.EquippedSkin(static_cast<int>(w)) : 0;
        ApplyWeaponSkin(table, models_.guns[w], skin_id);
    }
    const int torch = on ? profile.EquippedTorch() : 0;
    ApplyTorchSkin(table, models_, torch, 60.0f);
    if (!on) return;
    torch_tint_ = SurvivalTorch(torch).tint;
    LogInfo("survival: looks applied (skins {}/{}/{}/{}/{}, torch {})", profile.EquippedSkin(1), profile.EquippedSkin(2),
            profile.EquippedSkin(3), profile.EquippedSkin(4), profile.EquippedSkin(5), torch);
}

void SurvivalNight::LoadCheckpoint(const SurvivalCheckpoint& c) {
    if (c.wave <= 1) return;
    wave_ = c.wave - 1;
    for (size_t i = 0; i < weapons_.size() && i < c.weapons.size(); ++i) {
        weapons_[i].owned = c.weapons[i][0] != 0 || i == 0;
        weapons_[i].clip = c.weapons[i][1];
        weapons_[i].reserve = c.weapons[i][2];
    }
    max_health_ = std::max(1.0f, c.max_health);
    health_ = max_health_;
    boons_.Load(c.boons);
    flares_ = c.flares;
    score_ = c.score;
    kills_ = c.kills;
    headshots_ = c.headshots;
    shots_ = c.shots;
    hits_ = c.hits;
    best_chain_ = c.best_chain;
    run_time_ = c.run_time;
    lamps_out_ = SurvivalLampsOut(wave_ + 1);
    north_end_ = NorthEndOf(SurvivalNightOf(wave_ + 1));
    night_door_ = SurvivalNightOf(wave_ + 1) >= 4 ? 1.0f : 0.0f;
    for (const SurvivalWeapon w : {SurvivalWeapon::Double, SurvivalWeapon::Lever, SurvivalWeapon::Shotgun, SurvivalWeapon::Magnum, SurvivalWeapon::Handgun}) {
        if (weapons_[static_cast<size_t>(w)].owned) {
            SelectWeapon(w);
            break;
        }
    }
    kept_wave_ = c.wave;
    kept_break_ = false;
    {
        const glm::vec3 feet = game_.GetPlayer().Feet();
        float side = -1.2f;
        for (const auto& due : kSurvivalWeaponUnlocks) {
            const auto weapon = static_cast<SurvivalWeapon>(due.weapon_index);
            if (wave_ < due.after_wave || weapons_[static_cast<size_t>(weapon)].owned) continue;
            Drop(feet + glm::vec3(side, 0.0f, 1.8f), SurvivalPickupKind::Weapon, weapon);
            side += 0.8f;
            LogInfo("survival: a gun due by wave {} laid at the feet", due.after_wave);
        }
    }
    LogInfo("survival: taken up at wave {} ({} boons, score {})", c.wave, boons_.Taken(), score_);
}

void SurvivalNight::KeepRecord() {
    if (wave_ <= 0) return;
    SurvivalProfile& p = game_.SurvivalProfileData();
    bool changed = false;
    if (start_ == SurvivalStart::Daily) {
        const int today = SurvivalToday();
        if (p.daily_date != today) {
            p.daily_date = today;
            p.daily_wave = p.daily_score = 0;
        }
        if (wave_ > p.daily_wave || score_ > p.daily_score) {
            p.daily_wave = std::max(p.daily_wave, wave_);
            p.daily_score = std::max(p.daily_score, score_);
            changed = true;
        }
    } else if (score_ > p.best_score[static_cast<size_t>(difficulty_)]) {
        p.best_score[static_cast<size_t>(difficulty_)] = score_;
        changed = true;
    }
    if (!credits_paid_) {
        credits_paid_ = true;
        const int cleared = phase_ == SurvivalPhase::Dead ? std::max(0, wave_ - 1) : wave_;
        SurvivalPayoutInput in;
        in.waves_cleared = cleared;
        in.kills = kills_;
        in.headshots = headshots_;
        in.best_chain = best_chain_;
        in.missions_done = missions_done_;
        in.bosses_down = bosses_down_;
        in.difficulty = static_cast<int>(difficulty_);
        in.daily = start_ == SurvivalStart::Daily;
        in.daily_mutator = daily_mutator_;
        in.daily_goal_fresh = daily_goal_fresh_;
        in.daily_streak = p.daily_streak;
        const SurvivalPayout payout = ComputeSurvivalCredits(in);
        last_payout_total_ = payout.total;
        last_payout_daily_ = payout.daily_bonus;
        if (payout.total > 0) {
            p.credits += payout.total;
            changed = true;
        }
        hud_.end_credits_earned = payout.total;
        hud_.end_credits_total = p.credits;
        hud_.end_daily_bonus = payout.daily_bonus;
        LogInfo("survival: run payout +${} (waves {}, combat {}, x{:.2f}, daily bonus +${}) -> total ${}",
                payout.total, payout.wave_credits, payout.combat_credits, payout.multiplier, payout.daily_bonus, p.credits);
    }
    if (changed) game_.MarkSurvivalProfile();
}

}
