#include "engine/audio/wwise_bank.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>

namespace pt::audio {
namespace {

class BankError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t begin, size_t end) : data_(data), pos_(begin), end_(end) {}

    template <typename T>
    T Take() {
        if (pos_ + sizeof(T) > end_) {
            throw BankError(std::format("read past end at 0x{:X}", pos_));
        }
        T value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    uint8_t U8() { return Take<uint8_t>(); }
    int8_t S8() { return Take<int8_t>(); }
    uint16_t U16() { return Take<uint16_t>(); }
    int16_t S16() { return Take<int16_t>(); }
    uint32_t U32() { return Take<uint32_t>(); }
    int32_t S32() { return Take<int32_t>(); }
    float F32() { return Take<float>(); }
    double F64() { return Take<double>(); }

    std::vector<uint8_t> Raw(size_t size) {
        if (pos_ + size > end_) {
            throw BankError(std::format("read past end at 0x{:X}", pos_));
        }
        std::vector<uint8_t> out(data_ + pos_, data_ + pos_ + size);
        pos_ += size;
        return out;
    }

    size_t Pos() const { return pos_; }
    size_t Remaining() const { return end_ - pos_; }

private:
    const uint8_t* data_;
    size_t pos_;
    size_t end_;
};

float PropUnion(uint8_t id, uint32_t raw) {
    if (prop::IsInteger(id)) {
        int32_t value;
        std::memcpy(&value, &raw, 4);
        return static_cast<float>(value);
    }
    float value;
    std::memcpy(&value, &raw, 4);
    return value;
}

PropBundle ReadPropBundle(Reader& r) {
    PropBundle bundle;
    const uint8_t count = r.U8();
    std::vector<uint8_t> ids(count);
    for (auto& id : ids) {
        id = r.U8();
    }
    for (uint8_t id : ids) {
        bundle.values.push_back({id, PropUnion(id, r.U32())});
    }
    return bundle;
}

RangedPropBundle ReadRangedPropBundle(Reader& r) {
    RangedPropBundle bundle;
    const uint8_t count = r.U8();
    std::vector<uint8_t> ids(count);
    for (auto& id : ids) {
        id = r.U8();
    }
    for (uint8_t id : ids) {
        const float lo = PropUnion(id, r.U32());
        const float hi = PropUnion(id, r.U32());
        bundle.values.push_back({id, lo, hi});
    }
    return bundle;
}

std::vector<CurvePoint> ReadGraphPoints(Reader& r, uint32_t count) {
    std::vector<CurvePoint> points(count);
    for (auto& p : points) {
        p.x = r.F32();
        p.y = r.F32();
        p.interp = static_cast<Interp>(r.U32());
    }
    return points;
}

std::vector<RtpcBinding> ReadRtpcList(Reader& r) {
    std::vector<RtpcBinding> list(r.U16());
    for (auto& rtpc : list) {
        rtpc.game_parameter = r.U32();
        rtpc.param = r.U32();
        rtpc.curve_id = r.U32();
        rtpc.curve.scaling = static_cast<CurveScaling>(r.U8());
        const uint16_t size = r.U16();
        rtpc.curve.points = ReadGraphPoints(r, size);
    }
    return list;
}

Source ReadSource(Reader& r) {
    Source source;
    source.plugin_id = r.U32();
    source.stream_type = static_cast<StreamType>(r.U32());
    source.source_id = r.U32();
    source.file_id = r.U32();
    if (source.stream_type != StreamType::Streaming) {
        source.file_offset = r.U32();
        source.in_memory_size = r.U32();
    }
    const uint8_t bits = r.U8();
    source.language_specific = (bits & 1) != 0;
    source.has_source = (bits & 2) != 0;
    source.externally_supplied = (bits & 4) != 0;
    const uint32_t plugin_type = source.plugin_id & 0xF;
    if (plugin_type == 2 || plugin_type == 5) {
        const uint32_t size = r.U32();
        if (size) {
            source.params = r.Raw(size);
        }
    }
    return source;
}

FxChunk ReadFxChunk(Reader& r) {
    FxChunk fx;
    const uint8_t count = r.U8();
    if (count) {
        fx.bypass_bits = r.U8();
        fx.slots.resize(count);
        for (auto& slot : fx.slots) {
            slot.index = r.U8();
            slot.fx_id = r.U32();
            slot.share_set = r.U8() != 0;
            slot.rendered = r.U8() != 0;
        }
    }
    return fx;
}

Positioning ReadPositioning(Reader& r) {
    Positioning p;
    const uint8_t bits = r.U8();
    p.override_parent = (bits & 1) != 0;
    p.listener_relative_routing = (bits & 2) != 0;
    if (!(bits & 1)) {
        return p;
    }
    p.has_2d = r.U8() != 0;
    p.has_3d = r.U8() != 0;
    if (p.has_2d) {
        p.enable_panner = r.U8() != 0;
    }
    if (p.has_3d) {
        p.type_3d = r.U32();
        p.attenuation_id = r.U32();
        p.spatialized = r.U8() != 0;
        if ((p.type_3d & 3) == 1) {
            p.dynamic = r.U8() != 0;
        } else {
            p.path_mode = static_cast<PathMode>(r.U32());
            p.path_looping = r.U8() != 0;
            p.path_transition_ms = r.S32();
            p.path_follow_orientation = r.U8() != 0;
            p.path_vertices.resize(r.U32());
            for (auto& v : p.path_vertices) {
                v.x = r.F32();
                v.y = r.F32();
                v.z = r.F32();
                v.duration_ms = r.S32();
            }
            const uint32_t item_count = r.U32();
            p.path_items.resize(item_count);
            for (auto& item : p.path_items) {
                item.vertex_offset = r.U32();
                item.vertex_count = r.U32();
            }
            p.path_ranges.resize(item_count);
            for (auto& range : p.path_ranges) {
                range[0] = r.F32();
                range[1] = r.F32();
            }
        }
    }
    return p;
}

AuxParams ReadAux(Reader& r) {
    AuxParams aux;
    aux.override_game_aux = r.U8() != 0;
    aux.use_game_aux = r.U8() != 0;
    aux.override_user_aux = r.U8() != 0;
    aux.has_aux = r.U8() != 0;
    if (aux.has_aux) {
        for (auto& id : aux.user_aux) {
            id = r.U32();
        }
    }
    return aux;
}

AdvancedSettings ReadAdvanced(Reader& r) {
    AdvancedSettings a;
    a.virtual_queue = static_cast<VirtualQueue>(r.U8());
    a.kill_newest = r.U8() != 0;
    a.use_virtual_behavior = r.U8() != 0;
    a.max_instances = r.U16();
    a.global_limit = r.U8() != 0;
    a.below_threshold = static_cast<BelowThreshold>(r.U8());
    a.max_instances_override_parent = r.U8() != 0;
    a.virtual_voice_override_parent = r.U8() != 0;
    a.override_hdr_envelope = r.U8() != 0;
    a.override_analysis = r.U8() != 0;
    a.normalize_loudness = r.U8() != 0;
    a.enable_envelope = r.U8() != 0;
    return a;
}

std::vector<StateGroupBinding> ReadStateChunk(Reader& r) {
    std::vector<StateGroupBinding> groups(r.U32());
    for (auto& group : groups) {
        group.group_id = r.U32();
        group.sync = r.U8();
        group.states.resize(r.U16());
        for (auto& state : group.states) {
            state.state_id = r.U32();
            state.instance_id = r.U32();
        }
    }
    return groups;
}

NodeBase ReadNodeBase(Reader& r, bool feedback) {
    NodeBase node;
    const uint8_t override_fx = r.U8();
    node.fx = ReadFxChunk(r);
    node.fx.override_parent = override_fx != 0;
    node.override_bus_id = r.U32();
    node.parent_id = r.U32();
    node.priority_override_parent = r.U8() != 0;
    node.priority_apply_distance_factor = r.U8() != 0;
    node.props = ReadPropBundle(r);
    node.ranged_props = ReadRangedPropBundle(r);
    node.positioning = ReadPositioning(r);
    node.aux = ReadAux(r);
    node.advanced = ReadAdvanced(r);
    node.states = ReadStateChunk(r);
    node.rtpcs = ReadRtpcList(r);
    if (feedback) {
        node.feedback_bus_id = r.U32();
    }
    return node;
}

std::vector<uint32_t> ReadChildren(Reader& r) {
    std::vector<uint32_t> children(r.U32());
    for (auto& c : children) {
        c = r.U32();
    }
    return children;
}

std::vector<ExceptionEntry> ReadExceptions(Reader& r) {
    std::vector<ExceptionEntry> list(r.U32());
    for (auto& e : list) {
        e.id = r.U32();
        e.is_bus = r.U8() != 0;
    }
    return list;
}

std::unique_ptr<HircObject> ParseState(Reader& r) {
    auto obj = std::make_unique<StateObject>();
    const uint8_t count = r.U8();
    std::vector<uint8_t> ids(count);
    for (auto& id : ids) {
        id = r.U8();
    }
    for (uint8_t id : ids) {
        obj->props.push_back({id, r.F32()});
    }
    return obj;
}

std::unique_ptr<HircObject> ParseSound(Reader& r, bool feedback) {
    auto obj = std::make_unique<SoundObject>();
    obj->source = ReadSource(r);
    obj->node = ReadNodeBase(r, feedback);
    return obj;
}

enum class ActionKind { Stop, Pause, Resume, Play, SetValue, SetAkProp, NoParams, SetState, SetGameParameter, SetSwitch, BypassFx, Seek, Unknown };

ActionKind KindOfCategory(uint8_t category) {
    switch (category) {
        case 0x01: return ActionKind::Stop;
        case 0x02: return ActionKind::Pause;
        case 0x03: return ActionKind::Resume;
        case 0x04:
        case 0x05: return ActionKind::Play;
        case 0x06:
        case 0x07: return ActionKind::SetValue;
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x20: case 0x30:
            return ActionKind::SetAkProp;
        case 0x10: case 0x11: case 0x15: case 0x16: case 0x17: case 0x1C: case 0x1D:
            return ActionKind::NoParams;
        case 0x12: return ActionKind::SetState;
        case 0x13:
        case 0x14: return ActionKind::SetGameParameter;
        case 0x19: return ActionKind::SetSwitch;
        case 0x1A:
        case 0x1B: return ActionKind::BypassFx;
        case 0x1E: return ActionKind::Seek;
        default: return ActionKind::Unknown;
    }
}

std::unique_ptr<HircObject> ParseAction(Reader& r) {
    auto obj = std::make_unique<ActionObject>();
    obj->action_type = r.U16();
    obj->target_id = r.U32();
    obj->target_is_bus = (r.U8() & 1) != 0;
    obj->props = ReadPropBundle(r);
    obj->ranged_props = ReadRangedPropBundle(r);
    const ActionKind kind = KindOfCategory(obj->Category());
    switch (kind) {
        case ActionKind::Stop:
        case ActionKind::Pause:
        case ActionKind::Resume:
            obj->fade_curve = static_cast<Interp>(r.U8() & 0x1F);
            if (kind == ActionKind::Pause) {
                obj->include_pending_resume = (r.U8() & 1) != 0;
            } else if (kind == ActionKind::Resume) {
                obj->master_resume = (r.U8() & 1) != 0;
            }
            obj->exceptions = ReadExceptions(r);
            break;
        case ActionKind::Play:
            obj->fade_curve = static_cast<Interp>(r.U8() & 0x1F);
            obj->bank_id = r.U32();
            break;
        case ActionKind::SetValue:
            obj->fade_curve = static_cast<Interp>(r.U8() & 0x1F);
            obj->exceptions = ReadExceptions(r);
            break;
        case ActionKind::SetAkProp:
        case ActionKind::SetGameParameter:
            obj->fade_curve = static_cast<Interp>(r.U8() & 0x1F);
            obj->value_meaning = static_cast<ValueMeaning>(r.U8());
            obj->value = {r.F32(), r.F32(), r.F32()};
            obj->exceptions = ReadExceptions(r);
            break;
        case ActionKind::SetState:
            obj->state_group_id = r.U32();
            obj->state_id = r.U32();
            break;
        case ActionKind::SetSwitch:
            obj->switch_group_id = r.U32();
            obj->switch_id = r.U32();
            break;
        case ActionKind::BypassFx:
            obj->bypass = r.U8() != 0;
            obj->bypass_mask = r.U8();
            obj->exceptions = ReadExceptions(r);
            break;
        case ActionKind::Seek:
            obj->seek_relative_to_duration = r.U8() != 0;
            obj->seek_value = {r.F32(), r.F32(), r.F32()};
            obj->snap_to_marker = r.U8() != 0;
            obj->exceptions = ReadExceptions(r);
            break;
        case ActionKind::NoParams:
            break;
        case ActionKind::Unknown:
            throw BankError(std::format("unknown action type 0x{:04X}", obj->action_type));
    }
    return obj;
}

std::unique_ptr<HircObject> ParseEvent(Reader& r) {
    auto obj = std::make_unique<EventObject>();
    obj->action_ids.resize(r.U32());
    for (auto& id : obj->action_ids) {
        id = r.U32();
    }
    return obj;
}

std::unique_ptr<HircObject> ParseRanSeq(Reader& r, bool feedback) {
    auto obj = std::make_unique<RanSeqObject>();
    obj->node = ReadNodeBase(r, feedback);
    obj->loop_count = r.U16();
    obj->loop_mod_min = r.U16();
    obj->loop_mod_max = r.U16();
    obj->transition_time = r.F32();
    obj->transition_mod_min = r.F32();
    obj->transition_mod_max = r.F32();
    obj->avoid_repeat_count = r.U16();
    obj->transition_mode = static_cast<TransitionMode>(r.U8());
    obj->shuffle = r.U8() == 1;
    obj->sequence = r.U8() == 1;
    obj->using_weight = r.U8() != 0;
    obj->reset_playlist_each_play = r.U8() != 0;
    obj->restart_backward = r.U8() != 0;
    obj->continuous = r.U8() != 0;
    obj->global = r.U8() != 0;
    obj->children = ReadChildren(r);
    obj->playlist.resize(r.U16());
    for (auto& entry : obj->playlist) {
        entry.id = r.U32();
        entry.weight = r.S32();
    }
    return obj;
}

std::unique_ptr<HircObject> ParseSwitch(Reader& r, bool feedback) {
    auto obj = std::make_unique<SwitchObject>();
    obj->node = ReadNodeBase(r, feedback);
    obj->group_type = r.U32();
    obj->group_id = r.U32();
    obj->default_switch = r.U32();
    obj->continuous_validation = r.U8() != 0;
    obj->children = ReadChildren(r);
    obj->switches.resize(r.U32());
    for (auto& sw : obj->switches) {
        sw.switch_id = r.U32();
        sw.nodes.resize(r.U32());
        for (auto& n : sw.nodes) {
            n = r.U32();
        }
    }
    obj->switch_params.resize(r.U32());
    for (auto& p : obj->switch_params) {
        p.node_id = r.U32();
        p.first_only = r.U8() != 0;
        p.continue_playback = r.U8() != 0;
        p.on_switch_mode = r.U32();
        p.fade_out_ms = r.S32();
        p.fade_in_ms = r.S32();
    }
    return obj;
}

std::unique_ptr<HircObject> ParseActorMixer(Reader& r, bool feedback) {
    auto obj = std::make_unique<ActorMixerObject>();
    obj->node = ReadNodeBase(r, feedback);
    obj->children = ReadChildren(r);
    return obj;
}

std::unique_ptr<HircObject> ParseBus(Reader& r, bool feedback) {
    auto obj = std::make_unique<BusObject>();
    obj->parent_bus_id = r.U32();
    obj->props = ReadPropBundle(r);
    obj->positioning_enabled = r.U8() != 0;
    obj->positioning_enable_panner = r.U8() != 0;
    obj->kill_newest = r.U8() != 0;
    obj->use_virtual_behavior = r.U8() != 0;
    obj->max_instances = r.U16();
    obj->max_instances_override_parent = r.U8() != 0;
    obj->channel_config = r.U16();
    r.U8();
    r.U8();
    obj->hdr_bus = r.U8() != 0;
    obj->hdr_release_exponential = r.U8() != 0;
    obj->recovery_time_ms = r.S32();
    obj->max_duck_volume = r.F32();
    obj->ducks.resize(r.U32());
    for (auto& duck : obj->ducks) {
        duck.bus_id = r.U32();
        duck.volume = r.F32();
        duck.fade_out_ms = r.S32();
        duck.fade_in_ms = r.S32();
        duck.curve = static_cast<Interp>(r.U8());
        duck.target_prop = r.U8();
    }
    obj->fx = ReadFxChunk(r);
    obj->rtpcs = ReadRtpcList(r);
    obj->states = ReadStateChunk(r);
    if (feedback) {
        obj->feedback_bus_id = r.U32();
    }
    return obj;
}

std::unique_ptr<HircObject> ParseLayer(Reader& r, bool feedback) {
    auto obj = std::make_unique<LayerObject>();
    obj->node = ReadNodeBase(r, feedback);
    obj->children = ReadChildren(r);
    obj->layers.resize(r.U32());
    for (auto& layer : obj->layers) {
        layer.layer_id = r.U32();
        layer.rtpcs = ReadRtpcList(r);
        layer.crossfade_rtpc_id = r.U32();
        layer.associations.resize(r.U32());
        for (auto& assoc : layer.associations) {
            assoc.child_id = r.U32();
            const uint32_t count = r.U32();
            assoc.curve.points = ReadGraphPoints(r, count);
        }
    }
    return obj;
}

void ReadMusicNode(Reader& r, MusicNodeData& music, bool feedback) {
    music.node = ReadNodeBase(r, feedback);
    music.children = ReadChildren(r);
    music.meter.grid_period_ms = r.F64();
    music.meter.grid_offset_ms = r.F64();
    music.meter.tempo = r.F32();
    music.meter.beats_per_bar = r.U8();
    music.meter.beat_value = r.U8();
    music.meter_override = r.U8() != 0;
    music.stingers.resize(r.U32());
    for (auto& s : music.stingers) {
        s.trigger_id = r.U32();
        s.segment_id = r.U32();
        s.sync = r.U32();
        s.cue_filter = r.U32();
        s.dont_repeat_ms = r.S32();
        s.lookahead = r.U32();
    }
}

std::unique_ptr<HircObject> ParseMusicSegment(Reader& r, bool feedback) {
    auto obj = std::make_unique<MusicSegmentObject>();
    ReadMusicNode(r, obj->music, feedback);
    obj->duration_ms = r.F64();
    obj->markers.resize(r.U32());
    for (auto& marker : obj->markers) {
        marker.id = r.U32();
        marker.position_ms = r.F64();
        const uint32_t name_size = r.U32();
        if (name_size) {
            const auto raw = r.Raw(name_size);
            marker.name.assign(raw.begin(), raw.end());
            while (!marker.name.empty() && marker.name.back() == '\0') {
                marker.name.pop_back();
            }
        }
    }
    return obj;
}

std::unique_ptr<HircObject> ParseMusicTrack(Reader& r, bool feedback) {
    auto obj = std::make_unique<MusicTrackObject>();
    obj->sources.resize(r.U32());
    for (auto& s : obj->sources) {
        s = ReadSource(r);
    }
    const uint32_t count = r.U32();
    if (count) {
        obj->clips.resize(count);
        for (auto& clip : obj->clips) {
            clip.track = r.U32();
            clip.source_id = r.U32();
            clip.play_at_ms = r.F64();
            clip.begin_trim_ms = r.F64();
            clip.end_trim_ms = r.F64();
            clip.source_duration_ms = r.F64();
        }
        obj->sub_track_count = r.U32();
    }
    obj->clip_automation.resize(r.U32());
    for (auto& automation : obj->clip_automation) {
        automation.clip = r.U32();
        automation.kind = r.U32();
        const uint32_t points = r.U32();
        automation.points = ReadGraphPoints(r, points);
    }
    obj->node = ReadNodeBase(r, feedback);
    obj->track_type = r.U32();
    obj->lookahead_ms = r.S32();
    return obj;
}

MusicFade ReadMusicFade(Reader& r) {
    MusicFade fade;
    fade.time_ms = r.S32();
    fade.curve = static_cast<Interp>(r.U32());
    fade.offset_ms = r.S32();
    return fade;
}

std::vector<MusicTransitionRule> ReadTransitionRules(Reader& r) {
    std::vector<MusicTransitionRule> rules(r.U32());
    for (auto& rule : rules) {
        rule.sources.resize(r.U32());
        for (auto& id : rule.sources) {
            id = r.U32();
        }
        rule.destinations.resize(r.U32());
        for (auto& id : rule.destinations) {
            id = r.U32();
        }
        rule.source_fade = ReadMusicFade(r);
        rule.source_sync = r.U32();
        rule.source_cue_filter = r.U32();
        rule.play_post_exit = r.U8() != 0;
        rule.destination_fade = ReadMusicFade(r);
        rule.destination_cue_filter = r.U32();
        rule.jump_to_id = r.U32();
        rule.entry_type = r.U16();
        rule.play_pre_entry = r.U8() != 0;
        rule.match_source_cue_name = r.U8() != 0;
        rule.has_transition_object = r.U8() != 0;
        if (rule.has_transition_object) {
            rule.transition_segment_id = r.U32();
            rule.transition_fade_in = ReadMusicFade(r);
            rule.transition_fade_out = ReadMusicFade(r);
            rule.transition_play_pre_entry = r.U8() != 0;
            rule.transition_play_post_exit = r.U8() != 0;
        }
    }
    return rules;
}

MusicPlaylistNode ReadPlaylistNode(Reader& r, int depth) {
    if (depth > 64) {
        throw BankError("music playlist too deep");
    }
    MusicPlaylistNode node;
    node.segment_id = r.U32();
    node.item_id = r.U32();
    const uint32_t child_count = r.U32();
    node.type = static_cast<MusicPlaylistType>(r.U32());
    node.loop = r.S16();
    node.weight = r.U32();
    node.avoid_repeat_count = r.U16();
    node.using_weight = r.U8() != 0;
    node.shuffle = r.U8() != 0;
    for (uint32_t i = 0; i < child_count; ++i) {
        node.children.push_back(ReadPlaylistNode(r, depth + 1));
    }
    return node;
}

std::unique_ptr<HircObject> ParseMusicRanSeq(Reader& r, bool feedback) {
    auto obj = std::make_unique<MusicRanSeqObject>();
    ReadMusicNode(r, obj->music, feedback);
    obj->rules = ReadTransitionRules(r);
    obj->playlist_item_count = r.U32();
    obj->playlist = ReadPlaylistNode(r, 0);
    return obj;
}

std::unique_ptr<HircObject> ParseAttenuation(Reader& r) {
    auto obj = std::make_unique<AttenuationObject>();
    const uint8_t cone = r.U8();
    obj->cone_enabled = (cone & 1) != 0;
    if (obj->cone_enabled) {
        obj->cone.inside_degrees = r.F32();
        obj->cone.outside_degrees = r.F32();
        obj->cone.outside_volume = r.F32();
        obj->cone.low_pass = r.F32();
    }
    for (auto& slot : obj->curve_to_use) {
        slot = r.S8();
    }
    obj->curves.resize(r.U8());
    for (auto& curve : obj->curves) {
        curve.scaling = static_cast<CurveScaling>(r.U8());
        const uint16_t size = r.U16();
        curve.points = ReadGraphPoints(r, size);
    }
    obj->rtpcs = ReadRtpcList(r);
    return obj;
}

std::unique_ptr<HircObject> ParseDialogueEvent(Reader& r) {
    auto obj = std::make_unique<DialogueEventObject>();
    obj->probability = r.U8();
    const uint32_t depth = r.U32();
    obj->arguments.resize(depth);
    for (auto& arg : obj->arguments) {
        arg.group_id = r.U32();
    }
    for (auto& arg : obj->arguments) {
        arg.group_type = r.U8();
    }
    const uint32_t size = r.U32();
    obj->mode = r.U8();
    const auto raw = r.Raw(size);
    const size_t count = size / 12;
    struct RawNode {
        uint32_t key;
        uint32_t payload;
        uint16_t weight;
        uint16_t probability;
    };
    std::vector<RawNode> nodes(count);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(&nodes[i].key, raw.data() + i * 12, 4);
        std::memcpy(&nodes[i].payload, raw.data() + i * 12 + 4, 4);
        std::memcpy(&nodes[i].weight, raw.data() + i * 12 + 8, 2);
        std::memcpy(&nodes[i].probability, raw.data() + i * 12 + 10, 2);
    }
    auto build = [&](auto&& self, size_t index, uint32_t level) -> DialogueTreeNode {
        if (index >= nodes.size() || level > depth) {
            throw BankError("bad dialogue tree");
        }
        DialogueTreeNode node;
        node.key = nodes[index].key;
        node.weight = nodes[index].weight;
        node.probability = nodes[index].probability;
        if (level == depth) {
            node.audio_node_id = nodes[index].payload;
        } else {
            const uint32_t first = nodes[index].payload & 0xFFFF;
            const uint32_t n = nodes[index].payload >> 16;
            for (uint32_t i = 0; i < n; ++i) {
                node.children.push_back(self(self, first + i, level + 1));
            }
        }
        return node;
    };
    if (!nodes.empty()) {
        obj->tree = build(build, 0, 0);
        obj->has_tree = true;
    }
    return obj;
}

