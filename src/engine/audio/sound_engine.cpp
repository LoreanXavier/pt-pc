#include "engine/audio/sound_engine.h"

#include "engine/audio/channel_layout.h"

#if defined(__aarch64__)
#include <arm_acle.h>
#else
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include "engine/core/log.h"
#include "engine/platform/os.h"

namespace pt::audio {
namespace {

constexpr float kSamplesPerMs = kOutputRate / 1000.0f;
constexpr int kMaxPlayDepth = 32;
constexpr uint16_t kRumbleDevice = 406;
constexpr float kCenterGain = 0.70710678f;
// the eboot's audio frame: an item that ends inside one hands over to the next item of a Disabled transition at the frame's end
constexpr uint64_t kEbootFrame = 1024;

uint64_t MsToSamples(double ms) {
    return ms <= 0.0 ? 0 : static_cast<uint64_t>(std::llround(ms * kSamplesPerMs));
}

constexpr uint32_t kStateParams[6] = {rtpc_param::Volume, rtpc_param::Pitch, rtpc_param::Lpf, rtpc_param::BusVolume,
                                      rtpc_param::OutputBusVolume, rtpc_param::GameAuxSendVolume};

enum Speaker { kFl, kFr, kFc, kBl, kBr, kSl, kSr, kSpeakers };
constexpr int kSpeakerAngles[3] = {43, 142, 203};
constexpr float kSpreadDensity = 1.0f / 43.0f;
constexpr float kPanUnitsPerRadian = 81.48733f;
constexpr uint8_t kSpreadOrder[7][7] = {{kFl}, {kFl, kFr}, {kFl, kFc, kFr}, {kBl, kFl, kFr, kBr}, {kBl, kFl, kFc, kFr, kBr},
                                        {kBl, kSl, kFl, kFr, kSr, kBr}, {kBl, kSl, kFl, kFc, kFr, kSr, kBr}};
constexpr float kFoldLeft[kSpeakers] = {1.0f, 0.0f, kCenterGain, kCenterGain, 0.0f, kCenterGain, 0.0f};
constexpr float kFoldRight[kSpeakers] = {0.0f, 1.0f, kCenterGain, 0.0f, kCenterGain, 0.0f, kCenterGain};

using PanTable = std::array<std::array<float, 2>, 257>;

PanTable BuildPanTable(bool center) {
    constexpr double kStep = 6.283185307179586 / 512.0;
    PanTable table{};
    int i = 0;
    for (int k = 0; k <= 3; ++k) {
        double half = 0.0;
        double mid = 0.0;
        int end = 256;
        if (k == 0) {
            end = kSpeakerAngles[0];
            half = center ? end * kStep * 0.5 : end * kStep;
            mid = center ? half : 0.0;
        } else if (k < 3) {
            end = kSpeakerAngles[k];
            half = (kSpeakerAngles[k] - kSpeakerAngles[k - 1]) * kStep * 0.5;
            mid = kSpeakerAngles[k - 1] * kStep + half;
        } else {
            half = (256 - kSpeakerAngles[2]) * kStep;
            mid = 3.141592653589793;
        }
        for (; i <= end; ++i) {
            const double phi = i * kStep - mid;
            const double p0 = std::sin(half - phi) * std::sin(half - phi);
            const double p1 = std::sin(half + phi) * std::sin(half + phi);
            table[i] = {static_cast<float>(p0 / (p0 + p1)), static_cast<float>(p1 / (p0 + p1))};
        }
    }
    return table;
}

void AddSpeakerPower(float center, int index, float* power) {
    static const PanTable plane = BuildPanTable(false);
    static const PanTable front = BuildPanTable(true);
    index &= 0x1FF;
    const bool left = index < 0x101;
    const int u = left ? index : 0x200 - index;
    int a = 0;
    int b = 0;
    if (u > kSpeakerAngles[2]) {
        a = left ? kBl : kBr;
        b = left ? kBr : kBl;
    } else if (u > kSpeakerAngles[1]) {
        a = left ? kSl : kSr;
        b = left ? kBl : kBr;
    } else if (u > kSpeakerAngles[0]) {
        a = left ? kFl : kFr;
        b = left ? kSl : kSr;
    } else {
        a = left ? kFr : kFl;
        b = left ? kFl : kFr;
        if (center > 0.0f) {
            power[kFc] += center * front[u][0];
            power[a] += (1.0f - center) * plane[u][0];
            power[b] += (1.0f - center) * plane[u][1] + center * front[u][1];
            return;
        }
    }
    power[a] += plane[u][0];
    power[b] += plane[u][1];
}

void SpeakerGains(float azimuth, float center, float spread, uint32_t channels, float (*gains)[kSpeakers]) {
    channels = std::clamp<uint32_t>(channels, 1, 7);
    const float width = spread * 2.56f / static_cast<float>(channels);
    const int points = static_cast<int>(width * kSpreadDensity + 1.0f) + 1;
    const float half = width / static_cast<float>(points);
    float start = spread * 2.56f * (1.0f - 1.0f / static_cast<float>(channels)) - azimuth * kPanUnitsPerRadian;
    if ((points & 1) == 0) {
        start -= half;
    }
    const float norm = 1.0f / static_cast<float>(points * static_cast<int>(channels));
    for (uint32_t k = 0; k < channels; ++k) {
        float* g = gains[kSpreadOrder[channels - 1][k]];
        std::fill(g, g + kSpeakers, 0.0f);
        float f = 2.0f * half * static_cast<float>(points >> 1) + start + 512.0f;
        for (int p = 0; p < points; ++p) {
            AddSpeakerPower(center, static_cast<int>(f + 0.5f), g);
            f -= 2.0f * half;
        }
        for (int s = 0; s < kSpeakers; ++s) {
            g[s] = std::sqrt(g[s] * norm);
        }
        start -= 2.0f * width;
    }
}

int SpeakerSlot(uint32_t bit) {
    switch (bit) {
        case 0x1: return kFl;
        case 0x2: return kFr;
        case 0x4: return kFc;
        case 0x10: return kBl;
        case 0x20: return kBr;
        case 0x200: return kSl;
        case 0x400: return kSr;
        default: return -1;
    }
}

void DownmixCoefficients(uint32_t channels, uint32_t mask, std::array<float, 8>& left, std::array<float, 8>& right) {
    left.fill(0.0f);
    right.fill(0.0f);
    if (channels == 1) {
        left[0] = right[0] = kCenterGain;
        if (mask == 0x8) {
            left[0] = right[0] = 0.5f;
        }
        return;
    }
    if (std::popcount(mask) != static_cast<int>(channels)) {
        mask = DefaultChannelMask(channels);
    }
    uint32_t c = 0;
    for (uint32_t bit = 0; bit < 18 && c < channels && c < 8; ++bit) {
        if (!(mask & (1u << bit))) {
            continue;
        }
        switch (1u << bit) {
            case 0x1: left[c] = 1.0f; break;
            case 0x2: right[c] = 1.0f; break;
            case 0x4: left[c] = right[c] = kCenterGain; break;
            case 0x8: left[c] = right[c] = 0.5f; break;
            case 0x10: left[c] = kCenterGain; break;
            case 0x20: right[c] = kCenterGain; break;
            case 0x40: left[c] = 1.0f; break;
            case 0x80: right[c] = 1.0f; break;
            case 0x100: left[c] = right[c] = 0.5f; break;
            case 0x200: left[c] = kCenterGain; break;
            case 0x400: right[c] = kCenterGain; break;
            default: left[c] = right[c] = 0.5f; break;
        }
        ++c;
    }
    if (channels == 2 && mask == 0x3) {
        left = {1.0f, 0.0f};
        right = {0.0f, 1.0f};
    }
}

struct ToneParams {
    float gain_db = 0.0f;
    float start_freq = 440.0f;
    float stop_freq = 440.0f;
    float start_rand_min = 0.0f;
    float start_rand_max = 0.0f;
    bool sweep = false;
    uint32_t sweep_type = 0;
    uint32_t wave = 0;
    uint32_t mode = 0;
    float fixed_duration = 1.0f;
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain_duration = 1.0f;
    float sustain_db = 0.0f;
    float release = 0.0f;
};

ToneParams ReadToneParams(const std::vector<uint8_t>& p) {
    ToneParams t;
    auto f32 = [&](size_t off) {
        float v = 0.0f;
        if (off + 4 <= p.size()) {
            std::memcpy(&v, p.data() + off, 4);
        }
        return v;
    };
    auto u32 = [&](size_t off) {
        uint32_t v = 0;
        if (off + 4 <= p.size()) {
            std::memcpy(&v, p.data() + off, 4);
        }
        return v;
    };
    t.gain_db = f32(0);
    t.start_freq = f32(4);
    t.stop_freq = f32(8);
    t.start_rand_min = f32(12);
    t.start_rand_max = f32(16);
    t.sweep = p.size() > 20 && p[20] != 0;
    t.sweep_type = u32(21);
    t.wave = u32(33);
    t.mode = u32(37);
    t.fixed_duration = f32(41);
    t.attack = f32(45);
    t.decay = f32(49);
    t.sustain_duration = f32(53);
    t.sustain_db = f32(57);
    t.release = f32(61);
    return t;
}

float ToneDuration(const ToneParams& t) {
    return t.mode == 0 ? t.fixed_duration : t.attack + t.decay + t.sustain_duration + t.release;
}

}

float Ramp::Step(int64_t frame) const {
    if (frame < 0) {
        return from;
    }
    if (frame >= static_cast<int64_t>(frames)) {
        return end;
    }
    // 0x603230: position (frame - start) / frames through the curve 0x11A8990, the gains of a dB transition back to dB
    const float x = static_cast<float>(frame) / static_cast<float>(frames);
    if (decibels) {
        const float a = FastPow10(from * 0.05f);
        const float b = FastPow10(to * 0.05f);
        return FastGainToDb(a + (b - a) * InterpolateShape(curve, x));
    }
    return from + (to - from) * InterpolateShape(curve, x);
}

float Ramp::Value(uint64_t t) const {
    if (length == 0 || t >= start + length) {
        return end;
    }
    if (t <= start) {
        return from;
    }
    // frame k holds the value of step k and the mixer ramps from step k - 1 to it over the frame
    const uint64_t elapsed = t - start;
    const int64_t frame = static_cast<int64_t>(elapsed / kEbootFrame);
    const float f = static_cast<float>(elapsed % kEbootFrame) / static_cast<float>(kEbootFrame);
    const float a = Step(frame - 1);
    return a + (Step(frame) - a) * f;
}

uint64_t Ramp::Samples(float ms) {
    const int duration = static_cast<int>(ms);
    return duration <= 0 ? 0 : (static_cast<uint64_t>((duration + 20) / 21) + 1) * kEbootFrame;
}

void Ramp::Transition(float target, uint64_t now, float ms, Interp shape, bool db, bool mirror) {
    const float current = Value(now);
    decibels = db;
    // 0x5684D0, 0x578AD0, 0x57E520: no time or no change sets the value at once; otherwise 0x603390 starts a transition from the
    // current value, (ms + 20) / 21 frames long
    const int duration = static_cast<int>(ms);
    if (duration <= 0 || target == current) {
        Jump(target);
        return;
    }
    from = current;
    to = target;
    start = now;
    frames = static_cast<uint32_t>((duration + 20) / 21);
    length = Samples(ms);
    curve = shape == Interp::Constant ? Interp::Linear : shape;
    if (mirror && to <= from && curve != Interp::SCurve && curve != Interp::InvSCurve) {
        curve = static_cast<Interp>(8 - static_cast<int>(curve));
    }
    end = decibels ? FastGainToDb(FastPow10(to * 0.05f)) : to;
}

SoundEngine::SoundEngine(std::unique_ptr<SoundBankSet> banks) : banks_(std::move(banks)), rng_(std::random_device{}()) {
    for (const auto& bank : banks_->Banks()) {
        for (const auto& object : bank->Objects()) {
            if (object->type == HircType::Event) {
                events_[object->id] = static_cast<const EventObject*>(object.get());
            } else if (object->type == HircType::DialogueEvent) {
                dialogue_events_[object->id] = static_cast<const DialogueEventObject*>(object.get());
            }
        }
        if (bank->Settings()) {
            const auto& settings = *bank->Settings();
            volume_threshold_db_ = settings.volume_threshold_db;
            max_voices_ = settings.max_voices ? settings.max_voices : 256;
            for (const auto& group : settings.state_groups) {
                state_groups_[group.id] = &group;
            }
            for (const auto& parameter : settings.game_parameters) {
                rtpc_defaults_[parameter.id] = parameter.value;
            }
        }
        if (bank->Environment()) {
            environment_ = &*bank->Environment();
        }
    }
    for (const auto& [id, value] : rtpc_defaults_) {
        global_rtpc_[id].Jump(value);
    }
    BuildBuses();
    BuildNodes();
    BuildStateSlots();
    objects_[0].name = "global";
    trace_ = os::GetEnv("PT_AUDIO_TRACE").starts_with('1');
    mix_left_.assign(kBlockFrames, 0.0f);
    mix_right_.assign(kBlockFrames, 0.0f);
    voice_left_.assign(kBlockFrames, 0.0f);
    voice_right_.assign(kBlockFrames, 0.0f);
    for (auto& channel : sc_) {
        channel.assign(kBlockFrames, 0.0f);
    }
    for (auto& channel : sc_voice_) {
        channel.assign(kBlockFrames, 0.0f);
    }
    sc_peak_.assign(kBlockFrames, 0.0f);
    sc_weight_.assign(kBlockFrames, 0.0f);
    LogInfo("audio: {} events, {} nodes, {} buses, threshold {} dB, {} voices", events_.size(), nodes_.size(), buses_.size(),
            volume_threshold_db_, max_voices_);
}

SoundEngine::~SoundEngine() = default;

void SoundEngine::BuildBuses() {
    for (const auto& bank : banks_->Banks()) {
        for (const auto& object : bank->Objects()) {
            if (object->type != HircType::Bus && object->type != HircType::AuxBus) {
                continue;
            }
            auto bus = std::make_unique<BusRuntime>();
            bus->id = object->id;
            bus->object = static_cast<const BusObject*>(object.get());
            bus->left.assign(kBlockFrames, 0.0f);
            bus->right.assign(kBlockFrames, 0.0f);
            for (const auto& slot : bus->object->fx.slots) {
                const auto* fx = banks_->Find(slot.fx_id);
                if (!fx || (fx->type != HircType::FxCustom && fx->type != HircType::FxShareSet)) {
                    continue;
                }
                if (auto effect = CreateEffect(*static_cast<const FxObject*>(fx))) {
                    if (object->type == HircType::AuxBus) {
                        effect->MuteDry();
                    }
                    bus->mono_input = bus->mono_input || effect->MonoInput();
                    bus->front_input = bus->front_input || effect->FrontInput();
                    bus->sum_input = bus->sum_input || effect->SumInput();
                    bus->tail_seconds = std::max(bus->tail_seconds, effect->TailSeconds());
                    bus->effects.push_back(std::move(effect));
                }
            }
            buses_[object->id] = std::move(bus);
        }
    }
    for (auto& [id, bus] : buses_) {
        bus->parent = Bus(bus->object->parent_bus_id);
    }
    for (auto& [id, bus] : buses_) {
        int depth = 0;
        for (BusRuntime* b = bus->parent; b && depth < 64; b = b->parent) {
            ++depth;
        }
        bus->depth = depth;
        bus_order_.push_back(bus.get());
    }
    std::sort(bus_order_.begin(), bus_order_.end(), [](const BusRuntime* a, const BusRuntime* b) {
        return a->depth != b->depth ? a->depth > b->depth : a->id < b->id;
    });
    master_ = Bus(kMasterAudioBus);
    if (!master_) {
        for (BusRuntime* bus : bus_order_) {
            if (!bus->parent) {
                master_ = bus;
            }
        }
    }
}

void SoundEngine::BuildNodes() {
    for (const auto& bank : banks_->Banks()) {
        for (const auto& object : bank->Objects()) {
            if (const NodeBase* base = GetNodeBase(object.get())) {
                NodeInfo& info = nodes_[object->id];
                info.id = object->id;
                info.object = object.get();
                info.base = base;
            }
        }
    }
    for (auto& [id, info] : nodes_) {
        info.parent = info.base->parent_id ? Info(info.base->parent_id) : nullptr;
    }
    for (auto& [id, info] : nodes_) {
        int depth = 0;
        const NodeInfo* n = &info;
        bool found_bus = false;
        bool found_pos = false;
        bool found_game_aux = false;
        bool found_user_aux = false;
        bool found_limit = false;
        bool found_virtual = false;
        bool found_priority = false;
        bool found_center = false;
        for (; n && depth < 64; n = n->parent, ++depth) {
            const NodeBase* b = n->base;
            const bool top = n->parent == nullptr;
            if (const float* center = b->props.Find(prop::CenterPct); center && !found_center) {
                info.center = std::clamp(*center / 100.0f, 0.0f, 1.0f);
                found_center = true;
            }
            if (!found_bus && b->override_bus_id) {
                info.bus_id = b->override_bus_id;
                found_bus = true;
            }
            if (!found_pos && (b->positioning.override_parent || top)) {
                info.positioning = &b->positioning;
                info.positioning_owner = b;
                info.positioning_owner_id = n->id;
                found_pos = true;
            }
            if (!found_game_aux && (b->aux.override_game_aux || top)) {
                info.use_game_aux = b->aux.use_game_aux;
                found_game_aux = true;
            }
            if (!found_user_aux && (b->aux.override_user_aux || top)) {
                if (b->aux.has_aux) {
                    info.user_aux = b->aux.user_aux;
                }
                found_user_aux = true;
            }
            if (!found_limit && (b->advanced.max_instances_override_parent || top)) {
                info.limit_owner = n;
                found_limit = true;
            }
            if (!found_virtual && (b->advanced.virtual_voice_override_parent || top)) {
                info.below_threshold = b->advanced.below_threshold;
                info.virtual_queue = b->advanced.virtual_queue;
                found_virtual = true;
            }
            if (!found_priority && (b->priority_override_parent || top)) {
                info.priority = b->props.Get(prop::Priority, 50.0f);
                found_priority = true;
            }
        }
        info.depth = depth;
        if (!info.bus_id || !Bus(info.bus_id)) {
            info.bus_id = master_ ? master_->id : 0;
        }
        if (info.positioning && info.positioning->has_3d) {
            const auto* att = banks_->Find(info.positioning->attenuation_id);
            if (att && att->type == HircType::Attenuation) {
                info.attenuation = static_cast<const AttenuationObject*>(att);
            }
        }
    }
}

void SoundEngine::BuildStateSlots() {
    auto add = [&](uint32_t owner, bool is_bus, const std::vector<StateGroupBinding>& bindings, std::vector<size_t>& out) {
        for (const auto& binding : bindings) {
            StateSlot slot;
            slot.owner = owner;
            slot.is_bus = is_bus;
            slot.binding = &binding;
            out.push_back(state_slots_.size());
            slots_by_group_[binding.group_id].push_back(state_slots_.size());
            state_slots_.push_back(slot);
        }
    };
    for (auto& [id, bus] : buses_) {
        add(id, true, bus->object->states, bus->state_slots);
    }
    for (auto& [id, info] : nodes_) {
        add(id, false, info.base->states, info.state_slots);
    }
}

SoundEngine::NodeInfo* SoundEngine::Info(uint32_t id) {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

SoundEngine::BusRuntime* SoundEngine::Bus(uint32_t id) {
    auto it = buses_.find(id);
    return it == buses_.end() ? nullptr : it->second.get();
}

SoundEngine::ObjectState& SoundEngine::Object(GameObjectId id) {
    return objects_[id];
}

float SoundEngine::Random01() {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_);
}

float SoundEngine::RandomRange(float lo, float hi) {
    if (lo == hi) {
        return lo;
    }
    return lo + (hi - lo) * Random01();
}

bool SoundEngine::HasDialogueEvent(uint32_t id) const {
    return dialogue_events_.contains(id);
}

void SoundEngine::Push(Command command) {
    std::lock_guard lock(command_mutex_);
    commands_.push_back(std::move(command));
}

PlayingId SoundEngine::PostEvent(uint32_t event_id, GameObjectId object, std::shared_ptr<const Media> external) {
    if (!events_.contains(event_id)) {
        return 0;
    }
    PlayingId id = next_playing_id_.fetch_add(1);
    if (id == 0) {
        id = next_playing_id_.fetch_add(1);
    }
    {
        std::lock_guard lock(status_mutex_);
        live_ids_[id] = event_id;
        ++live_events_[event_id];
    }
    Command command;
    command.type = CommandType::PostEvent;
    command.playing_id = id;
    command.object = object;
    command.a = event_id;
    command.media = std::move(external);
    Push(std::move(command));
    return id;
}

PlayingId SoundEngine::PostEventMedia(uint32_t event_id, GameObjectId object, std::vector<uint32_t> media_ids) {
    if (!events_.contains(event_id)) {
        return 0;
    }
    PlayingId id = next_playing_id_.fetch_add(1);
    if (id == 0) {
        id = next_playing_id_.fetch_add(1);
    }
    {
        std::lock_guard lock(status_mutex_);
        live_ids_[id] = event_id;
        ++live_events_[event_id];
    }
    Command command;
    command.type = CommandType::PostEvent;
    command.playing_id = id;
    command.object = object;
    command.a = event_id;
    command.ids = std::move(media_ids);
    Push(std::move(command));
    return id;
}

std::vector<size_t> SoundEngine::ForcedIndices(const RanSeqObject& container, PlayingId id) const {
    std::vector<size_t> out;
    auto it = forced_media_.find(id);
    if (it == forced_media_.end()) {
        return out;
    }
    for (uint32_t media : it->second) {
        for (size_t i = 0; i < container.playlist.size(); ++i) {
            const HircObject* child = banks_->Find(container.playlist[i].id);
            if (child && child->type == HircType::Sound && static_cast<const SoundObject*>(child)->source.source_id == media) {
                out.push_back(i);
                break;
            }
        }
    }
    return out;
}

PlayingId SoundEngine::PostDialogue(uint32_t dialogue_id, std::vector<uint32_t> arguments, GameObjectId object) {
    if (!dialogue_events_.contains(dialogue_id)) {
        return 0;
    }
    PlayingId id = next_playing_id_.fetch_add(1);
    if (id == 0) {
        id = next_playing_id_.fetch_add(1);
    }
    {
        std::lock_guard lock(status_mutex_);
        live_ids_[id] = dialogue_id;
        ++live_events_[dialogue_id];
    }
    Command command;
    command.type = CommandType::PostDialogue;
    command.playing_id = id;
    command.object = object;
    command.a = dialogue_id;
    command.ids = std::move(arguments);
    Push(std::move(command));
    return id;
}

void SoundEngine::StopPlayingId(PlayingId id, float fade_seconds, Interp curve) {
    Command command;
    command.type = CommandType::StopPlaying;
    command.playing_id = id;
    command.value = fade_seconds * 1000.0f;
    command.a = static_cast<uint32_t>(curve);
    Push(std::move(command));
}

void SoundEngine::SeekPlayingId(PlayingId id, float seconds) {
    Command command;
    command.type = CommandType::SeekPlaying;
    command.playing_id = id;
    command.value = seconds;
    Push(std::move(command));
}

void SoundEngine::StopAll(float fade_seconds) {
    Command command;
    command.type = CommandType::StopAll;
    command.value = fade_seconds * 1000.0f;
    Push(std::move(command));
}

void SoundEngine::ResetSession() {
    Command command;
    command.type = CommandType::ResetSession;
    Push(std::move(command));
}

bool SoundEngine::IsPlaying(PlayingId id) const {
    std::lock_guard lock(status_mutex_);
    return live_ids_.contains(id);
}

bool SoundEngine::IsEventPlaying(uint32_t event_id) const {
    std::lock_guard lock(status_mutex_);
    auto it = live_events_.find(event_id);
    return it != live_events_.end() && it->second > 0;
}

void SoundEngine::RegisterObject(GameObjectId object, std::string name) {
    Command command;
    command.type = CommandType::RegisterObject;
    command.object = object;
    command.text = std::move(name);
    Push(std::move(command));
}

void SoundEngine::UnregisterObject(GameObjectId object) {
    Command command;
    command.type = CommandType::UnregisterObject;
    command.object = object;
    Push(std::move(command));
}

void SoundEngine::SetObjectTransform(GameObjectId object, const glm::vec3& position, const glm::vec3& forward) {
    Command command;
    command.type = CommandType::SetTransform;
    command.object = object;
    command.v0 = position;
    command.v1 = forward;
    Push(std::move(command));
}

void SoundEngine::SetObjectAuxSends(GameObjectId object, std::vector<std::pair<uint32_t, float>> sends) {
    Command command;
    command.type = CommandType::SetAuxSends;
    command.object = object;
    command.sends = std::move(sends);
    Push(std::move(command));
}

void SoundEngine::SetObjectObstruction(GameObjectId object, float obstruction, float occlusion) {
    Command command;
    command.type = CommandType::SetObstruction;
    command.object = object;
    command.value = obstruction;
    command.value2 = occlusion;
    Push(std::move(command));
}

void SoundEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    Command command;
    command.type = CommandType::SetListener;
    command.v0 = position;
    command.v1 = forward;
    command.v2 = up;
    Push(std::move(command));
}

