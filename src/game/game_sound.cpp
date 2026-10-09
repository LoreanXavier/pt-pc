#include "game/game_sound.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "engine/core/log.h"
#include "engine/vfx/vfx_file.h"

namespace pt::game {
namespace {

const char* FootstepMaterial(uint32_t geom_material) {
    switch (geom_material) {
    case 0x59D11AF1: return "tile";
    case 0x5F263606: return "conc";
    case 0x2D2AEFA4: return "BLOOD";
    default: return "wood";
    }
}

std::string VfxEvent(std::span<const uint8_t> data, std::string_view prefix) {
    for (size_t i = 2; i + prefix.size() < data.size(); ++i) {
        if (std::memcmp(data.data() + i, prefix.data(), prefix.size()) != 0) {
            continue;
        }
        const size_t length = data[i - 2] | (static_cast<size_t>(data[i - 1]) << 8);
        if (length > prefix.size() && length < 128 && i + length <= data.size()) {
            return std::string(reinterpret_cast<const char*>(data.data() + i), length);
        }
    }
    return {};
}

bool InsideBox(const glm::mat4& box, const glm::vec3& p) {
    const glm::vec3 local(glm::inverse(box) * glm::vec4(p, 1.0f));
    return std::abs(local.x) <= 0.5f && std::abs(local.y) <= 0.5f && std::abs(local.z) <= 0.5f;
}

glm::vec3 ClosestOnBox(const glm::mat4& box, const glm::vec3& p) {
    glm::vec3 local(glm::inverse(box) * glm::vec4(p, 1.0f));
    local = glm::clamp(local, glm::vec3(-0.5f), glm::vec3(0.5f));
    return glm::vec3(box * glm::vec4(local, 1.0f));
}

glm::vec3 EntityForward(const glm::mat4& world) {
    const glm::vec3 z(world[2]);
    const float length = glm::length(z);
    return length > 1e-6f ? z / length : glm::vec3(0.0f, 0.0f, 1.0f);
}

std::vector<glm::mat4> ShapeTransforms(const Stage& stage, const fox2::DataSetFile& f, const fox2::Entity& e, const char* property) {
    std::vector<glm::mat4> out;
    if (const fox2::Property* p = f.FindProperty(e, property)) {
        for (size_t i = 0; i < p->Count(); ++i) {
            if (const fox2::Entity* shape = f.ElementEntity(*p, i)) {
                out.push_back(stage.ToWorld(f.WorldTransform(*shape)));
            }
        }
    }
    return out;
}

}

bool GameSound::Init(bool open_device, std::string_view language, bool surround) {
    ready_ = system_.Init(game_.GetVfs(), open_device, surround);
    if (!ready_) {
        LogError("sound: init failed");
        return false;
    }
    system_.RegisterObject(0, "Global");
    system_.RegisterObject(kPlayerObject, "Player");
    system_.RegisterObject(kGimmickObject, "Gimmick");
    for (int i = 0; i < kRecordSoundCount; ++i) {
        system_.RegisterObject(kRecordSoundBase + i, "Gimmick");
    }
    for (uint32_t i = 0; i < kOneShotCount; ++i) {
        system_.RegisterObject(kOneShotBase + i, "OneShot");
    }
    system_.LoadSubtitles(language);
    system_.SetMarkerCallback([this](audio::PlayingId playing, std::string_view label) {
        const std::string id = system_.Subtitles().SubtitleIdForMarker(label);
        LogInfo("sound: marker {} -> subtitle {}", label, id.empty() ? "none" : id);
        if (!id.empty()) {
            // the subtitle follows its sound: a sound stopped early (a stop trap, Set_state_game_over's Stop_ALL after Lisa's
            // kill, a stage unload) ends it (SubtitlePlayer::EndStopped)
            game_.QueueSubtitle(id, 0.0f, playing);
            // the Archive's spoken lines (archive.h): a line heard in play opens its entry
            std::string key = "voice:" + id;
            std::transform(key.begin(), key.end(), key.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            game_.NoteArchive(key);
        }
    });
    LogInfo("sound: ready (device {})", system_.DeviceOpen());
    return true;
}

void GameSound::Shutdown() {
    if (ready_) {
        system_.Shutdown();
        ready_ = false;
    }
    dialogue_objects_.clear();
    one_shot_objects_.Reset();
}

audio::GameObjectId GameSound::OneShotObject(const glm::vec3& position) {
    const size_t previous_size = one_shot_objects_.Size();
    const audio::GameObjectId id = one_shot_objects_.Acquire([&](audio::PlayingId playing) { return system_.IsPlaying(playing); });
    if (one_shot_objects_.Size() != previous_size) {
        system_.RegisterObject(id, "OneShot");
    }
    system_.SetObjectTransform(id, position, glm::vec3(0.0f, 0.0f, 1.0f));
    SetAreaSends(id, position);
    return id;
}

// 0xF9B040: groups sorted by ascending priority (0x1392650), the first one containing the point wins
const GameSound::Area* GameSound::AreaAt(const glm::vec3& position) const {
    const Area* best = nullptr;
    for (const Area& area : areas_) {
        for (const glm::mat4& shape : area.shapes) {
            if (InsideBox(shape, position) && (!best || area.priority < best->priority)) {
                best = &area;
                break;
            }
        }
    }
    return best;
}

// 0xFA9680, 0xFA8DC0: the aux sends of the area at the object's own position (0xF9B040)
void GameSound::SetAreaSends(audio::GameObjectId object, const glm::vec3& position) {
    const Area* area = AreaAt(position);
    system_.SetObjectAuxSends(object, area ? std::span<const audio::AuxSendLevel>(area->sends) : std::span<const audio::AuxSendLevel>());
}

uint32_t GameSound::PostEvent(std::string_view name, const glm::vec3* position) {
    if (!ready_) {
        return 0;
    }
    NoteArchiveEvent(name);
    const audio::GameObjectId object = position ? OneShotObject(*position) : 0;
    const audio::PlayingId playing = system_.PostEvent(name, object);
    if (position) one_shot_objects_.Track(object, playing);
    return playing;
}

// the Archive's sounds without a transcript (archive.h, unlock key event:<name>): the event posted in play opens the entry
void GameSound::NoteArchiveEvent(std::string_view name) {
    if (name.starts_with("Play_")) {
        game_.NoteArchive("event:" + std::string(name));
    }
}

uint32_t GameSound::PostEventId(uint32_t id, const glm::vec3* position) {
    if (!ready_) {
        return 0;
    }
    const audio::GameObjectId object = position ? OneShotObject(*position) : 0;
    const audio::PlayingId playing = system_.PostEventId(id, object);
    if (position) one_shot_objects_.Track(object, playing);
    return playing;
}

uint32_t GameSound::PostEventMedia(std::string_view name, std::vector<uint32_t> media_ids, const glm::vec3* position) {
    if (!ready_) {
        return 0;
    }
    const audio::GameObjectId object = position ? OneShotObject(*position) : 0;
    const audio::PlayingId playing = system_.PostEventMedia(name, std::move(media_ids), object);
    if (position) one_shot_objects_.Track(object, playing);
    return playing;
}

uint32_t GameSound::PostRecordEvent(int record, std::string_view name, const glm::vec3& position) {
    if (!ready_ || record < 0 || record >= kRecordSoundCount) {
        return ready_ ? PostEvent(name, &position) : 0;
    }
    MoveRecordSound(record, position);
    return system_.PostEvent(name, kRecordSoundBase + record);
}

uint32_t GameSound::PostRecordEventId(int record, uint32_t id, const glm::vec3& position) {
    if (!ready_ || record < 0 || record >= kRecordSoundCount) {
        return ready_ ? PostEventId(id, &position) : 0;
    }
    MoveRecordSound(record, position);
    return system_.PostEventId(id, kRecordSoundBase + record);
}

// the record's slot of the ShGimmick sound control follows its connect point or body while its sounds play (0x954270; Lisa's
// steps and breath, 0x12888D0: +0xC0 sets her slot's position, +0x78 moves the voices); its area sends follow too
void GameSound::MoveRecordSound(int record, const glm::vec3& position) {
    if (!ready_ || record < 0 || record >= kRecordSoundCount) {
        return;
    }
    const audio::GameObjectId object = kRecordSoundBase + record;
    system_.SetObjectTransform(object, position, glm::vec3(0.0f, 0.0f, 1.0f));
    SetAreaSends(object, position);
}

bool GameSound::IsEventPlaying(std::string_view name) const {
    return ready_ && system_.IsEventPlaying(name);
}

bool GameSound::IsPlaying(uint32_t playing_id) const {
    return ready_ && system_.IsPlaying(playing_id);
}

void GameSound::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    listener_ = position;
    if (ready_) {
        system_.SetListener(position, forward, up);
        system_.SetObjectTransform(kPlayerObject, game_.GetPlayer().Feet(), game_.GetPlayer().BodyForward());
    }
}

void GameSound::StopAll() {
    if (ready_) {
        system_.StopAll(0.0f);
    }
    for (Emitter& e : emitters_) {
        e.playing = 0;
    }
    for (const Ambient& a : ambients_) {
        if (ready_) {
            system_.UnregisterObject(a.object);
        }
    }
    ambients_.clear();
    for (const DialogueObject& d : dialogue_objects_) {
        if (ready_) system_.UnregisterObject(d.object);
    }
    dialogue_objects_.clear();
    current_stage_ = 0;
    current_area_.clear();
}

uint32_t GameSound::PlayStream(std::vector<uint8_t> wem, const glm::vec3* position) {
    if (!ready_) {
        return 0;
    }
    const audio::GameObjectId object = position ? OneShotObject(*position) : 0;
    const audio::PlayingId playing = system_.PlayStream(std::move(wem), object);
    if (position) one_shot_objects_.Track(object, playing);
    return playing;
}

void GameSound::SeekPlayingId(uint32_t playing_id, float seconds) {
    if (ready_ && playing_id) {
        system_.SeekPlayingId(playing_id, seconds);
    }
}

void GameSound::StopPlayingId(uint32_t playing_id, float fade_seconds) {
    if (ready_ && playing_id) {
        system_.StopPlayingId(playing_id, fade_seconds);
    }
}

void GameSound::SetState(std::string_view group, std::string_view state) {
    if (ready_) {
        system_.SetState(group, state);
    }
}

void GameSound::SetRtpc(std::string_view name, float value) {
    if (ready_) {
        system_.SetRtpc(name, value);
    }
}

void GameSound::Footstep(bool left, const glm::vec3& position) {
    if (!ready_) {
        return;
    }
    system_.SetObjectTransform(kPlayerObject, position, game_.GetPlayer().BodyForward());
    SetAreaSends(kPlayerObject, position);
    const uint32_t surface = game_.SurfaceMaterial(position);
    const char* material = FootstepMaterial(surface);
    if (material != footstep_material_ || surface != footstep_surface_) {
        footstep_material_ = material;
        footstep_surface_ = surface;
        LogDebug("sound: footstep material {} (surface {:#x} at {:.2f} {:.2f} {:.2f})", material, surface, position.x, position.y, position.z);
    }
    system_.SetSwitch("Material", material, kPlayerObject);
    system_.PostEvent(left ? "Play_plr_footstep_wk_l" : "Play_plr_footstep_wk_r", kPlayerObject);
}

// 0xF923C0, 0xFA7FB0: an anim event posts its sound on an object of its own, registered on demand ("AnimEvent"); 0x634080 releases the
// object once its sounds end, so the -200 dB SetVolume_O of Play_plr_footstep_creak mutes a creak only while the previous one plays
void GameSound::AnimEvent(std::string_view sound, uint64_t event, const glm::vec3& position) {
    if (!ready_) {
        return;
    }
    AnimObject& o = anim_objects_[event];
    if (!o.object) {
        o.object = next_anim_object_++;
        system_.RegisterObject(o.object, "AnimEvent");
    }
    system_.SetObjectTransform(o.object, position, game_.GetPlayer().BodyForward());
    SetAreaSends(o.object, position);
    system_.SetSwitch("Material", FootstepMaterial(game_.SurfaceMaterial(position)), o.object);
    o.playing.push_back(system_.PostEvent(sound, o.object));
}

audio::PlayingId GameSound::PostGimmickDialogue(uint32_t dialogue_event, std::span<const std::string_view> arguments, GimmickType source) {
    if (!ready_) {
        return 0;
    }
    // One talk at a time: every record's dialogue goes to slot 0 of the shared ShGimmick sound control, and a new one queued
    // there (0x1255B20, priority not below the playing one's; every P.T. dialogue event has priority 0) has the update
    // 0x1255F70 stop the playing sequence (slot flag 4, vfunc +0x88 with the slot's transition, 0) before 0x12563B0 opens
    // the new one. The port had kept the old talk playing under the new one, so each re-entry into the maze B bathroom
    // stacked another baby's talk.
    for (const DialogueObject& d : dialogue_objects_) {
        system_.StopPlayingId(d.playing, 0.0f);
        system_.UnregisterObject(d.object);
    }
    dialogue_objects_.clear();
    const Gimmick& g = game_.Objects().GetGimmick(source);
    const glm::vec3 position(g.world[3]);
    const glm::vec3 axis(g.world[2]);
    const glm::vec3 forward = glm::length(axis) > 1e-6f ? glm::normalize(axis) : glm::vec3(0, 0, 1);
    const audio::GameObjectId object = next_dialogue_object_++;
    system_.RegisterObject(object, "Gimmick");
    system_.SetObjectTransform(object, position, forward);
    SetAreaSends(object, position);
    const audio::PlayingId playing = system_.PostDialogueEventId(dialogue_event, arguments, object);
    if (playing) dialogue_objects_.push_back({object, playing, source});
    else system_.UnregisterObject(object);
    const Area* area = AreaAt(position);
    LogDebug("sound: gimmick dialogue at ({:.2f} {:.2f} {:.2f}), area {}", position.x, position.y, position.z, area ? area->name : "none");
    return playing;
}

audio::PlayingId GameSound::PostDialogueAt(uint32_t dialogue_event, std::span<const std::string_view> arguments, const glm::vec3& position) {
    if (!ready_) {
        return 0;
    }
    const audio::GameObjectId object = OneShotObject(position);
    const audio::PlayingId playing = system_.PostDialogueEventId(dialogue_event, arguments, object);
    one_shot_objects_.Track(object, playing);
    return playing;
}

void GameSound::UpdateGimmickDialogue() {
    std::erase_if(dialogue_objects_, [&](const DialogueObject& d) {
        if (!system_.IsPlaying(d.playing)) {
            system_.UnregisterObject(d.object);
            return true;
        }
        const Gimmick& g = game_.Objects().GetGimmick(d.source);
        const glm::vec3 position(g.world[3]);
        const glm::vec3 axis(g.world[2]);
        system_.SetObjectTransform(d.object, position, glm::length(axis) > 1e-6f ? glm::normalize(axis) : glm::vec3(0, 0, 1));
        SetAreaSends(d.object, position);
        return false;
    });
}

void GameSound::UpdateAnimObjects() {
    for (auto& [event, o] : anim_objects_) {
        std::erase_if(o.playing, [&](audio::PlayingId id) { return !system_.IsPlaying(id); });
        if (o.object && o.playing.empty()) {
            system_.UnregisterObject(o.object);
            o.object = 0;
        }
    }
}

void GameSound::OnStageLoaded(Stage& stage) {
    for (const auto& file : stage.files) {
        const fox2::DataSetFile& f = *file->file;
        for (const fox2::Entity& e : f.Entities()) {
            if (e.class_name == "SoundSource") {
                Emitter emitter;
                emitter.stage_id = stage.id;
                emitter.object = next_emitter_++;
                emitter.event = f.GetString(e, "eventName");
                const glm::mat4 world = stage.ToWorld(f.WorldTransform(e));
                emitter.position = glm::vec3(world[3]);
                emitter.forward = EntityForward(world);
                emitter.shapes = ShapeTransforms(stage, f, e, "shapes");
                emitter.range = f.GetFloat(e, "playRange", 0, 20.0f);
                if (ready_) {
                    system_.RegisterObject(emitter.object, f.EntityName(e));
                    system_.SetObjectTransform(emitter.object, emitter.position, emitter.forward);
                }
                emitters_.push_back(std::move(emitter));
            } else if (e.class_name == "SoundAreaGroup") {
                Area area;
                area.stage_id = stage.id;
                area.name = f.EntityName(e);
                area.priority = f.GetInt(e, "priority");
                if (const fox2::Property* members = f.FindProperty(e, "members")) {
                    for (size_t i = 0; i < members->Count(); ++i) {
                        if (const fox2::Entity* member = f.ElementEntity(*members, i)) {
                            auto shapes = ShapeTransforms(stage, f, *member, "shapes");
                            area.shapes.insert(area.shapes.end(), shapes.begin(), shapes.end());
                        }
                    }
                }
                if (const fox2::Entity* parameter = f.GetEntity(e, "parameter")) {
                    area.ambient_event = f.GetString(*parameter, "ambientEvent");
                    area.rtpc_name = f.GetString(*parameter, "ambientRtpcName");
                    area.rtpc_value = f.GetFloat(*parameter, "ambientRtpcValue");
                    if (const fox2::Property* sends = f.FindProperty(*parameter, "auxSends")) {
                        for (size_t i = 0; i < sends->Count(); ++i) {
                            float level = 0.0f;
                            std::memcpy(&level, sends->Element(i), sizeof(float));
                            area.sends.push_back({f.KeyString(*sends, i), level});
                        }
                    }
                }
                areas_.push_back(std::move(area));
            } else if (e.class_name == "SoundAreaEdge") {
                Edge edge;
                edge.stage_id = stage.id;
                if (const fox2::Entity* prev = f.GetEntity(e, "prevArea")) {
                    edge.prev = f.EntityName(*prev);
                }
                if (const fox2::Entity* next = f.GetEntity(e, "nextArea")) {
                    edge.next = f.EntityName(*next);
                }
                if (const fox2::Entity* parameter = f.GetEntity(e, "parameter")) {
                    edge.fade_ms = static_cast<float>(f.GetInt(*parameter, "fadeTime"));
                }
                edges_.push_back(std::move(edge));
            } else if (e.class_name == "FxLocatorData") {
                // any effect with an FxSoundCallProgramEffectNode sounds while its locator is shown, not only the fxsd files:
                // fx_sh_wtrbld01b_s2, the blood water of the bathroom, drips Play_sfx_water_drop_b
                const std::string vfx = f.GetString(e, "vfxFile");
                auto node = sound_nodes_.find(vfx);
                if (node == sound_nodes_.end()) {
                    vfx::SoundNode read;
                    bool found = false;
                    if (auto bytes = game_.GetVfs().ReadFile(vfx)) {
                        found = vfx::ReadSoundNode(*bytes, read);
                        if (!found && vfx.find("/vfx_data/sound/") != std::string::npos) {
                            read.play = VfxEvent(*bytes, "Play_");
                            read.stop = VfxEvent(*bytes, "Stop_");
                            read.stop_playing = read.stop.empty();
                            read.fade = 0.3f;
                            found = !read.play.empty();
                        }
                    }
                    node = sound_nodes_.emplace(vfx, found ? std::optional<vfx::SoundNode>(read) : std::nullopt).first;
                }
                if (!node->second) {
                    continue;
                }
                VfxSound sound;
                sound.stage_id = stage.id;
                sound.entity = &e;
                sound.play = node->second->play;
                sound.stop = node->second->stop;
                sound.stop_playing = node->second->stop_playing;
                sound.stop_fade = node->second->fade;
                sound.stop_curve = std::min(node->second->curve, 9u);
                const glm::mat4 world = stage.ToWorld(f.WorldTransform(e));
                sound.position = glm::vec3(world[3]);
                sound.forward = EntityForward(world);
                sound.object = next_emitter_++;
                if (sound.play.empty()) {
                    continue;
                }
                if (ready_) {
                    system_.RegisterObject(sound.object, f.EntityName(e));
                    system_.SetObjectTransform(sound.object, sound.position, sound.forward);
                }
                vfx_sounds_.push_back(std::move(sound));
            } else if (e.class_name == "SoundAreaGlobal") {
                const std::string rtpc = f.GetString(e, "volumeRtpc");
                if (!rtpc.empty()) {
                    volume_rtpc_ = rtpc;
                }
            }
        }
    }
}

// An active block is deactivated before it unloads (0x486FA0): its sources stop at once (0xF9FED0) and its effect instances are
// suspended, then deleted with the bodies (0xB56190 marks the instance, +0xB9 bit 0), which releases their sound nodes (0xB6E0F0)
void GameSound::OnStageUnloading(const Stage& stage) {
    for (Emitter& e : emitters_) {
        if (e.stage_id == stage.id) {
            if (ready_) {
                if (e.playing) {
                    system_.StopPlayingId(e.playing, 0.0f);
                }
                system_.UnregisterObject(e.object);
            }
        }
    }
    std::erase_if(emitters_, [&](const Emitter& e) { return e.stage_id == stage.id; });
    for (VfxSound& v : vfx_sounds_) {
        if (v.stage_id == stage.id && ready_) {
            ReleaseVfxSound(v);
            system_.UnregisterObject(v.object);
        }
    }
    std::erase_if(vfx_sounds_, [&](const VfxSound& v) { return v.stage_id == stage.id; });
    std::erase_if(areas_, [&](const Area& a) { return a.stage_id == stage.id; });
    std::erase_if(edges_, [&](const Edge& e) { return e.stage_id == stage.id; });
}

void GameSound::UpdateEmitters() {
    for (Emitter& e : emitters_) {
        // A SoundSourceBody takes its event (+0x7C) and shapes only at its block's activation (+0x40, 0xF9F4B0 -> 0xF9F4D0);
        // until then the source manager's update (0xF9E810 -> +0xD0, 0xFA0B30) finds no event and starts nothing. The
        // deactivation (+0x48, 0xF9FED0) stops a playing sound at once (transition 0, linear)
        const Stage* stage = game_.Stages().FindById(e.stage_id);
        if (!stage || !stage->active) {
            if (e.playing) {
                system_.StopPlayingId(e.playing, 0.0f);
                e.playing = 0;
            }
            if (e.on) {
                e.on = false;
                LogInfo("sound: source {} off (stage {} inactive)", e.event, e.stage_id);
            }
            continue;
        }
        glm::vec3 position = e.position;
        if (!e.shapes.empty()) {
            float best = 1e30f;
            for (const glm::mat4& shape : e.shapes) {
                const glm::vec3 p = ClosestOnBox(shape, listener_);
                const float d = glm::length(p - listener_);
                if (d < best) {
                    best = d;
                    position = p;
                }
            }
            system_.SetObjectTransform(e.object, position, e.forward);
        }
        const float distance = glm::length(position - listener_);
        if (distance <= e.range) {
            if (!e.on) {
                e.on = true;
                LogInfo("sound: source {} on at {:.1f} m (stage {})", e.event, distance, e.stage_id);
            }
            if (!e.playing || !system_.IsPlaying(e.playing)) {
                SetAreaSends(e.object, position);
                e.playing = system_.PostEvent(e.event, e.object);
            }
        } else if (distance > e.range * 1.05f && e.playing) {
            system_.StopPlayingId(e.playing, 0.5f);
            e.playing = 0;
            e.on = false;
            LogInfo("sound: source {} off at {:.1f} m (stage {})", e.event, distance, e.stage_id);
        }
    }
}

// the locator's effect instance goes, and the sound node's release 0xB6E0F0 stops a sound that still plays over the node's fade and
// curve, or posts its soundStop, or lets it play out
void GameSound::ReleaseVfxSound(VfxSound& v) {
    if (v.playing && system_.IsPlaying(v.playing)) {
        if (v.stop_playing) {
            system_.StopPlayingId(v.playing, v.stop_fade, static_cast<audio::Interp>(v.stop_curve));
        } else if (!v.stop.empty()) {
            system_.PostEvent(v.stop, v.object);
        }
    }
    v.playing = 0;
}

// PT_MUTE_EVENT=<event>: that effect sound is not posted (a test compares a capture with and without it)
audio::PlayingId GameSound::PostVfxEvent(const std::string& event, audio::GameObjectId object) {
    static const char* muted = std::getenv("PT_MUTE_EVENT");
    if (muted && event == muted) {
        return 0;
    }
    NoteArchiveEvent(event);
    return system_.PostEvent(event, object);
}

void GameSound::UpdateVfxSounds(float dt) {
    for (VfxSound& v : vfx_sounds_) {
        Stage* stage = game_.Stages().FindById(v.stage_id);
        if (!stage) {
            continue;
        }
        // the effect instance of a loaded but inactive stage is suspended until the activation (VfxScene::SyncStages), so its
        // sound node posts nothing before it
        const BodyState& body = stage->Body(v.entity);
        const bool visible = stage->active && body.visible && body.enable;
        if (!visible) {
            if (v.visible) {
                v.visible = false;
                ReleaseVfxSound(v);
            }
            continue;
        }
        if (!v.visible) {
            v.visible = true;
            v.delay = 0.0f;
            SetAreaSends(v.object, v.position);
            v.playing = PostVfxEvent(v.play, v.object);
            LogDebug("sound: vfx {} at ({:.2f} {:.2f} {:.2f})", v.play, v.position.x, v.position.y, v.position.z);
        } else if (!v.playing || !system_.IsPlaying(v.playing)) {
            if (v.play == "Play_sfx_water_drop_b") {
                v.delay -= dt;
                if (v.delay <= 0.0f) {
                    SetAreaSends(v.object, v.position);
                    v.playing = PostVfxEvent(v.play, v.object);
                    LogDebug("sound: vfx {} again at ({:.2f} {:.2f} {:.2f}), playing {}", v.play, v.position.x, v.position.y, v.position.z,
                             v.playing);
                    v.delay = 0.8f + static_cast<float>(rand()) / static_cast<float>(RAND_MAX) * 1.0f;
                }
            }
        }
    }
}

// 0xF9B650, 0xF9CD00, 0xF9CF00, 0xF9D260: one Ambient object per ambience, volumeRtpc ramps over the edge fadeTime + 1 ms
void GameSound::UpdateAreas(float dt) {
    const Area* best = AreaAt(listener_);
    const uint32_t stage = best ? best->stage_id : 0;
    const std::string name = best ? best->name : std::string();
    const std::string event = best ? best->ambient_event : std::string();
    if (!event.empty() && std::none_of(ambients_.begin(), ambients_.end(), [&](const Ambient& a) { return a.event == event; })) {
        Ambient a;
        a.event = event;
        a.object = next_ambient_object_++;
        system_.RegisterObject(a.object, "Ambient");
        ambients_.push_back(std::move(a));
    }
    if (stage != current_stage_ || name != current_area_) {
        float rate = 1000.0f;
        for (const Edge& edge : edges_) {
            const bool forward = edge.prev == current_area_ && edge.next == name;
            const bool backward = edge.prev == name && edge.next == current_area_;
            if ((edge.stage_id == stage || edge.stage_id == current_stage_) && (forward || backward)) {
                rate = 1000.0f / (edge.fade_ms + 1.0f);
                break;
            }
        }
        for (Ambient& a : ambients_) {
            a.rate = rate;
        }
        current_stage_ = stage;
        current_area_ = name;
        if (best && !best->rtpc_name.empty()) {
            system_.SetRtpc(best->rtpc_name, best->rtpc_value);
        }
        LogDebug("sound: area {} (ambient {}) at sample {}", name.empty() ? "none" : name, event, system_.RenderedFrames());
    }
    for (Ambient& a : ambients_) {
        const float step = dt * a.rate;
        a.fade = std::clamp(a.fade + (a.event == event ? step : -step), 0.0f, 1.0f);
        if (a.fade > 0.0f) {
            if (!a.playing || !system_.IsPlaying(a.playing)) {
                a.playing = system_.PostEvent(a.event, a.object);
            }
            system_.SetRtpc(volume_rtpc_, a.fade, a.object);
        }
    }
    std::erase_if(ambients_, [&](const Ambient& a) {
        if (a.fade > 0.0f || a.event == event) {
            return false;
        }
        system_.UnregisterObject(a.object);
        return true;
    });
}

void GameSound::Update(float dt) {
    if (!ready_) {
        return;
    }
    UpdateAreas(dt);
    UpdateEmitters();
    UpdateVfxSounds(dt);
    UpdateAnimObjects();
    UpdateGimmickDialogue();
    system_.Update(dt);
}

}
