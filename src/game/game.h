#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/fs/mods.h"
#include "engine/fs/vfs.h"
#include "engine/physics/collision_world.h"
#include "engine/platform/input.h"
#include "engine/render/camera.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_renderer.h"
#include "engine/script/mod_lua.h"
#include "game/blur_sway.h"
#include "game/demo_system.h"
#include "game/floor_level.h"
#include "game/game_controller.h"
#include "game/game_objects.h"
#include "game/message_system.h"
#include "game/nazo.h"
#include "game/parameters.h"
#include "game/player.h"
#include "game/save_data.h"
#include "game/screen_effects.h"
#include "game/game_plus_arm.h"
#include "game/speedrun.h"
#include "game/script_host.h"
#include "game/stage_manager.h"
#include "game/trap_system.h"

namespace pt::game {

class GameAudio {
public:
    virtual ~GameAudio() = default;
    virtual uint32_t PostEvent(std::string_view name, const glm::vec3* position) = 0;
    virtual uint32_t PostEventId(uint32_t id, const glm::vec3* position) = 0;
    virtual bool IsEventPlaying(std::string_view name) const = 0;
    virtual bool IsPlaying(uint32_t playing_id) const = 0;
    virtual void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) = 0;
    virtual void Update(float dt) = 0;
    virtual void OnStageLoaded(Stage&) {}
    virtual void OnStageUnloading(const Stage&) {}
    virtual void Footstep(bool, const glm::vec3&) {}
    virtual void AnimEvent(std::string_view, uint64_t, const glm::vec3&) {}
    virtual void StopAll() {}
    virtual uint32_t PlayStream(std::vector<uint8_t>, const glm::vec3*) { return 0; }
    virtual uint32_t PostEventMedia(std::string_view, std::vector<uint32_t>, const glm::vec3*) { return 0; }
    virtual void SeekPlayingId(uint32_t, float) {}
    virtual void StopPlayingId(uint32_t, float) {}
    virtual void SetState(std::string_view, std::string_view) {}
    virtual void SetRtpc(std::string_view, float) {}
    virtual uint32_t PostRecordEvent(int, std::string_view name, const glm::vec3& position) { return PostEvent(name, &position); }
    virtual uint32_t PostRecordEventId(int, uint32_t id, const glm::vec3& position) { return PostEventId(id, &position); }
    virtual void MoveRecordSound(int, const glm::vec3&) {}
};

struct HandyReflection {
    bool active = false;
    bool primed = false;
    bool fading = false;
    glm::vec3 position{0.0f};
    glm::vec3 second{0.0f};
    glm::vec3 previous{0.0f};
    glm::vec3 camera{0.0f};
    glm::vec3 forward{0.0f, 0.0f, 1.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float first_weight = 1.0f;
    float second_weight = 0.0f;
    float spot_share = 0.0f;
    float first_attenuation = 0.0f;
    float second_attenuation = 0.0f;
    glm::vec3 samples[4]{};
    glm::vec3 readback{0.0f};
    bool has_readback = false;
    uint32_t readback_age = 0;
};

class StatusFlags {
public:
    void Acquire(const std::string& flag, const std::string& holder) { holders_[flag].insert(holder); }
    void Release(const std::string& flag, const std::string& holder) { holders_[flag].erase(holder); }
    std::string Describe() const {
        std::string text;
        for (const auto& [flag, holders] : holders_) {
            for (const std::string& holder : holders) {
                text += flag + ":" + holder + " ";
            }
        }
        return text;
    }
    bool IsSet(const std::string& flag) const {
        auto it = holders_.find(flag);
        return it != holders_.end() && !it->second.empty();
    }

private:
    std::map<std::string, std::set<std::string>> holders_;
};

struct GameConfig {
    std::filesystem::path save_path;
    bool use_save = true;
    bool first_boot_options = false;
    std::string start_floor;
    uint32_t seed = 0;
    bool trap_log = false;
    int handy_light_roll = 0;
    int bug_screen = -1;
    int street_offer = 0;
    bool release_locks = false;
    bool theater = false;
};

struct ArchiveEntry;

class Game {
public:
    Game(Vfs& vfs, ModelCache& models);
    ~Game();

