#include "game/archive_theater.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/core/log.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_renderer.h"
#include "game/archive.h"
#include "game/archive_model_camera.h"
#include "game/game.h"
#include "game/game_sound.h"
#include "game/input_script.h"
#include "game/loop_browser.h"

namespace pt::game {
namespace {

constexpr double kDemoFps = 59.94;
// the ending's narration over black ends and the street fades in (gc_p06_010_final's FadeIn at frame 2415): the teaser starts there
constexpr double kTeaserFrame = 2410.0;
constexpr float kTeaserRate = 30.0f;
// the boot of the theater's session gives up after this long without reaching its picture
constexpr double kBootLimit = 90.0;
// a demo that has not started this long after its play gives up
constexpr double kStartLimit = 15.0;
// the model viewer's spot: this far from the start room's player start toward its door, under the light over the door
constexpr float kSpotAhead = 1.0f;
// a model lower than this stands this high, at a table's height
constexpr float kTableBelow = 0.6f;
constexpr float kTableHeight = 0.85f;
constexpr const char* kTrapDemo = "ShTrapExecRelativeStageDemoPlayCallbackDataElement";

// the walk of the loop browser's previews (main.cpp LoopPreviewRoute): out of the start room through its door, then the hallway
constexpr const char* kWalkRoute =
    "20 sstep 15\n20 swait 5\n20 sfree\n20 goto 0 0 12.5 900\n20 swait 10\n20 sfree\n20 sbrowsed 900\n20 sstep 15\n20 swait 5\n20 sfree\n";

glm::vec3 FoxForward(float yaw) {
    return {std::sin(yaw), 0.0f, std::cos(yaw)};
}

}

ArchiveTheater::ArchiveTheater(Vfs& vfs, ModelCache& models, const ArchiveEntry& entry, const Settings& settings)
    : vfs_(vfs), models_(models), entry_(entry), settings_(settings) {
    if (entry.media == ArchiveMedia::Model) {
        kind_ = Kind::Model;
    } else if (entry.extra == "preface") {
        kind_ = Kind::Preface;
    } else if (entry.extra == "opening") {
        kind_ = Kind::Opening;
    } else if (entry.extra == "room") {
        kind_ = Kind::Room;
    } else if (entry.extra == "kill") {
        kind_ = Kind::Kill;
    } else if (entry.extra == "ending") {
        kind_ = Kind::Ending;
    } else if (entry.extra == "teaser") {
        kind_ = Kind::Teaser;
    } else {
        // a hallway demo; "lit": with the flashlight on, as it is after the f060 pickup
        kind_ = Kind::Hallway;
    }
}

ArchiveTheater::~ArchiveTheater() {
    if (game_) {
        game_->SetAudio(nullptr);
    }
    if (sound_) {
        sound_->Shutdown();
    }
    sound_.reset();
    game_.reset();
    LogInfo("theater: {} closed", entry_.id);
}

bool ArchiveTheater::Start() {
    GameConfig config;
    config.theater = true;
    config.use_save = false;
    config.street_offer = -1;
    config.start_floor = std::string(entry_.floor.empty() ? std::string_view("f010") : entry_.floor);
    if (kind_ == Kind::Hallway || kind_ == Kind::Kill) {
        browse_index_ = BrowseIndexOf(entry_.floor, 1);
        if (browse_index_ <= 0) {
            LogWarn("theater: {} has no loop to enter for floor {}", entry_.id, entry_.floor);
            return false;
        }
        config.start_floor = std::string(kBrowseLoops[browse_index_].previous);
    }
    LogInfo("theater: {} ({}) starts on {}", entry_.id, entry_.asset, config.start_floor);
    game_ = std::make_unique<Game>(vfs_, models_);
    if (!game_->Init(config)) {
        LogError("theater: session start failed for {}", entry_.id);
        return false;
    }
    game_->Controller().first_boot = false;
    game_->Controller().replay_preface = kind_ == Kind::Preface;
    game_->Theater().opening = kind_ == Kind::Opening;
    game_->SetOptionsUiAvailable(false);
    game_->Demos().time_scale = settings_.demo_rate;
    if (settings_.sound) {
        sound_ = std::make_unique<GameSound>(*game_);
        if (sound_->Init(settings_.open_device, "Eng")) {
            sound_->System().SetMasterVolume(settings_.volume);
            game_->SetAudio(sound_.get());
        } else {
            sound_.reset();
        }
    }
    if (kind_ == Kind::Hallway || kind_ == Kind::Kill) {
        script_ = std::make_unique<InputScript>();
        script_->Parse(kWalkRoute);
    }
    return true;
}

void ArchiveTheater::Stop() {
    Finish("left");
}

void ArchiveTheater::Finish(const char* why) {
    if (phase_ == Phase::Done) return;
    phase_ = Phase::Done;
    LogInfo("theater: {} done ({})", entry_.id, why);
}

bool ArchiveTheater::Showing() const {
    switch (kind_) {
    case Kind::Model: return phase_ == Phase::Model;
    case Kind::Preface: return demo_seen_;
    case Kind::Opening: return game_ && game_->Controller().Step() >= 12;
    case Kind::Ending: return demo_seen_;
    case Kind::Teaser: return demo_seen_ && !teaser_fast_;
    default: return phase_ == Phase::Playing;
    }
}

Camera ArchiveTheater::ViewCamera() const {
    if (!model_mode_) {
        return game_->GetCamera();
    }
    Camera camera;
    const glm::vec3 look(-std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), -std::cos(yaw_) * std::cos(pitch_));
    camera.position = target_ - look * distance_;
    camera.yaw = yaw_;
    camera.pitch = pitch_;
    camera.fov_y = glm::radians(40.0f);
    return camera;
}

