#include "engine/audio/sound_system.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "engine/audio/audio_engine.h"
#include "engine/audio/sound_engine.h"
#include "engine/core/log.h"
#include "engine/fs/vfs.h"

namespace pt::audio {

SoundSystem::SoundSystem() = default;

SoundSystem::~SoundSystem() {
    Shutdown();
}

bool SoundSystem::Init(Vfs& vfs, bool open_device) {
    Shutdown();
    vfs_ = &vfs;
    auto banks = std::make_unique<SoundBankSet>();
    std::string error;
    if (!banks->Load(vfs, SoundBankSet::DefaultPackages(), &error)) {
        LogError("audio: {}", error);
        return false;
    }
    LogInfo("audio: {} banks, {} media", banks->Banks().size(), banks->AllMedia().size());
    engine_ = std::make_unique<SoundEngine>(std::move(banks));
    engine_->SetRtpc(Fnv1Hash32("volumeRtpc"), 1.0f, 0, false);
    if (const char* solo = std::getenv("PT_SOLO_EVENT")) {
        solo_event_ = Fnv1Hash32(solo);
        LogInfo("audio: only {} ({:08X}) is posted", solo, solo_event_);
    }
    if (const char* set = std::getenv("PT_SET_RTPC")) {
        const std::string text = set;
        const size_t at = text.find('=');
        if (at != std::string::npos) {
            const uint32_t parameter = static_cast<uint32_t>(std::strtoul(text.substr(0, at).c_str(), nullptr, 10));
            const float value = std::strtof(text.substr(at + 1).c_str(), nullptr);
            engine_->SetRtpc(parameter, value, 0, false);
            LogInfo("audio: game parameter {} set to {}", parameter, value);
        }
    }
    if (open_device) {
        auto output = std::make_unique<AudioOutput>();
        SoundEngine* engine = engine_.get();
        if (output->Open(kOutputRate, [engine](float* out, uint32_t frames) { engine->Render(out, frames); })) {
            output_ = std::move(output);
        } else {
            LogWarn("audio: no playback device, offline rendering only");
        }
    }
    return true;
}

void SoundSystem::Shutdown() {
    if (output_) {
        output_->Close();
        output_.reset();
    }
    engine_.reset();
}

PlayingId SoundSystem::PostEvent(std::string_view event_name, GameObjectId object) {
    const uint32_t id = Fnv1Hash32(event_name);
    const PlayingId playing = PostEventId(id, object);
    if (!playing && engine_) {
        std::lock_guard lock(warn_mutex_);
        if (warned_events_.insert(id).second) {
            LogWarn("audio: event {} ({:08X}) not found", event_name, id);
        }
    }
    return playing;
}

PlayingId SoundSystem::PostEventId(uint32_t event_id, GameObjectId object) {
    if (solo_event_ != 0 && event_id != solo_event_) {
        return 0;
    }
    return engine_ ? engine_->PostEvent(event_id, object) : 0;
}

PlayingId SoundSystem::PostEventMedia(std::string_view event_name, std::vector<uint32_t> media_ids, GameObjectId object) {
    return engine_ ? engine_->PostEventMedia(Fnv1Hash32(event_name), object, std::move(media_ids)) : 0;
}

PlayingId SoundSystem::PostDialogueEvent(std::string_view dialogue_event, std::span<const std::string_view> arguments, GameObjectId object) {
    return PostDialogueEventId(Fnv1Hash32(dialogue_event), arguments, object);
}

PlayingId SoundSystem::PostDialogueEventId(uint32_t dialogue_event_id, std::span<const std::string_view> arguments, GameObjectId object) {
    if (!engine_) {
        return 0;
    }
    std::vector<uint32_t> ids;
    for (std::string_view argument : arguments) {
        ids.push_back(argument.empty() ? 0 : Fnv1Hash32(argument));
    }
    return engine_->PostDialogue(dialogue_event_id, std::move(ids), object);
}

void SoundSystem::StopPlayingId(PlayingId id, float fade_seconds, Interp curve) {
    if (engine_) {
        engine_->StopPlayingId(id, fade_seconds, curve);
    }
}

void SoundSystem::SeekPlayingId(PlayingId id, float seconds) {
    if (engine_) {
        engine_->SeekPlayingId(id, seconds);
    }
}

void SoundSystem::StopAll(float fade_seconds) {
    if (engine_) {
        engine_->StopAll(fade_seconds);
    }
}

bool SoundSystem::IsPlaying(PlayingId id) const {
    return engine_ && engine_->IsPlaying(id);
}

bool SoundSystem::IsEventPlaying(std::string_view event_name) const {
    return IsEventIdPlaying(Fnv1Hash32(event_name));
}

bool SoundSystem::IsEventIdPlaying(uint32_t event_id) const {
    return engine_ && engine_->IsEventPlaying(event_id);
}

void SoundSystem::RegisterObject(GameObjectId object, std::string_view debug_name) {
    if (engine_) {
        engine_->RegisterObject(object, std::string(debug_name));
    }
}

void SoundSystem::UnregisterObject(GameObjectId object) {
    if (engine_) {
        engine_->UnregisterObject(object);
    }
}

void SoundSystem::SetObjectTransform(GameObjectId object, const glm::vec3& position, const glm::vec3& forward) {
    if (engine_) {
        engine_->SetObjectTransform(object, position, forward);
    }
}

void SoundSystem::SetObjectAuxSends(GameObjectId object, std::span<const AuxSendLevel> sends) {
    if (!engine_) {
        return;
    }
    std::vector<std::pair<uint32_t, float>> ids;
    for (const auto& send : sends) {
        ids.push_back({Fnv1Hash32(send.bus), send.level});
    }
    engine_->SetObjectAuxSends(object, std::move(ids));
}

void SoundSystem::SetObjectObstruction(GameObjectId object, float obstruction, float occlusion) {
    if (engine_) {
        engine_->SetObjectObstruction(object, obstruction, occlusion);
    }
}

void SoundSystem::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    if (engine_) {
        engine_->SetListener(position, forward, up);
    }
}