    bool Init(const GameConfig& config);
    const GameConfig& Config() const { return config_; }
    void Update(float dt, const InputState& input);
    void CollectDraws(std::vector<DrawItem>& out);
    Camera GetCamera() const;
    void SetShowBody(bool on) { show_body_ = on; }
    bool ShowBody() const { return show_body_; }
    bool BodyShown() const { return show_body_ || third_person_shown_; }
    void SetDetachedView(bool on) { detached_view_ = on; }
    bool DetachedView() const { return detached_view_ || third_person_shown_; }
    bool FreeView() const { return detached_view_; }
    void SetThirdPerson(bool on);
    bool ThirdPerson() const { return third_person_; }
    bool ThirdPersonCameraActive() const { return third_person_ && third_person_primed_ && !detached_view_; }
    Camera ViewCamera() const;
    float ThirdPersonWeight() const { return third_person_weight_; }
    Camera ThirdPersonTurn(const Camera& before, const Camera& after) const;
    Camera PeepholeTurn(const Camera& turned) const;
    bool HandyLens(glm::vec3& out) const {
        out = handy_lens_;
        return handy_lens_valid_;
    }
    void RequestPhotoMode(int mode) { photo_request_ = mode == 0 ? 3 : mode; }
    int TakePhotoModeRequest() { const int r = photo_request_; photo_request_ = 0; return r; }
    void RequestFreeCamera(bool on) { freecam_request_ = on ? 1 : 2; }
    int TakeFreeCameraRequest() { return std::exchange(freecam_request_, 0); }
    void RequestPhotoCamera(const glm::vec4& offset_look) { photo_camera_request_ = offset_look; }
    std::optional<glm::vec4> TakePhotoCameraRequest() { auto r = photo_camera_request_; photo_camera_request_.reset(); return r; }
    void RequestPhotoTarget(const glm::vec2& right_forward) { photo_target_ = right_forward; }
    glm::vec2 PhotoTarget() const { return photo_target_; }
    void RequestPhotoView(const glm::vec3& position, const glm::vec3& target) { photo_view_request_ = std::pair(position, target); }
    std::optional<std::pair<glm::vec3, glm::vec3>> TakePhotoViewRequest() { return std::exchange(photo_view_request_, std::nullopt); }
    void RequestPhotoSettings(const std::vector<int>& values) { photo_settings_request_ = values; }
    std::vector<int> TakePhotoSettingsRequest() { auto r = std::move(photo_settings_request_); photo_settings_request_.clear(); return r; }
    void SetCameraOverride(const std::optional<Camera>& camera, bool follows_view = false) {
        camera_override_ = camera;
        camera_override_follows_view_ = camera.has_value() && follows_view;
    }
    glm::vec3 HandyAim() const { return light_aim_; }
    void SetHandyPoseOverride(const std::optional<std::pair<glm::vec3, glm::vec3>>& pose) { handy_pose_override_ = pose; }
    const std::optional<std::pair<glm::vec3, glm::vec3>>& HandyPoseOverride() const { return handy_pose_override_; }
    float HandyPickupHold() const { return handy_hold_; }
    float HandyDemoPose() const { return handy_demo_; }
    const HandyReflection& HandyReflectionState() const { return handy_reflection_; }
    void SetHandyReflectionReadback(const glm::vec3& colour) {
        handy_reflection_.readback = colour;
        handy_reflection_.has_readback = true;
        handy_reflection_.readback_age = 0;
    }

    Vfs& GetVfs() { return vfs_; }
    ModelCache& Models() { return models_; }
    StageManager& Stages() { return stages_; }
    const CollisionWorld& Collision() const { return collision_; }
    const CollisionWorld& LineCollision() const { return line_collision_; }
    uint32_t SurfaceMaterial(const glm::vec3& feet) const;
    const std::vector<SurfaceTriangle>& Surfaces() const { return surfaces_; }
    Player& GetPlayer() { return player_; }
    const Player& GetPlayer() const { return player_; }
    GameObjects& Objects() { return objects_; }
    FloorLevel& Floor() { return floor_; }
    NazoManager& Nazo() { return nazo_; }
    GameController& Controller() { return controller_; }
    MessageSystem& Messages() { return messages_; }
    TrapSystem& Traps() { return traps_; }
    DemoSystem& Demos() { return demos_; }
    ScreenEffects& Effects() { return effects_; }
    ScriptHost& Scripts() { return scripts_; }
    ModLua* ModScripts() { return mod_scripts_.get(); }
    void LoadModScripts(const mods::ModSet& mods);
    StatusFlags& Status() { return status_; }
    const InputState& LastInput() const { return last_input_; }
    uint64_t Frame() const { return frame_; }
    double Time() const { return time_; }

