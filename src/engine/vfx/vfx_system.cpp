#include "engine/vfx/vfx_system.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <numeric>

#include "engine/core/log.h"
#include "engine/render/scene_lighting.h"

namespace pt::vfx {

std::atomic<float> g_flare_ghost_scale{0.0f};
void SetFlareGhostScale(float scale) { g_flare_ghost_scale = scale; }

namespace {

constexpr float kTwoPi = 6.2831855f;
constexpr float kDegToRad = 0.017453292f;
constexpr float kRandomScale = 2.3283064e-10f;
constexpr float kProbabilityScale = 2.3050234e-08f;
constexpr size_t kMaxParticles = 4096;
constexpr const char* kLensFlareDir = "/Assets/sh/effect/vfx_data/lensflare/";
constexpr float kFlareCorners[4] = {-2.3561945f, -0.7853982f, 2.3561945f, 0.7853982f};
constexpr float kFlareCheck = 6.0f / 30.0f;
constexpr float kFlareRayCheck = 4.0f / 30.0f;
constexpr float kFlareLux = 10.267087f;
constexpr float kCameraCut = 3.0f;
constexpr float kCameraCutCos = 0.7071f;

glm::vec3 TransformPoint(const glm::mat4& m, const glm::vec3& p) {
    return glm::vec3(m * glm::vec4(p, 1.0f));
}

glm::mat3 EulerYxz(const glm::vec3& r) {
    const glm::mat4 m = glm::rotate(glm::mat4(1.0f), r.y, glm::vec3(0.0f, 1.0f, 0.0f)) *
                        glm::rotate(glm::mat4(1.0f), r.x, glm::vec3(1.0f, 0.0f, 0.0f)) *
                        glm::rotate(glm::mat4(1.0f), r.z, glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::mat3(m);
}

glm::mat3 PlaneAxisFix(uint32_t mode, const glm::vec3& d) {
    switch (mode) {
    case 1:
        return glm::mat3(glm::rotate(glm::mat4(1.0f), std::atan2(-d.y, d.z), glm::vec3(1.0f, 0.0f, 0.0f)));
    case 2:
        return glm::mat3(glm::rotate(glm::mat4(1.0f), std::atan2(d.x, d.z), glm::vec3(0.0f, 1.0f, 0.0f)));
    case 3:
        return glm::mat3(glm::rotate(glm::mat4(1.0f), std::atan2(d.x, -d.y), glm::vec3(0.0f, 0.0f, 1.0f)));
    default:
        return glm::mat3(1.0f);
    }
}

glm::mat4 EulerZyx(const glm::vec3& r) {
    return glm::rotate(glm::mat4(1.0f), r.z, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::rotate(glm::mat4(1.0f), r.y, glm::vec3(0.0f, 1.0f, 0.0f)) *
           glm::rotate(glm::mat4(1.0f), r.x, glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::vec4 WrapUnorm8(const glm::vec4& c) {
    glm::vec4 out;
    for (int i = 0; i < 4; ++i) {
        const float v = c[i] * 255.0f;
        out[i] = static_cast<float>((std::abs(v) < 2.0e9f ? static_cast<int32_t>(v) : 0) & 0xFF) / 255.0f;
    }
    return out;
}

float Saturate(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

uint32_t XorShiftOnce(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 5;
    return x;
}

float HalfTrunc(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    const uint32_t mag = std::min(bits & 0x7FFFFFFFu, 0x47FFFFFFu);
    bits = mag < 0x38000000u ? (bits & 0x80000000u) : (bits & 0x80000000u) | (mag & ~0x1FFFu);
    std::memcpy(&f, &bits, 4);
    return f;
}

float Unorm8(float f) {
    return std::floor(std::clamp(f, 0.0f, 1.0f) * 255.0f) / 255.0f;
}

glm::vec3 PackedColor(const glm::vec4& c) {
    const float u = HalfTrunc(std::floor(c.r * 15.0f) * 16.0f + std::floor(c.g * 15.0f) + c.b * 0.9375f);
    const float hi = std::floor(u * 0.0625f);
    return glm::vec3(hi / 15.0f, std::floor(u - hi * 16.0f) / 15.0f, (u - std::floor(u)) * 1.0666667f);
}

}

uint32_t XorShift(uint32_t& state) {
    uint32_t x = state ? state : 1u;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 5;
    state = x;
    return x;
}

float Random01(uint32_t& state) {
    return static_cast<float>(XorShift(state)) * kRandomScale;
}

System::System() = default;
System::~System() = default;

std::shared_ptr<const EffectDef> System::Load(const std::string& path) {
    if (auto it = effects_.find(path); it != effects_.end()) {
        return it->second;
    }
    std::shared_ptr<const EffectDef> def;
    if (!reader_) {
        return def;
    }
    const auto started = std::chrono::steady_clock::now();
    struct SlowLog {
        const std::string& path;
        std::chrono::steady_clock::time_point started;
        ~SlowLog() {
            if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); ms > 5.0) {
                LogInfo("vfx: effect {} loaded in {:.1f} ms", path, ms);
            }
        }
    } slow_log{path, started};
    auto bytes = reader_(path);
    if (!bytes) {
        if (missing_.insert(path).second) {
            LogWarn("vfx: {} not found", path);
        }
        return def;
    }
    File file;
    std::string error;
    if (ParseFile(*bytes, file, &error)) {
        const size_t slash = path.find_last_of('/');
        def = CompileEffect(file, path.substr(slash == std::string::npos ? 0 : slash + 1));
    } else {
        LogWarn("vfx: {}: {}", path, error);
    }
    if (def) {
        for (const ShapeDef& shape : def->shapes) {
            if (shape.kind != ShapeKind::Model || models_.contains(shape.model)) {
                continue;
            }
            std::shared_ptr<const ModelMesh> mesh = model_reader_ ? model_reader_(shape.model) : nullptr;
            if (mesh) {
                LogDebug("vfx: {} model {:#x}: {} vertices, {} triangles", def->name, shape.model, mesh->positions.size(), mesh->indices.size() / 3);
            } else {
                LogWarn("vfx: {} model {:#x} not found", def->name, shape.model);
            }
            models_[shape.model] = std::move(mesh);
        }
    }
    effects_[path] = def;
    return def;
}

const LensFlareDef* System::LoadFlare(const std::string& name) {
    if (auto it = flares_.find(name); it != flares_.end()) {
        return it->second.get();
    }
    std::unique_ptr<LensFlareDef> def;
    if (reader_) {
        const std::string path = kLensFlareDir + name + ".vfxlf";
        if (auto bytes = reader_(path)) {
            auto flare = std::make_unique<LensFlareDef>();
            if (LoadLensFlare(*bytes, name, *flare)) {
                def = std::move(flare);
            } else {
                LogWarn("vfx: lens flare {} has no shapes", path);
            }
        } else {
            LogWarn("vfx: lens flare {} not found", path);
        }
    }
    const LensFlareDef* result = def.get();
    flares_[name] = std::move(def);
    return result;
}

void System::Preload(const std::string& path, const TextureResolver& textures, const TextureResolver& cubes) {
    const std::shared_ptr<const EffectDef> def = Load(path);
    if (!def) {
        return;
    }
    auto texture = [&textures](const std::string& name) {
        if (!name.empty()) {
            textures(name);
        }
    };
    for (const ShapeDef& shape : def->shapes) {
        const MaterialDef& m = shape.material;
        texture(m.texture);
        texture(shape.light_mask);
        if (m.kind == MaterialKind::Scroll) {
            texture(m.rain_texture);
        }
        if (m.kind == MaterialKind::Liquid && m.reflection > 0.0f && !m.reflection_texture.empty()) {
            cubes(m.reflection_texture);
        }
    }
    for (const FlareDef& flare : def->flares) {
        if (const LensFlareDef* lens = LoadFlare(flare.lens_flare)) {
            for (const LensFlareElement& el : lens->elements) {
                texture(el.texture);
            }
        }
    }
}

void System::ClearCache() {
    effects_.clear();
    flares_.clear();
    models_.clear();
}

uint32_t System::SeedFor(const Instance& inst, uint32_t seed, uint8_t type, uint32_t node_id) const {
    if (type == 2) {
        return seed ? seed : 1u;
    }
    if (type == 1) {
        return seed + inst.random;
    }
    return inst.random + node_id + inst.random;
}

bool System::Spawn(const InstanceKey& key, const std::string& path, const glm::mat4& world, uint32_t seed) {
    std::shared_ptr<const EffectDef> def = Load(path);
    if (!def || (def->shapes.empty() && def->flares.empty())) {
        return false;
    }
    Instance inst;
    inst.def = def;
    inst.world = world;
    inst.previous_world = world;
    inst.seed = seed ? seed : 0xFFFFFFu;
    inst.random = XorShiftOnce(inst.seed);
    if (inst.random == 0) {
        inst.random = 1;
    }
    inst.shapes.resize(def->shapes.size());
    inst.flares.resize(def->flares.size());
    inst.slot_rng.resize(def->random_slots, 1u);
    inst.slot_batch.resize(def->random_slots, 0.0f);
    for (const Expr& e : def->exprs) {
        if (e.slot >= 0) {
            inst.slot_rng[e.slot] = SeedFor(inst, e.seed, e.seed_type, e.node_id);
        }
    }
    for (size_t i = 0; i < def->shapes.size(); ++i) {
        ShapeState& state = inst.shapes[i];
        state.emits.resize(def->emits.size());
        for (size_t k = 0; k < def->emits.size(); ++k) {
            const EmitNode& e = def->emits[k];
            state.emits[k].rng = SeedFor(inst, e.seed, e.seed_type, e.node_id);
        }
        const LifeDef& life = def->shapes[i].life;
        state.life_rng = SeedFor(inst, life.seed, life.seed_type, life.node_id);
        state.particle_rng = inst.random;
        state.material_rng = SeedFor(inst, 0, 0, def->shapes[i].material.node_id);
    }
    instances_[key] = std::move(inst);
    return true;
}

void System::SetTransform(const InstanceKey& key, const glm::mat4& world) {
    if (auto it = instances_.find(key); it != instances_.end()) {
        it->second.world = world;
    }
}

void System::SnapshotWorlds() {
    for (auto& [key, inst] : instances_) {
        inst.previous_world = inst.world;
    }
}

void System::SetParameter(const InstanceKey& key, uint32_t name, const glm::vec4& value) {
    if (auto it = instances_.find(key); it != instances_.end()) {
        it->second.params[name] = value;
    }
}

void System::SetParameterLow16(const InstanceKey& key, uint32_t low16, const glm::vec4& value) {
    if (auto it = instances_.find(key); it != instances_.end()) {
        auto& list = it->second.params_low16;
        for (auto& [k, v] : list) {
            if (k == low16) {
                v = value;
                return;
            }
        }
        list.emplace_back(low16, value);
    }
}

bool System::IsPlaying(const InstanceKey& key) const {
    const auto it = instances_.find(key);
    if (it == instances_.end()) return false;
    const Instance& instance = it->second;
    if (!instance.finished) return true;
    return std::any_of(instance.shapes.begin(), instance.shapes.end(), [](const ShapeState& shape) { return !shape.particles.empty(); });
}

void System::Remove(const InstanceKey& key) {
    instances_.erase(key);
}

void System::Stop(const InstanceKey& key) {
    const auto it = instances_.find(key);
    if (it == instances_.end()) {
        return;
    }
    Instance& inst = it->second;
    inst.finished = true;
    inst.stopped = true;
    const size_t slots = inst.def->random_slots;
    for (ShapeState& state : inst.shapes) {
        size_t w = 0;
        for (size_t r = 0; r < state.particles.size(); ++r) {
            if (state.particles[r].infinite) {
                continue;
            }
            if (w != r) {
                state.particles[w] = state.particles[r];
                std::copy_n(state.slots.begin() + r * slots, slots, state.slots.begin() + w * slots);
            }
            ++w;
        }
        state.particles.resize(w);
        state.slots.resize(w * slots);
    }
}

bool System::IsStopped(const InstanceKey& key) const {
    const auto it = instances_.find(key);
    return it != instances_.end() && it->second.stopped;
}

void System::RemoveOwner(uint64_t owner) {
    std::erase_if(instances_, [owner](const auto& kv) { return kv.first.owner == owner; });
}

void System::Clear() {
    instances_.clear();
}

size_t System::ParticleCount() const {
    size_t n = 0;
    for (const auto& [key, inst] : instances_) {
        for (const ShapeState& s : inst.shapes) {
            n += s.particles.size();
        }
    }
    return n;
}

std::vector<std::pair<std::string, size_t>> System::Stats() const {
    std::map<std::string, size_t> counts;
    for (const auto& [key, inst] : instances_) {
        size_t n = 0;
        for (const ShapeState& s : inst.shapes) {
            n += s.particles.size();
        }
        counts[inst.def->name] += n;
    }
    std::vector<std::pair<std::string, size_t>> out(counts.begin(), counts.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

void System::InitEmit(EmitState& s, const EmitNode& e) {
    uint32_t life_offset = 0;
    uint32_t life_range = 0;
    if (e.life_range) {
        life_offset = XorShift(s.rng) % (uint32_t(e.life_range) * 2u);
        life_range = e.life_range;
    }
    uint32_t start = e.delay;
    if (e.delay_range) {
        start = (e.delay + XorShift(s.rng) % (uint32_t(e.delay_range) * 2u)) & 0xFFFFu;
    }
    const int32_t end_raw = static_cast<int32_t>(life_offset) - static_cast<int32_t>(life_range) + static_cast<int32_t>(start) + e.life;
    const uint32_t end = end_raw < 0 ? start : static_cast<uint32_t>(end_raw) & 0xFFFFu;
    s.start = static_cast<uint16_t>(start);
    s.end = static_cast<uint16_t>(end);
    const int32_t span = static_cast<int32_t>(end) - static_cast<int32_t>(start);
    if (e.fade_reverse) {
        const uint16_t len = span > 0 ? static_cast<uint16_t>(static_cast<float>(span) * (1.0f - e.fade_position)) : 0;
        s.fade_in_start = s.start;
        s.fade_in_end = static_cast<uint16_t>(s.start + len);
        s.fade_in_len = len;
        s.fade_out_start = s.fade_out_end = s.fade_out_len = 0;
    } else {
        const uint16_t off = span > 0 ? static_cast<uint16_t>(static_cast<float>(span) * e.fade_position) : 0;
        s.fade_in_start = s.fade_in_end = s.fade_in_len = 0;
        s.fade_out_start = static_cast<uint16_t>(off + s.start);
        s.fade_out_end = s.end;
        s.fade_out_len = static_cast<uint16_t>(s.end - s.fade_out_start);
    }
    s.initialized = true;
}

int System::Emit(Instance& inst, ShapeState& state, const EffectDef& def, int32_t index, float dframes, const ViewInfo& view) {
    if (index < 0 || index >= static_cast<int32_t>(def.emits.size())) {
        return 0;
    }
    const EmitNode& e = def.emits[index];
    EmitState& s = state.emits[index];
    switch (e.op) {
    case EmitOp::FirstLoopOnly:
        return inst.loop == 0 ? Emit(inst, state, def, e.input, dframes, view) : 0;
    case EmitOp::Lod: {
        const int n = Emit(inst, state, def, e.input, dframes, view);
        if (n <= 0 || e.lod_distance <= 1e-5f) {
            return n;
        }
        const glm::vec3 d = glm::vec3(inst.world[3]) - view.position;
        float t = std::min(1.0f, glm::dot(d, d) / (e.lod_distance * e.lod_distance));
        if (e.lod_inverse) {
            t = 1.0f - t;
        }
        return n - static_cast<int>(std::floor(static_cast<float>(n) * e.lod_percent) * t);
    }
    case EmitOp::Delay: {
        const float t = inst.frame / kFrameRate;
        const float dt = dframes / kFrameRate;
        if (!s.initialized) {
            s.delay_start = e.delay_s;
            s.initialized = true;
        }
        if (e.once) {
            if (s.loop != inst.loop) {
                s.fired = false;
                s.loop = inst.loop;
            }
            const bool past = def.play_mode == 2 && s.delay_start + e.life_s <= t;
            if (t >= s.delay_start && !s.fired && !past) {
                s.fired = true;
                s.delay_start = e.delay_s + e.delay_range_s * Random01(s.rng);
                return static_cast<int>(e.num);
            }
            return 0;
        }
        if (t >= s.delay_start && t < s.delay_start + e.life_s) {
            s.counter = std::min(s.counter, 1.0f) + dt;
            if (s.counter >= 1.0f / kFrameRate) {
                s.counter = 0.0f;
                return static_cast<int>(e.num);
            }
        }
        return 0;
    }
    case EmitOp::Interval:
        break;
    }
    const uint32_t frame = static_cast<uint32_t>(inst.frame);
    if (frame == 0 || !s.initialized) {
        InitEmit(s, e);
    }
    if (frame < s.start || frame > s.end) {
        s.counter = e.interval;
        return 0;
    }
    float fade = 1.0f;
    if (!e.fade_reverse) {
        if (s.fade_out_len != 0 && s.fade_out_start <= frame && frame <= s.fade_out_end) {
            fade = 1.0f - static_cast<float>(frame - s.fade_out_start) / static_cast<float>(s.fade_out_len);
        }
    } else if (s.fade_in_len != 0 && s.fade_in_start <= frame && frame <= s.fade_in_end) {
        fade = static_cast<float>(frame - s.fade_in_start) / static_cast<float>(s.fade_in_len);
    }
    if (e.interval <= s.counter) {
        s.counter = 0.0f;
        const uint32_t r = XorShift(s.rng);
        if (e.probability <= static_cast<float>(r) * kProbabilityScale) {
            return 0;
        }
        uint32_t n = e.num_max;
        const uint32_t r2 = XorShift(s.rng);
        if (e.num_max != e.num_min) {
            n = r2 % (uint32_t(e.num_max) - e.num_min) + e.num_min;
        }
        return static_cast<int>(fade * static_cast<float>(n));
    }
    s.counter = std::floor(s.counter + dframes);
    return 0;
}

void System::GenerateSlots(Instance& inst, const EffectDef& def, int32_t index, glm::vec4* slots, const Particle& p, uint8_t* done) {
    if (index < 0) {
        return;
    }
    const Expr& e = def.exprs[index];
    if (done[index]) {
        return;
    }
    done[index] = 1;
    for (int32_t in : e.in) {
        GenerateSlots(inst, def, in, slots, p, done);
    }
    if (e.slot < 0) {
        return;
    }
    uint32_t& rng = inst.slot_rng[e.slot];
    glm::vec4& out = slots[e.slot];
    switch (e.op) {
    case ExprOp::Random: {
        if (e.flags & 2u) {
            const float r = Random01(rng);
            out = e.v0 + (e.v1 - e.v0) * r;
        } else {
            const float r0 = Random01(rng);
            const float r1 = Random01(rng);
            const float r2 = Random01(rng);
            const float r3 = Random01(rng);
            out = e.v0 + (e.v1 - e.v0) * glm::vec4(r0, r1, r2, r3);
        }
        break;
    }
    case ExprOp::UvRandom: {
        const uint32_t w = static_cast<uint32_t>(e.v0.x);
        const uint32_t h = static_cast<uint32_t>(e.v0.y);
        const float cw = w ? 1.0f / static_cast<float>(w) : 1.0f;
        const float ch = h ? 1.0f / static_cast<float>(h) : 1.0f;
        const bool flip_u = (e.flags & 1u) != 0;
        const bool flip_v = (e.flags & 2u) != 0;
        const float u = w ? static_cast<float>(XorShift(rng) % w) : 0.0f;
        const float v = h ? static_cast<float>(XorShift(rng) % h) : 0.0f;
        glm::vec4 rect(u * cw, v * ch, cw, ch);
        if (flip_u || flip_v) {
            const uint32_t bit_u = XorShift(rng) & 1u;
            const uint32_t bit_v = XorShift(rng) & 1u;
            if (flip_u && bit_u) {
                rect.x += rect.z;
                rect.z = -rect.z;
            }
            if (flip_v && bit_v) {
                rect.y += rect.w;
                rect.w = -rect.w;
            }
        }
        out = rect;
        break;
    }
    case ExprOp::UvAnime: {
        const uint32_t v = p.random;
        const uint32_t total = std::max(1u, static_cast<uint32_t>(e.v0.y * e.v0.z));
        out = glm::vec4((e.flags & 2u) ? static_cast<float>(v % total) : 0.0f, (e.flags & 4u) ? static_cast<float>(v & 1u) : 0.0f,
                        (e.flags & 8u) ? static_cast<float>((v >> 1) & 1u) : 0.0f, 0.0f);
        break;
    }
    case ExprOp::Spread: {
        float horizontal = 0.0f;
        float vertical = 0.0f;
        float length = 0.0f;
        float phi = 0.0f;
        if ((e.mode & 1u) == 0) {
            const float low = 1.5703f - e.v0.x * 1.5703f;
            const float latitude = low + (1.5703f - low) * Random01(rng);
            phi = e.v0.y * Random01(rng);
            length = e.v0.z + (e.v0.w - e.v0.z) * Random01(rng);
            horizontal = std::cos(latitude);
            vertical = std::sin(latitude);
        } else {
            const float polar = e.v0.x * 1.57f;
            horizontal = std::sin(polar);
            vertical = std::cos(polar);
            if ((e.mode & 2u) == 0) {
                length = e.v0.z + (e.v0.w - e.v0.z) * Random01(rng);
                phi = e.v0.y * Random01(rng);
            } else {
                if (p.index == 0) {
                    inst.slot_batch[e.slot] = e.v0.z + (e.v0.w - e.v0.z) * Random01(rng);
                }
                length = inst.slot_batch[e.slot];
                phi = e.v0.y * static_cast<float>(p.index) / static_cast<float>(std::max(1u, p.count));
            }
        }
        glm::vec3 v = glm::vec3(horizontal * std::sin(phi), vertical, horizontal * std::cos(phi)) * length;
        const glm::quat rotation(e.v1.w, e.v1.x, e.v1.y, e.v1.z);
        if (rotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
            v = rotation * v;
        }
        out = glm::vec4(v, 0.0f);
        break;
    }
    default:
        break;
    }
}

void System::Spawn(Instance& inst, size_t shape_index, int count, const ViewInfo& view) {
    const EffectDef& def = *inst.def;
    const ShapeDef& shape = def.shapes[shape_index];
    ShapeState& state = inst.shapes[shape_index];
    (void)view;
    if (count <= 0) {
        return;
    }
    const size_t limit = shape.max_particles ? std::min<size_t>(shape.max_particles, kMaxParticles) : kMaxParticles;
    std::vector<uint8_t> done_storage(def.exprs.size());
    const size_t before = state.particles.size();
    for (int i = 0; i < count && state.particles.size() < limit; ++i) {
        Particle p;
        p.index = static_cast<uint32_t>(i);
        p.count = static_cast<uint32_t>(count);
        p.life = shape.life.min;
        if (shape.life.max > shape.life.min) {
            p.life = shape.life.min + (shape.life.max - shape.life.min) * Random01(state.life_rng);
        }
        p.life = std::max(p.life, 1.0f / kFrameRate);
        p.infinite = shape.life.infinite;
        p.spawn_world = inst.world;
        if (shape.particle_random) {
            p.random = static_cast<uint16_t>(XorShift(state.particle_rng));
        }
        const size_t base = state.slots.size();
        state.slots.resize(base + def.random_slots, glm::vec4(0.0f));
        std::fill(done_storage.begin(), done_storage.end(), uint8_t(0));
        for (int32_t a : shape.attr) {
            GenerateSlots(inst, def, a, state.slots.data() + base, p, done_storage.data());
        }
        state.particles.push_back(p);
    }
    if (shape.particle_random && state.particles.size() > before) {
        XorShift(state.particle_rng);
    }
}

void System::Update(float dt, const ViewInfo& view) {
    const float dframes = dt * kFrameRate;
    std::erase_if(instances_, [](const auto& kv) {
        return kv.second.stopped && std::all_of(kv.second.shapes.begin(), kv.second.shapes.end(), [](const ShapeState& s) { return s.particles.empty(); });
    });
    const bool cut = !last_view_ || glm::distance(last_view_->position, view.position) > kCameraCut ||
                     glm::dot(last_view_->forward, view.forward) < kCameraCutCos;
    last_view_ = view;
    for (auto& [key, inst] : instances_) {
        const EffectDef& def = *inst.def;
        for (size_t i = 0; i < def.flares.size(); ++i) {
            UpdateFlare(inst, def.flares[i], inst.flares[i], dt, view, cut);
        }
        for (size_t i = 0; i < inst.shapes.size(); ++i) {
            ShapeState& state = inst.shapes[i];
            const MaterialDef& material = def.shapes[i].material;
            if (material.kind == MaterialKind::Scroll) {
                state.rain_offset -= glm::vec2(dt * material.rain_scroll_speed);
                state.rain_offset -= glm::floor(state.rain_offset);
                for (int layer = 0; layer < 2; ++layer) {
                    state.rain_phase[layer] += dt * material.rain_phase_speed *
                        (static_cast<float>(XorShift(state.material_rng)) * 1.1641532e-10f + 0.5f);
                    state.rain_phase[layer] = std::fmod(state.rain_phase[layer], kTwoPi);
                }
            }
            size_t w = 0;
            const size_t slots = def.random_slots;
            for (size_t r = 0; r < state.particles.size(); ++r) {
                Particle& p = state.particles[r];
                if (inst.started) {
                    p.age += dt;
                }
                if (!p.infinite && p.age >= p.life) {
                    continue;
                }
                if (w != r) {
                    state.particles[w] = p;
                    std::copy_n(state.slots.begin() + r * slots, slots, state.slots.begin() + w * slots);
                }
                ++w;
            }
            state.particles.resize(w);
            state.slots.resize(w * slots);
        }
        if (inst.started) {
            inst.frame += dframes;
            if (def.play_mode == 2) {
                if (inst.frame > static_cast<float>(def.fade_out_start)) {
                    inst.frame = static_cast<float>(def.fade_in_end);
                    ++inst.loop;
                }
            } else if (inst.frame >= static_cast<float>(def.all_frame)) {
                if (def.play_mode == 0) {
                    inst.finished = true;
                    inst.frame = static_cast<float>(def.all_frame);
                } else {
                    inst.frame = std::fmod(inst.frame, static_cast<float>(def.all_frame));
                    ++inst.loop;
                }
            }
        }
        inst.started = true;
        if (inst.finished) {
            continue;
        }
        for (size_t i = 0; i < inst.shapes.size(); ++i) {
            const int n = Emit(inst, inst.shapes[i], def, def.shapes[i].emit, dframes, view);
            Spawn(inst, i, n, view);
        }
    }
}

glm::vec4 System::Receive(const Instance& inst, uint32_t name, const glm::vec4& fallback) const {
    if (auto it = inst.params.find(name); it != inst.params.end()) {
        return it->second;
    }
    for (const auto& [low, value] : inst.params_low16) {
        if (low == (name & 0xFFFFu)) {
            return value;
        }
    }
    return fallback;
}

glm::vec4 System::Eval(const EvalContext& c, int32_t index) const {
    if (index < 0) {
        return glm::vec4(0.0f);
    }
    const Expr& e = c.def->exprs[index];
    switch (e.op) {
    case ExprOp::Zero:
        return glm::vec4(0.0f);
    case ExprOp::Const:
    case ExprOp::UvMap:
        return e.v0;
    case ExprOp::Random:
    case ExprOp::UvRandom:
    case ExprOp::Spread:
        return e.slot >= 0 ? c.slots[e.slot] : glm::vec4(0.0f);
    case ExprOp::Composition: {
        const glm::vec4 a = Eval(c, e.in[0]);
        const glm::vec4 b = Eval(c, e.in[1]);
        return a + glm::mix(b, e.v1, e.v0);
    }
    case ExprOp::Multiply: {
        const glm::vec4 a = Eval(c, e.in[0]);
        const glm::vec4 b = Eval(c, e.in[1]);
        return a * glm::mix(b, e.v1, e.v0);
    }
    case ExprOp::UniformVelocity:
        return Eval(c, e.in[0]) * c.age;
    case ExprOp::UniformAccel:
        return Eval(c, e.in[0]) * c.age + Eval(c, e.in[1]) * (c.age * c.age);
    case ExprOp::UniformVelocityTime:
        return Eval(c, e.in[0]) * Eval(c, e.in[1]);
    case ExprOp::DragTime: {
        const float drag = e.v0.x;
        const float scale = e.v0.y;
        const bool plain = drag == 1.0f && scale == 1.0f;
        float v = c.age;
        if (e.mode == 0) {
            v = plain ? c.ratio : scale * (1.0f - std::pow(std::max(0.0f, 1.0f - c.ratio), drag));
        } else if (e.mode == 1) {
            v = plain ? c.age : scale * (1.0f - std::pow(std::max(0.0f, 1.0f - c.ratio), drag)) * c.life;
        } else if (e.mode == 2) {
            const float length = c.def->Length();
            v = plain ? c.effect_time : scale * (1.0f - std::pow(std::max(0.0f, 1.0f - c.effect_ratio), drag)) * length;
        }
        return glm::vec4(v);
    }
    case ExprOp::TimeScale: {
        const float s = e.v1.x + (e.v1.y - e.v1.x) * c.ratio;
        const glm::vec4 in = Eval(c, e.in[0]);
        return (e.v0 + (glm::vec4(1.0f) - e.v0) * s) * in;
    }
    case ExprOp::Keyframe: {
        const glm::vec4 in = e.in[0] >= 0 ? Eval(c, e.in[0]) : glm::vec4(0.0f);
        float t = c.age;
        if (e.time == TimeSource::LifeRatio) {
            t = c.ratio;
        } else if (e.time == TimeSource::EffectTime) {
            t = c.effect_time;
        } else if (e.time == TimeSource::EffectRatio) {
            t = c.effect_ratio;
        }
        glm::vec4 out(0.0f);
        for (int k = 0; k < 4; ++k) {
            const Curve& curve = e.curves[k];
            switch (e.mode) {
            case 0:
                out[k] = in[k] + curve.Eval(t);
                break;
            case 1:
                out[k] = in[k] - curve.Eval(t);
                break;
            case 2:
                out[k] = in[k] * curve.Eval(t);
                break;
            default:
                out[k] = curve.Eval(in[k]);
                break;
            }
        }
        return out;
    }
    case ExprOp::Oscillate: {
        const glm::vec4 a = Eval(c, e.in[0]);
        const glm::vec4 b = Eval(c, e.in[1]);
        const glm::vec4 x = a * b * kTwoPi;
        return glm::vec4(std::sin(x.x), std::sin(x.y), std::sin(x.z), std::sin(x.w));
    }
    case ExprOp::UvAnime: {
        const glm::vec4 slot = e.slot >= 0 ? c.slots[e.slot] : glm::vec4(0.0f);
        const uint32_t w = std::max(1u, static_cast<uint32_t>(e.v0.y));
        const uint32_t h = std::max(1u, static_cast<uint32_t>(e.v0.z));
        const uint32_t total = w * h;
        uint32_t frame = static_cast<uint32_t>(std::max(0.0f, e.v0.x * c.age)) + static_cast<uint32_t>(slot.x);
        frame = (e.flags & 1u) ? std::min(frame, total - 1) : frame % total;
        glm::vec4 rect(static_cast<float>(frame % w) / static_cast<float>(w), static_cast<float>(frame / w) / static_cast<float>(h),
                       1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h));
        if (((e.flags & 16u) != 0) != (slot.y > 0.5f)) {
            rect.x += rect.z;
            rect.z = -rect.z;
        }
        if (((e.flags & 32u) != 0) != (slot.z > 0.5f)) {
            rect.y += rect.w;
            rect.w = -rect.w;
        }
        return rect;
    }
    case ExprOp::CameraCorrection: {
        const glm::vec4 in = Eval(c, e.in[0]);
        const bool world = e.in[0] >= 0 && c.def->exprs[e.in[0]].world;
        const glm::vec3 origin = e.mode == 1 ? (world ? glm::vec3(in) : TransformPoint(c.world, glm::vec3(in))) : glm::vec3(c.world[3]);
        glm::vec3 dir = c.view->position - origin;
        const float len = glm::length(dir);
        if (len < 1e-5f) {
            return in;
        }
        dir /= len;
        if (!world) {
            dir = glm::transpose(glm::mat3(c.world)) * dir;
            const float l2 = glm::length(dir);
            dir = l2 > 1e-6f ? dir / l2 : dir;
        }
        return in + glm::vec4(dir * e.v0.x, 0.0f);
    }
    case ExprOp::CameraFollow: {
        const glm::vec3 q = glm::vec3(Eval(c, e.in[0]) + e.v0);
        glm::vec3 forward = c.view->forward;
        glm::vec3 up = c.view->up;
        glm::vec3 right = c.view->right;
        if (e.flags & 1u) {
            forward.y = 0.0f;
            forward = glm::length(forward) > 1e-5f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
            up = glm::vec3(0.0f, 1.0f, 0.0f);
            right = glm::normalize(glm::cross(forward, up));
        }
        return glm::vec4(c.view->position + right * q.x + up * q.y + forward * q.z, 0.0f);
    }
    case ExprOp::CenterScroll: {
        const glm::vec3 p = glm::vec3(Eval(c, e.in[0]));
        const glm::vec3 center = c.view->position + c.view->right * e.v1.x + c.view->up * e.v1.y + c.view->forward * e.v1.z;
        const glm::vec3 range = glm::max(glm::vec3(e.v0), glm::vec3(1e-3f));
        glm::vec3 d = p - center + range;
        d = d - glm::floor(d / (2.0f * range)) * (2.0f * range) - range;
        return glm::vec4(center + d, 0.0f);
    }
    case ExprOp::CenterDistRate: {
        const glm::vec4 in = e.in[0] >= 0 ? Eval(c, e.in[0]) : glm::vec4(1.0f);
        const glm::vec3 center = c.view->position + c.view->right * e.v3.x + c.view->up * e.v3.y + c.view->forward * e.v3.z;
        const glm::vec3 d = glm::abs(c.position - center);
        float t = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float span = e.v1[k] - e.v0[k];
            t = std::max(t, span > 1e-5f ? Saturate((d[k] - e.v0[k]) / span) : (d[k] > e.v0[k] ? 1.0f : 0.0f));
        }
        return in * (e.v2.x + (e.v2.y - e.v2.x) * t);
    }
    case ExprOp::CameraAngle: {
        glm::vec3 axis(0.0f);
        axis[std::min<int>(e.mode, 2)] = 1.0f;
        if (e.flags & 1u) {
            axis = glm::normalize(glm::mat3(c.world) * axis);
        }
        return glm::vec4(std::acos(std::clamp(-glm::dot(c.view->forward, axis), -1.0f, 1.0f)));
    }
    case ExprOp::InterpolateLine: {
        const float t = c.particle && c.particle->count ? static_cast<float>(c.particle->index) / static_cast<float>(c.particle->count) : 0.0f;
        return e.v0 + (e.v1 - e.v0) * t;
    }
    case ExprOp::Receive:
        return Receive(*c.inst, e.receive, e.v0) * e.v1;
    case ExprOp::Pass:
        return Eval(c, e.in[0]);
    case ExprOp::Wind: {
        glm::mat3 r(c.world);
        for (int i = 0; i < 3; ++i) {
            const float l = glm::length(r[i]);
            r[i] = l > 1e-6f ? r[i] / l : r[i];
        }
        return glm::vec4(glm::transpose(r) * wind_ * e.v0.x, 0.0f);
    }
    }
    return glm::vec4(0.0f);
}

void System::Build(const ViewInfo& view, const TextureResolver& textures, const TextureResolver& cubes, const LightingProbe& lighting,
                   RenderList& out, std::vector<LightOut>& lights, float blend) {
    out.near_plane = view.near_plane;
    for (auto& [key, inst] : instances_) {
        const EffectDef& def = *inst.def;
        if (!filter_.empty() && def.name.find(filter_) == std::string::npos) {
            continue;
        }
        const glm::mat4 world = BlendTransform(inst.previous_world, inst.world, blend);
        for (size_t i = 0; i < def.shapes.size(); ++i) {
            const size_t first = lights.size();
            BuildShape(inst, world, def.shapes[i], inst.shapes[i], view, textures, cubes, lighting, out, lights);
            for (size_t k = first; k < lights.size(); ++k) {
                lights[k].id = (key.owner * 0x9E3779B97F4A7C15ull) ^ (key.id << 12) ^ (uint64_t(i) << 6) ^ (k - first) ^ 0xFA00000000000000ull;
            }
        }
        for (size_t i = 0; i < def.flares.size() && !inst.stopped; ++i) {
            flare_effect_ = &def.name;
            BuildFlare(world, def.flares[i], inst.flares[i], view, textures, out);
        }
    }
}

void System::BuildShape(const Instance& inst, const glm::mat4& world, const ShapeDef& shape, const ShapeState& state, const ViewInfo& view,
                        const TextureResolver& textures, const TextureResolver& cubes, const LightingProbe& lighting, RenderList& out,
                        std::vector<LightOut>& lights) const {
    if (state.particles.empty()) {
        return;
    }
    const ModelMesh* mesh = nullptr;
    if (shape.kind == ShapeKind::Model) {
        const auto it = models_.find(shape.model);
        if (it == models_.end() || !it->second || it->second->indices.empty()) {
            return;
        }
        mesh = it->second.get();
    }
    const EffectDef& def = *inst.def;
    EvalContext c;
    c.def = &def;
    c.inst = &inst;
    c.view = &view;
    c.effect_time = inst.frame / kFrameRate;
    c.effect_ratio = Saturate(inst.frame / static_cast<float>(def.all_frame));
    const MaterialDef& m = shape.material;
    const size_t slots = def.random_slots;
    if (shape.kind == ShapeKind::SpotLight || shape.kind == ShapeKind::PointLight) {
        for (size_t pi = 0; pi < state.particles.size(); ++pi) {
            const Particle& p = state.particles[pi];
            c.particle = &p;
            c.slots = slots ? &state.slots[pi * slots] : nullptr;
            c.life = p.life;
            c.age = p.infinite ? std::fmod(p.age, p.life) : p.age;
            c.ratio = Saturate(c.age / p.life);
            c.world = shape.local_space ? world : p.spawn_world;
            LightOut l;
            l.spot = shape.kind == ShapeKind::SpotLight;
            const glm::vec3 pos = glm::vec3(Eval(c, shape.attr[kPosition]));
            l.position = shape.world_position ? pos : TransformPoint(c.world, pos);
            const glm::vec4 color = shape.attr[kColor] >= 0 ? Eval(c, shape.attr[kColor]) : glm::vec4(1.0f);
            l.color = glm::vec3(color);
            l.inner_range = shape.inner_range;
            l.outer_range = std::max(shape.outer_range, 0.01f);
            l.attenuation = shape.attenuation;
            l.shadow = shape.cast_shadow;
            l.specular = shape.specular;
            l.view_bias = shape.view_bias;
            l.shadow_bias = shape.shadow_bias;
            if (l.spot) {
                const glm::vec3 rot = glm::vec3(Eval(c, shape.attr[kRotation]));
                const glm::mat3 frame = glm::mat3(c.world) * EulerYxz(rot);
                l.direction = glm::normalize(frame * glm::vec3(0.0f, -1.0f, 0.0f));
                l.up = frame * glm::vec3(0.0f, 0.0f, 1.0f);
                const glm::vec4 cone = shape.attr[kExtra] >= 0 ? Eval(c, shape.attr[kExtra]) : glm::vec4(60.0f, 30.0f, 1.0f, 1.0f);
                l.umbra = cone.x;
                l.penumbra = cone.y;
                l.shadow_umbra = cone.x * shape.shadow_umbra_scale;
                l.shadow_penumbra = cone.y * shape.shadow_penumbra_scale;
                l.lumen = color.w;
                if (!shape.light_mask.empty()) {
                    l.mask = static_cast<int32_t>(textures(shape.light_mask));
                }
            } else {
                const glm::vec4 extra = shape.attr[kExtra] >= 0 ? Eval(c, shape.attr[kExtra]) : glm::vec4(6500.0f, 0.0f, 1.0f, 0.0f);
                l.temperature = extra.x > 0.0f ? extra.x : 6500.0f;
                l.lumen = extra.z * color.w;
                if (shape.attr[kScale] >= 0) {
                    const glm::vec4 range = Eval(c, shape.attr[kScale]);
                    l.inner_range = range.x;
                    l.outer_range = std::max(range.y, 0.01f);
                }
            }
            if (shape.light_area) {
                const glm::mat3 frame(c.world);
                const glm::vec3 lengths(glm::length(frame[0]), glm::length(frame[1]), glm::length(frame[2]));
                if (lengths.x > 1.0e-6f && lengths.y > 1.0e-6f && lengths.z > 1.0e-6f) {
                    const glm::mat3 axes(frame[0] / lengths.x, frame[1] / lengths.y, frame[2] / lengths.z);
                    const glm::vec4& q = shape.area_rotation;
                    const glm::mat3 rotation = axes * glm::mat3_cast(glm::normalize(glm::quat(q.w, q.x, q.y, q.z)));
                    const glm::vec3 half = 0.5f * shape.area_scale * lengths;
                    l.area = true;
                    l.area_world = glm::mat4(glm::vec4(rotation[0] * half.x, 0.0f), glm::vec4(rotation[1] * half.y, 0.0f),
                                             glm::vec4(rotation[2] * half.z, 0.0f), glm::vec4(l.position + frame * shape.area_translation, 1.0f));
                }
            }
            if (l.lumen > 0.0f) {
                lights.push_back(l);
            }
        }
        return;
    }
    const uint32_t texture = textures ? textures(m.texture) : 0;
    const bool reflects = m.kind == MaterialKind::Liquid && m.reflection > 0.0f && cubes;
    const uint32_t cube = reflects ? cubes(m.reflection_texture) : kNoTexture;
    Draw draw;
    draw.layer = shape.kind == ShapeKind::Sprite2D ? Layer::Screen : Layer::World;
    draw.blend = m.blend;
    draw.first = static_cast<uint32_t>(out.quads.size());
    draw.priority = static_cast<int32_t>(shape.priority);
    draw.cull = mesh && shape.cull_face;
    const glm::vec3 origin = glm::vec3(world[3]);
    glm::vec3 ambient(1.0f);
    bool ambient_ready = false;
    draw.depth = glm::dot(origin - view.position, view.forward) - shape.sort_offset;
    uint32_t flags = 0;
    glm::vec4 luminance(0.0f);
    switch (m.kind) {
    case MaterialKind::DynamicLuminance:
        flags |= kQuadLuminance;
        luminance = glm::vec4(m.min_ev, m.max_ev, m.lum_min, m.lum_max);
        break;
    case MaterialKind::Lit:
        flags |= kQuadExposure;
        break;
    case MaterialKind::Scroll:
        flags |= kQuadExposure | kQuadRain;
        break;
    case MaterialKind::Liquid:
        flags |= kQuadLiquid | (m.liquid_hnm ? kQuadLiquidHnm : 0u) | (m.opaque ? kQuadClip : 0u);
        luminance = glm::vec4(m.transparency, 0.0f, m.fresnel_base, m.fresnel_power);
        draw.refract = draw.layer == Layer::World;
        draw.opaque = m.opaque;
        break;
    case MaterialKind::Unlit:
        if (shape.kind != ShapeKind::Sprite2D) {
            flags |= kQuadExposure;
        }
        break;
    }
    if (m.soft && m.soft_factor > 0.0f) {
        flags |= kQuadSoft;
    }
    if (shape.kind == ShapeKind::Sprite2D) {
        flags |= kQuadScreen;
    }
    if (m.anime_blend) {
        flags |= kQuadAnimeBlend;
    }
    if (mesh) {
        flags |= kQuadTriangle;
    }
    using Item = BuildItem;
    std::vector<Item>& items = build_items_;
    items.clear();
    using LitRef = BuildLitRef;
    std::vector<LitRef>& lit_refs = build_lit_refs_;
    lit_refs.clear();
    const bool block_lit = m.kind == MaterialKind::Lit && static_cast<bool>(light_block_);
    items.reserve(state.particles.size());
    for (size_t pi = 0; pi < state.particles.size(); ++pi) {
        const Particle& p = state.particles[pi];
        c.particle = &p;
        c.slots = slots ? &state.slots[pi * slots] : nullptr;
        c.life = p.life;
        c.age = p.infinite ? std::fmod(p.age, p.life) : p.age;
        c.ratio = Saturate(c.age / p.life);
        c.world = shape.local_space ? world : p.spawn_world;
        const glm::vec3 local = glm::vec3(Eval(c, shape.attr[kPosition]));
        glm::vec3 world_pos = shape.world_position ? local : TransformPoint(c.world, local);
        c.position = world_pos;
        const glm::vec4 scale = shape.attr[kScale] >= 0 ? Eval(c, shape.attr[kScale]) : glm::vec4(1.0f);
        const glm::vec4 rot = shape.attr[kRotation] >= 0 ? Eval(c, shape.attr[kRotation]) : glm::vec4(0.0f);
        const glm::vec4 uv = shape.attr[kUv] >= 0 ? Eval(c, shape.attr[kUv]) : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        glm::vec4 color = shape.attr[kColor] >= 0 ? Eval(c, shape.attr[kColor]) : glm::vec4(1.0f);
        if (mesh) {
            color = WrapUnorm8(color);
        } else {
            color.a = Saturate(color.a);
        }
        if (color.a <= 0.0f && m.blend != BlendMode::Opaque) {
            continue;
        }
        Quad q;
        const float x0 = -shape.center_u;
        const float x1 = 1.0f - shape.center_u;
        const float y0 = shape.center_v - 1.0f;
        const float y1 = shape.center_v;
        if (shape.kind == ShapeKind::Sprite2D) {
            const float sw = static_cast<float>(shape.screen_width);
            const float sh = static_cast<float>(shape.screen_height);
            const glm::vec2 center(local.x, local.y);
            const glm::vec2 size(scale.x, scale.y);
            const glm::vec2 lo = center + glm::vec2(x0, -y1) * size;
            const glm::vec2 hi = center + glm::vec2(x1, -y0) * size;
            auto ndc = [&](float px, float py) { return glm::vec4(px / sw * 2.0f - 1.0f, py / sh * 2.0f - 1.0f, 0.0f, 1.0f); };
            q.corner[0] = ndc(lo.x, lo.y);
            q.corner[1] = ndc(hi.x, lo.y);
            q.corner[2] = ndc(lo.x, hi.y);
            q.corner[3] = ndc(hi.x, hi.y);
        } else if (!mesh) {
            glm::vec3 ax;
            glm::vec3 ay;
            glm::vec2 size(scale.x, scale.y);
            if (shape.kind == ShapeKind::Plane) {
                const glm::mat3 frame = shape.world_position ? glm::mat3(1.0f) : glm::mat3(c.world);
                glm::mat3 fix(1.0f);
                if (shape.axis_fix >= 1 && shape.axis_fix <= 3) {
                    const glm::vec3 d = glm::inverse(frame) * (view.position - world_pos);
                    fix = PlaneAxisFix(shape.axis_fix, d);
                }
                const glm::mat3 basis = frame * fix * glm::mat3(EulerZyx(glm::vec3(rot)));
                ax = basis[0];
                ay = basis[1];
                size *= shape.base_size;
            } else {
                glm::vec3 right = view.right;
                glm::vec3 up = view.up;
                if (shape.kind == ShapeKind::SpriteRot && rot.z != 0.0f) {
                    const float cs = std::cos(rot.z);
                    const float sn = std::sin(rot.z);
                    const glm::vec3 r2 = right * cs + up * sn;
                    const glm::vec3 u2 = up * cs - right * sn;
                    right = r2;
                    up = u2;
                }
                ax = right;
                ay = up;
                if (!shape.world_position) {
                    const glm::vec3 s = glm::vec3(glm::length(glm::vec3(c.world[0])), glm::length(glm::vec3(c.world[1])), 1.0f);
                    size *= glm::vec2(s.x, s.y);
                }
            }
            if (m.camera_z_offset != 0.0f) {
                const glm::vec3 d = world_pos - view.position;
                const float len = glm::length(d);
                if (len > 1e-5f) {
                    world_pos += d / len * m.camera_z_offset;
                }
            }
            const glm::vec3 px0 = ax * (x0 * size.x);
            const glm::vec3 px1 = ax * (x1 * size.x);
            const glm::vec3 py0 = ay * (y0 * size.y);
            const glm::vec3 py1 = ay * (y1 * size.y);
            q.corner[0] = glm::vec4(world_pos + px0 + py1, 1.0f);
            q.corner[1] = glm::vec4(world_pos + px1 + py1, 1.0f);
            q.corner[2] = glm::vec4(world_pos + px0 + py0, 1.0f);
            q.corner[3] = glm::vec4(world_pos + px1 + py0, 1.0f);
        }
        q.uv = glm::vec4(uv.x, uv.y, uv.x + uv.z, uv.y + uv.w);
        q.uv_next = q.uv;
        float blend_weight = 0.0f;
        if (m.anime_blend) {
            const uint32_t total = m.anime_w * m.anime_h;
            const float f = std::max(0.0f, c.age * m.anime_fps);
            const uint32_t f0 = static_cast<uint32_t>(f) % total;
            const uint32_t f1 = (f0 + 1) % total;
            blend_weight = f - std::floor(f);
            auto cell = [&](uint32_t k) {
                const float cw = 1.0f / static_cast<float>(m.anime_w);
                const float ch = 1.0f / static_cast<float>(m.anime_h);
                const float u0 = static_cast<float>(k % m.anime_w) * cw;
                const float v0 = static_cast<float>(k / m.anime_w) * ch;
                return glm::vec4(u0, v0, u0 + cw, v0 + ch);
            };
            q.uv = cell(f0);
            q.uv_next = cell(f1);
        }
        glm::vec3 rgb = glm::vec3(color);
        glm::vec3 unlit(0.0f);
        glm::vec4 quad_luminance = luminance;
        if (block_lit) {
            lit_refs.push_back({items.size(), items.size(), rgb, world_pos});
        } else if (m.kind == MaterialKind::Lit || m.kind == MaterialKind::Liquid) {
            LightingSample light{glm::vec3(1.0f), glm::vec3(0.0f)};
            if (lighting) {
                if (!ambient_ready) {
                    ambient = lighting(origin, true).ambient;
                    ambient_ready = true;
                }
                light = lighting(world_pos, false);
                light.ambient = ambient;
            }
            if (m.kind == MaterialKind::Liquid) {
                quad_luminance.y = glm::dot(rgb, glm::vec3(0.2126f, 0.7152f, 0.0722f));
                unlit = rgb;
                rgb *= light.ambient * m.ambient_rate + light.point * (m.point_rate * (1.0f - m.transparency));
            } else {
                rgb *= light.ambient * m.ambient_rate + light.directional * m.directional_rate + light.point * m.point_rate;
            }
            static const bool debug = std::getenv("PT_VFX_DEBUG") != nullptr;
            if (debug && pi == 0) {
                LogDebug("vfx: {} lit at ({:.2f} {:.2f} {:.2f}) ambient ({:.3f} {:.3f} {:.3f}) point ({:.3f} {:.3f} {:.3f}) alpha {:.3f}", def.name,
                         world_pos.x, world_pos.y, world_pos.z, light.ambient.r, light.ambient.g, light.ambient.b, light.point.r, light.point.g,
                         light.point.b, color.a);
            }
        } else if (m.kind == MaterialKind::Scroll) {
            rgb *= m.luminance;
        }
        q.color = glm::vec4(rgb, color.a);
        q.params = glm::vec4((flags & kQuadSoft) ? 1.0f / m.soft_factor : 0.0f, m.fade_near, m.fade_far, blend_weight);
        q.luminance = quad_luminance;
        q.extra = glm::vec4(m.reflection, m.kind == MaterialKind::Liquid && !m.opaque ? 2.0f * m.refraction : m.refraction, m.roughness, unlit.b);
        q.info = glm::uvec4(texture, flags, cube, glm::packHalf2x16(glm::vec2(unlit.r, unlit.g)));
        if (m.kind == MaterialKind::Scroll) {
            q.info.z = m.rain_texture.empty() ? kNoTexture : textures(m.rain_texture);
            const glm::vec2 angle = m.rain_angle + m.rain_swing * glm::sin(state.rain_phase);
            q.rain_rotation = glm::vec4(std::sin(angle.x), std::cos(angle.x), std::sin(angle.y), std::cos(angle.y));
            q.luminance = glm::vec4(m.rain_scale, state.rain_offset);
            q.extra = glm::vec4(m.texture_scale * 0.5f, std::sin(m.texture_angle), std::cos(m.texture_angle));
        }
        const float depth = glm::dot(world_pos - view.position, view.forward);
        if (mesh) {
            glm::mat4 base = c.world;
            base[3] = glm::vec4(world_pos, 1.0f);
            const glm::mat4 xf = base * EulerZyx(glm::vec3(rot)) * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
            auto vertex = [&](uint32_t i) {
                glm::vec3 p = glm::vec3(xf * glm::vec4(mesh->positions[i], 1.0f));
                if (m.camera_z_offset != 0.0f) {
                    const glm::vec3 d = p - view.position;
                    const float len = glm::length(d);
                    p += len > 1e-5f ? d / len * m.camera_z_offset : glm::vec3(0.0f);
                }
                return glm::vec4(p, 1.0f);
            };
            auto texcoord = [&](uint32_t i) {
                const glm::vec2 t = i < mesh->uvs.size() ? mesh->uvs[i] : glm::vec2(0.0f);
                return shape.model_uv ? glm::vec2(uv) + glm::vec2(uv.z, uv.w) * t : t;
            };
            const size_t second = shape.invert_face ? 2 : 1;
            const size_t third = shape.invert_face ? 1 : 2;
            for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
                const uint32_t i0 = mesh->indices[t];
                const uint32_t i1 = mesh->indices[t + second];
                const uint32_t i2 = mesh->indices[t + third];
                q.corner[0] = vertex(i0);
                q.corner[2] = vertex(i1);
                q.corner[1] = vertex(i2);
                q.corner[3] = q.corner[2];
                q.uv = glm::vec4(texcoord(i0), texcoord(i2));
                q.uv_next = glm::vec4(texcoord(i1), texcoord(i1));
                items.push_back({depth, q});
            }
            if (block_lit) {
                lit_refs.back().last = items.size();
            }
            continue;
        }
        items.push_back({depth, q});
        if (block_lit) {
            lit_refs.back().last = items.size();
        }
    }
    if (items.empty()) {
        return;
    }
    if (block_lit && !lit_refs.empty()) {
        auto box_block = [&](size_t first, size_t last) {
            glm::vec3 lo(1e30f);
            glm::vec3 hi(-1e30f);
            for (size_t k = first; k < last; ++k) {
                for (const glm::vec4& corner : items[k].quad.corner) {
                    lo = glm::min(lo, glm::vec3(corner));
                    hi = glm::max(hi, glm::vec3(corner));
                }
            }
            return light_block_((lo + hi) * 0.5f, (hi - lo) * 0.5f);
        };
        const LightBlock shared = shape.separate_draw ? LightBlock{} : box_block(lit_refs.front().first, lit_refs.back().last);
        static const bool debug = std::getenv("PT_VFX_DEBUG") != nullptr;
        for (const LitRef& r : lit_refs) {
            const LightBlock block = shape.separate_draw ? box_block(r.first, r.last) : shared;
            if (debug && &r - lit_refs.data() < 4) {
                LogDebug("vfx: {} light block at ({:.2f} {:.2f} {:.2f}): sky ({:.3f} {:.3f} {:.3f}), {} lights: ({:.2f} {:.2f} {:.2f}) "
                         "({:.0f} {:.0f} {:.0f}), ({:.2f} {:.2f} {:.2f}) ({:.0f} {:.0f} {:.0f}), ({:.2f} {:.2f} {:.2f}) ({:.0f} {:.0f} {:.0f})",
                         def.name, r.position.x, r.position.y, r.position.z, block.sky.r, block.sky.g, block.sky.b, block.count,
                         block.position[0].x, block.position[0].y, block.position[0].z, block.color[0].r, block.color[0].g,
                         block.color[0].b, block.position[1].x, block.position[1].y, block.position[1].z, block.color[1].r,
                         block.color[1].g, block.color[1].b, block.position[2].x, block.position[2].y, block.position[2].z,
                         block.color[2].r, block.color[2].g, block.color[2].b);
            }
            glm::vec3 point(0.0f);
            for (uint32_t i = 0; i < block.count; ++i) {
                const float d = std::max(0.01f, glm::length(block.position[i] - r.position));
                point += block.color[i] * std::max(0.0f, 1.0f / (d * d) - block.inv_r4[i] * d * d);
            }
            const glm::vec3 rgb = r.unlit * (block.sky * m.ambient_rate + block.directional * m.directional_rate + point * m.point_rate);
            for (size_t k = r.first; k < r.last; ++k) {
                items[k].quad.color = glm::vec4(rgb, items[k].quad.color.a);
            }
        }
    }
    if ((shape.sort_mode != 0 || shape.separate_draw) && draw.blend == BlendMode::Alpha) {
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.depth > b.depth; });
    }
    for (const Item& it : items) {
        out.quads.push_back(it.quad);
    }
    draw.count = static_cast<uint32_t>(out.quads.size()) - draw.first;
    out.draws.push_back(draw);
}

System::FlareLight System::Light(const glm::mat4& world, const FlareDef& flare, const ViewInfo& view) const {
    FlareLight l;
    l.position = TransformPoint(world, glm::vec3(flare.offset));
    const glm::vec3 to_camera = view.position - l.position;
    const float length2 = glm::dot(to_camera, to_camera);
    l.distance = std::sqrt(length2);
    const glm::vec3 dir = length2 >= 1e-6f ? to_camera / l.distance : -view.forward;
    const glm::vec3 axis = glm::vec3(world[2]);
    const float axis_length = glm::length(axis);
    const float facing = axis_length > 1e-6f ? glm::dot(dir, axis / axis_length) : 1.0f;
    const float cos_half = std::cos(flare.angle * 0.5f * kDegToRad);
    l.cone = facing <= cos_half ? 0.0f : (facing - cos_half) / (1.0f - cos_half);
    const float span = flare.base_distance - flare.limit_distance;
    const float t = std::fabs(span) > 1e-6f ? std::max(0.0f, (l.distance - flare.limit_distance) / span) : (l.distance <= flare.limit_distance ? 1.0f : 0.0f);
    l.fade = std::min(1.0f, t * t);
    if (l.distance <= 0.001f) {
        l.fade = 0.0f;
        l.distance = 0.001f;
    } else {
        l.ratio = flare.base_distance / l.distance;
    }
    l.depth = glm::dot(view.forward, l.position - view.position);
    return l;
}

void System::UpdateFlare(const Instance& inst, const FlareDef& flare, FlareState& s, float dt, const ViewInfo& view, bool cut) {
    const LensFlareDef* def = LoadFlare(flare.lens_flare);
    if (!def) {
        return;
    }
    const FlareLight light = Light(inst.world, flare, view);
    if (cut) {
        s.visible = light.depth >= 0.0f && (!def->collision_check || light.cone > 0.0f);
        s.shield_time = s.visible_time = 100.0f;
        s.check = def->collision_check ? kFlareRayCheck : kFlareCheck;
        return;
    }
    auto shield = [&s] {
        if (s.visible) {
            s.visible = false;
            s.shield_time = 0.0f;
        }
    };
    if (light.depth < 0.0f) {
        shield();
    }
    if (s.visible) {
        s.visible_time += dt;
    } else {
        s.shield_time += dt;
    }
    s.check -= dt;
    if (s.check < 0.0f) {
        s.check = def->collision_check ? kFlareRayCheck : kFlareCheck;
        const bool clear = light.depth >= 0.0f && (!def->collision_check || light.cone > 0.0f);
        if (clear && !s.visible) {
            s.visible = true;
            s.visible_time = 0.0f;
        } else if (!clear && def->collision_check) {
            shield();
        }
    }
}

void System::BuildFlare(const glm::mat4& world, const FlareDef& flare, const FlareState& s, const ViewInfo& view, const TextureResolver& textures,
                        RenderList& out) {
    const LensFlareDef* def = LoadFlare(flare.lens_flare);
    if (!def) {
        return;
    }
    const FlareLight light = Light(world, flare, view);
    if (light.cone <= 0.0f || light.depth <= 0.0f) {
        return;
    }
    const glm::vec4 clip = view.view_projection * glm::vec4(light.position, 1.0f);
    const glm::vec2 ndc = std::fabs(clip.w) > 1e-6f ? glm::vec2(clip.x, -clip.y) / clip.w : glm::vec2(0.0f);
    const bool on_screen = std::fabs(ndc.x) <= 1.0f && std::fabs(ndc.y) <= 1.0f;
    glm::vec4 params(Unorm8(0.5f), Unorm8(0.5f), 0.0f, std::max(flare.depth_tolerance, 0.001f));
    if (on_screen) {
        params = glm::vec4(Unorm8(ndc.x * 0.5f + 0.5f), Unorm8(ndc.y * -0.5f + 0.5f), HalfTrunc(clip.w), params.w);
    }
    glm::vec4 root(0.0f);
    if (def->exposure_blend > 0.0f) {
        const glm::vec3 c = light_math::ColorFromTemperature(flare.temperature, 0.0f, flare.lux, glm::vec3(1.0f)) * kFlareLux;
        root = glm::vec4(c.r, glm::length(c), def->exposure_blend, 0.0f);
    }
    const float inv_aspect = 1.0f / view.aspect;
    Draw draw;
    draw.layer = Layer::Flare;
    draw.blend = BlendMode::Add;
    draw.first = static_cast<uint32_t>(out.quads.size());
    for (const LensFlareElement& el : def->elements) {
        if (!s.visible && s.shield_time >= el.fade_out) {
            continue;
        }
        static const char* ghost_env = std::getenv("PT_FLARE_GHOSTS");
        const float ghost_scale = ghost_env ? static_cast<float>(std::atof(ghost_env)) : g_flare_ghost_scale.load();
        const bool full_ghost = el.offset_type == 2 && el.offset_scale < 0.0f && el.width >= 2.0f;
        if (full_ghost && ghost_scale <= 0.0f) {
            continue;
        }
        glm::vec2 pos(0.0f);
        switch (el.offset_type) {
        case 0: pos = el.base_offset; break;
        case 1: pos = ndc + el.base_offset; break;
        case 2: pos = ndc * el.offset_scale; break;
        case 3: pos = glm::vec2(ndc.x * el.offset_scale, ndc.y); break;
        case 4: pos = glm::vec2(ndc.x, ndc.y * el.offset_scale); break;
        default: break;
        }
        const glm::vec2 scale_at = el.scale_at_light ? ndc : pos;
        const glm::vec2 alpha_at = el.alpha_at_light ? ndc : pos;
        float sx = el.scale_x.Eval(scale_at, inv_aspect);
        float sy = el.scale_y.Eval(scale_at, inv_aspect);
        float alpha = el.alpha.Eval(alpha_at, inv_aspect);
        float fade = 1.0f - std::min(1.0f, s.shield_time / std::max(0.001f, el.fade_out));
        if (s.visible) {
            fade = (fade - 1.0f) * (1.0f - std::min(1.0f, s.visible_time / std::max(0.001f, el.fade_in))) + 1.0f;
        }
        if (el.angle_scale_x.valid) {
            sx *= el.angle_scale_x.Eval(light.cone);
        }
        if (el.angle_scale_y.valid) {
            sy *= el.angle_scale_y.Eval(light.cone);
        }
        alpha *= fade;
        if (full_ghost) alpha *= ghost_scale;
        if (el.angle_alpha.valid) {
            alpha *= el.angle_alpha.Eval(light.cone);
        }
        const float base = flare.base_distance;
        switch (el.distance_scaling) {
        case 1: {
            const float f = flare.shape_limit ? base / light.distance : light.ratio;
            sx *= f;
            sy *= f;
            break;
        }
        case 2:
            if (!flare.shape_limit) {
                alpha *= light.fade;
            } else if (light.distance <= base) {
                alpha *= el.limit_distance < light.distance ? 0.0f : 1.0f;
            } else {
                const float t = std::max(0.0f, (light.distance - el.limit_distance) / (base - el.limit_distance));
                alpha *= t * t;
            }
            break;
        case 3: {
            const float limit = flare.shape_limit ? el.limit_distance : flare.limit_distance;
            const float f = limit <= base ? limit / light.distance : std::clamp((limit - light.distance) / (limit - base), 0.0f, 1.0f) * (base / light.distance);
            sx *= f;
            sy *= f;
            break;
        }
        default: break;
        }
        if (alpha <= 0.0f) {
            continue;
        }
        float angle = el.base_rotate;
        if (el.rotate_type == 1) {
            angle -= std::atan2(pos.y, pos.x);
        } else if (el.rotate_type == 2) {
            angle += std::atan2(pos.y, pos.x);
        }
        const glm::vec2 center(pos.x * 0.5f + 0.5f, pos.y * -0.5f + 0.5f);
        const float ax = sx * el.width * 0.25f;
        const float ay = sy * el.height * 0.25f * view.aspect;
        Quad q;
        for (int i = 0; i < 4; ++i) {
            const float a = angle + kFlareCorners[i];
            const glm::vec2 p = center + glm::vec2(ax * std::cos(a), ay * std::sin(a));
            q.corner[i] = glm::vec4(p * 2.0f - 1.0f, 0.0f, 1.0f);
        }
        q.uv = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        q.uv_next = q.uv;
        q.color = glm::vec4(PackedColor(el.color), alpha * el.color.a);
        q.params = params;
        q.luminance = root;
        q.info = glm::uvec4(textures ? textures(el.texture) : 0u, kQuadFlare, kNoTexture, 0u);
        out.quads.push_back(q);
        if (static const bool flare_log = std::getenv("PT_FLARE_LOG") != nullptr; flare_log) {
            LogInfo("flare: {} {} light ({:.2f} {:.2f} {:.2f}) d {:.2f} ndc ({:.3f} {:.3f}) w {:.2f} on_screen {} cone {:.2f} fade {:.2f} visible {} "
                    "shape pos ({:.3f} {:.3f}) size ({:.2f} {:.2f}) alpha {:.3f}",
                    flare_effect_ ? *flare_effect_ : std::string(), el.texture.substr(el.texture.rfind('/') + 1), light.position.x, light.position.y,
                    light.position.z, light.distance, ndc.x, ndc.y, clip.w, on_screen, light.cone, light.fade, s.visible, pos.x, pos.y, sx, sy,
                    alpha * el.color.a);
        }
    }
    draw.count = static_cast<uint32_t>(out.quads.size()) - draw.first;
    if (draw.count) {
        out.draws.push_back(draw);
    }
}

}
