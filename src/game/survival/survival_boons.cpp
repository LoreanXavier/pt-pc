#include "game/survival/survival_boons.h"

#include <algorithm>

namespace pt::game {

namespace {

constexpr SurvivalBoonSpec kBoons[] = {
    {"pc_boon_quick_hands", 3},   {"pc_boon_deep_pockets", 2}, {"pc_boon_steady_hands", 2}, {"pc_boon_buckshot", 2},
    {"pc_boon_hollow_points", 3}, {"pc_boon_heavy_rounds", 2}, {"pc_boon_second_wind", 3}, {"pc_boon_bloodletting", 3},
    {"pc_boon_long_legs", 2},     {"pc_boon_searing_light", 2}, {"pc_boon_stopping_power", 2}, {"pc_boon_scavenger", 2},
    {"pc_boon_headhunter", 3},    {"pc_boon_lamps_grace", 1},  {"pc_boon_flare_pouch", 2},  {"pc_boon_cold_blood", 2},
    {"pc_boon_last_breath", 1},   {"pc_boon_quick_pump", 2},   {"pc_boon_fan_the_hammer", 2}, {"pc_boon_iron_grip", 2},
};
static_assert(std::size(kBoons) == static_cast<size_t>(SurvivalBoon::Count));

}

const SurvivalBoonSpec& BoonSpec(SurvivalBoon boon) {
    return kBoons[std::min(static_cast<size_t>(boon), std::size(kBoons) - 1)];
}

int SurvivalBoons::Taken() const {
    int n = 0;
    for (const uint8_t r : rank) n += r;
    return n;
}

std::string SurvivalBoons::Save() const {
    std::string s;
    for (const uint8_t r : rank) s.push_back("0123456789abcdef"[std::min<int>(r, 15)]);
    return s;
}

void SurvivalBoons::Load(const std::string& text) {
    rank = {};
    for (size_t i = 0; i < rank.size() && i < text.size(); ++i) {
        const char c = text[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0;
        rank[i] = static_cast<uint8_t>(std::min(v, BoonSpec(static_cast<SurvivalBoon>(i)).ranks));
    }
}

}
