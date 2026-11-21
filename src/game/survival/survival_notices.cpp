#include <algorithm>
#include <cmath>

#include "engine/core/log.h"
#include "game/game.h"
#include "game/survival/survival.h"

namespace pt::game {

namespace {

constexpr uint32_t kSoundCrackle = 0xE4B855B2;
constexpr float kVisionFrom = 3.0f;
constexpr float kVisionTo = 5.0f;
constexpr const char* kAccusations[] = {
    "pc_survival_accuse_01", "pc_survival_accuse_02", "pc_survival_accuse_03", "pc_survival_accuse_04", "pc_survival_accuse_05", 
    "pc_survival_accuse_06", "pc_survival_accuse_07", "pc_survival_accuse_08", "pc_survival_accuse_09", "pc_survival_accuse_10", 
    "pc_survival_accuse_11", "pc_survival_accuse_12", "pc_survival_accuse_13", "pc_survival_accuse_14", "pc_survival_accuse_15", 
    "pc_survival_accuse_16", "pc_survival_accuse_17", "pc_survival_accuse_18", "pc_survival_accuse_19", "pc_survival_accuse_20"};
constexpr int kAccusationsFirst = 5;
constexpr int kAccusationsPlain = 12;

}

SurvivalNight::Vision SurvivalNight::VisionOf(std::string_view key) {
    if (key == "pc_survival_task_fuse" || key == "pc_survival_omen_blackout") return Vision::Fuse;
    if (key == "pc_survival_task_radio") return Vision::Radio;
    if (key == "pc_survival_task_fragments" || key == "pc_survival_omen_hunt" || key == "pc_survival_theme_boss_dark" ||
        key == "pc_survival_theme_boss_final") {
        return Vision::Photo;
    }
    if (key == "pc_survival_task_lamps" || key == "pc_survival_omen_hell") return Vision::Lantern;
    if (key == "pc_survival_task_door") return Vision::Door;
    return Vision::None;
}

void SurvivalNight::Notice(const std::string& key, std::string extra, float hold, Vision vision) {
    notices_.push_back({key, std::move(extra), hold, vision});
}

void SurvivalNight::UpdateNotices(float dt) {
    if (!hud_.task_key.empty() && hud_.task_key == notice_task_key_) {
        if (hud_.task_count > notice_task_count_ && hud_.task_need > 0 && hud_.task_need < 100 && !hud_.task_done && !hud_.task_lost) {
            Notice(hud_.task_key, std::to_string(hud_.task_count) + " / " + std::to_string(hud_.task_need), 3.5f, Vision::None);
        }
        if ((hud_.task_done || hud_.task_lost) && !notice_task_over_) {
            Notice(hud_.task_done ? "pc_survival_task_done" : "pc_survival_task_lost", "", 3.5f, Vision::None);
            notice_task_over_ = true;
        }
    }
    notice_task_count_ = hud_.task_count;
    if (notice_on_) {
        notice_age_ += dt;
        if (notice_age_ >= notice_.hold + 0.15f) notice_on_ = false;
    }
    if (!notice_on_ && !notices_.empty() && offer_count_ == 0) {
        notice_ = std::move(notices_.front());
        notices_.pop_front();
        notice_on_ = true;
        notice_age_ = 0.0f;
        notice_flash_ = -1;
        ++hud_.notice_serial;
        hud_.notice_key = notice_.key;
        hud_.notice_extra = notice_.extra;
        hud_.notice_hold = notice_.hold;
        game_.Captions().SkipNext(kSoundCrackle);
        game_.PostSoundId(kSoundCrackle);
        LogInfo("survival: notice {}{}{}", notice_.key, notice_.extra.empty() ? "" : " ", notice_.extra);
    }
    hud_.notice_age = notice_on_ ? notice_age_ : -1.0f;
    hud_.notice_vision = notice_on_ && notice_.vision != Vision::None;
    int vision_index = 0;
    float vision_k = 0.0f;
    const bool flashing = VisionFlash(vision_index, vision_k);
    hud_.vision_kind = hud_.notice_vision ? static_cast<int>(notice_.vision) : 0;
    hud_.vision_index = flashing ? vision_index : -1;
    hud_.vision_k = flashing ? vision_k : 0.0f;
    hud_.vision_window = hud_.notice_vision && notice_age_ >= kVisionFrom && notice_age_ < kVisionTo
                             ? (notice_age_ - kVisionFrom) / (kVisionTo - kVisionFrom) : -1.0f;
    hud_.vision_tint = (events_ & kEventHell) != 0      ? glm::vec3(1.0f, 0.55f, 0.5f)
                       : StreetLightLevel() < 0.5f ? glm::vec3(0.78f, 0.88f, 1.0f)
                                                     : glm::vec3(1.0f, 0.94f, 0.86f);
    UpdateAccusations(dt);
    if (wave_ >= 30 && phase_ == SurvivalPhase::Wave && !hud_.dead) {
        glitch_wait_ -= dt * (1.0f + 1.5f * fear_);
        if (glitch_wait_ <= 0.0f) {
            ++hud_.glitch_serial;
            const float late = std::clamp((static_cast<float>(wave_) - 30.0f) / 70.0f, 0.0f, 1.0f);
            glitch_wait_ = (36.0f - 26.0f * late) * (0.6f + 0.8f * Rand());
        }
    }
}

void SurvivalNight::UpdateAccusations(float dt) {
    if (wave_ < 20 || phase_ != SurvivalPhase::Wave || hud_.dead || caught_ || hud_.vision_window >= 0.0f) return;
    accuse_wait_ -= dt * (1.0f + fear_);
    if (accuse_wait_ > 0.0f) return;
    const int pool = wave_ < 50 ? kAccusationsFirst : wave_ <= kSurvivalWaves ? kAccusationsPlain : static_cast<int>(std::size(kAccusations));
    int line = static_cast<int>(Rand() * static_cast<float>(pool)) % pool;
    if (line == accuse_last_) line = (line + 1 + static_cast<int>(Rand() * static_cast<float>(pool - 1))) % pool;
    Accuse(line);
    const float late = std::clamp((static_cast<float>(wave_) - 20.0f) / 80.0f, 0.0f, 1.0f);
    accuse_wait_ = (wave_ > kSurvivalWaves ? 32.0f : 150.0f - 100.0f * late) * (0.7f + 0.6f * Rand());
}

void SurvivalNight::Accuse(int line) {
    line = std::clamp(line, 0, static_cast<int>(std::size(kAccusations)) - 1);
    accuse_last_ = line;
    hud_.accuse_key = kAccusations[line];
    ++hud_.accuse_serial;
    game_.Captions().SkipNext(kSoundCrackle);
    game_.PostSoundId(kSoundCrackle);
    Voice(hud_.accuse_key, 1.3f);
    Play(thud_, nullptr, 6.0f, 0.55f);
    shake_ = std::min(1.0f, shake_ + 0.35f);
    Frighten(0.12f);
    LogInfo("survival: accusation {}", hud_.accuse_key);
}

bool SurvivalNight::VisionFlash(int& index, float& k) const {
    if (!notice_on_ || notice_.vision == Vision::None || notice_age_ < kVisionFrom || notice_age_ >= kVisionTo) return false;
    const int count = 4 + std::min(4, wave_ / 25);
    const float slot = (kVisionTo - kVisionFrom) / static_cast<float>(count);
    const float t = notice_age_ - kVisionFrom;
    index = std::min(count - 1, static_cast<int>(t / slot));
    k = (t - static_cast<float>(index) * slot) / (slot * 0.62f);
    return k < 1.0f;
}

}
