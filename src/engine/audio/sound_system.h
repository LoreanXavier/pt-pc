#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "engine/audio/motion_generator.h"
#include "engine/audio/controller_pcm_capture.h"
#include "engine/audio/subtitles.h"
#include "engine/audio/wwise_bank.h"

namespace pt {
class Vfs;
}

namespace pt::audio {

using GameObjectId = uint64_t;
using PlayingId = uint32_t;

struct AuxSendLevel {
    std::string bus;
    float level = 1.0f;
};

struct SoundSystemStats {
    uint32_t voices = 0;
    uint32_t virtual_voices = 0;
    uint32_t playing_ids = 0;
    uint32_t sequencers = 0;
    float peak_left = 0.0f;
    float peak_right = 0.0f;
};

class SoundEngine;
class AudioOutput;

class SoundSystem {
public:
    SoundSystem();
    ~SoundSystem();
    SoundSystem(const SoundSystem&) = delete;
    SoundSystem& operator=(const SoundSystem&) = delete;

    bool Init(Vfs& vfs, bool open_device, bool surround_output = false);
    void Shutdown();

    PlayingId PostEvent(std::string_view event_name, GameObjectId object = 0);
    PlayingId PostEventId(uint32_t event_id, GameObjectId object = 0);
    PlayingId PostEventMedia(std::string_view event_name, std::vector<uint32_t> media_ids, GameObjectId object = 0);
    PlayingId PostDialogueEvent(std::string_view dialogue_event, std::span<const std::string_view> arguments, GameObjectId object = 0);
    PlayingId PostDialogueEventId(uint32_t dialogue_event_id, std::span<const std::string_view> arguments, GameObjectId object = 0);
    void StopPlayingId(PlayingId id, float fade_seconds = 0.0f, Interp curve = Interp::Linear);
    void SeekPlayingId(PlayingId id, float seconds);
    void StopAll(float fade_seconds = 0.0f);
    bool IsPlaying(PlayingId id) const;
    bool IsEventPlaying(std::string_view event_name) const;
    bool IsEventIdPlaying(uint32_t event_id) const;

    void RegisterObject(GameObjectId object, std::string_view debug_name);
    void UnregisterObject(GameObjectId object);
    void SetObjectTransform(GameObjectId object, const glm::vec3& position, const glm::vec3& forward);
    void SetObjectAuxSends(GameObjectId object, std::span<const AuxSendLevel> sends);
    void SetObjectObstruction(GameObjectId object, float obstruction, float occlusion);
    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);

    void SetState(std::string_view group, std::string_view state);
    void SetStateId(uint32_t group_id, uint32_t state_id);
    void SetSwitch(std::string_view group, std::string_view value, GameObjectId object);
    void SetSwitchId(uint32_t group_id, uint32_t value_id, GameObjectId object);
    void SetRtpc(std::string_view name, float value, GameObjectId object = 0);
    void SetRtpcId(uint32_t parameter_id, float value, GameObjectId object = 0);
    void ResetRtpc(std::string_view name, GameObjectId object = 0);

    PlayingId PlayStream(std::vector<uint8_t> wem_riff, GameObjectId object = 0);
    void SetMarkerCallback(std::function<void(PlayingId, std::string_view label)> callback);
    void SetEndCallback(std::function<void(PlayingId)> callback);

    void Update(float dt);
    void RenderOffline(float* interleaved_stereo, uint32_t frames);

    bool LoadSubtitles(std::string_view language);
    const SubtitleTable& Subtitles() const { return subtitles_; }
    std::vector<std::vector<uint8_t>> SabTables() const;

    void SetRandomSeed(uint32_t seed);
    void SetMasterVolume(float gain);
    SoundSystemStats Stats() const;
    bool HasEvent(uint32_t event_id) const;
    bool DeviceOpen() const;
    uint64_t RenderedFrames() const;
    MotionLevels Motion() const;
    void SetControllerCaptureEvent(uint32_t event_id);
    void SetControllerCaptureEvents(std::span<const uint32_t> event_ids);
    bool TryReadControllerPcm(ControllerPcmBlock& block);
    uint32_t ControllerPcmDroppedBlocks() const;
    void SetFrozen(bool frozen);

private:
    Vfs* vfs_ = nullptr;
    std::unique_ptr<SoundEngine> engine_;
    std::unique_ptr<AudioOutput> output_;
    std::function<void(PlayingId, std::string_view)> marker_callback_;
    std::function<void(PlayingId)> end_callback_;
    SubtitleTable subtitles_;
    std::mutex warn_mutex_;
    std::unordered_set<uint32_t> warned_events_;
    uint32_t solo_event_ = 0;
};

}
