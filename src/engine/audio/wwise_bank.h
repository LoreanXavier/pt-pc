#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pt::audio {

uint32_t Fnv1Hash32(std::string_view name);

enum class HircType : uint8_t {
    State = 0x01,
    Sound = 0x02,
    Action = 0x03,
    Event = 0x04,
    RanSeqCntr = 0x05,
    SwitchCntr = 0x06,
    ActorMixer = 0x07,
    Bus = 0x08,
    LayerCntr = 0x09,
    MusicSegment = 0x0A,
    MusicTrack = 0x0B,
    MusicSwitchCntr = 0x0C,
    MusicRanSeqCntr = 0x0D,
    Attenuation = 0x0E,
    DialogueEvent = 0x0F,
    FeedbackBus = 0x10,
    FeedbackNode = 0x11,
    FxShareSet = 0x12,
    FxCustom = 0x13,
    AuxBus = 0x14,
};

const char* HircTypeName(HircType type);

namespace prop {
constexpr uint8_t Volume = 0x00;
constexpr uint8_t Lfe = 0x01;
constexpr uint8_t Pitch = 0x02;
constexpr uint8_t Lpf = 0x03;
constexpr uint8_t BusVolume = 0x04;
constexpr uint8_t Priority = 0x05;
constexpr uint8_t Loop = 0x07;
constexpr uint8_t MuteRatio = 0x0A;
constexpr uint8_t PanLr = 0x0B;
constexpr uint8_t PanFr = 0x0C;
constexpr uint8_t CenterPct = 0x0D;
constexpr uint8_t DelayTime = 0x0E;
constexpr uint8_t TransitionTime = 0x0F;
constexpr uint8_t Probability = 0x10;
constexpr uint8_t UserAuxSendVolume0 = 0x12;
constexpr uint8_t GameAuxSendVolume = 0x16;
constexpr uint8_t OutputBusVolume = 0x17;
constexpr uint8_t OutputBusLpf = 0x18;
constexpr uint8_t InitialDelay = 0x19;
constexpr uint8_t MakeUpGain = 0x21;
bool IsInteger(uint8_t id);
}

namespace rtpc_param {
constexpr uint32_t Volume = 0x00;
constexpr uint32_t Lfe = 0x01;
constexpr uint32_t Pitch = 0x02;
constexpr uint32_t Lpf = 0x03;
constexpr uint32_t BusVolume = 0x04;
constexpr uint32_t InitialDelay = 0x06;
constexpr uint32_t Priority = 0x08;
constexpr uint32_t UserAuxSendVolume0 = 0x0F;
constexpr uint32_t GameAuxSendVolume = 0x13;
constexpr uint32_t PanX2d = 0x14;
constexpr uint32_t PanY2d = 0x15;
constexpr uint32_t OutputBusVolume = 0x16;
constexpr uint32_t OutputBusLpf = 0x17;
constexpr uint32_t MakeUpGain = 0x24;
}

enum class CurveScaling : uint8_t { None = 0, Unsupported = 1, Db = 2, Log = 3, DbToLin = 4 };

enum class Interp : uint32_t {
    Log3 = 0, Sine = 1, Log1 = 2, InvSCurve = 3, Linear = 4, SCurve = 5, Exp1 = 6, SineRecip = 7, Exp3 = 8, Constant = 9,
};

float InterpolateShape(Interp shape, float t);

struct CurvePoint {
    float x = 0.0f;
    float y = 0.0f;
    Interp interp = Interp::Linear;
};

struct Curve {
    CurveScaling scaling = CurveScaling::None;
    std::vector<CurvePoint> points;

    float EvaluateRaw(float x) const;
    float Evaluate(float x) const;
    bool Empty() const { return points.empty(); }
};

float ApplyScaling(CurveScaling scaling, float value);

struct PropValue {
    uint8_t id = 0;
    float value = 0.0f;
};

struct PropBundle {
    std::vector<PropValue> values;
    const float* Find(uint8_t id) const;
    float Get(uint8_t id, float fallback = 0.0f) const;
    bool Has(uint8_t id) const { return Find(id) != nullptr; }
};

struct RangedPropValue {
    uint8_t id = 0;
    float min = 0.0f;
    float max = 0.0f;
};

struct RangedPropBundle {
    std::vector<RangedPropValue> values;
    const RangedPropValue* Find(uint8_t id) const;
};

struct RtpcBinding {
    uint32_t game_parameter = 0;
    uint32_t param = 0;
    uint32_t curve_id = 0;
    Curve curve;
};