void ArchiveTheater::CollectDraws(std::vector<DrawItem>& out) {
    if (!model_mode_) {
        game_->CollectDraws(out);
        return;
    }
    if (gimmick_) {
        std::vector<DrawItem> all;
        game_->Objects().CollectDraws(all);
        for (const DrawItem& item : all) {
            if (item.mesh == model_mesh_) out.push_back(item);
        }
        return;
    }
    out.insert(out.end(), model_draws_.begin(), model_draws_.end());
}

void ArchiveTheater::Update(float dt, const InputState& input) {
    if (!game_ || phase_ == Phase::Done) return;
    ++frame_;
    phase_time_ += dt;
    // the player's subtitles in the player's language, as the menu has them now
    game_->Options().subtitles = settings_.options.subtitles;
    game_->Options().subtitle_language = settings_.options.subtitle_language;
    game_->Options().brightness = settings_.options.brightness;
    GameController& controller = game_->Controller();
    if (browse_index_ > 0 && !prepared_ && controller.Step() >= 1) {
        // after the boot's save step, which sets the floor and the puzzles of a new game
        game_->PrepareTheaterLoop(browse_index_);
        prepared_ = true;
    }
    InputState state;
    if (script_) {
        script_->Apply(frame_, *game_, state);
    }
    game_->Update(dt, state);
    const int step = controller.Step();
    const std::string asset(entry_.asset);
    DemoSystem& demos = game_->Demos();
    switch (phase_) {
    case Phase::Boot:
        if (phase_time_ > kBootLimit) {
            Finish("the session never reached its picture");
            return;
        }
        if (kind_ == Kind::Model || kind_ == Kind::Room) {
            // in play, after StartGame's fade in (OnPreGame)
            const ScreenEffects& fx = game_->Effects();
            const bool faded_in = !fx.IsFadeProcessing() && fx.FadeShown().a < 0.004f;
            if (step == 15 && controller.RequestedStep() == 15 && faded_in && ++settle_ticks_ >= 10) {
                if (kind_ == Kind::Model) {
                    StartModel();
                } else {
                    Present();
                }
            }
        } else if (kind_ == Kind::Preface || kind_ == Kind::Opening) {
            if (step >= (kind_ == Kind::Preface ? 8 : 12)) {
                phase_ = Phase::Playing;
                phase_time_ = 0.0;
            }
        } else if (kind_ == Kind::Ending || kind_ == Kind::Teaser) {
            if (step == 15 && controller.RequestedStep() == 15) {
                // as the f160 exit door's demo does at its Finish on the ending floor (demoScriptGotoEnding): the ending loads
                game_->Floor().SetFloorLevel("ending");
                controller.ChangeGameStep("GotoEnding");
                phase_ = Phase::Playing;
                phase_time_ = 0.0;
            }
        } else if (script_ && script_->Idle() && game_->BrowseArrived() && step == 15 && !demos.ControlsPlayer()) {
            // through the door and in the hallway, its entrance traps run (light set, doors, sound)
            if (++settle_ticks_ >= 30) Present();
        }
        break;
    case Phase::Playing:
        if (kind_ == Kind::Preface) {
            demo_seen_ = demo_seen_ || demos.IsPlaying(asset);
            if (step >= 11 && step < 15) Finish("the preface ended");
        } else if (kind_ == Kind::Opening) {
            if (step >= 14) Finish("the opening ended");
        } else if (kind_ == Kind::Ending || kind_ == Kind::Teaser) {
            const bool playing = demos.IsPlaying(asset);
            demo_seen_ = demo_seen_ || playing;
            if (kind_ == Kind::Teaser && playing) {
                const double frame = demos.PlayTime(asset) * kDemoFps;
                if (frame < kTeaserFrame && !teaser_fast_ && frame > 0.0) {
                    // the narration over black runs silent and fast; the street, the reveal and the credits play as they are
                    teaser_fast_ = true;
                    demos.time_scale = kTeaserRate;
                    if (sound_) sound_->System().SetMasterVolume(0.0f);
                } else if (frame >= kTeaserFrame && teaser_fast_) {
                    teaser_fast_ = false;
                    demos.time_scale = settings_.demo_rate;
                    demos.ResyncAudio(asset);
                    if (sound_) sound_->System().SetMasterVolume(settings_.volume);
                }
            }
            if ((demo_seen_ && !playing) || step >= 29) Finish("the ending ended");
            if (!demo_seen_ && phase_time_ > kBootLimit) Finish("the ending never started");
        } else {
            const bool playing = demos.IsPlaying(asset);
            demo_seen_ = demo_seen_ || playing;
            if (aim_) UpdateAim(dt);
            if (demo_seen_ && !playing) Finish("the demo ended");
            if (!demo_seen_ && phase_time_ > kStartLimit) Finish("the demo never started");
        }
        break;
    case Phase::Model:
        UpdateModel(dt, input);
        break;
    default:
        break;
    }
}