void SoundEngine::SetState(uint32_t group, uint32_t state) {
    Command command;
    command.type = CommandType::SetState;
    command.a = group;
    command.b = state;
    Push(std::move(command));
}

void SoundEngine::SetSwitch(uint32_t group, uint32_t value, GameObjectId object) {
    Command command;
    command.type = CommandType::SetSwitch;
    command.a = group;
    command.b = value;
    command.object = object;
    Push(std::move(command));
}

void SoundEngine::SetRtpc(uint32_t parameter, float value, GameObjectId object, bool reset) {
    Command command;
    command.type = reset ? CommandType::ResetRtpc : CommandType::SetRtpc;
    command.a = parameter;
    command.value = value;
    command.object = object;
    Push(std::move(command));
}

void SoundEngine::SetRandomSeed(uint32_t seed) {
    Command command;
    command.type = CommandType::SetSeed;
    command.a = seed;
    Push(std::move(command));
}

void SoundEngine::SetMasterGain(float gain) {
    Command command;
    command.type = CommandType::SetMasterGain;
    command.value = gain;
    Push(std::move(command));
}

std::vector<EngineNotification> SoundEngine::TakeNotifications() {
    std::lock_guard lock(notify_mutex_);
    std::vector<EngineNotification> out;
    out.swap(notifications_);
    return out;
}

EngineStats SoundEngine::Stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void SoundEngine::ApplyCommand(Command& command) {
    switch (command.type) {
        case CommandType::PostEvent:
            if (!command.ids.empty()) {
                forced_media_[command.playing_id] = command.ids;
            }
            StartEvent(command.playing_id, command.a, command.object, command.media);
            break;
        case CommandType::PostDialogue: {
            playing_[command.playing_id] = {command.a, command.object, 0};
            AddRef(command.playing_id);
            auto it = dialogue_events_.find(command.a);
            if (it != dialogue_events_.end() && it->second->has_tree) {
                const DialogueTreeNode* node = &it->second->tree;
                for (size_t level = 0; level < it->second->arguments.size() && node; ++level) {
                    const uint32_t value = level < command.ids.size() ? command.ids[level] : 0;
                    const DialogueTreeNode* match = nullptr;
                    const DialogueTreeNode* fallback = nullptr;
                    for (const auto& child : node->children) {
                        if (child.key == value && value != 0) {
                            match = &child;
                        } else if (child.key == 0) {
                            fallback = &child;
                        }
                    }
                    node = match ? match : fallback;
                }
                if (node && node->audio_node_id) {
                    PlayContext ctx;
                    ctx.playing_id = command.playing_id;
                    ctx.object = command.object;
                    ctx.start_time = now_;
                    PlayNode(node->audio_node_id, ctx);
                }
            }
            Release(command.playing_id);
            break;
        }
        case CommandType::StopPlaying:
            StopPlayingInternal(command.playing_id, command.value, static_cast<Interp>(command.a));
            break;
        case CommandType::SeekPlaying:
            SeekPlayingInternal(command.playing_id, command.value);
            break;
        case CommandType::StopAll:
            StopAllInternal(command.value);
            break;
        case CommandType::ResetSession:
            ResetSessionInternal();
            break;
        case CommandType::RegisterObject:
            Object(command.object).name = command.text;
            removed_objects_.erase(command.object);
            break;
        case CommandType::UnregisterObject: {
            StopObject(command.object);
            ObjectState& object = Object(command.object);
            object.switches.clear();
            object.rtpcs.clear();
            object.has_aux_sends = false;
            object.aux_sends.clear();
            object.obstruction = 0.0f;
            object.occlusion = 0.0f;
            for (auto& [id, list] : action_props_) {
                std::erase_if(list, [&](const ActionProps& props) { return !props.all_objects && props.object == command.object; });
            }
            if (command.object != 0) {
                removed_objects_.insert(command.object);
            }
            break;
        }
        case CommandType::SetTransform: {
            ObjectState& object = Object(command.object);
            object.has_transform = true;
            object.position = command.v0;
            if (glm::dot(command.v1, command.v1) > 1e-12f) {
                object.forward = glm::normalize(command.v1);
            }
            break;
        }
        case CommandType::SetAuxSends: {
            ObjectState& object = Object(command.object);
            object.has_aux_sends = true;
            object.aux_sends = command.sends;
            break;
        }
        case CommandType::SetObstruction: {
            ObjectState& object = Object(command.object);
            object.obstruction = std::clamp(command.value, 0.0f, 1.0f);
            object.occlusion = std::clamp(command.value2, 0.0f, 1.0f);
            break;
        }
        case CommandType::SetListener: {
            listener_position_ = command.v0;
            if (glm::dot(command.v1, command.v1) > 1e-12f) {
                listener_forward_ = glm::normalize(command.v1);
            }
            glm::vec3 up = command.v2 - glm::dot(command.v2, listener_forward_) * listener_forward_;
            if (glm::dot(up, up) < 1e-8f) {
                up = listener_up_ - glm::dot(listener_up_, listener_forward_) * listener_forward_;
            }
            if (glm::dot(up, up) > 1e-8f) {
                listener_up_ = glm::normalize(up);
                listener_right_ = glm::normalize(glm::cross(listener_forward_, listener_up_));
            }
            break;
        }
        case CommandType::SetState:
            SetStateInternal(command.a, command.b, now_);
            break;
        case CommandType::SetSwitch:
            Object(command.object).switches[command.a] = command.b;
            break;
        case CommandType::SetRtpc:
            if (command.object == 0) {
                global_rtpc_[command.a].Jump(command.value);
            } else {
                Object(command.object).rtpcs[command.a].Jump(command.value);
            }
            break;
        case CommandType::ResetRtpc:
            if (command.object == 0) {
                auto it = rtpc_defaults_.find(command.a);
                global_rtpc_[command.a].Jump(it == rtpc_defaults_.end() ? 0.0f : it->second);
            } else {
                Object(command.object).rtpcs.erase(command.a);
            }
            break;
        case CommandType::SetSeed:
            rng_.seed(command.a);
            break;
        case CommandType::SetMasterGain:
            master_gain_ = command.value;
            break;
    }
}

void SoundEngine::AddRef(PlayingId id) {
    auto it = playing_.find(id);
    if (it != playing_.end()) {
        ++it->second.refs;
    }
}

void SoundEngine::Release(PlayingId id) {
    auto it = playing_.find(id);
    if (it == playing_.end()) {
        return;
    }
    if (--it->second.refs > 0) {
        return;
    }
    playing_.erase(it);
    forced_media_.erase(id);
    ended_ids_.push_back(id);
    pending_notifications_.push_back({EngineNotification::Kind::End, id, {}});
}

void SoundEngine::StartEvent(PlayingId id, uint32_t event_id, GameObjectId object, const std::shared_ptr<const Media>& external) {
    playing_[id] = {event_id, object, 0};
    Object(object);
    AddRef(id);
    auto it = events_.find(event_id);
    if (it != events_.end()) {
        for (uint32_t action_id : it->second->action_ids) {
            const HircObject* found = banks_->Find(action_id);
            if (!found || found->type != HircType::Action) {
                continue;
            }
            const auto& action = *static_cast<const ActionObject*>(found);
            float delay = action.props.Get(prop::DelayTime);
            if (const auto* range = action.ranged_props.Find(prop::DelayTime)) {
                delay += RandomRange(range->min, range->max);
            }
            if (delay <= 0.0f) {
                ExecuteAction(action, id, object, now_, external);
            } else {
                PendingAction pending;
                pending.time = now_ + MsToSamples(delay);
                pending.order = order_counter_++;
                pending.playing_id = id;
                pending.object = object;
                pending.action = &action;
                pending.external = external;
                pending_.push_back(std::move(pending));
                AddRef(id);
            }
        }
    }
    Release(id);
}

void SoundEngine::ExecuteAction(const ActionObject& action, PlayingId playing_id, GameObjectId object, uint64_t time,
                                const std::shared_ptr<const Media>& external) {
    float fade = action.props.Get(prop::TransitionTime);
    if (const auto* range = action.ranged_props.Find(prop::TransitionTime)) {
        fade += RandomRange(range->min, range->max);
    }
    if (trace_) {
        LogDebug("audio {:.4f}: action 0x{:04X} {} target {} object {} fade {} ms", time / 48000.0, action.action_type, action.id,
                 action.target_id, object, fade);
    }
    switch (action.Category()) {
        case 0x04:
        case 0x05: {
            PlayContext ctx;
            ctx.playing_id = playing_id;
            ctx.object = object;
            ctx.start_time = time;
            ctx.fade_in_ms = fade;
            ctx.fade_curve = action.fade_curve;
            ctx.external = external;
            PlayNode(action.target_id, ctx);
            break;
        }
        case 0x01:
            StopMatching(action, object, time, fade, action.fade_curve);
            break;
        case 0x02:
            PauseMatching(action, object, time, fade, action.fade_curve, true);
            global_pause_ = global_pause_ || (action.action_type & 0xFF) == 0x04 || (action.action_type & 0xFF) == 0x08;
            break;
        case 0x03:
            PauseMatching(action, object, time, fade, action.fade_curve, false);
            global_pause_ = global_pause_ && (action.action_type & 0xFF) != 0x04 && (action.action_type & 0xFF) != 0x08;
            break;
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
            SetAkProp(action, object, time);
            break;
        case 0x12:
            SetStateInternal(action.state_group_id, action.state_id, time);
            break;
        case 0x19:
            Object(object).switches[action.switch_group_id] = action.switch_id;
            break;
        case 0x13:
        case 0x14:
            SetGameParameter(action, object, time);
            break;
        case 0x1C:
            BreakMatching(action, object);
            break;
        case 0x1E:
            SeekMatching(action, object);
            break;
        default:
            if (warned_.insert(action.id).second) {
                LogWarn("audio: action 0x{:04X} ({}) not supported", action.action_type, action.id);
            }
            break;
    }
}

void SoundEngine::PlayNode(uint32_t node_id, PlayContext ctx) {
    if (ctx.depth > kMaxPlayDepth) {
        return;
    }
    NodeInfo* info = Info(node_id);
    if (!info) {
        if (warned_.insert(node_id).second) {
            LogWarn("audio: play target {} not found", node_id);
        }
        return;
    }
    RollNodeRandom(*info->base, ctx);
    switch (info->object->type) {
        case HircType::Sound:
            PlaySound(info, *static_cast<const SoundObject*>(info->object), ctx);
            break;
        case HircType::RanSeqCntr:
            PlayRanSeq(info, *static_cast<const RanSeqObject*>(info->object), ctx);
            break;
        case HircType::SwitchCntr:
            PlaySwitch(info, *static_cast<const SwitchObject*>(info->object), ctx);
            break;
        case HircType::LayerCntr:
            PlayLayer(info, *static_cast<const LayerObject*>(info->object), ctx);
            break;
        case HircType::MusicRanSeqCntr:
        case HircType::MusicSegment:
            PlayMusic(info, ctx);
            break;
        case HircType::FeedbackNode:
            CreateVoice(info, ctx);
            break;
        default:
            if (warned_.insert(node_id).second) {
                LogWarn("audio: {} {} cannot be played", HircTypeName(info->object->type), node_id);
            }
            break;
    }
}

