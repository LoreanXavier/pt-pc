#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/anim/help_bones.h"
#include "engine/anim/sim_physics.h"
#include "engine/anim/skeleton.h"
#include "engine/render/camera.h"
#include "game/demo_stream.h"
#include "game/gimmick_animation.h"

namespace pt {
struct GpuMesh;
struct DrawItem;
}

namespace pt::game {

class Game;
struct Stage;

struct DemoCameraParams {
    float focal_length = 13.0f;
    float focus_distance = 5.0f;
    float aperture = 1.0f;
    float shutter_speed = 0.0f;
    float exposure_compensation = 0.0f;
    float min_exposure = -10.0f;
    float max_exposure = 1.0f;
    float bloom_size = 2.0f;
    float bloom_weight = 1.2f;
    float bloom_extraction = 3.0f;
    float key_value = 1.0f;
    float near_clip = 0.05f;
    float far_clip = 2000.0f;
    float add_exposure[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float functor_focal_length = 0.0f;
    uint32_t set_mask = 0;
};

// defaults: CreateLight's reset 0xB25310
struct DemoLight {
    std::string demo_id;
    std::string name;
    bool point = false;
    bool enabled = true;
    int light_type = 0;
    std::string locator;
    glm::vec4 color{0.1f, 0.5f, 0.1f, 1.0f};
    float temperature = 6500.0f;
    float lumen = 1.0f;
    float deflection = 0.0f;
    float power_scale = 1.0f;
    float attenuation_exponent = 1.2f;
    float umbra = 45.0f;
    float penumbra = 30.0f;
    float inner_range = 0.05f;
    float outer_range = 5.0f;
    bool shadow = true;
    float shadow_umbra = 45.0f;
    float shadow_penumbra = 45.0f;
    float shadow_attenuation_exponent = 1.2f;
    float view_bias = 0.0f;
    float bias = 0.0f;
    bool specular = true;
    float shadow_strength = 1.0f;
    float light_size = 0.0f;
    int end_frame = -1;
    glm::quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 local_translation{0.0f};
    std::string constraint_locator;
    glm::vec3 constraint_offset{0.0f};
    glm::vec3 constraint_rotation_offset{0.0f};
    glm::mat4 world{1.0f};
};

struct DemoEffect {
    std::string demo_id;
    uint64_t instance = 0;
    uint64_t effect_name = 0;
    uint64_t file = 0;
    std::string file_path;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    std::string null_locator;
    glm::vec3 connect_offset{0.0f};
    glm::quat connect_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::mat4 world{1.0f};
    std::map<uint64_t, glm::vec4> parameters;
    int created_frame = 0;
    int end_frame = -1;
    std::string sound_event;
    // FxSoundCallProgramEffectNode (read by 0xB6DCB0): soundStop, flags bit 2 (0x37CD447C, stop the playing id when the effect
    // instance is destroyed), the stop fade (0xD2ECAC68, seconds) and its curve (0xE3A9CADA, AkCurveInterpolation)
    std::string sound_stop;
    bool sound_stop_playing = false;
    float sound_stop_fade = 0.0f;
    uint32_t sound_stop_curve = 4;
    bool sound_pending = false;
    uint64_t sound_object = 0;
    uint32_t sound_id = 0;
};

struct DemoScreenState {
    bool tone_set = false;
    glm::vec4 color_scale{1.0f};
    float start_slope = 0.4f;
    float end_slope = 0.6f;
    std::map<uint64_t, std::vector<float>> values;
    std::map<std::pair<uint64_t, uint64_t>, std::vector<float>> named_values;
};

struct DemoUiEvent {
    std::string demo_id;
    uint64_t functor = 0;
    uint64_t graph = 0;
    uint64_t file = 0;
    std::string file_path;
    uint64_t text = 0;
    int frame = 0;
};

struct DemoModel {
    std::string name;
    std::string fmdl;
    bool skinned = false;
    bool drawn = false;
    bool visible = true;
    bool active = true;
    int root_actor = -1;
    int skeleton_actor = -1;
    int points_actor = -1;
    const GpuMesh* mesh = nullptr;
    std::shared_ptr<const anim::Skeleton> skeleton;
    std::shared_ptr<const anim::HelpBones> help_bones;
    std::vector<int> bone_of_unit;
    std::vector<glm::mat4> bone_world;
    std::vector<glm::mat4> skin;
    anim::Pose pose;
    glm::mat4 world{1.0f};
    std::set<uint64_t> hidden_meshes;
    bool player_own = false;
    // the model's bone simulation (.sim) and its state; the sections of the physics functor 0x116E2D91CDAE that take the
    // model's units out of the world (the ending's plr0 at frames 4893 to 4927 and 5625 to 5659, around cuts)
    std::shared_ptr<const anim::SimRig> sim_rig;
    std::shared_ptr<anim::SimPhysics> sim;
    double sim_frame = -1.0;
    bool sim_suspended = false;
    std::vector<std::pair<int32_t, int32_t>> sim_suspend_sections;
};

struct DemoInterpolation {
    const anim::StreamEvent* event = nullptr;
    int32_t start = 0;
    int32_t end = 0;
};

struct PlayingDemo {
    std::string demo_id;
    double time = 0.0;
    double length = 0.0;
    double frame = 0.0;
    double previous_frame = -1.0;
    uint32_t length_frames = 0;
    bool has_transform = false;
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 translation{0.0f};
    bool finished = false;
    bool loop = false;
    bool started = false;
    double startup = 0.0;
    // game frames from Play to the start: 3, one more when a save is being written (demo.md Playback)
    double startup_frames = 0.0;
    bool startup_io_checked = false;
    double advance_wait = 0.0;
    double finish_wait = 0.0;
    bool skip_requested = false;
    bool end_reached = false;
    // FinishMotion was posted in this play, by the data or by the runtime at the end (once a play, demo.md Playback)
    bool finish_motion_sent = false;
    std::shared_ptr<DemoStreamData> stream;
    std::shared_ptr<const DemoInfo> info;
    size_t next_event = 0;
    std::vector<DemoInterpolation> interpolations;
    std::map<std::pair<const anim::StreamEvent*, uint32_t>, float> start_values;
    std::vector<const anim::StreamEvent*> skip_events;
    bool camera_created = false;
    bool camera_valid = false;
    bool camera_seen = false;
    bool camera_handed_back = false;
    double camera_linger = 0.0;
    glm::vec3 camera_position{0.0f};
    glm::quat camera_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float camera_focal = 13.0f;
    bool blend_active = false;
    int32_t blend_start = 0;
    int32_t blend_frames = 0;
    glm::vec3 blend_position{0.0f};
    glm::quat blend_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float blend_focal = 13.0f;
    glm::vec3 blend_offset{0.0f};
    bool blend_offset_set = false;
    Camera blend_from;
    bool blend_from_set = false;
    DemoCameraParams camera_params;
    bool player_taken = false;
    bool player_released = false;
    std::string player_model;
    // 0x945620 clears the model's hide bits (+0x1AC bits 0 and 1) before it shows or hides a group, so the player's own
    // body is drawn only in demos that run the mesh functor 0xBFA41F9E6521
    bool player_visible = false;
    std::vector<DemoModel> models;
    std::set<std::string> locators;
    std::vector<DemoLight> lights;
    std::vector<DemoEffect> effects;
    DemoScreenState screen;
    uint32_t sound_id = 0;
    bool sound_started = false;
    bool audio_clock = false;
    uint64_t audio_start_frames = 0;
    uint64_t audio_last_frames = 0;
    // the audio clock's lead over the demo clock, smoothed (DemoSystem::Update), and its last reading in demo frames
    double audio_error = 0.0;
    double audio_frame = -1.0;
    // the demo frame the audio clock's count starts from (0, or the frame of a ResyncAudio)
    double audio_base = 0.0;
    // a demo played by another demo's play functor runs on that demo's clock (0x797650 -> 0xB15FC0: player +0x288 = the parent's
    // player, +0x290 = the play event's frame), so its frame is the parent's minus the offset once that is positive
    std::string sync_parent;
    double sync_offset = 0.0;
    std::set<uint64_t> logged_functors;
    // scenery hold (PlayScenery): the demo runs fast and silent to this frame and stays there, for its stage lights, fog, tone,
    // exposure, models and effects; it shows no camera, characters, texts or fades and never finishes. Negative: a normal play
    double hold_frame = -1.0;
    bool Held() const { return hold_frame >= 0.0; }

