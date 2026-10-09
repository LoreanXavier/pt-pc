#pragma once

#include <glm/glm.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/data/fox2.h"

namespace pt::game {

struct StaticModelPlacement {
    std::string name;
    const fox2::Entity* entity = nullptr;
    std::string model_file;
    std::string geom_file;
    glm::mat4 world{1.0f};
    glm::vec4 color{1.0f};
    bool visible_geom = false;
    int draw_rejection_level = 0;
};

struct LightPlacement {
    std::string name;
    const fox2::Entity* entity = nullptr;
    std::string class_name;
    glm::mat4 world{1.0f};
    bool enable = true;
    glm::vec4 color{1.0f};
    float temperature = 6500.0f;
    float lumen = 0.0f;
    float inner_range = 0.0f;
    float outer_range = 0.0f;
    float light_size = 0.0f;
    float dimmer = 1.0f;
    float umbra_angle = 0.0f;
    float penumbra_angle = 0.0f;
    float attenuation_exponent = 1.0f;
    bool cast_shadow = false;
    bool has_specular = true;
};

struct TrapCallback {
    std::string class_name;
    const fox2::Entity* entity = nullptr;
};

struct TrapCondition {
    std::string name;
    const fox2::Entity* entity = nullptr;
    bool enable = true;
    bool is_once = false;
    bool is_and_check = false;
    std::vector<std::string> check_names;
    std::vector<std::string> exec_names;
    std::vector<TrapCallback> checks;
    std::vector<TrapCallback> execs;
};

struct TrapPlacement {
    std::string name;
    const fox2::Entity* entity = nullptr;
    bool enable = true;
    std::vector<glm::mat4> boxes;
    std::vector<TrapCondition> conditions;
};

struct LocatorPlacement {
    std::string name;
    std::string class_name;
    glm::mat4 world{1.0f};
    const fox2::Entity* entity = nullptr;
};

struct MessageScript {
    std::string name;
    std::string class_name;
    const fox2::Entity* entity = nullptr;
    bool enable = true;
    std::string demo_id;
    std::string message_name;
    bool order_floor = false;
    std::vector<std::string> floor_names;
    std::string script_file;
};

struct SoundSourcePlacement {
    std::string name;
    const fox2::Entity* entity = nullptr;
    std::string event_name;
    glm::mat4 world{1.0f};
    float play_range = 0.0f;
};

struct StageData {
    std::string package_path;
    std::shared_ptr<fox2::DataSetFile> file;
    glm::mat4 root{1.0f};
    bool has_root = false;
    std::map<std::string, glm::mat4> connectors;
    std::vector<StaticModelPlacement> static_models;
    std::vector<LightPlacement> lights;
    std::vector<TrapPlacement> traps;
    std::vector<LocatorPlacement> locators;
    std::vector<LocatorPlacement> game_objects;
    std::vector<MessageScript> message_scripts;
    std::vector<SoundSourcePlacement> sound_sources;
};

// logs nothing, so it can run on a worker (StageManager's prefetch); LogStageData logs the summary line
std::unique_ptr<StageData> BuildStageData(std::shared_ptr<fox2::DataSetFile> file, std::string package_path);
void LogStageData(const StageData& stage);

}