std::shared_ptr<const MotionGeneratorParams> SoundEngine::MotionParams(uint32_t source_id) {
    auto it = motion_params_.find(source_id);
    if (it != motion_params_.end()) {
        return it->second;
    }
    std::shared_ptr<const MotionGeneratorParams> result;
    const HircObject* fx = banks_->Find(source_id);
    if (fx && (fx->type == HircType::FxCustom || fx->type == HircType::FxShareSet)) {
        auto params = std::make_shared<MotionGeneratorParams>();
        if (params->Parse(static_cast<const FxObject*>(fx)->params)) {
            result = std::move(params);
        }
    }
    if (!result) {
        LogWarn("audio: motion generator parameters {} missing or malformed", source_id);
    }
    motion_params_[source_id] = result;
    return result;
}

MotionLevels SoundEngine::Motion() const {
    const uint32_t packed = motion_levels_.load(std::memory_order_relaxed);
    return MotionLevels{static_cast<uint8_t>(packed & 0xFF), static_cast<uint8_t>(packed >> 8 & 0xFF)};
}

void SoundEngine::PlaySound(NodeInfo* info, const SoundObject& sound, const PlayContext& ctx) {
    CreateVoice(info, ctx);
}

SoundEngine::PlaylistState& SoundEngine::StepPlaylist(const RanSeqObject& container, GameObjectId object) {
    const uint64_t key = container.global ? container.id : (static_cast<uint64_t>(container.id) ^ (object * 0x9E3779B97F4A7C15ull));
    return step_playlists_[key];
}

size_t SoundEngine::PickPlaylistIndex(const RanSeqObject& container, PlaylistState& state, bool continuous) {
    const size_t count = container.playlist.size();
    if (count == 0) {
        return SIZE_MAX;
    }
    if (container.sequence) {
        size_t index = std::min(state.sequence_index, count - 1);
        if (!container.restart_backward) {
            state.sequence_index = (index + 1) % count;
        } else if (!state.backward) {
            if (index + 1 < count) {
                state.sequence_index = index + 1;
            } else {
                state.backward = true;
                state.sequence_index = index > 0 ? index - 1 : 0;
            }
        } else if (index > 0) {
            state.sequence_index = index - 1;
        } else {
            state.backward = false;
            state.sequence_index = count > 1 ? 1 : 0;
        }
        return index;
    }
    const size_t avoid = std::min<size_t>(container.avoid_repeat_count, count - 1);
    std::vector<size_t> candidates;
    if (container.shuffle) {
        if (state.shuffle_pool.empty()) {
            for (size_t i = 0; i < count; ++i) {
                state.shuffle_pool.push_back(i);
            }
        }
        for (size_t i : state.shuffle_pool) {
            if (std::find(state.recent.begin(), state.recent.end(), i) == state.recent.end()) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) {
            candidates = state.shuffle_pool;
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (std::find(state.recent.begin(), state.recent.end(), i) == state.recent.end()) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) {
            for (size_t i = 0; i < count; ++i) {
                candidates.push_back(i);
            }
        }
    }
    double total = 0.0;
    for (size_t i : candidates) {
        total += container.using_weight ? std::max(container.playlist[i].weight, 0) : 1;
    }
    size_t picked = candidates.front();
    if (total > 0.0) {
        double r = static_cast<double>(Random01()) * total;
        for (size_t i : candidates) {
            r -= container.using_weight ? std::max(container.playlist[i].weight, 0) : 1;
            if (r <= 0.0) {
                picked = i;
                break;
            }
            picked = i;
        }
    }
    if (avoid > 0) {
        state.recent.push_back(picked);
        while (state.recent.size() > avoid) {
            state.recent.erase(state.recent.begin());
        }
    }
    if (container.shuffle) {
        state.shuffle_pool.erase(std::remove(state.shuffle_pool.begin(), state.shuffle_pool.end(), picked), state.shuffle_pool.end());
    }
    return picked;
}

void SoundEngine::PlayRanSeq(NodeInfo* info, const RanSeqObject& container, const PlayContext& ctx) {
    const std::vector<size_t> forced = ForcedIndices(container, ctx.playing_id);
    if (!container.continuous) {
        const size_t index = !forced.empty() ? forced.front() : PickPlaylistIndex(container, StepPlaylist(container, ctx.object), false);
        if (index != SIZE_MAX) {
            PlayContext child = ctx;
            ++child.depth;
            PlayNode(container.playlist[index].id, child);
        }
        return;
    }
    auto seq = std::make_unique<Sequencer>();
    seq->serial = serial_counter_++;
    seq->container = &container;
    seq->node = info;
    seq->playing_id = ctx.playing_id;
    seq->object = ctx.object;
    seq->parent = ctx.sequencer;
    seq->parent_token = ctx.item_token;
    seq->layers = ctx.layers;
    seq->external = ctx.external;
    seq->random_volume = ctx.random_volume;
    seq->random_pitch = ctx.random_pitch;
    seq->random_lpf = ctx.random_lpf;
    int loops = container.loop_count;
    if (loops > 0 && (container.loop_mod_min || container.loop_mod_max)) {
        loops += static_cast<int>(std::lround(RandomRange(-static_cast<float>(container.loop_mod_min), container.loop_mod_max)));
        loops = std::max(loops, 1);
    }
    seq->loops_left = container.loop_count == 0 ? -1 : loops;
    seq->items_left_in_pass = container.playlist.size();
    if (!forced.empty()) {
        seq->forced = forced;
        seq->loops_left = 1;
        seq->items_left_in_pass = forced.size();
    }
    if (!container.reset_playlist_each_play) {
        seq->playlist = StepPlaylist(container, ctx.object);
    }
    seq->first_fade_ms = ctx.fade_in_ms;
    seq->first_fade_curve = ctx.fade_curve;
    if (Sequencer* parent = FindSequencer(ctx.sequencer)) {
        ++parent->items[ctx.item_token];
    }
    AddRef(ctx.playing_id);
    Sequencer* raw = seq.get();
    sequencers_.push_back(std::move(seq));
    SequencerStartItem(*raw, ctx.start_time);
}

void SoundEngine::PlaySwitch(NodeInfo* info, const SwitchObject& container, const PlayContext& ctx) {
    uint32_t value = 0;
    if (container.group_type == 1) {
        auto it = states_.find(container.group_id);
        value = it == states_.end() ? 0 : it->second;
    } else {
        const ObjectState& object = Object(ctx.object);
        auto it = object.switches.find(container.group_id);
        if (it != object.switches.end()) {
            value = it->second;
        } else if (ctx.object != 0) {
            const ObjectState& global = Object(0);
            auto git = global.switches.find(container.group_id);
            value = git == global.switches.end() ? 0 : git->second;
        }
    }
    if (value == 0) {
        value = container.default_switch;
    }
    const SwitchPackage* package = nullptr;
    for (const auto& p : container.switches) {
        if (p.switch_id == value) {
            package = &p;
        }
    }
    if (!package) {
        for (const auto& p : container.switches) {
            if (p.switch_id == container.default_switch) {
                package = &p;
            }
        }
    }
    if (!package) {
        return;
    }
    for (uint32_t node : package->nodes) {
        PlayContext child = ctx;
        ++child.depth;
        PlayNode(node, child);
    }
}

void SoundEngine::PlayLayer(NodeInfo* info, const LayerObject& container, const PlayContext& ctx) {
    for (uint32_t child_id : container.children) {
        PlayContext child = ctx;
        ++child.depth;
        for (const auto& layer : container.layers) {
            for (const auto& assoc : layer.associations) {
                if (assoc.child_id == child_id) {
                    child.layers.push_back({&layer, layer.crossfade_rtpc_id && !assoc.curve.Empty() ? &assoc.curve : nullptr});
                }
            }
        }
        PlayNode(child_id, child);
    }
}

bool SoundEngine::AdmitVoice(NodeInfo* info, GameObjectId object, float priority) {
    if (created_this_block_ >= max_voices_ * 2) {
        return false;
    }
    if (const NodeInfo* owner = info->limit_owner) {
        const AdvancedSettings& adv = owner->base->advanced;
        if (adv.max_instances > 0) {
            int count = 0;
            Voice* oldest = nullptr;
            for (auto& v : voices_) {
                if (v->finished || v->stopping || (!adv.global_limit && v->object != object) || !ChainContains(v->node, owner->id)) {
                    continue;
                }
                ++count;
                if (!oldest || v->serial < oldest->serial) {
                    oldest = v.get();
                }
            }
            if (count >= adv.max_instances) {
                if (adv.kill_newest || !oldest) {
                    return false;
                }
                StopVoice(*oldest, now_, 5.0f, Interp::Linear);
                if (Sequencer* seq = FindSequencer(oldest->sequencer); seq && ChainContains(seq->node, owner->id)) {
                    seq->finished = true;
                }
            }
        }
    }
    uint32_t live = 0;
    Voice* victim = nullptr;
    for (auto& v : voices_) {
        if (v->finished) {
            continue;
        }
        ++live;
        const float p = v->node ? v->node->priority : 50.0f;
        const float vp = victim && victim->node ? victim->node->priority : 50.0f;
        if (!victim || p < vp || (p == vp && v->serial < victim->serial)) {
            victim = v.get();
        }
    }
    if (live >= max_voices_) {
        const float vp = victim && victim->node ? victim->node->priority : 50.0f;
        if (!victim || vp >= priority) {
            return false;
        }
        FinishVoice(*victim, now_);
    }
    return true;
}

SoundEngine::Voice* SoundEngine::CreateVoice(NodeInfo* info, const PlayContext& ctx, std::shared_ptr<const Media> clip, uint64_t clip_begin,
                                             uint64_t clip_end) {
    std::shared_ptr<const Media> media;
    Generator generator = Generator::None;
    const FxObject* generator_params = nullptr;
    std::shared_ptr<const MotionGeneratorParams> motion;
    float motion_offset_db = 0.0f;
    int loop_count = 1;
    if (clip) {
        media = std::move(clip);
    } else if (info->object->type == HircType::Sound) {
        const auto& sound = *static_cast<const SoundObject*>(info->object);
        const uint32_t plugin = sound.source.plugin_id;
        if (plugin == codec::Pcm || plugin == codec::Adpcm || plugin == codec::Vorbis) {
            media = banks_->FindMedia(sound.source.source_id);
            if (!media) {
                if (warned_.insert(sound.source.source_id).second) {
                    LogWarn("audio: media {} of sound {} not loaded", sound.source.source_id, sound.id);
                }
                return nullptr;
            }
        } else if (plugin == codec::External) {
            media = ctx.external;
            if (!media) {
                if (warned_.insert(sound.id).second) {
                    LogWarn("audio: sound {} needs an external source", sound.id);
                }
                return nullptr;
            }
        } else if (plugin == codec::Silence || plugin == codec::ToneGenerator) {
            const HircObject* fx = banks_->Find(sound.source.source_id);
            generator = plugin == codec::Silence ? Generator::Silence : Generator::Tone;
            if (fx && (fx->type == HircType::FxCustom || fx->type == HircType::FxShareSet)) {
                generator_params = static_cast<const FxObject*>(fx);
            }
        } else {
            if (warned_.insert(sound.id).second) {
                LogWarn("audio: sound {} source plugin 0x{:08X} not supported", sound.id, plugin);
            }
            return nullptr;
        }
        if (const float* loop = sound.node.props.Find(prop::Loop)) {
            loop_count = static_cast<int>(*loop);
        }
    } else if (info->object->type == HircType::FeedbackNode) {
        const auto& node = *static_cast<const FeedbackNodeObject*>(info->object);
        for (const FeedbackSource& source : node.sources) {
            if (source.company_id == 0 && source.device_id == kRumbleDevice && source.source.plugin_id == codec::MotionGenerator) {
                motion = MotionParams(source.source.source_id);
                motion_offset_db = source.volume_offset;
                break;
            }
        }
        if (!motion) {
            if (warned_.insert(node.id).second) {
                LogWarn("audio: motion node {} has no rumble motion generator", node.id);
            }
            return nullptr;
        }
        generator = Generator::Motion;
        if (const float* loop = node.node.props.Find(prop::Loop)) {
            loop_count = static_cast<int>(*loop);
        }
    }
    if (!AdmitVoice(info, ctx.object, info->priority)) {
        return nullptr;
    }
    auto voice = std::make_unique<Voice>();
    Voice& v = *voice;
    v.serial = serial_counter_++;
    v.playing_id = ctx.playing_id;
    v.object = ctx.object;
    v.node = info;
    v.bus_id = info->bus_id;
    v.layers = ctx.layers;
    v.generator = generator;
    v.generator_params = generator_params;
    v.start_time = ctx.start_time;
    v.random_volume = ctx.random_volume;
    v.random_pitch = ctx.random_pitch;
    v.random_lpf = ctx.random_lpf;
    for (const NodeInfo* n = info->parent; n; n = n->parent) {
        if (n->object->type != HircType::ActorMixer) {
            continue;
        }
        for (const auto& range : n->base->ranged_props.values) {
            const float value = RandomRange(range.min, range.max);
            switch (range.id) {
                case prop::Volume: v.random_volume += value; break;
                case prop::Pitch: v.random_pitch += value; break;
                case prop::Lpf: v.random_lpf += value; break;
                case prop::InitialDelay: v.start_time += MsToSamples((value + n->base->props.Get(prop::InitialDelay)) * 1000.0f); break;
                default: break;
            }
        }
    }
    if (media) {
        v.media = media;
        v.reader = media->OpenReader();
        v.channels = std::min<uint32_t>(media->Channels(), 8);
        v.source_total = media->Frames();
        DownmixCoefficients(media->Channels(), media->Info().channel_mask, v.mix_l, v.mix_r);
        v.markers = media->Info().markers;
        v.scratch.resize(static_cast<size_t>(kBlockFrames) * media->Channels());
        if (clip_end > clip_begin) {
            SetupSourceRegion(v, clip_begin, clip_end, 1);
        } else {
            SetupSourceRegion(v, 0, v.source_total, loop_count);
        }
    } else if (generator == Generator::Silence) {
        float duration = 1.0f;
        if (generator_params && generator_params->params.size() >= 12) {
            float values[3];
            std::memcpy(values, generator_params->params.data(), 12);
            duration = std::max(0.0f, values[0] + RandomRange(values[1], values[2]));
        }
        v.generator_frames = MsToSamples(duration * 1000.0f);
        v.channels = 1;
        v.repeats_left = loop_count == 0 ? -1 : loop_count - 1;
    } else if (generator == Generator::Tone) {
        const ToneParams tone = generator_params ? ReadToneParams(generator_params->params) : ToneParams();
        v.generator_frames = MsToSamples(ToneDuration(tone) * 1000.0f);
        v.channels = 1;
        v.repeats_left = loop_count == 0 ? -1 : loop_count - 1;
        v.noise_state = serial_counter_ * 747796405u + 2891336453u;
    } else if (generator == Generator::Motion) {
        v.motion = std::move(motion);
        v.generator_frames = MsToSamples(v.motion->Duration() * 1000.0f);
        v.channels = 1;
        v.repeats_left = loop_count == 0 ? -1 : loop_count - 1;
        v.random_volume += motion_offset_db;
        v.bus_id = 0;
    }
    if (generator != Generator::None) {
        DownmixCoefficients(1, 0x4, v.mix_l, v.mix_r);
    }
    v.play_fade.Jump(1.0f);
    if (Ramp::Samples(ctx.fade_in_ms) > 0) {
        v.play_fade.Jump(0.0f);
        v.play_fade.Transition(1.0f, v.start_time, ctx.fade_in_ms, ctx.fade_curve, false, true);
    }
    v.stop_fade.Jump(1.0f);
    v.pause_fade.Jump(1.0f);
    v.rs_l.assign(kBlockFrames * 4 + 8, 0.0f);
    v.rs_r.assign(kBlockFrames * 4 + 8, 0.0f);
    v.rs_count = 1;
    v.rs_pos = 1.0;
    // the limiter's side chain: a spatialized mono voice goes through its speaker gains (UpdateVoiceParams), a voice with more
    // channels than a plain stereo pair keeps them apart, 2D each on its own speaker, spatialized through its speaker gains
    {
        const bool spatial = info->positioning && info->positioning->has_3d && info->positioning->spatialized;
        uint32_t mask = v.media ? v.media->Info().channel_mask : 0x4u;
        if (v.media && std::popcount(mask) != static_cast<int>(v.media->Channels())) {
            mask = DefaultChannelMask(v.media->Channels());
        }
        if (v.channels == 1 || generator != Generator::None) {
            v.sc_mode = spatial ? 1 : 0;
            if (!spatial && mask == 0x8) {
                v.sc_mode = 1;
                v.sc_matrix[0][7] = 1.0f;
            }
        } else if (spatial || mask != 0x3) {
            v.sc_mode = 2;
            v.sc_split = v.channels;
            for (uint32_t c = 0; c < v.sc_split; ++c) {
                v.sc_rs[c].assign(kBlockFrames * 4 + 8, 0.0f);
            }
            uint32_t c = 0;
            for (uint32_t bit = 0; bit < 18 && c < v.sc_split; ++bit) {
                if (!(mask & (1u << bit))) {
                    continue;
                }
                if (const int slot = SpeakerSlot(1u << bit); slot >= 0) {
                    v.sc_matrix[c][slot] = 1.0f;
                } else if ((1u << bit) == 0x8) {
                    v.sc_matrix[c][7] = 1.0f;
                } else {
                    v.sc_matrix[c][kFl] = v.mix_l[c];
                    v.sc_matrix[c][kFr] = v.mix_r[c];
                }
                ++c;
            }
        }
    }
    if (const Positioning* pos = info->positioning; pos && pos->UserDefined3d() && !pos->path_items.empty()) {
        const uint32_t count = static_cast<uint32_t>(pos->path_items.size());
        const bool random = pos->path_mode == PathMode::StepRandom || pos->path_mode == PathMode::ContinuousRandom ||
                            pos->path_mode == PathMode::StepRandomPickNewPath;
        const uint32_t owner = info->positioning_owner_id;
        v.path_index = random ? static_cast<uint32_t>(Random01() * count) % count : path_sequence_[owner]++ % count;
        if (v.path_index < pos->path_ranges.size()) {
            const auto& range = pos->path_ranges[v.path_index];
            v.path_offset = glm::vec2((Random01() - 0.5f) * range[0], (Random01() - 0.5f) * range[1]);
        }
    }
    v.sequencer = ctx.sequencer;
    v.item_token = ctx.item_token;
    if (Sequencer* seq = FindSequencer(ctx.sequencer)) {
        ++seq->items[ctx.item_token];
    }
    AddRef(ctx.playing_id);
    ++created_this_block_;
    if (trace_) {
        LogDebug("audio {:.4f}: voice {} node {} media {} start {:.4f} bus {} seq {}/{}", now_ / 48000.0, v.serial, info->id,
                 v.media ? v.media->Id() : 0, v.start_time / 48000.0, v.bus_id, v.sequencer, v.item_token);
    }
    Voice* raw = voice.get();
    voices_.push_back(std::move(voice));
    return raw;
}

