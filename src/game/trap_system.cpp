#include "game/trap_system.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <vector>

#include "engine/core/log.h"
#include "game/game.h"

namespace pt::game {
namespace {

constexpr float kPi = 3.14159265f;

float Wrap(float a) {
    const float t = a + kPi;
    return t >= 0.0f ? std::fmod(t, 2.0f * kPi) - kPi : kPi - std::fmod(-t, 2.0f * kPi);
}

float FoxYawOf(const glm::vec3& v) {
    if (std::abs(v.x) + std::abs(v.z) < 1e-6f) {
        return 0.0f;
    }
    return std::atan2(v.x, v.z);
}

}

const char* TrapFlagString(TrapFlag flag) {
    switch (flag) {
    case TrapFlag::Enter: return "GEO_TRAP_S_ENTER";
    case TrapFlag::Inside: return "GEO_TRAP_S_INSIDE";
    case TrapFlag::Out: return "GEO_TRAP_S_OUT";
    default: return "GEO_TRAP_S_NONE";
    }
}

void TrapSystem::ForgetStage(const Stage& stage) {
    std::erase_if(traps_, [&](const auto& item) { return item.first.stage_id == stage.id; });
    std::erase_if(conditions_, [&](const auto& item) { return item.first.stage_id == stage.id; });
    std::erase_if(elements_, [&](const auto& item) { return item.first.stage_id == stage.id; });
}

void TrapSystem::DumpTraps(const Stage& stage) const {
    for (const auto& file : stage.files) {
        for (const TrapPlacement& trap : file->traps) {
            for (const glm::mat4& box : trap.boxes) {
                const glm::mat4 world = stage.ToWorld(box);
                LogInfo("trap box: {} {} at ({:.2f} {:.2f} {:.2f}) size ({:.2f} {:.2f} {:.2f}) {}", stage.label, trap.name, world[3][0], world[3][1],
                        world[3][2], glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])),
                        trap.enable ? "" : "(disabled)");
            }
        }
    }
}

bool TrapSystem::PlayerInside(const Stage& stage, const TrapPlacement& trap) const {
    // 0xBEEC20: a point query at the trap object
    const glm::vec3 point = game_.GetPlayer().TrapPoint();
    for (const glm::mat4& box : trap.boxes) {
        const glm::vec3 local(glm::inverse(stage.ToWorld(box)) * glm::vec4(point, 1.0f));
        if (std::abs(local.x) <= 0.5f && std::abs(local.y) <= 0.5f && std::abs(local.z) <= 0.5f) {
            return true;
        }
    }
    return false;
}

float TrapSystem::TrapYaw(const Context& ctx) const {
    const glm::mat4 world = ctx.stage->ToWorld(ctx.file->file->WorldTransform(*ctx.trap->entity));
    return FoxYawOf(glm::vec3(world[2]));
}

bool TrapSystem::ButtonPressed(const std::string& button) const {
    const uint32_t pressed = game_.GetPlayer().FramePressedButtons();
    if (button == "Zoom") {
        return (pressed & kPadZoom) != 0;
    }
    if (button == "Action") {
        return (pressed & kPadAction) != 0;
    }
    return true;
}

// the XMark's trap reads `Action`, the cross bit (0x917AE0) that is also the interact button: the gouge is the action button
// (E, Enter, Space, left mouse, cross) or the dedicated X (the X key, the pad button that carries the X label)
bool TrapSystem::GougePressed() const {
    return (game_.GetPlayer().FramePressedButtons() & (kPadGouge | kPadAction)) != 0;
}

bool TrapSystem::TargetInView(const Context& ctx, const fox2::Entity* locator, float area, glm::vec3* position) const {
    if (!locator) {
        return false;
    }
    const glm::vec3 target(ctx.stage->ToWorld(ctx.file->file->WorldTransform(*locator))[3]);
    if (position) {
        *position = target;
    }
    return game_.InViewNdc(target, area);
}

