#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "game/survival/survival_skins.h"

namespace pt::game {

struct SurvivalCheckpoint {
    int wave = 0;
    std::array<std::array<int, 3>, 6> weapons{};
    float max_health = 100.0f;
    std::string boons;
    int flares = 0;
    int score = 0;
    int kills = 0;
    int headshots = 0;
    int shots = 0;
    int hits = 0;
    int best_chain = 0;
    float run_time = 0.0f;
};

enum SurvivalUnlock : uint32_t {
    kUnlockBlued = 1u,
    kUnlockAmber = 2u,
    kUnlockNickel = 4u,
    kUnlockGold = 8u,
    kUnlockRed = 16u,
};
inline constexpr uint32_t kFinishNeeds[] = {0u, kUnlockBlued, kUnlockNickel, kUnlockGold};
inline constexpr const char* kFinishNames[] = {"pc_finish_standard", "pc_finish_blued", "pc_finish_nickel", "pc_finish_gold"};
inline constexpr uint32_t kTorchNeeds[] = {0u, kUnlockAmber, kUnlockRed};
inline constexpr const char* kTorchNames[] = {"pc_torch_white", "pc_torch_amber", "pc_torch_red", "pc_torch_uv", "pc_torch_emerald"};

struct SurvivalProfile {
    std::array<SurvivalCheckpoint, 5> checkpoints;
    std::array<int, 5> best_score{};
    int daily_date = 0;
    int daily_wave = 0;
    int daily_score = 0;
    int daily_wins = 0;
    int daily_streak = 0;
    int daily_last_win_date = 0;
    uint32_t unlocks = 0;
    int finish = 0;
    int torch = 0;
    int credits = 0;
    uint64_t owned_skins = 1ull;
    std::array<int, 6> weapon_skins{};
    uint32_t owned_torches = 1u;
    uint32_t owned_reloads = 1u;
    std::array<int, 6> weapon_reloads{};

    bool OwnsSkin(int id) const {
        if (id <= 0) return true;
        if (id >= kSurvivalSkinCount) return false;
        return (owned_skins & (1ull << static_cast<unsigned>(id))) != 0ull;
    }
    void GrantSkin(int id) {
        if (id >= 0 && id < kSurvivalSkinCount) {
            owned_skins |= (1ull << static_cast<unsigned>(id));
            owned_reloads |= (1u << static_cast<unsigned>(SurvivalSkin(id).reload));
        }
    }
    bool OwnsTorch(int id) const {
        if (id <= 0) return true;
        if (id >= kSurvivalTorchCount) return false;
        return (owned_torches & (1u << static_cast<unsigned>(id))) != 0u;
    }
    void GrantTorch(int id) {
        if (id >= 0 && id < kSurvivalTorchCount) owned_torches |= (1u << static_cast<unsigned>(id));
    }
    bool OwnsReload(int style) const {
        if (style <= 0) return true;
        if (style >= 4) return false;
        if ((owned_reloads & (1u << static_cast<unsigned>(style))) != 0u) return true;
        for (int i = 0; i < kSurvivalSkinCount; ++i) {
            if (OwnsSkin(i) && static_cast<int>(SurvivalSkin(i).reload) == style) return true;
        }
        return false;
    }
    void GrantReload(int style) {
        if (style >= 0 && style < 4) owned_reloads |= (1u << static_cast<unsigned>(style));
    }
    static int ReloadPrice(int style) {
        static constexpr int kPrices[4] = {0, 900, 1800, 4000};
        return (style >= 0 && style < 4) ? kPrices[style] : 0;
    }
    int EquippedSkin(int weapon) const {
        if (weapon < 0 || weapon >= static_cast<int>(weapon_skins.size())) return 0;
        const int id = std::clamp(weapon_skins[static_cast<size_t>(weapon)], 0, kSurvivalSkinCount - 1);
        return OwnsSkin(id) ? id : 0;
    }
    int EquippedReloadChoice(int weapon) const {
        if (weapon < 0 || weapon >= static_cast<int>(weapon_reloads.size())) return 0;
        const int choice = std::clamp(weapon_reloads[static_cast<size_t>(weapon)], 0, 4);
        if (choice == 0) return 0;
        return OwnsReload(choice - 1) ? choice : 0;
    }
    ReloadStyle ResolvedReloadStyle(int weapon) const {
        const int choice = EquippedReloadChoice(weapon);
        if (choice > 0) return static_cast<ReloadStyle>(choice - 1);
        return SurvivalSkin(EquippedSkin(weapon)).reload;
    }
    int EquippedTorch() const {
        const int id = std::clamp(torch, 0, kSurvivalTorchCount - 1);
        return OwnsTorch(id) ? id : 0;
    }

    std::string Save() const;
    void Load(const std::string& text);
};

int SurvivalToday();
int SurvivalYesterday(int yyyymmdd);

}
