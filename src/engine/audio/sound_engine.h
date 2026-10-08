#pragma once

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/audio/dsp.h"
#include "engine/audio/controller_pcm_capture.h"
#include "engine/audio/motion_generator.h"
#include "engine/audio/sound_package.h"
#include "engine/audio/wem.h"
#include "engine/audio/wwise_bank.h"

namespace pt::audio {

using GameObjectId = uint64_t;
using PlayingId = uint32_t;

constexpr uint32_t kBlockFrames = 256;
constexpr uint32_t kExternalSourceEvent = 0xAE3819F7u;
constexpr uint32_t kMasterAudioBus = 3803692087u;

struct Ramp {
    float from = 0.0f;
    float to = 0.0f;
    float end = 0.0f;
    uint64_t start = 0;
    uint64_t length = 0;
    uint32_t frames = 0;
    Interp curve = Interp::Linear;
    bool decibels = false;

    float Value(uint64_t t) const;
    bool Done(uint64_t t) const { return length == 0 || t >= start + length; }
    void Transition(float target, uint64_t now, float ms, Interp shape, bool db, bool mirror);
    void Jump(float value) {
        from = to = end = value;
        length = 0;
        frames = 0;
    }
    static uint64_t Samples(float ms);

private:
    float Step(int64_t frame) const;
};

struct EngineStats {
    uint32_t voices = 0;
    uint32_t virtual_voices = 0;
    uint32_t playing_ids = 0;
    uint32_t sequencers = 0;
    uint32_t music = 0;
    float peak_left = 0.0f;
    float peak_right = 0.0f;
};

struct EngineNotification {
    enum class Kind { Marker, End };
    Kind kind = Kind::End;
    PlayingId playing_id = 0;
    std::string label;
};

class SoundEngine {
public:
    explicit SoundEngine(std::unique_ptr<SoundBankSet> banks);
    ~SoundEngine();

    PlayingId PostEvent(uint32_t event_id, GameObjectId object, std::shared_ptr<const Media> external = nullptr);
    PlayingId PostEventMedia(uint32_t event_id, GameObjectId object, std::vector<uint32_t> media_ids);
    PlayingId PostDialogue(uint32_t dialogue_id, std::vector<uint32_t> arguments, GameObjectId object);
    void StopPlayingId(PlayingId id, float fade_seconds, Interp curve = Interp::Linear);
    void SeekPlayingId(PlayingId id, float seconds);
    void StopAll(float fade_seconds);
    bool IsPlaying(PlayingId id) const;
    bool IsEventPlaying(uint32_t event_id) const;
    void RegisterObject(GameObjectId object, std::string name);
    void UnregisterObject(GameObjectId object);
    void SetObjectTransform(GameObjectId object, const glm::vec3& position, const glm::vec3& forward);
    void SetObjectAuxSends(GameObjectId object, std::vector<std::pair<uint32_t, float>> sends);
    void SetObjectObstruction(GameObjectId object, float obstruction, float occlusion);
    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    void SetState(uint32_t group, uint32_t state);
    void SetSwitch(uint32_t group, uint32_t value, GameObjectId object);
    void SetRtpc(uint32_t parameter, float value, GameObjectId object, bool reset);
    void SetRandomSeed(uint32_t seed);
    void SetMasterGain(float gain);

    bool HasEvent(uint32_t event_id) const { return events_.contains(event_id); }
    bool HasDialogueEvent(uint32_t id) const;
    const SoundBankSet& Banks() const { return *banks_; }
    std::vector<EngineNotification> TakeNotifications();
    EngineStats Stats() const;
    uint64_t RenderedFrames() const { return rendered_frames_.load(); }
    MotionLevels Motion() const;
    void SetControllerCaptureEvent(uint32_t event_id) { controller_pcm_capture_.SetEvent(event_id); }
    void SetControllerCaptureEvents(std::span<const uint32_t> event_ids) { controller_pcm_capture_.SetEvents(event_ids); }
    bool TryReadControllerPcm(ControllerPcmBlock& block) { return controller_pcm_capture_.TryPop(block); }
    uint32_t ControllerPcmDroppedBlocks() const { return controller_pcm_capture_.DroppedBlocks(); }
    void SetFrozen(bool frozen) { frozen_.store(frozen); }