struct FxSlot {
    uint8_t index = 0;
    uint32_t fx_id = 0;
    bool share_set = false;
    bool rendered = false;
};

struct FxChunk {
    bool override_parent = false;
    uint8_t bypass_bits = 0;
    std::vector<FxSlot> slots;
};

struct PathVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int32_t duration_ms = 0;
};

struct PathPlaylistItem {
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
};

enum class PathMode : uint32_t {
    StepSequence = 0, StepRandom = 1, ContinuousSequence = 2, ContinuousRandom = 3, StepSequencePickNewPath = 4, StepRandomPickNewPath = 5,
};

struct Positioning {
    bool override_parent = false;
    bool listener_relative_routing = false;
    bool has_2d = false;
    bool has_3d = false;
    bool enable_panner = false;
    uint32_t type_3d = 0;
    uint32_t attenuation_id = 0;
    bool spatialized = false;
    bool dynamic = false;
    PathMode path_mode = PathMode::StepSequence;
    bool path_looping = false;
    int32_t path_transition_ms = 0;
    bool path_follow_orientation = false;
    std::vector<PathVertex> path_vertices;
    std::vector<PathPlaylistItem> path_items;
    std::vector<std::array<float, 2>> path_ranges;

    bool GameDefined3d() const { return has_3d && (type_3d & 3) == 1; }
    bool UserDefined3d() const { return has_3d && (type_3d & 3) == 0; }
};

struct AuxParams {
    bool override_game_aux = false;
    bool use_game_aux = false;
    bool override_user_aux = false;
    bool has_aux = false;
    std::array<uint32_t, 4> user_aux{};
};

enum class BelowThreshold : uint8_t { ContinueToPlay = 0, KillVoice = 1, SetAsVirtualVoice = 2, KillIfOneShotElseVirtual = 3 };
enum class VirtualQueue : uint8_t { FromBeginning = 0, FromElapsedTime = 1, Resume = 2 };

struct AdvancedSettings {
    VirtualQueue virtual_queue = VirtualQueue::FromBeginning;
    bool kill_newest = false;
    bool use_virtual_behavior = false;
    uint16_t max_instances = 0;
    bool global_limit = false;
    BelowThreshold below_threshold = BelowThreshold::ContinueToPlay;
    bool max_instances_override_parent = false;
    bool virtual_voice_override_parent = false;
    bool override_hdr_envelope = false;
    bool override_analysis = false;
    bool normalize_loudness = false;
    bool enable_envelope = false;
};

struct StateEntry {
    uint32_t state_id = 0;
    uint32_t instance_id = 0;
};

struct StateGroupBinding {
    uint32_t group_id = 0;
    uint8_t sync = 0;
    std::vector<StateEntry> states;
};

struct NodeBase {
    FxChunk fx;
    uint32_t override_bus_id = 0;
    uint32_t parent_id = 0;
    bool priority_override_parent = false;
    bool priority_apply_distance_factor = false;
    PropBundle props;
    RangedPropBundle ranged_props;
    Positioning positioning;
    AuxParams aux;
    AdvancedSettings advanced;
    std::vector<StateGroupBinding> states;
    std::vector<RtpcBinding> rtpcs;
    uint32_t feedback_bus_id = 0;
};

enum class StreamType : uint32_t { Data = 0, Streaming = 1, PrefetchStreaming = 2 };

namespace codec {
constexpr uint32_t Pcm = 0x00010001;
constexpr uint32_t Adpcm = 0x00020001;
constexpr uint32_t Vorbis = 0x00040001;
constexpr uint32_t External = 0x00080001;
constexpr uint32_t Silence = 0x00650002;
constexpr uint32_t ToneGenerator = 0x00660002;
constexpr uint32_t PeakLimiter = 0x006E0003;
constexpr uint32_t MatrixReverb = 0x00730003;
constexpr uint32_t RoomVerb = 0x00760003;
constexpr uint32_t StereoDelay = 0x00870003;
constexpr uint32_t MotionGenerator = 0x01950005;
}

struct Source {
    uint32_t plugin_id = 0;
    StreamType stream_type = StreamType::Data;
    uint32_t source_id = 0;
    uint32_t file_id = 0;
    uint32_t file_offset = 0;
    uint32_t in_memory_size = 0;
    bool language_specific = false;
    bool has_source = false;
    bool externally_supplied = false;
    std::vector<uint8_t> params;
};

