#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
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
#include "game/run_stride.h"
#include "game/ui/crosshair.h"
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
#include "game/sound_captions.h"
#include "game/screen_effects.h"
#include "game/game_plus_arm.h"
#include "game/game_plus_intercom.h"
#include "game/speedrun.h"
#include "game/script_host.h"
#include "game/stage_manager.h"
#include "game/survival/survival.h"
#include "game/survival/street_house.h"
#include "game/trap_system.h"

namespace pt::audio {
class Media;
}

namespace pt::game {

class GameAudio {
public:
    virtual ~GameAudio() = default;
    virtual uint32_t PostEvent(std::string_view name, const glm::vec3* position) = 0;
    virtual uint32_t PostEventId(uint32_t id, const glm::vec3* position) = 0;
    virtual bool IsEventPlaying(std::string_view name) const = 0;
    virtual bool IsPlaying(uint32_t playing_id) const = 0;
    virtual bool IsEventIdPlaying(uint32_t) const { return false; }
    virtual void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) = 0;
    virtual void Update(float dt) = 0;
    virtual void OnStageLoaded(Stage&) {}
    virtual void OnStageUnloading(const Stage&) {}
    virtual void Footstep(bool, const glm::vec3&) {}
    virtual void AnimEvent(std::string_view, uint64_t, const glm::vec3&) {}
    virtual void StopAll() {}
    virtual void ResetSession() {}
    virtual uint32_t PlayStream(std::vector<uint8_t>, const glm::vec3*) { return 0; }
    virtual uint32_t PlayStreamAs(std::vector<uint8_t>, std::string_view, const glm::vec3&) { return 0; }
    virtual uint32_t PlayMedia(std::shared_ptr<const audio::Media>, const glm::vec3*) { return 0; }
    virtual uint32_t PlayMediaAs(std::shared_ptr<const audio::Media>, uint32_t, const glm::vec3&) { return 0; }
    virtual std::shared_ptr<const audio::Media> FindMedia(uint32_t) const { return nullptr; }
    virtual uint32_t PostEventMedia(std::string_view, std::vector<uint32_t>, const glm::vec3*) { return 0; }
    virtual void SeekPlayingId(uint32_t, float) {}
    virtual void StopPlayingId(uint32_t, float) {}
    virtual void SetState(std::string_view, std::string_view) {}
    virtual void SetRtpc(std::string_view, float) {}
    virtual uint32_t PostRecordEvent(int, std::string_view name, const glm::vec3& position) { return PostEvent(name, &position); }
    virtual uint32_t PostRecordEventId(int, uint32_t id, const glm::vec3& position) { return PostEventId(id, &position); }
    virtual void MoveRecordSound(int, const glm::vec3&) {}
    virtual void SetPostObserver(std::function<void(uint32_t, const std::optional<glm::vec3>&)>) {}
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
class LisaDate;

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
    void SetTracedBody(bool on) { traced_body_ = on; }
    bool ShowBody() const { return show_body_; }
    bool BodyShown() const { return show_body_ || (third_person_shown_ && !camera_override_); }
    void SetDetachedView(bool on) { detached_view_ = on; }
    bool DetachedView() const { return detached_view_ || third_person_shown_; }
    bool FreeView() const { return detached_view_; }
    void SetThirdPerson(bool on);
    bool ThirdPerson() const { return third_person_; }
    void SetThirdOrbit(const glm::vec4& orbit) { third_orbit_ = orbit; }
    bool ThirdPersonCameraActive() const { return third_person_ && third_person_primed_ && !detached_view_; }
    Camera ViewCamera(bool fov_offset = true) const;
    Camera ViewCameraBase(bool fov_offset) const;
    void SetFovOffset(int degrees) { fov_offset_ = static_cast<float>(degrees); }
    void SetHouseSprint(bool on) {
        house_sprint_ = on;
        if (!on && !street_walk_) ResetStreetSprint();
    }
    bool HouseSprintEnabled() const { return house_sprint_; }
    float FovOffsetNow() const;
    Camera PlayerViewCamera() const;
    float ThirdPersonWeight() const { return third_person_weight_; }
    float ThirdPersonBodyShare() const;
    bool ThirdPersonShown() const { return third_person_shown_; }
    Camera ThirdPersonTurn(const Camera& before, const Camera& after) const;
    glm::mat4 ThirdPersonBodyTurn(const Camera& before, const Camera& after) const;
    Camera PeepholeTurn(const Camera& turned) const;
    void SetHandyLensRadiance(const glm::vec3& radiance) { handy_lens_radiance_ = radiance; }
    bool HandyLens(glm::vec3& out) const {
        out = handy_lens_;
        return handy_lens_valid_;
    }
    void HandyBeam(glm::vec3& origin, glm::vec3& direction) const {
        origin = handy_origin_;
        direction = handy_direction_;
    }
    void RequestPhotoMode(int mode) { photo_request_ = mode == 0 ? 3 : mode; }
    int TakePhotoModeRequest() { const int r = photo_request_; photo_request_ = 0; return r; }
    void RequestFreeCamera(bool on) { freecam_request_ = on ? 1 : 2; }
    int TakeFreeCameraRequest() { return std::exchange(freecam_request_, 0); }
    void RequestNeuralRendering(int value) { neural_request_ = value + 1; }
    int TakeNeuralRenderingRequest() { return std::exchange(neural_request_, 0); }
    void RequestPhotoCamera(const glm::vec4& offset_look) { photo_camera_request_ = offset_look; }
    std::optional<glm::vec4> TakePhotoCameraRequest() { auto r = photo_camera_request_; photo_camera_request_.reset(); return r; }
    void RequestPhotoTarget(const glm::vec2& right_forward) { photo_target_ = right_forward; }
    glm::vec2 PhotoTarget() const { return photo_target_; }
    void RequestPhotoView(const glm::vec3& position, const glm::vec3& target) { photo_view_request_ = std::pair(position, target); }
    std::optional<std::pair<glm::vec3, glm::vec3>> TakePhotoViewRequest() { return std::exchange(photo_view_request_, std::nullopt); }
    void RequestPhotoSettings(const std::vector<int>& values) { photo_settings_request_ = values; }
    void RequestPhotoGrid(int grid) { photo_grid_request_ = grid; }
    int TakePhotoGridRequest() { const int r = photo_grid_request_; photo_grid_request_ = -1; return r; }
    void RequestPhotoTake() { photo_take_request_ = true; }
    bool TakePhotoTakeRequest() { const bool r = photo_take_request_; photo_take_request_ = false; return r; }
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
    glm::vec2 HandyBreath() const { return {handy_breath_time_, handy_breath_weight_}; }
    float HandyMaskRoll() const { return handy_mask_roll_; }
    glm::vec2 HandyPickupMaskRoll() const { return {handy_pickup_roll_, handy_pickup_weight_}; }
    const HandyReflection& HandyReflectionState() const { return handy_reflection_; }
    void SetHandyReflectionReadback(const glm::vec3& colour) {
        handy_reflection_.readback = colour;
        handy_reflection_.has_readback = true;
        handy_reflection_.readback_age = 0;
    }

