#include "game/game.h"
#include "game/outro_input.h"
#include "game/archive.h"
#include "game/loop_browser.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <fstream>
#include <sstream>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "engine/fs/fox_crypt.h"
#include "engine/fs/mods.h"
#include "game/player_animation.h"
#include "game/render_scene.h"
#include "game/ui/port_credits.h"

namespace pt::game {
namespace {

constexpr double kGameFrame = 1.0 / 30.0;
constexpr float kBrowseFadeSeconds = 0.5f;
// the ending demo frame the street walk keeps as scenery: the wide shot of the street after the close-up (5700), before the white fade
constexpr double kStreetSceneryFrame = 5760.0;
// the street walk: where the outro's man walks at that frame, facing down the street, and the walls around the straight street
// (ending file space x and z; the building fronts stand at about x -19 and 12.5, the ground ends at z -14)
const glm::vec3 kStreetSpawn(-1.15f, 0.05f, 27.0f);
constexpr glm::vec2 kStreetMin(-19.5f, -12.5f);
constexpr glm::vec2 kStreetMax(13.5f, 190.0f);
// the street's ambience: the rain the hallway hears outside its windows (sfx_common)
constexpr const char* kStreetAmbience = "Play_sfx_rainfall_01";

// The flashlight model's own bbox is +/-0.0246 m across and -0.1139 to +0.1160 m along its local Z, and that Z is the beam:
// the body is 0.23 m long, its head (the lens end, the wider half) is local +Z, and its mid point sits 0.1159 m behind the
// tip. The beam leaves the flash point - the point HandyLightPose puts where the reference captures put the light and
// Game::UpdateHandyReflection reads its cone back from - and the player's right hand is at that same point, thumb and fingers
// pinched to hold it. Standing the model's mid point on the flash point therefore put its whole body behind the hand, the
// tail 0.34 m from the camera, and left the lens 0.10 m short of the cone the player is meant to be shining. This offset is
// the mid point's place in the beam's own frame (x right of the beam, y up, z along it), measured from the flash point, so
// the head and its lit lens stand just past the flash point on the beam's own axis and the hand wraps the body. It was
// measured by screenshotting candidates and reading the model's own pixels against the hand's (scratch/handy_sweep.py,
// scratch/handy_grip.py, scratch/handy_hand.py); PT_HANDY_MODEL_POS=x,y,z overrides it.
constexpr glm::vec3 kHandyModelOffset{-0.105f, 0.020f, -0.045f};

float NextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state) / 4294967296.0f;
}

}

Game::Game(Vfs& vfs, ModelCache& models)
    : vfs_(vfs), models_(models), stages_(vfs, models), objects_(*this), floor_(*this), nazo_(*this), controller_(*this), messages_(*this),
      traps_(*this), demos_(*this), scripts_(*this) {}

Game::~Game() {
    stages_.on_loaded = nullptr;
    stages_.on_unloading = nullptr;
}

bool Game::Init(const GameConfig& config) {
    config_ = config;
    if (config.use_save && !config.save_path.empty()) {
        save_store_.SetDirectory(config.save_path.parent_path(), "PT_Save_Data");
    }
    traps_.debug_log = config.trap_log;
    objects_.Reset();
    if (!scripts_.Init()) {
        return false;
    }
    stages_.on_loaded = [this](Stage& stage) { OnStageLoaded(stage); };
    stages_.on_unloading = [this](Stage& stage) { OnStageUnloading(stage); };
    if (!stages_.LoadResident("/Assets/sh/level/common/resident.fpk")) {
        LogError("game: resident package missing");
        return false;
    }
    RunSetupScript("as/sh/level_asset/chara/parameter/ShParameterTables.lua");
    player_.params = parameters_.player;
    RunSetupScript("as/sh/level_asset/chara/gimmick/ShGimmickSetUp.lua");
    RunSetupScript("as/sh/sound/scripts/motion/setup.lua");
    objects_.ResolveModels();
    // the theater's session shares the player's animation set the first game loaded (its globals hold what the first game's
    // player plays, which a second load would replace under it)
    if (!config.theater) {
        LoadPlayerAnimation(vfs_);
    }
    if (config.theater) {
        fox_rng_ = ReadTsc() | 1u;
    } else if (config.seed) {
        fox_rng_ = config.seed;
        SetFixedRandomSeed(config.seed);
    } else {
        // the Fox generator is seeded once per boot from the time (0xA5AF60), so every play rolls its own f160 light and blur pulses
        /* Seeded from the clock once per boot like the original (0xA5AF60): the f160 light and blur pulses are meant to differ from play to play. */
        fox_rng_ = ReadTsc() | 1u;
    }
    floor_.SeedRandom(config.seed ? config.seed : ReadTsc());
    controller_.first_boot = true;
    LogInfo("game: initialized");
    LogInfo("game+: {}", PT_GAMEPLUS ? "content in this build" : "not in this build (PT_GAMEPLUS off; finishes are still counted)");
    return true;
}

void Game::LoadModScripts(const mods::ModSet& set) {
    for (const mods::Mod& mod : set.mods) {
        if (!mod.enabled || !mod.has_script) {
            continue;
        }
        auto code = mods::ReadDiskFile(mod.root / "init.lua");
        if (!code) {
            LogWarn("mods: {}: init.lua cannot be read", mod.Name());
            continue;
        }
        if (!mod_scripts_) {
            mod_scripts_ = std::make_unique<ModLua>([](bool error, std::string_view text) {
                if (error) {
                    LogError("mods: {}", text);
                } else {
                    LogInfo("mod: {}", text);
                }
            });
            mod_scripts_->SetQueries({[this] { return floor_.CurrentFloorName(); }, [this] { return floor_.LoopCount(); },
                                      [this] { return controller_.Step(); }});
        }
        if (mod_scripts_->AddMod(mod.Name(), mod.folder + "/init.lua", *code)) {
            LogInfo("mods: {} init.lua ran", mod.Name());
        }
    }
}

void Game::RunSetupScript(std::string_view archive_path) {
    auto code = mods::ReadOverride(archive_path);
    if (!code) {
        code = vfs_.Archive().Read(archive_path);
    }
    if (!code) {
        LogWarn("game: setup script {} missing", archive_path);
        return;
    }
    std::vector<uint8_t> plain = fox::IsWrapped(*code) ? fox::Unwrap(*code) : *code;
    scripts_.Vm().RunChunk(archive_path, plain);
}

void Game::OnStageLoaded(Stage& stage) {
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::time_point from, clock::time_point to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
    const auto started = clock::now();
    scripts_.LoadStageScripts(stage);
    const auto scripts_at = clock::now();
    demos_.IndexStage(stage);
    nazo_.RegisterStage(stage);
    messages_.RegisterStage(stage);
    nazo_.RestoreCompletedPeephole(stage);
    const auto registered_at = clock::now();
    if (audio_) {
        audio_->OnStageLoaded(stage);
    }
    const auto audio_at = clock::now();
    if (stage.label == "current") {
        SpawnPlayer(stage);
    }
    if (traps_.debug_log) {
        traps_.DumpTraps(stage);
    }
    if (const double total = ms(started, clock::now()); total > 5.0) {
        LogInfo("stage: {} registered in {:.1f} ms (scripts {:.1f}, demos and traps {:.1f}, sound {:.1f})", stage.label, total, ms(started, scripts_at),
                ms(scripts_at, registered_at), ms(registered_at, audio_at));
    }
}

void Game::OnStageUnloading(Stage& stage) {
    traps_.ForgetStage(stage);
    demos_.ForgetStage(stage);
    nazo_.ForgetStage(stage);
    messages_.ForgetStage(stage);
    if (audio_) {
        audio_->OnStageUnloading(stage);
    }
    // ShGimmick RemoveLocators (0x1253D50) sets flag 0x40, and the next ShGimmick update runs 0x953FE0; AddLocators sets nothing
    locators_dirty_ = true;
}

void Game::SpawnPlayer(Stage& stage) {
    for (const auto& file : stage.files) {
        for (const LocatorPlacement& locator : file->game_objects) {
            if (locator.class_name == "ShPlayer") {
                player_.Spawn(stage.ToWorld(locator.world));
                return;
            }
        }
    }
}