void SoundEngine::SetupSourceRegion(Voice& v, uint64_t begin, uint64_t end, int loop_count) {
    v.range_begin = std::min(begin, v.source_total);
    v.range_end = std::clamp(end, v.range_begin, v.source_total);
    v.repeats_left = loop_count == 0 ? -1 : std::max(loop_count - 1, 0);
    v.loop_points = false;
    if (v.media && v.media->Info().looping && loop_count != 1) {
        v.loop_start = std::clamp<uint64_t>(v.media->Info().loop_start, v.range_begin, v.range_end);
        v.loop_end = std::clamp<uint64_t>(v.media->Info().loop_end, v.loop_start, v.range_end);
        v.loop_points = v.loop_end > v.loop_start;
    }
    v.source_pos = v.range_begin;
    if (v.reader && v.range_begin > 0) {
        v.reader->Seek(v.range_begin);
    }
}

void SoundEngine::RollNodeRandom(const NodeBase& base, PlayContext& ctx) {
    float delay = base.props.Get(prop::InitialDelay);
    for (const auto& range : base.ranged_props.values) {
        const float value = RandomRange(range.min, range.max);
        switch (range.id) {
            case prop::Volume: ctx.random_volume += value; break;
            case prop::Pitch: ctx.random_pitch += value; break;
            case prop::Lpf: ctx.random_lpf += value; break;
            case prop::InitialDelay: delay += value; break;
            default: break;
        }
    }
    if (delay > 0.0f) {
        ctx.start_time += MsToSamples(delay * 1000.0f);
    }
}

void SoundEngine::FinishVoice(Voice& v, uint64_t time) {
    if (!v.finished) {
        v.finished = true;
        v.finish_time = std::max(time, v.start_time);
    }
}

uint64_t SoundEngine::RemainingOutputFrames(const Voice& v) const {
    double frames = 0.0;
    if (v.generator != Generator::None) {
        frames = static_cast<double>(v.generator_frames - std::min(v.generator_pos, v.generator_frames));
    } else if (v.reader) {
        const uint64_t pass_end = (v.repeats_left != 0 && v.loop_points) ? v.loop_end : v.range_end;
        frames = static_cast<double>(pass_end > v.source_pos ? pass_end - v.source_pos : 0);
        if (frames <= 0.0) {
            frames = static_cast<double>(v.range_end - v.range_begin);
        }
    }
    return static_cast<uint64_t>(frames / std::max(v.pitch_ratio, 0.01));
}

void SoundEngine::NotifyVoiceFinished(Voice* v) {
    if (v->notified) {
        return;
    }
    v->notified = true;
    if (trace_) {
        LogDebug("audio {:.4f}: voice {} finished at {:.4f}", now_ / 48000.0, v->serial, v->finish_time / 48000.0);
    }
    if (v->sequencer) {
        SequencerItemDone(v->sequencer, v->item_token, v->finish_time);
    }
    if (v->music) {
        if (MusicPlayer* player = FindMusic(v->music)) {
            --player->live_voices;
            MusicCheckDone(*player);
        }
    }
    Release(v->playing_id);
}

SoundEngine::Sequencer* SoundEngine::FindSequencer(uint32_t serial) {
    if (!serial) {
        return nullptr;
    }
    for (auto& s : sequencers_) {
        if (s->serial == serial) {
            return s.get();
        }
    }
    return nullptr;
}

void SoundEngine::ScheduleTimer(uint64_t time, bool music, uint32_t serial) {
    timers_.push_back({time, order_counter_++, music, serial});
}

void SoundEngine::SequencerStartItem(Sequencer& seq, uint64_t time) {
    seq.item_scheduled = false;
    if (seq.finished || seq.breaking) {
        SequencerCheckDone(seq);
        return;
    }
    const RanSeqObject& container = *seq.container;
    if (seq.items_left_in_pass == 0) {
        if (seq.loops_left > 0) {
            --seq.loops_left;
        }
        if (seq.loops_left == 0 || container.playlist.empty()) {
            seq.finished = true;
            SequencerCheckDone(seq);
            return;
        }
        seq.items_left_in_pass = container.playlist.size();
        if (container.sequence && !container.restart_backward) {
            seq.playlist.sequence_index = 0;
        }
    }
    const size_t index = !seq.forced.empty() ? seq.forced[std::min(seq.forced_next++, seq.forced.size() - 1)]
                                             : PickPlaylistIndex(container, seq.playlist, true);
    if (index == SIZE_MAX) {
        seq.finished = true;
        SequencerCheckDone(seq);
        return;
    }
    --seq.items_left_in_pass;
    if (trace_) {
        LogDebug("audio {:.4f}: sequencer {} item {} ({}) at {:.4f}, {} left in pass, loops {}", now_ / 48000.0, seq.serial, index,
                 container.playlist[index].id, time / 48000.0, seq.items_left_in_pass, seq.loops_left);
    }
    const uint32_t token = seq.next_token++;
    seq.items[token] = 0;
    seq.item_starts[token] = time;
    PlayContext ctx;
    ctx.playing_id = seq.playing_id;
    ctx.object = seq.object;
    ctx.start_time = time;
    if (seq.first_item) {
        ctx.fade_in_ms = seq.first_fade_ms;
        ctx.fade_curve = seq.first_fade_curve;
    }
    seq.first_item = false;
    ctx.sequencer = seq.serial;
    ctx.item_token = token;
    ctx.layers = seq.layers;
    ctx.external = seq.external;
    ctx.random_volume = seq.random_volume;
    ctx.random_pitch = seq.random_pitch;
    ctx.random_lpf = seq.random_lpf;
    const uint32_t serial = seq.serial;
    PlayNode(container.playlist[index].id, ctx);
    Sequencer* current = FindSequencer(serial);
    if (!current) {
        return;
    }
    const float delay = container.transition_time + RandomRange(container.transition_mod_min, container.transition_mod_max);
    if (container.transition_mode == TransitionMode::TriggerRate) {
        current->item_scheduled = true;
        current->scheduled_time = time + std::max<uint64_t>(MsToSamples(delay), 1);
        ScheduleTimer(current->scheduled_time, false, serial);
    }
    auto it = current->items.find(token);
    if (it != current->items.end() && it->second == 0) {
        current->items.erase(it);
        current->item_starts.erase(token);
        if (++current->empty_items > static_cast<int>(container.playlist.size()) * 2 + 4) {
            current->finished = true;
            SequencerCheckDone(*current);
            return;
        }
        if (container.transition_mode != TransitionMode::TriggerRate) {
            const uint64_t next = time + std::max(container.transition_mode == TransitionMode::Delay ? MsToSamples(delay) : 0, MsToSamples(100.0));
            current->item_scheduled = true;
            current->scheduled_time = next;
            ScheduleTimer(next, false, serial);
        }
    } else {
        current->empty_items = 0;
    }
}

void SoundEngine::SequencerItemDone(uint32_t serial, uint32_t token, uint64_t time) {
    Sequencer* seq = FindSequencer(serial);
    if (!seq) {
        return;
    }
    auto it = seq->items.find(token);
    if (it == seq->items.end()) {
        return;
    }
    if (--it->second > 0) {
        return;
    }
    seq->items.erase(it);
    const RanSeqObject& container = *seq->container;
    uint64_t item_start = time;
    if (auto start = seq->item_starts.find(token); start != seq->item_starts.end()) {
        item_start = start->second;
        seq->item_starts.erase(start);
    }
    if (seq->finished || seq->breaking || container.transition_mode == TransitionMode::TriggerRate) {
        SequencerCheckDone(*seq, time);
        return;
    }
    if (!seq->items.empty() || seq->item_scheduled) {
        return;
    }
    float delay = 0.0f;
    if (container.transition_mode == TransitionMode::Delay) {
        delay = container.transition_time + RandomRange(container.transition_mod_min, container.transition_mod_max);
    } else if (container.transition_mode == TransitionMode::Disabled && time > item_start) {
        // without a transition the next item waits for the end of the eboot audio frame the item ended in, counted from the
        // item's start: in audio_sd_f040 each cry_s01 item starts ceil(length / 1024) * 1024 samples after the previous one,
        // while a Delay transition (cry_l01) counts its delay from the exact end
        time = item_start + (time - item_start + kEbootFrame - 1) / kEbootFrame * kEbootFrame;
    }
    const uint64_t next = std::max(time + MsToSamples(delay), now_);
    if (next < block_end_) {
        SequencerStartItem(*seq, next);
    } else {
        seq->item_scheduled = true;
        seq->scheduled_time = next;
        ScheduleTimer(next, false, serial);
    }
}

void SoundEngine::SequencerCheckDone(Sequencer& seq, uint64_t time) {
    if (seq.released || !seq.items.empty()) {
        return;
    }
    if (!(seq.finished || seq.breaking)) {
        return;
    }
    seq.released = true;
    seq.finished = true;
    const uint32_t parent = seq.parent;
    const uint32_t token = seq.parent_token;
    const PlayingId id = seq.playing_id;
    if (parent) {
        SequencerItemDone(parent, token, std::max(time, now_));
    }
    Release(id);
}

SoundEngine::MusicPlayer* SoundEngine::FindMusic(uint32_t serial) {
    if (!serial) {
        return nullptr;
    }
    for (auto& m : music_) {
        if (m->serial == serial) {
            return m.get();
        }
    }
    return nullptr;
}

void SoundEngine::PlayMusic(NodeInfo* info, const PlayContext& ctx) {
    auto player = std::make_unique<MusicPlayer>();
    player->serial = serial_counter_++;
    player->playing_id = ctx.playing_id;
    player->object = ctx.object;
    player->node = info;
    player->layers = ctx.layers;
    player->fade_in_ms = ctx.fade_in_ms;
    player->fade_curve = ctx.fade_curve;
    uint32_t first = 0;
    if (info->object->type == HircType::MusicRanSeqCntr) {
        player->ranseq = static_cast<const MusicRanSeqObject*>(info->object);
        PlaylistFrame frame;
        frame.node = &player->ranseq->playlist;
        for (size_t i = 0; i < frame.node->children.size(); ++i) {
            frame.order.push_back(i);
        }
        player->stack.push_back(std::move(frame));
        first = MusicNextSegment(*player);
    } else {
        first = info->id;
    }
    if (!first) {
        return;
    }
    AddRef(ctx.playing_id);
    MusicPlayer* raw = player.get();
    music_.push_back(std::move(player));
    MusicStartSegment(*raw, first, ctx.start_time);
}

uint32_t SoundEngine::MusicNextSegment(MusicPlayer& player) {
    for (int guard = 0; guard < 4096 && !player.stack.empty(); ++guard) {
        PlaylistFrame& frame = player.stack.back();
        const MusicPlaylistNode* node = frame.node;
        if (node->children.empty()) {
            if (node->segment_id == 0) {
                player.stack.pop_back();
                continue;
            }
            if (node->loop == 0 || frame.loops_done < node->loop) {
                ++frame.loops_done;
                return node->segment_id;
            }
            player.stack.pop_back();
            continue;
        }
        const bool step = node->type == MusicPlaylistType::StepSequence || node->type == MusicPlaylistType::StepRandom;
        const bool random = node->type == MusicPlaylistType::ContinuousRandom || node->type == MusicPlaylistType::StepRandom;
        if (frame.child_pos < frame.order.size() && !(step && frame.child_pos > 0 && frame.loops_done == 0 && false)) {
            if (frame.child_pos == 0 && random) {
                std::shuffle(frame.order.begin(), frame.order.end(), rng_);
            }
            const size_t child = frame.order[frame.child_pos++];
            PlaylistFrame next;
            next.node = &node->children[child];
            for (size_t i = 0; i < next.node->children.size(); ++i) {
                next.order.push_back(i);
            }
            if (step) {
                frame.child_pos = frame.order.size();
            }
            player.stack.push_back(std::move(next));
            continue;
        }
        ++frame.loops_done;
        if (node->loop == 0 || frame.loops_done < node->loop) {
            frame.child_pos = 0;
            continue;
        }
        player.stack.pop_back();
    }
    return 0;
}

void SoundEngine::MusicStartSegment(MusicPlayer& player, uint32_t segment_id, uint64_t time) {
    player.has_next = false;
    const HircObject* found = banks_->Find(segment_id);
    if (!found || found->type != HircType::MusicSegment || player.stopping) {
        MusicCheckDone(player);
        return;
    }
    const auto& segment = *static_cast<const MusicSegmentObject*>(found);
    player.current_segment = segment_id;
    double entry = 0.0;
    double exit = segment.duration_ms;
    if (!segment.markers.empty()) {
        entry = segment.markers.front().position_ms;
        exit = segment.markers.front().position_ms;
        for (const auto& marker : segment.markers) {
            entry = std::min(entry, marker.position_ms);
            exit = std::max(exit, marker.position_ms);
        }
    }
    const uint32_t serial = player.serial;
    for (uint32_t track_id : segment.music.children) {
        NodeInfo* track_info = Info(track_id);
        if (!track_info || track_info->object->type != HircType::MusicTrack) {
            continue;
        }
        const auto& track = *static_cast<const MusicTrackObject*>(track_info->object);
        uint32_t sub_track = 0;
        if (track.track_type == 1 && track.sub_track_count > 1) {
            sub_track = static_cast<uint32_t>(Random01() * track.sub_track_count) % track.sub_track_count;
        } else if (track.track_type == 2 && track.sub_track_count > 1) {
            sub_track = player.sequence_tracks[track_id]++ % track.sub_track_count;
        }
        for (const auto& clip : track.clips) {
            if (track.track_type != 0 && clip.track != sub_track) {
                continue;
            }
            auto media = banks_->FindMedia(clip.source_id);
            if (!media) {
                if (warned_.insert(clip.source_id).second) {
                    LogWarn("audio: music media {} not loaded", clip.source_id);
                }
                continue;
            }
            const double clip_start = clip.play_at_ms + clip.begin_trim_ms;
            const double clip_end = clip.play_at_ms + clip.source_duration_ms + clip.end_trim_ms;
            const double from = std::max(clip_start, entry);
            if (clip_end <= from + 0.01) {
                continue;
            }
            PlayContext ctx;
            ctx.playing_id = player.playing_id;
            ctx.object = player.object;
            ctx.start_time = time + MsToSamples(from - entry);
            ctx.layers = player.layers;
            if (player.first) {
                ctx.fade_in_ms = player.fade_in_ms;
                ctx.fade_curve = player.fade_curve;
            }
            Voice* v = CreateVoice(track_info, ctx, media, MsToSamples(from - clip.play_at_ms), MsToSamples(clip_end - clip.play_at_ms));
            if (!v) {
                continue;
            }
            v->music = serial;
            ++player.live_voices;
        }
    }
    player.first = false;
    if (trace_) {
        LogDebug("audio {:.4f}: music {} segment {} at {:.6f}, entry {} ms, exit {} ms, {} voices", now_ / 48000.0, player.serial,
                 segment_id, time / 48000.0, entry, exit, player.live_voices);
    }
    player.next_segment = player.ranseq ? MusicNextSegment(player) : 0;
    player.has_next = player.next_segment != 0;
    if (player.has_next) {
        player.next_time = time + MsToSamples(exit - entry);
        ScheduleTimer(player.next_time, true, serial);
    }
    MusicCheckDone(player);
}

void SoundEngine::MusicCheckDone(MusicPlayer& player) {
    if (player.released || player.live_voices > 0 || (player.has_next && !player.stopping)) {
        return;
    }
    player.released = true;
    Release(player.playing_id);
}

bool SoundEngine::ChainContains(const NodeInfo* node, uint32_t id) const {
    for (int depth = 0; node && depth < 64; node = node->parent, ++depth) {
        if (node->id == id) {
            return true;
        }
    }
    return false;
}

bool SoundEngine::BusChainContains(uint32_t bus, uint32_t id) const {
    auto it = buses_.find(bus);
    for (const BusRuntime* b = it == buses_.end() ? nullptr : it->second.get(); b; b = b->parent) {
        if (b->id == id) {
            return true;
        }
    }
    return false;
}

bool SoundEngine::ActionMatches(const ActionObject& action, GameObjectId object, const NodeInfo* node, uint32_t bus,
                                GameObjectId event_object) const {
    const uint8_t scope = action.Scope();
    if ((scope & 1) && object != event_object) {
        return false;
    }
    switch (scope & 0xFE) {
        case 0x04:
            return true;
        case 0x08:
            for (const auto& e : action.exceptions) {
                if (e.is_bus ? BusChainContains(bus, e.id) : ChainContains(node, e.id)) {
                    return false;
                }
            }
            return true;
        default:
            return action.target_is_bus ? BusChainContains(bus, action.target_id) : ChainContains(node, action.target_id);
    }
}

void SoundEngine::StopVoice(Voice& v, uint64_t time, float fade_ms, Interp curve) {
    if (v.finished) {
        return;
    }
    const uint64_t length = Ramp::Samples(fade_ms);
    if (v.start_time > time || length == 0 || v.paused) {
        FinishVoice(v, std::max(time, now_));
        return;
    }
    if (v.stopping && v.stop_fade.start + v.stop_fade.length <= time + length) {
        return;
    }
    v.stopping = true;
    v.stop_fade.Transition(0.0f, time, fade_ms, curve, false, true);
}