struct HircObject {
    HircType type = HircType::State;
    uint32_t id = 0;
    virtual ~HircObject() = default;
};

struct StateObject : HircObject {
    std::vector<std::pair<uint32_t, float>> props;
    const float* Find(uint32_t param) const;
};

struct SoundObject : HircObject {
    Source source;
    NodeBase node;
};

struct ExceptionEntry {
    uint32_t id = 0;
    bool is_bus = false;
};

enum class ValueMeaning : uint8_t { Default = 0, Independent = 1, Offset = 2 };

struct ActionObject : HircObject {
    uint16_t action_type = 0;
    uint32_t target_id = 0;
    bool target_is_bus = false;
    PropBundle props;
    RangedPropBundle ranged_props;
    Interp fade_curve = Interp::Linear;
    uint32_t bank_id = 0;
    bool include_pending_resume = false;
    bool master_resume = false;
    std::vector<ExceptionEntry> exceptions;
    ValueMeaning value_meaning = ValueMeaning::Default;
    std::array<float, 3> value{};
    uint32_t state_group_id = 0;
    uint32_t state_id = 0;
    uint32_t switch_group_id = 0;
    uint32_t switch_id = 0;
    bool bypass = false;
    uint8_t bypass_mask = 0;
    bool seek_relative_to_duration = false;
    std::array<float, 3> seek_value{};
    bool snap_to_marker = false;

    uint8_t Category() const { return static_cast<uint8_t>(action_type >> 8); }
    uint8_t Scope() const { return static_cast<uint8_t>(action_type & 0xFF); }
};

struct EventObject : HircObject {
    std::vector<uint32_t> action_ids;
};

enum class TransitionMode : uint8_t { Disabled = 0, CrossFadeAmp = 1, CrossFadePower = 2, Delay = 3, SampleAccurate = 4, TriggerRate = 5 };

struct PlaylistEntry {
    uint32_t id = 0;
    int32_t weight = 0;
};

struct RanSeqObject : HircObject {
    NodeBase node;
    uint16_t loop_count = 1;
    uint16_t loop_mod_min = 0;
    uint16_t loop_mod_max = 0;
    float transition_time = 0.0f;
    float transition_mod_min = 0.0f;
    float transition_mod_max = 0.0f;
    uint16_t avoid_repeat_count = 0;
    TransitionMode transition_mode = TransitionMode::Disabled;
    bool shuffle = false;
    bool sequence = false;
    bool using_weight = false;
    bool reset_playlist_each_play = false;
    bool restart_backward = false;
    bool continuous = false;
    bool global = false;
    std::vector<uint32_t> children;
    std::vector<PlaylistEntry> playlist;
};

struct SwitchPackage {
    uint32_t switch_id = 0;
    std::vector<uint32_t> nodes;
};

struct SwitchNodeParams {
    uint32_t node_id = 0;
    bool first_only = false;
    bool continue_playback = false;
    uint32_t on_switch_mode = 0;
    int32_t fade_out_ms = 0;
    int32_t fade_in_ms = 0;
};

struct SwitchObject : HircObject {
    NodeBase node;
    uint32_t group_type = 0;
    uint32_t group_id = 0;
    uint32_t default_switch = 0;
    bool continuous_validation = false;
    std::vector<uint32_t> children;
    std::vector<SwitchPackage> switches;
    std::vector<SwitchNodeParams> switch_params;
};

struct ActorMixerObject : HircObject {
    NodeBase node;
    std::vector<uint32_t> children;
};

struct DuckInfo {
    uint32_t bus_id = 0;
    float volume = 0.0f;
    int32_t fade_out_ms = 0;
    int32_t fade_in_ms = 0;
    Interp curve = Interp::Linear;
    uint8_t target_prop = 0;
};

struct BusObject : HircObject {
    uint32_t parent_bus_id = 0;
    PropBundle props;
    bool positioning_enabled = false;
    bool positioning_enable_panner = false;
    bool kill_newest = false;
    bool use_virtual_behavior = false;
    uint16_t max_instances = 0;
    bool max_instances_override_parent = false;
    uint16_t channel_config = 0;
    bool hdr_bus = false;
    bool hdr_release_exponential = false;
    int32_t recovery_time_ms = 0;
    float max_duck_volume = 0.0f;
    std::vector<DuckInfo> ducks;
    FxChunk fx;
    std::vector<RtpcBinding> rtpcs;
    std::vector<StateGroupBinding> states;
    uint32_t feedback_bus_id = 0;
};