// the place the level data plays the demo at: a ShDemoExec trap of the entry's floor (its demo center or its own transform, as
// TrapSystem::ExecDemo sets it) and that trap's box as the player's place; else a script that plays it (demoScriptPlayDemo after
// another demo, or gamePlayDemo on a message) with its demo center, and the place of the demo that leads to it
bool ArchiveTheater::FindPlacement(const std::string& demo, glm::mat4& transform, glm::vec3* viewpoint) {
    bool found = false;
    bool found_on_floor = false;
    game_->Stages().ForEachStage(
        [&](Stage& stage) {
            for (const auto& file : stage.files) {
                const fox2::DataSetFile& f = *file->file;
                for (const TrapPlacement& trap : file->traps) {
                    for (const TrapCondition& condition : trap.conditions) {
                        for (const TrapCallback& exec : condition.execs) {
                            if (found_on_floor || !exec.entity || exec.class_name != kTrapDemo || f.GetString(*exec.entity, "demoId") != demo) {
                                continue;
                            }
                            bool on_floor = !f.GetBool(*exec.entity, "orderFloorDemo");
                            if (const fox2::Property* floors = f.FindProperty(*exec.entity, "floorNameArray")) {
                                for (size_t i = 0; i < floors->Count(); ++i) on_floor = on_floor || f.ElementString(*floors, i) == entry_.floor;
                            }
                            if (found && !on_floor) continue;
                            glm::mat4 origin = trap.entity ? stage.ToWorld(f.WorldTransform(*trap.entity)) : glm::mat4(1.0f);
                            if (f.GetBool(*exec.entity, "orderDemoCenter")) {
                                if (const fox2::Entity* center = f.GetEntity(*exec.entity, "demoCenterLink")) {
                                    origin = stage.ToWorld(f.WorldTransform(*center));
                                }
                            }
                            transform = origin;
                            if (viewpoint && !trap.boxes.empty()) *viewpoint = glm::vec3(stage.ToWorld(trap.boxes.front())[3]);
                            found = true;
                            found_on_floor = on_floor;
                        }
                    }
                }
            }
        },
        false);
    if (found) return true;
    std::string leading;
    game_->Stages().ForEachStage(
        [&](Stage& stage) {
            for (const auto& file : stage.files) {
                const fox2::DataSetFile& f = *file->file;
                for (const MessageScript& script : file->message_scripts) {
                    if (found || !script.entity) continue;
                    const bool chained = f.GetString(*script.entity, "demoIdNext") == demo;
                    const bool posted = !chained && f.GetString(*script.entity, "demoId") == demo && script.class_name == "ShGameControllerMessageScript";
                    if (!chained && !posted) continue;
                    const fox2::Entity* center = f.GetEntity(*script.entity, "demoCenterLink");
                    if (!center) {
                        if (const fox2::Entity* params = f.GetEntity(*script.entity, "parameters")) center = f.GetEntity(*params, "demoCenterLink");
                    }
                    if (!center) continue;
                    transform = stage.ToWorld(f.WorldTransform(*center));
                    if (chained) leading = script.demo_id;
                    found = true;
                }
            }
        },
        false);
    if (found && viewpoint && !leading.empty() && leading != demo) {
        glm::mat4 unused(1.0f);
        FindPlacement(leading, unused, viewpoint);
    }
    return found;
}

