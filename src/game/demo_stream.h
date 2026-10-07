#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/anim/demo_file.h"

namespace pt::fox2 {
class DataSetFile;
struct Entity;
}

namespace pt::game {

constexpr double kDemoFramesPerSecond = anim::kDemoFramesPerSecond;
constexpr uint64_t kDemoSendMessageFunctor = 0xCEE2EDD67199;

struct DemoControlCharacter {
    std::string model;
    std::string character_id;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 start_translation{0.0f};
    glm::quat start_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool controlled_at_start = true;
};

struct DemoClip {
    std::string name;
    std::string camera;
    int start = 0;
    int end = 0;
    int offset = 0;
};

struct DemoStreamData {
    anim::DemoStreamFile file;
    std::vector<anim::DecodedTrack> tracks;
    std::vector<const anim::StreamEvent*> events;
    std::vector<uint8_t> sound_wem;
    uint32_t length_frames = 0;
    bool has_camera = false;
    int camera_actor = -1;
    int camera_param_actor = -1;

    bool Load(std::vector<uint8_t> bytes, std::string* error);
    bool Sample(int track, double frame, glm::vec4& out) const;
    bool SampleTransform(size_t actor, double frame, glm::quat& rotation, glm::vec3& translation) const;
    int FindActor(anim::ActorKind kind, std::string_view target) const { return file.FindActor(kind, target); }
};

struct DemoInfo {
    std::string demo_id;
    std::string stream_path;
    std::string motion_path;
    std::string audio_path;
    int length_frames = 0;
    bool on_memory = true;
    uint32_t stage_id = 0;
    int camera_interp_type = 0;
    int camera_interp_frames = 0;
    float camera_interp_curve_rate = 1.0f;
    float camera_interp_scurve_center = 0.5f;
    glm::vec3 camera_start_translation{0.0f};
    glm::quat camera_start_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float camera_start_focal = 13.0f;
    glm::vec3 camera_end_translation{0.0f};
    glm::quat camera_end_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float camera_end_focal = 13.0f;
    glm::quat transform_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 transform_translation{0.0f};
    std::vector<DemoControlCharacter> control_characters;
    std::map<std::string, std::string> part_names;
    std::map<std::string, int> locator_types;
    std::map<std::string, std::string> model_files;
    std::map<std::string, std::string> help_bone_files;
    std::map<std::string, std::string> parts_files;
    std::map<std::string, std::string> model_parts;
    std::map<std::string, std::string> setup_lights;
    std::map<std::string, std::string> file_params;
    std::vector<DemoClip> clips;
    std::shared_ptr<DemoStreamData> stream;
    bool stream_failed = false;

    const DemoControlCharacter* ControlCharacter(std::string_view model) const;
    bool ControlsPlayer() const;
};

bool ReadDemoInfo(const fox2::DataSetFile& file, const fox2::Entity& entity, DemoInfo& out);

}