std::unique_ptr<HircObject> ParseFeedbackNode(Reader& r, bool feedback) {
    auto obj = std::make_unique<FeedbackNodeObject>();
    obj->sources.resize(r.U32());
    for (auto& s : obj->sources) {
        s.company_id = r.U16();
        s.device_id = r.U16();
        s.volume_offset = r.F32();
        s.source = ReadSource(r);
    }
    obj->node = ReadNodeBase(r, feedback);
    return obj;
}

std::unique_ptr<HircObject> ParseFx(Reader& r) {
    auto obj = std::make_unique<FxObject>();
    obj->plugin_id = r.U32();
    const uint32_t size = r.U32();
    if (size) {
        obj->params = r.Raw(size);
    }
    const uint8_t media_count = r.U8();
    for (uint8_t i = 0; i < media_count; ++i) {
        const uint8_t index = r.U8();
        obj->media.push_back({index, r.U32()});
    }
    obj->rtpcs = ReadRtpcList(r);
    return obj;
}

std::unique_ptr<HircObject> ParseObject(HircType type, Reader& r, bool feedback) {
    switch (type) {
        case HircType::State: return ParseState(r);
        case HircType::Sound: return ParseSound(r, feedback);
        case HircType::Action: return ParseAction(r);
        case HircType::Event: return ParseEvent(r);
        case HircType::RanSeqCntr: return ParseRanSeq(r, feedback);
        case HircType::SwitchCntr: return ParseSwitch(r, feedback);
        case HircType::ActorMixer: return ParseActorMixer(r, feedback);
        case HircType::Bus:
        case HircType::FeedbackBus:
        case HircType::AuxBus: return ParseBus(r, feedback);
        case HircType::LayerCntr: return ParseLayer(r, feedback);
        case HircType::MusicSegment: return ParseMusicSegment(r, feedback);
        case HircType::MusicTrack: return ParseMusicTrack(r, feedback);
        case HircType::MusicRanSeqCntr: return ParseMusicRanSeq(r, feedback);
        case HircType::Attenuation: return ParseAttenuation(r);
        case HircType::DialogueEvent: return ParseDialogueEvent(r);
        case HircType::FeedbackNode: return ParseFeedbackNode(r, feedback);
        case HircType::FxShareSet:
        case HircType::FxCustom: return ParseFx(r);
        case HircType::MusicSwitchCntr: {
            auto obj = std::make_unique<MusicSwitchObject>();
            ReadMusicNode(r, obj->music, feedback);
            obj->rules = ReadTransitionRules(r);
            r.Raw(r.Remaining());
            return obj;
        }
    }
    return nullptr;
}

