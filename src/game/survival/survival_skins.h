#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <glm/vec3.hpp>

namespace pt {
class TextureManager;
}

namespace pt::game {

inline constexpr int kSurvivalSkinCount = 64;
inline constexpr int kSurvivalTorchCount = 5;
inline constexpr int kSurvivalDailyMutatorCount = 8;
inline constexpr int kMythicAnimFrames = 12;

enum class SkinRarity : uint8_t {
    Factory = 0,
    Tactical = 1,
    Artisan = 2,
    Horror = 3,
    Mythic = 4,
};

enum class ReloadStyle : uint8_t {
    Standard = 0,
    Tactical = 1,
    Gunslinger = 2,
    Telekinetic = 3,
};

enum class SkinPattern : uint8_t {
    FactoryOriginal = 0,
    BrushedMetal,
    CarbonWeave,
    DigitalCamo,
    OrganicCamo,
    TigerStripe,
    HexGrid,
    SplinterCamo,
    HazardStripe,
    Damascus,
    CaseHardened,
    Filigree,
    Marble,
    BloodSplatter,
    SilentRust,
    AsylumTile,
    Glitch204863,
    CrackedMagma,
    AnodizedFade,
    BoneCrack,
};

struct SurvivalSkinDef {
    int id = 0;
    const char* name = "";
    SkinRarity rarity = SkinRarity::Factory;
    ReloadStyle reload = ReloadStyle::Standard;
    SkinPattern pattern = SkinPattern::BrushedMetal;
    int price = 0;
    int milestone_wave = 0;
    int daily_wins_need = 0;
    glm::vec3 metal_a{0.16f};
    glm::vec3 metal_b{0.08f};
    glm::vec3 grip_a{0.18f, 0.10f, 0.06f};
    glm::vec3 grip_b{0.10f, 0.05f, 0.03f};
    glm::vec3 accent{0.42f, 0.36f, 0.24f};
    glm::vec3 metal_srm{0.75f, 0.32f, 0.14f};
    glm::vec3 grip_srm{0.22f, 0.72f, 0.02f};
    glm::vec3 accent_srm{0.85f, 0.22f, 0.25f};
    glm::vec3 aura_color{0.95f, 0.18f, 0.14f};
};

struct SurvivalTorchDef {
    int id = 0;
    const char* name_key = "";
    int price = 0;
    int milestone_wave = 0;
    int daily_wins_need = 0;
    glm::vec3 tint{1.0f, 1.0f, 1.0f};
};

struct DailyMutatorDef {
    int id = 0;
    const char* name_key = "";
    const char* note_key = "";
    float credits_mult = 1.4f;
    float enemy_speed_mult = 1.0f;
    float enemy_hp_mult = 1.0f;
    float enemy_count_mult = 1.0f;
    float player_hp_mult = 1.0f;
    float damage_mult = 1.0f;
    float headshot_mult = 1.0f;
    float reload_speed_mult = 1.0f;
    float fire_rate_mult = 1.0f;
    float ammo_cap_mult = 1.0f;
    float stun_mult = 1.0f;
    bool red_lamps = false;
    bool half_lamps = false;
    bool dense_fog = false;
    bool heavy_iron = false;
    bool lisa_stalks = false;
    int starting_boon = -1;
};

struct SurvivalPayoutInput {
    int waves_cleared = 0;
    int kills = 0;
    int headshots = 0;
    int best_chain = 0;
    int missions_done = 0;
    int bosses_down = 0;
    int difficulty = 1;
    bool daily = false;
    int daily_mutator = 0;
    bool daily_goal_fresh = false;
    int daily_streak = 0;
};

struct SurvivalPayout {
    int wave_credits = 0;
    int combat_credits = 0;
    float multiplier = 1.0f;
    int daily_bonus = 0;
    int total = 0;
};

struct SkinTextureSet {
    uint32_t metal_albedo = 0;
    uint32_t metal_srm = 0;
    uint32_t grip_albedo = 0;
    uint32_t grip_srm = 0;
    uint32_t accent_albedo = 0;
    uint32_t accent_srm = 0;
};

struct MythicAnimSet {
    std::array<uint32_t, kMythicAnimFrames> metal_albedo{};
    std::array<uint32_t, kMythicAnimFrames> accent_albedo{};
    uint32_t metal_srm = 0;
    uint32_t grip_albedo = 0;
    uint32_t grip_srm = 0;
    uint32_t accent_srm = 0;
};

struct TorchTextureSet {
    uint32_t body_albedo = 0;
    uint32_t body_srm = 0;
};

const std::array<SurvivalSkinDef, kSurvivalSkinCount>& SurvivalSkins();
const SurvivalSkinDef& SurvivalSkin(int id);

const std::array<SurvivalTorchDef, kSurvivalTorchCount>& SurvivalTorches();
const SurvivalTorchDef& SurvivalTorch(int id);

const std::array<DailyMutatorDef, kSurvivalDailyMutatorCount>& DailyMutators();
const DailyMutatorDef& DailyMutatorById(int id);
const DailyMutatorDef& DailyMutatorForDate(int yyyymmdd);

const char* SkinRarityKey(SkinRarity rarity);
const char* ReloadStyleKey(ReloadStyle style);

SurvivalPayout ComputeSurvivalCredits(const SurvivalPayoutInput& in);

SkinTextureSet EnsureSkinTextures(TextureManager& textures, int skin_id);

MythicAnimSet EnsureMythicAnimTextures(TextureManager& textures, int skin_id);

glm::vec3 MythicLiveColor(int skin_id, float time_sec);

TorchTextureSet EnsureTorchTextures(TextureManager& textures, int torch_id);

}