    void SetAudio(GameAudio* audio) { audio_ = audio; }
    GameAudio* Audio() { return audio_; }
    uint32_t PostSound(std::string_view event, const glm::vec3& position, bool positional);
    void PostSoundId(uint32_t id);
    uint32_t PostSoundIdAt(uint32_t id, const glm::vec3& position);
    uint32_t PostSoundIdOnOcho(uint32_t id, const glm::vec3& position);
    void MoveOchoSound(const glm::vec3& position);
    void RegisterAnimEvent(std::string_view anim_event, std::string_view sound_event);
    static constexpr float kOriginalFrameRate = 30.0f;
    float FrameDelta() const { return frame_delta_; }
    void RequestScreenshot(std::string path) { screenshot_requests_.push_back(std::move(path)); }
    void RequestQuit() { quit_requested_ = true; }
    bool QuitRequested() const { return quit_requested_; }
    std::vector<std::string> TakeScreenshotRequests() { return std::exchange(screenshot_requests_, {}); }
    float OriginalFrames() const { return frame_delta_ * kOriginalFrameRate; }
    bool GameFrameTick() const { return game_frame_tick_; }
    void FloorEnvironment();
    bool IsSoundPlaying(uint32_t playing_id) const;
    void ShowCaption(uint32_t message_id);
    bool InViewNdc(const glm::vec3& position, float area) const;
    void SetMirrorViewportBit(int bit, bool on);
    uint32_t MirrorViewportBits() const { return mirror_viewport_bits_; }
    GameParameters& Parameters() { return parameters_; }
    void CallBgm(std::string_view event);
    void StopBgm(std::string_view event);
    bool IsPlayingBgm(std::string_view event) const;

    void SendControllerMessage(std::string_view message) { messages_.PostControllerMessage(message); }
    struct SubtitleRequest {
        std::string subtitle_id;
        float offset_seconds = 0.0f;
        uint32_t sound = 0;
    };
    void QueueSubtitle(std::string_view subtitle_id, float offset_seconds = 0.0f, uint32_t sound = 0) {
        subtitle_requests_.push_back({std::string(subtitle_id), offset_seconds, sound});
    }
    std::vector<SubtitleRequest> TakeSubtitleRequests() { return std::exchange(subtitle_requests_, {}); }
    void ClearPendingSubtitles();
    bool TakeSubtitleClear() { return std::exchange(clear_subtitles_pending_, false); }
    void SetSubtitleVisible(std::string_view message_id, bool visible);
    void SetFloorLighting(int row);
    int FloorLightingRow() const { return lighting_row_; }
    void ApplyStageTransform();
    float PlayerHeightAboveStageRoot() const;