GlobalSettings ParseGlobalSettings(Reader& r) {
    GlobalSettings s;
    s.volume_threshold_db = r.F32();
    s.max_voices = r.U16();
    s.state_groups.resize(r.U32());
    for (auto& group : s.state_groups) {
        group.id = r.U32();
        group.default_transition_ms = r.U32();
        group.transitions.resize(r.U32());
        for (auto& t : group.transitions) {
            t.from = r.U32();
            t.to = r.U32();
            t.time_ms = r.U32();
        }
    }
    s.switch_groups.resize(r.U32());
    for (auto& group : s.switch_groups) {
        group.group_id = r.U32();
        group.rtpc_id = r.U32();
        const uint32_t count = r.U32();
        group.points = ReadGraphPoints(r, count);
    }
    s.game_parameters.resize(r.U32());
    for (auto& p : s.game_parameters) {
        p.id = r.U32();
        p.value = r.F32();
    }
    return s;
}

EnvSettings ParseEnvSettings(Reader& r) {
    EnvSettings env;
    for (EnvCurve* curve : {&env.obstruction_volume, &env.obstruction_lpf, &env.occlusion_volume, &env.occlusion_lpf}) {
        curve->enabled = r.U8() != 0;
        curve->curve.scaling = static_cast<CurveScaling>(r.U8());
        const uint16_t count = r.U16();
        curve->curve.points = ReadGraphPoints(r, count);
    }
    return env;
}

}