// the middle of what the demo moves (its models, or its place before they appear), eased so a swinging door turns the view slowly
void ArchiveTheater::UpdateAim(float dt) {
    glm::vec3 sum(0.0f);
    int count = 0;
    glm::vec3 origin = look_target_;
    for (const PlayingDemo& demo : game_->Demos().Playing()) {
        if (demo.demo_id != entry_.asset) continue;
        origin = demo.translation;
        for (const DemoModel& model : demo.models) {
            if (!model.drawn || !model.visible || !model.active) continue;
            sum += glm::vec3(model.world[3]);
            ++count;
        }
    }
    if (count == 0) {
        if (!aim_started_) look_target_ = origin;
    } else if (!aim_started_) {
        look_target_ = sum / static_cast<float>(count);
        aim_started_ = true;
    } else {
        look_target_ += (sum / static_cast<float>(count) - look_target_) * std::min(1.0f, dt * 3.0f);
    }
    // the session's own camera, so its handy light and its view turn together; a demo camera, when one shows, takes over
    Camera demo_camera;
    if (game_->Demos().CameraOverride(demo_camera)) {
        game_->SetCameraOverride(std::nullopt);
        return;
    }
    Camera camera = game_->GetPlayer().MakeCamera();
    const glm::vec3 to = look_target_ - camera.position;
    if (glm::length(to) > 0.1f) {
        const glm::vec3 d = glm::normalize(to);
        camera.yaw = std::atan2(-d.x, -d.z);
        camera.pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
        camera.roll = 0.0f;
    }
    game_->SetCameraOverride(camera);
}