    void Render(float* out, uint32_t frames);
    void Render(float* out, uint32_t frames, uint32_t channels);

    struct LayerLink {
        const Layer* layer = nullptr;
        const Curve* crossfade = nullptr;
    };

    struct NodeInfo {
        uint32_t id = 0;
        const HircObject* object = nullptr;
        const NodeBase* base = nullptr;
        NodeInfo* parent = nullptr;
        uint32_t bus_id = 0;
        const Positioning* positioning = nullptr;
        const NodeBase* positioning_owner = nullptr;
        uint32_t positioning_owner_id = 0;
        const AttenuationObject* attenuation = nullptr;
        bool use_game_aux = false;
        std::array<uint32_t, 4> user_aux{};
        const NodeInfo* limit_owner = nullptr;
        BelowThreshold below_threshold = BelowThreshold::ContinueToPlay;
        VirtualQueue virtual_queue = VirtualQueue::FromElapsedTime;
        float priority = 50.0f;
        float center = 0.0f;
        std::vector<size_t> state_slots;
        int depth = 0;
    };

private:
    enum class CommandType : uint8_t {
        PostEvent, PostDialogue, StopPlaying, StopAll, RegisterObject, UnregisterObject, SetTransform, SetAuxSends, SetObstruction,
        SetListener, SetState, SetSwitch, SetRtpc, ResetRtpc, SetSeed, SetMasterGain, SeekPlaying,
    };

    struct Command {
        CommandType type = CommandType::PostEvent;
        PlayingId playing_id = 0;
        GameObjectId object = 0;
        uint32_t a = 0;
        uint32_t b = 0;
        float value = 0.0f;
        float value2 = 0.0f;
        glm::vec3 v0{0.0f};
        glm::vec3 v1{0.0f};
        glm::vec3 v2{0.0f};
        std::string text;
        std::vector<uint32_t> ids;
        std::vector<std::pair<uint32_t, float>> sends;
        std::shared_ptr<const Media> media;
    };

    struct ObjectState {
        std::string name;
        bool has_transform = false;
        glm::vec3 position{0.0f};
        glm::vec3 forward{0.0f, 0.0f, 1.0f};
        std::unordered_map<uint32_t, uint32_t> switches;
        std::unordered_map<uint32_t, Ramp> rtpcs;
        bool has_aux_sends = false;
        std::vector<std::pair<uint32_t, float>> aux_sends;
        float obstruction = 0.0f;
        float occlusion = 0.0f;
    };

    struct PlayContext {
        PlayingId playing_id = 0;
        GameObjectId object = 0;
        uint64_t start_time = 0;
        float fade_in_ms = 0.0f;
        Interp fade_curve = Interp::Linear;
        uint32_t sequencer = 0;
        uint32_t item_token = 0;
        std::vector<LayerLink> layers;
        std::shared_ptr<const Media> external;
        float random_volume = 0.0f;
        float random_pitch = 0.0f;
        float random_lpf = 0.0f;
        int depth = 0;
    };

    struct PlaylistState {
        size_t sequence_index = 0;
        bool backward = false;
        std::vector<size_t> recent;
        std::vector<size_t> shuffle_pool;
    };

    struct Sequencer {
        uint32_t serial = 0;
        const RanSeqObject* container = nullptr;
        const NodeInfo* node = nullptr;
        PlayingId playing_id = 0;
        GameObjectId object = 0;
        uint32_t parent = 0;
        uint32_t parent_token = 0;
        std::vector<LayerLink> layers;
        std::shared_ptr<const Media> external;
        float random_volume = 0.0f;
        float random_pitch = 0.0f;
        float random_lpf = 0.0f;
        int loops_left = 1;
        size_t items_left_in_pass = 0;
        std::unordered_map<uint32_t, int> items;
        std::unordered_map<uint32_t, uint64_t> item_starts;
        uint32_t next_token = 1;
        bool breaking = false;
        bool finished = false;
        bool released = false;
        bool item_scheduled = false;
        uint64_t scheduled_time = 0;
        PlaylistState playlist;
        float first_fade_ms = 0.0f;
        Interp first_fade_curve = Interp::Linear;
        bool first_item = true;
        int empty_items = 0;
        std::vector<size_t> forced;
        size_t forced_next = 0;
        bool paused = false;
        uint64_t paused_at = 0;
    };