void SoundEngine::StopMatching(const ActionObject& action, GameObjectId object, uint64_t time, float fade_ms, Interp curve) {
    for (auto& v : voices_) {
        if (!v->finished && v->node && ActionMatches(action, v->object, v->node, v->bus_id, object)) {
            StopVoice(*v, time, fade_ms, curve);
        }
    }
    for (auto& seq : sequencers_) {
        if (!seq->finished && ActionMatches(action, seq->object, seq->node, seq->node->bus_id, object)) {
            seq->finished = true;
        }
    }
    for (auto& player : music_) {
        if (!player->stopping && ActionMatches(action, player->object, player->node, player->node->bus_id, object)) {
            player->stopping = true;
            player->has_next = false;
        }
    }
    for (auto& pending : pending_) {
        if (pending.cancelled || (pending.action->Category() != 0x04 && pending.action->Category() != 0x05)) {
            continue;
        }
        const NodeInfo* target = Info(pending.action->target_id);
        if (target && ActionMatches(action, pending.object, target, target->bus_id, object)) {
            pending.cancelled = true;
        }
    }
    for (auto& seq : sequencers_) {
        SequencerCheckDone(*seq);
    }
    for (auto& player : music_) {
        MusicCheckDone(*player);
    }
}

void SoundEngine::PauseMatching(const ActionObject& action, GameObjectId object, uint64_t time, float fade_ms, Interp curve, bool pause) {
    const uint64_t length = Ramp::Samples(fade_ms);
    for (auto& v : voices_) {
        if (v->finished || !v->node || !ActionMatches(action, v->object, v->node, v->bus_id, object)) {
            continue;
        }
        if (pause) {
            if (v->paused) {
                continue;
            }
            if (!v->pausing) v->paused_at = time;
            if (!v->started || length == 0) {
                v->paused = true;
                v->pausing = false;
                v->pause_fade.Jump(0.0f);
            } else {
                v->pausing = true;
                v->pause_fade.Transition(0.0f, time, fade_ms, curve, false, true);
            }
        } else if (v->paused || v->pausing) {
            // a voice scheduled to start later (a music segment's clip, a container item) keeps its distance from the pause
            if (!v->started && v->start_time > v->paused_at && time > v->paused_at) v->start_time += time - v->paused_at;
            v->paused = false;
            v->pausing = false;
            v->pause_fade.Transition(1.0f, time, fade_ms, curve, false, true);
        }
    }
    // The schedules of music playlists and containers stop with their voices: without this a playlist paused by Pause_All (the
    // option screen) started its next segment unpaused when its timer came, and the f110 music came back a few seconds into the menu
    for (auto& player : music_) {
        if (player->released || !player->node || !ActionMatches(action, player->object, player->node, player->node->bus_id, object)) {
            continue;
        }
        if (pause && !player->paused) {
            player->paused = true;
            player->paused_at = time;
        } else if (!pause && player->paused) {
            player->paused = false;
            if (player->has_next && !player->stopping && time > player->paused_at) {
                player->next_time += time - player->paused_at;
                ScheduleTimer(player->next_time, true, player->serial);
            }
        }
    }
    for (auto& seq : sequencers_) {
        if (seq->finished || !seq->node || !ActionMatches(action, seq->object, seq->node, seq->node->bus_id, object)) {
            continue;
        }
        if (pause && !seq->paused) {
            seq->paused = true;
            seq->paused_at = time;
        } else if (!pause && seq->paused) {
            seq->paused = false;
            if (seq->item_scheduled && time > seq->paused_at) {
                seq->scheduled_time += time - seq->paused_at;
                ScheduleTimer(seq->scheduled_time, false, seq->serial);
            }
        }
    }
}

void SoundEngine::BreakMatching(const ActionObject& action, GameObjectId object) {
    for (auto& seq : sequencers_) {
        if (!seq->finished && ActionMatches(action, seq->object, seq->node, seq->node->bus_id, object)) {
            seq->breaking = true;
            SequencerCheckDone(*seq);
        }
    }
    for (auto& v : voices_) {
        if (!v->finished && v->node && ActionMatches(action, v->object, v->node, v->bus_id, object)) {
            v->repeats_left = 0;
        }
    }
}

void SoundEngine::SeekMatching(const ActionObject& action, GameObjectId object) {
    const float value = action.seek_value[0] + RandomRange(action.seek_value[1], action.seek_value[2]);
    for (auto& v : voices_) {
        if (v->finished || !v->reader || !v->node || !ActionMatches(action, v->object, v->node, v->bus_id, object)) {
            continue;
        }
        const uint64_t length = v->range_end - v->range_begin;
        uint64_t offset = action.seek_relative_to_duration ? static_cast<uint64_t>(std::clamp(value, 0.0f, 1.0f) * length)
                                                           : MsToSamples(std::max(value, 0.0f));
        uint64_t target = v->range_begin + offset;
        if (v->loop_points && v->repeats_left != 0 && target >= v->loop_end) {
            target = v->loop_start + (target - v->loop_start) % (v->loop_end - v->loop_start);
        }
        target = std::min(target, v->range_end);
        v->source_pos = target;
        v->reader->Seek(target);
        v->rs_count = 1;
        v->rs_pos = 1.0;
        v->source_done = false;
    }
}

// the source position of every voice of a playing id, in seconds of its media
void SoundEngine::SeekPlayingInternal(PlayingId id, float seconds) {
    for (auto& v : voices_) {
        if (v->finished || !v->reader || !v->media || v->playing_id != id) {
            continue;
        }
        const uint64_t offset = static_cast<uint64_t>(std::llround(std::max(seconds, 0.0f) * static_cast<double>(v->media->SampleRate())));
        const uint64_t target = std::min(v->range_begin + offset, v->range_end);
        v->source_pos = target;
        v->reader->Seek(target);
        v->rs_count = 1;
        v->rs_pos = 1.0;
        v->source_done = false;
    }
}

void SoundEngine::SetAkProp(const ActionObject& action, GameObjectId object, uint64_t time) {
    const uint8_t category = action.Category();
    const uint8_t scope = action.Scope();
    float fade = action.props.Get(prop::TransitionTime);
    if (const auto* range = action.ranged_props.Find(prop::TransitionTime)) {
        fade += RandomRange(range->min, range->max);
    }
    bool reset = category == 0x09 || category == 0x0B || category == 0x0D || category == 0x0F || category == 0x07;
    float value = action.value[0] + RandomRange(action.value[1], action.value[2]);
    if (category == 0x06) {
        value = -96.3f;
    }
    auto pick = [&](ActionProps& props) -> Ramp& {
        switch (category) {
            case 0x08:
            case 0x09: return props.pitch;
            case 0x0C:
            case 0x0D: return props.bus_volume;
            case 0x0E:
            case 0x0F: return props.lpf;
            default: return props.volume;
        }
    };
    auto apply = [&](Ramp& ramp) {
        const bool db = category != 0x08 && category != 0x09 && category != 0x0E && category != 0x0F;
        if (reset) {
            ramp.Transition(0.0f, time, fade, action.fade_curve, db, true);
        } else if (action.value_meaning == ValueMeaning::Offset) {
            ramp.Transition(ramp.to + value, time, fade, action.fade_curve, db, true);
        } else {
            ramp.Transition(value, time, fade, action.fade_curve, db, true);
        }
    };
    if ((scope & 0xFE) == 0x04 || (scope & 0xFE) == 0x08) {
        for (auto& [id, list] : action_props_) {
            for (auto& props : list) {
                if (!(scope & 1) || props.object == object) {
                    apply(pick(props));
                }
            }
        }
        return;
    }
    const bool all_objects = !(scope & 1);
    auto& list = action_props_[action.target_id];
    ActionProps* entry = nullptr;
    for (auto& props : list) {
        if (props.all_objects == all_objects && (all_objects || props.object == object)) {
            entry = &props;
        }
    }
    if (!entry) {
        list.push_back({});
        entry = &list.back();
        entry->object = object;
        entry->all_objects = all_objects;
    }
    apply(pick(*entry));
}

void SoundEngine::SetGameParameter(const ActionObject& action, GameObjectId object, uint64_t time) {
    const uint32_t parameter = action.target_id;
    const bool reset = action.Category() == 0x14;
    const bool object_scope = (action.Scope() & 1) != 0;
    float fade = action.props.Get(prop::TransitionTime);
    if (const auto* range = action.ranged_props.Find(prop::TransitionTime)) {
        fade += RandomRange(range->min, range->max);
    }
    auto default_it = rtpc_defaults_.find(parameter);
    const float default_value = default_it == rtpc_defaults_.end() ? 0.0f : default_it->second;
    Ramp* ramp = nullptr;
    if (object_scope && object != 0) {
        ObjectState& state = Object(object);
        auto it = state.rtpcs.find(parameter);
        if (it == state.rtpcs.end()) {
            Ramp initial;
            initial.Jump(RtpcValue(parameter, 0));
            it = state.rtpcs.emplace(parameter, initial).first;
        }
        ramp = &it->second;
    } else {
        auto it = global_rtpc_.find(parameter);
        if (it == global_rtpc_.end()) {
            Ramp initial;
            initial.Jump(default_value);
            it = global_rtpc_.emplace(parameter, initial).first;
        }
        ramp = &it->second;
    }
    const float value = action.value[0] + RandomRange(action.value[1], action.value[2]);
    float target = value;
    if (reset) {
        target = default_value;
    } else if (action.value_meaning == ValueMeaning::Offset) {
        target = ramp->to + value;
    }
    // 0x58A260: a game parameter moves without the dB conversion and without the reversed curve
    ramp->Transition(target, time, fade, action.fade_curve, false, false);
    static const bool trace = std::getenv("PT_TRACE_RTPC") != nullptr;
    if (trace) {
        LogInfo("rtpc trace: action type {:#x} parameter {} {} object {} target {:.3f} fade {:.3f}", action.action_type, parameter,
                object_scope && object != 0 ? "object" : "global", object, target, fade);
    }
}

void SoundEngine::SetStateInternal(uint32_t group, uint32_t state, uint64_t time) {
    auto current = states_.find(group);
    const uint32_t previous = current == states_.end() ? 0 : current->second;
    if (current != states_.end() && previous == state) {
        return;
    }
    states_[group] = state;
    uint32_t transition_ms = 0;
    auto settings = state_groups_.find(group);
    if (settings != state_groups_.end()) {
        transition_ms = settings->second->TransitionTime(previous, state);
    }
    auto slots = slots_by_group_.find(group);
    if (slots == slots_by_group_.end()) {
        return;
    }
    for (size_t index : slots->second) {
        StateSlot& slot = state_slots_[index];
        const StateObject* instance = nullptr;
        for (const auto& entry : slot.binding->states) {
            if (entry.state_id == state) {
                const HircObject* found = banks_->Find(entry.instance_id);
                if (found && found->type == HircType::State) {
                    instance = static_cast<const StateObject*>(found);
                }
            }
        }
        for (int p = 0; p < 6; ++p) {
            const float* value = instance ? instance->Find(kStateParams[p]) : nullptr;
            // 0x578AD0: volume properties (the bit mask 0x304FC0113) move as dB transitions
            slot.values[p].Transition(value ? *value : 0.0f, time, static_cast<float>(transition_ms), Interp::Linear, p == 0 || p >= 3, true);
        }
    }
}

void SoundEngine::StopPlayingInternal(PlayingId id, float fade_ms, Interp curve) {
    for (auto& v : voices_) {
        if (v->playing_id == id) {
            StopVoice(*v, now_, fade_ms, curve);
        }
    }
    for (auto& seq : sequencers_) {
        if (seq->playing_id == id) {
            seq->finished = true;
        }
    }
    for (auto& player : music_) {
        if (player->playing_id == id) {
            player->stopping = true;
            player->has_next = false;
        }
    }
    for (auto& pending : pending_) {
        if (pending.playing_id == id) {
            pending.cancelled = true;
        }
    }
    for (auto& seq : sequencers_) {
        SequencerCheckDone(*seq);
    }
    for (auto& player : music_) {
        MusicCheckDone(*player);
    }
}

void SoundEngine::StopAllInternal(float fade_ms) {
    for (auto& v : voices_) {
        StopVoice(*v, now_, fade_ms, Interp::Linear);
    }
    for (auto& seq : sequencers_) {
        seq->finished = true;
    }
    for (auto& player : music_) {
        player->stopping = true;
        player->has_next = false;
    }
    for (auto& pending : pending_) {
        if (pending.action->Category() == 0x04 || pending.action->Category() == 0x05) {
            pending.cancelled = true;
        }
    }
    for (auto& seq : sequencers_) {
        SequencerCheckDone(*seq);
    }
    for (auto& player : music_) {
        MusicCheckDone(*player);
    }
}

// The port's Reset Progress and loop browser start a new session in the same process; the original only ever had a boot for
// that. Whatever an event or the game set on the engine (a state group, a global or object game parameter, a switch, an
// object's aux sends or obstruction, a SetVolume/SetPitch/SetLPF action's offset, a Pause_All left without its Resume_All)
// would otherwise carry over into the new session's sounds.
void SoundEngine::ResetSessionInternal() {
    std::vector<uint32_t> groups;
    for (const auto& [group, state] : states_) {
        if (state != 0) groups.push_back(group);
    }
    for (uint32_t group : groups) {
        SetStateInternal(group, 0, now_);
    }
    states_.clear();
    global_rtpc_.clear();
    for (const auto& [id, value] : rtpc_defaults_) {
        global_rtpc_[id].Jump(value);
    }
    for (auto& [id, object] : objects_) {
        object.switches.clear();
        object.rtpcs.clear();
        object.has_aux_sends = false;
        object.aux_sends.clear();
        object.obstruction = 0.0f;
        object.occlusion = 0.0f;
    }
    action_props_.clear();
    global_pause_ = false;
    LogInfo("sound: session reset ({} state groups back to none)", groups.size());
}

void SoundEngine::StopObject(GameObjectId object) {
    for (auto& v : voices_) {
        if (v->object == object) {
            StopVoice(*v, now_, 10.0f, Interp::Linear);
        }
    }
    for (auto& seq : sequencers_) {
        if (seq->object == object) {
            seq->finished = true;
        }
    }
    for (auto& player : music_) {
        if (player->object == object) {
            player->stopping = true;
            player->has_next = false;
        }
    }
    for (auto& pending : pending_) {
        if (pending.object == object) {
            pending.cancelled = true;
        }
    }
    for (auto& seq : sequencers_) {
        SequencerCheckDone(*seq);
    }
    for (auto& player : music_) {
        MusicCheckDone(*player);
    }
}

float SoundEngine::RtpcValue(uint32_t parameter, GameObjectId object) const {
    if (object != 0) {
        auto obj = objects_.find(object);
        if (obj != objects_.end()) {
            auto it = obj->second.rtpcs.find(parameter);
            if (it != obj->second.rtpcs.end()) {
                return it->second.Value(now_);
            }
        }
    }
    auto it = global_rtpc_.find(parameter);
    if (it != global_rtpc_.end()) {
        return it->second.Value(now_);
    }
    auto def = rtpc_defaults_.find(parameter);
    return def == rtpc_defaults_.end() ? 0.0f : def->second;
}

float SoundEngine::SumRtpcs(const std::vector<RtpcBinding>& bindings, uint32_t param, GameObjectId object) const {
    float sum = 0.0f;
    for (const auto& binding : bindings) {
        if (binding.param == param) {
            sum += binding.curve.Evaluate(RtpcValue(binding.game_parameter, object));
        }
    }
    return sum;
}

void SoundEngine::ActionPropOffsets(uint32_t id, GameObjectId object, float& volume, float& pitch, float& lpf, float& bus_volume) const {
    auto it = action_props_.find(id);
    if (it == action_props_.end()) {
        return;
    }
    for (const auto& props : it->second) {
        if (props.all_objects || props.object == object) {
            volume += props.volume.Value(now_);
            pitch += props.pitch.Value(now_);
            lpf += props.lpf.Value(now_);
            bus_volume += props.bus_volume.Value(now_);
        }
    }
}

float SoundEngine::StateOffset(const std::vector<size_t>& slots, int index) const {
    float sum = 0.0f;
    for (size_t slot : slots) {
        sum += state_slots_[slot].values[index].Value(now_);
    }
    return sum;
}

glm::vec3 SoundEngine::EmitterPosition(const Voice& v, const ObjectState& object) const {
    const Positioning* pos = v.node ? v.node->positioning : nullptr;
    if (pos && pos->UserDefined3d()) {
        glm::vec3 local(0.0f);
        if (!pos->path_items.empty() && !pos->path_vertices.empty()) {
            const auto& item = pos->path_items[std::min<size_t>(v.path_index, pos->path_items.size() - 1)];
            const uint32_t first = std::min<uint32_t>(item.vertex_offset, static_cast<uint32_t>(pos->path_vertices.size() - 1));
            const uint32_t count = std::min<uint32_t>(item.vertex_count, static_cast<uint32_t>(pos->path_vertices.size()) - first);
            double total = 0.0;
            for (uint32_t i = 0; i + 1 < count; ++i) {
                total += std::max(pos->path_vertices[first + i].duration_ms, 0);
            }
            double t = now_ > v.start_time ? static_cast<double>(now_ - v.start_time) / kSamplesPerMs : 0.0;
            if (pos->path_looping && total > 0.0) {
                t = std::fmod(t, total);
            }
            const PathVertex* a = &pos->path_vertices[first];
            local = glm::vec3(a->x, a->y, a->z);
            for (uint32_t i = 0; i + 1 < count; ++i) {
                const PathVertex& p0 = pos->path_vertices[first + i];
                const PathVertex& p1 = pos->path_vertices[first + i + 1];
                const double d = std::max(p0.duration_ms, 0);
                if (t <= d && d > 0.0) {
                    const float w = static_cast<float>(t / d);
                    local = glm::mix(glm::vec3(p0.x, p0.y, p0.z), glm::vec3(p1.x, p1.y, p1.z), w);
                    break;
                }
                t -= d;
                local = glm::vec3(p1.x, p1.y, p1.z);
            }
        }
        local.x += v.path_offset.x;
        local.z += v.path_offset.y;
        return listener_position_ + listener_right_ * local.x + listener_up_ * local.y + listener_forward_ * local.z;
    }
    return object.has_transform ? object.position : listener_position_;
}

