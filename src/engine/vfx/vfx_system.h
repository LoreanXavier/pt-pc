#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/render/camera.h"
#include "engine/vfx/vfx_effect.h"
#include "engine/vfx/vfx_lensflare.h"

namespace pt::vfx {

void SetFlareGhostScale(float scale);

struct ViewInfo {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::mat4 view_projection{1.0f};
    float aspect = 16.0f / 9.0f;
    float fov_y = 1.0f;
    float near_plane = 0.05f;
};

enum QuadFlag : uint32_t {
    kQuadSoft = 1u << 0,
    kQuadExposure = 1u << 1,
    kQuadLuminance = 1u << 2,
    kQuadLiquid = 1u << 3,
    kQuadScreen = 1u << 4,
    kQuadAnimeBlend = 1u << 5,
    kQuadFlare = 1u << 6,
    kQuadLiquidHnm = 1u << 7,
    kQuadClip = 1u << 8,
    kQuadTriangle = 1u << 9,
    kQuadRain = 1u << 10,
};

constexpr uint32_t kNoTexture = 0xFFFFFFFFu;

struct Quad {
    glm::vec4 corner[4];
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
    glm::vec4 uv_next{0.0f, 0.0f, 1.0f, 1.0f};
    glm::vec4 color{1.0f};
    glm::vec4 params{0.0f};
    glm::vec4 luminance{0.0f};
    glm::vec4 extra{0.0f};
    glm::uvec4 info{0u};
    glm::vec4 rain_rotation{0.0f};
};

static_assert(sizeof(Quad) == 192, "Quad must match vfx_particle.vert's storage buffer stride");

enum class Layer : uint8_t { World, Flare, Screen };

struct Draw {
    Layer layer = Layer::World;
    BlendMode blend = BlendMode::Alpha;
    uint32_t first = 0;
    uint32_t count = 0;
    float depth = 0.0f;
    int32_t priority = 0;
    bool cull = false;
    bool refract = false;
    bool opaque = false;
};

struct RenderList {
    std::vector<Quad> quads;
    std::vector<Draw> draws;
    float near_plane = 0.05f;

    void Clear() {
        quads.clear();
        draws.clear();
    }
};

struct LightOut {
    bool spot = false;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 color{1.0f};
    float lumen = 0.0f;
    float temperature = 6500.0f;
    float umbra = 60.0f;
    float penumbra = 30.0f;
    float inner_range = 0.0f;
    float outer_range = 1.0f;
    float attenuation = 1.0f;
    bool shadow = false;
    bool specular = true;
    float view_bias = 0.0f;
    float shadow_bias = 0.0f;
    float shadow_umbra = 60.0f;
    float shadow_penumbra = 30.0f;
    int32_t mask = -1;
    uint64_t id = 0;
    bool area = false;
    glm::mat4 area_world{1.0f};
};

struct LightingSample {
    glm::vec3 ambient{0.0f};
    glm::vec3 point{0.0f};
    glm::vec3 directional{0.0f};
};

struct ModelMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;
};

using TextureResolver = std::function<uint32_t(const std::string&)>;
using FileReader = std::function<std::optional<std::vector<uint8_t>>(const std::string&)>;
using ModelReader = std::function<std::shared_ptr<const ModelMesh>(uint64_t code)>;
using LightingProbe = std::function<LightingSample(const glm::vec3&, bool ambient)>;

struct LightBlock {
    glm::vec3 sky{0.0f};
    glm::vec3 directional{0.0f};
    uint32_t count = 0;
    glm::vec3 position[3]{};
    glm::vec3 color[3]{};
    float inv_r4[3]{};
};
using LightBlockProbe = std::function<LightBlock(const glm::vec3& center, const glm::vec3& half)>;

struct InstanceKey {
    uint64_t owner = 0;
    uint64_t id = 0;
    auto operator<=>(const InstanceKey&) const = default;
};

class System {
public:
    System();
    ~System();

    void SetReader(FileReader reader) { reader_ = std::move(reader); }
    void SetModelReader(ModelReader reader) { model_reader_ = std::move(reader); }
    void SetFilter(std::string filter) { filter_ = std::move(filter); }
    void SetWind(const glm::vec3& velocity) { wind_ = velocity; }
    std::shared_ptr<const EffectDef> Load(const std::string& path);
    const LensFlareDef* LoadFlare(const std::string& name);
    void Preload(const std::string& path, const TextureResolver& textures, const TextureResolver& cubes);
    void ClearCache();

    bool Exists(const InstanceKey& key) const { return instances_.contains(key); }
    bool IsPlaying(const InstanceKey& key) const;
    bool Spawn(const InstanceKey& key, const std::string& path, const glm::mat4& world, uint32_t seed);
    void SetTransform(const InstanceKey& key, const glm::mat4& world);
    void SnapshotWorlds();
    template <typename F>
    void ForEachMoving(float blend, F&& f) const {
        for (const auto& [key, inst] : instances_) {
            if (inst.previous_world != inst.world) {
                f(key, *inst.def, BlendTransform(inst.previous_world, inst.world, blend));
            }
        }
    }
    void SetParameter(const InstanceKey& key, uint32_t name, const glm::vec4& value);
    void SetParameterLow16(const InstanceKey& key, uint32_t low16, const glm::vec4& value);
    void Remove(const InstanceKey& key);
    void Stop(const InstanceKey& key);
    bool IsStopped(const InstanceKey& key) const;
    void RemoveOwner(uint64_t owner);
    void Clear();
    template <typename F>
    void ForEachKey(F&& f) const {
        for (const auto& [key, inst] : instances_) {
            f(key);
        }
    }