void Game::Update(float dt, const InputState& pad_input) {
    // a save dialog is a system dialog on the PS4: it takes the pad, the game gets no input while it is open
    static const InputState kNoInput{};
    const InputState& input = save_dialog_ || EndingOutroInputBlocked(controller_.Step()) ? kNoInput : pad_input;
    if (loop_reload_pending_) {
        // the loop browser's pick: the screen fades out first, then the session the browser was used in is stopped
        browse_fade_wait_ -= dt;
        if (browse_fade_wait_ <= 0.0f) {
            loop_reload_pending_ = false;
            browse_fade_wait_ = 0.0f;
            TearDownSession();
            controller_.ChangeGameStep("ResetGame");
        }
    }
    if(progress_reset_pending_) {
        progress_reset_pending_=false;
        LogInfo("save: confirmed reset restarts the current game session");
        // the reset is a new game as a fresh boot without a save starts it: nothing of the session it was made in carries over
        // (its demos, speech, sounds, Lisa's kill and the player's body), as for a loop browser pick
        TearDownSession();
        controller_.ChangeGameStep("FinishEndingRestartGame");
    }
    if (paused_) {
        effects_.Update(dt);
        return;
    }
    speedrun_.Tick(dt, controller_.LoadStep());
    last_input_ = input;
    frame_delta_ = dt;
    ++frame_;
    time_ += dt;
    grain_time_ += dt;
    game_frame_tick_ = grain_time_ + 1e-6 >= kGameFrame;
    if (game_frame_tick_) {
        // 0x9398E0: grain noise offset per game frame
        grain_time_ = std::fmod(grain_time_ + 1e-6, kGameFrame);
        effects_.grain_offset = glm::vec2(NextRandom(grain_seed_), NextRandom(grain_seed_));
    }
    controller_.Update(dt);
    if (mod_scripts_) {
        mod_scripts_->Tick(dt);
    }
    UpdateBlurSway();
    if (floor_.CurrentFloorName() != prefetch_floor_ || stages_.LoadCount() != prefetch_loads_) {
        prefetch_floor_ = floor_.CurrentFloorName();
        prefetch_loads_ = stages_.LoadCount();
        stages_.PrefetchMazeFor(prefetch_floor_);
    }
    stages_.Update();
    UpdateGamePlus();
    UpdateGamePlusSounds();
    UpdateGamePlusDemos();
    messages_.Dispatch();
    demos_.Update(dt);
    messages_.Dispatch();
    // The next hallway copy is loaded when the player passes the clock. Its collision worlds are built on a worker and taken
    // kCollisionBuildTicks ticks (half a second) later, always on that tick, while the new stage is still behind the closed end
    // door: the synchronous rebuild there took 25 ms of the main thread, which with the stage load made the hitch at the clock
    // on every loop
    constexpr uint64_t kCollisionBuildTicks = 30;
    if (stages_.CollisionBuildRunning() && frame_ >= collision_take_frame_) {
        stages_.FinishCollisionBuild(collision_, &surfaces_, line_collision_, reflection_collision_);
        geom_generation_ = stages_.GeomGeneration();
    }
    if (collision_generation_ != stages_.CollisionGeneration()) {
        if (stages_.CollisionDeferrable()) {
            stages_.StartCollisionBuild();
            collision_take_frame_ = frame_ + kCollisionBuildTicks;
        } else {
            stages_.BuildCollision(collision_, &surfaces_, &line_collision_, &reflection_collision_);
            geom_generation_ = stages_.GeomGeneration();
        }
        collision_generation_ = stages_.CollisionGeneration();
    } else if (geom_generation_ != stages_.GeomGeneration()) {
        // a door or the clock's models switching their collision (demo scripts, several times a loop): owner flags, not a
        // rebuild of every loaded stage's triangles (6 to 25 ms each)
        const auto start = std::chrono::steady_clock::now();
        stages_.UpdateCollisionActive(collision_, &surfaces_, &line_collision_, &reflection_collision_);
        geom_generation_ = stages_.GeomGeneration();
        LogDebug("stage: collision flags, {} triangles, {} material surfaces, {} line check triangles ({:.2f} ms)", collision_.ActiveTriangles(),
                 surfaces_.size(), line_collision_.ActiveTriangles(),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    if (player_.spawned) {
        const bool demo_control = demos_.ControlsPlayer();
        for (char set : {'A', 'B', 'C'}) {
            if (demo_control) {
                player_.locks.Add(set, "Demo", 0xFFFFFFFFu);
            } else {
                player_.locks.Remove(set, "Demo");
            }
        }
        PlayerFrameContext context;
        context.nazo_action_armed = nazo_.IsActionArmed();
        context.big_zoom = traps_.big_zoom;
        context.full_screen_blur = effects_.full_screen_blur;
        context.invert_x = options_.invert_x;
        context.invert_y = options_.invert_y;
        context.peephole_theater = nazo_.IsPeepholeTheaterActive();
        if (!demo_control) {
            player_.Update(dt, input, collision_, context);
            // the camera plugin's zoom-out branch (0x984A62) calls AbortPeephole once in every game frame the zoom falls; the
            // port's zoom runs per tick, so a fall in either tick of the frame aborts once, on the next frame tick
            zoom_fell_ = zoom_fell_ || player_.ZoomFalling();
        }
        if (zoom_fell_ && game_frame_tick_) {
            zoom_fell_ = false;
            nazo_.AbortPeephole();
        }
        while (previous_foot_steps_ < player_.foot_steps) {
            ++previous_foot_steps_;
            nazo_.CountFootstep();
        }
        while (previous_sounding_steps_ < player_.sounding_steps) {
            ++previous_sounding_steps_;
            if (audio_) {
                audio_->Footstep(LastFootLeft(next_foot_left_), player_.Feet());
            }
            next_foot_left_ = !next_foot_left_;
        }
        for (uint64_t event : std::exchange(player_.anim_sounds, {})) {
            auto sound = anim_sounds_.find(event);
            if (audio_ && sound != anim_sounds_.end()) {
                audio_->AnimEvent(sound->second, event, player_.Feet());
            }
        }
        trap_dt_ += dt;
        // once per game frame: at the player's frame end, and while a demo holds the player (who is not updated) on the frame tick
        if ((demo_control ? game_frame_tick_ : player_.FrameEnds()) && (!config_.theater || theater_.traps)) {
            traps_.Update(trap_dt_);
            trap_dt_ = 0.0f;
        }
        messages_.Dispatch();
        player_.EndFrame();
    }
    if (locators_dirty_) {
        objects_.HideRecordsWithoutLocator();
        locators_dirty_ = false;
    }
    objects_.Update(dt);
    effects_.Update(dt);
    UpdateHandyTarget(dt);
    UpdateHandyReflection(dt);
    UpdateThirdPerson(dt);
    if (audio_) {
        const Camera camera = GetCamera();
        audio_->SetListener(camera.position, camera.Forward(), camera.Up());
        audio_->SetRtpc("player_direction", std::fmod(glm::degrees(player_.CameraFoxYaw()) + 360.0f, 360.0f));
        audio_->Update(dt);
    }
}

uint32_t Game::SurfaceMaterial(const glm::vec3& feet) const {
    uint32_t material = 0;
    float best = feet.y - 0.6f;
    for (const SurfaceTriangle& t : surfaces_) {
        const glm::vec2 p(feet.x, feet.z);
        const glm::vec2 a(t.a.x, t.a.z);
        const glm::vec2 b(t.b.x, t.b.z);
        const glm::vec2 c(t.c.x, t.c.z);
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-8f) {
            continue;
        }
        const float u = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / area;
        const float v = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / area;
        const float w = 1.0f - u - v;
        if (u < 0.0f || v < 0.0f || w < 0.0f) {
            continue;
        }
        const float y = u * t.a.y + v * t.b.y + w * t.c.y;
        if (y > best && y < feet.y + 0.4f) {
            best = y;
            material = t.material;
        }
    }
    // the street walk: the road and sidewalks carry no footstep material (no movement hull), their steps sound as concrete
    if (material == 0 && street_walk_) {
        material = 0x5F263606;
    }
    return material;
}

void Game::CollectDraws(std::vector<DrawItem>& out) {
    if (draw_generation_ != stages_.Generation()) {
        draws_.clear();
        stages_.CollectDraws(draws_);
        draw_generation_ = stages_.Generation();
    }
    out.insert(out.end(), draws_.begin(), draws_.end());
    objects_.CollectDraws(out);
    demos_.CollectDraws(out);
    AddMirrorBody(out);
    if (!mockup_draws_.empty()) {
        static const std::vector<glm::mat4> kBindPose(256, glm::mat4(1.0f));
        for (const auto& [mesh, world] : mockup_draws_) {
            DrawItem item;
            item.mesh = mesh;
            item.transform = world;
            if (mesh->skinned) item.skin = kBindPose;
            out.push_back(item);
        }
    }
    // Game+ tier 2: Lisa's left side drawn as hsh0's (GamePlusArm); after the mockup draws, so smodel's bind pose Lisa shows it too
    if (GamePlusTier() >= 2 && game_plus_arm_.Prepare(models_)) {
        game_plus_arm_.Apply(out);
    }
}

// The player's own body in the mirror. The MirrorCapture views (view index 1) draw it and the camera view does not: floor_f060
// 3420 to 3500 and the user's PS4 screenshot of 30.09 show the player's hood, jacket, shirt and chin in the mirror, after
// gc_p00_030 had shown MESH_arm and hidden MESH_ROOT (0x945620), so in play the model has its parts' groups again (MESH_arm,
// the first-person arms, hidden). It is posed by the walk and stand clips that place the camera (PlayerBody) and drawn only
// while a capture runs (0x959FD0), no demo holds the player and the peephole theater does not carry the camera away from the
// body (the user: the player is not seen while peeking through the hole). The camera view's shadow maps leave it out
// (hidden_views bit 0); the mirror view's MirrorLight takes it as a caster (RecordShadows).
void Game::AddMirrorBody(std::vector<DrawItem>& out) {
    // PT_MIRROR_BODY=0 drops the player's body alone from the capture, so a shot with and one without show its share
    static const bool body_off = [] {
        const char* off = std::getenv("PT_MIRROR_BODY");
        return off && *off == '0';
    }();
    // the free camera's and the third person view's body (BodyShown) needs no mirror: it draws in the camera view too
    const bool third_person_transition = third_person_ && !FreeView() && third_person_weight_ > 0.0f && player_.handy_light.enable;
    if (body_off || !player_.spawned || (!BodyShown() && !third_person_transition && (!effects_.mirror_capture || mirror_viewport_bits_ == 0 || demos_.ControlsPlayer() ||
        nazo_.IsPeepholeTheaterActive()))) {
        static const bool trace = std::getenv("PT_MIRROR_TRACE") != nullptr;
        if (trace) {
            LogInfo("mirror trace: body skipped spawned {} capture {} bits {:#x} demo_controls {} peephole {}", player_.spawned,
                    effects_.mirror_capture, mirror_viewport_bits_, demos_.ControlsPlayer(), nazo_.IsPeepholeTheaterActive());
        }
        return;
    }
    const std::string& file = demos_.PlayerModelFile();
    const ModelEntry* entry = file.empty() ? nullptr : models_.Get(file);
    const std::shared_ptr<const anim::HelpBones> help = demos_.PlayerHelpBones();
    const std::shared_ptr<const anim::SimRig> sim = demos_.PlayerSimRig();
    // The free camera and the photo mode show the body from outside, holding the flashlight. The light arm is one pose
    // (0xFC517D57B3A9F1EE: the fist raised to the chin) that does not follow the view's pitch, while the light turns with the view
    // about the wrist (HandyLightPose). There the hand takes the light's frame: its pose against the light at kGripPitch, with
    // the view's pitch and the aim now (two bone IK of the arm, PlayerBody::ReachLeftHand), and the flashlight (0.23 m long, as
    // the player parts' lig0_main0_def) lies in the fist along the beam. The light then leaves its lens, 0.12 m ahead of where
    // the light stands in play (the original's flash point is at the fist's front, which a body that long would push into the
    // face). The mirror keeps the original's arm
    handy_lens_valid_ = false;
    glm::mat4 grip_hand(1.0f);
    glm::mat4 grip_prop(1.0f);
    bool gripped = false;
    const PlayerBody::HandReach reach = [&](const glm::mat4& hand_model, glm::mat4& target_model) {
        constexpr float kGripPitch = -15.0f;
        // the fist's hollow in the hand bone's frame (x along the fingers, y from the finger tips to the back of the hand, z across
        // it): the fingers' first joints stand at x 0.085 to 0.090, y 0.013 to 0.018, the middle ones at x 0.10 to 0.12, y -0.01,
        // the last ones curled back to x 0.11, y -0.035 to -0.041 (PT_GRIP_LOG); the flashlight runs across the hand there. At
        // (0.088, -0.030) it lay on the finger tips with the palm open above it (the user's still of 06.10)
        constexpr glm::vec3 kFist{0.095f, -0.013f, -0.005f};
        constexpr float kHoldAt = 0.06f;
        constexpr float kGripRaise = 25.0f;
        const auto frame = [](const glm::vec3& r, const glm::vec3& u, const glm::vec3& d, const glm::vec3& o) {
            return glm::mat4(glm::vec4(r, 0.0f), glm::vec4(u, 0.0f), glm::vec4(d, 0.0f), glm::vec4(o, 1.0f));
        };
        const glm::mat4 body = player_.PoseTransform();
        const glm::mat4 hand_world = body * hand_model;
        // The hand is posed against the light's frame of the view, but the body (and the arm's shoulder) follows the view's yaw
        // eased (10.3): in a fast turn it lags the view by up to 150 degrees, and a hand put where the view's frame says swung
        // round the head and into it. The frame here keeps within kGripLag of the body's facing; the light still shines along
        // the view (it leaves the lens, so in such a turn the beam leaves the flashlight at an angle for a moment)
        constexpr float kGripLag = 0.4363323f;
        // the drawn eye: the body is drawn half a step behind the logic on a frame's last tick (Player::DrawnOffset); posed against
        // the logic's eye the hand shook by that half step every other tick while walking (the light leaves its lens: the beam
        // shook with it)
        Camera own = player_.MakeCamera();
        own.position += player_.DrawnOffset();
        const float lag = std::remainder(player_.CameraFoxYaw() - player_.BodyFoxYaw(), 6.28318531f);
        own.yaw -= lag - std::clamp(lag, -kGripLag, kGripLag);
        const float yaw_back = own.yaw - player_.MakeCamera().yaw;
        Camera reference = own;
        reference.pitch = glm::radians(kGripPitch);
        reference.roll = 0.0f;
        glm::vec3 ref_flash, ref_dir, ref_right, ref_up;
        HandyLightPose(reference, reference.position + reference.Forward() * 5.0f, 0.0f, 0.0f, ref_flash, ref_dir, &ref_right, &ref_up);
        glm::vec3 flash, dir, right, up;
        // the aim point turned with the frame about the eye
        const glm::vec3 aim = own.position + glm::angleAxis(yaw_back, glm::vec3(0.0f, 1.0f, 0.0f)) * (light_aim_ - own.position);
        HandyLightPose(own, aim, handy_hold_, handy_demo_, flash, dir, &right, &up);
        const glm::mat4 aim_reference = glm::inverse(frame(ref_right, ref_up, ref_dir, ref_flash));
        grip_hand = frame(right, up, dir, flash) * aim_reference * hand_world;
        const glm::vec3 fist(aim_reference * hand_world * glm::vec4(kFist, 1.0f));
        // The fist's hollow runs along the hand bone's -z (the fingers' joints line up across z, PT_GRIP_LOG). Carried by the
        // light's frame the hand kept the light arm's pose, whose hollow stands about 22 degrees off the beam (prop axis
        // (-0.36 -0.14 -0.92) in the hand's frame), so the flashlight, laid along the beam, crossed the fingers diagonally: its
        // head over the knuckles and its tail out past the palm, read as lying on the hand rather than held. The hand turns
        // about the fist's hollow until that hollow runs along the beam; the hollow's point stays, so the flashlight keeps its
        // place and only the hand closes round it
        {
            const glm::vec3 hollow_point(grip_hand * glm::vec4(kFist, 1.0f));
            const glm::vec3 hollow = glm::normalize(glm::vec3(grip_hand * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
            const glm::mat4 turn = glm::translate(glm::mat4(1.0f), hollow_point) * glm::mat4_cast(glm::quat(hollow, glm::normalize(dir))) *
                                   glm::translate(glm::mat4(1.0f), -hollow_point);
            grip_hand = turn * grip_hand;
        }
        // the fist holds the body near its tail (local z -0.06 of -0.114), so the tail clears the face at every pitch
        grip_prop = glm::inverse(grip_hand) * frame(glm::cross(up, dir), up, dir, flash + right * fist.x + up * fist.y + dir * (fist.z + kHoldAt));
        // looking up past kGripRaise the hand would follow the view over the head: it stays where kGripRaise puts it and only
        // turns the light up
        const Camera view = own;
        if (view.pitch > glm::radians(kGripRaise)) {
            Camera raised = view;
            raised.pitch = glm::radians(kGripRaise);
            const glm::quat lower(view.Forward(), raised.Forward());
            grip_hand[3] = glm::vec4(view.position + lower * (glm::vec3(grip_hand[3]) - view.position), 1.0f);
        }
        // The flashlight (0.23 m, its tail 0.114 m behind its middle) must not pass through the head: looking up, and in a turn
        // the body has not caught up with, the light's frame put its tail into the forehead and the hair. The hand moves out of
        // the head's way along the line from the head's middle (SKL_004_HEAD raised by kHeadCentre) to the flashlight's nearest
        // point, until that point is kHeadClear from it
        constexpr float kHeadClear = 0.17f;
        constexpr float kHeadCentre = 0.09f;
        if (glm::mat4 head_model; player_.BoneModel("SKL_004_HEAD", head_model)) {
            const glm::vec3 head = glm::vec3(body * head_model[3]) + glm::vec3(0.0f, kHeadCentre, 0.0f);
            const glm::mat4 prop = grip_hand * grip_prop;
            const glm::vec3 axis = glm::normalize(glm::vec3(prop[2]));
            const glm::vec3 mid(prop[3]);
            const float along = std::clamp(glm::dot(head - mid, axis), -0.114f, 0.116f);
            const glm::vec3 nearest = mid + axis * along;
            const glm::vec3 away = nearest - head;
            const float d = glm::length(away);
            if (d < kHeadClear && d > 1.0e-4f) {
                grip_hand[3] += glm::vec4(away / d * (kHeadClear - d), 0.0f);
            }
        }
        target_model = glm::inverse(body) * grip_hand;
        gripped = true;
        return true;
    };
    const bool grip = (DetachedView() || third_person_transition) && player_.handy_light.enable && glm::length(light_aim_) > 0.0f;
    if (!entry || !entry->mesh || !player_.BodySkin(help.get(), player_skin_, player_.handy_light.enable, sim.get(), grip ? &reach : nullptr)) {
        return;
    }
    if (gripped) {
        // the flashlight follows the hand as it was reached (grip_prop: its place in the hand's frame), so it stays in the fist
        // where the arm falls short
        glm::mat4 hand_model;
        if (player_.BoneModel("SKL_013_LHAND", hand_model)) {
            grip_prop = player_.PoseTransform() * hand_model * grip_prop;
            static const bool grip_log = std::getenv("PT_GRIP_LOG") != nullptr;
            if (grip_log) {
                const glm::mat4 to_hand = glm::inverse(player_.PoseTransform() * hand_model);
                std::string text;
                for (const char* name : {"SKL_101_LF10", "SKL_102_LF11", "SKL_103_LF12", "SKL_104_LF21", "SKL_105_LF22", "SKL_106_LF23", "SKL_107_LF31",
                                         "SKL_108_LF32", "SKL_109_LF33", "SKL_110_LF40", "SKL_111_LF41", "SKL_112_LF42", "SKL_113_LF43", "SKL_114_LF51",
                                         "SKL_115_LF52", "SKL_116_LF53"}) {
                    glm::mat4 bone;
                    if (player_.BoneModel(name, bone)) {
                        const glm::vec3 p(to_hand * player_.PoseTransform() * bone[3]);
                        text += std::format(" {} ({:.3f} {:.3f} {:.3f})", name + 8, p.x, p.y, p.z);
                    }
                }
                const glm::vec3 c(to_hand * grip_prop[3]);
                const glm::vec3 axis(to_hand * glm::vec4(glm::vec3(grip_prop[2]), 0.0f));
                LogInfo("grip: prop mid ({:.3f} {:.3f} {:.3f}) axis ({:.3f} {:.3f} {:.3f}){}", c.x, c.y, c.z, axis.x, axis.y, axis.z, text);
            }
        }
        // the light leaves just past the lens (the model's tip at +0.1159)
        handy_lens_ = glm::vec3(grip_prop * glm::vec4(0.0f, 0.0f, 0.12f, 1.0f));
        handy_lens_valid_ = true;
    }
    static const bool feet_trace = std::getenv("PT_THIRD_TRACE") != nullptr;
    if (feet_trace && third_person_) {
        glm::mat4 left, right;
        if (player_.BoneModel("SKL_032_LFOOT", left) && player_.BoneModel("SKL_042_RFOOT", right)) {
            const glm::vec3 l(player_.PoseTransform() * left[3]);
            const glm::vec3 r(player_.PoseTransform() * right[3]);
            LogInfo("third person feet: frame {} left ({:.4f} {:.4f} {:.4f}) right ({:.4f} {:.4f} {:.4f}) lens ({:.4f} {:.4f} {:.4f}) light ({:.4f} {:.4f} {:.4f})",
                    frame_, l.x, l.y, l.z, r.x, r.y, r.z, handy_lens_.x, handy_lens_.y, handy_lens_.z, handy_origin_.x, handy_origin_.y,
                    handy_origin_.z);
        }
    }
    DrawItem item;
    item.mesh = entry->mesh.get();
    item.transform = player_.PoseTransform();
    item.skin = player_skin_;
    item.hidden_meshes = HiddenMeshes(*item.mesh, demos_.PlayerDefaultHidden());
    item.hidden_views = BodyShown() ? 0u : 1u;
    out.push_back(item);
    // the flashlight in the hand while it is lit: the bathroom's flashlight model (kHandyModel) at the light's origin, turned to
    // its beam (the light comes from the hand, HandyLightPose); PT_HANDY_MODEL_TURN=x,y,z (degrees) turns the model in its frame
    static constexpr const char* kHandyModel = "/Assets/sh/environ/object/shsb/light/shsb_lght005/scenes/shsb_lght005.fmdl";
    // PT_HANDY_MODEL_HIDE=1 drops the prop alone, leaving the light on: the shot with it minus the shot without it is the
    // model's own pixels (scratch/handy_iso.py)
    static const bool handy_model_hidden = std::getenv("PT_HANDY_MODEL_HIDE") != nullptr;
    if (!handy_model_hidden && player_.handy_light.enable && glm::length(handy_direction_) > 0.5f) {
        // The model is in pt14_hallway.fpk. A loop browser pick past f060 starts in the start room with the light picked up,
        // before the hallway's package is mounted, and a failed load stays failed in the model cache (no flashlight in the hand
        // for the whole session): the cache is asked only once the file can be read
        if (!handy_model_readable_ && frame_ >= handy_model_probe_frame_) {
            handy_model_readable_ = vfs_.ReadFile(kHandyModel).has_value();
            handy_model_probe_frame_ = frame_ + 30;
        }
        if (const ModelEntry* light = handy_model_readable_ ? models_.Get(kHandyModel) : nullptr; light && light->mesh) {
            if (std::getenv("PT_HANDY_BONES") != nullptr) {
                LogInfo("handy bones: flash point at world ({:.3f} {:.3f} {:.3f}) beam ({:.3f} {:.3f} {:.3f})", handy_origin_.x, handy_origin_.y, handy_origin_.z, handy_direction_.x, handy_direction_.y, handy_direction_.z);
            }
            static const glm::vec3 turn = [] {
                glm::vec3 t(0.0f);
                if (const char* e = std::getenv("PT_HANDY_MODEL_TURN")) std::sscanf(e, "%f,%f,%f", &t.x, &t.y, &t.z);
                return glm::radians(t);
            }();
            const glm::vec3 z = glm::normalize(handy_direction_);
            glm::vec3 x = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), z);
            x = glm::length(x) > 1.0e-6f ? glm::normalize(x) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 y = glm::cross(z, x);
            // kHandyModelOffset: the model's mid point in the beam's own frame (x right of the beam, y up, z along it), measured
            // from the flash point the cone leaves (HandyLightPose, RenderSceneBuilder::AddHandyLight). The hand holds the
            // flashlight at its body between the thumb and the fingers, so the body spans the flash point: with the offset below
            // the head and its lit lens stand just past the flash point on the beam's own axis and the hand wraps the tail.
            // PT_HANDY_MODEL_POS=x,y,z overrides it (measured with scratch/handy_sweep.py, scratch/handy_grip.py).
            static const glm::vec3 pos = [] {
                glm::vec3 v(kHandyModelOffset);
                if (const char* e = std::getenv("PT_HANDY_MODEL_POS")) std::sscanf(e, "%f,%f,%f", &v.x, &v.y, &v.z);
                return v;
            }();
            const glm::vec3 place = gripped ? glm::vec3(grip_prop[3]) : handy_origin_ + x * pos.x + y * pos.y + z * pos.z;
            glm::mat4 frame = gripped ? grip_prop : glm::mat4(glm::vec4(x, 0.0f), glm::vec4(y, 0.0f), glm::vec4(z, 0.0f), glm::vec4(place, 1.0f));
            frame = frame * glm::rotate(glm::mat4(1.0f), turn.x, glm::vec3(1.0f, 0.0f, 0.0f)) *
                    glm::rotate(glm::mat4(1.0f), turn.y, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::rotate(glm::mat4(1.0f), turn.z, glm::vec3(0.0f, 0.0f, 1.0f));
            DrawItem prop;
            prop.mesh = light->mesh.get();
            prop.transform = frame;
            prop.hidden_views = item.hidden_views;
            out.push_back(prop);
            // the lit lens: a small emissive disc at the head end (model local +Z tip, z = +0.116), facing the
            // beam. The housing is a closed metal body lit from the inside, so it draws near-black; the lens
            // has to be emissive to read as the source of the light (PT_HANDY_BONES measures the lens dark).
            // Unlit + kKindConstant goes through the forward emissive pipeline (scene_frame.cpp RecordForward)
            // with depth testing, so the housing occludes it from behind and it shows from the front.
            // PT_HANDY_LENS_HIDE=1 drops the disc alone.
            static const bool lens_hidden = std::getenv("PT_HANDY_LENS_HIDE") != nullptr;
            if (!lens_hidden) {
                if (!handy_lens_mesh_) {
                    MeshData disc;
                    disc.name = "handy_lens";
                    constexpr int kLensSegments = 16;
                    constexpr float kLensRadius = 0.020f;
                    Vertex centre;
                    centre.position = glm::vec3(0.0f);
                    centre.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                    centre.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                    centre.uv0 = glm::vec2(0.5f);
                    disc.vertices.push_back(centre);
                    for (int i = 0; i <= kLensSegments; ++i) {
                        const float a = 2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(kLensSegments);
                        Vertex v;
                        v.position = glm::vec3(std::cos(a) * kLensRadius, std::sin(a) * kLensRadius, 0.0f);
                        v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                        v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                        v.uv0 = glm::vec2(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a));
                        disc.vertices.push_back(v);
                    }
                    for (int i = 1; i <= kLensSegments; ++i) {
                        disc.indices.push_back(0);
                        disc.indices.push_back(static_cast<uint32_t>(i));
                        disc.indices.push_back(static_cast<uint32_t>(i + 1));
                    }
                    SubMesh sub;
                    sub.first_index = 0;
                    sub.index_count = static_cast<uint32_t>(disc.indices.size());
                    sub.vertex_offset = 0;
                    sub.material = 0;
                    sub.pass = RenderPass::Unlit;
                    sub.kind = gpu::kKindConstant;
                    sub.double_sided = false;
                    disc.submeshes.push_back(sub);
                    disc.bounds_min = glm::vec3(-kLensRadius, -kLensRadius, 0.0f);
                    disc.bounds_max = glm::vec3(kLensRadius, kLensRadius, 0.0f);
                    handy_lens_mesh_ = models_.Scene().Upload(disc);
                    MaterialGpu lens_mat;
                    lens_mat.albedo = TextureManager::kWhite;
                    lens_mat.kind = gpu::kKindConstant;
                    lens_mat.albedo_factor = glm::vec4(1.0f, 0.96f, 0.88f, 1.0f);
                    lens_mat.params = glm::vec4(0.0f, 4.0f, 0.0f, 0.0f);
                    handy_lens_material_ = static_cast<int32_t>(models_.Textures().AddMaterial(lens_mat));
                }
                if (handy_lens_mesh_ && handy_lens_material_ >= 0) {
                    DrawItem lens;
                    lens.mesh = handy_lens_mesh_.get();
                    lens.transform = frame * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0012f, 0.1159f));
                    lens.material_override = handy_lens_material_;
                    lens.hidden_views = item.hidden_views;
                    out.push_back(lens);
                }
            }
            if (std::getenv("PT_HANDY_BONES") != nullptr) {
                glm::mat4 hand_model;
                if (player_.BoneModel("SKL_013_LHAND", hand_model)) {
                    const glm::mat4 hand = player_.PoseTransform() * hand_model;
                    const glm::vec3 hx = glm::normalize(glm::vec3(hand[0])), hy = glm::normalize(glm::vec3(hand[1])), hz = glm::normalize(glm::vec3(hand[2]));
                    LogInfo("handy bones: hand frame at ({:.4f} {:.4f} {:.4f}) x ({:.4f} {:.4f} {:.4f}) y ({:.4f} {:.4f} {:.4f}) z ({:.4f} {:.4f} {:.4f})", hand[3].x, hand[3].y,
                            hand[3].z, hx.x, hx.y, hx.z, hy.x, hy.y, hy.z, hz.x, hz.y, hz.z);
                }
                const glm::vec3 axis = glm::normalize(glm::vec3(frame[2]));
                const glm::vec3 tail = place - axis * 0.114f;
                const glm::vec3 tip = place + axis * 0.116f;
                LogInfo("handy bones: prop mid at world ({:.3f} {:.3f} {:.3f}) tail ({:.3f} {:.3f} {:.3f}) tip ({:.3f} {:.3f} {:.3f})", place.x, place.y, place.z, tail.x, tail.y, tail.z, tip.x, tip.y, tip.z);
            }
        }
    }
}

