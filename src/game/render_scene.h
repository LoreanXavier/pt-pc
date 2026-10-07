#pragma once

#include <glm/glm.hpp>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/render/camera.h"
#include "engine/render/scene_lighting.h"

namespace pt {
class Vfs;
}

namespace pt::fox2 {
class DataSetFile;
struct Entity;
}

namespace pt::game {

class Game;
struct Stage;
struct StageData;
struct DemoLight;

void HandyLightPose(const Camera& camera, const glm::vec3& aim_point, float pickup_hold, float demo_pose, glm::vec3& position,
                    glm::vec3& direction, glm::vec3* right_out = nullptr, glm::vec3* up_out = nullptr);

struct TickBlend {
    float t = 1.0f;
    glm::vec3 handy_aim{0.0f};
    std::vector<DemoLight> demo_lights;
};

class RenderSceneBuilder {
public:
    void Build(Game& game, const Camera& camera, float dt, SceneLighting& out, const TickBlend* blend = nullptr);
    void BuildFromStage(const StageData& stage, Vfs& vfs, SceneLighting& out);
    bool log_lens_next = false;

private:
    struct ProbeFile {
        std::map<std::string, std::array<glm::vec3, 9>> probes;
    };
    struct ResidentSettings {
        bool loaded = false;
        float tonemap_speed = 1.6f;
        ExposureSettings defaults;
        glm::vec3 color_scale{1.0f, 1.12f, 1.3f};
        float start_slope = 0.4f;
        float end_slope = 0.6f;
        std::map<std::string, std::string> luts;
        bool tpp_tonemap = true;
        float tpp_threshold = 0.3f;
        float tpp_range = 8.0f;
        uint32_t plugin_flags = 0x907;
        std::string reflection_texture;
    };

    const ProbeFile* LoadProbes(Vfs& vfs, const std::string& path);
    void AddLight(const fox2::DataSetFile& file, const fox2::Entity& e, const glm::mat4& file_to_world, uint64_t id, SceneLighting& out) const;
    std::optional<SceneProbe> BuildProbe(Vfs& vfs, const fox2::DataSetFile& file, const fox2::Entity& e, const glm::mat4& file_to_world);
    static std::optional<SceneOccluder> BuildOccluder(const fox2::DataSetFile& file, const fox2::Entity& e, const glm::mat4& file_to_world);
    void AddHandyLight(Game& game, const Camera& camera, const glm::vec3& aim_point, float dt, SceneLighting& out);
    void AddHandyReflection(Game& game, const SceneLight& handy, SceneLighting& out) const;
    void AddDemoLight(const DemoLight& light, const TickBlend* blend, SceneLighting& out) const;
    void ApplyDemoCamera(Game& game, SceneLighting& out) const;
    void AddTppAtmosphere(Game& game, SceneLighting& out) const;
    void AddMirrors(Game& game, SceneLighting& out) const;
    void AddMirrorLight(Game& game, const Camera& camera, SceneLighting& out) const;
    void LoadResidentSettings(Game& game);
    float FocusDistance(Game& game, const Camera& camera, float dt);

    std::unordered_map<std::string, ProbeFile> probe_files_;
    struct StaticEntities {
        std::vector<const fox2::Entity*> probes;
        std::vector<const fox2::Entity*> occluders;
    };
    std::unordered_map<const fox2::DataSetFile*, StaticEntities> static_entities_;
    std::unordered_map<uint64_t, std::pair<glm::mat4, std::optional<SceneProbe>>> probe_cache_;
    std::unordered_map<uint64_t, std::pair<glm::mat4, std::optional<SceneOccluder>>> occluder_cache_;
    std::vector<std::pair<uint32_t, const void*>> loaded_stage_files_;
    ResidentSettings resident_;
    glm::vec3 handy_color_{1.0f};
    glm::vec3 handy_from_{1.0f};
    glm::vec3 handy_target_{1.0f};
    float handy_fade_ = 0.0f;
    bool handy_initialized_ = false;
    float focus_ = 3.0f;
    glm::vec3 focus_eye_{0.0f};
    glm::vec3 focus_forward_{0.0f, 0.0f, 1.0f};
    bool focus_valid_ = false;
    std::string lut_path_;
    std::string previous_lut_path_;
    float lut_blend_ = 1.0f;
};

}