    Vfs& GetVfs() { return vfs_; }
    ModelCache& Models() { return models_; }
    int32_t HandyLensMaterial() const { return handy_lens_material_; }
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

    void SetAudio(GameAudio* audio);
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
    void SetSoundCaptions(bool on);
    SoundCaptions& Captions() { return captions_; }
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
    bool BrowseSurvival();
    bool NightDoorPending() const { return browse_night_door_; }
    bool OpenNightDoor();
    bool OpenNightExit();
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
    void ToggleStreetFlashlight();
    void ToggleStreetFlashlightImmediate();
    bool StreetPending() const { return street_pending_; }
    bool OfferStreetWalk();
    bool StreetOfferPending() const { return street_offer_ == StreetOffer::Asking; }
    void RequestMenuClose() { menu_close_request_ = true; }
    void RequestSurvivalHub() { survival_hub_request_ = true; }
    void AnswerStreetOffer(bool walk);
    void UpdateStreetOffer();
    void StartPortCredits();
    bool UpdatePortCredits(float dt);
    void SkipPortCredits();
    bool PortCreditsActive() const { return port_credits_time_ >= 0.0f; }
    float PortCreditsTime() const { return port_credits_time_; }
    bool TakeMenuCloseRequest() { return std::exchange(menu_close_request_, false); }
    bool TakeSurvivalHubRequest() { return std::exchange(survival_hub_request_, false); }
    void LeaveStreetWalk();
    void EndStreetWalk();
    void UpdateStreetSprint(float dt, const InputState& input);
    void ResetStreetSprint();
    float StreetRunWeight() const;
    static constexpr int kTierSurvival = 4;
    bool NightUnlocked() const;
    bool NightUnlockKept() const { return night_unlocked_; }
    void SetNightUnlocked(bool on) { night_unlocked_ = on; }
    void UnlockNight(std::string_view why);
    bool SurvivalAvailable() const { return NightUnlocked(); }
    bool SurvivalWithWalk() const { return (ending_tier_ >= 0 ? ending_tier_ : GamePlusTier()) >= kTierSurvival && NightUnlocked(); }
    void NoteEndingTier() { ending_tier_ = GamePlusTier(); }
    void RequestSurvival() { survival_requested_ = true; }
    void SetSurvivalStart(SurvivalStart start) { survival_start_ = start; }
    void SetSurvivalProfile(const std::string& text) { survival_profile_.Load(text); }
    SurvivalProfile& SurvivalProfileData() { return survival_profile_; }
    const SurvivalProfile& SurvivalProfileData() const { return survival_profile_; }
    void MarkSurvivalProfile() { survival_profile_dirty_ = true; }
    std::optional<std::string> TakeSurvivalProfile() {
        if (!std::exchange(survival_profile_dirty_, false)) return std::nullopt;
        return survival_profile_.Save();
    }
    void StartSurvival();
    void StopSurvival();
    bool SurvivalActive() const { return survival_ && survival_->Active(); }
    const SurvivalNight* Survival() const { return survival_.get(); }
    SurvivalNight* EnsureSurvival();
    bool SurvivalChoosing() const { return SurvivalActive() && survival_->Choosing(); }
    void ChooseSurvivalOffer(int index) {
        if (SurvivalActive()) survival_->ChooseOffer(index);
    }
    void SurvivalAgentInput(InputState& input) {
        if (SurvivalActive()) survival_->AgentInput(input);
    }
    void SurvivalAct(const std::string& action) {
        if (survival_ && survival_->Active()) survival_->Act(action);
    }
    const StreetHouse* House() const { return street_walk_ ? house_.get() : nullptr; }
    bool FinishIntoNight();
    void SetSurvivalBest(int wave) { survival_best_ = wave; }
    void SetSurvivalDifficulty(int difficulty) { survival_difficulty_ = difficulty; }
    int SurvivalDifficultyLevel() const { return survival_difficulty_; }
    void SetCrosshair(const CrosshairStyle& style) { crosshair_ = style; }
    const CrosshairStyle& Crosshair() const { return crosshair_; }
    bool SurvivalPlayed() const { return survival_best_ > 0 || SurvivalActive(); }
    int TakeSurvivalRecord();
    void SetBrowseUnlocks(uint32_t reached, bool finished) { browse_reached_ = reached; game_finished_ = finished; }
    uint32_t BrowseReached() const { return browse_reached_; }
    bool BrowseUnlocked(int index) const {
        return !config_.release_locks || (game_finished_ && index >= 0 && index < 32 && (browse_reached_ & (1u << index)) != 0);
    }
    bool BrowseLocksOn() const { return config_.release_locks; }
    void NoteArchive(std::string_view key);
    void LogModInspect(const Camera& camera);
    void LogCapturePose(const Camera& camera);
    void SetArchiveSeen(std::set<std::string> keys) { archive_seen_ = std::move(keys); }
    const std::set<std::string>& ArchiveSeen() const { return archive_seen_; }
    bool ArchiveUnlocked(const ArchiveEntry& entry) const;
    std::vector<std::string> BackfillArchive(uint32_t finishes);
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
    void ReleaseUnusedAssets(std::string_view why);
    bool VideoMemoryTight();
    static constexpr float kEndingSkipHold = 1.5f;
    void UpdateEndingSkip(bool pressed, bool held, float dt, bool blocked);
    float EndingSkipAlpha() const { return ending_skip_alpha_; }
    float EndingSkipProgress() const { return ending_skip_hold_ / kEndingSkipHold; }
    void OnFloorReached(std::string_view floor, int pass);
    void EnableGamePlus();
    void PersistGamePlusFinish();
    bool GamePlus() const { return game_plus_; }
    uint32_t Finishes() const { return finishes_; }
    int GamePlusNoticeDue();
    int CurrentLoopIndex() const { return current_loop_index_; }
    void TakeGamePlusNotice(int tier);
    int GamePlusTier() const;
    void SetGamePlusSetting(bool on, bool now);
    bool GamePlusSetting() const { return game_plus_setting_; }
    void LatchGamePlus();
    static constexpr int kTierStreetAmbience = 3;
    static constexpr int kTierMazeMusic = 3;
    static constexpr int kTierBathroomMusic = 3;
    static constexpr int kTierRadioLines = 3;
    static constexpr int kTierHeartbeat = 3;
    static constexpr int kTierLaughs = 3;
    static constexpr int kTierDemos = 4;
    void OnTrapDemo(std::string_view demo_id, const glm::mat4& origin, bool loop);
    static constexpr int kTierIntercom = 2;
    bool GamePlusIntercomLight(glm::vec3& position, glm::vec3& direction, glm::mat4& area, bool& has_area, float& level) const;
    void IntercomScript(std::string_view what);
    int IntercomShown() const;
    bool IntercomActive() const { return intercom_.phase != intercom::Play::Phase::Idle; }
    bool GamePlusTubShown() const;
    bool GamePlusTubView(glm::vec3& eye, glm::vec3& target) const;
    void OnLisaRush(std::string_view how);
    void LisaDateFloorEntered(std::string_view from, std::string_view floor);
    int LisaRushes() const { return lisa_rushes_; }
    void SetLisaRushesForTest(int rushes);
    bool LisaPortraitPosition(glm::vec3& out, glm::vec3* normal = nullptr) const;
    glm::vec3 LisaHintLightColour() const { return lisa_hint_colour_; }
    bool LisaHintGlint(glm::vec3& position, glm::vec3& normal, float& strength) const;
    float LisaHintVignette() const { return lisa_vignette_; }
    void RequestLisaDate(std::string cause) { lisa_date_request_ = std::move(cause); }
    std::optional<std::string> TakeLisaDateRequest() { return std::exchange(lisa_date_request_, std::nullopt); }
    void SetLisaDateScene(LisaDate* date) { lisa_date_scene_ = date; }
    LisaDate* LisaDateScene() const { return lisa_date_scene_; }
    bool LisaDateActive() const { return lisa_date_scene_ != nullptr; }
    void RequestLeaveLisaDate() { leave_lisa_date_ = true; }
    bool TakeLeaveLisaDateRequest() { return std::exchange(leave_lisa_date_, false); }
    void HoldSaves(bool hold);
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
    void SetBootHold(bool hold) { boot_hold_ = hold; }
    bool BootHold() const { return boot_hold_; }
    struct SaveDialog {
        std::string key;
        enum class Then { Nothing, NewSave, Retry } then = Then::Nothing;
    };
    const std::optional<SaveDialog>& PendingSaveDialog() const { return save_dialog_; }
    void CloseSaveDialog();
    void ShowNoticeDialog(std::string key) { save_dialog_ = SaveDialog{std::move(key), SaveDialog::Then::Nothing}; }
    uint32_t SaveIoCount() const { return save_io_count_; }
    bool SaveIoLoading() const { return save_io_loading_; }
    uint32_t SaveLoads() const { return save_loads_; }
    bool ResumedThresholdArtifact() const { return resumed_threshold_ && floor_.CurrentFloorName() == "f100"; }
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
    float handy_breath_time_ = 0.0f;
    float handy_breath_weight_ = 0.0f;
    glm::vec3 handy_breath_feet_{0.0f};
    float handy_mask_roll_ = 0.0f;
    float handy_mask_roll_held_ = 0.0f;
    int handy_mask_clip_ = -1;
    float handy_pickup_roll_ = 0.0f;
    float handy_pickup_weight_ = 0.0f;
    double handy_pickup_frame_ = -1.0;
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
    bool reach_snap_ = true;
    bool detached_view_ = false;
    bool third_person_ = false;
    float fov_offset_ = 0.0f;
    bool third_person_shown_ = false;
    float third_person_weight_ = 0.0f;
    glm::vec4 third_orbit_ = ThirdOrbitFromEnvironment();
    static glm::vec4 ThirdOrbitFromEnvironment();
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
    int neural_request_ = 0;
    std::optional<glm::vec4> photo_camera_request_;
    std::optional<std::pair<glm::vec3, glm::vec3>> photo_view_request_;
    std::vector<int> photo_settings_request_;
    int photo_grid_request_ = -1;
    bool photo_take_request_ = false;
    bool traced_body_ = false;
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
    glm::vec3 handy_lens_radiance_{0.0f};
    MaterialGpu handy_lens_gpu_{};
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
    SoundCaptions captions_;
    std::set<std::string> bgm_playing_;
    InputState last_input_;
    glm::vec2 demo_light_mouse_{0.0f};
    uint64_t frame_ = 0;
    double time_ = 0.0;
    double last_save_request_ = -1e9;
    std::optional<SaveProgress> saved_progress_;
    std::optional<SaveDialog> save_dialog_;
    bool boot_hold_ = false;
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
    uint32_t save_loads_ = 0;
    bool resumed_threshold_ = false;
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
    bool house_sprint_ = false;
    bool street_sprinting_ = false;
    float street_speed_ = 1.0f;
    float street_bob_phase_ = 0.0f;
    run_stride::SteadyEye run_eye_;
    float street_bob_weight_ = 0.0f;
    int street_bob_steps_ = 0;
    enum class StreetOffer { None, Asking, Walk, Restart };
    StreetOffer street_offer_ = StreetOffer::None;
    bool menu_close_request_ = false;
    bool survival_hub_request_ = false;
    float port_credits_time_ = -1.0f;
    bool port_credits_skip_ = false;
    glm::vec3 street_safe_feet_{0.0f};
    float street_safe_wait_ = 0.0f;
    uint32_t street_rain_ = 0;
    std::unique_ptr<SurvivalNight> survival_;
    bool survival_requested_ = false;
    std::unique_ptr<StreetHouse> house_;
    int ending_tier_ = -1;
    bool night_unlocked_ = false;
    int survival_best_ = 0;
    int survival_difficulty_ = 1;
    CrosshairStyle crosshair_;
    bool browse_survival_ = false;
    bool browse_night_door_ = false;
    float night_door_wait_ = -1.0f;
    float night_exit_wait_ = -1.0f;
    int survival_record_ = 0;
    SurvivalStart survival_start_ = SurvivalStart::Fresh;
    SurvivalProfile survival_profile_;
    bool survival_profile_dirty_ = false;
    uint32_t browse_reached_ = 0;
    bool game_finished_ = false;
    uint32_t browse_unlock_generation_ = 0;
    bool game_plus_ = false;
    bool game_plus_setting_ = true;
    bool game_plus_active_ = true;
    SpeedrunTimer speedrun_;
    bool options_after_restart_ = false;
    const GpuMesh* tub_lisa_mesh_ = nullptr;
    bool ending_skip_armed_ = false;
    float ending_skip_hold_ = 0.0f;
    float ending_skip_alpha_ = 0.0f;
    void UpdateGamePlusSounds();
    void UpdateGamePlusDemos();
    void UpdateGamePlusIntercom(float dt);
    bool SwapIntercom(Stage& stage, bool show, intercom::Play::Site& site);
    void StartIntercom(intercom::Play::Phase phase, const intercom::Play::Site& site, std::string_view why);
    void StopIntercom(std::string_view why);
    void AddIntercomDraw(std::vector<DrawItem>& out) const;
    const intercom::Play::Site* IntercomSite(bool inside_only) const;
    intercom::Play intercom_;
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
    uint32_t gameplus_announced_ = 0;
    bool gameplus_door_seen_ = false;
    int current_loop_index_ = -1;
    GamePlusArm game_plus_arm_;
    std::vector<std::pair<const GpuMesh*, glm::mat4>> mockup_draws_;
    void UpdateGamePlus();
    bool UpdateLisaPortrait(float dt);
    bool InViewHeldZoom(const glm::vec3& position, float area) const;
    void UpdateLisaHints(float dt);
    void LisaRushCounted(int rush);
    void LisaHintsOff(std::string_view why, bool sad);
    void PlayLisaHint(int kind, int rush);
    void LoadLisaHintSounds();
    int lisa_hint_level_ = 0;
    bool lisa_hint_lit_ = false;
    glm::vec3 lisa_hint_colour_{1.0f};
    bool lisa_hint_aside_ = false;
    float lisa_glint_ = 0.0f;
    float lisa_glint_time_ = 0.0f;
    glm::vec3 lisa_glint_at_{0.0f};
    glm::vec3 lisa_glint_normal_{0.0f, 0.0f, 1.0f};
    bool lisa_glint_valid_ = false;
    float lisa_vignette_ = 0.0f;
    float lisa_heart_wait_ = 0.0f;
    int lisa_heart_beats_ = 0;
    float lisa_heart_pulse_ = 0.0f;
    uint32_t lisa_heart_id_ = 0;
    int lisa_sting_pending_ = 0;
    float lisa_sting_wait_ = 0.0f;
    bool lisa_hint_sounds_loaded_ = false;
    std::vector<std::vector<uint8_t>> lisa_sting_wav_;
    std::vector<uint8_t> lisa_heart_wav_;
    std::vector<uint8_t> lisa_sad_wav_;
    int lisa_rushes_ = 0;
    bool lisa_rush_counted_ = false;
    float lisa_portrait_time_ = 0.0f;
    bool lisa_portrait_armed_ = false;
    float lisa_portrait_lost_ = 0.0f;
    bool lisa_portrait_release_ = false;
    std::optional<std::string> lisa_date_request_;
    LisaDate* lisa_date_scene_ = nullptr;
    bool leave_lisa_date_ = false;
    bool saves_held_ = false;
    bool save_deferred_ = false;
    bool options_save_deferred_ = false;
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