// 0x1282C50 (the player body's update phase 5, 0x98BC70; phase 6, 0x127F6B0 and 0x12810C0, then sets the light): the handy
// light's look target ray starts at the body's feet + (0, 1.65, 0.15) turned by the body (0x1429DF0) and runs 10 m
// (0x1429A30) along the view (the camera component's pitch and yaw) turned by the light stick s (Player::LightStick) through
// the unnormalized quaternion (0.28125 s.y, -0.375 s.x, 0, 1), half of (-0.5625 s.y, -0.75 s.x) (0x1429A28, 0x1429A2C)
// crossed with +Z; the target is its first hit with mask 0x700 or its end, eased by 1 - 0.9375^(dt * 60) (0x1429A34,
// 0x1429A38) into the body's +0x90 and +0x70, where 0x12810C0 aims the light. The light follows that target through a second
// easing that keeps 0.32 per 30 Hz frame (fitted: mirror_f060 5123-5127 within 0.3 degrees, where the eased target alone
// runs about 0.7 frames ahead; the arm's aim in the skeleton path of 0x12810C0 was not traced). Per game tick, as the
// original's player update, so headless runs keep its easing.
void Game::UpdateHandyTarget(float dt) {
    constexpr glm::vec3 kRayOrigin{0.0f, 1.65f, 0.15f};
    constexpr float kRayLength = 10.0f;
    constexpr float kTargetKeep = 0.9375f;
    constexpr float kAimKeep = 0.32f;
    // After the flashlight pickup (gc_p00_030) the original's hand keeps another pose: floor_f060 stands 2 cm lower from the
    // hand-back (2740) with the light 0.19 m further back and 6 cm lower than in play at that pitch (3075-3085, -22.17 degrees),
    // through the turns of 3384-3400 and the walk to the sink, and from the look at the sink at 3534-3540 the camera height and
    // the pose are those of play again (3585-3595). tub_f060_light keeps it from the pickup to its end (3552) through looks
    // down to -55 degrees and two short walks, its pose following the pitch (HandyLightPose), so the pitch does not end it (the
    // port ended it on an 8 degree change). It ends with the look at the sink, which starts gc_p01_090 (likely: the demo the
    // look at the baby position starts; what the original switches was not traced), or when the player leaves the floor
    // The light's pose around the pickup itself (pickup_release_f060: the Spot_Mask constants and the camera of every frame from
    // 2639 to 3700, the light's offset from the camera, left, down, forward). While gc_p00_030's camera is on screen the light
    // stands 0.165, 0.131, 0.185 from it every frame (2639 to 2720, the camera rising from 0.83 to 1.62 m): 0x12810C0's branch
    // for the camera object of 0x970DE0 +0xC0, under which 0x1282C50 scales the light stick by 0.2 and does not ease the look
    // target. From the demo's hand-back (2740) the pickup pose comes in over about 20 frames (2760: 0.190, 0.052, 0.148), stays
    // through a yaw turn (3143 to 3172) and goes over about 20 frames when it ends (3234 to 3256). The port had put the light
    // in the play pose during the demo and switched the poses in one frame (a 0.19 to 0.20 m jump)
    constexpr float kPoseBlendSeconds = 20.0f / 30.0f;
    const float pose_step = std::max(dt, 0.0f) / kPoseBlendSeconds;
    const bool demo_camera = player_.handy_light.enable && demos_.HasActiveCamera();
    handy_demo_ = demo_camera ? 1.0f : std::max(0.0f, handy_demo_ - pose_step);
    if (demos_.IsPlaying("gc_p00_030")) {
        handy_hold_pending_ = true;
        handy_hold_release_ = false;
        handy_hold_ = player_.handy_light.enable ? 1.0f : 0.0f;
    } else if (handy_hold_pending_ && !demos_.ControlsPlayer()) {
        handy_hold_pending_ = false;
        handy_hold_release_ = false;
    }
    if (handy_hold_ > 0.0f && !handy_hold_pending_ && !handy_hold_release_ &&
        (demos_.IsPlaying("gc_p01_090") || demos_.ControlsPlayer() || !floor_.IsCurrentFloorName("f060"))) {
        handy_hold_release_ = true;
    }
    if (handy_hold_release_) {
        handy_hold_ = std::max(0.0f, handy_hold_ - pose_step);
        handy_hold_release_ = handy_hold_ > 0.0f;
    }
    // the free camera (Extras) does not move the light: it follows the player's own view
    const Camera camera = detached_view_ && camera_override_ ? player_.MakeCamera() : GetCamera();
    // the view without the camera's roll: the ray takes the camera component's pitch and yaw (+0x1C8, +0x1CC)
    const glm::vec3 forward = camera.Forward();
    glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
    right = glm::length(right) > 1e-4f ? glm::normalize(right) * (glm::dot(right, camera.Right()) < 0.0f ? -1.0f : 1.0f) : camera.Right();
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));
    // demos that drive the player hold every lock, and the ending outro holds the player's light-stick offset at zero
    const glm::vec2 s = demos_.ControlsPlayer() || EndingOutroInputBlocked(controller_.Step())
                            ? glm::vec2(0.0f)
                            : player_.LightStick() * (demo_camera ? 0.2f : 1.0f);
    const float a = 0.28125f * s.y;
    const float b = 0.375f * s.x;
    const float n2 = 1.0f + a * a + b * b;
    // the quaternion turns +Z and scales it by its squared length: the direction (0.75 s.x, -0.5625 s.y, 1 - a^2 - b^2) / n2
    // (x right, y up) and a ray of 10 n2 m
    const glm::vec3 direction = (right * (0.75f * s.x) - up * (0.5625f * s.y) + forward * (1.0f - a * a - b * b)) / n2;
    const float length = kRayLength * n2;
    // The body point sits a few cm from the player's camera (about 5 cm behind it at the mirror, 9 cm ahead of it at the
    // sink). The ray keeps that offset from the shown camera, turned by its yaw, so that it starts at the body point in play
    // and where the body would stand behind a scripted, demo or peephole camera
    const glm::vec3 body_point(player_.BodyTransform() * glm::vec4(kRayOrigin, 1.0f));
    const glm::vec3 own_forward = player_.CameraForward();
    const glm::vec3 own_right(-own_forward.z, 0.0f, own_forward.x);
    const glm::vec3 offset = body_point - player_.Eye();
    glm::vec3 view_forward(forward.x, 0.0f, forward.z);
    view_forward = glm::length(view_forward) > 1e-4f ? glm::normalize(view_forward) : own_forward;
    const glm::vec3 view_right(-view_forward.z, 0.0f, view_forward.x);
    const glm::vec3 origin = camera.position + view_forward * glm::dot(offset, own_forward) + view_right * glm::dot(offset, own_right) +
                             glm::vec3(0.0f, offset.y, 0.0f);
    glm::vec3 raw = origin + direction * length;
    RayHit hit;
    if (line_collision_.Raycast(origin, direction, length, hit)) {
        raw = hit.point;
    }
    // after a teleport or a camera cut the target starts at the new one (the original eases it from wherever it was): a
    // camera that moves more than 0.25 m or turns more than 15 degrees in a tick (the look turns at most 176 degrees a
    // second), and every tick of an input script camera (scamera: a reference pose that the port's own view did not follow,
    // so the target is taken there as the focus is); compare_ref --replay's shot camera is where the view already is (its look
    // follows the capture's camera every tick) and keeps the eased target, the lag the captures show in fast turns
    if (!light_target_valid_ || demo_camera || (camera_override_ && !camera_override_follows_view_) || glm::distance(origin, light_origin_) > 3.0f ||
        glm::distance(camera.position, light_camera_) > 0.25f || glm::dot(forward, light_forward_) < 0.9659f) {
        light_target_ = light_aim_ = raw;
    }
    light_target_valid_ = true;
    light_origin_ = origin;
    light_camera_ = camera.position;
    light_forward_ = forward;
    light_target_ += (raw - light_target_) * (1.0f - std::pow(kTargetKeep, std::max(dt, 0.0f) * 60.0f));
    light_aim_ += (light_target_ - light_aim_) * (1.0f - std::pow(kAimKeep, std::max(dt, 0.0f) * 30.0f));
}

