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

// SceneLight::id of the player's handy light (RenderSceneBuilder::AddHandyLight); the mirror's LocalLight is this light
constexpr uint64_t kHandyLightId = 0x48414E44594C4954ull;
// SceneLight::id of the MirrorLight, the handy light seen in the mirror (RenderSceneBuilder::AddMirrorLight)
constexpr uint64_t kMirrorLightId = 0x4D4952524F524C54ull;
// SceneLight::id of the first of the handy light's four reflection lights (RenderSceneBuilder::AddHandyReflection), + 0 to 3
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
    // The light data's priority byte (+0x69): 0x40 by default (0xD4DF90), 0x80 for demo lights and light copies (0xCD9FE0),
    // 0xC0 for level lights (lightsel_f060, lightsel_f010); the shadow selection takes the lights below 0x80 first
    uint8_t priority = 0x40;
    // Views the light is left out of, as DrawItem::hidden_views: bit 0 the camera view, bit 1 the mirror view. The capture
    // views select and shadow their own lights (0xD3F0C0 gives MirrorCapture and MirrorCaptureLow their own limits of 2 and
    // 2), so a light only the mirror view has takes its shadow casters from that view (RecordShadows)
    uint8_t hidden_views = 0;
    // the entity or demo light name for the PT_SHOT_LIGHTS log (empty for effect lights)
    std::string name;
    // The point the shadow ranking and the light LOD measure from, GrLight +0x80. Level lights: the irradiationPoint's position,
    // or the light's own (spots 0xE166A0 through +0xD0, which 0xCDB4F0 copies; points 0xE10B90 through 0xCDA930); other
    // lights: the centre of their grid box
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
    // the ShLightProbe entity (logs)
    std::string name;
};

struct SceneMirror {
    const GpuMesh* mesh = nullptr;
    glm::mat4 transform{1.0f};
    // the Mirror's lightAreaLocatorHandle box, [-0.5, 0.5] on each axis in this frame (a Locator's box is size wide, 0x51BB50)
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
    // the main view's byte +0x5D4 (0xCAF440, set by the floor environment 0x922C60 on f110 only): its light selection
    // (0xD3F0C0) then allows 10 shadowed lights below priority 0x80 and 5 more instead of 3 and 5
    bool wide_shadow_limit = false;
    // ReflectMapBlend m_localParam[1].zw and [2].w (0xDDFD60): strength scale, bias, edge flag (rendering.md 12.16)
    float reflect_scale = 1.0f;
    float reflect_bias = 0.0f;
    bool reflect_edge = false;
    bool colour_banding_canceller = false;
    // the SUBSURFACE_SCATTER plugin's bit in the main view's plugin mask (0x922C60 -> 0xCAF3E0): set on the ending only
    bool subsurface_scatter = false;
};

struct TppAtmosphereSettings {
    bool enabled = false;
    bool tonemap = false;
    // an enabled TppSky: the sky pixels take Sky_Draw_TppBaked's far fog and Bayer offset (compose.frag)
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
    // the atmosphere's directional light over pi (0x8EEC00: the moon in P.T., moonColor x moonLux / pi), its direction toward
    // the light (m_localParam[1] of VolFog_TppVolFog) and the fog's dirLightGain (0x906330)
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

// OccluderEx (0xE07C70): a planar polygon of up to 7 points; the culling core leaves out a grid box it hides from the eye
struct SceneOccluder {
    std::array<glm::vec3, 7> points{};
    uint32_t count = 0;
    bool one_sided = false;
    // the runtime occluder's flag 4 (0xD53AC0 passes it to 0xD2A100, which then takes the other distance limit and
    // 0xD2AC10 keeps such occluders by distance); which OccluderEx property sets it was not traced (0xDA78B0), so none
    bool flag4 = false;
};

// The flashlight reflection's colour sample (0x9359D0, rendering.md 12.5): the world points whose screen positions are the
// corners of the Draw2D_ShSpotLightReflection quad, in the order of its vertices (0, 0), (1, 0), (0, 1), (1, 1). The renderer
// projects them with the camera view, samples the scene through the quad as SceneRenderer::RecordReflectionSample describes
// and returns the colour kFramesInFlight frames later in RenderStats::reflection_readback
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
    // 0x959FD0: the floor's MirrorCapture flag (0x1B86A28) and a MirrorSwitch viewport bit (0x1C938D0) are set; bit 1
    // (the High traps) picks MirrorCapture, 512x512, over MirrorCaptureLow, 128x128 (0x959070)
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