bool TrapSystem::Check(const Context& ctx, const TrapCallback& callback) {
    const fox2::DataSetFile& f = *ctx.file->file;
    const fox2::Entity& e = *callback.entity;
    const Player& player = game_.GetPlayer();
    if (callback.class_name == "ShTrapCheckIsPlayerCallbackDataElement") {
        return true;
    }
    if (callback.class_name == "ShTrapCheckPlayerInputDirectionCallbackDataElement") {
        float ref_yaw = TrapYaw(ctx);
        if (const fox2::Entity* target = f.GetEntity(e, "targetLink")) {
            ref_yaw = FoxYawOf(glm::vec3(ctx.stage->ToWorld(f.WorldTransform(*target))[2]));
        }
        const float limit = glm::radians(f.GetFloat(e, "degreeLimit", 0, 45.0f));
        bool dir_ok = true;
        if (f.GetBool(e, "isCheckPlayerDir")) {
            // 0x915850 reads the published body rotation (+0x40, the previous frame's), not the camera yaw
            dir_ok = std::abs(Wrap(player.PublishedBodyFoxYaw() - ref_yaw)) <= limit;
        }
        bool button_ok = true;
        if (f.GetBool(e, "isCheckButton", 0, true)) {
            const std::string input = f.GetString(e, "input");
            if (input == "Zoom") {
                button_ok = (player.FramePressedButtons() & kPadZoom) != 0;
            } else if (input == "LStick") {
                if (player.LeftStickLocked() || player.StickMagnitude() <= 0.0f) {
                    button_ok = false;
                } else {
                    button_ok = std::abs(Wrap(player.StickHeading() - ref_yaw)) <= limit;
                }
            }
        }
        return button_ok && dir_ok;
    }
    if (callback.class_name == "ShTrapCheckIsInViewCallbackDataElement") {
        const fox2::Entity* locator = f.GetEntity(e, "checkLocator");
        const float area = f.GetFloat(e, "checkAreaSize", 0, 0.4f);
        const std::string button = f.GetString(e, "button");
        bool gouge = false;
        for(const auto& exec : ctx.condition->execs) {
            if(exec.entity && exec.class_name == "ShTrapExecNazoCallbackDataElement" && f.GetString(*exec.entity,"checkName") == "XMark")gouge=true;
        }
        auto in_view = [&]() { return TargetInView(ctx, locator, area, nullptr) &&
            (gouge && button == "Action" ? GougePressed() : ButtonPressed(button)); };
        if (!f.GetBool(e, "isOutOfViewAfter")) {
            return in_view();
        }
        ElementState& state = elements_[{ctx.stage->id, &e}];
        const bool now = in_view();
        if (!state.was_in_view) {
            if (now) {
                state.was_in_view = true;
            }
            return false;
        }
        if (!now) {
            state.was_in_view = false;
            return true;
        }
        return false;
    }
    LogDebug("trap: unknown check {}", callback.class_name);
    return true;
}

bool TrapSystem::RunChecks(const Context& ctx) {
    for (const TrapCallback& check : ctx.condition->checks) {
        if (check.entity && !Check(ctx, check)) {
            return false;
        }
    }
    return true;
}