// 0x9359D0, from the handy light's frame update 0x124F980 while the light is on (0x124FBF0 sets the module's switch +0x42DC with
// the light). The beam runs from the light along its axis for its outer range (+0x42CC, written by 0x124FE00) and takes its first
// hit with mask 0x80700 and query tag inclusions 0x80 and 0x10, 2 cm into the surface along the hit normal, or 2 cm past its
// end (the end minus 0.02 times the normalized end-to-start vector). When that point moves less than 1.01 times the range but
// more than 1 m in depth (0.3 m inside a trapKind 1 box, which no test reaches, see HandyReflection::spot_share), the second
// pair takes the previous point at full weight and the first starts at 0, and the weights cross at 3 per second; a longer
// move puts both there at once. Before the axis the module casts four rays at half the penumbra around it (315, 45, 225 and
// 135 degrees, 0x13CE7E4) with the same filter: their hits (or ends) are the screen corners of the Draw2D_ShSpotLightReflection
// quad whose read back colour tints the lights (HandyReflection::samples, readback), and their normals (the ray reversed
// when it hits nothing) with the axis's, averaged, ease the spots' axis +0x90 by a third a frame.
void Game::UpdateHandyReflection(float dt) {
    HandyReflection& r = handy_reflection_;
    if (!player_.spawned || !player_.handy_light.enable) {
        r.active = false;
        r.primed = false;
        return;
    }
    const HandyLightParameters& p = parameters_.handy_light;
    const Camera camera = detached_view_ && camera_override_ ? player_.MakeCamera() : GetCamera();
    glm::vec3 origin(0.0f);
    glm::vec3 direction(0.0f, 0.0f, 1.0f);
    HandyLightPose(camera, light_aim_, handy_hold_, handy_demo_, origin, direction);
    if (handy_pose_override_) {
        origin = handy_pose_override_->first;
        direction = handy_pose_override_->second;
    }
    handy_origin_ = origin;
    handy_direction_ = direction;
    const float outer = std::max(p.outer_range, 0.0001f);
    // the light's frame (0x937C00 and 0x938000 with up (0, 1, 0)): +Z the axis; the sign of X is a guess
    const glm::vec3 axis_z = direction;
    glm::vec3 axis_x = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), axis_z);
    axis_x = glm::length(axis_x) > 1.0e-6f ? glm::normalize(axis_x) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 axis_y = glm::cross(axis_z, axis_x);
    // ray i: cos(t) Z + sin(t) (-sin(a) X + cos(a) Y), t the half penumbra (+0x42D0), a from 0x13CE7E4
    static constexpr float kSampleAngles[4] = {315.0f, 45.0f, 225.0f, 135.0f};
    const float tilt = glm::radians(p.penumbra_angle * 0.5f);
    glm::vec3 normal_sum(0.0f);
    for (int i = 0; i < 4; ++i) {
        const float a = glm::radians(kSampleAngles[i]);
        const glm::vec3 d = glm::normalize(std::cos(tilt) * axis_z + std::sin(tilt) * (-std::sin(a) * axis_x + std::cos(a) * axis_y));
        RayHit h;
        if (reflection_collision_.Raycast(origin, d, outer, h, true)) {
            r.samples[i] = h.point;
            normal_sum += h.normal;
        } else {
            r.samples[i] = origin + d * outer;
            normal_sum -= d;
        }
    }
    glm::vec3 position = origin + direction * (outer + 0.02f);
    RayHit hit;
    if (reflection_collision_.Raycast(origin, direction, outer, hit, true)) {
        position = hit.point - hit.normal * 0.02f;
        normal_sum += hit.normal;
    } else {
        normal_sum -= direction;
    }
    if (std::getenv("PT_REFLECT_LOG")) {
        // Accepted surfaces along the reflection beam, retaining original winding and double-sided shape flags.
        glm::vec3 from = origin;
        float left = outer;
        for (int n = 0; n < 8 && left > 0.0f; ++n) {
            RayHit h;
            if (!reflection_collision_.Raycast(from, direction, left, h, true)) {
                break;
            }
            const CollisionTriangle& t = reflection_collision_.Triangles()[h.triangle];
            LogInfo("reflect ray: {:.3f} m at ({:.3f} {:.3f} {:.3f}) normal ({:.2f} {:.2f} {:.2f}) tags {:#018x} shape {:#x} prim {:#06x} {}", outer - left + h.distance,
                    h.point.x, h.point.y, h.point.z, h.normal.x, h.normal.y, h.normal.z, t.tags, t.shape_flags, t.prim_info, reflection_collision_.OwnerName(t.owner));
            from = h.point + direction * 0.001f;
            left -= h.distance + 0.001f;
        }
    }
    const glm::vec3 forward = camera.Forward();
    const auto depth = [&](const glm::vec3& point) { return glm::dot(point - camera.position, forward); };
    // a camera cut, a teleport or an input script camera starts the lights at the new hit, as UpdateHandyTarget starts the
    // look target there: a reference pose the port's own view did not follow would fade from wherever the beam was before
    if ((camera_override_ && !camera_override_follows_view_) || glm::distance(camera.position, r.camera) > 0.25f ||
        glm::dot(forward, r.forward) < 0.9659f) {
        r.primed = false;
    }
    r.camera = camera.position;
    r.forward = forward;
    // the average of the five normals (0.2 times their sum, normalized)
    const glm::vec3 average = glm::length(normal_sum) > 1.0e-6f ? glm::normalize(normal_sum) : -direction;
    if (!r.primed) {
        r.previous = r.second = position;
        r.first_weight = 1.0f;
        r.second_weight = 0.0f;
        r.fading = false;
        r.normal = average;
        r.primed = true;
    } else {
        // +0x90 = normalize(0.67 +0x90 + 0.33 average), once an original frame
        const float keep = std::pow(0.67f, std::max(dt, 0.0f) * kOriginalFrameRate);
        const glm::vec3 eased = r.normal * keep + average * (1.0f - keep);
        r.normal = glm::length(eased) > 1.0e-6f ? glm::normalize(eased) : average;
    }
    if (glm::distance(position, r.previous) <= outer * 1.01f) {
        if (std::abs(depth(position) - depth(r.previous)) > 1.0f) {
            if (!r.fading) {
                r.second = r.previous;
                r.first_weight = 0.0f;
                r.second_weight = 1.0f;
                r.fading = true;
            } else if (r.second_weight < 0.5f) {
                r.second = r.previous;
                std::swap(r.first_weight, r.second_weight);
            }
        }
    } else {
        r.second = position;
        r.first_weight = 1.0f;
        r.second_weight = 0.0f;
    }
    if (r.fading) {
        r.first_weight = std::min(1.0f, r.first_weight + 3.0f * dt);
        r.second_weight = std::max(0.0f, r.second_weight - 3.0f * dt);
        if (r.first_weight >= 1.0f && r.second_weight <= 0.0f) {
            r.fading = false;
        }
    }
    // the reflection trap boxes the hit is in (ShSpotLightReflectionTrap, registered in the eight slot table 0x1C91900)
    bool kind0 = false;
    bool kind1 = false;
    static const float trap_extent = [] {
        const char* e = std::getenv("PT_REFLECT_TRAP_EXTENT");
        return e ? static_cast<float>(std::atof(e)) : 0.0f;
    }();
    if (trap_extent > 0.0f) {
        stages_.ForEachStage(
            [&](Stage& stage) {
                if (!stage.active) {
                    return;
                }
                for (const auto& data : stage.files) {
                    const fox2::DataSetFile& f = *data->file;
                    for (const fox2::Entity& e : f.Entities()) {
                        if (e.class_name != "ShSpotLightReflectionTrap") {
                            continue;
                        }
                        const glm::mat4 world = stage.ToWorld(f.WorldTransform(e));
                        const glm::vec3 local(glm::inverse(world) * glm::vec4(position, 1.0f));
                        const bool inside = std::abs(local.x) <= trap_extent && std::abs(local.y) <= trap_extent && std::abs(local.z) <= trap_extent;
                        if (std::getenv("PT_REFLECT_LOG")) {
                            LogInfo("reflect trap {} kind {} centre ({:.3f} {:.3f} {:.3f}) local ({:.2f} {:.2f} {:.2f}){}", f.GetString(e, "name"), f.GetInt(e, "trapKind"),
                                    world[3].x, world[3].y, world[3].z, local.x, local.y, local.z, inside ? " inside" : "");
                        }
                        if (inside) {
                            (f.GetInt(e, "trapKind") == 0 ? kind0 : kind1) = true;
                        }
                    }
                }
            },
            false);
    }
    r.spot_share = std::clamp(r.spot_share + (kind0 ? 0.05f : -0.05f) * dt * 30.0f, 0.0f, 1.0f);
    if (std::getenv("PT_REFLECT_LOG")) {
        LogInfo("reflect: hit ({:.3f} {:.3f} {:.3f}) weights {:.2f} {:.2f} spot share {:.2f}", position.x, position.y, position.z, r.first_weight,
                r.second_weight, r.spot_share);
    }
    r.previous = position;
    r.position = position;
    const float outer4 = outer * outer * outer * outer;
    const auto attenuation = [&](const glm::vec3& point) {
        const float w2 = std::max(depth(point), 1.0e-3f) * std::max(depth(point), 1.0e-3f);
        return std::clamp(1.0f / w2 - w2 / outer4, 0.0f, 1.0f);
    };
    r.first_attenuation = attenuation(r.position);
    r.second_attenuation = attenuation(r.second);
    r.active = true;
    // the free camera's and the photo mode's view is not the player's: the colour stays the one read from the player's view
    // (RenderSceneBuilder::AddHandyReflection samples nothing then)
    if (!detached_view_) {
        ++r.readback_age;
    }
}

Camera Game::GetCamera() const {
    if (camera_override_) {
        return *camera_override_;
    }
    Camera camera = player_.MakeCamera();
    // the drawn camera trails the logic's by half a step on a frame's last tick (Player::UpdateBody), the traps' in-view
    // tests keep the logic's (InViewNdc)
    camera.position += player_.DrawnOffset();
    // 0x983B20 (0x984DC2 to 0x984E94): while the peephole theater plays (nazo word 2 bit 8) the camera plugin writes
    // the camera component's +0xB0 (its interface +0x90), the position 0x978450 hands to the camera entity, with the
    // `Peephole` asset translation + rotate(look, +Z): 1 m past the hole along the view. In maze B the bathroom's inner
    // wall stands 1.03 m behind the corridor wall, so the view starts just behind it (its back face) and looks into the
    // bathroom; the zoom (2.0 while the action is armed) and the rotation stay the player's
    glm::vec3 hole(0.0f);
    if (nazo_.IsPeepholeTheaterActive() && nazo_.PeepholePosition(hole)) {
        camera.position = hole + camera.Forward();
    }
    demos_.CameraOverride(camera);
    return camera;
}

Camera Game::PeepholeTurn(const Camera& turned) const {
    glm::vec3 hole(0.0f);
    if (camera_override_ || !nazo_.IsPeepholeTheaterActive() || !nazo_.PeepholePosition(hole)) {
        return turned;
    }
    Camera out = turned;
    player_.ClampPeepholeLook(out.yaw, out.pitch);
    out.position = hole + out.Forward();
    return out;
}

