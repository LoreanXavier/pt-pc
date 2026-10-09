#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "game/floor_level.h"

namespace pt::fox2 {
struct Entity;
}

namespace pt::game {

class Game;
struct Stage;

enum class NazoState : int { Inactive = 0, Active = 1, Cleared = 2 };

class NazoManager {
public:
    static constexpr int kCount = 5;
    static constexpr float kHoldLimit = 2.0f;
    static constexpr float kPeepholeHoldLimit = 34.0f;

    explicit NazoManager(Game& game) : game_(game) {}

    void RegisterStage(Stage& stage);
    void RestoreCompletedPeephole(Stage& stage);
    void ForgetStage(const Stage& stage);

    void Activate(NazoId id);
    void Deactivate(NazoId id);
    void Prepare(NazoId id);
    void ForceClear(NazoId id);
    void ResetState(NazoId id);
    void ApplyVisuals(NazoId id);
    bool IsCleared(NazoId id) const { return cleared_[Index(id)] != 0; }
    bool IsActive(NazoId id) const { return (active_mask_ >> Index(id)) & 1; }
    uint32_t Word(NazoId id) const { return word_[Index(id)]; }
    NazoState State(NazoId id) const;
    void ResetAllStates();
    // a new session (loop browser pick, progress reset): every state, timer and counter as at boot; the stage assets stay
    void ResetSession();
    // the state ResetSession sets, one line (Game::DescribeSessionState)
    std::string DescribeSession() const;
    void CancelPendingClearSound() { clear_sound_pending_ = false; }
    void SetCondition(std::string_view condition, const glm::vec3& target = glm::vec3(0.0f));
    bool IsActionArmed() const { return action_armed_; }
    bool IsPeepholeTheaterActive() const { return (word_[2] & 8) != 0; }
    // nazo service +0x60 (0x1267390), called by the camera's zoom-out branch (0x984A62) on every frame the zoom falls
    void AbortPeephole();
    // translation of the `Peephole` control asset (GetAssetTransform 0x9125D0), the pivot of the theater camera
    bool PeepholePosition(glm::vec3& out) const { return AssetPosition("Peephole", out); }
    // any control asset's translation (XMarkText, HELL_H, ...), for the input script's fent
    bool ControlAssetPosition(std::string_view name, glm::vec3& out) const { return AssetPosition(name, out); }
    bool PeepholeEye(glm::vec3& out) const {
        if (!peephole_eye_valid_ || !IsPeepholeTheaterActive()) return false;
        out = peephole_eye_;
        return true;
    }
    bool IsPhotoOptionPending() const { return IsActive(NazoId::Photo) && !(word_[3] & 0x100); }
    void CountFootstep();
    uint32_t PhotoWord() const { return word_[3]; }
    void SetPhotoWord(uint32_t word) { word_[3] = word; }
    void StartVoiceRecognition();
    void StopVoiceRecognition();
    bool VoiceRecognitionActive() const { return voice_active_; }
    void OnVoiceKeyword(std::string_view keyword);
    void Update(float dt);
    int TrueEndSteps() const { return true_end_steps_; }
    float TrueEndTimer() const { return true_end_timer_; }

private:
    static int Index(NazoId id) { return static_cast<int>(id); }
    void SetAssetEnable(std::string_view name, bool on);
    bool AssetPosition(std::string_view name, glm::vec3& out) const;
    void OnClear(NazoId id);
    void Arm(int state, uint32_t flag);
    void Commit();
    void HelloStep();
    void UpdateTrueEnd(float dt);
    void UpdatePeephole(float dt);
    void Sound2D(uint32_t id);
    void SoundAt(uint32_t id, std::string_view asset);

    struct AssetRef {
        uint32_t stage_id = 0;
        const fox2::Entity* entity = nullptr;
    };

    Game& game_;
    std::array<uint32_t, kCount> word_{};
    uint32_t active_mask_ = 0;
    std::array<uint8_t, kCount> cleared_{};
    float peephole_timer_ = 0.0f;
    float clear_sound_timer_ = 0.0f;
    float hold_timer_ = 0.0f;
    float true_end_timer_ = 0.0f;
    int frames_since_hide_ = 0;
    int hide_ticks_ = 0;
    // AbortPeephole calls that went through (the zoom-out branch, once per game frame of a falling zoom), logged per clear
    int abort_calls_ = 0;
    int armed_state_ = 0;
    uint32_t armed_flag_ = 0;
    int true_end_steps_ = 0;
    bool clear_sound_pending_ = false;
    bool action_armed_ = false;
    bool hide_effect_next_frame_ = false;
    bool voice_active_ = false;
    bool jack_heard_ = false;
    uint32_t peephole_sound_ = 0;
    glm::vec3 peephole_eye_{0.0f};
    bool peephole_eye_valid_ = false;
    glm::vec3 last_target_{0.0f};
    std::map<std::string, std::vector<AssetRef>, std::less<>> assets_;
};

std::string_view NazoName(NazoId id);

}