void SoundEngine::UpdateBuses() {
    for (auto& [id, bus] : buses_) {
        bus->active_voices = 0;
    }
    for (auto& v : voices_) {
        if (v->finished || !v->started || v->paused) {
            continue;
        }
        for (BusRuntime* b = Bus(v->bus_id); b; b = b->parent) {
            ++b->active_voices;
        }
    }
    for (BusRuntime* bus : bus_order_) {
        const BusObject& o = *bus->object;
        float bus_db = o.props.Get(prop::BusVolume) + StateOffset(bus->state_slots, 3) + SumRtpcs(o.rtpcs, rtpc_param::BusVolume, 0);
        float volume = 0.0f;
        float pitch = 0.0f;
        float lpf = 0.0f;
        float bus_action = 0.0f;
        ActionPropOffsets(bus->id, 0, volume, pitch, lpf, bus_action);
        bus_db += bus_action + volume + bus->duck_db;
        bus->gain_prev = bus->gain;
        // 0x59D1E0, 0x59D2C0: the bus volume in dB through the eboot's fast 10^x
        bus->gain = FastPow10(bus_db * 0.05f);
        bus->voice_volume_db = o.props.Get(prop::Volume) + StateOffset(bus->state_slots, 0) + SumRtpcs(o.rtpcs, rtpc_param::Volume, 0);
        bus->voice_pitch = o.props.Get(prop::Pitch) + pitch + StateOffset(bus->state_slots, 1) + SumRtpcs(o.rtpcs, rtpc_param::Pitch, 0);
        bus->voice_lpf = o.props.Get(prop::Lpf) + lpf + StateOffset(bus->state_slots, 2) + SumRtpcs(o.rtpcs, rtpc_param::Lpf, 0);
    }
    for (auto it = bus_order_.rbegin(); it != bus_order_.rend(); ++it) {
        BusRuntime* bus = *it;
        if (bus->parent) {
            bus->voice_volume_db += bus->parent->voice_volume_db;
            bus->voice_pitch += bus->parent->voice_pitch;
            bus->voice_lpf += bus->parent->voice_lpf;
        }
        // the limiter's side chain: the gain from this bus's input to the master's, and whether a signal gets there unchanged
        bus->chain = bus->gain * (bus->parent ? bus->parent->chain : 1.0f);
        bus->chain_prev = bus->gain_prev * (bus->parent ? bus->parent->chain_prev : 1.0f);
        bus->sc_direct = bus == master_ || (bus->effects.empty() && bus->parent && bus->parent->sc_direct);
    }
    const float block_ms = kBlockFrames / kSamplesPerMs;
    for (BusRuntime* bus : bus_order_) {
        for (const auto& duck : bus->object->ducks) {
            BusRuntime* target = Bus(duck.bus_id);
            if (!target) {
                continue;
            }
            const float depth = std::min(duck.volume, 0.0f);
            if (bus->active_voices > 0) {
                const float step = duck.fade_out_ms > 0 ? -depth * block_ms / duck.fade_out_ms : -depth;
                target->duck_db = std::max(depth, target->duck_db - step);
                bus->duck_release_at = now_ + MsToSamples(std::max(bus->object->recovery_time_ms, 0));
            } else if (now_ >= bus->duck_release_at) {
                const float step = duck.fade_in_ms > 0 ? -depth * block_ms / duck.fade_in_ms : -depth;
                target->duck_db = std::min(0.0f, target->duck_db + step);
            }
        }
    }
}

void SoundEngine::UpdateVoiceParams(Voice& v) {
    auto obj_it = objects_.find(v.object);
    static const ObjectState kEmpty;
    const ObjectState& obj = obj_it == objects_.end() ? kEmpty : obj_it->second;
    float volume = v.random_volume;
    float pitch = v.random_pitch;
    float lpf = v.random_lpf;
    float game_aux_db = 0.0f;
    float output_bus_db = 0.0f;
    float makeup = 0.0f;
    float user_aux_db[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float crossfade = 1.0f;
    for (const NodeInfo* n = v.node; n; n = n->parent) {
        const NodeBase* b = n->base;
        for (const auto& p : b->props.values) {
            switch (p.id) {
                case prop::Volume: volume += p.value; break;
                case prop::Pitch: pitch += p.value; break;
                case prop::Lpf: lpf += p.value; break;
                case prop::GameAuxSendVolume: game_aux_db += p.value; break;
                case prop::OutputBusVolume: output_bus_db += p.value; break;
                case prop::MakeUpGain: makeup += p.value; break;
                case prop::UserAuxSendVolume0:
                case prop::UserAuxSendVolume0 + 1:
                case prop::UserAuxSendVolume0 + 2:
                case prop::UserAuxSendVolume0 + 3: user_aux_db[p.id - prop::UserAuxSendVolume0] += p.value; break;
                default: break;
            }
        }
        for (const auto& rtpc : b->rtpcs) {
            const float y = rtpc.curve.Evaluate(RtpcValue(rtpc.game_parameter, v.object));
            switch (rtpc.param) {
                case rtpc_param::Volume: volume += y; break;
                case rtpc_param::Pitch: pitch += y; break;
                case rtpc_param::Lpf: lpf += y; break;
                case rtpc_param::GameAuxSendVolume: game_aux_db += y; break;
                case rtpc_param::OutputBusVolume: output_bus_db += y; break;
                case rtpc_param::MakeUpGain: makeup += y; break;
                case rtpc_param::UserAuxSendVolume0:
                case rtpc_param::UserAuxSendVolume0 + 1:
                case rtpc_param::UserAuxSendVolume0 + 2:
                case rtpc_param::UserAuxSendVolume0 + 3: user_aux_db[rtpc.param - rtpc_param::UserAuxSendVolume0] += y; break;
                default: break;
            }
        }
        float bus_action = 0.0f;
        ActionPropOffsets(n->id, v.object, volume, pitch, lpf, bus_action);
        if (!n->state_slots.empty()) {
            volume += StateOffset(n->state_slots, 0);
            pitch += StateOffset(n->state_slots, 1);
            lpf += StateOffset(n->state_slots, 2);
            output_bus_db += StateOffset(n->state_slots, 4);
            game_aux_db += StateOffset(n->state_slots, 5);
        }
    }
    for (const auto& link : v.layers) {
        for (const auto& rtpc : link.layer->rtpcs) {
            const float y = rtpc.curve.Evaluate(RtpcValue(rtpc.game_parameter, v.object));
            switch (rtpc.param) {
                case rtpc_param::Volume: volume += y; break;
                case rtpc_param::Pitch: pitch += y; break;
                case rtpc_param::Lpf: lpf += y; break;
                default: break;
            }
        }
        if (link.crossfade) {
            crossfade *= std::clamp(link.crossfade->EvaluateRaw(RtpcValue(link.layer->crossfade_rtpc_id, v.object)), 0.0f, 1.0f);
        }
    }
    const BusRuntime* bus = Bus(v.bus_id);
    const float bus_voice_db = bus ? bus->voice_volume_db : 0.0f;
    if (bus) {
        pitch += bus->voice_pitch;
        lpf += bus->voice_lpf;
    }

    float att_db = 0.0f;
    float cone_db = 0.0f;
    float aux_game_att = 0.0f;
    float aux_user_att = 0.0f;
    float lpf_floor = 0.0f;
    bool panned = false;
    bool front_custom = false;
    v.sum_scale = 1.0f;
    const Positioning* pos = v.node ? v.node->positioning : nullptr;
    if (pos && pos->has_3d) {
        const glm::vec3 emitter = EmitterPosition(v, obj);
        const glm::vec3 rel = emitter - listener_position_;
        const float distance = glm::length(rel);
        const float height = glm::dot(rel, listener_up_);
        const float horizontal = std::sqrt(std::max(distance * distance - height * height, 0.0f));
        const float azimuth = distance > 1e-4f ? std::atan2(glm::dot(rel, listener_right_), glm::dot(rel, listener_forward_)) : 0.0f;
        float spread = 0.0f;
        if (const AttenuationObject* att = v.node->attenuation) {
            if (const Curve* c = att->SlotCurve(kAttVolumeDry)) {
                att_db += c->Evaluate(distance);
            }
            if (const Curve* c = att->SlotCurve(kAttVolumeAuxGameDef)) {
                aux_game_att += c->Evaluate(distance);
            }
            if (const Curve* c = att->SlotCurve(kAttVolumeAuxUserDef)) {
                aux_user_att += c->Evaluate(distance);
            }
            if (const Curve* c = att->SlotCurve(kAttLowPass)) {
                lpf_floor = std::max(lpf_floor, c->Evaluate(distance));
            }
            if (const Curve* c = att->SlotCurve(kAttSpread)) {
                spread = std::clamp(c->Evaluate(horizontal), 0.0f, 100.0f);
            }
            if (att->cone_enabled && pos->GameDefined3d() && obj.has_transform && distance > 1e-4f) {
                const float cos_angle = glm::dot(obj.forward, -rel / distance);
                const float angle = glm::degrees(std::acos(std::clamp(cos_angle, -1.0f, 1.0f)));
                const float inside = att->cone.inside_degrees * 0.5f;
                const float outside = std::max(att->cone.outside_degrees * 0.5f, inside + 0.001f);
                const float w = std::clamp((angle - inside) / (outside - inside), 0.0f, 1.0f);
                cone_db = (att->cone.outside_volume + SumRtpcs(att->rtpcs, 0x0D, v.object)) * w;
                lpf_floor = std::max(lpf_floor, (att->cone.low_pass + SumRtpcs(att->rtpcs, 0x0E, v.object)) * w);
            }
        }
        if (pos->spatialized) {
            float gains[kSpeakers][kSpeakers];
            if (v.channels == 1 || v.generator != Generator::None) {
                SpeakerGains(azimuth, v.node->center, spread, 1, gains);
                float l = 0.0f;
                float r = 0.0f;
                float sum = 0.0f;
                float all = 0.0f;
                for (int s = 0; s < kSpeakers; ++s) {
                    l += gains[kFl][s] * kFoldLeft[s];
                    r += gains[kFl][s] * kFoldRight[s];
                    sum += s == kFc ? 0.0f : gains[kFl][s];
                    all += gains[kFl][s];
                }
                v.matrix[0] = v.matrix[1] = l / kCenterGain * 0.5f;
                v.matrix[2] = v.matrix[3] = r / kCenterGain * 0.5f;
                v.send_scale = l + r > 1e-9f ? sum / (l + r) : 1.0f;
                // the Matrix Reverb sums all seven speaker channels of its bus, the centre included
                v.sum_scale = l + r > 1e-9f ? all / (l + r) : 1.0f;
                v.front_matrix[0] = v.front_matrix[1] = gains[kFl][kFl] / kCenterGain * 0.5f;
                v.front_matrix[2] = v.front_matrix[3] = gains[kFl][kFr] / kCenterGain * 0.5f;
                front_custom = true;
                for (int s = 0; s < kSpeakers; ++s) {
                    v.sc_matrix[0][s] = gains[kFl][s];
                }
            } else {
                uint32_t mask = v.media->Info().channel_mask;
                if (std::popcount(mask) != static_cast<int>(v.media->Channels())) {
                    mask = DefaultChannelMask(v.media->Channels());
                }
                SpeakerGains(azimuth, v.node->center, spread, static_cast<uint32_t>(std::popcount(mask & ~0x8u)), gains);
                uint32_t c = 0;
                for (uint32_t bit = 0; bit < 18 && c < v.channels; ++bit) {
                    if (!(mask & (1u << bit))) {
                        continue;
                    }
                    if (const int slot = SpeakerSlot(1u << bit); slot >= 0) {
                        v.mix_l[c] = v.mix_r[c] = 0.0f;
                        for (int s = 0; s < kSpeakers; ++s) {
                            v.mix_l[c] += gains[slot][s] * kFoldLeft[s];
                            v.mix_r[c] += gains[slot][s] * kFoldRight[s];
                            v.sc_matrix[c][s] = gains[slot][s];
                        }
                    }
                    ++c;
                }
                v.matrix[0] = 1.0f;
                v.matrix[1] = 0.0f;
                v.matrix[2] = 0.0f;
                v.matrix[3] = 1.0f;
            }
            panned = true;
        }
    }
    if (!panned && pos && !pos->has_3d && pos->enable_panner && v.node->positioning_owner) {
        const float pan = std::clamp(v.node->positioning_owner->props.Get(prop::PanLr) / 100.0f, -1.0f, 1.0f);
        v.matrix[0] = pan > 0.0f ? 1.0f - pan : 1.0f;
        v.matrix[1] = 0.0f;
        v.matrix[2] = 0.0f;
        v.matrix[3] = pan < 0.0f ? 1.0f + pan : 1.0f;
    } else if (!panned) {
        v.matrix[0] = 1.0f;
        v.matrix[1] = 0.0f;
        v.matrix[2] = 0.0f;
        v.matrix[3] = 1.0f;
    }
    if (!front_custom) {
        std::copy(v.matrix, v.matrix + 4, v.front_matrix);
    }

    float obstruction_db = 0.0f;
    float occlusion_db = 0.0f;
    if (environment_ && (obj.obstruction > 0.0f || obj.occlusion > 0.0f)) {
        if (environment_->obstruction_volume.enabled) {
            obstruction_db = environment_->obstruction_volume.curve.Evaluate(obj.obstruction * 100.0f);
        }
        if (environment_->obstruction_lpf.enabled) {
            lpf_floor = std::max(lpf_floor, environment_->obstruction_lpf.curve.Evaluate(obj.obstruction * 100.0f));
        }
        if (environment_->occlusion_volume.enabled) {
            occlusion_db = environment_->occlusion_volume.curve.Evaluate(obj.occlusion * 100.0f);
        }
        if (environment_->occlusion_lpf.enabled) {
            lpf_floor = std::max(lpf_floor, environment_->occlusion_lpf.curve.Evaluate(obj.occlusion * 100.0f));
        }
    }

    const float dry_db = volume + bus_voice_db + att_db + cone_db + obstruction_db + occlusion_db + output_bus_db + makeup;
    // FUN_005a0fc0 turns the voice volume and the output bus volume into gains with the eboot's fast 10^x, and 0x59F978 the make-up
    // gain (PBI +0xD4, RTPC 0x24), one conversion each; the attenuation and obstruction curves give linear gains (1 + y) and stay
    // exact
    // (0x5F2FC0 turns the cone's share of its outside volume into a gain with the fast 10^x as well, on the dry ray only)
    v.dry_gain = FastPow10((volume + bus_voice_db) * 0.05f) * FastPow10(makeup * 0.05f) * FastPow10(output_bus_db * 0.05f) *
                 FastPow10(cone_db * 0.05f) * DbToGain(att_db + obstruction_db + occlusion_db) * crossfade;
    float loudest_db = dry_db;

    std::array<Voice::Send, Voice::kMaxSends> previous = v.sends;
    const uint32_t previous_count = v.send_count;
    v.send_count = 0;
    // 0x581880 turns a game-defined send's volume (PBI +0x10C) and each user send's volume (+0xEC) into gains with the fast 10^x,
    // the game-defined one times its object's level; the voice volume and make-up gain come in through the voice's own gain
    // (0x59F800), the aux attenuation curves and the occlusion as gains (0x5F2C50); the aux bus's voice volume is not traced and
    // stays exact
    auto add_send = [&](uint32_t bus_id, float send_db, float level, float exact_db) {
        const BusRuntime* aux = Bus(bus_id);
        if (!aux || v.send_count >= Voice::kMaxSends) {
            return;
        }
        const float total_db = volume + aux->voice_volume_db + makeup + send_db + GainToDb(level) + exact_db;
        loudest_db = std::max(loudest_db, total_db);
        Voice::Send send;
        send.bus = bus_id;
        send.gain = FastPow10(volume * 0.05f) * FastPow10(makeup * 0.05f) * FastPow10(send_db * 0.05f) * level *
                    DbToGain(aux->voice_volume_db + exact_db) * crossfade *
                    (aux->mono_input ? v.send_scale : aux->sum_input ? v.sum_scale : 1.0f);
        send.front = aux->front_input;
        for (uint32_t i = 0; i < previous_count; ++i) {
            if (previous[i].bus == bus_id) {
                send.prev = previous[i].prev;
            }
        }
        v.sends[v.send_count++] = send;
    };
    if (v.node && v.node->use_game_aux) {
        const ObjectState* source = &obj;
        if (!obj.has_aux_sends) {
            auto global = objects_.find(0);
            source = global == objects_.end() ? nullptr : &global->second;
        }
        if (source) {
            for (const auto& [aux_bus, level] : source->aux_sends) {
                if (level > 0.0f) {
                    add_send(aux_bus, game_aux_db, level, aux_game_att + occlusion_db);
                }
            }
        }
    }
    if (v.node) {
        for (int i = 0; i < 4; ++i) {
            if (v.node->user_aux[i]) {
                add_send(v.node->user_aux[i], user_aux_db[i], 1.0f, aux_user_att + occlusion_db);
            }
        }
    }

    // 0x605130, 0x604E60: 16.16 step from the pitch clamped to 2400 cents; a change ramps the step over 1024 frames
    const float rate_ratio = v.media ? static_cast<float>(v.media->SampleRate()) / static_cast<float>(kOutputRate) : 1.0f;
    /* Wwise's resampler as 0x605130 does it: pitch clamped to +-2400 cents, a 16.16 step, and a change ramps over 1024 frames. */
    const float cents = std::clamp(pitch, -2400.0f, 2400.0f);
    if (!v.rs_pitch_set || cents != v.rs_cents) {
        const float scaled = std::exp2(cents * 0.00083333335f) * rate_ratio * 65536.0f;
        const uint32_t step = static_cast<uint32_t>(static_cast<int64_t>(static_cast<double>(scaled) + 0.5));
        if (!v.rs_pitch_set) {
            v.rs_step = step;
            v.rs_ramp = 1024;
            v.rs_pitch_set = true;
        } else {
            if (v.rs_step != v.rs_target) {
                v.rs_step += static_cast<uint32_t>((static_cast<int32_t>(v.rs_target - v.rs_step) * static_cast<int32_t>(v.rs_ramp)) / 1024);
            }
            v.rs_ramp = 0;
        }
        v.rs_target = step;
        v.rs_cents = cents;
    }
    v.pitch_ratio = std::max(static_cast<double>(v.rs_target) / 65536.0, 0.001);
    lpf = std::clamp(std::max(lpf, lpf_floor), 0.0f, 100.0f);
    // PT_TRACE_VOICES=1: each voice's level terms at its first update (with PT_SOLO_EVENT, one event's voices alone)
    static const bool trace_voices = std::getenv("PT_TRACE_VOICES") != nullptr;
    if (trace_voices && !v.lpf_init) {
        const float distance = pos && pos->has_3d ? glm::length(EmitterPosition(v, obj) - listener_position_) : -1.0f;
        LogInfo("voice trace: playing {} node {} object {} distance {:.2f} volume {:.1f} bus {:.1f} attenuation {:.1f} obstruction {:.1f} "
                "occlusion {:.1f} output bus {:.1f} makeup {:.1f} dry {:.1f} dB, lpf {:.0f}, {} sends, loudest {:.1f} dB",
                v.playing_id, v.node ? v.node->id : 0u, v.object, distance, volume, bus_voice_db, att_db, obstruction_db, occlusion_db,
                output_bus_db, makeup, dry_db, lpf, v.send_count, loudest_db);
    }
    if (!v.lpf_init) {
        v.lpf_init = true;
        v.lpf_value = v.lpf_from = v.lpf_target = lpf;
        v.lpf_step = 8;
        ButterworthLowPass(LpfToCutoffHz(lpf), v.lpf_coef);
    } else if (lpf != v.lpf_target) {
        v.lpf_from = v.lpf_value;
        v.lpf_target = lpf;
        v.lpf_step = 0;
    }

    const float effective_db = loudest_db + (crossfade > 0.0f ? GainToDb(crossfade) : -200.0f);
    const bool audible = effective_db >= volume_threshold_db_;
    const BelowThreshold mode = v.node ? v.node->below_threshold : BelowThreshold::ContinueToPlay;
    if (!audible) {
        const bool looping = v.repeats_left != 0;
        if (mode == BelowThreshold::KillVoice || (mode == BelowThreshold::KillIfOneShotElseVirtual && !looping)) {
            FinishVoice(v, v.sequencer ? now_ + std::max<uint64_t>(RemainingOutputFrames(v), kBlockFrames) : now_);
            return;
        }
        if ((mode == BelowThreshold::SetAsVirtualVoice || mode == BelowThreshold::KillIfOneShotElseVirtual) && !v.is_virtual && v.reader) {
            v.is_virtual = true;
            v.virtual_since = now_;
        }
        if (v.is_virtual && !looping) {
            const uint64_t remaining = RemainingOutputFrames(v);
            if (now_ - v.virtual_since >= remaining) {
                FinishVoice(v, v.virtual_since + remaining);
                return;
            }
        }
    } else if (v.is_virtual) {
        v.is_virtual = false;
        const VirtualQueue queue = v.node ? v.node->virtual_queue : VirtualQueue::FromElapsedTime;
        if (queue == VirtualQueue::FromBeginning) {
            v.source_pos = v.range_begin;
            v.reader->Seek(v.source_pos);
        } else if (queue == VirtualQueue::FromElapsedTime) {
            uint64_t target = v.source_pos + static_cast<uint64_t>(static_cast<double>(now_ - v.virtual_since) * v.pitch_ratio);
            const uint64_t pass_end = (v.repeats_left != 0 && v.loop_points) ? v.loop_end : v.range_end;
            if (target >= pass_end && v.repeats_left != 0) {
                const uint64_t begin = v.loop_points ? v.loop_start : v.range_begin;
                const uint64_t span = std::max<uint64_t>(pass_end - begin, 1);
                target = begin + (target - begin) % span;
            }
            if (target >= v.range_end) {
                FinishVoice(v, now_);
                return;
            }
            v.source_pos = target;
            v.reader->Seek(target);
        }
        v.rs_count = 1;
        v.rs_pos = 1.0;
        v.source_done = false;
    }
}

uint32_t SoundEngine::PullSource(Voice& v, float* left, float* right, uint32_t frames, size_t index) {
    uint32_t produced = 0;
    if (v.generator != Generator::None) {
        const ToneParams tone = v.generator == Generator::Tone && v.generator_params ? ReadToneParams(v.generator_params->params) : ToneParams();
        const float gain = DbToGain(tone.gain_db);
        const float sustain = DbToGain(tone.sustain_db);
        const double duration = static_cast<double>(v.generator_frames) / kOutputRate;
        while (produced < frames) {
            if (v.generator_pos >= v.generator_frames) {
                if (v.repeats_left != 0) {
                    if (v.repeats_left > 0) {
                        --v.repeats_left;
                    }
                    v.generator_pos = 0;
                    if (v.generator_frames == 0) {
                        break;
                    }
                    continue;
                }
                break;
            }
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(frames - produced, v.generator_frames - v.generator_pos));
            for (uint32_t i = 0; i < n; ++i) {
                float s = 0.0f;
                if (v.generator == Generator::Tone) {
                    const double t = static_cast<double>(v.generator_pos + i) / kOutputRate;
                    double freq = tone.start_freq;
                    if (tone.sweep && duration > 0.0) {
                        const double w = t / duration;
                        freq = tone.sweep_type == 1 && tone.start_freq > 0.0f && tone.stop_freq > 0.0f
                                   ? tone.start_freq * std::pow(tone.stop_freq / tone.start_freq, w)
                                   : tone.start_freq + (tone.stop_freq - tone.start_freq) * w;
                    }
                    v.tone_phase += freq / kOutputRate;
                    v.tone_phase -= std::floor(v.tone_phase);
                    const float phase = static_cast<float>(v.tone_phase);
                    switch (tone.wave) {
                        case 0: s = std::sin(phase * 6.2831853f); break;
                        case 1: s = 4.0f * std::fabs(phase - 0.5f) - 1.0f; break;
                        case 2: s = phase < 0.5f ? 1.0f : -1.0f; break;
                        case 3: s = 2.0f * phase - 1.0f; break;
                        default: {
                            v.noise_state = v.noise_state * 1664525u + 1013904223u;
                            const float white = static_cast<float>(static_cast<int32_t>(v.noise_state)) / 2147483648.0f;
                            if (tone.wave == 4) {
                                s = white;
                            } else {
                                float* b = v.pink;
                                b[0] = 0.99886f * b[0] + white * 0.0555179f;
                                b[1] = 0.99332f * b[1] + white * 0.0750759f;
                                b[2] = 0.96900f * b[2] + white * 0.1538520f;
                                b[3] = 0.86650f * b[3] + white * 0.3104856f;
                                b[4] = 0.55000f * b[4] + white * 0.5329522f;
                                b[5] = -0.7616f * b[5] - white * 0.0168980f;
                                s = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362f) * 0.11f;
                                b[6] = white * 0.115926f;
                            }
                            break;
                        }
                    }
                    float envelope = 1.0f;
                    if (tone.mode == 1) {
                        const float tt = static_cast<float>(t);
                        if (tt < tone.attack) {
                            envelope = tone.attack > 0.0f ? tt / tone.attack : 1.0f;
                        } else if (tt < tone.attack + tone.decay) {
                            envelope = 1.0f + (sustain - 1.0f) * (tt - tone.attack) / std::max(tone.decay, 1e-6f);
                        } else if (tt < tone.attack + tone.decay + tone.sustain_duration) {
                            envelope = sustain;
                        } else {
                            const float r = tt - tone.attack - tone.decay - tone.sustain_duration;
                            envelope = tone.release > 0.0f ? sustain * std::max(0.0f, 1.0f - r / tone.release) : 0.0f;
                        }
                    }
                    s *= gain * envelope;
                }
                left[produced + i] = s * v.mix_l[0];
                right[produced + i] = s * v.mix_r[0];
            }
            v.generator_pos += n;
            produced += n;
        }
        return produced;
    }
    if (!v.reader) {
        return 0;
    }
    const uint32_t channels = v.media->Channels();
    while (produced < frames) {
        const uint64_t pass_end = (v.repeats_left != 0 && v.loop_points) ? v.loop_end : v.range_end;
        if (v.source_pos >= pass_end) {
            if (v.repeats_left != 0) {
                if (v.repeats_left > 0) {
                    --v.repeats_left;
                }
                v.source_pos = v.loop_points ? v.loop_start : v.range_begin;
                v.reader->Seek(v.source_pos);
                if (pass_end <= v.source_pos) {
                    break;
                }
                continue;
            }
            break;
        }
        const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>({frames - produced, pass_end - v.source_pos, kBlockFrames}));
        const uint32_t got = v.reader->Read(v.scratch.data(), want);
        if (got == 0) {
            v.source_pos = pass_end;
            if (v.repeats_left == 0) {
                break;
            }
            continue;
        }
        for (const auto& marker : v.markers) {
            if (marker.sample >= v.source_pos && marker.sample < v.source_pos + got && !marker.label.empty()) {
                pending_notifications_.push_back({EngineNotification::Kind::Marker, v.playing_id, marker.label});
            }
        }
        const float* src = v.scratch.data();
        if (channels == 1) {
            const float cl = v.mix_l[0];
            const float cr = v.mix_r[0];
            for (uint32_t i = 0; i < got; ++i) {
                left[produced + i] = src[i] * cl;
                right[produced + i] = src[i] * cr;
            }
        } else {
            const uint32_t used = std::min<uint32_t>(channels, 8);
            for (uint32_t i = 0; i < got; ++i) {
                const float* frame = src + static_cast<size_t>(i) * channels;
                float l = 0.0f;
                float r = 0.0f;
                for (uint32_t c = 0; c < used; ++c) {
                    l += frame[c] * v.mix_l[c];
                    r += frame[c] * v.mix_r[c];
                }
                left[produced + i] = l;
                right[produced + i] = r;
            }
            for (uint32_t c = 0; c < v.sc_split; ++c) {
                float* dst = v.sc_rs[c].data() + index + produced;
                for (uint32_t i = 0; i < got; ++i) {
                    dst[i] = src[static_cast<size_t>(i) * channels + c];
                }
            }
        }
        v.source_pos += got;
        produced += got;
    }
    return produced;
}