    glm::mat4 Transform() const;
};

class DemoSystem {
public:
    explicit DemoSystem(Game& game);
    ~DemoSystem();

    void IndexStage(Stage& stage);
    void ForgetStage(const Stage& stage);
    bool Play(std::string_view demo_id);
    // the ending street walk: plays the demo as scenery held at `frame` (PlayingDemo::hold_frame)
    bool PlayScenery(std::string_view demo_id, double frame);
    // a held scenery demo has reached its frame
    bool SceneryReady() const;
    bool IsHeld(std::string_view demo_id) const;
    // the exposure of a held scenery demo's camera (its lens is not used), when no demo camera is on screen
    const DemoCameraParams* SceneryCameraParams() const;
    void SetDemoTransform(std::string_view demo_id, const glm::quat& rotation, const glm::vec3& translation);
    void StopAll();
    void ToggleLoop(std::string_view demo_id);
    void Skip(std::string_view demo_id = {});
    // the Archive's teaser (archive_theater.h): after a fast run to a frame, the demo's sound jumps there and its clock follows it
    void ResyncAudio(std::string_view demo_id);
    bool IsPlaying(std::string_view demo_id) const;
    bool IsAnyPlaying() const { return !playing_.empty(); }
    bool ControlsPlayer() const;
    // a playing demo's camera is on screen (not the last camera kept while a demo holds the player)
    bool HasActiveCamera() const { return ActiveCamera() != nullptr; }
    bool CameraOverride(Camera& camera) const;
    bool CameraWorld(glm::vec3& position, glm::quat& rotation, float& fov_y) const;
    double PlayTime(std::string_view demo_id) const;
    const std::vector<PlayingDemo>& Playing() const { return playing_; }
    const DemoInfo* Find(std::string_view demo_id) const;
    void Update(float dt);

