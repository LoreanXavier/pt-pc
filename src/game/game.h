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
    // a new session (Reset Progress, the loop browser): everything the last one set on the sound engine goes back to the boot's
    virtual void ResetSession() {}
    virtual uint32_t PlayStream(std::vector<uint8_t>, const glm::vec3*) { return 0; }
    // an event whose container plays these media in order, once (Game+'s chosen takes)
    virtual uint32_t PostEventMedia(std::string_view, std::vector<uint32_t>, const glm::vec3*) { return 0; }
    virtual void SeekPlayingId(uint32_t, float) {}
    virtual void StopPlayingId(uint32_t, float) {}
    virtual void SetState(std::string_view, std::string_view) {}
    virtual void SetRtpc(std::string_view, float) {}
    // A ShGimmick record's own sound (CallSound, PostSoundEvent): posted on that record's object, which MoveRecordSound
    // places while it plays (0x954270 moves it to the record's connect point every update)
    virtual uint32_t PostRecordEvent(int, std::string_view name, const glm::vec3& position) { return PostEvent(name, &position); }
    virtual uint32_t PostRecordEventId(int, uint32_t id, const glm::vec3& position) { return PostEventId(id, &position); }
    virtual void MoveRecordSound(int, const glm::vec3&) {}
};

// The handy light's reflection module (0x9359D0, created by 0x933660 with 0x933C20; rendering.md 4.7): where the beam meets the
// surfaces it keeps two pairs of lights, a point and a spot each, the first at the beam's hit and the second at the hit it fades
// out from after a jump in depth. Game::UpdateHandyReflection, RenderSceneBuilder::AddHandyReflection
struct HandyReflection {
    bool active = false;
    bool primed = false;
    bool fading = false;               // +0x42D8
    glm::vec3 position{0.0f};          // reflectLightPoint1 and reflectLight0 (+0x70, the hit of the frame)
    glm::vec3 second{0.0f};            // reflectLightPoint2 and reflectLight02 (+0x50)
    glm::vec3 previous{0.0f};          // the hit of the previous frame (+0x70 before the update)
    glm::vec3 camera{0.0f};            // the view of the previous tick, to start over after a cut
    glm::vec3 forward{0.0f, 0.0f, 1.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};  // the spots' axis (+0x90), eased toward the rays' mean normal
    float first_weight = 1.0f;         // +0x42B8
    float second_weight = 0.0f;        // +0x42BC
    // +0x42C0, which rises by 0.05 a frame while the hit is in a trapKind 0 ShSpotLightReflectionTrap box. The boxes' extents
    // (slot +0x80 of the table at 0x1C91900) are zeroed by 0x932CA0 and written by no function found (0x938BD0 and 0x939250
    // copy the entity's matrix, 0x933110 its inverse), so the test never passes and the share stays 0: the spots stay off
    float spot_share = 0.0f;
    float first_attenuation = 0.0f;    // 1/w^2 - w^2/R^4 of each light's depth w in the view, R the handy light's outer range
    float second_attenuation = 0.0f;
    // The four rays around the beam (0x13CE7E4: 315, 45, 225 and 135 degrees about the axis): where each meets the surfaces,
    // or its end, unmoved by the 2 cm offset. Their screen positions are the corners of the quad Draw2D_ShSpotLightReflection
    // draws into the module's 64x64 view (+0x2A0), whose read back pixel colours the lights
    glm::vec3 samples[4]{};
    // The colour read back from that view (+0x22B8, the word at (0, 32) of the 64x64 RGBA8 copy at +0x2B8), from the frame
    // the renderer finished kFramesInFlight frames before (Game::SetHandyReflectionReadback). readback_age counts the active
    // updates since a new one arrived; a headless run that renders only its shots never brings one in time, and the lights
    // keep the white of the f050a and lisa_balcony captures (RenderSceneBuilder::AddHandyReflection)
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
    // 1..100 takes the place of the f160 handy light's roll (FloorEnvironment; 0: the roll stands), so a comparison can give the
    // port the colour a capture rolled
    int handy_light_roll = 0;
    // 0..6 takes the place of the bug screen pick of gc_p02_080 (DemoUi::Text; 6 is the port's seventh page; -1: the pick stands)
    int bug_screen = -1;
    // the question at the end of the credits (--street-offer): 0 asks, 1 walks the street, 2 restarts, -1 never asks
    int street_offer = 0;
    // the loop browser's release locks (PT_RELEASE_LOCKS, --release-locks; Game::BrowseUnlocked)
    bool release_locks = false;
    // a session of the Archive's theater (archive_theater.h): a second game that plays one demo or shows one model while the
    // player's game stays paused. It shares the loaded data of the first (the player's animation set is not loaded again), skips
    // the opening unless asked to (Game::TheaterControl), and never closes the menu that opened it
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
    // `follows_view`: the camera is where the player's own view already is (compare_ref --replay's shots), so the handy light
    // keeps its eased aim and spots; any other override is a pose the view did not follow and they start there at once
    // the free camera and photo mode (Extras) show the player's body in the camera view, as the mirror shows it
    void SetShowBody(bool on) { show_body_ = on; }
    bool ShowBody() const { return show_body_; }
    // the body is drawn in the camera view: the free camera and the photo mode (ShowBody) or the third person view
    bool BodyShown() const { return show_body_ || third_person_shown_; }
    // the free camera (Extras) and the photo mode: the rendered view is not the player's, whose body and handy light stay where
    // the player's own view puts them, with the body shown or not; the third person view while it shows the body, too
    void SetDetachedView(bool on) { detached_view_ = on; }
    bool DetachedView() const { return detached_view_ || third_person_shown_; }
    // the free camera and the photo mode alone: their camera goes where the player's view does not follow, so the flashlight
    // reflection keeps the colour read from the player's last view (the third person camera follows the view and reads it)
    bool FreeView() const { return detached_view_; }
    // Third person (Extras, pt.ini [camera] third_person; not in the original, gameplay.md 10.6): the frame is drawn from behind
    // the player's right shoulder while the logic keeps the player's own first person view (GetCamera, MakeCamera, InViewNdc),
    // so every trap, in-view test, trigger and demo runs as in first person. Off by default; off, ViewCamera is GetCamera
    void SetThirdPerson(bool on);
    bool ThirdPerson() const { return third_person_; }
    bool ThirdPersonCameraActive() const { return third_person_ && third_person_primed_ && !detached_view_; }
    // the camera the frame is drawn with: GetCamera, or while the third person view is on, the camera behind the shoulder
    // (blended with GetCamera while it leaves or returns for the zoom, the peephole, a demo or a tight spot)
    Camera ViewCamera() const;
    // the share of the third person view in ViewCamera, 0 to 1 (eased; 0 is the first person view)
    float ThirdPersonWeight() const { return third_person_weight_; }
    // a drawn camera turned between ticks by the look that reaches the render early (RenderMouseLook): the third person camera
    // orbits the head by that turn instead of turning in place
    Camera ThirdPersonTurn(const Camera& before, const Camera& after) const;
    // a drawn camera turned between ticks (RenderMouseLook) while the peephole theater plays: the turn kept inside the look
    // aperture the next tick will clamp it to, and the camera 1 m past the hole along the turned view (GetCamera), so the
    // drawn frames neither overshoot the aperture and snap back nor turn without moving the eye
    Camera PeepholeTurn(const Camera& turned) const;
    // the flashlight's lens in the free camera and the photo mode, where the hand holds it (AddMirrorBody); false elsewhere
    bool HandyLens(glm::vec3& out) const {
        out = handy_lens_;
        return handy_lens_valid_;
    }
    // input script `sphoto 1|2|0`: start the photo mode with its panel (1) or without (2), or end it (0; main.cpp), for
    // captures without a keyboard
    void RequestPhotoMode(int mode) { photo_request_ = mode == 0 ? 3 : mode; }
    int TakePhotoModeRequest() { const int r = photo_request_; photo_request_ = 0; return r; }
    // input script `sfreecam 1|0`: the free camera on or off (main.cpp), as F6 or the Extras row
    void RequestFreeCamera(bool on) { freecam_request_ = on ? 1 : 2; }
    int TakeFreeCameraRequest() { return std::exchange(freecam_request_, 0); }
    // input script `sphotocam right up forward look_dy`: the photo camera at an offset in the player's frame (x right, y up from
    // the eye, z along the facing), looking at the eye raised by look_dy
    void RequestPhotoCamera(const glm::vec4& offset_look) { photo_camera_request_ = offset_look; }
    std::optional<glm::vec4> TakePhotoCameraRequest() { auto r = photo_camera_request_; photo_camera_request_.reset(); return r; }
    void RequestPhotoTarget(const glm::vec2& right_forward) { photo_target_ = right_forward; }
    glm::vec2 PhotoTarget() const { return photo_target_; }
    // input script `sphotoview` / `sphotofile`: the photo camera at a world position, looking at a world point (the loop browser's
    // previews frame a subject that is not where the player looks)
    void RequestPhotoView(const glm::vec3& position, const glm::vec3& target) { photo_view_request_ = std::pair(position, target); }
    std::optional<std::pair<glm::vec3, glm::vec3>> TakePhotoViewRequest() { return std::exchange(photo_view_request_, std::nullopt); }
    // input script `sphotoset fov roll focus aperture letterbox`: the photo panel's values (focus and aperture as panel steps)
    void RequestPhotoSettings(const std::vector<int>& values) { photo_settings_request_ = values; }
    std::vector<int> TakePhotoSettingsRequest() { auto r = std::move(photo_settings_request_); photo_settings_request_.clear(); return r; }
    void SetCameraOverride(const std::optional<Camera>& camera, bool follows_view = false) {
        camera_override_ = camera;
        camera_override_follows_view_ = camera.has_value() && follows_view;
    }
    // the handy light's look target, eased (0x1282C50; UpdateHandyTarget)
    glm::vec3 HandyAim() const { return light_aim_; }
    // VR with the flashlight on a tracked controller (docs/vr.md): the light's position and direction in the world in place of
    // the hand's pose about the camera; the game's light model (colour, cone, mask, shadow, reflections) is unchanged
    void SetHandyPoseOverride(const std::optional<std::pair<glm::vec3, glm::vec3>>& pose) { handy_pose_override_ = pose; }
    const std::optional<std::pair<glm::vec3, glm::vec3>>& HandyPoseOverride() const { return handy_pose_override_; }
    // the hand's pose after the flashlight pickup: 1 from the pickup until the player walks, then blending to 0, the pose of
    // play (UpdateHandyTarget)
    float HandyPickupHold() const { return handy_hold_; }
    // the light's pose under a demo camera (0.165 m left, 0.13 m down, 0.185 m ahead of it): 1 while a demo camera is on
    // screen, blending to 0 after it (UpdateHandyTarget)
    float HandyDemoPose() const { return handy_demo_; }
    const HandyReflection& HandyReflectionState() const { return handy_reflection_; }
    // the colour the renderer read back for the reflection lights (RenderStats::reflection_readback)
    void SetHandyReflectionReadback(const glm::vec3& colour) {
        handy_reflection_.readback = colour;
        handy_reflection_.has_readback = true;
        handy_reflection_.readback_age = 0;
    }