    void ResetSaveBlocks();
    void RequestSave();
    void RequestOptionsSave();
    bool BrowseLoop(int index);
    bool LoopReloadPending() const { return loop_reload_pending_; }
    bool ApplyBrowseReload();
    void OnBrowseFloorEntered();
    void OnBasementExit();
    void EndBrowseSuppression(std::string_view why);
    bool BrowseSaveSuppressed() const { return browse_save_suppressed_; }
    bool BrowseEntryPending() const { return browse_waiting_entry_ && browse_loop_ > 0 && !loop_reload_pending_; }
    void BrowseEnterHallway();
    bool BrowseArrived() const { return browse_arrived_; }
    bool BrowseLightPickedUp(std::string_view floor) const;
    bool SaveInFlight(double seconds) const { return time_ - last_save_request_ < seconds; }
    bool ResetProgress();
    void ResetSessionState();
    void CaptureBootEffects() { boot_effects_ = effects_; }
    std::string DescribeSessionState() const;
    bool StartStreetWalk();
    void UpdateStreetWalk(float dt);
    bool StreetWalkActive() const { return street_walk_; }
    bool StreetPending() const { return street_pending_; }
    bool OfferStreetWalk();
    bool StreetOfferPending() const { return street_offer_ == StreetOffer::Asking; }
    void AnswerStreetOffer(bool walk);
    void UpdateStreetOffer();
    void StartPortCredits();
    bool UpdatePortCredits(float dt);
    void SkipPortCredits();
    bool PortCreditsActive() const { return port_credits_time_ >= 0.0f; }
    float PortCreditsTime() const { return port_credits_time_; }
    bool TakeMenuCloseRequest() { return std::exchange(menu_close_request_, false); }
    void LeaveStreetWalk();
    void EndStreetWalk();
    void SetBrowseUnlocks(uint32_t reached, bool finished) { browse_reached_ = reached; game_finished_ = finished; }
    uint32_t BrowseReached() const { return browse_reached_; }
    bool BrowseUnlocked(int index) const {
        return !config_.release_locks || (game_finished_ && index >= 0 && index < 32 && (browse_reached_ & (1u << index)) != 0);
    }
    bool BrowseLocksOn() const { return config_.release_locks; }
    void NoteArchive(std::string_view key);
    void SetArchiveSeen(std::set<std::string> keys) { archive_seen_ = std::move(keys); }
    const std::set<std::string>& ArchiveSeen() const { return archive_seen_; }
    bool ArchiveUnlocked(const ArchiveEntry& entry) const;
    uint32_t ArchiveGeneration() const { return archive_generation_; }
    void RequestArchive(std::string_view id) { archive_request_ = std::string(id); }
    std::string TakeArchiveRequest() { return std::exchange(archive_request_, std::string()); }
    bool ArchiveRequestPending() const { return !archive_request_.empty(); }
    void SetArchiveTheaterActive(bool on) { archive_theater_active_ = on; }
    bool ArchiveTheaterActive() const { return archive_theater_active_; }
    void SetArchiveTheaterShowing(bool on) { archive_theater_showing_ = on; }
    bool ArchiveTheaterShowing() const { return archive_theater_showing_; }
    struct TheaterControl {
        bool traps = true;
        bool ocho = true;
        bool opening = false;
    };
    TheaterControl& Theater() { return theater_; }
    void PrepareTheaterLoop(int index);
    bool GameFinished() const { return game_finished_; }
    uint32_t BrowseUnlockGeneration() const { return browse_unlock_generation_; }
    void NoteBrowseReached(int index);
    void NoteGameFinished();
    void OnFloorReached(std::string_view floor, int pass);
    void EnableGamePlus();
    bool GamePlus() const { return game_plus_; }
    uint32_t Finishes() const { return finishes_; }
    int GamePlusTier() const;
    static constexpr int kTierStreetAmbience = 3;
    static constexpr int kTierMazeMusic = 3;
    static constexpr int kTierBathroomMusic = 3;
    static constexpr int kTierRadioLines = 3;
    static constexpr int kTierHeartbeat = 3;
    static constexpr int kTierLaughs = 3;
    static constexpr int kTierDemos = 4;
    void OnTrapDemo(std::string_view demo_id, const glm::mat4& origin, bool loop);
    bool GamePlusTubShown() const;
    void AddMockupDraw(const GpuMesh* mesh, const glm::mat4& world) { mockup_draws_.push_back({mesh, world}); }
    void ClearMockupDraws() { mockup_draws_.clear(); }
    bool ProgressResetPending() const { return progress_reset_pending_; }
    SpeedrunTimer& Speedrun() { return speedrun_; }
    const SpeedrunTimer& Speedrun() const { return speedrun_; }
    void SpeedrunStartGame();
    void SpeedrunFloorLeft(std::string_view floor, int pass);
    void SpeedrunEnding();
    bool SpeedrunResultsPending() const {
        return StreetOfferPending() && speedrun_.Enabled() && speedrun_.GetState() == SpeedrunTimer::State::Finished;
    }
    void AnswerSpeedrunMenu();
    bool TakeOptionsAfterRestart() { return std::exchange(options_after_restart_, false); }
    bool SavesEnabled() const { return save_store_.Enabled(); }
    void RequestLoad();
    bool SaveBusy() const { return save_dialog_.has_value(); }
    struct SaveDialog {
        std::string key;
        enum class Then { Nothing, NewSave, Retry } then = Then::Nothing;
    };
    const std::optional<SaveDialog>& PendingSaveDialog() const { return save_dialog_; }
    void CloseSaveDialog();
    uint32_t SaveIoCount() const { return save_io_count_; }
    bool SaveIoLoading() const { return save_io_loading_; }
    bool ShowOptionMenuOnFirstBoot() const { return true; }
    bool OptionsUiAvailable() const { return options_ui_; }
    void SetOptionsUiAvailable(bool available) { options_ui_ = available; }
    GameOptions& Options() { return options_; }