void ArchiveTheater::Present() {
    game_->Theater().traps = false;
    game_->Theater().ocho = false;
    const std::string demo(entry_.asset);
    DemoSystem& demos = game_->Demos();
    Player& player = game_->GetPlayer();
    if (kind_ == Kind::Kill) {
        // Lisa's kill stands at the player as OchoLogic::Kill puts it
        const float yaw = player.BodyFoxYaw();
        demos.SetDemoTransform(demo, glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)), player.Feet());
    } else {
        if (kind_ == Kind::Room && !game_->Stages().IsActive("next")) {
            // the hallway behind the start room's door, which the door's trap shows as the player walks up to it
            game_->Stages().RequestActivate("next");
        }
        if (entry_.extra == "lit") {
            // after the f060 pickup (gc_p00_030) in one sitting the flashlight is on
            player.handy_light.enable = true;
        }
        glm::mat4 transform(1.0f);
        glm::vec3 viewpoint(0.0f);
        bool has_view = false;
        glm::vec3* view = kind_ == Kind::Room ? nullptr : &viewpoint;
        viewpoint = glm::vec3(std::nanf(""));
        if (FindPlacement(demo, transform, view)) {
            demos.SetDemoTransform(demo, glm::quat_cast(glm::mat3(transform)), glm::vec3(transform[3]));
            has_view = view && !std::isnan(viewpoint.x);
            LogInfo("theater: {} placed at ({:.2f} {:.2f} {:.2f})", demo, transform[3].x, transform[3].y, transform[3].z);
        } else {
            LogWarn("theater: {} has no placement in the level data, playing at origin", demo);
        }
        aim_ = kind_ == Kind::Hallway;
        if (has_view) {
            // a demo without a camera of its own is seen from where its trap is walked into, looking at it
            const glm::vec3 feet(viewpoint.x, player.Feet().y, viewpoint.z);
            const glm::vec3 to(transform[3].x - feet.x, 0.0f, transform[3].z - feet.z);
            const float yaw = glm::length(to) > 0.2f ? std::atan2(to.x, to.z) : player.BodyFoxYaw();
            player.Warp(feet, yaw);
            LogInfo("theater: viewer at ({:.2f} {:.2f} {:.2f})", feet.x, feet.y, feet.z);
        }
    }
    demos.Play(demo);
    phase_ = Phase::Playing;
    phase_time_ = 0.0;
}

