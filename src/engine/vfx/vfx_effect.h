#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/vfx/vfx_file.h"

namespace pt::vfx {

constexpr float kFrameRate = 60.0f;

enum class ExprOp : uint8_t {
    Zero,
    Const,
    Random,
    Composition,
    Multiply,
    UniformVelocity,
    UniformAccel,
    UniformVelocityTime,
    DragTime,
    TimeScale,
    Keyframe,
    Oscillate,
    UvMap,
    UvRandom,
    UvAnime,
    CameraCorrection,
    CameraFollow,
    CenterScroll,
    CenterDistRate,
    CameraAngle,
    InterpolateLine,
    Spread,
    Receive,
    Pass,
    Wind,
};

enum class TimeSource : uint8_t { Age, LifeRatio, EffectTime, EffectRatio };

struct Curve {
    std::vector<float> times;
    std::vector<float> values;
    std::vector<float> inv_span;

    float Eval(float t) const;
};

struct Expr {
    ExprOp op = ExprOp::Zero;
    int32_t in[2] = {-1, -1};
    int32_t slot = -1;
    glm::vec4 v0{0.0f};
    glm::vec4 v1{0.0f};
    glm::vec4 v2{0.0f};
    glm::vec4 v3{0.0f};
    uint32_t flags = 0;
    uint32_t seed = 0;
    uint8_t seed_type = 0;
    uint32_t node_id = 0;
    uint8_t mode = 0;
    TimeSource time = TimeSource::Age;
    uint32_t receive = 0;
    Curve curves[4];
    bool world = false;
};

enum class EmitOp : uint8_t { Interval, Delay, FirstLoopOnly, Lod };

struct EmitNode {
    EmitOp op = EmitOp::Interval;
    int32_t input = -1;
    float interval = 1.0f;
    float probability = 100.0f;
    float fade_position = 1.0f;
    bool fade_reverse = false;
    uint16_t num_min = 1;
    uint16_t num_max = 1;
    uint16_t delay = 0;
    uint16_t delay_range = 0;
    uint16_t life = 1;
    uint16_t life_range = 0;
    uint32_t seed = 0;
    uint8_t seed_type = 0;
    uint32_t node = 0;
    uint32_t node_id = 0;
    float delay_s = 0.0f;
    float delay_range_s = 0.0f;
    float life_s = 0.0f;
    uint32_t num = 1;
    bool once = false;
    float lod_distance = 0.0f;
    float lod_percent = 1.0f;
    bool lod_inverse = false;
};

struct LifeDef {
    float min = 1.0f;
    float max = 1.0f;
    bool infinite = false;
    uint32_t seed = 0;
    uint8_t seed_type = 0;
    uint32_t node = 0xFFFFFFFFu;
    uint32_t node_id = 0;
};

enum class BlendMode : uint8_t { Alpha = 0, Add = 1, Sub = 2, Mul = 3, Min = 4, Opaque = 5 };

enum class MaterialKind : uint8_t { Unlit, DynamicLuminance, Lit, Liquid, Scroll };

struct MaterialDef {
    MaterialKind kind = MaterialKind::Unlit;
    BlendMode blend = BlendMode::Alpha;
    uint32_t shader_type = 0;
    std::string texture;
    std::string normal;
    bool soft = false;
    float soft_factor = 1.0f;
    float camera_z_offset = 0.0f;
    float fade_near = 0.0f;
    float fade_far = 0.0f;
    bool opaque = false;
    float min_ev = 0.0f;
    float max_ev = 1.0f;
    float lum_min = 1.0f;
    float lum_max = 1.0f;
    float ambient_rate = 1.0f;
    float directional_rate = 0.0f;
    float point_rate = 0.0f;
    uint32_t point_lights = 3;
    bool anime_blend = false;
    float anime_fps = 60.0f;
    uint32_t anime_w = 1;
    uint32_t anime_h = 1;
    float transparency = 1.0f;
    float roughness = 0.0f;
    bool liquid_hnm = false;
    float refraction = 0.0f;
    float fresnel_base = 0.0f;
    float fresnel_power = 1.0f;
    float reflection = 0.0f;
    std::string reflection_texture;
    uint32_t node = 0xFFFFFFFFu;
    uint32_t node_id = 0;
    std::string rain_texture;
    glm::vec2 rain_scale{1.0f};
    glm::vec2 rain_angle{0.0f};
    glm::vec2 rain_swing{0.0f};
    float rain_scroll_speed = 0.0f;
    float rain_phase_speed = 0.0f;
    glm::vec2 texture_scale{1.0f};
    float texture_angle = 0.0f;
    float luminance = 1.0f;
};

enum class ShapeKind : uint8_t { Sprite, SpriteRot, Plane, Sprite2D, Model, SpotLight, PointLight };

enum Attr : uint8_t { kPosition = 0, kScale, kRotation, kUv, kColor, kExtra, kAttrCount };

struct ShapeDef {
    ShapeKind kind = ShapeKind::Sprite;
    uint32_t node = 0;
    int32_t emit = -1;
    LifeDef life;
    MaterialDef material;
    int32_t attr[kAttrCount] = {-1, -1, -1, -1, -1, -1};
    bool world_position = false;
    bool local_space = true;
    float center_u = 0.5f;
    float center_v = 0.5f;
    float base_size = 1.0f;
    uint32_t sort_mode = 0;
    float sort_offset = 0.0f;
    bool cull_face = false;
    bool invert_face = false;
    bool model_uv = false;
    uint64_t model = 0;
    uint32_t axis_fix = 0;
    bool separate_draw = false;
    uint32_t screen_width = 1280;
    uint32_t screen_height = 720;
    uint32_t priority = 0;
    float inner_range = 0.0f;
    float outer_range = 1.0f;
    float attenuation = 1.0f;
    bool cast_shadow = false;
    bool specular = true;
    float view_bias = 0.0f;
    float shadow_bias = 0.0f;
    float shadow_umbra_scale = 1.0f;
    float shadow_penumbra_scale = 1.0f;
    std::string light_mask;
    // light area box (0xB84260): enableLightArea, lightAreaTranslation, the rotation 0x733D3780 (x, y, z, w), lightAreaScale (full size)
    bool light_area = false;
    glm::vec3 area_translation{0.0f};
    glm::vec4 area_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec3 area_scale{1.0f};
    uint32_t max_particles = 0;
    bool particle_random = false;
};

struct FlareDef {
    uint32_t node = 0;
    std::string lens_flare;
    float lux = 10.0f;
    float base_distance = 5.0f;
    float limit_distance = 100.0f;
    float temperature = 5500.0f;
    glm::vec4 offset{0.0f};
    uint32_t draw_priority = 0;
    float angle = 360.0f;
    bool shape_limit = false;
    float depth_tolerance = 0.0f;
};

struct EffectDef {
    std::string name;
    uint32_t all_frame = 1;
    uint32_t play_mode = 0;
    uint32_t fade_in_end = 0;
    uint32_t fade_out_start = 0;
    uint32_t update_type = 0;
    std::vector<Expr> exprs;
    std::vector<EmitNode> emits;
    std::vector<ShapeDef> shapes;
    std::vector<FlareDef> flares;
    uint32_t random_slots = 0;
    std::vector<std::string> unsupported;

    float Length() const { return static_cast<float>(all_frame) / kFrameRate; }
};

std::shared_ptr<EffectDef> CompileEffect(const File& file, std::string name);

}