    Vfs& GetVfs() { return vfs_; }
    ModelCache& Models() { return models_; }
    StageManager& Stages() { return stages_; }
    const CollisionWorld& Collision() const { return collision_; }
    // the detailed surfaces the camera's line checks hit (mask 0x700), StageManager::LineChecks
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
    // the installed mods' init.lua scripts (docs/modding.md); nullptr without any, so the event sites cost one check
    ModLua* ModScripts() { return mod_scripts_.get(); }
    // runs every enabled mod's init.lua once, after the game's own scripts
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
    // true on the tick that starts a 1/30 s game frame (the grain clock's boundary): the systems the eboot runs once per frame
    // (Lisa's update, the nazo overlay removal, the traps while a demo holds the player) run on it
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
        // the sound whose marker started it (0: none); the subtitle ends when that sound stops
        uint32_t sound = 0;
    };
    void QueueSubtitle(std::string_view subtitle_id, float offset_seconds = 0.0f, uint32_t sound = 0) {
        subtitle_requests_.push_back({std::string(subtitle_id), offset_seconds, sound});
    }
    std::vector<SubtitleRequest> TakeSubtitleRequests() { return std::exchange(subtitle_requests_, {}); }
    // a loop transition drops the previous loop's speech: the next GameUi::Update consumes this and clears its player
    void ClearPendingSubtitles();
    bool TakeSubtitleClear() { return std::exchange(clear_subtitles_pending_, false); }
    void SetSubtitleVisible(std::string_view message_id, bool visible);
    void SetFloorLighting(int row);
    int FloorLightingRow() const { return lighting_row_; }
    void ApplyStageTransform();
    float PlayerHeightAboveStageRoot() const;

    void ResetSaveBlocks();
    void RequestSave();
    // the pause menu's close (gameplay.md 3.3): options only, the progress as last saved or loaded
    void RequestOptionsSave();
    bool BrowseLoop(int index);
    bool LoopReloadPending() const { return loop_reload_pending_; }
    bool ApplyBrowseReload();
    void OnBrowseFloorEntered();
    void OnBasementExit();
    // the loop browser's run joins normal progression (a checkpoint the original saves at): saves are written again
    void EndBrowseSuppression(std::string_view why);
    bool BrowseSaveSuppressed() const { return browse_save_suppressed_; }
    // a browsed loop after the opening room is waiting for its hallway: the reset skips the stand-up demo and leaves through the door.
    // Not during the pick's fade out: the session the browser was used in still runs step 15 then, and BrowseEnterHallway would
    // light the handy light and place the door there, for the reset to throw away
    bool BrowseEntryPending() const { return browse_waiting_entry_ && browse_loop_ > 0 && !loop_reload_pending_; }
    // run every frame of step 15 while BrowseEntryPending(): puts the player in the start room's door once OnPreGame's messages are read
    void BrowseEnterHallway();
    // the last loop browser pick has reached its loop: its floor was entered through the start room door (a loop after the opening
    // room), or the reset that starts the opening room or the ending began (the preview capture waits on it, then on the step)
    bool BrowseArrived() const { return browse_arrived_; }
    // a played game reaches this floor holding the flashlight picked up on f060, before maze A turns it off (BrowseEnterHallway)
    bool BrowseLightPickedUp(std::string_view floor) const;
    // a save was requested less than `seconds` of game time ago, so its write may still hold the demo stream reads (demo.md Playback)
    bool SaveInFlight(double seconds) const { return time_ - last_save_request_ < seconds; }
    bool ResetProgress();
    // step 20 of a loop browser pick or a progress reset, the stages unloaded: what the session kept starts over as at boot
    void ResetSessionState();
    // the screen effects as the boot's OnInit (step 0) leaves them (gr_init's LUT among them), which ResetSessionState restores
    void CaptureBootEffects() { boot_effects_ = effects_; }
    // input script `sstate save|compare`: the session state a new session must rebuild as a fresh boot does, one item per line
    std::string DescribeSessionState() const;
    // the ending street walk (gameplay.md, street walk): the player walks the ending stage after the credits
    bool StartStreetWalk();
    void UpdateStreetWalk(float dt);
    bool StreetWalkActive() const { return street_walk_; }
    // the loop browser's street entry: the ending stage loads and the walk starts in place of the ending demo (controller step 27)
    bool StreetPending() const { return street_pending_; }
    // the end of the credits (FinishEndingRestartGame from step 28): true when the street walk is offered (step 32 waits for the
    // answer); the menu asks (main.cpp, PcSettings street page)
    bool OfferStreetWalk();
    bool StreetOfferPending() const { return street_offer_ == StreetOffer::Asking; }
    void AnswerStreetOffer(bool walk);
    // step 32: the answer, once the menu that asked has closed
    void UpdateStreetOffer();
    // the port's own credits page (gameplay.md, port credits) between the end of the ending's credits and the street question or
    // the restart (controller step 33): true from UpdatePortCredits once it has run out or was skipped (confirm or back)
    void StartPortCredits();
    bool UpdatePortCredits(float dt);
    void SkipPortCredits();
    bool PortCreditsActive() const { return port_credits_time_ >= 0.0f; }
    float PortCreditsTime() const { return port_credits_time_; }
    bool TakeMenuCloseRequest() { return std::exchange(menu_close_request_, false); }
    // leave the street: the ending's own restart (steps 29 and 30)
    void LeaveStreetWalk();
    // the walk's state goes (a reset, a loop browser pick, the restart)
    void EndStreetWalk();
    // loop browser unlocks (release builds, main.cpp): the entries reached in normal play and whether the game was finished once,
    // kept by main in pt.ini (not in the save, so Reset Progress keeps them); the generation changes with every new record
    void SetBrowseUnlocks(uint32_t reached, bool finished) { browse_reached_ = reached; game_finished_ = finished; }
    uint32_t BrowseReached() const { return browse_reached_; }
    // an entry can be picked: always in the developer build; in the release build once the game was finished and the
    // entry's loop was reached in play (docs/gameplay.md, loop browser)
    bool BrowseUnlocked(int index) const {
        return !config_.release_locks || (game_finished_ && index >= 0 && index < 32 && (browse_reached_ & (1u << index)) != 0);
    }
    bool BrowseLocksOn() const { return config_.release_locks; }
    // the Archive (archive.h): what play reached, by the entries' unlock keys, kept in pt.ini [progress] archive by main as the
    // loop browser's record is (not in the save, so Reset Progress keeps it). An entry can be opened always in the developer
    // build, and in the release build once its key was reached ("finished": once the game was finished)
    void NoteArchive(std::string_view key);
    void SetArchiveSeen(std::set<std::string> keys) { archive_seen_ = std::move(keys); }
    const std::set<std::string>& ArchiveSeen() const { return archive_seen_; }
    bool ArchiveUnlocked(const ArchiveEntry& entry) const;
    uint32_t ArchiveGeneration() const { return archive_generation_; }
    // input script `sarchive <id>`: open an Archive entry as its menu row does (main.cpp takes the request)
    void RequestArchive(std::string_view id) { archive_request_ = std::string(id); }
    std::string TakeArchiveRequest() { return std::exchange(archive_request_, std::string()); }
    bool ArchiveRequestPending() const { return !archive_request_.empty(); }
    // main.cpp: the Archive's theater runs (input script `sarchived` waits for its end)
    void SetArchiveTheaterActive(bool on) { archive_theater_active_ = on; }
    bool ArchiveTheaterActive() const { return archive_theater_active_; }
    // main.cpp: the theater has its picture on screen (input script `sarchiveshown` waits for it)
    void SetArchiveTheaterShowing(bool on) { archive_theater_showing_ = on; }
    bool ArchiveTheaterShowing() const { return archive_theater_showing_; }
    // the theater's switches over its own session (GameConfig::theater): its traps and Lisa's logic stop once the demo's place is
    // set, and the opening plays only for the entries that show it
    struct TheaterControl {
        bool traps = true;
        bool ocho = true;
        bool opening = false;
    };
    TheaterControl& Theater() { return theater_; }
    // the theater's session (GameConfig::theater) enters loop browser entry `index` as a pick does (ApplyBrowseReload): the start
    // room on the loop's previous floor with the puzzles before it solved, left through its door into the loop. Called once the
    // boot's save step has run (it sets the floor and the puzzles the pick needs over what that step left)
    void PrepareTheaterLoop(int index);
    bool GameFinished() const { return game_finished_; }
    uint32_t BrowseUnlockGeneration() const { return browse_unlock_generation_; }
    void NoteBrowseReached(int index);
    void NoteGameFinished();
    // NextFloor entered `floor` on pass `pass` (FloorLevel::NextFloor): a loop reached in normal play unlocks its entry
    void OnFloorReached(std::string_view floor, int pass);
    // the finished game's marker (the save's port tag at 0x70, save_data.cpp): Game+ is on from the first finish
    void EnableGamePlus();
    bool GamePlus() const { return game_plus_; }
    // finished games (the save's port count): 1 shows the bathtub Lisa, 2 and more also Lisa's hsh0 arm
    uint32_t Finishes() const { return finishes_; }
    // the Game+ tier whose content shows (tier N unlocks at the Nth finish and stays): 0 without a finish, and always 0 in a build
    // without PT_GAMEPLUS; PT_GAMEPLUS_TIER=<n> forces a tier for tests
    int GamePlusTier() const;
    // the tier each Game+ item unlocks at (easy to move)
    static constexpr int kTierStreetAmbience = 3;
    static constexpr int kTierMazeMusic = 3;
    static constexpr int kTierBathroomMusic = 3;
    static constexpr int kTierRadioLines = 3;
    static constexpr int kTierHeartbeat = 3;
    static constexpr int kTierLaughs = 3;
    static constexpr int kTierDemos = 4;
    // a trap started a demo at this origin (TrapSystem::ExecDemo): Game+ tier 4 starts its unused sibling with it
    void OnTrapDemo(std::string_view demo_id, const glm::mat4& origin, bool loop);
    // the bathtub Lisa is drawn in an active stage (input script `sexpect gameplus`)
    bool GamePlusTubShown() const;
    // input script `smodel` (previews of unused content, not game behaviour): a model drawn at a world transform, a skinned one in
    // its bind pose; `smodel off` clears them
    void AddMockupDraw(const GpuMesh* mesh, const glm::mat4& world) { mockup_draws_.push_back({mesh, world}); }
    void ClearMockupDraws() { mockup_draws_.clear(); }
    bool ProgressResetPending() const { return progress_reset_pending_; }
    // the speedrun timer (PC extra, Extras > Speedrun timer; speedrun.h, docs/gameplay.md speedrun mode)
    SpeedrunTimer& Speedrun() { return speedrun_; }
    const SpeedrunTimer& Speedrun() const { return speedrun_; }
    // step 14 (the player gets control): a run starts when none is going
    void SpeedrunStartGame();
    // NextFloor leaves `floor` on pass `pass`: a split
    void SpeedrunFloorLeft(std::string_view floor, int pass);
    // step 21 (GotoEnding): the run ends
    void SpeedrunEnding();
    // the end of the credits asks on the speedrun's results page in place of the street question (main.cpp, PcSettings)
    bool SpeedrunResultsPending() const {
        return StreetOfferPending() && speedrun_.Enabled() && speedrun_.GetState() == SpeedrunTimer::State::Finished;
    }
    // the results page's "Return to menu": the ending's restart, with the first boot's option screen before the preface
    void AnswerSpeedrunMenu();
    bool TakeOptionsAfterRestart() { return std::exchange(options_after_restart_, false); }
    bool SavesEnabled() const { return save_store_.Enabled(); }
    void RequestLoad();
    // the save request's dialogs (SaveRequest_Update 0x9476D0, states 0xD to 0x14): the boot load waits while one is open (step 1)
    bool SaveBusy() const { return save_dialog_.has_value(); }
    // The start's loading screen waits on this before the first floor shows (main.cpp: the enhanced textures being prepared),
    // so their swap does not freeze the game once it is playing.
    void SetBootHold(bool hold) { boot_hold_ = hold; }
    bool BootHold() const { return boot_hold_; }
    struct SaveDialog {
        // SYSTEM.lng key, or a PC text key for the system's own out-of-space dialog
        std::string key;
        enum class Then { Nothing, NewSave, Retry } then = Then::Nothing;
    };
    const std::optional<SaveDialog>& PendingSaveDialog() const { return save_dialog_; }
    void CloseSaveDialog();
    // a PC notice in the same centred dialog (a PC text key), closed by the accept button: settings that need a restart
    void ShowNoticeDialog(std::string key) { save_dialog_ = SaveDialog{std::move(key), SaveDialog::Then::Nothing}; }
    // every save write and load read the save job starts (it posts SaveUiDisp or LoadUiDisp), for the save icon; the kind of the last
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
    // a loop browser pick or a progress reset stops the session it was made in: demos, voice recognition, sounds, speech and
    // pad requests, and asks the controller for a new session at its stage unload
    void TearDownSession();
    void UpdateHandyTarget(float dt);
    void UpdateHandyReflection(float dt);
    int FoxRandom(int max, int min);
    void RunSetupScript(std::string_view archive_path);
    void AddMirrorBody(std::vector<DrawItem>& out);
    void UpdateThirdPerson(float dt);
    // the head point the third person camera orbits (the drawn body's feet raised, without the walk's head bob) and the camera
    // there for a look of `yaw` and `pitch` at the eased distance and shoulder offset
    glm::vec3 ThirdPersonHead() const;
    glm::vec3 ThirdPersonPosition(float yaw, float pitch) const;
    bool show_body_ = false;
    bool detached_view_ = false;
    bool third_person_ = false;
    // the body is drawn and the light leaves the flashlight in the hand (the camera is far enough from the head)
    bool third_person_shown_ = false;
    float third_person_weight_ = 0.0f;
    // the eased distance behind the shoulder point, the shoulder offset and the lift, each kept clear of the line check surfaces
    float third_person_distance_ = 0.0f;
    float third_person_shoulder_ = 0.0f;
    float third_person_lift_ = 0.0f;
    // too tight for a camera behind the head (a corner the head is pressed into): first person until there is room again
    bool third_person_blocked_ = false;
    bool third_person_primed_ = false;
    float third_person_tight_ = 0.0f;
    // how far ahead the player's view meets the line check surfaces (eased), where the camera looks
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
    // the floor and stage load count StageManager::PrefetchMazeFor last looked at
    std::string prefetch_floor_;
    uint32_t prefetch_loads_ = 0;
    CollisionWorld collision_;
    CollisionWorld line_collision_;
    CollisionWorld reflection_collision_;
    std::vector<SurfaceTriangle> surfaces_;
    uint64_t collision_generation_ = 0;
    uint64_t geom_generation_ = 0;
    // the tick a worker build of the collision worlds is taken (StageManager::StartCollisionBuild)
    uint64_t collision_take_frame_ = 0;
    uint64_t draw_generation_ = 0;
    std::vector<DrawItem> draws_;
    std::vector<glm::mat4> player_skin_;
    // the flashlight lens glow: a small emissive disc mesh + material, built once on first use
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
    // the progress of the last save or load, which an options-only save writes back unchanged
    std::optional<SaveProgress> saved_progress_;
    std::optional<SaveDialog> save_dialog_;
    bool boot_hold_ = false;
    // 0x9476D0 +0x28: the out-of-space dialog is shown once per save; a second failure gives sys_save_failed_4
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
    // the fade out before a browsed loop's reset, seconds left
    float browse_fade_wait_ = 0.0f;
    bool browse_save_suppressed_ = false;
    bool browse_waiting_entry_ = false;
    // the browsed loop's player is in the start room door trap; frames step 15 waited for OnPreGame's messages before that
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
    // Game+ tier 3 sounds (Game::UpdateGamePlusSounds): playing ids and their state
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