    void SetPaused(bool paused) { paused_ = paused; }
    bool Paused() const { return paused_; }
    void SetVoiceListening(bool listening) { voice_listening_ = listening; }
    bool VoiceListening() const { return voice_listening_; }
    void OnVoiceKeyword(std::string_view keyword) { nazo_.OnVoiceKeyword(keyword); }

private:
    std::optional<Camera> camera_override_;
    bool camera_override_follows_view_ = false;
    glm::vec3 light_target_{0.0f};
    glm::vec3 light_aim_{0.0f};
    std::optional<std::pair<glm::vec3, glm::vec3>> handy_pose_override_;
    glm::vec3 light_origin_{0.0f};
    glm::vec3 light_camera_{0.0f};
    glm::vec3 light_forward_{0.0f, 0.0f, 1.0f};
    bool light_target_valid_ = false;
    float handy_hold_ = 0.0f;
    bool handy_hold_pending_ = false;
    bool handy_hold_release_ = false;
    float handy_demo_ = 0.0f;
    HandyReflection handy_reflection_;
    void OnStageLoaded(Stage& stage);
    void OnStageUnloading(Stage& stage);
    void SpawnPlayer(Stage& stage);
    void UpdateBlurSway();
    void TearDownSession();
    void UpdateHandyTarget(float dt);
    void UpdateHandyReflection(float dt);
    int FoxRandom(int max, int min);
    void RunSetupScript(std::string_view archive_path);
    void AddMirrorBody(std::vector<DrawItem>& out);
    void UpdateThirdPerson(float dt);
    glm::vec3 ThirdPersonHead() const;
    glm::vec3 ThirdPersonPosition(float yaw, float pitch) const;
    bool show_body_ = false;
    bool detached_view_ = false;
    bool third_person_ = false;
    bool third_person_shown_ = false;
    float third_person_weight_ = 0.0f;
    float third_person_distance_ = 0.0f;
    float third_person_shoulder_ = 0.0f;
    float third_person_lift_ = 0.0f;
    bool third_person_blocked_ = false;
    bool third_person_primed_ = false;
    float third_person_tight_ = 0.0f;
    float third_person_aim_ = 12.0f;
    std::vector<glm::vec3> characters_;
    glm::vec3 handy_lens_{0.0f};
    bool handy_lens_valid_ = false;
    glm::vec3 handy_origin_{0.0f};
    glm::vec3 handy_direction_{0.0f};
    int photo_request_ = 0;
    int freecam_request_ = 0;
    std::optional<glm::vec4> photo_camera_request_;
    std::optional<std::pair<glm::vec3, glm::vec3>> photo_view_request_;
    std::vector<int> photo_settings_request_;
    glm::vec2 photo_target_{0.0f};

