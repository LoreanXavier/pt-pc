#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pt {

struct GpuMesh;

enum class LightType : uint8_t { Point = 0, Spot = 1 };
enum class AreaClipMode : uint8_t { None = 0, Box = 1, ProjectiveAperture = 2 };

constexpr uint64_t kHandyLightId = 0x48414E44594C4954ull;
constexpr uint64_t kMirrorLightId = 0x4D4952524F524C54ull;
constexpr uint64_t kHandyReflectionId = 0x5245464C45435430ull;

struct SceneLight {
    LightType type = LightType::Point;
    uint64_t id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 intensity{0.0f};
    float source_radius = 0.0f;
    float inner_range = 0.0f;
    float outer_range = 1.0f;
    float dimmer = 0.0f;
    float cos_outer = -1.0f;
    float inv_cone_range = 1.0f;
    float cone_exponent = 1.0f;
    float shadow_cos_outer = -1.0f;
    float shadow_inv_cone_range = 1.0f;
    float shadow_fov = 0.0f;
    float shadow_bias = 0.0f;
    float view_bias = 0.0f;
    float specular_scale = 1.0f;
    float diffuse_scale = 1.0f;
    float shadow_strength = 1.0f;
    glm::vec4 lod{0.0f};
    bool cast_shadow = false;
    bool has_area = false;
    AreaClipMode area_clip_mode = AreaClipMode::Box;
    glm::mat4 area_to_box{1.0f};
    glm::mat4 area_world{1.0f};
    bool masked = false;
    float mask_fov = 0.0f;
    int32_t mask_texture = -1;
    uint8_t priority = 0x40;
    uint8_t hidden_views = 0;
    std::string name;
    glm::vec3 rank_point{0.0f};
    bool has_rank_point = false;
};

struct SceneProbe {
    glm::mat4 world_to_box{1.0f};
    glm::mat4 box_world{1.0f};
    glm::vec3 positive_scale{1.0e4f};
    glm::vec3 negative_scale{1.0e4f};
    float weight = 1.0f;
    int priority = 0;
    std::array<glm::vec3, 9> sh{};
    std::string name;
};

struct SceneMirror {
    const GpuMesh* mesh = nullptr;
    glm::mat4 transform{1.0f};
    glm::mat4 light_area{1.0f};
    bool has_light_area = false;
};

struct ExposureSettings {
    float min_ev = -10.0f;
    float max_ev = 1.0f;
    float compensation = 0.0f;
    float key = 1.0f;
    float add_comp[3] = {-4.8f, -3.8f, -2.4f};
    float add_comp_ev[3] = {0.0f, -3.0f, -5.0f};
    float speed = 1.6f;
    float bloom_weight = 1.2f;
    float bloom_extraction = 3.0f;
    float bloom_size = 2.0f;
    bool pinned = false;
    float pinned_ev = 0.0f;
};

struct ScreenSettings {
    std::string lut_path;
    std::string previous_lut_path;
    float lut_blend = 1.0f;
    glm::vec3 color_scale{1.0f, 1.12f, 1.3f};
    float start_slope = 0.4f;
    float end_slope = 0.6f;
    bool film_grain = true;
    float grain_strength = 1.0f;
    bool grain_alt = false;
    glm::vec2 grain_offset{0.0f};
    bool screen_distortion = true;
    bool full_screen_blur = false;
    float blur_blend_rate = 0.85f;
    float blur_fetch_band = 2.5f;
    bool depth_of_field = true;
    float focus_distance = 1.0f;
    float focal_length = 35.0f;
    float aperture = 100.0f;
    uint32_t dof_flags = 0;
    bool motion_blur = true;
    bool fixed_shutter = false;
    float shutter_speed = 0.0f;
    float zoom = 1.0f;
    bool local_reflections = true;
    bool wide_shadow_limit = false;
    float reflect_scale = 1.0f;
    float reflect_bias = 0.0f;
    bool reflect_edge = false;
    bool colour_banding_canceller = false;
    bool subsurface_scatter = false;
};

struct TppAtmosphereSettings {
    bool enabled = false;
    bool tonemap = false;
    bool sky = false;
    float threshold = 0.3f;
    float range = 8.0f;
    glm::vec3 fog_self{0.0f};
    float fog_density = 0.0f;
    float fog_falloff = 0.0f;
    float fog_near = 0.0f;
    float fog_far = 70.0f;
    glm::vec3 fog_mie{0.0f};
    float fog_mie_anisotropy = 0.0f;
    glm::vec3 fog_rayleigh{0.0f};
    float exposure_offset_values[3] = {0.0f, 0.0f, 0.0f};
    float exposure_offset_targets[3] = {0.0f, 0.0f, 0.0f};
    glm::vec3 dir_color{0.0f};
    glm::vec3 light_dir{0.0f, 1.0f, 0.0f};
    float dir_gain = 0.0f;
    bool area = false;
    glm::vec3 area_min{0.0f};
    glm::vec3 area_max{0.0f};
    glm::vec3 area_color{0.0f};
    float area_density = 0.0f;
    float area_near = 0.0f;
    float area_falloff = 0.0f;
    bool area_inverse = false;
};

struct SceneOccluder {
    std::array<glm::vec3, 7> points{};
    uint32_t count = 0;
    bool one_sided = false;
    bool flag4 = false;
};

struct SceneReflectionSample {
    bool active = false;
    glm::vec3 points[4]{};
};

struct SceneLighting {
    bool valid = false;
    TppAtmosphereSettings tpp;
    std::vector<SceneLight> lights;
    std::vector<SceneProbe> probes;
    std::vector<SceneMirror> mirrors;
    std::vector<SceneOccluder> occluders;
    bool mirror_capture = false;
    bool mirror_high = false;
    std::string reflection_texture;
    SceneReflectionSample reflection_sample;
    ExposureSettings exposure;
    ScreenSettings screen;
};

namespace light_math {

glm::vec3 ColorFromTemperature(float temperature, float deflection, float lumen, const glm::vec3& color);
float SpotSolidAngle(float umbra_degrees, float penumbra_degrees, float exponent);
float SourceRadius(float light_size);
std::array<float, 9> ShBasis(const glm::vec3& n);

}

}