    void Update(float dt, const ViewInfo& view);
    void SetLightBlockProbe(LightBlockProbe probe) { light_block_ = std::move(probe); }
    void Build(const ViewInfo& view, const TextureResolver& textures, const TextureResolver& cubes, const LightingProbe& lighting,
               RenderList& out, std::vector<LightOut>& lights, float blend = 1.0f);

    size_t InstanceCount() const { return instances_.size(); }
    size_t ParticleCount() const;
    std::vector<std::pair<std::string, size_t>> Stats() const;

private:
    struct Particle {
        float age = 0.0f;
        float life = 1.0f;
        bool infinite = false;
        uint32_t index = 0;
        uint32_t count = 1;
        uint16_t random = 0;
        glm::mat4 spawn_world{1.0f};
    };

    struct EmitState {
        uint32_t rng = 1;
        float counter = 1e9f;
        uint16_t start = 0;
        uint16_t end = 0;
        uint16_t fade_in_start = 0;
        uint16_t fade_in_end = 0;
        uint16_t fade_in_len = 0;
        uint16_t fade_out_start = 0;
        uint16_t fade_out_end = 0;
        uint16_t fade_out_len = 0;
        float delay_start = 0.0f;
        uint32_t loop = 0xFFFFFFFFu;
        bool fired = false;
        bool initialized = false;
    };

    struct ShapeState {
        std::vector<Particle> particles;
        std::vector<glm::vec4> slots;
        std::vector<EmitState> emits;
        uint32_t life_rng = 1;
        uint32_t particle_rng = 1;
        uint32_t material_rng = 1;
        glm::vec2 rain_offset{0.0f};
        glm::vec2 rain_phase{0.0f};
    };

    struct FlareState {
        bool visible = false;
        float shield_time = 100.0f;
        float visible_time = 100.0f;
        float check = 0.0f;
    };

    struct FlareLight {
        glm::vec3 position{0.0f};
        float distance = 0.001f;
        float depth = 0.0f;
        float cone = 0.0f;
        float fade = 0.0f;
        float ratio = 0.0f;
    };

    struct Instance {
        std::shared_ptr<const EffectDef> def;
        glm::mat4 world{1.0f};
        glm::mat4 previous_world{1.0f};
        uint32_t seed = 1;
        uint32_t random = 1;
        float frame = 0.0f;
        uint32_t loop = 0;
        bool started = false;
        bool finished = false;
        bool stopped = false;
        std::vector<ShapeState> shapes;
        std::vector<FlareState> flares;
        std::vector<uint32_t> slot_rng;
        std::vector<float> slot_batch;
        std::unordered_map<uint32_t, glm::vec4> params;
        std::vector<std::pair<uint32_t, glm::vec4>> params_low16;
    };

    struct EvalContext {
        const EffectDef* def = nullptr;
        const Instance* inst = nullptr;
        const ViewInfo* view = nullptr;
        const Particle* particle = nullptr;
        const glm::vec4* slots = nullptr;
        float age = 0.0f;
        float life = 1.0f;
        float ratio = 0.0f;
        float effect_time = 0.0f;
        float effect_ratio = 0.0f;
        glm::mat4 world{1.0f};
        glm::vec3 position{0.0f};
    };

    int Emit(Instance& inst, ShapeState& state, const EffectDef& def, int32_t emit, float dframes, const ViewInfo& view);
    void InitEmit(EmitState& s, const EmitNode& e);
    void Spawn(Instance& inst, size_t shape, int count, const ViewInfo& view);
    void GenerateSlots(Instance& inst, const EffectDef& def, int32_t expr, glm::vec4* slots, const Particle& p, uint8_t* done);
    glm::vec4 Eval(const EvalContext& c, int32_t expr) const;
    glm::vec4 Receive(const Instance& inst, uint32_t name, const glm::vec4& fallback) const;
    uint32_t SeedFor(const Instance& inst, uint32_t seed, uint8_t type, uint32_t node_id) const;
    void BuildShape(const Instance& inst, const glm::mat4& world, const ShapeDef& shape, const ShapeState& state, const ViewInfo& view,
                    const TextureResolver& textures, const TextureResolver& cubes, const LightingProbe& lighting, RenderList& out,
                    std::vector<LightOut>& lights) const;
    FlareLight Light(const glm::mat4& world, const FlareDef& flare, const ViewInfo& view) const;
    void UpdateFlare(const Instance& inst, const FlareDef& flare, FlareState& state, float dt, const ViewInfo& view, bool cut);
    const std::string* flare_effect_ = nullptr;
    void BuildFlare(const glm::mat4& world, const FlareDef& flare, const FlareState& state, const ViewInfo& view, const TextureResolver& textures,
                    RenderList& out);

    FileReader reader_;
    ModelReader model_reader_;
    std::unordered_map<std::string, std::shared_ptr<const EffectDef>> effects_;
    std::unordered_map<uint64_t, std::shared_ptr<const ModelMesh>> models_;
    std::unordered_map<std::string, std::unique_ptr<LensFlareDef>> flares_;
    std::set<std::string> missing_;
    std::string filter_;
    std::map<InstanceKey, Instance> instances_;
    std::optional<ViewInfo> last_view_;
    glm::vec3 wind_{0.0f};
    LightBlockProbe light_block_;
    struct BuildItem {
        float depth;
        Quad quad;
    };
    struct BuildLitRef {
        size_t first = 0;
        size_t last = 0;
        glm::vec3 unlit{0.0f};
        glm::vec3 position{0.0f};
    };
    mutable std::vector<BuildItem> build_items_;
    mutable std::vector<BuildLitRef> build_lit_refs_;
};

uint32_t XorShift(uint32_t& state);
float Random01(uint32_t& state);

}