struct LayerAssociation {
    uint32_t child_id = 0;
    Curve curve;
};

struct Layer {
    uint32_t layer_id = 0;
    std::vector<RtpcBinding> rtpcs;
    uint32_t crossfade_rtpc_id = 0;
    std::vector<LayerAssociation> associations;
};

struct LayerObject : HircObject {
    NodeBase node;
    std::vector<uint32_t> children;
    std::vector<Layer> layers;
};

struct MusicMeter {
    double grid_period_ms = 0.0;
    double grid_offset_ms = 0.0;
    float tempo = 0.0f;
    uint8_t beats_per_bar = 0;
    uint8_t beat_value = 0;
};

struct MusicStinger {
    uint32_t trigger_id = 0;
    uint32_t segment_id = 0;
    uint32_t sync = 0;
    uint32_t cue_filter = 0;
    int32_t dont_repeat_ms = 0;
    uint32_t lookahead = 0;
};

struct MusicNodeData {
    NodeBase node;
    std::vector<uint32_t> children;
    MusicMeter meter;
    bool meter_override = false;
    std::vector<MusicStinger> stingers;
};

struct MusicMarker {
    uint32_t id = 0;
    double position_ms = 0.0;
    std::string name;
};

struct MusicSegmentObject : HircObject {
    MusicNodeData music;
    double duration_ms = 0.0;
    std::vector<MusicMarker> markers;
};

struct MusicClip {
    uint32_t track = 0;
    uint32_t source_id = 0;
    double play_at_ms = 0.0;
    double begin_trim_ms = 0.0;
    double end_trim_ms = 0.0;
    double source_duration_ms = 0.0;
};

struct ClipAutomation {
    uint32_t clip = 0;
    uint32_t kind = 0;
    std::vector<CurvePoint> points;
};

struct MusicTrackObject : HircObject {
    std::vector<Source> sources;
    std::vector<MusicClip> clips;
    uint32_t sub_track_count = 0;
    std::vector<ClipAutomation> clip_automation;
    NodeBase node;
    uint32_t track_type = 0;
    int32_t lookahead_ms = 0;
};

struct MusicFade {
    int32_t time_ms = 0;
    Interp curve = Interp::Linear;
    int32_t offset_ms = 0;
};

struct MusicTransitionRule {
    std::vector<uint32_t> sources;
    std::vector<uint32_t> destinations;
    MusicFade source_fade;
    uint32_t source_sync = 0;
    uint32_t source_cue_filter = 0;
    bool play_post_exit = false;
    MusicFade destination_fade;
    uint32_t destination_cue_filter = 0;
    uint32_t jump_to_id = 0;
    uint16_t entry_type = 0;
    bool play_pre_entry = false;
    bool match_source_cue_name = false;
    bool has_transition_object = false;
    uint32_t transition_segment_id = 0;
    MusicFade transition_fade_in;
    MusicFade transition_fade_out;
    bool transition_play_pre_entry = false;
    bool transition_play_post_exit = false;
};

enum class MusicPlaylistType : uint32_t {
    ContinuousSequence = 0, StepSequence = 1, ContinuousRandom = 2, StepRandom = 3, None = 0xFFFFFFFF,
};

struct MusicPlaylistNode {
    uint32_t segment_id = 0;
    uint32_t item_id = 0;
    MusicPlaylistType type = MusicPlaylistType::None;
    int16_t loop = 0;
    uint32_t weight = 0;
    uint16_t avoid_repeat_count = 0;
    bool using_weight = false;
    bool shuffle = false;
    std::vector<MusicPlaylistNode> children;
};

struct MusicRanSeqObject : HircObject {
    MusicNodeData music;
    std::vector<MusicTransitionRule> rules;
    uint32_t playlist_item_count = 0;
    MusicPlaylistNode playlist;
};

struct MusicSwitchObject : HircObject {
    MusicNodeData music;
    std::vector<MusicTransitionRule> rules;
};

struct ConeParams {
    float inside_degrees = 0.0f;
    float outside_degrees = 0.0f;
    float outside_volume = 0.0f;
    float low_pass = 0.0f;
};

enum AttenuationSlot : int { kAttVolumeDry = 0, kAttVolumeAuxGameDef = 1, kAttVolumeAuxUserDef = 2, kAttLowPass = 3, kAttSpread = 4 };