    void CollectDraws(std::vector<DrawItem>& out) const;
    // The player's own model as plr0_main0_def_v00.parts describes it: the model file, its help bones and the groups its
    // invisibleMeshNames hide (MESH_arm), for the body the mirror captures draw in play
    const std::string& PlayerModelFile();
    const std::set<uint64_t>& PlayerDefaultHidden();
    // the player's body groups as the parts list them (the one model the original keeps for the session; a new session's
    // body is the boot's)
    void ResetPlayerBody() { player_hidden_ = player_default_hidden_; }
    const std::set<uint64_t>& PlayerHidden() const { return player_hidden_; }
    std::shared_ptr<const anim::HelpBones> PlayerHelpBones();
    // the bone simulation of the player's own model (null without a .sim or with PT_SIM=0)
    std::shared_ptr<const anim::SimRig> PlayerSimRig();
    const std::vector<DemoLight>& Lights() const { return lights_; }
    const std::vector<DemoEffect>& Effects() const { return effects_; }
    std::vector<DemoUiEvent> TakeUiEvents() { return std::exchange(ui_events_, {}); }
    const DemoCameraParams* CameraParams() const;
    // the camera lens of one original frame (1/30 s) ago, for the depth of field: the DOF plugin (0xCC9D40) reads the GrCamera
    // before the frame's demo update writes it (boot_dof: in all 313 frames whose focus changed, the blur ratio pass used the
    // previous frame's; kill_dof 3750: 0.2 where the camera holds 0.118); null when no demo camera was shown then
    const DemoCameraParams* DofLens() const;
    const DemoScreenState* ScreenState() const;
    GimmickAnimation& Gimmicks() { return *gimmicks_; }
    void SetGameLens(float focus_distance, float aperture, float shutter_speed);