uint32_t Fnv1Hash32(std::string_view name) {
    uint32_t value = 2166136261u;
    for (char c : name) {
        unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<unsigned char>(byte - 'A' + 'a');
        }
        value *= 16777619u;
        value ^= byte;
    }
    return value;
}

const char* HircTypeName(HircType type) {
    switch (type) {
        case HircType::State: return "State";
        case HircType::Sound: return "Sound";
        case HircType::Action: return "Action";
        case HircType::Event: return "Event";
        case HircType::RanSeqCntr: return "RanSeqCntr";
        case HircType::SwitchCntr: return "SwitchCntr";
        case HircType::ActorMixer: return "ActorMixer";
        case HircType::Bus: return "Bus";
        case HircType::LayerCntr: return "LayerCntr";
        case HircType::MusicSegment: return "MusicSegment";
        case HircType::MusicTrack: return "MusicTrack";
        case HircType::MusicSwitchCntr: return "MusicSwitchCntr";
        case HircType::MusicRanSeqCntr: return "MusicRanSeqCntr";
        case HircType::Attenuation: return "Attenuation";
        case HircType::DialogueEvent: return "DialogueEvent";
        case HircType::FeedbackBus: return "FeedbackBus";
        case HircType::FeedbackNode: return "FeedbackNode";
        case HircType::FxShareSet: return "FxShareSet";
        case HircType::FxCustom: return "FxCustom";
        case HircType::AuxBus: return "AuxBus";
    }
    return "Unknown";
}