    struct PlaylistFrame {
        const MusicPlaylistNode* node = nullptr;
        int loops_done = 0;
        size_t child_pos = 0;
        std::vector<size_t> order;
    };

    struct MusicPlayer {
        uint32_t serial = 0;
        PlayingId playing_id = 0;
        GameObjectId object = 0;
        const NodeInfo* node = nullptr;
        const MusicRanSeqObject* ranseq = nullptr;
        std::vector<PlaylistFrame> stack;
        std::vector<LayerLink> layers;
        int live_voices = 0;
        bool stopping = false;
        bool released = false;
        bool has_next = false;
        uint64_t next_time = 0;
        uint32_t next_segment = 0;
        uint32_t current_segment = 0;
        float fade_in_ms = 0.0f;
        Interp fade_curve = Interp::Linear;
        bool first = true;
        std::unordered_map<uint32_t, uint32_t> sequence_tracks;
        bool paused = false;
        uint64_t paused_at = 0;
    };

    enum class Generator : uint8_t { None, Silence, Tone, Motion };

    struct Voice {
        uint32_t serial = 0;
        PlayingId playing_id = 0;
        GameObjectId object = 0;
        const NodeInfo* node = nullptr;
        uint32_t bus_id = 0;
        std::shared_ptr<const Media> media;
        std::unique_ptr<MediaReader> reader;
        uint32_t channels = 1;
        std::array<float, 8> mix_l{};
        std::array<float, 8> mix_r{};
        Generator generator = Generator::None;
        const FxObject* generator_params = nullptr;
        std::shared_ptr<const MotionGeneratorParams> motion;
        uint64_t generator_frames = 0;
        uint64_t generator_pos = 0;
        double tone_phase = 0.0;
        float pink[7] = {};
        uint32_t noise_state = 1;
        uint64_t start_time = 0;
        bool started = false;
        uint64_t source_pos = 0;
        uint64_t source_total = 0;
        uint64_t range_begin = 0;
        uint64_t range_end = 0;
        bool loop_points = false;
        uint64_t loop_start = 0;
        uint64_t loop_end = 0;
        int repeats_left = 0;
        std::vector<float> scratch;
        std::vector<float> rs_l;
        std::vector<float> rs_r;
        size_t rs_count = 0;
        double rs_pos = 1.0;
        uint32_t rs_step = 0x10000;
        uint32_t rs_target = 0x10000;
        uint32_t rs_ramp = 1024;
        float rs_cents = 0.0f;
        bool rs_pitch_set = false;
        bool source_done = false;
        size_t rs_valid_end = 0;
        Ramp play_fade;
        Ramp stop_fade;
        Ramp pause_fade;
        bool stopping = false;
        bool paused = false;
        bool pausing = false;
        uint64_t paused_at = 0;
        bool finished = false;
        bool notified = false;
        uint64_t finish_time = 0;
        bool is_virtual = false;
        uint64_t virtual_since = 0;
        float random_volume = 0.0f;
        float random_pitch = 0.0f;
        float random_lpf = 0.0f;
        uint32_t sequencer = 0;
        uint32_t item_token = 0;
        uint32_t music = 0;
        std::vector<LayerLink> layers;
        std::vector<WemMarker> markers;
        uint32_t path_index = 0;
        glm::vec2 path_offset{0.0f};
        float lpf_value = 0.0f;
        float lpf_from = 0.0f;
        float lpf_target = 0.0f;
        int lpf_step = 8;
        bool lpf_init = false;
        float lpf_coef[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        float lpf_hist[2][4] = {};
        float gain_prev[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        bool gain_init = false;
        struct Send {
            uint32_t bus = 0;
            float gain = 0.0f;
            float prev = 0.0f;
            bool front = false;
        };
        static constexpr uint32_t kMaxSends = 8;
        std::array<Send, kMaxSends> sends{};
        uint32_t send_count = 0;
        float matrix[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        float front_matrix[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        float dry_gain = 0.0f;
        float send_scale = 1.0f;
        float sum_scale = 1.0f;
        double pitch_ratio = 1.0;
        uint8_t sc_mode = 0;
        uint32_t sc_split = 0;
        std::array<std::array<float, 8>, 8> sc_matrix{};
        std::array<std::vector<float>, 8> sc_rs;
        float sc_lpf_hist[8][4] = {};
        float sc_dry_prev = 0.0f;
    };

    struct BusRuntime {
        uint32_t id = 0;
        const BusObject* object = nullptr;
        BusRuntime* parent = nullptr;
        int depth = 0;
        std::vector<float> left;
        std::vector<float> right;
        bool has_input = false;
        uint64_t active_until = 0;
        std::vector<std::unique_ptr<Effect>> effects;
        float tail_seconds = 0.0f;
        float gain = 1.0f;
        float gain_prev = 1.0f;
        float voice_volume_db = 0.0f;
        float voice_pitch = 0.0f;
        float voice_lpf = 0.0f;
        std::vector<size_t> state_slots;
        int active_voices = 0;
        float duck_db = 0.0f;
        uint64_t duck_release_at = 0;
        bool effects_running = false;
        bool mono_input = false;
        bool front_input = false;
        bool sum_input = false;
        float chain = 1.0f;
        float chain_prev = 1.0f;
        bool sc_direct = true;
    };

    struct StateSlot {
        uint32_t owner = 0;
        bool is_bus = false;
        const StateGroupBinding* binding = nullptr;
        std::array<Ramp, 6> values;
    };

    struct ActionProps {
        GameObjectId object = 0;
        bool all_objects = false;
        Ramp volume;
        Ramp pitch;
        Ramp lpf;
        Ramp bus_volume;
    };

    struct PendingAction {
        uint64_t time = 0;
        uint64_t order = 0;
        PlayingId playing_id = 0;
        GameObjectId object = 0;
        const ActionObject* action = nullptr;
        std::shared_ptr<const Media> external;
        bool cancelled = false;
    };

    struct Timer {
        uint64_t time = 0;
        uint64_t order = 0;
        bool music = false;
        uint32_t serial = 0;
    };

    struct PlayingRecord {
        uint32_t event_id = 0;
        GameObjectId object = 0;
        int refs = 0;
    };

    void Push(Command command);
    void BuildNodes();
    void BuildBuses();
    void BuildStateSlots();
    NodeInfo* Info(uint32_t id);
    BusRuntime* Bus(uint32_t id);
    ObjectState& Object(GameObjectId id);
    float Random01();
    float RandomRange(float lo, float hi);

    void ApplyCommand(Command& command);
    void StartEvent(PlayingId id, uint32_t event_id, GameObjectId object, const std::shared_ptr<const Media>& external);
    void AddRef(PlayingId id);
    void Release(PlayingId id);
    void ExecuteAction(const ActionObject& action, PlayingId playing_id, GameObjectId object, uint64_t time,
                       const std::shared_ptr<const Media>& external);
    void PlayNode(uint32_t node_id, PlayContext ctx);
    void PlaySound(NodeInfo* info, const SoundObject& sound, const PlayContext& ctx);
    void PlayRanSeq(NodeInfo* info, const RanSeqObject& container, const PlayContext& ctx);
    void PlaySwitch(NodeInfo* info, const SwitchObject& container, const PlayContext& ctx);
    void PlayLayer(NodeInfo* info, const LayerObject& container, const PlayContext& ctx);
    void PlayMusic(NodeInfo* info, const PlayContext& ctx);
    size_t PickPlaylistIndex(const RanSeqObject& container, PlaylistState& state, bool continuous);
    PlaylistState& StepPlaylist(const RanSeqObject& container, GameObjectId object);

    Voice* CreateVoice(NodeInfo* info, const PlayContext& ctx, std::shared_ptr<const Media> clip = nullptr, uint64_t clip_begin = 0,
                       uint64_t clip_end = 0);
    bool AdmitVoice(NodeInfo* info, GameObjectId object, float priority);
    void SetupSourceRegion(Voice& voice, uint64_t begin, uint64_t end, int loop_count);
    void RollNodeRandom(const NodeBase& base, PlayContext& ctx);
    void FinishVoice(Voice& voice, uint64_t time);
    uint64_t RemainingOutputFrames(const Voice& voice) const;
    void NotifyVoiceFinished(Voice* voice);

    void SequencerStartItem(Sequencer& seq, uint64_t time);
    void SequencerItemDone(uint32_t serial, uint32_t token, uint64_t time);
    void SequencerCheckDone(Sequencer& seq, uint64_t time = 0);
    Sequencer* FindSequencer(uint32_t serial);
    void ScheduleTimer(uint64_t time, bool music, uint32_t serial);

    uint32_t MusicNextSegment(MusicPlayer& player);
    void MusicStartSegment(MusicPlayer& player, uint32_t segment_id, uint64_t time);
    MusicPlayer* FindMusic(uint32_t serial);
    void MusicCheckDone(MusicPlayer& player);

    bool ChainContains(const NodeInfo* node, uint32_t id) const;
    bool BusChainContains(uint32_t bus, uint32_t id) const;
    bool ActionMatches(const ActionObject& action, GameObjectId object, const NodeInfo* node, uint32_t bus, GameObjectId event_object) const;
    void StopMatching(const ActionObject& action, GameObjectId object, uint64_t time, float fade_ms, Interp curve);
    void StopVoice(Voice& voice, uint64_t time, float fade_ms, Interp curve);
    void PauseMatching(const ActionObject& action, GameObjectId object, uint64_t time, float fade_ms, Interp curve, bool pause);
    void BreakMatching(const ActionObject& action, GameObjectId object);
    void SeekMatching(const ActionObject& action, GameObjectId object);
    void SeekPlayingInternal(PlayingId id, float seconds);
    void SetAkProp(const ActionObject& action, GameObjectId object, uint64_t time);
    void SetGameParameter(const ActionObject& action, GameObjectId object, uint64_t time);
    void SetStateInternal(uint32_t group, uint32_t state, uint64_t time);
    void StopPlayingInternal(PlayingId id, float fade_ms, Interp curve);
    void StopAllInternal(float fade_ms);
    void StopObject(GameObjectId object);

    float RtpcValue(uint32_t parameter, GameObjectId object) const;
    float SumRtpcs(const std::vector<RtpcBinding>& bindings, uint32_t param, GameObjectId object) const;
    void ActionPropOffsets(uint32_t id, GameObjectId object, float& volume, float& pitch, float& lpf, float& bus_volume) const;
    float StateOffset(const std::vector<size_t>& slots, int index) const;
    glm::vec3 EmitterPosition(const Voice& voice, const ObjectState& object) const;
    void UpdateVoiceParams(Voice& voice);
    void UpdateBuses();
    uint32_t PullSource(Voice& voice, float* left, float* right, uint32_t frames, size_t index);
    uint32_t RenderVoice(Voice& voice, uint32_t offset, uint32_t frames);
    uint32_t RenderMotion(Voice& voice, uint32_t offset, uint32_t frames);
    std::shared_ptr<const MotionGeneratorParams> MotionParams(uint32_t source_id);
    void ApplyVoiceLpf(Voice& voice, float* left, float* right, uint32_t frames);
    void RenderBlock(float* out, uint32_t frames, uint32_t channels);
    void ProcessDue(uint64_t block_end);

    std::unique_ptr<SoundBankSet> banks_;
    std::unordered_map<uint32_t, const EventObject*> events_;
    std::unordered_map<uint32_t, const DialogueEventObject*> dialogue_events_;
    std::unordered_map<uint32_t, NodeInfo> nodes_;
    std::unordered_map<uint32_t, std::unique_ptr<BusRuntime>> buses_;
    std::vector<BusRuntime*> bus_order_;
    BusRuntime* master_ = nullptr;
    std::vector<StateSlot> state_slots_;
    std::unordered_map<uint32_t, std::vector<size_t>> slots_by_group_;
    std::unordered_map<uint32_t, const StateGroupSettings*> state_groups_;
    std::unordered_map<uint32_t, float> rtpc_defaults_;
    const EnvSettings* environment_ = nullptr;
    float volume_threshold_db_ = -80.0f;
    uint32_t max_voices_ = 256;

    mutable std::mutex command_mutex_;
    std::vector<Command> commands_;
    std::vector<Command> command_work_;
    std::atomic<PlayingId> next_playing_id_{1};
    mutable std::mutex status_mutex_;
    std::unordered_map<PlayingId, uint32_t> live_ids_;
    std::unordered_map<uint32_t, uint32_t> live_events_;
    std::mutex notify_mutex_;
    std::vector<EngineNotification> notifications_;
    std::vector<EngineNotification> pending_notifications_;
    std::vector<PlayingId> ended_ids_;
    mutable std::mutex stats_mutex_;
    EngineStats stats_;
    std::atomic<uint64_t> rendered_frames_{0};

    uint64_t now_ = 0;
    uint64_t block_end_ = 0;
    uint64_t order_counter_ = 0;
    uint32_t serial_counter_ = 1;
    std::mt19937 rng_;
    float master_gain_ = 1.0f;
    glm::vec3 listener_position_{0.0f};
    glm::vec3 listener_forward_{0.0f, 0.0f, 1.0f};
    glm::vec3 listener_up_{0.0f, 1.0f, 0.0f};
    glm::vec3 listener_right_{-1.0f, 0.0f, 0.0f};
    std::unordered_map<GameObjectId, ObjectState> objects_;
    std::unordered_set<GameObjectId> removed_objects_;
    std::unordered_map<uint32_t, uint32_t> states_;
    std::unordered_map<uint32_t, Ramp> global_rtpc_;
    std::unordered_map<uint32_t, std::vector<ActionProps>> action_props_;
    std::unordered_map<uint64_t, PlaylistState> step_playlists_;
    std::unordered_map<uint32_t, uint32_t> path_sequence_;
    std::unordered_map<PlayingId, PlayingRecord> playing_;
    std::vector<PendingAction> pending_;
    std::vector<Timer> timers_;
    std::vector<std::unique_ptr<Voice>> voices_;
    std::vector<std::unique_ptr<Sequencer>> sequencers_;
    std::unordered_map<PlayingId, std::vector<uint32_t>> forced_media_;
    std::vector<size_t> ForcedIndices(const RanSeqObject& container, PlayingId id) const;
    std::vector<std::unique_ptr<MusicPlayer>> music_;
    std::unordered_set<uint32_t> warned_;
    std::vector<float> mix_left_;
    std::vector<float> mix_right_;
    std::vector<float> voice_left_;
    std::vector<float> voice_right_;
    ControllerPcmCapture controller_pcm_capture_;
    std::array<std::vector<float>, 8> sc_;
    std::array<std::vector<float>, 8> sc_voice_;
    std::vector<float> sc_peak_;
    std::vector<float> sc_weight_;
    std::vector<PendingAction> due_actions_;
    std::vector<Timer> due_timers_;
    uint32_t created_this_block_ = 0;
    bool global_pause_ = false;
    std::unordered_map<uint32_t, std::shared_ptr<const MotionGeneratorParams>> motion_params_;
    float motion_mix_[2] = {0.0f, 0.0f};
    std::atomic<uint32_t> motion_levels_{0};
    std::atomic<bool> frozen_{false};
    bool trace_ = false;
};

}