    Vfs& vfs_;
    ModelCache& models_;
    GameConfig config_;
    StageManager stages_;
    std::string prefetch_floor_;
    uint32_t prefetch_loads_ = 0;
    CollisionWorld collision_;
    CollisionWorld line_collision_;
    CollisionWorld reflection_collision_;
    std::vector<SurfaceTriangle> surfaces_;
    uint64_t collision_generation_ = 0;
    uint64_t geom_generation_ = 0;
    uint64_t collision_take_frame_ = 0;
    uint64_t draw_generation_ = 0;
    std::vector<DrawItem> draws_;
    std::vector<glm::mat4> player_skin_;
    std::unique_ptr<GpuMesh> handy_lens_mesh_;
    int32_t handy_lens_material_ = -1;
    bool handy_model_readable_ = false;
    uint64_t handy_model_probe_frame_ = 0;
    Player player_;
    GameObjects objects_;
    FloorLevel floor_;
    NazoManager nazo_;
    GameController controller_;
    MessageSystem messages_;
    TrapSystem traps_;
    DemoSystem demos_;
    ScreenEffects effects_;
    ScreenEffects boot_effects_;
    ScriptHost scripts_;
    std::unique_ptr<ModLua> mod_scripts_;
    StatusFlags status_;
    GameAudio* audio_ = nullptr;
    std::set<std::string> bgm_playing_;
    InputState last_input_;
    uint64_t frame_ = 0;
    double time_ = 0.0;
    double last_save_request_ = -1e9;
    std::optional<SaveProgress> saved_progress_;
    std::optional<SaveDialog> save_dialog_;
    int save_retries_ = 0;
    void OnSaveWritten(bool ok);
    double grain_time_ = 0.0;
    bool game_frame_tick_ = false;
    bool zoom_fell_ = false;
    uint32_t grain_seed_ = 0x6C078965u;
    int lighting_row_ = 0;
    uint32_t mirror_viewport_bits_ = 0;
    GameParameters parameters_;
    SaveStore save_store_;
    uint32_t save_io_count_ = 0;
    bool save_io_loading_ = false;
    GameOptions options_;
    bool options_ui_ = false;
    int previous_foot_steps_ = 0;
    float trap_dt_ = 0.0f;
    int previous_sounding_steps_ = 0;
    std::map<uint64_t, std::string> anim_sounds_;
    bool next_foot_left_ = true;
    float frame_delta_ = 1.0f / 60.0f;
    std::vector<std::string> screenshot_requests_;
    bool quit_requested_ = false;
    bool progress_reset_pending_ = false;
    bool loop_reload_pending_ = false;
    float browse_fade_wait_ = 0.0f;
    bool browse_save_suppressed_ = false;
    bool browse_waiting_entry_ = false;
    bool browse_door_placed_ = false;
    int browse_door_wait_ = 0;
    bool browse_arrived_ = true;
    int browse_loop_ = -1;
    bool street_walk_ = false;
    bool street_pending_ = false;
    enum class StreetOffer { None, Asking, Walk, Restart };
    StreetOffer street_offer_ = StreetOffer::None;
    bool menu_close_request_ = false;
    float port_credits_time_ = -1.0f;
    bool port_credits_skip_ = false;
    glm::vec3 street_safe_feet_{0.0f};
    float street_safe_wait_ = 0.0f;
    uint32_t street_rain_ = 0;
    uint32_t browse_reached_ = 0;
    bool game_finished_ = false;
    uint32_t browse_unlock_generation_ = 0;
    bool game_plus_ = false;
    SpeedrunTimer speedrun_;
    bool options_after_restart_ = false;
    const GpuMesh* tub_lisa_mesh_ = nullptr;
    void UpdateGamePlusSounds();
    void UpdateGamePlusDemos();
    std::string gp_man_floor_;
    int gp_man_pass_ = 0;
    uint64_t gp_man_since_ = 0;
    bool gp_man_done_ = false;
    void StopGamePlusSound(uint32_t& id, float fade);
    uint32_t gp_street_ = 0;
    uint32_t gp_maze_ = 0;
    uint32_t gp_unrest_ = 0;
    bool gp_unrest_wanted_ = false;
    uint32_t gp_heartbeat_ = 0;
    bool gp_heartbeat_armed_ = false;
    uint32_t gp_radio_line_ = 0;
    int gp_radio_stage_ = 0;
    bool gp_radio_seen_ = false;
    std::string gp_radio_floor_;
    int gp_radio_pass_ = 0;
    uint32_t gp_laugh_ = 0;
    bool gp_laugh_near_ = false;
    int gp_laugh_visits_ = 0;
    uint32_t finishes_ = 0;
    GamePlusArm game_plus_arm_;
    std::vector<std::pair<const GpuMesh*, glm::mat4>> mockup_draws_;
    void UpdateGamePlus();
    bool locators_dirty_ = false;
    bool voice_listening_ = false;
    BlurSway blur_sway_;
    uint32_t fox_rng_ = 0x6C078965u;
    bool paused_ = false;
    std::set<std::string> archive_seen_;
    uint32_t archive_generation_ = 0;
    std::string archive_request_;
    bool archive_theater_active_ = false;
    bool archive_theater_showing_ = false;
    TheaterControl theater_;
    std::vector<SubtitleRequest> subtitle_requests_;
    bool clear_subtitles_pending_ = false;
};

}