uint32_t SoundEngine::RenderVoice(Voice& v, uint32_t offset, uint32_t frames) {
    const uint32_t n = frames - offset;
    float* out_l = voice_left_.data();
    float* out_r = voice_right_.data();
    const double ratio = static_cast<double>(std::max(v.rs_step, v.rs_target)) / 65536.0;
    const size_t need = static_cast<size_t>(v.rs_pos + (n - 1) * ratio) + 3;
    const uint32_t split = v.sc_split;
    if (need > v.rs_l.size()) {
        v.rs_l.resize(need + 16, 0.0f);
        v.rs_r.resize(need + 16, 0.0f);
        for (uint32_t c = 0; c < split; ++c) {
            v.sc_rs[c].resize(need + 16, 0.0f);
        }
    }
    if (need > v.rs_count) {
        const uint32_t want = static_cast<uint32_t>(need - v.rs_count);
        uint32_t got = 0;
        if (!v.source_done) {
            while (got < want) {
                const uint32_t chunk = std::min<uint32_t>(want - got, kBlockFrames);
                const uint32_t pulled = PullSource(v, v.rs_l.data() + v.rs_count + got, v.rs_r.data() + v.rs_count + got, chunk, v.rs_count + got);
                got += pulled;
                if (pulled < chunk) {
                    v.source_done = true;
                    v.rs_valid_end = v.rs_count + got;
                    break;
                }
            }
        }
        std::fill(v.rs_l.begin() + static_cast<ptrdiff_t>(v.rs_count + got), v.rs_l.begin() + static_cast<ptrdiff_t>(need), 0.0f);
        std::fill(v.rs_r.begin() + static_cast<ptrdiff_t>(v.rs_count + got), v.rs_r.begin() + static_cast<ptrdiff_t>(need), 0.0f);
        for (uint32_t c = 0; c < split; ++c) {
            std::fill(v.sc_rs[c].begin() + static_cast<ptrdiff_t>(v.rs_count + got), v.sc_rs[c].begin() + static_cast<ptrdiff_t>(need), 0.0f);
        }
        v.rs_count = need;
    }
    uint32_t produced = 0;
    const float* bl = v.rs_l.data();
    const float* br = v.rs_r.data();
    for (; produced < n; ++produced) {
        const size_t i = static_cast<size_t>(v.rs_pos);
        if (v.source_done && i >= v.rs_valid_end) {
            break;
        }
        // 0x605BC0: linear between the frames around the index, 16-bit fraction
        const float f = static_cast<float>(v.rs_pos - static_cast<double>(i));
        out_l[produced] = bl[i] + f * (bl[i + 1] - bl[i]);
        out_r[produced] = br[i] + f * (br[i + 1] - br[i]);
        for (uint32_t c = 0; c < split; ++c) {
            const float* bc = v.sc_rs[c].data();
            sc_voice_[c][produced] = bc[i] + f * (bc[i + 1] - bc[i]);
        }
        uint32_t step = v.rs_step;
        if (step != v.rs_target) {
            // 0x606400
            ++v.rs_ramp;
            const int64_t delta = static_cast<int64_t>(v.rs_target) - v.rs_step;
            step = static_cast<uint32_t>((static_cast<int64_t>(v.rs_step) * 1024 + delta * v.rs_ramp) >> 10);
            if (v.rs_ramp >= 1024) {
                v.rs_step = v.rs_target;
            }
        }
        v.rs_pos += static_cast<double>(step) / 65536.0;
    }
    const size_t base = static_cast<size_t>(v.rs_pos);
    if (base > 1) {
        const size_t drop = std::min(base - 1, v.rs_count);
        std::memmove(v.rs_l.data(), v.rs_l.data() + drop, sizeof(float) * (v.rs_count - drop));
        std::memmove(v.rs_r.data(), v.rs_r.data() + drop, sizeof(float) * (v.rs_count - drop));
        for (uint32_t c = 0; c < split; ++c) {
            std::memmove(v.sc_rs[c].data(), v.sc_rs[c].data() + drop, sizeof(float) * (v.rs_count - drop));
        }
        v.rs_count -= drop;
        v.rs_pos -= static_cast<double>(drop);
        v.rs_valid_end = v.rs_valid_end > drop ? v.rs_valid_end - drop : 0;
    }
    if (produced == 0) {
        return 0;
    }
    ApplyVoiceLpf(v, out_l, out_r, produced);
    const uint64_t t0 = now_ + offset;
    const uint64_t t1 = t0 + produced;
    const float fade0 = v.play_fade.Value(t0) * v.stop_fade.Value(t0) * v.pause_fade.Value(t0);
    const float fade1 = v.play_fade.Value(t1) * v.stop_fade.Value(t1) * v.pause_fade.Value(t1);
    float target[4];
    for (int k = 0; k < 4; ++k) {
        target[k] = v.matrix[k] * v.dry_gain;
    }
    if (!v.gain_init) {
        std::copy(target, target + 4, v.gain_prev);
        for (uint32_t i = 0; i < v.send_count; ++i) {
            v.sends[i].prev = v.sends[i].gain;
        }
        v.sc_dry_prev = v.dry_gain;
        v.gain_init = true;
    }
    const float inv = 1.0f / static_cast<float>(produced);
    const float block_inv = 1.0f / static_cast<float>(frames);
    BusRuntime* bus = Bus(v.bus_id);
    if (controller_pcm_capture_.HasSelectedEvents()) {
        const auto record = playing_.find(v.playing_id);
        if (record != playing_.end()) {
            for (uint32_t i = 0; i < produced; ++i) {
                const float w = (static_cast<float>(i) + 1.0f) * inv;
                const float fade = fade0 + (fade1 - fade0) * w;
                const float a = (v.gain_prev[0] + (target[0] - v.gain_prev[0]) * w) * fade;
                const float b = (v.gain_prev[1] + (target[1] - v.gain_prev[1]) * w) * fade;
                const float c = (v.gain_prev[2] + (target[2] - v.gain_prev[2]) * w) * fade;
                const float d = (v.gain_prev[3] + (target[3] - v.gain_prev[3]) * w) * fade;
                controller_pcm_capture_.Accumulate(record->second.event_id, offset + i, a * out_l[i] + b * out_r[i],
                                                   c * out_l[i] + d * out_r[i]);
            }
        }
    }
    // the limiter's side chain takes what reaches the master through buses without effects, times their gains on the way
    auto chain_at = [&](const BusRuntime* dest, uint32_t i) {
        return dest->chain_prev + (dest->chain - dest->chain_prev) * (static_cast<float>(offset + i) + 1.0f) * block_inv;
    };
    auto mix = [&](BusRuntime* dest, const float* g0, const float* g1, bool side) {
        if (!dest) {
            return;
        }
        if (g0[0] == 0.0f && g0[1] == 0.0f && g0[2] == 0.0f && g0[3] == 0.0f && g1[0] == 0.0f && g1[1] == 0.0f && g1[2] == 0.0f &&
            g1[3] == 0.0f) {
            return;
        }
        dest->has_input = true;
        float* dl = dest->left.data() + offset;
        float* dr = dest->right.data() + offset;
        const bool sc = side && dest->sc_direct;
        float* fl = sc_[kFl].data() + offset;
        float* fr = sc_[kFr].data() + offset;
        for (uint32_t i = 0; i < produced; ++i) {
            const float w = (static_cast<float>(i) + 1.0f) * inv;
            const float fade = fade0 + (fade1 - fade0) * w;
            const float a = (g0[0] + (g1[0] - g0[0]) * w) * fade;
            const float b = (g0[1] + (g1[1] - g0[1]) * w) * fade;
            const float c = (g0[2] + (g1[2] - g0[2]) * w) * fade;
            const float d = (g0[3] + (g1[3] - g0[3]) * w) * fade;
            const float l = a * out_l[i] + b * out_r[i];
            const float r = c * out_l[i] + d * out_r[i];
            dl[i] += l;
            dr[i] += r;
            if (sc) {
                const float k = chain_at(dest, i);
                fl[i] += l * k;
                fr[i] += r * k;
            }
        }
    };
    mix(bus, v.gain_prev, target, v.sc_mode == 0);
    std::copy(target, target + 4, v.gain_prev);
    if (bus && bus->sc_direct && v.sc_mode != 0 && (v.sc_dry_prev != 0.0f || v.dry_gain != 0.0f)) {
        float* weight = sc_weight_.data();
        for (uint32_t i = 0; i < produced; ++i) {
            const float w = (static_cast<float>(i) + 1.0f) * inv;
            weight[i] = (v.sc_dry_prev + (v.dry_gain - v.sc_dry_prev) * w) * (fade0 + (fade1 - fade0) * w) * chain_at(bus, i);
        }
        if (v.sc_mode == 1) {
            // the mono signal is (left + right) / (mix_l + mix_r) of the stereo pair PullSource made of it
            const float norm = v.mix_l[0] + v.mix_r[0];
            const float scale = norm > 1e-9f ? 1.0f / norm : 0.0f;
            for (int s = 0; s < 8; ++s) {
                const float m = v.sc_matrix[0][s] * scale;
                if (m == 0.0f) {
                    continue;
                }
                float* dst = sc_[s].data() + offset;
                for (uint32_t i = 0; i < produced; ++i) {
                    dst[i] += (out_l[i] + out_r[i]) * m * weight[i];
                }
            }
        } else {
            for (uint32_t c = 0; c < split; ++c) {
                const float* src = sc_voice_[c].data();
                for (int s = 0; s < 8; ++s) {
                    const float m = v.sc_matrix[c][s];
                    if (m == 0.0f) {
                        continue;
                    }
                    float* dst = sc_[s].data() + offset;
                    for (uint32_t i = 0; i < produced; ++i) {
                        dst[i] += src[i] * m * weight[i];
                    }
                }
            }
        }
    }
    v.sc_dry_prev = v.dry_gain;
    for (uint32_t i = 0; i < v.send_count; ++i) {
        auto& send = v.sends[i];
        const float* m = send.front ? v.front_matrix : v.matrix;
        const float g0[4] = {m[0] * send.prev, m[1] * send.prev, m[2] * send.prev, m[3] * send.prev};
        const float g1[4] = {m[0] * send.gain, m[1] * send.gain, m[2] * send.gain, m[3] * send.gain};
        mix(Bus(send.bus), g0, g1, true);
        send.prev = send.gain;
    }
    return produced;
}

