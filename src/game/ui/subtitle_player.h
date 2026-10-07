#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/subtitles.h"

namespace pt {
class Vfs;
}

namespace pt::game {

struct SubtitleView {
    uint32_t key = 0;
    std::string id;
    std::string text;
    int category = 5;
};

class SubtitlePlayer {
public:
    static constexpr int kLanguageCount = 12;

    void Init(Vfs& vfs) { vfs_ = &vfs; }
    void SetLanguage(int language);
    int Language() const { return language_; }

    bool Play(std::string_view subtitle_id, float offset_seconds, std::optional<glm::vec3> source = std::nullopt, uint32_t sound = 0);
    bool PlayKey(uint32_t key, float offset_seconds, std::optional<glm::vec3> source = std::nullopt, uint32_t sound = 0);
    void EndStopped(const std::function<bool(uint32_t)>& alive);
    bool Seek(uint32_t key, float time);
    void Update(float dt);
    void Clear() { active_.clear(); }
    void SetListener(const glm::vec3& position) { listener_ = position; }
    bool Current(std::string_view hidden_id, SubtitleView& out) const;
    struct TranscriptLine {
        float start = 0.0f;
        float end = 0.0f;
        std::string text;
    };
    std::vector<TranscriptLine> Transcript(std::string_view subtitle_id, int language);
    bool Active() const { return !active_.empty(); }

    static int PlayingPriority(int category);
    static float RangeRadius(int range);

private:
    struct LanguageData {
        audio::SubtitleTable table;
        bool loaded = false;
        bool failed = false;
    };
    struct Playing {
        uint32_t key = 0;
        std::string id;
        float time = 0.0f;
        uint64_t order = 0;
        std::optional<glm::vec3> source;
        uint32_t sound = 0;
    };

    LanguageData* Table(int language);
    const audio::SubtitleEntry* Entry(uint32_t key) const;
    bool InRange(const Playing& playing, const audio::SubtitleEntry& entry) const;

    Vfs* vfs_ = nullptr;
    int language_ = 0;
    std::array<std::unique_ptr<LanguageData>, kLanguageCount> languages_;
    std::vector<Playing> active_;
    uint64_t order_ = 0;
    glm::vec3 listener_{0.0f};
};

}