bool prop::IsInteger(uint8_t id) {
    switch (id) {
        case 0x07:
        case 0x0E:
        case 0x0F:
        case 0x11:
        case 0x1D:
        case 0x28:
        case 0x29:
        case 0x2B:
        case 0x2C:
            return true;
        default:
            return false;
    }
}

float InterpolateShape(Interp shape, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float kHalfPi = 1.57079632679f;
    constexpr float kPi = 3.14159265359f;
    switch (shape) {
        case Interp::Log3: {
            const float u = 1.0f - t;
            return 1.0f - u * u * u;
        }
        case Interp::Sine: return std::sin(t * kHalfPi);
        case Interp::Log1: return t * (3.0f - t) * 0.5f;
        case Interp::InvSCurve: return t <= 0.5f ? 0.5f * std::sin(t * kPi) : 1.0f - 0.5f * std::sin(t * kPi);
        case Interp::Linear: return t;
        case Interp::SCurve: return 0.5f * (1.0f - std::cos(t * kPi));
        case Interp::Exp1: return t * (t + 1.0f) * 0.5f;
        case Interp::SineRecip: return 1.0f - std::cos(t * kHalfPi);
        case Interp::Exp3: return t * t * t;
        case Interp::Constant: return t >= 1.0f ? 1.0f : 0.0f;
    }
    return t;
}