// The third person view (Extras; not in the original, gameplay.md 10.6). The camera looks along the player's own view from
// behind the right shoulder: kThirdShoulder to the right of the head point (the drawn body's feet raised by kThirdHeight, so the
// walk's head bob and the head's roll stay out of it) and kThirdLift above it, then back along the view by a distance that
// depends on the pitch (closer looking down, where the camera rises over the head, and closer looking up, where it sinks toward
// the floor). Each part is kept clear of the surfaces the camera's line checks hit (mask 0x700, LineCollision, as the focus ray):
// the shoulder offset and the lift by rays from the head, the distance by five rays back from the shoulder point (the middle and
// four 0.8 kThirdProbe off it, a box the near plane fits in). A surface that comes closer pulls the camera in at once (it never
// stands inside one); room behind lets it out again over about half a second. Where the corridor is narrower than kThirdWide
// the distance and the shoulder offset shrink (the short pull in through the hallway's doorways). Where little is left behind
// the shoulder (the back to a wall across the hallway) the camera first moves out to the side, up to kThirdCloseShoulder more
// (a close over-the-shoulder view at 0.5 to 0.7 m from the head); only where less than kThirdTooClose is left behind the
// shoulder point, or the camera would come within kThirdHeadClear of the head's middle (the near plane would cut the hood),
// the view goes to first person until kThirdRoomAgain and a little more than that are free. A character drawn in the view
// (Lisa, GameObjects::DrawnCharacters) is a column of kThirdCharacterRadius the camera keeps out of as out of a wall.
namespace {
constexpr float kThirdHeight = 1.60f;
constexpr float kThirdShoulder = 0.36f;
constexpr float kThirdLift = 0.10f;
constexpr float kThirdDistance = 1.60f;
constexpr float kThirdDistanceDown = 1.20f;
constexpr float kThirdDistanceUp = 1.00f;
constexpr float kThirdProbe = 0.15f;
constexpr float kThirdWide = 2.0f;
constexpr float kThirdTooClose = 0.12f;
constexpr float kThirdRoomAgain = 0.3f;
constexpr float kThirdHeadClear = 0.3f;
constexpr float kThirdCloseShoulder = 0.22f;
// The camera looks at the point the player's own view looks at (its line check hit, at most kThirdAimFar away), so the middle
// of the frame is the middle of the first person view: what the zoom and the nazo actions aim at (the peephole, the photo
// pieces, the phone) is aimed at the same way. The turn this takes from the view's direction is at most kThirdAimTurn
constexpr float kThirdAimFar = 12.0f;
constexpr float kThirdAimNear = 0.4f;
constexpr float kThirdAimTurn = 0.35f;
constexpr float kThirdCloseFrom = 0.8f;
constexpr float kThirdCharacterRadius = 0.42f;
constexpr float kThirdCharacterHeight = 1.85f;
constexpr float kThirdEnterSeconds = 0.6f;
constexpr float kThirdLeaveSeconds = 0.25f;
// the camera this far from the player's eye shows the body and the flashlight in the hand
constexpr float kThirdShowBody = 0.35f;

glm::vec3 LookForward(float yaw, float pitch) {
    return glm::vec3(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
}

float WrapAngle(float a) {
    return std::remainder(a, 6.28318531f);
}

// Camera::Right of an unrolled camera
glm::vec3 LookRight(float yaw) {
    return glm::vec3(std::cos(yaw), 0.0f, -std::sin(yaw));
}

}

void Game::SetThirdPerson(bool on) {
    if (on == third_person_) {
        return;
    }
    third_person_ = on;
    // PT_THIRD_DRAWPOSE=0 draws the logic's body instead of the drawn pose (A/B of the feet)
    static const bool drawn_pose = [] {
        const char* e = std::getenv("PT_THIRD_DRAWPOSE");
        return !e || *e != '0';
    }();
    player_.SetDrawPose(on && drawn_pose);
    third_person_weight_ = 0.0f;
    third_person_shown_ = false;
    third_person_primed_ = false;
    third_person_blocked_ = false;
    LogInfo("third person: {}", on ? "on" : "off");
}

glm::vec3 Game::ThirdPersonHead() const {
    return glm::vec3(player_.DrawnBodyTransform()[3]) + glm::vec3(0.0f, kThirdHeight, 0.0f);
}

glm::vec3 Game::ThirdPersonPosition(float yaw, float pitch) const {
    return ThirdPersonHead() + LookRight(yaw) * third_person_shoulder_ + glm::vec3(0.0f, third_person_lift_, 0.0f) -
           LookForward(yaw, pitch) * third_person_distance_;
}

Camera Game::ViewCamera() const {
    Camera camera = GetCamera();
    if (!third_person_ || camera_override_ || third_person_weight_ <= 0.0f) {
        return camera;
    }
    const float w = third_person_weight_;
    const float s = w * w * (3.0f - 2.0f * w);
    const glm::vec3 aim = camera.position + camera.Forward() * third_person_aim_;
    camera.position = glm::mix(camera.position, ThirdPersonPosition(camera.yaw, camera.pitch), s);
    camera.roll *= 1.0f - s;
    const glm::vec3 to_aim = aim - camera.position;
    if (glm::length(to_aim) > 1.0e-3f) {
        const glm::vec3 d = glm::normalize(to_aim);
        const float yaw = camera.yaw + std::clamp(WrapAngle(std::atan2(-d.x, -d.z) - camera.yaw), -kThirdAimTurn, kThirdAimTurn);
        const float pitch = camera.pitch + std::clamp(std::asin(std::clamp(d.y, -1.0f, 1.0f)) - camera.pitch, -kThirdAimTurn, kThirdAimTurn);
        camera.yaw += (yaw - camera.yaw) * s;
        camera.pitch += (pitch - camera.pitch) * s;
    }
    // PT_THIRD_ORBIT="yaw pitch distance [height]" (degrees, metres; tests of the drawn body's feet and hands): the view looks at
    // the drawn body's point that high (0.6 m: the hips; below 0: the left hand) from that yaw about it (0 behind, 90 its right) and that pitch down,
    // ignoring walls
    static const glm::vec4 orbit = [] {
        glm::vec4 v(0.0f, 0.0f, 0.0f, 0.6f);
        if (const char* e = std::getenv("PT_THIRD_ORBIT")) std::sscanf(e, "%f %f %f %f", &v.x, &v.y, &v.z, &v.w);
        return v;
    }();
    if (orbit.z > 0.0f) {
        glm::vec3 target = glm::vec3(player_.PoseTransform()[3]) + glm::vec3(0.0f, orbit.w, 0.0f);
        // a negative height looks at the left hand (the flashlight's grip)
        if (glm::mat4 hand; orbit.w < 0.0f && player_.BoneModel("SKL_013_LHAND", hand)) {
            target = glm::vec3(player_.PoseTransform() * hand[3]);
        }
        camera.yaw = player_.yaw + glm::radians(orbit.x);
        camera.pitch = -glm::radians(orbit.y);
        camera.roll = 0.0f;
        camera.position = target - camera.Forward() * orbit.z;
    }
    return camera;
}

Camera Game::ThirdPersonTurn(const Camera& before, const Camera& after) const {
    if (!third_person_ || camera_override_ || third_person_weight_ <= 0.0f) {
        return after;
    }
    const glm::vec3 head = ThirdPersonHead();
    const glm::vec3 arm = before.position - head;
    const glm::vec3 f0 = LookForward(before.yaw, before.pitch);
    const glm::vec3 r0 = LookRight(before.yaw);
    const glm::vec3 u0 = glm::cross(r0, f0);
    const glm::vec3 f1 = LookForward(after.yaw, after.pitch);
    const glm::vec3 r1 = LookRight(after.yaw);
    const glm::vec3 u1 = glm::cross(r1, f1);
    Camera out = after;
    out.position = head + r1 * glm::dot(arm, r0) + u1 * glm::dot(arm, u0) + f1 * glm::dot(arm, f0);
    return out;
}

void Game::UpdateThirdPerson(float dt) {
    glm::vec3 demo_position(0.0f);
    glm::quat demo_rotation(1.0f, 0.0f, 0.0f, 0.0f);
    float demo_fov = 0.0f;
    // demos and cutscenes keep their own cameras, the script cameras and the free camera theirs; the peephole carries the view
    // through the hole. Each is a cut to first person, and the view comes back from there when it ends
    if (!third_person_ || !player_.spawned || camera_override_ || demos_.ControlsPlayer() || demos_.CameraWorld(demo_position, demo_rotation, demo_fov) ||
        nazo_.IsPeepholeTheaterActive()) {
        third_person_weight_ = 0.0f;
        third_person_shown_ = false;
        third_person_primed_ = false;
        return;
    }
    const Camera view = GetCamera();
    const glm::vec3 head = ThirdPersonHead();
    const glm::vec3 right = LookRight(view.yaw);
    const glm::vec3 forward = LookForward(view.yaw, view.pitch);
    const glm::vec3 up = glm::cross(right, forward);
    const auto room = [&](const glm::vec3& from, const glm::vec3& direction, float length) {
        RayHit hit;
        return line_collision_.Raycast(from, direction, length, hit) ? hit.distance : length;
    };
    const float right_room = room(head, right, kThirdWide);
    const float left_room = room(head, -right, kThirdWide);
    // the corridor's width eases (a doorway or a side opening passing by would jerk the distance), the walls themselves do not
    const float tight_now = std::clamp((kThirdWide - right_room - left_room) / (0.5f * kThirdWide), 0.0f, 1.0f);
    third_person_tight_ = third_person_primed_ ? third_person_tight_ + (tight_now - third_person_tight_) * (1.0f - std::exp(-4.0f * std::max(dt, 0.0f)))
                                              : tight_now;
    const float tight = third_person_tight_;
    const float pitch_share = view.pitch < 0.0f ? -view.pitch / Player::kPitchDown : view.pitch / Player::kPitchUp;
    float want = glm::mix(kThirdDistance, view.pitch < 0.0f ? kThirdDistanceDown : kThirdDistanceUp, std::clamp(pitch_share, 0.0f, 1.0f));
    want *= 1.0f - 0.3f * tight;
    // the aim (ViewCamera): the first person view's line check hit, eased in its logarithm so an edge passing the middle of the
    // view does not swing the camera
    {
        RayHit hit;
        float aim = kThirdAimFar;
        if (line_collision_.Raycast(view.position, view.Forward(), kThirdAimFar, hit)) {
            aim = std::max(hit.distance, kThirdAimNear);
        }
        third_person_aim_ = third_person_primed_ ? std::exp(std::log(third_person_aim_) + (std::log(aim) - std::log(third_person_aim_)) *
                                                                                            (1.0f - std::exp(-8.0f * std::max(dt, 0.0f))))
                                                 : aim;
    }
    const float shoulder_room = std::max(0.0f, right_room - kThirdProbe);
    // the distance found last tick: little room behind moves the camera out to the side
    const float close = third_person_primed_ ? std::clamp((kThirdCloseFrom - third_person_distance_) / (kThirdCloseFrom - kThirdTooClose), 0.0f, 1.0f) : 0.0f;
    const float shoulder_target = std::min(kThirdShoulder * (1.0f - 0.4f * tight) + kThirdCloseShoulder * close, shoulder_room);
    const float lift_room = std::max(0.0f, room(head, glm::vec3(0.0f, 1.0f, 0.0f), kThirdLift + kThirdProbe) - kThirdProbe);
    const float lift_target = std::min(kThirdLift, lift_room);
    // in quickly where a surface comes closer (and never inside it, `limit`), out again with a half life of about 0.2 s
    const auto ease = [&](float& value, float target, float limit) {
        if (!third_person_primed_) {
            value = target;
        } else {
            const float rate = target < value ? 12.0f : 3.0f;
            value += (target - value) * (1.0f - std::exp(-rate * std::max(dt, 0.0f)));
        }
        value = std::min(value, limit);
    };
    ease(third_person_shoulder_, shoulder_target, shoulder_room);
    ease(third_person_lift_, lift_target, lift_room);
    const glm::vec3 shoulder = head + right * third_person_shoulder_ + glm::vec3(0.0f, third_person_lift_, 0.0f);
    const float corner = 0.8f * kThirdProbe;
    const glm::vec3 starts[5] = {shoulder, shoulder + right * corner, shoulder - right * corner, shoulder + up * corner, shoulder - up * corner};
    float free = want;
    for (int i = 0; i < 5; ++i) {
        RayHit hit;
        if (line_collision_.Raycast(starts[i], -forward, want + kThirdProbe, hit)) {
            free = std::min(free, hit.distance - (i == 0 ? kThirdProbe : 0.05f));
        }
    }
    // characters: the ray against a vertical column round each one's feet
    characters_.clear();
    objects_.DrawnCharacters(characters_);
    for (const glm::vec3& feet : characters_) {
        const glm::vec2 d(-forward.x, -forward.z);
        const glm::vec2 m(shoulder.x - feet.x, shoulder.z - feet.z);
        const float a = glm::dot(d, d);
        const float b = 2.0f * glm::dot(m, d);
        const float c = glm::dot(m, m) - kThirdCharacterRadius * kThirdCharacterRadius;
        float t = -1.0f;
        if (c <= 0.0f) {
            t = 0.0f;
        } else if (a > 1.0e-6f && b * b - 4.0f * a * c >= 0.0f) {
            t = (-b - std::sqrt(b * b - 4.0f * a * c)) / (2.0f * a);
        }
        const float y = shoulder.y - forward.y * std::max(t, 0.0f);
        if (t >= 0.0f && t < free + kThirdProbe && y > feet.y - 0.2f && y < feet.y + kThirdCharacterHeight) {
            free = std::min(free, t - 0.05f);
            static const bool character_trace = std::getenv("PT_THIRD_TRACE") != nullptr;
            if (character_trace) {
                LogInfo("third person: character at ({:.2f} {:.2f} {:.2f}) holds the camera to {:.2f} m", feet.x, feet.y, feet.z, std::max(free, 0.0f));
            }
        }
    }
    free = std::max(free, 0.0f);
    const float head_clear = glm::distance(shoulder - forward * free, head);
    if (free < kThirdTooClose || head_clear < kThirdHeadClear) {
        third_person_blocked_ = true;
    } else if (free >= std::min(kThirdRoomAgain, 0.9f * want) && head_clear >= kThirdHeadClear + 0.08f) {
        third_person_blocked_ = false;
    }
    ease(third_person_distance_, free, free);
    third_person_primed_ = true;
    // the zoom (with it the photo pieces, the phone and the peephole, taken while it is held) looks in first person
    const bool first_person = player_.zoom > 1.0001f || player_.zooming || third_person_blocked_;
    const float step = std::max(dt, 0.0f) / (first_person ? kThirdLeaveSeconds : kThirdEnterSeconds);
    third_person_weight_ = std::clamp(third_person_weight_ + (first_person ? -step : step), 0.0f, 1.0f);
    third_person_shown_ = glm::distance(ViewCamera().position, view.position) > kThirdShowBody;
    static const bool trace = std::getenv("PT_THIRD_TRACE") != nullptr;
    if (trace) {
        LogInfo("third person: weight {:.2f} shown {} distance {:.2f} (want {:.2f} free {:.2f}) shoulder {:.2f} lift {:.2f} room {:.2f} {:.2f} tight {:.2f}{}",
                third_person_weight_, third_person_shown_, third_person_distance_, want, free, third_person_shoulder_, third_person_lift_, left_room,
                right_room, tight, third_person_blocked_ ? " blocked" : "");
        LogInfo("third person: {} close {:.2f} characters {}", player_.DescribeDrawPose(), close, characters_.size());
    }
}

uint32_t Game::PostSound(std::string_view event, const glm::vec3& position, bool positional) {
    if (event.empty()) {
        return 0;
    }
    LogInfo("sound: {}{}", event, positional ? std::format(" at ({:.2f} {:.2f} {:.2f})", position.x, position.y, position.z) : std::string());
    return audio_ ? audio_->PostEvent(event, positional ? &position : nullptr) : 0;
}

void Game::PostSoundId(uint32_t id) {
    LogInfo("sound: id {:#x}", id);
    if (audio_) {
        audio_->PostEventId(id, nullptr);
    }
}

uint32_t Game::PostSoundIdAt(uint32_t id, const glm::vec3& position) {
    LogInfo("sound: id {:#x} at ({:.2f} {:.2f} {:.2f})", id, position.x, position.y, position.z);
    return audio_ ? audio_->PostEventId(id, &position) : 0;
}

uint32_t Game::PostSoundIdOnOcho(uint32_t id, const glm::vec3& position) {
    LogInfo("sound: id {:#x} on Lisa at ({:.2f} {:.2f} {:.2f})", id, position.x, position.y, position.z);
    // Lisa's slot of the ShGimmick sound control is her record's sound object (record 1, GimmickType::Ocho)
    return audio_ ? audio_->PostRecordEventId(1, id, position) : 0;
}

void Game::MoveOchoSound(const glm::vec3& position) {
    if (audio_) {
        audio_->MoveRecordSound(1, position);
    }
}

void Game::RegisterAnimEvent(std::string_view anim_event, std::string_view sound_event) {
    anim_sounds_[StrCode64(anim_event) & kStrCode64Mask] = std::string(sound_event);
}

int Game::FoxRandom(int max, int min) {
    fox_rng_ ^= fox_rng_ << 13;
    fox_rng_ ^= fox_rng_ >> 17;
    fox_rng_ ^= fox_rng_ << 5;
    return min + static_cast<int>(static_cast<double>(max - min) * (fox_rng_ / 4294967296.0));
}

void Game::FloorEnvironment() {
    effects_.full_screen_blur = false;
    effects_.maze_viewport = floor_.IsCurrentFloorName("f110");
    effects_.handy_light_color = glm::vec3(1.0f);
    effects_.handy_light_color_fade = false;
    if (floor_.IsCurrentFloorName("f160")) {
        int r = FoxRandom(101, 1);
        if (config_.handy_light_roll > 0) {
            r = config_.handy_light_roll;
        }
        effects_.handy_light_color = r <= 5    ? glm::vec3(1.0f, 0.0f, 0.0f)
                                     : r <= 10 ? glm::vec3(0.0f, 1.0f, 0.0f)
                                     : r <= 15 ? glm::vec3(0.0f, 0.0f, 1.0f)
                                     : r <= 20 ? glm::vec3(1.0f, 1.0f, 0.0f)
                                               : glm::vec3(1.0f);
        effects_.handy_light_color_fade = true;
        LogInfo("floor: f160 handy light roll {} -> ({} {} {})", r, effects_.handy_light_color.r, effects_.handy_light_color.g,
                effects_.handy_light_color.b);
    }
    const bool ending = floor_.IsCurrentFloorName("ending");
    effects_.film_grain_strength = ending ? 0.0f : 1.0f;
    // the REFLECT_MAP plugin's blend constants (0x922C60 -> plugin +0x68/+0x6C/+0x70 -> 0xCB26B0 -> 0xDDFD60 registers 0xA5, 0xB5)
    effects_.reflect_scale = ending ? 3.0f : 2.0f;
    effects_.reflect_bias = ending ? 0.05f : 0.0f;
    effects_.reflect_edge = ending;
    // view +0x460 mask bit of the SUBSURFACE_SCATTER plugin (0xCAF3E0)
    effects_.subsurface_scatter = ending;
    effects_.colour_banding_canceller = false;
    effects_.screen_distortion = !ending;
}

void Game::UpdateBlurSway() {
    if (!effects_.full_screen_blur) {
        return;
    }
    const Camera camera = GetCamera();
    effects_.blur_blend_rate = blur_sway_.Update(frame_delta_, camera.position, camera.Forward(),
                                                 [this](int max, int min) { return FoxRandom(max, min); });
}

bool Game::IsSoundPlaying(uint32_t playing_id) const {
    return audio_ && playing_id != 0 && audio_->IsPlaying(playing_id);
}

void Game::ShowCaption(uint32_t message_id) {
    LogInfo("caption: {:#x}", message_id);
    effects_.caption_id = message_id;
    effects_.caption_time = 0.0f;
}

// a loop transition drops the previous loop's speech: queued subtitles and the caption never reach the next loop, and the
// UI clears what is already playing when it consumes the flag
void Game::ClearPendingSubtitles() {
    subtitle_requests_.clear();
    effects_.caption_id = 0;
    effects_.caption_time = 0.0f;
    clear_subtitles_pending_ = true;
}

bool Game::InViewNdc(const glm::vec3& position, float area) const {
    // 0x917B30 / 0x918560 divide the clip point by w and test ndc.z > -1, |ndc.x| and |ndc.y| < area. The original's projection
    // (captured, reference_captures.md 8) has w = depth along the view and ndc.z = -f / (f - n) + n f / ((f - n) depth), which
    // is below -1 for a point behind the camera (depth < 0) and for one past the far clip: "ndc.z > -1" means 0 < depth < far.
    // Written out in camera space: a GL style matrix gives ndc.z > 1 behind the camera, which passed, and a target behind the
    // player counted as in view (the HELLO letter H beside the corridor's end).
    const Camera camera = player_.MakeCamera();
    const glm::vec3 offset = position - camera.position;
    const float depth = glm::dot(offset, camera.Forward());
    if (!(depth > 0.0f && depth < camera.far_plane)) {
        return false;
    }
    const float tan_half = std::tan(camera.fov_y * 0.5f);
    const float ndc_y = glm::dot(offset, camera.Up()) / (depth * tan_half);
    const float ndc_x = glm::dot(offset, camera.Right()) / (depth * tan_half * (16.0f / 9.0f));
    return ndc_x > -area && ndc_x < area && ndc_y > -area && ndc_y < area;
}

void Game::SetMirrorViewportBit(int bit, bool on) {
    const uint32_t before = mirror_viewport_bits_;
    if (on) {
        mirror_viewport_bits_ |= 1u << bit;
    } else {
        mirror_viewport_bits_ &= ~(1u << bit);
    }
    if (mirror_viewport_bits_ != before) {
        LogInfo("mirror: viewport bits {:#x} ({} {})", mirror_viewport_bits_, bit == 1 ? "High" : "Low", on ? "on" : "off");
    }
}

void Game::CallBgm(std::string_view event) {
    bgm_playing_.insert(std::string(event));
    // Game+ tier 3: on f060, the loop that locks the player in the bathroom with the baby in the sink, unrest01 replaces the
    // herald01 bed the baby's trap calls, until the loop ends (UpdateGamePlusSounds)
    if (GamePlusTier() >= kTierBathroomMusic && event == "Play_bgm_herald01" && floor_.IsCurrentFloorName("f060")) {
        if (!(gp_unrest_ && audio_ && audio_->IsPlaying(gp_unrest_))) {
            gp_unrest_ = PostSound("Play_bgm_unrest01", glm::vec3(0.0f), false);
        }
        gp_unrest_wanted_ = true;
        LogInfo("game+: unrest01 in place of herald01 (f060, frame {})", frame_);
        return;
    }
    // Game+ tier 3: in the red maze (f110) mystery01 replaces the corridor_f110 bed its lights' trap calls
    if (GamePlusTier() >= kTierMazeMusic && event == "Play_bgm_corridor_f110") {
        if (!(gp_maze_ && audio_ && audio_->IsPlaying(gp_maze_))) {
            gp_maze_ = PostSound("Play_bgm_mystery01", glm::vec3(0.0f), false);
        }
        LogInfo("game+: mystery01 in place of corridor_f110 (red maze, frame {})", frame_);
        return;
    }
    PostSound(event, glm::vec3(0.0f), false);
}

void Game::StopBgm(std::string_view event) {
    std::string play = std::string(event);
    if (play.starts_with("Stop_")) {
        play = "Play_" + play.substr(5);
    }
    bgm_playing_.erase(play);
    if (play == "Play_bgm_herald01" && gp_unrest_) {
        StopGamePlusSound(gp_unrest_, 2.0f);
        gp_unrest_wanted_ = false;
    }
    if (play == "Play_bgm_corridor_f110" && gp_maze_) {
        StopGamePlusSound(gp_maze_, 2.0f);
    }
    PostSound(event, glm::vec3(0.0f), false);
}

bool Game::IsPlayingBgm(std::string_view event) const {
    if (gp_unrest_wanted_ && event == "Play_bgm_herald01" && audio_) {
        return audio_->IsPlaying(gp_unrest_);
    }
    if (gp_maze_ && event == "Play_bgm_corridor_f110" && audio_) {
        return audio_->IsPlaying(gp_maze_);
    }
    if (audio_) {
        return audio_->IsEventPlaying(event);
    }
    return bgm_playing_.contains(std::string(event));
}

void Game::SetSubtitleVisible(std::string_view message_id, bool visible) {
    LogInfo("subtitle: {} visible {}", message_id, visible);
    effects_.subtitle_message_id = std::string(message_id);
    effects_.subtitles_visible = visible;
}

void Game::SetFloorLighting(int row) {
    lighting_row_ = row;
}

void Game::ApplyStageTransform() {
    Stage* stage = stages_.Find("next");
    if (!stage) {
        stage = stages_.Find("current");
    }
    if (stage) {
        floor_.SetStageTransform(stage->runtime_root);
    }
}

float Game::PlayerHeightAboveStageRoot() const {
    auto& stages = const_cast<StageManager&>(stages_);
    const Stage* current = stages.Find("current");
    const float root_y = current ? current->runtime_root[3][1] : 0.0f;
    return player_.Feet().y - root_y;
}

// The street walk (gameplay.md, street walk). The ending stage was never walked in the original: its road, sidewalks and buildings
// have no movement hull (geom node 0), only the detailed node 1 surfaces over the models' vertices, which the walk adds to the
// player's world (StageManager::PrepareWalkSurfaces) inside invisible walls around the straight street (the ground ends 1.5 m past
// both short sides, and the side streets and the bend lead out of the visible street). The look is the ending demo's: it is held
// as silent scenery at the frame of its wide street shot (DemoSystem::PlayScenery: its lights, fog, tone and exposure, cars and
// effects; no camera, characters, texts or fades). The player is the outro's man in first person: the walk clips of
// ShPlayer_layers.mtar move the camera as in the hallway.
bool Game::StartStreetWalk() {
    if (!floor_.IsCurrentFloorName("ending") || !stages_.IsActive("current")) {
        LogWarn("street: no ending stage to walk");
        return false;
    }
    demos_.StopAll();
    demos_.PlayScenery("gc_p06_010_final", kStreetSceneryFrame);
    if (Stage* street = stages_.Find("current")) {
        std::vector<GeomTriangle> walls;
        const glm::vec2 corners[4] = {{kStreetMin.x, kStreetMin.y}, {kStreetMax.x, kStreetMin.y}, {kStreetMax.x, kStreetMax.y},
                                      {kStreetMin.x, kStreetMax.y}};
        const glm::vec3 center(0.5f * (kStreetMin.x + kStreetMax.x), 0.0f, 0.5f * (kStreetMin.y + kStreetMax.y));
        for (int i = 0; i < 4; ++i) {
            const glm::vec2 a = corners[i];
            const glm::vec2 b = corners[(i + 1) % 4];
            const glm::vec3 a0(a.x, -3.0f, a.y), a1(a.x, 6.0f, a.y), b0(b.x, -3.0f, b.y), b1(b.x, 6.0f, b.y);
            for (auto t : {std::array{a0, b0, b1}, std::array{a0, b1, a1}}) {
                // the world's triangles are one sided: each wall faces the street. Both windings would push a player touching the
                // wall both ways at once, and he stuck there
                if (glm::dot(glm::cross(t[1] - t[0], t[2] - t[0]), center - t[0]) < 0.0f) {
                    std::swap(t[1], t[2]);
                }
                GeomTriangle tri{};
                tri.a = t[0];
                tri.b = t[1];
                tri.c = t[2];
                walls.push_back(tri);
            }
        }
        stages_.PrepareWalkSurfaces(*street, std::move(walls));
    }
    street_walk_ = true;
    street_pending_ = false;
    street_offer_ = StreetOffer::None;
    player_.spawned = true;
    player_.handy_light.enable = false;
    player_.Warp(kStreetSpawn, 0.0f);
    player_.pitch = player_.target_pitch = 0.0f;
    street_safe_feet_ = kStreetSpawn;
    // the ending demo's first sound (0xdfba4d10) sets its cutscene state, under which the game's own sounds are silent; the
    // original leaves it to the restart's OnPreOpeningDemo, which posts this
    if (audio_) {
        audio_->PostEvent("Set_state_in_game", nullptr);
    }
    street_safe_wait_ = 0.0f;
    effects_.SetFadeColor(0, 0, 0, 255);
    effects_.CallStrongFadeOut(0.0f);
    controller_.SetStep(31);
    LogInfo("street: walk started at ({:.2f} {:.2f} {:.2f})", player_.Feet().x, player_.Feet().y, player_.Feet().z);
    return true;
}

void Game::UpdateStreetWalk(float dt) {
    if (!street_walk_) {
        return;
    }
    // the screen stays black while the scenery runs to its frame, then fades in
    if (demos_.SceneryReady() && effects_.FadeShown().a > 0.5f && !effects_.IsFadeProcessing()) {
        effects_.CallFadeIn(1.5f);
        LogInfo("street: scenery ready, fade in");
    }
    if (audio_ && demos_.SceneryReady() && (street_rain_ == 0 || !audio_->IsPlaying(street_rain_))) {
        street_rain_ = audio_->PostEvent(kStreetAmbience, nullptr);
    }
    const glm::vec3 feet = player_.Feet();
    const bool inside = feet.x > kStreetMin.x && feet.x < kStreetMax.x && feet.z > kStreetMin.y && feet.z < kStreetMax.y;
    if (feet.y < -5.0f || !inside) {
        // a gap in the street's surfaces or a way past the walls: back to the last place stood on
        LogWarn("street: left the street at ({:.2f} {:.2f} {:.2f}); back to ({:.2f} {:.2f} {:.2f})", feet.x, feet.y, feet.z,
                street_safe_feet_.x, street_safe_feet_.y, street_safe_feet_.z);
        player_.Warp(street_safe_feet_ + glm::vec3(0.0f, 0.05f, 0.0f), player_.BodyFoxYaw());
        return;
    }
    street_safe_wait_ += dt;
    if (street_safe_wait_ >= 0.5f && std::abs(feet.y) < 1.0f) {
        street_safe_wait_ = 0.0f;
        street_safe_feet_ = feet;
    }
}

bool Game::OfferStreetWalk() {
    if (street_walk_ || config_.street_offer < 0) {
        return false;
    }
    if (config_.street_offer > 0) {
        street_offer_ = config_.street_offer == 1 ? StreetOffer::Walk : StreetOffer::Restart;
        LogInfo("street: offer answered by --street-offer ({})", config_.street_offer == 1 ? "walk" : "restart");
        return true;
    }
    street_offer_ = StreetOffer::Asking;
    LogInfo("street: credits finished, walk offered");
    return true;
}

void Game::AnswerStreetOffer(bool walk) {
    if (street_offer_ != StreetOffer::Asking) {
        return;
    }
    street_offer_ = walk ? StreetOffer::Walk : StreetOffer::Restart;
    menu_close_request_ = true;
    LogInfo("street: offer answered: {}", walk ? "walk the street" : "restart");
}

void Game::StartPortCredits() {
    port_credits_time_ = 0.0f;
    port_credits_skip_ = false;
    LogInfo("credits: port credits page started ({:.1f} s)", port_credits::Duration());
}

bool Game::UpdatePortCredits(float dt) {
    if (port_credits_time_ < 0.0f) {
        return true;
    }
    port_credits_time_ += dt;
    if (!port_credits_skip_ && port_credits_time_ < port_credits::Duration()) {
        return false;
    }
    LogInfo("credits: port credits page {} at {:.2f} s", port_credits_skip_ ? "skipped" : "finished", port_credits_time_);
    port_credits_time_ = -1.0f;
    port_credits_skip_ = false;
    return true;
}

void Game::SkipPortCredits() {
    if (port_credits_time_ >= 0.0f) {
        port_credits_skip_ = true;
    }
}

void Game::UpdateStreetOffer() {
    if (street_offer_ == StreetOffer::Walk) {
        street_offer_ = StreetOffer::None;
        if (!StartStreetWalk()) {
            controller_.SetStep(29);
        }
    } else if (street_offer_ == StreetOffer::Restart) {
        street_offer_ = StreetOffer::None;
        controller_.SetStep(29);
    }
}

void Game::LeaveStreetWalk() {
    if (!street_walk_) {
        return;
    }
    LogInfo("street: left by the player, ending restart follows");
    EndStreetWalk();
    controller_.SetStep(29);
}

void Game::EndStreetWalk() {
    const bool walking = street_walk_;
    street_walk_ = false;
    street_pending_ = false;
    street_offer_ = StreetOffer::None;
    if (audio_ && street_rain_) {
        audio_->StopPlayingId(street_rain_, 1.0f);
    }
    street_rain_ = 0;
    if (walking) {
        demos_.StopAll();
    }
}

void Game::NoteBrowseReached(int index) {
    if (index < 0 || index >= static_cast<int>(kBrowseLoops.size()) || (browse_reached_ & (1u << index))) {
        return;
    }
    browse_reached_ |= 1u << index;
    ++browse_unlock_generation_;
    LogInfo("loop browser: {} reached in play, its entry unlocks", kBrowseLoops[index].label);
}

void Game::SpeedrunStartGame() {
    if (!speedrun_.Enabled() || speedrun_.GetState() == SpeedrunTimer::State::Running) return;
    // a loop browser pick is not a run (its own picks abandon one)
    if (browse_save_suppressed_ || browse_waiting_entry_) return;
    bool solved = false;
    for (NazoId id : {NazoId::XMark, NazoId::Hello, NazoId::Peephole, NazoId::Photo, NazoId::TrueEnd}) solved = solved || nazo_.IsCleared(id);
    const bool full = floor_.IsCurrentFloorName("f000") && floor_.LoopCount() <= 1 && !solved;
    speedrun_.Start(floor_.CurrentFloorName(), full);
}

void Game::SpeedrunFloorLeft(std::string_view floor, int pass) {
    speedrun_.Split(floor, pass);
}

void Game::SpeedrunEnding() {
    speedrun_.Finish(floor_.CurrentFloorName(), floor_.LoopCount());
}

void Game::AnswerSpeedrunMenu() {
    if (street_offer_ != StreetOffer::Asking) {
        return;
    }
    options_after_restart_ = true;
    AnswerStreetOffer(false);
    LogInfo("speedrun: return to menu (option screen first)");
}

// Game+ tier 1 (docs/gameplay.md, Game+): the unused headless Lisa in the bathtub (shsb_bath001_ocho001, hidden by its entity
// flags 6 in the hallway and maze C, no script shows her) sits in the tub of the bathroom whose sink holds the baby, in every
// loop and only while the baby is there (the Baby gimmick drawn; its locators are in the sinks: hallway and maze C environ
// (-1.56, 0.94, -18.27), mazes A and B (-27.8, 0.94, -4.25)). Each tub is a placement of shsb_bath001.fmdl (the hallway's at
// identity, maze B's shsb_bath001_0001 turned 180 degrees), and her vertices share the tub model's frame, so her draw takes the
// placement of the tub nearest the baby (her middle within 3.5 m of it: 2.2 m in every bathroom). An extra stage draw without an
// entity, visual only, no collision; the mesh is the model cache's (from the hallway or maze C, or read directly).
namespace {
constexpr const char* kTubModel = "/shsb_bath001.fmdl";
constexpr const char* kTubLisaModel = "/Assets/sh/environ/object/shsb/bath/shsb_bath001/scenes/shsb_bath001_ocho001.fmdl";
const glm::vec4 kTubLisaMiddle(-2.07f, 0.66f, -20.43f, 1.0f);
}

void Game::UpdateGamePlus() {
    const Gimmick& baby = objects_.Gimmicks()[static_cast<size_t>(GimmickType::Baby)];
    const bool show = GamePlusTier() >= 1 && objects_.GimmickDrawn("Baby");
    if (show && !tub_lisa_mesh_) {
        stages_.ForEachStage([&](Stage& stage) {
            for (const Stage::Draw& draw : stage.draws) {
                if (!tub_lisa_mesh_ && draw.entity && draw.mesh && draw.file &&
                    draw.file->file->EntityName(*draw.entity).ends_with("shsb_bath001_ocho001_0000")) {
                    tub_lisa_mesh_ = draw.mesh;
                }
            }
        });
        if (!tub_lisa_mesh_) {
            if (const ModelEntry* model = models_.Get(kTubLisaModel)) tub_lisa_mesh_ = model->mesh.get();
        }
    }
    const glm::vec3 baby_at(baby.world[3]);
    stages_.ForEachStage([&](Stage& stage) {
        const StageData* want_file = nullptr;
        glm::mat4 want_world(1.0f);
        if (show && tub_lisa_mesh_ && stage.active) {
            for (const auto& file : stage.files) {
                for (const auto& placement : file->static_models) {
                    if (!placement.model_file.ends_with(kTubModel)) continue;
                    const glm::vec3 middle(stage.file_to_world * placement.world * kTubLisaMiddle);
                    if (glm::distance(middle, baby_at) < 3.5f) {
                        want_file = file.get();
                        want_world = placement.world;
                    }
                }
            }
        }
        auto it = std::find_if(stage.draws.begin(), stage.draws.end(),
                               [&](const Stage::Draw& d) { return d.entity == nullptr && d.mesh && d.mesh == tub_lisa_mesh_; });
        const bool shown = it != stage.draws.end();
        if (shown && (!want_file || it->file_transform != want_world)) {
            stage.draws.erase(it);
            stages_.MarkVisualsDirty();
            LogInfo("game+: tub Lisa leaves {}, baby not in its sink", stage.label);
        }
        const bool still = std::any_of(stage.draws.begin(), stage.draws.end(),
                                       [&](const Stage::Draw& d) { return d.entity == nullptr && d.mesh && d.mesh == tub_lisa_mesh_; });
        if (want_file && !still) {
            stage.draws.push_back({want_file, nullptr, tub_lisa_mesh_, want_world, glm::vec4(1.0f)});
            stages_.MarkVisualsDirty();
            const glm::vec3 at(stage.file_to_world * want_world * kTubLisaMiddle);
            LogInfo("game+: tub Lisa in {} ({}), baby in its sink, world ({:.2f} {:.2f} {:.2f})", stage.label,
                    floor_.CurrentFloorName(), at.x, at.y, at.z);
        }
    });
}

bool Game::GamePlusTubShown() const {
    bool shown = false;
    const_cast<StageManager&>(stages_).ForEachStage([&](Stage& stage) {
        if (!stage.active) return;
        for (const Stage::Draw& d : stage.draws) shown = shown || (d.entity == nullptr && d.mesh && d.mesh == tub_lisa_mesh_);
    });
    return shown;
}

void Game::StopGamePlusSound(uint32_t& id, float fade) {
    if (id && audio_) audio_->StopPlayingId(id, fade);
    id = 0;
}

// Game+ tier 3 (docs/gameplay.md, Game+): the original's unused sound events, posted as the original posts its own (their banks'
// bus, volume and 2D or 3D positioning; the pause menu's Pause_All and a game over's Stop_ALL reach them as every sound), each
// with one playing id so none stacks or repeats:
// - the unnamed 56 s music bed 0x1A95368D loops under the street walk;
// - Play_bgm_mystery01 replaces corridor_f110 on f110, the red maze (Game::CallBgm), and fades when the loop ends;
// - Play_bgm_unrest01 replaces herald01 on f060 (Game::CallBgm) and fades when the loop ends;
// - Play_radio_voice_06, 07 and 08 follow the radio's broadcast on both f050 passes (the loops in which Lisa haunts the player
//   from behind, the second the gouge loop), at the radio, back to back, with their own subtitles (tria1000_1d1010, 1e1010,
//   1f1010, started by the media's markers as the broadcast's are); 08 ("You, what did you do? ... MURDERER!") answers 07's "by your own hand", so it comes last;
// - Play_sfx_heartbeat (its own layered loop) under the stand-up gc_p00_020 after a game over;
// - Lisa's laughs at the HELL writing (HELL_Ican, f070 to f100): the first visit of a session takes laughs 2 then 3 of
//   Play_voice_ene_laugh_01's six as one sound, the next visit 4 then 6, alternating (a visit: coming within 2.5 m; leaving
//   past 4 m ends it).
void Game::UpdateGamePlusSounds() {
    if (!audio_) return;
    const int tier = GamePlusTier();
    auto alive = [&](uint32_t id) { return id != 0 && audio_->IsPlaying(id); };
    const int step = controller_.Step();
    const std::string floor = floor_.CurrentFloorName();
    // the street walk's bed
    const bool street = tier >= kTierStreetAmbience && street_walk_ && demos_.SceneryReady();
    if (street && !alive(gp_street_)) {
        gp_street_ = audio_->PostEventId(0x1A95368Du, nullptr);
        LogInfo("game+: street walk ambience (frame {})", frame_);
    } else if (!street && gp_street_) {
        StopGamePlusSound(gp_street_, 1.5f);
    }
    // the red maze's bed (Game::CallBgm) ends with the loop
    if (gp_maze_ && (floor != "f110" || step != 15)) {
        StopGamePlusSound(gp_maze_, 2.0f);
    }
    // the bathroom loop's bed ends with the loop
    if (gp_unrest_ && (floor != "f060" || step != 15)) {
        StopGamePlusSound(gp_unrest_, 2.0f);
        gp_unrest_wanted_ = false;
    }
    // the stand-up after a game over
    // (the game over's steps 16 to 20; the boot's own gc_p00_020 is not one)
    if (step >= 16 && step <= 20) gp_heartbeat_armed_ = true;
    const bool standing_up = demos_.IsPlaying("gc_p00_020");
    if (tier >= kTierHeartbeat && gp_heartbeat_armed_ && standing_up && !gp_heartbeat_) {
        gp_heartbeat_ = PostSound("Play_sfx_heartbeat", glm::vec3(0.0f), false);
        LogInfo("game+: heartbeat under the stand-up (frame {})", frame_);
    }
    if (gp_heartbeat_ && !standing_up) {
        StopGamePlusSound(gp_heartbeat_, 0.8f);
        gp_heartbeat_armed_ = false;
    }
    // the radio's extra lines on f050
    const int pass = floor_.LoopCount();
    if (floor != gp_radio_floor_ || pass != gp_radio_pass_) {
        StopGamePlusSound(gp_radio_line_, 0.5f);
        gp_radio_floor_ = floor;
        gp_radio_pass_ = pass;
        gp_radio_stage_ = 0;
        gp_radio_seen_ = false;
    }
    if (tier >= kTierRadioLines && floor == "f050" && step == 15) {
        const bool broadcast = audio_->IsEventPlaying("Play_radio_f050");
        if (broadcast) gp_radio_seen_ = true;
        // the radio of the hallway copy the player is in: the nearest of the active stages' radios
        glm::vec3 radio(0.0f);
        float radio_distance = 1e30f;
        stages_.ForEachStage([&](Stage& stage) {
            if (!stage.active) return;
            for (const auto& file : stage.files) {
                if (const fox2::Entity* e = file->file->ByShortName("FxLocator_fxsd_sh_radio_f050")) {
                    const glm::vec3 at = glm::vec3(stage.ToWorld(file->file->WorldTransform(*e))[3]);
                    if (glm::distance(at, player_.Feet()) < radio_distance) {
                        radio_distance = glm::distance(at, player_.Feet());
                        radio = at;
                    }
                }
            }
        });
        const bool close_by = radio_distance < 12.0f;
        static constexpr const char* kLines[3] = {"Play_radio_voice_06", "Play_radio_voice_07", "Play_radio_voice_08"};
        if (gp_radio_seen_ && !broadcast && gp_radio_stage_ < 3 && !alive(gp_radio_line_)) {
            if (close_by) {
                gp_radio_line_ = PostSound(kLines[gp_radio_stage_], radio, true);
                LogInfo("game+: radio line {} after the broadcast ({} pass {}, frame {})", kLines[gp_radio_stage_], floor, pass, frame_);
                ++gp_radio_stage_;
            } else {
                gp_radio_stage_ = 3;
            }
        }
    }
    // Lisa's laughs at the HELL writing
    glm::vec3 writing(0.0f);
    const bool hell_floor = floor == "f070" || floor == "f080" || floor == "f090" || floor == "f100";
    if (tier >= kTierLaughs && step == 15 && hell_floor && nazo_.ControlAssetPosition("HELL_Ican", writing)) {
        const float distance = glm::distance(glm::vec2(writing.x, writing.z), glm::vec2(player_.Feet().x, player_.Feet().z));
        if (!gp_laugh_near_ && distance < 2.5f && !alive(gp_laugh_)) {
            gp_laugh_near_ = true;
            const bool first = gp_laugh_visits_ % 2 == 0;
            ++gp_laugh_visits_;
            // Play_voice_ene_laugh_01's takes 2 and 3 (media 748023997, 1065241238), or 4 and 6 (73183476, 823343976)
            std::vector<uint32_t> takes = first ? std::vector<uint32_t>{748023997u, 1065241238u} : std::vector<uint32_t>{73183476u, 823343976u};
            gp_laugh_ = audio_->PostEventMedia("Play_voice_ene_laugh_01", std::move(takes), &writing);
            LogInfo("game+: Lisa laughs at the HELL writing ({} pair, visit {}, frame {})", first ? "first" : "second", gp_laugh_visits_, frame_);
        } else if (gp_laugh_near_ && distance > 4.0f) {
            gp_laugh_near_ = false;
        }
    }
}

// Game+ tier 4 (docs/gameplay.md, Game+): the original's unused demos, each started with the used demo it was made beside, by
// that demo's own trap and at its origin (they share its frame): gc_p03_012's 13 roaches with gc_p03_011's (f060, round the
// bathroom's toilet and walls), gc_p04_330's 7 with gc_p04_320's (f050, the lobby wall and the first corridor's end),
// gc_p04_200's 7 with gc_p04_120 (f070, down the basement stairs at the lobby's end); looping as their sibling does
void Game::OnTrapDemo(std::string_view demo_id, const glm::mat4& origin, bool loop) {
    if (GamePlusTier() < kTierDemos) return;
    static constexpr std::pair<std::string_view, std::string_view> kSiblings[] = {
        {"gc_p03_011", "gc_p03_012"}, {"gc_p04_320", "gc_p04_330"}, {"gc_p04_120", "gc_p04_200"}};
    for (const auto& [used, unused] : kSiblings) {
        if (demo_id != used || !demos_.Find(unused) || demos_.IsPlaying(unused)) continue;
        demos_.SetDemoTransform(unused, glm::quat_cast(glm::mat3(origin)), glm::vec3(origin[3]));
        if (demos_.Play(unused)) {
            if (loop) demos_.ToggleLoop(unused);
            LogInfo("game+: demo {} with {} at ({:.2f} {:.2f} {:.2f}) (frame {})", unused, used, origin[3].x, origin[3].y, origin[3].z, frame_);
        }
    }
}

// Game+ tier 4: on f100 the man's footsteps, gc_p04_290 (an effect and Play_man_fs_f100_01), once a loop, a few seconds into
// it, in the frame f070's gc_p04_280 plays in (its demo element's demoCenterLink, in the hallway's first corridor), of the hallway
// copy the player is in
void Game::UpdateGamePlusDemos() {
    const int step = controller_.Step();
    const std::string floor = floor_.CurrentFloorName();
    const int pass = floor_.LoopCount();
    if (gp_man_floor_ != floor || gp_man_pass_ != pass) {
        gp_man_floor_ = floor;
        gp_man_pass_ = pass;
        gp_man_since_ = frame_;
        gp_man_done_ = false;
    }
    if (GamePlusTier() < kTierDemos || step != 15 || floor != "f100" || gp_man_done_ || frame_ - gp_man_since_ < 360) return;
    if (!demos_.Find("gc_p04_290")) return;
    gp_man_done_ = true;
    glm::mat4 origin(1.0f);
    float best = 1e30f;
    stages_.ForEachStage([&](Stage& stage) {
        if (!stage.active) return;
        for (const auto& file : stage.files) {
            const fox2::DataSetFile& data = *file->file;
            for (const fox2::Entity& e : data.Entities()) {
                if (!e.Find("demoId") || data.GetString(e, "demoId") != "gc_p04_280") continue;
                const fox2::Entity* center = data.GetEntity(e, "demoCenterLink");
                if (!center) continue;
                const glm::mat4 at = stage.ToWorld(data.WorldTransform(*center));
                const float d = glm::distance(glm::vec3(at[3]), player_.Feet());
                if (d < best) {
                    best = d;
                    origin = at;
                }
            }
        }
    });
    if (best > 1e29f) return;
    demos_.SetDemoTransform("gc_p04_290", glm::quat_cast(glm::mat3(origin)), glm::vec3(origin[3]));
    if (demos_.Play("gc_p04_290")) {
        LogInfo("game+: demo gc_p04_290, the man's footsteps, at ({:.2f} {:.2f} {:.2f}) (f100 pass {}, frame {})", origin[3].x, origin[3].y,
                origin[3].z, pass, frame_);
    }
}

int Game::GamePlusTier() const {
#if PT_GAMEPLUS
    static const int forced = [] {
        const char* v = std::getenv("PT_GAMEPLUS_TIER");
        return v && *v ? std::atoi(v) : -1;
    }();
    if (forced >= 0) return forced;
    return game_plus_ ? static_cast<int>(finishes_) : 0;
#else
    return 0;
#endif
}

void Game::EnableGamePlus() {
    if (!game_plus_) {
        LogInfo("game+: finished flag set (save tag)");
    }
    game_plus_ = true;
    ++finishes_;
    LogInfo("game+: finish {} counted", finishes_);
}

void Game::NoteGameFinished() {
    NoteBrowseReached(kBrowseStreet);
    if (!game_finished_) {
        game_finished_ = true;
        ++browse_unlock_generation_;
        LogInfo("loop browser: finished flag set");
    }
}

void Game::OnFloorReached(std::string_view floor, int pass) {
    NoteArchive("floor:" + std::string(floor));
    // a pick of the loop browser is not play: its own floors unlock nothing
    if (browse_save_suppressed_ || browse_waiting_entry_) {
        return;
    }
    NoteBrowseReached(BrowseIndexOf(floor, pass));
}

void Game::NoteArchive(std::string_view key) {
    if (config_.theater || !IsArchiveUnlockKey(key) || archive_seen_.contains(std::string(key))) {
        return;
    }
    archive_seen_.insert(std::string(key));
    ++archive_generation_;
    LogInfo("archive: {} reached in play", key);
}

bool Game::ArchiveUnlocked(const ArchiveEntry& entry) const {
    if (!config_.release_locks) {
        return true;
    }
    // the loop browser's rule (BrowseUnlocked): nothing before the game was finished once, then what play reached
    if (!game_finished_) {
        return false;
    }
    const std::string key = ArchiveUnlockKey(entry);
    return archive_seen_.contains(key) || key == "finished" || entry.section == ArchiveSection::Unused;
}

bool Game::ResetProgress() {
    if(!save_store_.Enabled()) return false;
    const bool reset=save_store_.Reset();
    // A live restart must resume saving; Reset() disables the old save job.
    if(reset) {
        save_store_.SetDirectory(config_.save_path.parent_path(),"PT_Save_Data");
        game_plus_=false;
        finishes_ = 0;
        // the reset runs the ending's restart (step 30), which keeps the save floor as the original does; a reset starts at f000
        floor_.SetSaveFloorName("f000");
        browse_save_suppressed_ = browse_waiting_entry_ = browse_door_placed_ = false;
        browse_arrived_ = true;
        browse_loop_ = -1;
        // a pick still fading out is superseded by the reset
        loop_reload_pending_ = false;
        browse_fade_wait_ = 0.0f;
        progress_reset_pending_=true;
        speedrun_.Abandon("progress reset");
    }
    return reset;
}

void Game::TearDownSession() {
    demos_.StopAll();
    nazo_.StopVoiceRecognition();
    if (audio_) {
        audio_->ResetSession();
    }
    ClearPendingSubtitles();
    PostSound("Set_state_none", glm::vec3(0.0f), false);
    bgm_playing_.clear();
    controller_.RequestNewSession();
    controller_.ClearPadEnable();
}

// What a boot sets up before its first floor and the session then changes, outside the gimmick records (GameObjects::Reset)
// and Lisa (OchoLogic::Setup): the stages are unloaded (step 20), the floor and the puzzles come next from the reset itself
// (step 30) or the browser (ApplyBrowseReload). Options, the save store, Game+ and the browser's own state stay.
void Game::ResetSessionState() {
    nazo_.ResetSession();
    demos_.ResetPlayerBody();
    floor_.SetPending(0);
    const bool ev_pinned = effects_.ev_pinned;
    const float pinned_ev = effects_.pinned_ev;
    effects_ = boot_effects_;
    effects_.ev_pinned = ev_pinned;
    effects_.pinned_ev = pinned_ev;
    player_.handy_light = HandyLight{};
    light_target_valid_ = false;
    handy_hold_ = handy_demo_ = 0.0f;
    handy_hold_pending_ = handy_hold_release_ = false;
    handy_reflection_ = HandyReflection{};
    mirror_viewport_bits_ = 0;
    lighting_row_ = 0;
    blur_sway_ = BlurSway{};
    bgm_playing_.clear();
    subtitle_requests_.clear();
    clear_subtitles_pending_ = true;
    EndStreetWalk();
    LogInfo("game: session state reset as at boot");
}

std::string Game::DescribeSessionState() const {
    std::string text;
    auto line = [&](std::string_view key, const std::string& value) { text += std::format("{}: {}\n", key, value); };
    line("floor", std::format("{} loop {} save {} pending {} lisa killed {}", floor_.CurrentFloorName(), floor_.LoopCount(), floor_.SaveFloorName(),
                              floor_.Pending(), floor_.LisaKilled()));
    line("nazo", nazo_.DescribeSession());
    for (const Gimmick& g : objects_.Gimmicks()) {
        std::string hidden;
        for (uint64_t mesh : g.hidden_meshes) {
            hidden += std::format(" {:#x}", mesh);
        }
        line("gimmick " + g.name,
             std::format("enabled {} shown {} placed {} active {} mesh {} motion {} requested {} rate {:.2f} logic {} lights {}{}{} stage light {}{} "
                         "strong {} views {} sound {} hidden{}",
                         g.enabled, g.shown, g.placed, g.active, g.mesh != nullptr, g.motion, g.motion_requested, g.anim_rate, g.logic_state,
                         g.lights[0], g.lights[1], g.lights[2], g.stage_light, g.stage_light_red, g.freezer_strong, g.hidden_views,
                         !g.sound_handles.empty(), hidden));
    }
    auto& ocho = const_cast<GameObjects&>(objects_).Ocho();
    line("ocho", std::format("state {} visible {} killed {}", ocho.State(), ocho.Visible(), ocho.HasKilled()));
    const ScreenEffects& e = effects_;
    line("effects", std::format("lut {} {} blur {} {:.2f} {:.2f} grain {} {:.2f} banding {} distortion {} handy colour ({:.2f} {:.2f} {:.2f}) {} mirror {} "
                                "maze {} subtitles {} {} {} subliminal {} {} {} overlay '{}' caption {}",
                                e.lut_control, e.lut, e.full_screen_blur, e.blur_blend_rate, e.blur_fetch_band, e.film_grain, e.film_grain_strength,
                                e.colour_banding_canceller, e.screen_distortion, e.handy_light_color.r, e.handy_light_color.g, e.handy_light_color.b,
                                e.handy_light_color_fade, e.mirror_capture, e.maze_viewport, e.subtitles_enabled, e.subtitles_visible,
                                e.subtitle_message_id, e.subliminal_image, e.subliminal_flag, e.subliminal_no_string, e.overlay_texture, e.caption_id));
    line("player", std::format("handy light {} {:.2f} locks {}", player_.handy_light.enable, player_.handy_light.lumen, player_.locks.Describe()));
    std::string body;
    for (uint64_t mesh : const_cast<DemoSystem&>(demos_).PlayerHidden()) {
        body += std::format(" {:#x}", mesh);
    }
    line("player body hidden", body);
    std::string playing;
    for (const auto& demo : demos_.Playing()) {
        playing += " " + demo.demo_id;
    }
    line("demos", playing);
    std::string bgm;
    for (const std::string& event : bgm_playing_) {
        bgm += " " + event;
    }
    line("bgm", bgm);
    line("view", std::format("mirror bits {:#x} lighting row {} show body {} camera override {} paused {}", mirror_viewport_bits_, lighting_row_,
                             show_body_, camera_override_.has_value(), paused_));
    line("speech", std::format("queued {} clear pending {}", subtitle_requests_.size(), clear_subtitles_pending_));
    line("status", status_.Describe());
    return text;
}

void Game::ResetSaveBlocks() {
    floor_.SetSaveFloorName("f000");
    floor_.SetFloorLevel("f000");
    floor_.ResetLoopCount();
    nazo_.ResetAllStates();
    // Options_StaticInit (0x91D7B0): the defaults follow the system language; a save read next replaces them (RequestLoad)
    const std::string system_language = SystemLanguageTag();
    options_ = DefaultOptionsForLocale(system_language);
    LogInfo("options: defaults for system language {}: subtitles {} language {}", system_language, options_.subtitles,
            options_.subtitle_language);
    game_plus_ = false;
    finishes_ = 0;
}

bool Game::BrowseLoop(int index) {
    if (index < 0 || index >= static_cast<int>(kBrowseLoops.size())) return false;
    // only from the running game, the start room's opening, the ending demo or the street walk (steps 15, 13, 28 and 31), which a
    // reset leaves as a game over does: a pick during a reset, a game over, the preface, the first boot's option screen or the
    // end of the credits would restart a sequence the controller is still running
    const int step = controller_.Step();
    if ((step != 13 && step != 15 && step != 28 && step != 31) || controller_.RequestedStep() != step || loop_reload_pending_) {
        LogInfo("loop browser: {} refused at controller step {}", kBrowseLoops[index].floor, step);
        return false;
    }
    speedrun_.Abandon("loop browser pick");
    browse_loop_ = index;
    browse_save_suppressed_ = true;
    browse_waiting_entry_ = true;
    browse_door_placed_ = false;
    browse_door_wait_ = 0;
    browse_arrived_ = false;
    loop_reload_pending_ = true;
    browse_fade_wait_ = kBrowseFadeSeconds;
    effects_.CallStrongFadeOut(kBrowseFadeSeconds);
    LogInfo("loop browser: selected {} (pass {}); save writes suspended", kBrowseLoops[index].floor, kBrowseLoops[index].pass);
    return true;
}

void Game::PrepareTheaterLoop(int index) {
    if (!config_.theater || index < 0 || index >= static_cast<int>(kBrowseLoops.size())) return;
    const auto& loop = kBrowseLoops[index];
    browse_loop_ = index;
    browse_save_suppressed_ = true;
    browse_waiting_entry_ = index > 0;
    browse_door_placed_ = false;
    browse_door_wait_ = 0;
    browse_arrived_ = index == 0;
    floor_.SetFloorLevel(loop.previous);
    floor_.ResetLoopCount();
    if (index >= 8) nazo_.ForceClear(NazoId::XMark);
    if (index >= 12) nazo_.ForceClear(NazoId::Hello);
    if (index >= 14) nazo_.ForceClear(NazoId::Peephole);
    nazo_.CancelPendingClearSound();
    controller_.first_boot = false;
    controller_.replay_preface = false;
    LogInfo("theater: entering {} (pass {}) from the start room on {}", loop.floor, loop.pass, loop.previous);
}

bool Game::ApplyBrowseReload() {
    if (browse_loop_ < 0 || !browse_waiting_entry_) return false;
    const auto& loop = kBrowseLoops[browse_loop_];
    ClearPendingSubtitles();
    nazo_.ResetAllStates();
    floor_.SetFloorLevel(loop.previous);
    floor_.ResetLoopCount();
    if (browse_loop_ >= 8) nazo_.ForceClear(NazoId::XMark);
    if (browse_loop_ >= 12) nazo_.ForceClear(NazoId::Hello);
    // f110's peephole sends the player on to f120; a browsed f120 or later starts with it solved, as played progress would
    if (browse_loop_ >= 14) nazo_.ForceClear(NazoId::Peephole);
    nazo_.CancelPendingClearSound();
    controller_.first_boot = false;
    controller_.replay_preface = false;
    if (browse_loop_ == 0) {
        browse_waiting_entry_ = false;
        browse_arrived_ = true;
    }
    if (loop.floor == "ending" || loop.floor == "street") {
        // the street entry loads the ending stage as the ending does and starts the walk in place of its demo (step 27)
        street_pending_ = loop.floor == "street";
        floor_.SetFloorLevel("ending");
        browse_waiting_entry_ = false;
        browse_arrived_ = true;
        controller_.ChangeGameStep("GotoEnding");
        return true;
    }
    return false;
}

void Game::OnBrowseFloorEntered() {
    if (!browse_waiting_entry_ || browse_loop_ < 0) return;
    const auto& loop = kBrowseLoops[browse_loop_];
    floor_.SetFloorLevel(loop.floor);
    floor_.SetLoopCount(loop.pass);
    // a boot through f000 and f010 leaves every puzzle it has not reached deactivated (word 1, FloorLevel::RelocateGimmicks); the
    // browser's session reset leaves word 0, which has no visual state, so a browsed loop kept the puzzle assets of the session
    // before it (the XMark photo of the last puzzle). Before this floor's own setup runs, they take the boot's value
    nazo_.DeactivateUnset();
    browse_waiting_entry_ = false;
    browse_arrived_ = true;
    if (loop.floor == "f160") {
        // the final loop is only reached through Endf120, whose checkpoint saves f120 with every puzzle before it solved; f160's
        // exit door saves nothing, so without this the browsed final loop (and the ending after it) would never save again
        EndBrowseSuppression("entered f160 (the Endf120 checkpoint)");
        floor_.SetSaveFloorName("f120");
        RequestSave();
        return;
    }
    LogInfo("loop browser: entered {} (pass {}), save still suspended", loop.floor, loop.pass);
}

void Game::OnBasementExit() {
    if (!browse_save_suppressed_ || browse_waiting_entry_) return;
    EndBrowseSuppression("basement progression");
}

void Game::EndBrowseSuppression(std::string_view why) {
    if (!browse_save_suppressed_ && browse_loop_ < 0 && !browse_waiting_entry_) return;
    browse_save_suppressed_ = false;
    browse_waiting_entry_ = false;
    browse_arrived_ = true;
    browse_loop_ = -1;
    LogInfo("loop browser: {}; normal saving resumed", why);
}

// A browsed loop starts as a save made when its loop began would boot: the reset (ApplyBrowseReload) loads the start room on
// the loop's previous floor, the floor such a save holds (GoNextFloor stores the floor it leaves, FloorLevel::GoNextFloor), with
// the puzzles of the floors before it solved, and the start room's door then advances into the loop (NextFloor,
// OnBrowseFloorEntered). The handy light comes from the same path: OnPreGame (step 14) sends SetupFlashLight, and the start
// room's gameControllFlashLight.lua lights it only when the current floor is f060, f070, f080, f090, f120 or f160
// (gameplay.md 4.6). That script must read the start room's floor. Its message is delivered a game frame after OnPreGame
// (MessageSystem::PostControllerMessage), and the door, taken at once, advanced the floor first, so the script read the
// browsed floor: the f060 hallway (the bathroom aftermath, where gc_p00_030 hands the player the light from the toilet) and
// f120 started lit, the f050 passes, f100 and f110 dark. The player is therefore put in the door only once OnPreGame's posts
// are delivered, as a player who walks to the door always is.
// What a resume cannot replay is the pickup itself: in one sitting from f000's StartGame the light stays on from gc_p00_030
// on f060 (demo functor 0x36A45BC5DA0B) until maze A turns it off on f110 (trapFlashLightControl.lua); Endf120's restart runs
// StartGame on f120 again. So a loop whose floor comes after f060 and up to f110 in the floor table (both f050 passes, f070,
// f080, f090, f100, f110) is entered lit, as after the pickup: f070 and f110 follow start rooms on f050 and f100, which
// SetupFlashLight leaves dark (a resumed game is dark there, a played one is not), and f110 turns it off itself at maze A.
// Every other loop keeps what SetupFlashLight sets for its previous floor. Per loop: dark for f000 to f060 (f060 keeps its
// pickup), lit for the f050 passes to f100, lit into f110 until maze A, dark on f120 (after maze A), lit on f160.
bool Game::BrowseLightPickedUp(std::string_view floor) const {
    const int index = floor_.IndexOf(floor);
    return index > floor_.IndexOf("f060") && index <= floor_.IndexOf("f110");
}

// A browsed loop leaves the start room through its exit door: the player is put in that door's trap box (the start room trap whose
// ShDemoExec has nextFloorDemo), facing the door, so the trap itself plays the door demo and calls GoNextFloor in the next trap update,
// as when the player walks in (TrapSystem::ExecDemo: gc_p00_160 on f000, gc_p00_010 on later floors, no save). Playing the demo here
// instead would let the trap fire again when the demo carries the player through the box: its gc_p00_010 skips every running demo
// and a second GoNextFloor could advance two floors
void Game::BrowseEnterHallway() {
    if (!BrowseEntryPending() || browse_door_placed_) return;
    // OnPreGame's SetupFlashLight (and any other post of the start) is read on the start room's floor first (see above); a
    // queue that never empties gives up after a second
    if (messages_.Pending() && ++browse_door_wait_ < 60) return;
    browse_door_placed_ = true;
    const auto& loop = kBrowseLoops[browse_loop_];
    if (BrowseLightPickedUp(loop.floor) && !player_.handy_light.enable) {
        // the f060 pickup (gc_p00_030's functor 0x36A45BC5DA0B, the handy light's SetEnable(true)) was played earlier in this sitting
        player_.handy_light.enable = true;
        LogInfo("loop browser: {} is after the f060 flashlight pickup, handy light on", loop.floor);
    }
    LogInfo("loop browser: start room floor {}, handy light {}", floor_.CurrentFloorName(), player_.handy_light.enable ? "on" : "off");
    Stage* room = stages_.Find("current");
    if (!room) {
        LogWarn("loop browser: no start room, door left to the player");
        return;
    }
    for (const auto& file : room->files) {
        const fox2::DataSetFile& f = *file->file;
        for (const TrapPlacement& trap : file->traps) {
            if (trap.boxes.empty()) continue;
            for (const TrapCondition& condition : trap.conditions) {
                for (const TrapCallback& exec : condition.execs) {
                    if (!exec.entity || exec.class_name != "ShTrapExecRelativeStageDemoPlayCallbackDataElement" ||
                        !f.GetBool(*exec.entity, "nextFloorDemo")) {
                        continue;
                    }
                    const glm::vec3 box(room->ToWorld(trap.boxes.front())[3]);
                    const glm::vec3 feet = player_.Feet();
                    const glm::vec3 to_door(box.x - feet.x, 0.0f, box.z - feet.z);
                    const float fox_yaw = glm::length(to_door) > 1e-3f ? std::atan2(to_door.x, to_door.z) : player_.BodyFoxYaw();
                    // the browsed loop starts as a respawn in the start room (the stand-up), and the player walks out
                    // through the door himself; the auto-walk warp below breaks the trap demo, so tester builds keep it gated
                    if (!std::getenv("PT_BROWSE_DOOR")) {
                        player_.Warp(feet, fox_yaw);
                        LogInfo("loop browser: walk through the start room door");
                        return;
                    }
                    // the player-side edge of the trap box, so the player walks/falls into the box rather than spawning
                    // inside its geometry
                    const glm::vec3 edge =
                        glm::length(to_door) > 1e-3f ? box - (to_door / glm::length(to_door)) * 0.4f : box;
                    player_.Warp(glm::vec3(edge.x, player_.controller.position.y, edge.z), fox_yaw);
                    // the walk to the door crosses trap_activateNextStage first; the warp may not
                    if (!stages_.IsActive("next")) {
                        stages_.RequestActivate("next");
                    }
                    LogInfo("loop browser: player put at the start room door trap {} edge at ({:.2f} {:.2f} {:.2f})", trap.name,
                            edge.x, player_.controller.position.y, edge.z);
                    return;
                }
            }
        }
    }
    LogWarn("loop browser: start room has no exit door trap, door left to the player");
}

void Game::RequestSave() {
    if (browse_save_suppressed_) {
        LogInfo("loop browser: save request suppressed until basement progression");
        return;
    }
    last_save_request_ = time_;
    SaveFile file;
    file.options = options_;
    file.progress.floor = floor_.SaveFloorName();
    for (int i = 0; i < NazoManager::kCount; ++i) {
        file.progress.cleared[i] = nazo_.IsCleared(static_cast<NazoId>(i)) ? 1 : 0;
    }
    file.progress.photo_word = nazo_.PhotoWord();
    file.progress.game_plus = game_plus_;
    file.progress.finishes = finishes_;
    saved_progress_ = file.progress;
    ++save_io_count_;
    save_io_loading_ = false;
    if (!save_store_.Enabled()) {
        LogInfo("save: skipped (floor {})", file.progress.floor);
        return;
    }
    OnSaveWritten(save_store_.Save(file));
}

// SaveRequest_Update (0x9476D0) state 10 on a failed write: the job's result 9 (no space) opens the system's dialog the first time
// (states 0xD, 0xE) and then writes again (state 8, SaveAll); any other result, or a second failure, shows sys_save_failed_4
// (states 0xF, 0x10) and the request ends without a save.
void Game::OnSaveWritten(bool ok) {
    if (ok) {
        save_retries_ = 0;
        return;
    }
    if (save_dialog_) {
        return;
    }
    if (save_store_.LastWrite() == SaveWriteStatus::NoSpace && save_retries_ < 1) {
        ++save_retries_;
        save_dialog_ = SaveDialog{"pc_save_no_space", SaveDialog::Then::Retry};
    } else {
        save_retries_ = 0;
        save_dialog_ = SaveDialog{"sys_save_failed_4", SaveDialog::Then::Nothing};
    }
    LogInfo("save: dialog {} shown", save_dialog_->key);
}

void Game::CloseSaveDialog() {
    if (!save_dialog_) {
        return;
    }
    const SaveDialog dialog = *save_dialog_;
    save_dialog_.reset();
    LogInfo("save: dialog {} closed", dialog.key);
    if (dialog.then != SaveDialog::Then::Nothing) {
        // states 0xE and 0x14 go on to state 8, SaveAll: a full save of the state of the moment
        RequestSave();
    }
}

// The original's pause menu saves only its options block (gameplay.md 3.1, block 3; 3.3). A full save here wrote the
// puzzle states of the moment next to the last checkpoint floor, so a state reached mid transition (a peephole marked
// cleared before its exit message) was kept by every later menu close and survived a restart.
void Game::RequestOptionsSave() {
    if (!saved_progress_) {
        // first boot: nothing saved or loaded yet, so the progress written is the fresh one
        RequestSave();
        return;
    }
    last_save_request_ = time_;
    SaveFile file;
    file.options = options_;
    file.progress = *saved_progress_;
    ++save_io_count_;
    save_io_loading_ = false;
    if (!save_store_.Enabled()) {
        LogInfo("save: options skipped");
        return;
    }
    LogInfo("save: options only (floor {} kept)", file.progress.floor);
    OnSaveWritten(save_store_.Save(file));
}

void Game::RequestLoad() {
    ++save_io_count_;
    save_io_loading_ = true;
    const SaveLoadResult result = save_store_.LoadDetailed();
    const std::optional<SaveFile>& file = result.file;
    if (file) {
        options_ = file->options;
        game_plus_ = file->progress.game_plus;
        finishes_ = file->progress.finishes;
        // 0x9280E0 sets only the floor index (0x923B40); g_SaveFloorName keeps its value (f000 at boot) until a GoNextFloor or Endf120
        floor_.SetFloorLevel(file->progress.floor);
        // A resume on f050 starts without the flashlight, as in the original: OnPreGame's SetupFlashLight
        // (gameControllFlashLight.lua) lights it only on f060, f070, f080, f090, f120 and f160 (reference_captures.md,
        // floor_f050b: the capture booted from sv_x_f050 walks f050 dark).
        for (int i = 0; i < NazoManager::kCount; ++i) {
            const NazoId id = static_cast<NazoId>(i);
            if (file->progress.cleared[i] == 1) {
                nazo_.ForceClear(id);
            } else {
                nazo_.ResetState(id);
            }
        }
        nazo_.SetPhotoWord(file->progress.photo_word);
        nazo_.CancelPendingClearSound();
        saved_progress_ = file->progress;
        controller_.first_boot = false;
        LogInfo("save: loaded floor {}", file->progress.floor);
    } else if (result.status == SaveLoadStatus::Broken) {
        // 0x947C60: result 0xD -> state 0x13: sys_load_failed_3, then a new save over it (state 0x14 -> 8); not a first boot
        controller_.first_boot = false;
        save_dialog_ = SaveDialog{"sys_load_failed_3", SaveDialog::Then::NewSave};
        LogInfo("save: broken save data, dialog {} shown", save_dialog_->key);
    } else if (result.status == SaveLoadStatus::Unreadable) {
        // results 0x13 (newer version), 0x14 (wrong magic) and the rest -> state 0x11: sys_load_failed_2, nothing written
        controller_.first_boot = false;
        save_dialog_ = SaveDialog{"sys_load_failed_2", SaveDialog::Then::Nothing};
        LogInfo("save: unreadable save data, dialog {} shown", save_dialog_->key);
    } else if (result.status == SaveLoadStatus::Old) {
        // a version below 3 is cleared and reported as 0: nothing applied, nothing shown, no first boot
        controller_.first_boot = false;
        LogInfo("save: old save data ignored");
    } else {
        controller_.first_boot = true;
        LogInfo("save: no save data, first boot");
        RequestSave();
    }
    if (!config_.start_floor.empty()) {
        floor_.SetFloorLevel(config_.start_floor);
        controller_.first_boot = false;
    }
    floor_.ResetLoopCount();
}

}
