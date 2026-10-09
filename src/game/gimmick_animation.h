#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/anim/gani.h"
#include "engine/anim/help_bones.h"
#include "engine/anim/rig.h"
#include "engine/anim/skeleton.h"

namespace pt::game {

class Game;
struct Gimmick;
enum class GimmickType : uint8_t;

template <typename Item>
void AssignSkin(Item& item, std::span<const glm::mat4> skin) {
    if constexpr (requires { item.skin = skin; }) {
        item.skin = skin;
    }
}

struct ConnectPoint {
    std::string name;
    std::string parent;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

bool ReadConnectPoints(std::span<const uint8_t> data, std::vector<ConnectPoint>& out);

class GimmickAnimation {
public:
    static constexpr float kPlayInterpFrames = 8.0f;
    static constexpr float kIdleInterpFrames = 80.0f;

    explicit GimmickAnimation(Game& game) : game_(game) {}

    float MotionSeconds(std::string_view key);
    bool MotionLoops(std::string_view key);
    // 0x1253590: SetEnabled(false) stops the sounds of the record (sound control 0x95CFB0), the bag's talk included
    void StopSounds(GimmickType type);
    void ResetSession();
    void Update(float dt);
    std::span<const glm::mat4> Skin(GimmickType type) const;
    bool ConnectPointWorld(GimmickType type, std::string_view name, glm::vec3& out) const;
    bool BoneWorld(GimmickType type, std::string_view bone, glm::vec3& out) const;
    bool RigRoot(GimmickType type, glm::vec3& translation, glm::quat& rotation) const;
    // RIG_ROOT at the first and the last frame of the current motion when its root unit loops (the key stepping of
    // 0xAD4A90 and 0xAD5E70 carries the root across the loop seam); false when it does not loop
    bool RigRootLoop(GimmickType type, glm::vec3& start_translation, glm::quat& start_rotation, glm::vec3& end_translation,
                     glm::quat& end_rotation) const;
    const anim::GaniMotion* Motion(std::string_view key);

private:
    struct Record {
        std::string model;
        std::string parts;
        std::shared_ptr<const anim::Skeleton> skeleton;
        std::vector<uint64_t> bone_codes;
        std::vector<ConnectPoint> connect_points;
        std::string motion;
        const anim::GaniMotion* gani = nullptr;
        float motion_time = 0.0f;
        double played_frames = 0.0;
        uint32_t dialogue_id = 0;
        bool has_pose = false;
        anim::Pose pose;
        anim::Pose from;
        float blend = 1.0f;
        float blend_frames = 1.0f;
        std::vector<glm::mat4> world;
        std::vector<glm::mat4> skin;
        std::shared_ptr<const anim::Rig> rig;
        anim::RigBinding rig_binding;
        anim::RigPose rig_pose;
        anim::RigPose rig_from;
        bool rig_pose_valid = false;
        bool rig_from_valid = false;
        glm::vec3 root_translation{0.0f};
        glm::quat root_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        std::shared_ptr<const anim::HelpBones> help_bones;
    };

    void EnsureArchive();
    void EnsureModel(Record& record, const Gimmick& gimmick);
    void SampleMotion(const Record& record, const anim::GaniMotion& motion, double frame, anim::Pose& out) const;
    void FireEvents(Record& record, const Gimmick& gimmick, double from, double to);
    void BuildFallbackPose(Record& record);
    std::shared_ptr<const anim::Rig> LoadRig(const std::string& path);
    std::shared_ptr<const anim::HelpBones> LoadHelpBones(const std::string& path);
    void SampleRig(Record& record, double frame, bool blending);

    Game& game_;
    anim::MotionArchive archive_;
    bool archive_loaded_ = false;
    std::array<Record, 5> records_;
    std::map<std::string, const anim::GaniMotion*, std::less<>> by_key_;
    std::map<std::string, std::shared_ptr<const anim::Rig>, std::less<>> rigs_;
    std::map<std::string, std::shared_ptr<const anim::HelpBones>, std::less<>> help_bones_;
    anim::Pose ocho_fallback_;
    bool ocho_fallback_ready_ = false;
};

}