uint32_t SoundEngine::RenderMotion(Voice& v, uint32_t offset, uint32_t frames) {
    const uint32_t n = frames - offset;
    uint32_t produced = 0;
    bool sampled = false;
    while (produced < n) {
        if (v.generator_pos >= v.generator_frames) {
            if (v.repeats_left == 0 || v.generator_frames == 0) {
                break;
            }
            if (v.repeats_left > 0) {
                --v.repeats_left;
            }
            v.generator_pos = 0;
        }
        if (!sampled) {
            sampled = true;
            const uint64_t t0 = now_ + offset + produced;
            const float gain = v.dry_gain * v.play_fade.Value(t0) * v.stop_fade.Value(t0) * v.pause_fade.Value(t0);
            const double seconds = static_cast<double>(v.generator_pos) / kOutputRate;
            motion_mix_[0] += std::max(0.0f, v.motion->Sample(0, seconds) * gain);
            motion_mix_[1] += std::max(0.0f, v.motion->Sample(1, seconds) * gain);
        }
        const uint64_t step = std::min<uint64_t>(n - produced, v.generator_frames - v.generator_pos);
        v.generator_pos += step;
        produced += static_cast<uint32_t>(step);
    }
    return produced;
}

void SoundEngine::ApplyVoiceLpf(Voice& v, float* left, float* right, uint32_t frames) {
    // the stereo pair and the channels kept apart for the limiter's side chain go through the same filter
    float* channels[10] = {left, right};
    float* history[10] = {v.lpf_hist[0], v.lpf_hist[1]};
    uint32_t count = 2;
    for (uint32_t c = 0; c < v.sc_split; ++c, ++count) {
        channels[count] = sc_voice_[c].data();
        history[count] = v.sc_lpf_hist[c];
    }
    for (uint32_t done = 0; done < frames;) {
        const uint32_t n = std::min<uint32_t>(frames - done, 128);
        const bool was_bypassed = v.lpf_value <= 0.1f;
        if (v.lpf_step < 8) {
            ++v.lpf_step;
            v.lpf_value = v.lpf_from + (v.lpf_target - v.lpf_from) * static_cast<float>(v.lpf_step) * 0.125f;
            ButterworthLowPass(LpfToCutoffHz(v.lpf_value), v.lpf_coef);
        }
        float* channel[10];
        for (uint32_t c = 0; c < count; ++c) {
            channel[c] = channels[c] + done;
        }
        if (v.lpf_value <= 0.1f && (v.lpf_step >= 8 || v.lpf_target <= 0.1f)) {
            for (uint32_t c = 0; c < count; ++c) {
                const float last = channel[c][n - 1];
                std::fill(history[c], history[c] + 4, last);
            }
        } else {
            const float* k = v.lpf_coef;
            for (uint32_t c = 0; c < count; ++c) {
                float* h = history[c];
                if (was_bypassed) {
                    std::fill(h, h + 4, channel[c][0]);
                }
                float x1 = h[0];
                float x2 = h[1];
                float y1 = h[2];
                float y2 = h[3];
                float* s = channel[c];
                for (uint32_t i = 0; i < n; ++i) {
                    const float x = s[i];
                    const float y = k[0] * x + k[1] * x1 + k[2] * x2 - k[3] * y1 - k[4] * y2;
                    x2 = x1;
                    x1 = x;
                    y2 = y1;
                    y1 = y;
                    s[i] = y;
                }
                h[0] = x1;
                h[1] = x2;
                h[2] = y1;
                h[3] = y2;
            }
        }
        done += n;
    }
}

void SoundEngine::ProcessDue(uint64_t block_end) {
    for (int pass = 0; pass < 64; ++pass) {
        bool any = false;
        std::vector<PendingAction>& due = due_actions_;
        due.clear();
        for (size_t i = 0; i < pending_.size();) {
            if (pending_[i].time < block_end) {
                due.push_back(std::move(pending_[i]));
                pending_[i] = std::move(pending_.back());
                pending_.pop_back();
            } else {
                ++i;
            }
        }
        std::sort(due.begin(), due.end(), [](const PendingAction& a, const PendingAction& b) {
            return a.time != b.time ? a.time < b.time : a.order < b.order;
        });
        for (auto& action : due) {
            any = true;
            if (!action.cancelled) {
                ExecuteAction(*action.action, action.playing_id, action.object, std::max(action.time, now_), action.external);
            }
            Release(action.playing_id);
        }
        std::vector<Timer>& timers = due_timers_;
        timers.clear();
        for (size_t i = 0; i < timers_.size();) {
            if (timers_[i].time < block_end) {
                timers.push_back(timers_[i]);
                timers_[i] = timers_.back();
                timers_.pop_back();
            } else {
                ++i;
            }
        }
        std::sort(timers.begin(), timers.end(), [](const Timer& a, const Timer& b) { return a.time != b.time ? a.time < b.time : a.order < b.order; });
        for (const auto& timer : timers) {
            any = true;
            const uint64_t time = std::max(timer.time, now_);
            if (timer.music) {
                MusicPlayer* player = FindMusic(timer.serial);
                if (player && player->has_next && !player->stopping && !player->paused && player->next_time == timer.time) {
                    MusicStartSegment(*player, player->next_segment, time);
                }
            } else {
                Sequencer* seq = FindSequencer(timer.serial);
                if (seq && seq->item_scheduled && !seq->paused && seq->scheduled_time == timer.time) {
                    SequencerStartItem(*seq, time);
                }
            }
        }
        if (!any) {
            break;
        }
    }
}

void SoundEngine::RenderBlock(float* out, uint32_t frames, uint32_t output_channels) {
    block_end_ = now_ + frames;
    created_this_block_ = 0;
    motion_mix_[0] = motion_mix_[1] = 0.0f;
    controller_pcm_capture_.BeginBlock(frames);
    for (auto& [id, bus] : buses_) {
        std::fill(bus->left.begin(), bus->left.begin() + frames, 0.0f);
        std::fill(bus->right.begin(), bus->right.begin() + frames, 0.0f);
        bus->has_input = false;
    }
    for (auto& channel : sc_) {
        std::fill(channel.begin(), channel.begin() + frames, 0.0f);
    }
    ProcessDue(block_end_);
    UpdateBuses();
    for (size_t index = 0; index < voices_.size(); ++index) {
        Voice* v = voices_[index].get();
        if (v->finished) {
            continue;
        }
        if (!v->started) {
            if (v->paused || v->start_time >= block_end_) {
                continue;
            }
            v->started = true;
        }
        UpdateVoiceParams(*v);
        if (v->finished) {
            NotifyVoiceFinished(v);
            continue;
        }
        if (v->paused) {
            continue;
        }
        if (v->is_virtual) {
            continue;
        }
        const uint32_t offset = v->start_time > now_ ? static_cast<uint32_t>(v->start_time - now_) : 0;
        if (v->generator == Generator::Motion) {
            const uint32_t produced = RenderMotion(*v, offset, frames);
            if (produced < frames - offset) {
                FinishVoice(*v, now_ + offset + produced);
                NotifyVoiceFinished(v);
                continue;
            }
        } else {
            const uint32_t produced = RenderVoice(*v, offset, frames);
            const uint64_t end_time = now_ + offset + produced;
            if (produced < frames - offset && v->source_done) {
                FinishVoice(*v, end_time);
                NotifyVoiceFinished(v);
                continue;
            }
        }
        if (v->stopping && v->stop_fade.Done(block_end_)) {
            FinishVoice(*v, std::min(v->stop_fade.start + v->stop_fade.length, block_end_));
            NotifyVoiceFinished(v);
            continue;
        }
        if (v->pausing && v->pause_fade.Done(block_end_)) {
            v->pausing = false;
            v->paused = true;
        }
    }
    for (size_t index = 0; index < voices_.size(); ++index) {
        Voice* v = voices_[index].get();
        if (v->finished && !v->notified) {
            NotifyVoiceFinished(v);
        }
    }
    controller_pcm_capture_.SubmitBlock();
    for (BusRuntime* bus : bus_order_) {
        if (bus->has_input) {
            bus->active_until = block_end_ + static_cast<uint64_t>(bus->tail_seconds * kOutputRate);
        }
        const bool active = bus->has_input || (!bus->effects.empty() && now_ < bus->active_until);
        if (!active) {
            if (bus->effects_running) {
                for (auto& effect : bus->effects) {
                    effect->Reset();
                }
                bus->effects_running = false;
            }
            continue;
        }
        bus->effects_running = true;
        const float g0 = bus->gain_prev;
        const float g1 = bus->gain;
        const float inv = 1.0f / static_cast<float>(frames);
        if (g0 != 1.0f || g1 != 1.0f) {
            for (uint32_t i = 0; i < frames; ++i) {
                const float g = g0 + (g1 - g0) * (static_cast<float>(i) + 1.0f) * inv;
                bus->left[i] *= g;
                bus->right[i] *= g;
            }
        }
        /* 0x5AE4B0 links the master's 7.1 channels: the limiter detects on the loudest channel of each frame, not on the stereo fold the port outputs. */
        if (bus == master_) {
            // 0x5AE4B0 links the master's 7.1 channels: the limiter detects the largest of them in each frame, not the stereo fold
            for (uint32_t i = 0; i < frames; ++i) {
                float peak = 0.0f;
                for (const auto& channel : sc_) {
                    peak = std::max(peak, std::fabs(channel[i]));
                }
                sc_peak_[i] = peak;
            }
            for (auto& effect : bus->effects) {
                effect->SetDetector(sc_peak_.data());
            }
        }
        if (bus == master_ && output_channels != 2) {
            std::array<float*, 8> speakers{};
            for (size_t channel = 0; channel < speakers.size(); ++channel) {
                speakers[channel] = sc_[channel].data();
            }
            for (auto& effect : bus->effects) {
                effect->ProcessSurround(bus->left.data(), bus->right.data(), speakers, frames);
            }
        } else {
            for (auto& effect : bus->effects) {
                effect->Process(bus->left.data(), bus->right.data(), frames);
            }
        }
        if (BusRuntime* parent = bus->parent; parent && parent->sc_direct && !bus->sc_direct) {
            // an effect bus reaches the side chain as its stereo output on FL and FR
            for (uint32_t i = 0; i < frames; ++i) {
                const float k = parent->chain_prev + (parent->chain - parent->chain_prev) * (static_cast<float>(i) + 1.0f) * inv;
                sc_[kFl][i] += bus->left[i] * k;
                sc_[kFr][i] += bus->right[i] * k;
            }
        }
        if (BusRuntime* parent = bus->parent) {
            parent->has_input = true;
            for (uint32_t i = 0; i < frames; ++i) {
                parent->left[i] += bus->left[i];
                parent->right[i] += bus->right[i];
            }
        }
    }
    float peak_l = 0.0f;
    float peak_r = 0.0f;
    const std::array<const float*, 8> speaker_channels = {sc_[0].data(), sc_[1].data(), sc_[2].data(), sc_[3].data(),
                                                           sc_[4].data(), sc_[5].data(), sc_[6].data(), sc_[7].data()};
    for (uint32_t i = 0; i < frames; ++i) {
        float l = 0.0f;
        float r = 0.0f;
        if (master_) {
            l = master_->left[i];
            r = master_->right[i];
        }
        l = std::clamp(l * master_gain_, -1.0f, 1.0f);
        r = std::clamp(r * master_gain_, -1.0f, 1.0f);
        if (output_channels == 2) {
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        } else if (output_channels == 6) {
            WwiseToSdl51Frame(speaker_channels, i, out + static_cast<size_t>(i) * output_channels);
            for (uint32_t channel = 0; channel < output_channels; ++channel) {
                float& sample = out[static_cast<size_t>(i) * output_channels + channel];
                sample = std::clamp(sample * master_gain_, -1.0f, 1.0f);
            }
        } else {
            for (uint32_t channel = 0; channel < output_channels; ++channel) {
                const uint8_t speaker = kWwiseToSdl71[channel];
                const float sample = sc_[speaker][i] * master_gain_;
                out[static_cast<size_t>(i) * output_channels + channel] = std::clamp(sample, -1.0f, 1.0f);
            }
        }
        peak_l = std::max(peak_l, std::fabs(l));
        peak_r = std::max(peak_r, std::fabs(r));
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const std::unique_ptr<Voice>& v) { return v->finished && v->notified; }),
                  voices_.end());
    sequencers_.erase(std::remove_if(sequencers_.begin(), sequencers_.end(), [](const std::unique_ptr<Sequencer>& s) { return s->released; }),
                      sequencers_.end());
    music_.erase(std::remove_if(music_.begin(), music_.end(), [](const std::unique_ptr<MusicPlayer>& m) { return m->released; }), music_.end());
    if (!removed_objects_.empty()) {
        for (auto it = removed_objects_.begin(); it != removed_objects_.end();) {
            const GameObjectId id = *it;
            const bool used = std::any_of(voices_.begin(), voices_.end(), [id](const auto& v) { return v->object == id; }) ||
                              std::any_of(sequencers_.begin(), sequencers_.end(), [id](const auto& q) { return q->object == id; }) ||
                              std::any_of(music_.begin(), music_.end(), [id](const auto& m) { return m->object == id; });
            if (!used) {
                objects_.erase(id);
                it = removed_objects_.erase(it);
            } else {
                ++it;
            }
        }
    }
    motion_levels_.store(MotionToPadByte(motion_mix_[0]) | static_cast<uint32_t>(MotionToPadByte(motion_mix_[1])) << 8, std::memory_order_relaxed);
    now_ = block_end_;
    std::lock_guard lock(stats_mutex_);
    stats_.peak_left = std::max(stats_.peak_left * 0.95f, peak_l);
    stats_.peak_right = std::max(stats_.peak_right * 0.95f, peak_r);
}

void SoundEngine::Render(float* out, uint32_t frames) {
    Render(out, frames, 2);
}

void SoundEngine::Render(float* out, uint32_t frames, uint32_t output_channels) {
    if (output_channels != 2 && output_channels != 6 && output_channels != 8) {
        output_channels = 2;
    }
    if (frozen_.load()) {
        std::memset(out, 0, sizeof(float) * frames * output_channels);
        motion_levels_.store(0, std::memory_order_relaxed);
        return;
    }
#if defined(__aarch64__)
    /* Apple silicon (docs/macos.md): FPCR.FZ is the ARM64 flush to zero of both inputs and results, as FTZ and DAZ below */
    const uint64_t fpcr = __arm_rsr64("fpcr");
    __arm_wsr64("fpcr", fpcr | (uint64_t(1) << 24));
#else
    const unsigned int csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040);
#endif
    {
        std::lock_guard lock(command_mutex_);
        command_work_.swap(commands_);
    }
    for (auto& command : command_work_) {
        ApplyCommand(command);
    }
    command_work_.clear();
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(kBlockFrames, frames - done);
        RenderBlock(out + static_cast<size_t>(done) * output_channels, n, output_channels);
        done += n;
    }
    if (!global_pause_) {
        rendered_frames_.fetch_add(frames);
    }
    if (!ended_ids_.empty()) {
        std::lock_guard lock(status_mutex_);
        for (PlayingId id : ended_ids_) {
            auto it = live_ids_.find(id);
            if (it != live_ids_.end()) {
                auto ev = live_events_.find(it->second);
                if (ev != live_events_.end() && ev->second > 0 && --ev->second == 0) {
                    live_events_.erase(ev);
                }
                live_ids_.erase(it);
            }
        }
        ended_ids_.clear();
    }
    if (!pending_notifications_.empty()) {
        std::lock_guard lock(notify_mutex_);
        for (auto& n : pending_notifications_) {
            notifications_.push_back(std::move(n));
        }
        pending_notifications_.clear();
    }
    {
        std::lock_guard lock(stats_mutex_);
        stats_.voices = 0;
        stats_.virtual_voices = 0;
        for (const auto& v : voices_) {
            if (v->started && !v->finished) {
                ++stats_.voices;
                stats_.virtual_voices += v->is_virtual ? 1 : 0;
            }
        }
        stats_.playing_ids = static_cast<uint32_t>(playing_.size());
        stats_.sequencers = static_cast<uint32_t>(sequencers_.size());
        stats_.music = static_cast<uint32_t>(music_.size());
    }
#if defined(__aarch64__)
    __arm_wsr64("fpcr", fpcr);
#else
    _mm_setcsr(csr);
#endif
}

}