float ApplyScaling(CurveScaling scaling, float value) {
    switch (scaling) {
        case CurveScaling::Db: {
            const float y = std::clamp(value, -1.0f, 1.0f);
            const float linear = 1.0f - std::fabs(y);
            const float db = linear > 1.5849e-5f ? 20.0f * std::log10(linear) : -96.3f;
            return y < 0.0f ? db : -db;
        }
        case CurveScaling::DbToLin:
            return std::pow(10.0f, value / 20.0f);
        default:
            return value;
    }
}

float Curve::EvaluateRaw(float x) const {
    if (points.empty()) {
        return 0.0f;
    }
    if (x <= points.front().x) {
        return points.front().y;
    }
    if (x >= points.back().x) {
        return points.back().y;
    }
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const CurvePoint& a = points[i];
        const CurvePoint& b = points[i + 1];
        if (x < b.x) {
            if (a.interp == Interp::Constant) {
                return a.y;
            }
            const float span = b.x - a.x;
            const float t = span > 0.0f ? (x - a.x) / span : 1.0f;
            return a.y + (b.y - a.y) * InterpolateShape(a.interp, t);
        }
    }
    return points.back().y;
}

float Curve::Evaluate(float x) const {
    return ApplyScaling(scaling, EvaluateRaw(x));
}