int TrapSystem::ExecDemo(const Context& ctx, const fox2::Entity& e) {
    if (ctx.flag != TrapFlag::Enter) {
        return 0;
    }
    const fox2::DataSetFile& f = *ctx.file->file;
    const std::string demo_id = f.GetString(e, "demoId");
    if (f.GetBool(e, "orderFloorDemo")) {
        bool match = false;
        const fox2::Property* floors = f.FindProperty(e, "floorNameArray");
        for (size_t i = 0; floors && i < floors->Count(); ++i) {
            match = match || game_.Floor().IsCurrentFloorName(f.ElementString(*floors, i));
        }
        if (!match) {
            if (debug_log) {
                LogInfo("trap: demo {} not on floor {}", demo_id, game_.Floor().CurrentFloorName());
            }
            return 0;
        }
    }
    if (demo_id == "gc_p00_010") {
        game_.Demos().Skip();
    }
    if (!game_.Demos().Find(demo_id)) {
        return 0;
    }
    glm::mat4 origin(1.0f);
    if (f.GetBool(e, "orderDemoCenter")) {
        if (const fox2::Entity* center = f.GetEntity(e, "demoCenterLink")) {
            origin = ctx.stage->ToWorld(f.WorldTransform(*center));
        }
    } else {
        origin = ctx.stage->ToWorld(f.WorldTransform(*ctx.trap->entity));
    }
    game_.Demos().SetDemoTransform(demo_id, glm::quat_cast(glm::mat3(origin)), glm::vec3(origin[3]));
    if (game_.Demos().IsPlaying(demo_id)) {
        return 1;
    }
    LogInfo("trap: ShDemoExec {} (next floor {}, save {})", demo_id, f.GetBool(e, "nextFloorDemo"), f.GetBool(e, "needSave"));
    game_.Demos().Play(demo_id);
    if (f.GetBool(e, "loopDemo")) {
        game_.Demos().ToggleLoop(demo_id);
    }
    game_.OnTrapDemo(demo_id, origin, f.GetBool(e, "loopDemo"));
    if (f.GetBool(e, "nextFloorDemo")) {
        if (f.GetBool(e, "needSave")) game_.OnBasementExit();
        game_.Floor().GoNextFloor(f.GetBool(e, "needSave"));
    }
    return 1;
}

int TrapSystem::ExecDoor(const Context& ctx, const fox2::Entity& e) {
    const fox2::DataSetFile& f = *ctx.file->file;
    const Player& player = game_.GetPlayer();
    const fox2::Entity* model = f.GetEntity(e, "checkLocator");
    const bool target_visible = model && model->class_name == "StaticModel" && ctx.stage->Body(model).visible;
    const float trap_yaw = TrapYaw(ctx);
    ElementState& state = elements_[{ctx.stage->id, &e}];
    if (f.GetBool(e, "isSound")) {
        if (ctx.flag == TrapFlag::Enter) {
            state.timer = 0.0f;
            state.done = false;
            return 0;
        }
        if (ctx.flag != TrapFlag::Inside) {
            return 0;
        }
        if (state.done) {
            return 1;
        }
        const bool pushing = !player.LeftStickLocked() && player.StickMagnitude() > 0.2f &&
                             std::abs(Wrap(player.StickHeading() - trap_yaw)) < glm::radians(45.0f);
        if (!pushing) {
            state.timer = 0.0f;
            return 0;
        }
        state.timer += ctx.dt;
        if (state.timer > 0.15f && (target_visible || f.GetBool(e, "isNoTarget"))) {
            game_.PostSound(f.GetString(e, "soundId"), player.Feet(), true);
            state.done = true;
            return 1;
        }
        return 0;
    }
    if (f.GetBool(e, "isBigZoom")) {
        if (ctx.flag == TrapFlag::Out) {
            big_zoom = false;
            return 1;
        }
        if (ctx.flag == TrapFlag::Inside) {
            big_zoom = target_visible && std::abs(Wrap(player.CameraFoxYaw() - trap_yaw)) < glm::radians(15.0f);
            return 1;
        }
    }
    return 0;
}