    float time_scale = 1.0f;

private:
    std::shared_ptr<DemoStreamData> LoadStream(DemoInfo& info);
    std::shared_ptr<const anim::Skeleton> LoadSkeleton(const std::string& fmdl);
    std::shared_ptr<const anim::HelpBones> LoadHelpBones(const std::string& path);
    std::string HelpBonePath(const PlayingDemo& demo, const std::string& model) const;
    std::string PartsPath(const PlayingDemo& demo, const std::string& model) const;
    std::shared_ptr<const anim::SimRig> LoadSimRig(const std::string& parts_path, const std::string& fmdl);
    void SimulateModel(PlayingDemo& demo, DemoModel& model);
    void LoadPlayerParts();
    void SetPlayerMeshVisible(PlayingDemo& demo, uint64_t mesh, bool visible, bool functor);
    std::string MessageName(uint64_t hash) const;
    std::string ModelPath(const PlayingDemo& demo, const std::string& model, const std::string& event_path) const;
    // a file string of the demo's events through its own DemoData fileParams, then the names every demo registered
    std::string FilePath(const PlayingDemo& demo, uint64_t code) const;
    void Start(PlayingDemo& demo);
    void Advance(PlayingDemo& demo, float dt);
    void RunEvents(PlayingDemo& demo);
    void RunEvent(PlayingDemo& demo, const anim::StreamEvent& event);
    void RunFunctor(PlayingDemo& demo, const anim::StreamEvent& event, float t, bool first);
    void UpdateInterpolations(PlayingDemo& demo);
    void UpdateActors(PlayingDemo& demo);
    void UpdateCamera(PlayingDemo& demo);
    void UpdatePlayer(PlayingDemo& demo);
    void Finish(PlayingDemo& demo, bool skipped);
    void Restart(PlayingDemo& demo);
    void ReleasePlayer(PlayingDemo& demo, const char* reason, double functor_frame = -1.0);
    bool PlayerRootWorld(const PlayingDemo& demo, glm::vec3& position, glm::quat& rotation, double frame = -1.0) const;
    void HandCameraBack(PlayingDemo& demo);
    struct ModelSource {
        std::string fmdl;
        bool drawn = false;
        bool player_own = false;
    };
    std::string EventModelName(const PlayingDemo& demo, const anim::StreamEvent& event) const;
    ModelSource ResolveModel(const PlayingDemo& demo, const std::string& name, bool skinned);
    void PreloadModels(PlayingDemo& demo);
    void CreateModel(PlayingDemo& demo, const anim::StreamEvent& event);
    DemoModel* FindModel(PlayingDemo& demo, std::string_view name);
    DemoLight* FindLight(PlayingDemo& demo, std::string_view name);
    DemoEffect* FindEffect(PlayingDemo& demo, uint64_t instance);
    void RebuildOutputs();
    const PlayingDemo* ActiveCamera() const;
    void UpdateShownCamera();
    void StartEffectSound(DemoEffect& effect);
    void DetachEffectSound(DemoEffect& effect, float fade);
    void EndEffectSound(DemoEffect& effect);
    void Notify(PlayingDemo& demo, std::string_view message);

    Game& game_;
    std::map<std::string, std::shared_ptr<DemoInfo>, std::less<>> demos_;
    std::map<std::string, std::pair<glm::quat, glm::vec3>, std::less<>> pending_transforms_;
    struct PendingPlay {
        std::string id;
        std::string parent;
        double offset = 0.0;
    };
    std::vector<PendingPlay> pending_plays_;
    std::vector<PlayingDemo> playing_;
    std::unordered_map<uint64_t, std::string> message_names_;
    std::unordered_map<uint64_t, std::string> string_names_;
    std::map<std::string, std::shared_ptr<const anim::Skeleton>, std::less<>> skeletons_;
    std::map<std::string, std::shared_ptr<const anim::HelpBones>, std::less<>> help_bones_;
    std::map<std::string, std::shared_ptr<const anim::SimRig>, std::less<>> sim_rigs_;
    std::vector<DemoLight> lights_;
    std::vector<DemoEffect> effects_;
    bool player_parts_loaded_ = false;
    std::string player_model_file_;
    std::string player_help_bone_file_;
    std::set<uint64_t> player_default_hidden_;
    // Mesh groups of the player's own model whose own hidden bit is set (the model's +0x180 flags): the parts'
    // invisibleMeshNames at first, then what the mesh functor and VisibleMesh events change; the one model persists
    std::set<uint64_t> player_hidden_;
    std::vector<DemoUiEvent> ui_events_;
    uint64_t next_sound_object_ = 0;
    std::unique_ptr<GimmickAnimation> gimmicks_;
    float game_focus_distance_ = 1.0f;
    float game_aperture_ = 100.0f;
    float game_shutter_speed_ = 1.0f / 120.0f;
    // the demo camera on screen last; it stays there while no demo camera is enabled and a demo holds the player (0x7DF8B0)
    struct ShownCamera {
        bool valid = false;
        std::string demo_id;
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float focal = 13.0f;
        DemoCameraParams params;
    };
    ShownCamera shown_camera_;
    struct LensSample {
        bool valid = false;
        double age = 0.0;
        DemoCameraParams params;
    };
    LensSample lens_history_[4];
    bool holding_camera_ = false;
};

}
