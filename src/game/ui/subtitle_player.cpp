#include "game/ui/subtitle_player.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

#include "engine/core/log.h"
#include "engine/fs/vfs.h"

namespace pt::game {
namespace {

constexpr const char* kSubtitleLanguages[SubtitlePlayer::kLanguageCount] = {"Eng", "Fre", "Ger", "Spa", "Jpn", "Ita", "Por", "Tur", "Zhs", "Ara", "Rus", "Ukr", "Ces", "Pol"};

constexpr int kPlayingPriority[8] = {255, 0, 0, 10, 20, 30, 40, 0};
constexpr float kRangeRadius[4] = {0.0f, 20.0f, 40.0f, 70.0f};

constexpr uint16_t kCp1252[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                                  0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

bool ValidUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        const size_t extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 4;
        if (extra == 4 || (extra > 0 && i + extra >= text.size())) {
            return false;
        }
        for (size_t k = 1; k <= extra; ++k) {
            if ((static_cast<uint8_t>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

std::string Cp1252ToUtf8(std::string_view text) {
    std::string out;
    for (const char ch : text) {
        const uint8_t c = static_cast<uint8_t>(ch);
        const uint32_t code = c >= 0x80 && c < 0xA0 ? kCp1252[c - 0x80] : c;
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

}

int SubtitlePlayer::PlayingPriority(int category) {
    return category >= 0 && category < 8 ? kPlayingPriority[category] : 255;
}

float SubtitlePlayer::RangeRadius(int range) {
    return range >= 0 && range < 4 ? kRangeRadius[range] : 0.0f;
}

void SubtitlePlayer::SetLanguage(int language) {
    language_ = std::clamp(language, 0, kLanguageCount - 1);
    Table(language_);
}

SubtitlePlayer::LanguageData* SubtitlePlayer::Table(int language) {
    if (!vfs_ || language < 0 || language >= kLanguageCount) {
        return nullptr;
    }
    if (!languages_[language]) {
        languages_[language] = std::make_unique<LanguageData>();
    }
    LanguageData& lang = *languages_[language];
    if (!lang.loaded && !lang.failed) {
        std::string error;
        if (!lang.table.Load(*vfs_, kSubtitleLanguages[language], {}, &error)) {
            LogError("ui: subtitles {}: {}", kSubtitleLanguages[language], error);
            lang.failed = true;
            return nullptr;
        }
        lang.loaded = true;
        LogInfo("ui: subtitles {} loaded ({} entries)", kSubtitleLanguages[language], lang.table.Entries().size());
    }
    return &lang;
}

const audio::SubtitleEntry* SubtitlePlayer::Entry(uint32_t key) const {
    const LanguageData* lang = languages_[language_].get();
    if (lang && lang->loaded) {
        if (const audio::SubtitleEntry* entry = lang->table.FindByKey(key)) {
            return entry;
        }
    }
    const LanguageData* english = languages_[0].get();
    return english && english->loaded ? english->table.FindByKey(key) : nullptr;
}

bool SubtitlePlayer::InRange(const Playing& playing, const audio::SubtitleEntry& entry) const {
    const float radius = RangeRadius(entry.range);
    if (radius <= 0.0f) {
        return true;
    }
    if (!playing.source) {
        return false;
    }
    const glm::vec3 d = *playing.source - listener_;
    return glm::dot(d, d) <= radius * radius;
}

bool SubtitlePlayer::Play(std::string_view subtitle_id, float offset_seconds, std::optional<glm::vec3> source, uint32_t sound) {
    std::string id(subtitle_id);
    std::transform(id.begin(), id.end(), id.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    const uint32_t key = audio::SubtitleTable::SubtitleKey(id);
    if (!PlayKey(key, offset_seconds, source, sound)) {
        return false;
    }
    active_.back().id = id;
    return true;
}

bool SubtitlePlayer::PlayKey(uint32_t key, float offset_seconds, std::optional<glm::vec3> source, uint32_t sound) {
    Table(language_);
    Table(0);
    const audio::SubtitleEntry* entry = Entry(key);
    if (!entry || entry->lines.empty()) {
        LogWarn("ui: subtitle {:#x} not found", key);
        return false;
    }
    active_.erase(std::remove_if(active_.begin(), active_.end(), [&](const Playing& p) { return p.key == key; }), active_.end());
    Playing playing;
    playing.key = key;
    playing.id = entry->id;
    playing.time = offset_seconds;
    playing.order = ++order_;
    playing.source = source;
    playing.sound = sound;
    active_.push_back(playing);
    const float radius = RangeRadius(entry->range);
    LogInfo("ui: subtitle {:#x} {} started at {:.2f} s, category {}, character {}, range {} ({})", key, entry->id, offset_seconds, entry->category,
            entry->character, entry->range, radius > 0.0f ? std::format("shown within {} m of its voice", radius) : std::string("no distance limit"));
    return true;
}

void SubtitlePlayer::EndStopped(const std::function<bool(uint32_t)>& alive) {
    std::erase_if(active_, [&](const Playing& p) {
        if (p.sound == 0 || alive(p.sound)) return false;
        LogInfo("ui: subtitle {:#x} {} ended with its sound at {:.2f} s", p.key, p.id, p.time);
        return true;
    });
}

bool SubtitlePlayer::Seek(uint32_t key, float time) {
    for (Playing& p : active_) {
        if (p.key == key) {
            p.time = time;
            return true;
        }
    }
    return false;
}

void SubtitlePlayer::Update(float dt) {
    for (Playing& p : active_) {
        p.time += dt;
    }
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                 [&](const Playing& p) {
                                     const audio::SubtitleEntry* entry = Entry(p.key);
                                     return !entry || entry->lines.empty() || p.time >= entry->lines.back().end_seconds;
                                 }),
                  active_.end());
}

std::vector<SubtitlePlayer::TranscriptLine> SubtitlePlayer::Transcript(std::string_view subtitle_id, int language) {
    std::string id(subtitle_id);
    std::transform(id.begin(), id.end(), id.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    const uint32_t key = audio::SubtitleTable::SubtitleKey(id);
    const audio::SubtitleEntry* entry = nullptr;
    for (const int l : {language, 0}) {
        if (const LanguageData* data = Table(l); data && data->loaded && !entry) entry = data->table.FindByKey(key);
    }
    std::vector<TranscriptLine> out;
    for (const audio::SubtitleLine& line : entry ? entry->lines : std::vector<audio::SubtitleLine>{}) {
        std::string text = ValidUtf8(line.text) ? line.text : Cp1252ToUtf8(line.text);
        while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
        std::replace(text.begin(), text.end(), '\n', ' ');
        out.push_back({line.start_seconds, line.end_seconds, std::move(text)});
    }
    return out;
}

bool SubtitlePlayer::Current(std::string_view hidden_id, SubtitleView& out) const {
    const Playing* best = nullptr;
    int best_priority = 0;
    for (const Playing& p : active_) {
        if (!hidden_id.empty() && p.id == hidden_id) {
            continue;
        }
        const audio::SubtitleEntry* candidate = Entry(p.key);
        if (!candidate || !InRange(p, *candidate)) {
            continue;
        }
        const int priority = PlayingPriority(candidate->category);
        if (!best || priority < best_priority || (priority == best_priority && p.order > best->order)) {
            best = &p;
            best_priority = priority;
        }
    }
    if (!best) {
        return false;
    }
    const audio::SubtitleEntry* entry = Entry(best->key);
    if (!entry) {
        return false;
    }
    for (const audio::SubtitleLine& line : entry->lines) {
        if (best->time >= line.start_seconds && best->time < line.end_seconds) {
            out.key = best->key;
            out.id = best->id;
            out.text = ValidUtf8(line.text) ? line.text : Cp1252ToUtf8(line.text);
            out.category = entry->category;
            return !out.text.empty();
        }
    }
    return false;
}

}