int TrapSystem::ExecNazo(const Context& ctx, const fox2::Entity& e) {
    if (ctx.flag == TrapFlag::Enter) {
        return 0;
    }
    const fox2::DataSetFile& f = *ctx.file->file;
    const fox2::Entity* locator = f.GetEntity(e, "checkLocator");
    const float area = f.GetFloat(e, "checkAreaSize", 0, 0.4f);
    glm::vec3 position(0.0f);
    if (!f.GetBool(e, "isOutOfViewAfter")) {
        const bool gouge = f.GetString(e, "checkName") == "XMark";
        if (!(gouge ? GougePressed() : ButtonPressed(f.GetString(e, "button")))) {
            return 0;
        }
        if (!TargetInView(ctx, locator, area, &position)) {
            return 0;
        }
    } else {
        ElementState& state = elements_[{ctx.stage->id, &e}];
        const bool now = TargetInView(ctx, locator, area, &position);
        if (debug_log && now != state.last_logged) {
            state.last_logged = now;
            const glm::vec3 eye = game_.GetCamera().position;
            const Camera view = game_.GetPlayer().MakeCamera();
            LogInfo("trap: {} target {} view (flag {}) feet ({:.2f} {:.2f} {:.2f}) eye ({:.2f} {:.2f} {:.2f}) target ({:.2f} {:.2f} {:.2f}) depth {:.2f} (game camera forward ({:.2f} {:.2f} {:.2f}), player camera ({:.2f} {:.2f} {:.2f}))",
                    f.GetString(e, "checkName"), now ? "enters" : "leaves", TrapFlagString(ctx.flag), game_.GetPlayer().Feet().x,
                    game_.GetPlayer().Feet().y, game_.GetPlayer().Feet().z, eye.x, eye.y, eye.z, position.x, position.y, position.z,
                    glm::dot(position - view.position, view.Forward()), game_.GetCamera().Forward().x, game_.GetCamera().Forward().y,
                    game_.GetCamera().Forward().z, view.position.x, view.position.y, view.position.z);
        }
        if (!state.was_in_view) {
            if (now) {
                state.was_in_view = true;
            }
            return 0;
        }
        if (now) {
            return 0;
        }
        state.was_in_view = false;
    }
    game_.Nazo().SetCondition(f.GetString(e, "checkName"), position);
    return 1;
}

int TrapSystem::RunExecs(const Context& ctx) {
    int result = 0;
    const uint32_t stage_id = ctx.stage->id;
    for (const TrapCallback& exec : ctx.condition->execs) {
        if (!exec.entity || !game_.Stages().FindById(stage_id)) {
            break;
        }
        const fox2::DataSetFile& f = *ctx.file->file;
        int r = 0;
        if (exec.class_name == "GeoTrapScriptCallbackDataElement") {
            if (ctx.flag != TrapFlag::Inside) {
                r = game_.Scripts().CallTrapExec(f.GetString(*exec.entity, "scriptFile"), *ctx.stage, *ctx.trap->entity, *ctx.condition->entity,
                                                 ctx.flag);
            }
        } else if (exec.class_name == "ShTrapExecRelativeStageDemoPlayCallbackDataElement") {
            r = ExecDemo(ctx, *exec.entity);
        } else if (exec.class_name == "ShTrapExecDoorCallbackDataElement") {
            r = ExecDoor(ctx, *exec.entity);
        } else if (exec.class_name == "ShTrapExecNazoCallbackDataElement") {
            r = ExecNazo(ctx, *exec.entity);
        } else if (exec.class_name == "ShTrapExecMirrorSwitchCallbackDataElement") {
            const int bit = f.GetInt(*exec.entity, "viewportFlag", 0, 1);
            if (ctx.flag == TrapFlag::Enter) {
                game_.SetMirrorViewportBit(bit, true);
            } else if (ctx.flag == TrapFlag::Out) {
                game_.SetMirrorViewportBit(bit, false);
            }
            r = 1;
        } else if (ctx.flag == TrapFlag::Enter) {
            LogWarn("trap: unknown exec {}", exec.class_name);
        }
        result = std::max(result, r);
    }
    return result;
}