const float* PropBundle::Find(uint8_t id) const {
    for (const auto& v : values) {
        if (v.id == id) {
            return &v.value;
        }
    }
    return nullptr;
}

float PropBundle::Get(uint8_t id, float fallback) const {
    const float* v = Find(id);
    return v ? *v : fallback;
}

const RangedPropValue* RangedPropBundle::Find(uint8_t id) const {
    for (const auto& v : values) {
        if (v.id == id) {
            return &v;
        }
    }
    return nullptr;
}

const float* StateObject::Find(uint32_t param) const {
    for (const auto& [id, value] : props) {
        if (id == param) {
            return &value;
        }
    }
    return nullptr;
}

const Curve* AttenuationObject::SlotCurve(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(curve_to_use.size())) {
        return nullptr;
    }
    const int index = curve_to_use[slot];
    if (index < 0 || index >= static_cast<int>(curves.size())) {
        return nullptr;
    }
    return &curves[index];
}

float AttenuationObject::MaxDistance() const {
    const Curve* dry = SlotCurve(kAttVolumeDry);
    return dry && !dry->points.empty() ? dry->points.back().x : 0.0f;
}

uint32_t StateGroupSettings::TransitionTime(uint32_t from, uint32_t to) const {
    for (const auto& t : transitions) {
        if (t.from == from && t.to == to) {
            return t.time_ms;
        }
    }
    return default_transition_ms;
}