struct AttenuationObject : HircObject {
    bool cone_enabled = false;
    ConeParams cone;
    std::array<int8_t, 5> curve_to_use{-1, -1, -1, -1, -1};
    std::vector<Curve> curves;
    std::vector<RtpcBinding> rtpcs;

    const Curve* SlotCurve(int slot) const;
    float MaxDistance() const;
};

struct DialogueTreeNode {
    uint32_t key = 0;
    uint16_t weight = 0;
    uint16_t probability = 0;
    uint32_t audio_node_id = 0;
    std::vector<DialogueTreeNode> children;
};

struct DialogueArgument {
    uint32_t group_id = 0;
    uint8_t group_type = 0;
};

struct DialogueEventObject : HircObject {
    uint8_t probability = 100;
    std::vector<DialogueArgument> arguments;
    uint8_t mode = 0;
    DialogueTreeNode tree;
    bool has_tree = false;
};

struct FeedbackSource {
    uint16_t company_id = 0;
    uint16_t device_id = 0;
    float volume_offset = 0.0f;
    Source source;
};

struct FeedbackNodeObject : HircObject {
    std::vector<FeedbackSource> sources;
    NodeBase node;
};

struct FxObject : HircObject {
    uint32_t plugin_id = 0;
    std::vector<uint8_t> params;
    std::vector<std::pair<uint8_t, uint32_t>> media;
    std::vector<RtpcBinding> rtpcs;
};

struct StateTransition {
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t time_ms = 0;
};

struct StateGroupSettings {
    uint32_t id = 0;
    uint32_t default_transition_ms = 0;
    std::vector<StateTransition> transitions;
    uint32_t TransitionTime(uint32_t from, uint32_t to) const;
};

struct SwitchRtpcGroup {
    uint32_t group_id = 0;
    uint32_t rtpc_id = 0;
    std::vector<CurvePoint> points;
};

struct GameParameterDefault {
    uint32_t id = 0;
    float value = 0.0f;
};

struct GlobalSettings {
    float volume_threshold_db = -96.0f;
    uint16_t max_voices = 256;
    std::vector<StateGroupSettings> state_groups;
    std::vector<SwitchRtpcGroup> switch_groups;
    std::vector<GameParameterDefault> game_parameters;
};

struct EnvCurve {
    bool enabled = false;
    Curve curve;
};

struct EnvSettings {
    EnvCurve obstruction_volume;
    EnvCurve obstruction_lpf;
    EnvCurve occlusion_volume;
    EnvCurve occlusion_lpf;
};

struct MediaEntry {
    uint32_t id = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

class Bank {
public:
    static constexpr uint32_t kSupportedVersion = 88;

    bool Load(std::string name, std::shared_ptr<const std::vector<uint8_t>> storage, size_t offset, size_t size, std::string* error);

    const std::string& Name() const { return name_; }
    uint32_t Id() const { return id_; }
    uint32_t Version() const { return version_; }
    uint32_t LanguageId() const { return language_id_; }
    uint32_t ProjectId() const { return project_id_; }
    bool Feedback() const { return feedback_; }

    std::span<const uint8_t> Bytes() const;
    const std::vector<MediaEntry>& Media() const { return media_; }
    std::span<const uint8_t> MediaBytes(const MediaEntry& entry) const;
    const std::shared_ptr<const std::vector<uint8_t>>& Storage() const { return storage_; }
    size_t StorageOffset() const { return offset_; }

    const std::vector<std::unique_ptr<HircObject>>& Objects() const { return objects_; }
    const std::optional<GlobalSettings>& Settings() const { return settings_; }
    const std::optional<EnvSettings>& Environment() const { return environment_; }
    const std::unordered_map<uint32_t, std::string>& BankNames() const { return bank_names_; }

private:
    std::string name_;
    std::shared_ptr<const std::vector<uint8_t>> storage_;
    size_t offset_ = 0;
    size_t size_ = 0;
    uint32_t id_ = 0;
    uint32_t version_ = 0;
    uint32_t language_id_ = 0;
    uint32_t project_id_ = 0;
    bool feedback_ = false;
    size_t data_offset_ = 0;
    std::vector<MediaEntry> media_;
    std::vector<std::unique_ptr<HircObject>> objects_;
    std::optional<GlobalSettings> settings_;
    std::optional<EnvSettings> environment_;
    std::unordered_map<uint32_t, std::string> bank_names_;
};

const NodeBase* GetNodeBase(const HircObject* object);
const std::vector<uint32_t>* GetChildren(const HircObject* object);

}