void TrapSystem::Update(float dt) {
    std::vector<uint32_t> stage_ids;
    game_.Stages().ForEachStage([&](Stage& stage) {
        if (stage.active) {
            stage_ids.push_back(stage.id);
        }
    });
    // 0xBEF490 gathers the hits of every trap the object is in, 0xBEEC20 sorts them into the OUT, ENTER and INSIDE lists, and
    // only then 0xBEF910 runs the execs: all OUT, then all ENTER, then all INSIDE. So an exec that enables another trap's
    // condition takes effect in the next frame, and leaving one box while entering another of the same kind (two MirrorSwitch
    // boxes of one flag) clears before it sets.
    /* All OUT execs run first, then ENTER, then INSIDE, after every hit is gathered (0xBEF910): an exec that enables another trap only takes effect next frame. */
    struct Pending {
        uint32_t stage_id;
        Context ctx;
    };
    std::vector<Pending> out;
    std::vector<Pending> enter;
    std::vector<Pending> in;
    for (uint32_t stage_id : stage_ids) {
        Stage* stage = game_.Stages().FindById(stage_id);
        for (size_t fi = 0; stage && fi < stage->files.size(); ++fi) {
            for (size_t ti = 0; ti < stage->files[fi]->traps.size(); ++ti) {
                const StageData& file = *stage->files[fi];
                const TrapPlacement& trap = file.traps[ti];
                const bool inside = stage->Body(trap.entity).enable && PlayerInside(*stage, trap);
                bool& was_inside = traps_[{stage_id, trap.entity}];
                if (inside != was_inside && debug_log) {
                    LogInfo("trap: {} {}", inside ? "enter" : "leave", trap.name);
                }
                was_inside = inside;
                // --trap-log: follow the MirrorSwitch traps (the ones that drive the mirror capture) and the viewport bits
                // they set. The player's point is the controller sphere's centre 0xB010B0 tests them with.
                if (debug_log && trap.name.find("Mirror") != std::string::npos) {
                    const glm::vec3 p = game_.GetPlayer().TrapPoint();
                    const glm::vec3 feet = game_.GetPlayer().Feet();
                    static glm::vec3 last_p(0.0f);
                    static bool last_inside = false;
                    if (inside != last_inside || glm::length(p - last_p) > 0.05f) {
                        last_p = p;
                        last_inside = inside;
                        LogInfo("trap: {} inside {} point ({:.3f} {:.3f} {:.3f}) feet ({:.3f} {:.3f} {:.3f}) bits {:#x}", trap.name, inside, p.x,
                                p.y, p.z, feet.x, feet.y, feet.z, game_.MirrorViewportBits());
                    }
                }
                for (const TrapCondition& condition : trap.conditions) {
                    ConditionState& state = conditions_[{stage_id, condition.entity}];
                    Context ctx{stage, &file, &trap, &condition, TrapFlag::None, dt};
                    bool hit = false;
                    if (inside && !(condition.is_once && state.done) && stage->Body(condition.entity).enable) {
                        hit = RunChecks(ctx);
                    }
                    const bool was = state.flag == TrapFlag::Enter || state.flag == TrapFlag::Inside;
                    if (hit) {
                        ctx.flag = was ? TrapFlag::Inside : TrapFlag::Enter;
                        state.flag = ctx.flag;
                        (was ? in : enter).push_back({stage_id, ctx});
                    } else if (was) {
                        ctx.flag = TrapFlag::Out;
                        state.flag = TrapFlag::None;
                        out.push_back({stage_id, ctx});
                    }
                }
            }
        }
    }
    for (auto* list : {&out, &enter, &in}) {
        for (const Pending& pending : *list) {
            // an exec may have unloaded the stage (a floor change); its remaining hits go with it
            if (game_.Stages().FindById(pending.stage_id) != pending.ctx.stage) {
                continue;
            }
            const Context& ctx = pending.ctx;
            if (debug_log && ctx.flag != TrapFlag::Inside) {
                LogInfo("trap: {} {}", ctx.condition->name, TrapFlagString(ctx.flag));
            }
            const int result = RunExecs(ctx);
            if (result == 1 && game_.Stages().FindById(pending.stage_id) == ctx.stage) {
                conditions_[{pending.stage_id, ctx.condition->entity}].done = true;
            }
        }
    }
}

}
