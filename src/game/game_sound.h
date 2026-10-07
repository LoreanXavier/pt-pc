#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/sound_system.h"
#include "engine/vfx/vfx_file.h"
#include "game/game.h"

namespace pt::game {

class GameSound final : public GameAudio {
public:
    static constexpr audio::GameObjectId kPlayerObject = 0x200;
    static constexpr audio::GameObjectId kGimmickObject = 0x300;
    static constexpr audio::GameObjectId kRecordSoundBase = 0x310;
    static constexpr int kRecordSoundCount = 8;
    static constexpr audio::GameObjectId kOneShotBase = 0x10000;
    static constexpr uint32_t kOneShotCount = 48;
    static constexpr audio::GameObjectId kAnimEventBase = 0x7E000000;
    static constexpr audio::GameObjectId kAmbientBase = 0x7C000000;

    explicit GameSound(Game& game) : game_(game) {}

    bool Init(bool open_device, std::string_view language);
    void Shutdown();
    audio::SoundSystem& System() { return system_; }
    bool Ready() const { return ready_; }

    uint32_t PostEvent(std::string_view name, const glm::vec3* position) override;
    uint32_t PostEventId(uint32_t id, const glm::vec3* position) override;
    uint32_t PostEventMedia(std::string_view name, std::vector<uint32_t> media_ids, const glm::vec3* position) override;
    bool IsEventPlaying(std::string_view name) const override;
    bool IsPlaying(uint32_t playing_id) const override;
    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) override;
    void Update(float dt) override;
    void OnStageLoaded(Stage& stage) override;
    void OnStageUnloading(const Stage& stage) override;
    void Footstep(bool left, const glm::vec3& position) override;
    void AnimEvent(std::string_view sound, uint64_t event, const glm::vec3& position) override;
    void StopAll() override;
    uint32_t PlayStream(std::vector<uint8_t> wem, const glm::vec3* position) override;
    void SeekPlayingId(uint32_t playing_id, float seconds) override;
    void StopPlayingId(uint32_t playing_id, float fade_seconds) override;
    void SetState(std::string_view group, std::string_view state) override;
    void SetRtpc(std::string_view name, float value) override;
    uint32_t PostRecordEvent(int record, std::string_view name, const glm::vec3& position) override;
    uint32_t PostRecordEventId(int record, uint32_t id, const glm::vec3& position) override;
    void MoveRecordSound(int record, const glm::vec3& position) override;
    void SetAreaSends(audio::GameObjectId object, const glm::vec3& position);
    audio::PlayingId PostGimmickDialogue(uint32_t dialogue_event, std::span<const std::string_view> arguments, GimmickType source);
    audio::PlayingId PostDialogueAt(uint32_t dialogue_event, std::span<const std::string_view> arguments, const glm::vec3& position);

private:
    struct Emitter {
        uint32_t stage_id = 0;
        audio::GameObjectId object = 0;
        std::string event;
        glm::vec3 position{0.0f};
        glm::vec3 forward{0.0f, 0.0f, 1.0f};
        std::vector<glm::mat4> shapes;
        float range = 0.0f;
        audio::PlayingId playing = 0;
        bool on = false;
    };
    struct VfxSound {
        uint32_t stage_id = 0;
        const fox2::Entity* entity = nullptr;
        std::string play;
        std::string stop;
        bool stop_playing = true;
        float stop_fade = 0.3f;
        uint32_t stop_curve = 4;
        glm::vec3 position{0.0f};
        glm::vec3 forward{0.0f, 0.0f, 1.0f};
        audio::GameObjectId object = 0;
        audio::PlayingId playing = 0;
        bool visible = false;
        float delay = 0.0f;
    };
    struct Area {
        uint32_t stage_id = 0;
        std::string name;
        int priority = 0;
        std::vector<glm::mat4> shapes;
        std::string ambient_event;
        std::string rtpc_name;
        float rtpc_value = 0.0f;
        std::vector<audio::AuxSendLevel> sends;
    };
    struct Edge {
        uint32_t stage_id = 0;
        std::string prev;
        std::string next;
        float fade_ms = 0.0f;
    };
    struct Ambient {
        std::string event;
        audio::GameObjectId object = 0;
        audio::PlayingId playing = 0;
        float fade = 0.0f;
        float rate = 1000.0f;
    };

    struct AnimObject {
        audio::GameObjectId object = 0;
        std::vector<audio::PlayingId> playing;
    };
    struct DialogueObject {
        audio::GameObjectId object = 0;
        audio::PlayingId playing = 0;
        GimmickType source = GimmickType::Bag;
    };

    audio::GameObjectId OneShotObject(const glm::vec3& position);
    void NoteArchiveEvent(std::string_view name);
    const Area* AreaAt(const glm::vec3& position) const;
    void UpdateAnimObjects();
    void UpdateGimmickDialogue();
    void UpdateEmitters();
    void UpdateVfxSounds(float dt);
    audio::PlayingId PostVfxEvent(const std::string& event, audio::GameObjectId object);
    void ReleaseVfxSound(VfxSound& v);
    void UpdateAreas(float dt);

    Game& game_;
    audio::SoundSystem system_;
    bool ready_ = false;
    uint32_t next_one_shot_ = 0;
    audio::GameObjectId next_emitter_ = 0x20000;
    audio::GameObjectId next_anim_object_ = kAnimEventBase;
    std::map<uint64_t, AnimObject> anim_objects_;
    audio::GameObjectId next_dialogue_object_ = 0x7D000000;
    std::vector<DialogueObject> dialogue_objects_;
    glm::vec3 listener_{0.0f};
    std::vector<Emitter> emitters_;
    std::vector<VfxSound> vfx_sounds_;
    std::map<std::string, std::optional<vfx::SoundNode>, std::less<>> sound_nodes_;
    std::vector<Area> areas_;
    std::vector<Edge> edges_;
    std::vector<Ambient> ambients_;
    audio::GameObjectId next_ambient_object_ = kAmbientBase;
    uint32_t current_stage_ = 0;
    std::string current_area_;
    std::string volume_rtpc_ = "volumeRtpc";
    const char* footstep_material_ = nullptr;
};

}