void SoundSystem::SetState(std::string_view group, std::string_view state) {
    const bool none = state.empty() || state == "None" || state == "none";
    SetStateId(Fnv1Hash32(group), none ? 0 : Fnv1Hash32(state));
}

void SoundSystem::SetStateId(uint32_t group_id, uint32_t state_id) {
    if (engine_) {
        engine_->SetState(group_id, state_id);
    }
}

void SoundSystem::SetSwitch(std::string_view group, std::string_view value, GameObjectId object) {
    SetSwitchId(Fnv1Hash32(group), Fnv1Hash32(value), object);
}

void SoundSystem::SetSwitchId(uint32_t group_id, uint32_t value_id, GameObjectId object) {
    if (engine_) {
        engine_->SetSwitch(group_id, value_id, object);
    }
}

void SoundSystem::SetRtpc(std::string_view name, float value, GameObjectId object) {
    SetRtpcId(Fnv1Hash32(name), value, object);
}

void SoundSystem::SetRtpcId(uint32_t parameter_id, float value, GameObjectId object) {
    if (engine_) {
        engine_->SetRtpc(parameter_id, value, object, false);
    }
}

void SoundSystem::ResetRtpc(std::string_view name, GameObjectId object) {
    if (engine_) {
        engine_->SetRtpc(Fnv1Hash32(name), 0.0f, object, true);
    }
}

PlayingId SoundSystem::PlayStream(std::vector<uint8_t> wem_riff, GameObjectId object) {
    if (!engine_) {
        return 0;
    }
    auto storage = std::make_shared<const std::vector<uint8_t>>(std::move(wem_riff));
    std::string error;
    auto media = Media::Create(0, storage, 0, storage->size(), &error);
    if (!media) {
        LogError("audio: stream rejected: {}", error);
        return 0;
    }
    if (!engine_->HasEvent(kExternalSourceEvent)) {
        LogError("audio: external source event {:08X} not loaded", kExternalSourceEvent);
        return 0;
    }
    return engine_->PostEvent(kExternalSourceEvent, object, std::move(media));
}

void SoundSystem::SetMarkerCallback(std::function<void(PlayingId, std::string_view label)> callback) {
    marker_callback_ = std::move(callback);
}

void SoundSystem::SetEndCallback(std::function<void(PlayingId)> callback) {
    end_callback_ = std::move(callback);
}

void SoundSystem::Update(float) {
    if (!engine_) {
        return;
    }
    for (const auto& notification : engine_->TakeNotifications()) {
        if (notification.kind == EngineNotification::Kind::Marker) {
            if (marker_callback_) {
                marker_callback_(notification.playing_id, notification.label);
            }
        } else if (end_callback_) {
            end_callback_(notification.playing_id);
        }
    }
}

void SoundSystem::RenderOffline(float* interleaved_stereo, uint32_t frames) {
    if (!engine_ || output_) {
        std::memset(interleaved_stereo, 0, sizeof(float) * frames * 2);
        return;
    }
    engine_->Render(interleaved_stereo, frames);
}

bool SoundSystem::LoadSubtitles(std::string_view language) {
    if (!vfs_ || !engine_) {
        return false;
    }
    std::string error;
    if (!subtitles_.Load(*vfs_, language, engine_->Banks().SabTables(), &error)) {
        LogError("audio: {}", error);
        return false;
    }
    return true;
}

std::vector<std::vector<uint8_t>> SoundSystem::SabTables() const {
    return engine_ ? engine_->Banks().SabTables() : std::vector<std::vector<uint8_t>>();
}

void SoundSystem::SetRandomSeed(uint32_t seed) {
    if (engine_) {
        engine_->SetRandomSeed(seed);
    }
}

void SoundSystem::SetMasterVolume(float gain) {
    if (engine_) {
        engine_->SetMasterGain(gain);
    }
}

SoundSystemStats SoundSystem::Stats() const {
    SoundSystemStats out;
    if (engine_) {
        const EngineStats s = engine_->Stats();
        out.voices = s.voices;
        out.virtual_voices = s.virtual_voices;
        out.playing_ids = s.playing_ids;
        out.sequencers = s.sequencers + s.music;
        out.peak_left = s.peak_left;
        out.peak_right = s.peak_right;
    }
    return out;
}

bool SoundSystem::HasEvent(uint32_t event_id) const {
    return engine_ && engine_->HasEvent(event_id);
}

bool SoundSystem::DeviceOpen() const {
    return output_ != nullptr;
}

uint64_t SoundSystem::RenderedFrames() const {
    return engine_ ? engine_->RenderedFrames() : 0;
}

MotionLevels SoundSystem::Motion() const {
    return engine_ ? engine_->Motion() : MotionLevels{};
}

void SoundSystem::SetFrozen(bool frozen) {
    if (engine_) {
        engine_->SetFrozen(frozen);
    }
}

}
