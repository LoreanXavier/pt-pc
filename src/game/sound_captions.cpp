#include "game/sound_captions.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "engine/core/log.h"

namespace pt::game {
namespace {

constexpr CaptionRule kRules[] = {
    {0xBA6F9BBE, nullptr, "pc_cap_baby_laugh", false, 4.0f, 1, 2},
    {0x8D85EE16, nullptr, "pc_cap_pulse", false, 6.0f, 0, 2},
    {0xDE941744, nullptr, "pc_cap_pulse_end", false, 3.0f, 0, 2},
    {0x70FEA2B2, "Play_tel_10_call", "pc_cap_phone", true, 4.0f, 1, 2},
    {0x0C683F5C, "Play_sfx_clock_bell", "pc_cap_bell", false, 4.0f, 1, 2},
    {0xF27DAC95, nullptr, "pc_cap_clear", false, 4.0f, 1, 2},
    {0x5252493F, nullptr, "pc_cap_low_tone", false, 3.0f, 1, 2},
    {0xA5FDFC86, nullptr, "pc_cap_gouge", false, 3.0f, 0, 2},
    {0xE4B855B2, nullptr, "pc_cap_photo", false, 3.0f, 1, 2},
    {0xD567DD28, "Play_bgm_herald01", "pc_cap_herald", false, 5.0f, 1, 2},
    {0xC910062E, "Play_voice_ene_breath_01", "pc_cap_lisa_breath", true, 4.0f, 1, 2},
    {0x9983E7F9, "Play_voice_ene_cry_f020", "pc_cap_lisa_cry", true, 4.0f, 1, 2},
    {0xAD52F3C2, "Play_voice_ene_cry_s_01", "pc_cap_lisa_cry", true, 4.0f, 1, 2},
    {0x0CB2A9B7, "Play_voice_ene_cry_l_01", "pc_cap_lisa_cry", true, 4.0f, 1, 2},
    {0x958C5411, "Play_voice_ene_laugh_01", "pc_cap_lisa_laugh", true, 4.0f, 1, 2},
    {0x7B136101, "Play_ene_fs_chase_01", "pc_cap_steps_fast", true, 3.0f, 1, 2},
    {0xB8D5B537, "Play_ene_fs_f040_01", "pc_cap_steps", true, 4.0f, 1, 2},
    {0xF9E2D015, "Play_ene_fs_f060_01", "pc_cap_steps", true, 4.0f, 1, 2},
    {0x52B9C804, "Play_ene_fs_f070_01", "pc_cap_steps", true, 4.0f, 1, 2},
    {0x848AAACE, "Play_man_fs_f100_01", "pc_cap_steps_heavy", true, 4.0f, 1, 2},
    {0xC35146B3, "Play_voice_f030_baby_cry_s_lp", "pc_cap_baby_cry", true, 4.0f, 1, 1},
    {0x3B0C4560, "Play_voice_f060_baby_cry_s_lp", "pc_cap_baby_cry", true, 4.0f, 1, 1},
    {0x33794C06, "Play_voice_f060_baby_cry_m_lp", "pc_cap_baby_cry", true, 4.0f, 1, 1},
    {0xA8F0DD81, "Play_voice_f060_baby_cry_l_lp", "pc_cap_baby_cry", true, 4.0f, 1, 1},
    {0xE4388725, "Play_voice_f070_baby_cry_m_lp", "pc_cap_baby_cry", true, 4.0f, 1, 1},
    {0x4EC0C30A, "Play_voice_f070_baby_cry_l_lp", "pc_cap_baby_cry", true, 4.0f, 1, 1},
    {0xD7A9224D, "Play_voice_f090_baby_cry_l", "pc_cap_baby_cry", true, 4.0f, 1, 1},
    {0x4B307DCC, "Play_voice_f090_baby_cry_l_lp", "pc_cap_baby_cry", true, 4.0f, 1, 1},
    {0xCDD941C6, "Play_voice_f080_baby_cry_s_lp", "pc_cap_baby_fridge", true, 4.0f, 1, 1},
    {0xF24E571D, "Play_sfx_door_struck_01", "pc_cap_knock", true, 3.0f, 1, 1},
    {0xF24E571E, "Play_sfx_door_struck_02", "pc_cap_knock", true, 3.0f, 1, 1},
    {0xDCA2AB1F, "Play_sfx_head_struck_01", "pc_cap_thud", true, 3.0f, 0, 1},
    {0xDCA2AB1C, "Play_sfx_head_struck_02", "pc_cap_thud", true, 3.0f, 0, 1},
    {0x22072942, "Play_sfx_door_hit_01", "pc_cap_door_rattle", false, 2.5f, 0, 1},
    {0xA4D5B591, "Play_sfx_door_open_01", "pc_cap_door_open", true, 2.5f, 0, 0},
    {0xA4D5B592, "Play_sfx_door_open_02", "pc_cap_door_open", true, 2.5f, 0, 0},
    {0xA4D5B593, "Play_sfx_door_open_03", "pc_cap_door_open", true, 2.5f, 0, 0},
    {0xA4D5B594, "Play_sfx_door_open_04", "pc_cap_door_open", true, 2.5f, 0, 0},
    {0xA4D5B595, "Play_sfx_door_open_05", "pc_cap_door_open", true, 2.5f, 0, 0},
    {0xA4D5B596, "Play_sfx_door_open_06", "pc_cap_door_open", true, 2.5f, 0, 0},
    {0xA4D5B597, "Play_sfx_door_open_07", "pc_cap_door_open", true, 2.5f, 0, 0},
    {0xF02CAD37, "Play_sfx_door_close_01", "pc_cap_door_slam", true, 2.5f, 0, 0},
    {0xF02CAD34, "Play_sfx_door_close_02", "pc_cap_door_slam", true, 2.5f, 0, 0},
    {0xF02CAD35, "Play_sfx_door_close_03", "pc_cap_door_slam", true, 2.5f, 0, 0},
    {0x715D933C, "Play_sfx_wood_dr_creak_l_01", "pc_cap_door_creak", true, 3.0f, 0, 0},
    {0x237160FD, "Play_sfx_wood_dr_creak_s_01", "pc_cap_door_creak", true, 3.0f, 0, 0},
    {0x0BAAD84D, "Play_sfx_windows_break", "pc_cap_window", true, 3.0f, 2, 2},
    {0xFA72BDCE, "Play_sfx_light_off_01", "pc_cap_lights_off", false, 2.5f, 0, 0},
    {0x700D6EC0, "Play_sfx_light_on_01", "pc_cap_lights_on", false, 2.5f, 0, 0},
    {0xF1A9F09A, "Play_sfx_heartbeat", "pc_cap_heartbeat", false, 4.0f, 0, 0},
    {0x82963C93, "Play_sfx_house_creak_01", "pc_cap_creak", false, 4.0f, 0, 0},
    {0x255ECF63, "Play_radio_noise_lp", "pc_cap_radio_static", true, 4.0f, 0, 0},
    {0x4810A725, "Play_sfx_bug_loop_01", "pc_cap_static", false, 4.0f, 0, 0},
    {0x270D9BAC, "PT_Hint_Soft_Notes", "pc_cap_soft_notes", false, 3.5f, 0, 2},
    {0xE5067FE7, "PT_Hint_Soft_Heartbeat", "pc_cap_soft_heartbeat", false, 2.0f, 0, 2},
    {0x735D8E0D, "PT_Hint_Sad_Note", "pc_cap_sad_note", false, 4.0f, 0, 2},
    {0x9E69FB14, "PT_Intercom_Chime", "pc_cap_intercom_chime", true, 3.5f, 0, 1},
    {0x568431F2, "PT_Intercom_Hum", "pc_cap_intercom_hum", true, 4.0f, 0, 0},
    {0x44859513, "PT_Intercom_Crackle", "pc_cap_intercom_crackle", true, 2.5f, 0, 1},
};

}

std::span<const CaptionRule> SoundCaptions::Rules() {
    return kRules;
}

const CaptionRule* SoundCaptions::Find(uint32_t event) {
    for (const CaptionRule& rule : kRules) {
        if (rule.event == event) {
            return &rule;
        }
    }
    return nullptr;
}

CaptionSide SoundCaptions::SideOf(const glm::vec3& listener, const glm::vec3& forward, const glm::vec3& source) {
    glm::vec2 to(source.x - listener.x, source.z - listener.z);
    glm::vec2 facing(forward.x, forward.z);
    const float distance = glm::length(to);
    const float length = glm::length(facing);
    if (distance < 1.0f || length < 1.0e-4f) {
        return CaptionSide::None;
    }
    to /= distance;
    facing /= length;
    const float along = glm::dot(to, facing);
    const float across = to.x * -facing.y + to.y * facing.x;
    if (along >= 0.7071f) {
        return CaptionSide::Ahead;
    }
    if (along <= -0.7071f) {
        return CaptionSide::Behind;
    }
    return across >= 0.0f ? CaptionSide::Right : CaptionSide::Left;
}

float SoundCaptions::Line::Alpha() const {
    const float hold = rule ? rule->hold : 0.0f;
    return std::clamp((hold - age) / 0.5f, 0.0f, 1.0f);
}

void SoundCaptions::SetEnabled(bool on) {
    if (on == enabled_) {
        return;
    }
    enabled_ = on;
    log_posts_ = on && std::getenv("PT_LOG_SOUND_POSTS") != nullptr;
    if (!on) {
        Clear();
    }
}

void SoundCaptions::Clear() {
    lines_.clear();
    pulse_ = 0.0f;
}

void SoundCaptions::Post(uint32_t event, const std::optional<glm::vec3>& source) {
    if (skip_ != 0 && event == skip_) {
        skip_ = 0;
        return;
    }
    if (!enabled_) {
        return;
    }
    if (log_posts_) {
        LogInfo("sound caption: post {:08X}{}", event, Find(event) ? " (has a caption)" : "");
    }
    const CaptionRule* rule = Find(event);
    if (!rule) {
        return;
    }
    ++posted_;
    const std::string_view key = rule->key;
    for (Line& line : lines_) {
        if (std::string_view(line.rule->key) == key) {
            line.rule = rule;
            line.age = 0.0f;
            line.source = rule->positional ? source : std::nullopt;
            return;
        }
    }
    if (lines_.size() >= kMaxLines) {
        auto victim = lines_.begin();
        for (auto it = lines_.begin(); it != lines_.end(); ++it) {
            if (it->rule->priority < victim->rule->priority) {
                victim = it;
            }
        }
        if (victim->rule->priority > rule->priority) {
            return;
        }
        lines_.erase(victim);
    }
    Line line;
    line.rule = rule;
    line.source = rule->positional ? source : std::nullopt;
    lines_.push_back(line);
    LogInfo("sound caption: {} ({}){}", rule->key, rule->name ? rule->name : "unnamed event", source && rule->positional ? " with a place" : "");
    if (rule->pulse > 0) {
        pulse_ = kPulseSeconds;
        pulse_strength_ = rule->pulse >= 2 ? 0.7f : 0.35f;
    }
}

void SoundCaptions::Update(float dt, const glm::vec3& listener, const glm::vec3& forward) {
    for (Line& line : lines_) {
        line.age += dt;
        line.side = line.source ? SideOf(listener, forward, *line.source) : CaptionSide::None;
    }
    std::erase_if(lines_, [](const Line& line) { return line.rule && line.age >= line.rule->hold; });
}

std::pair<uint8_t, uint8_t> SoundCaptions::Pulse(float dt) {
    if (pulse_ <= 0.0f) {
        return {0, 0};
    }
    const float level = pulse_strength_ * (pulse_ / kPulseSeconds);
    pulse_ = std::max(0.0f, pulse_ - dt);
    const uint8_t small = static_cast<uint8_t>(std::clamp(level, 0.0f, 1.0f) * 255.0f);
    const uint8_t large = pulse_strength_ > 0.5f ? static_cast<uint8_t>(small / 3) : uint8_t{0};
    return {large, small};
}

}