// the model at the start room's player start, facing where the player faces, in the room's light; the camera turns around it
void ArchiveTheater::StartModel() {
    game_->Theater().traps = false;
    game_->Theater().ocho = false;
    model_mode_ = true;
    const Player& player = game_->GetPlayer();
    // a metre from the player start toward the door, under the light over the door
    spot_yaw_ = player.BodyFoxYaw();
    spot_ = player.Feet() + FoxForward(spot_yaw_) * kSpotAhead;
    const glm::mat4 place = glm::translate(glm::mat4(1.0f), spot_) * glm::mat4_cast(glm::angleAxis(spot_yaw_, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 lo(0.0f);
    glm::vec3 hi(1.0f);
    for (int t = 0; t < static_cast<int>(GimmickType::Count); ++t) {
        if (GimmickName(static_cast<GimmickType>(t)) == entry_.asset) gimmick_type_ = t;
    }
    if (gimmick_type_ >= 0) {
        gimmick_ = true;
        const GimmickType type = static_cast<GimmickType>(gimmick_type_);
        GameObjects& objects = game_->Objects();
        Gimmick& g = objects.GetGimmick(type);
        if (g.mesh) {
            lo = g.mesh->bounds_min;
            hi = g.mesh->bounds_max;
        }
        // standing on the floor at the spot, whatever the model's own origin (the fridge and the lamp hang from their tops); Lisa
        // stands on her own origin, which her bind pose's bounds do not show
        if (type != GimmickType::Ocho) {
            const glm::vec3 center = (lo + hi) * 0.5f;
            model_offset_ = glm::translate(glm::mat4(1.0f), glm::vec3(-center.x, -lo.y, -center.z));
            hi -= glm::vec3(center.x, lo.y, center.z);
            lo -= glm::vec3(center.x, lo.y, center.z);
        }
        g.world = place * model_offset_;
        g.enabled = g.shown = g.placed = g.active = true;
        g.hidden_views = 0;
        g.hidden_meshes = g.parts_hidden_meshes;
        if (type == GimmickType::Ocho) objects.SetOchoTransform(g.world);
        objects.PlayGimmickMotion(type, entry_.extra, false);
        model_mesh_ = g.mesh;
        LogInfo("theater: gimmick {} in motion {} at ({:.2f} {:.2f} {:.2f})", g.name, entry_.extra, spot_.x, spot_.y, spot_.z);
    } else if (const ModelEntry* model = models_.Get(std::string(entry_.asset))) {
        lo = model->mesh->bounds_min;
        hi = model->mesh->bounds_max;
        // standing on the floor at the spot, whatever the file's own origin
        const glm::vec3 center = (lo + hi) * 0.5f;
        DrawItem item;
        item.mesh = model->mesh.get();
        item.transform = place * glm::translate(glm::mat4(1.0f), glm::vec3(-center.x, -lo.y, -center.z));
        model_draws_.push_back(item);
        model_mesh_ = item.mesh;
        hi -= glm::vec3(center.x, lo.y, center.z);
        lo -= glm::vec3(center.x, lo.y, center.z);
        LogInfo("theater: model {} at ({:.2f} {:.2f} {:.2f}), {:.2f} m high", entry_.asset, spot_.x, spot_.y, spot_.z, hi.y - lo.y);
    } else {
        LogWarn("theater: model {} not found", entry_.asset);
    }
    const glm::vec3 extent = glm::max(hi - lo, glm::vec3(0.02f));
    const float size = std::max({extent.x, extent.y, extent.z});
    if (extent.y < kTableBelow) {
        // a small thing stands at a table's height, as the radio and the phone do in the hallway, nearer the room's light
        const glm::vec3 lift(0.0f, kTableHeight, 0.0f);
        for (DrawItem& item : model_draws_) item.transform = glm::translate(glm::mat4(1.0f), lift) * item.transform;
        if (gimmick_) model_offset_ = glm::translate(glm::mat4(1.0f), lift) * model_offset_;
        lo += lift;
        hi += lift;
        if (gimmick_) {
            Gimmick& g = game_->Objects().GetGimmick(static_cast<GimmickType>(gimmick_type_));
            g.world = place * model_offset_;
        }
    }
    target_ = spot_ + glm::vec3(0.0f, std::max(lo.y, 0.0f) + extent.y * 0.5f, 0.0f);
    model_home_ = target_;
    model_radius_ = size * 0.5f;
    distance_ = std::clamp(size * 1.9f + 0.05f, 0.1f, 6.0f);
    min_distance_ = std::max(0.08f, size * 0.6f);
    // the model alone on black: the floor's auto exposure would meter the black, so the exposure is held where the room's
    // lit surfaces read as in play (Settings::model_ev; the thumbnail capture holds it higher)
    game_->Effects().ev_pinned = true;
    game_->Effects().pinned_ev = settings_.model_ev;
    max_distance_ = std::max(distance_ * 2.0f, 1.5f);
    // in front of the model: looking back along its facing
    const glm::vec3 facing = FoxForward(spot_yaw_);
    yaw_ = std::atan2(facing.x, facing.z);
    pitch_ = -0.12f;
    phase_ = Phase::Model;
}

void ArchiveTheater::UpdateModel(float dt, const InputState& input) {
    if (gimmick_) {
        // the motion again from its start at its end (OchoDash and BagTalk hold their last frame in play), the model at the spot
        const GimmickType type = static_cast<GimmickType>(gimmick_type_);
        Gimmick& g = game_->Objects().GetGimmick(type);
        const float length = game_->Demos().Gimmicks().MotionSeconds(g.motion);
        if (length > 0.0f && g.motion_time >= length) game_->Objects().PlayGimmickMotion(type, entry_.extra, false);
        const glm::mat4 place = glm::translate(glm::mat4(1.0f), spot_) * glm::mat4_cast(glm::angleAxis(spot_yaw_, glm::vec3(0.0f, 1.0f, 0.0f)));
        g.world = place * model_offset_;
        if (type == GimmickType::Ocho) game_->Objects().SetOchoTransform(g.world);
    }
    // the left stick (WASD) pans; the right stick and D-pad (arrows) turn; the triggers zoom
    float turn = input.right_stick.x;
    float tilt = input.right_stick.y;
    if (input.raw_held & kRawLeft) turn -= 1.0f;
    if (input.raw_held & kRawRight) turn += 1.0f;
    if (input.raw_held & kRawUp) tilt += 1.0f;
    if (input.raw_held & kRawDown) tilt -= 1.0f;
    float zoom = 0.0f;
    if (input.raw_held & kRawR2) zoom -= 1.0f;
    if (input.raw_held & kRawL2) zoom += 1.0f;
    yaw_ -= turn * 1.6f * dt;
    pitch_ = std::clamp(pitch_ - tilt * 1.2f * dt, -1.2f, 1.2f);
    distance_ = std::clamp(distance_ * (1.0f + zoom * 1.2f * dt), min_distance_, max_distance_);
    const Camera camera = ViewCamera();
    target_ = PanArchiveModel(target_, model_home_, camera.Right(), camera.Up(), input.left_stick, dt, distance_, model_radius_);
}

}