bool Bank::Load(std::string name, std::shared_ptr<const std::vector<uint8_t>> storage, size_t offset, size_t size, std::string* error) {
    name_ = std::move(name);
    storage_ = std::move(storage);
    offset_ = offset;
    size_ = size;
    if (!storage_ || offset + size > storage_->size()) {
        if (error) {
            *error = "bank range outside its storage";
        }
        return false;
    }
    const uint8_t* data = storage_->data() + offset;
    try {
        struct Chunk {
            size_t start;
            size_t size;
        };
        std::unordered_map<std::string, Chunk> chunks;
        size_t pos = 0;
        while (pos + 8 <= size) {
            std::string tag(reinterpret_cast<const char*>(data + pos), 4);
            uint32_t chunk_size;
            std::memcpy(&chunk_size, data + pos + 4, 4);
            if (pos + 8 + chunk_size > size) {
                throw BankError(std::format("chunk {} runs past the bank", tag));
            }
            chunks[tag] = {pos + 8, chunk_size};
            pos += 8 + chunk_size;
        }
        auto header = chunks.find("BKHD");
        if (header == chunks.end() || header->second.size < 20) {
            throw BankError("no BKHD chunk");
        }
        Reader h(data, header->second.start, header->second.start + header->second.size);
        version_ = h.U32();
        id_ = h.U32();
        language_id_ = h.U32();
        feedback_ = (h.U32() & 1) != 0;
        project_id_ = h.U32();
        if (version_ != kSupportedVersion) {
            throw BankError(std::format("bank version {} is not supported (only {})", version_, kSupportedVersion));
        }
        auto didx = chunks.find("DIDX");
        auto data_chunk = chunks.find("DATA");
        if (didx != chunks.end() && data_chunk != chunks.end()) {
            data_offset_ = data_chunk->second.start;
            Reader r(data, didx->second.start, didx->second.start + didx->second.size);
            for (size_t i = 0; i < didx->second.size / 12; ++i) {
                MediaEntry entry;
                entry.id = r.U32();
                entry.offset = r.U32();
                entry.size = r.U32();
                if (static_cast<size_t>(entry.offset) + entry.size > data_chunk->second.size) {
                    throw BankError(std::format("media {} runs past DATA", entry.id));
                }
                entry.offset += static_cast<uint32_t>(data_offset_);
                media_.push_back(entry);
            }
        }
        if (auto hirc = chunks.find("HIRC"); hirc != chunks.end()) {
            Reader r(data, hirc->second.start, hirc->second.start + hirc->second.size);
            const uint32_t count = r.U32();
            objects_.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                const auto type = static_cast<HircType>(r.U8());
                const uint32_t object_size = r.U32();
                const size_t body = r.Pos();
                if (body + object_size > hirc->second.start + hirc->second.size) {
                    throw BankError("HIRC object runs past the chunk");
                }
                Reader b(data, body, body + object_size);
                const uint32_t id = b.U32();
                std::unique_ptr<HircObject> object;
                try {
                    object = ParseObject(type, b, feedback_);
                } catch (const BankError& e) {
                    throw BankError(std::format("{} {}: {}", HircTypeName(type), id, e.what()));
                }
                if (object) {
                    if (b.Remaining() != 0) {
                        throw BankError(std::format("{} {}: {} bytes left unparsed", HircTypeName(type), id, b.Remaining()));
                    }
                    object->type = type;
                    object->id = id;
                    objects_.push_back(std::move(object));
                }
                Reader skip(data, body + object_size, hirc->second.start + hirc->second.size);
                r = skip;
            }
        }
        if (auto stmg = chunks.find("STMG"); stmg != chunks.end()) {
            Reader r(data, stmg->second.start, stmg->second.start + stmg->second.size);
            settings_ = ParseGlobalSettings(r);
        }
        if (auto envs = chunks.find("ENVS"); envs != chunks.end()) {
            Reader r(data, envs->second.start, envs->second.start + envs->second.size);
            environment_ = ParseEnvSettings(r);
        }
        if (auto stid = chunks.find("STID"); stid != chunks.end()) {
            Reader r(data, stid->second.start, stid->second.start + stid->second.size);
            r.U32();
            const uint32_t count = r.U32();
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t bank_id = r.U32();
                const auto raw = r.Raw(r.U8());
                bank_names_[bank_id] = std::string(raw.begin(), raw.end());
            }
        }
    } catch (const BankError& e) {
        if (error) {
            *error = std::format("{}: {}", name_, e.what());
        }
        return false;
    }
    return true;
}

std::span<const uint8_t> Bank::Bytes() const {
    return std::span<const uint8_t>(storage_->data() + offset_, size_);
}

std::span<const uint8_t> Bank::MediaBytes(const MediaEntry& entry) const {
    return std::span<const uint8_t>(storage_->data() + offset_ + entry.offset, entry.size);
}

const NodeBase* GetNodeBase(const HircObject* object) {
    if (!object) {
        return nullptr;
    }
    switch (object->type) {
        case HircType::Sound: return &static_cast<const SoundObject*>(object)->node;
        case HircType::RanSeqCntr: return &static_cast<const RanSeqObject*>(object)->node;
        case HircType::SwitchCntr: return &static_cast<const SwitchObject*>(object)->node;
        case HircType::ActorMixer: return &static_cast<const ActorMixerObject*>(object)->node;
        case HircType::LayerCntr: return &static_cast<const LayerObject*>(object)->node;
        case HircType::MusicSegment: return &static_cast<const MusicSegmentObject*>(object)->music.node;
        case HircType::MusicTrack: return &static_cast<const MusicTrackObject*>(object)->node;
        case HircType::MusicRanSeqCntr: return &static_cast<const MusicRanSeqObject*>(object)->music.node;
        case HircType::MusicSwitchCntr: return &static_cast<const MusicSwitchObject*>(object)->music.node;
        case HircType::FeedbackNode: return &static_cast<const FeedbackNodeObject*>(object)->node;
        default: return nullptr;
    }
}

const std::vector<uint32_t>* GetChildren(const HircObject* object) {
    if (!object) {
        return nullptr;
    }
    switch (object->type) {
        case HircType::RanSeqCntr: return &static_cast<const RanSeqObject*>(object)->children;
        case HircType::SwitchCntr: return &static_cast<const SwitchObject*>(object)->children;
        case HircType::ActorMixer: return &static_cast<const ActorMixerObject*>(object)->children;
        case HircType::LayerCntr: return &static_cast<const LayerObject*>(object)->children;
        case HircType::MusicSegment: return &static_cast<const MusicSegmentObject*>(object)->music.children;
        case HircType::MusicRanSeqCntr: return &static_cast<const MusicRanSeqObject*>(object)->music.children;
        case HircType::MusicSwitchCntr: return &static_cast<const MusicSwitchObject*>(object)->music.children;
        default: return nullptr;
    }
}

}
