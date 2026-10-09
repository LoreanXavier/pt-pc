#include "game/input_script.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "game/archive.h"
#include "engine/platform/virtual_pad.h"
#include "game/game.h"
#include "game/render_scene.h"
#include "game/ui/game_ui.h"

namespace pt::game {

bool InputScript::Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LogError("input script: cannot open {}", path.string());
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return Parse(buffer.str());
}

bool InputScript::Parse(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        for (char& c : line) {
            if (c == ';') {
                c = '\n';
            }
        }
        std::istringstream parts_stream(line);
        std::string part;
        while (std::getline(parts_stream, part)) {
            std::istringstream words(part);
            Command command;
            if (!(words >> command.frame >> command.op)) {
                continue;
            }
            if (command.op == "sshot" || command.op == "shot") {
                std::string path;
                std::getline(words >> std::ws, path);
                while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) {
                    path.pop_back();
                }
                if (!path.empty()) {
                    command.text = path;
                    command.words.push_back(path);
                }
                commands_.push_back(std::move(command));
                continue;
            }
            std::string word;
            while (words >> word) {
                size_t used = 0;
                float value = 0.0f;
                try {
                    value = std::stof(word, &used);
                } catch (...) {
                    used = 0;
                }
                if (used == word.size()) {
                    command.args.push_back(value);
                } else {
                    command.text = word;
                    command.words.push_back(word);
                }
            }
            commands_.push_back(std::move(command));
        }
    }
    std::stable_sort(commands_.begin(), commands_.end(), [](const Command& a, const Command& b) { return a.frame < b.frame; });
    return true;
}

const Stage* InputScript::NearestHallway(Game& game) const {
    const Stage* best = nullptr;
    float best_distance = 1e30f;
    const glm::vec3 feet = game.GetPlayer().Feet();
    for (const auto& [label, stage] : game.Stages().Stages()) {
        glm::mat4 door;
        if (stage->ConnectorWorld("startDoor", door)) {
            const float d = glm::length(glm::vec3(door[3]) - feet);
            if (d < best_distance) {
                best_distance = d;
                best = stage.get();
            }
        }
    }
    return best;
}

void InputScript::Apply(uint64_t frame, Game& game, InputState& input) {
    for (size_t i = 0; i < releases_.size();) {
        if (releases_[i].frame <= frame) {
            if (pads_) {
                pads_->SetButton(releases_[i].slot, releases_[i].button, false);
            }
            releases_.erase(releases_.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < key_releases_.size();) {
        if (key_releases_[i].first <= frame) {
            const uint32_t code = key_releases_[i].second;
            if (device_ && code >= 0x10000u) {
                device_->InjectMouseButton(static_cast<uint8_t>(code - 0x10000u), false);
            } else if (device_) {
                device_->InjectKey(code, false);
            }
            key_releases_.erase(key_releases_.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (burst_left_ > 0) {
        game.RequestScreenshot(std::format("{}-{:02d}.png", burst_prefix_, burst_index_++));
        --burst_left_;
    }
    if (hold_until_ > 0 && !hold_done_ && game.Controller().Step() >= 15) {
        if (!holding_ && frame < hold_until_) {
            holding_ = true;
            game.SetPaused(true);
            LogInfo("input script: control at frame {}, game held until frame {}", frame, hold_until_);
        } else if (!holding_) {
            hold_done_ = true;
            const uint64_t late = frame - hold_until_;
            for (size_t i = next_; i < commands_.size(); ++i) {
                commands_[i].frame += late;
            }
            LogInfo("input script: control at frame {}, {} frames after hold frame {}, later lines shifted by {}", frame,
                    late, hold_until_, late);
        }
        if (holding_ && frame >= hold_until_) {
            holding_ = false;
            hold_done_ = true;
            game.SetPaused(false);
            LogInfo("input script: the game resumes at frame {}", frame);
        }
    }
    while (next_ < commands_.size() && commands_[next_].frame <= frame) {
        if (hold_until_ > 0 && !hold_done_ && !holding_ && commands_[next_].frame >= hold_until_) {
            break;
        }
        const Command& c = commands_[next_++];
        auto arg = [&](size_t i) { return i < c.args.size() ? c.args[i] : 0.0f; };
        if (c.op == "pattach" || c.op == "pdetach" || c.op == "pbutton" || c.op == "ptap" || c.op == "paxis" || c.op == "ptouch" || c.op == "pbot" ||
            c.op == "expect" || c.op == "sound") {
            PadCommand(c.op, c.words, c.args, frame, game, input);
        } else if (c.op == "slightstick") {
            if (c.text == "off") {
                game.GetPlayer().SetLightStickOverride(std::nullopt);
            } else {
                game.GetPlayer().SetLightStickOverride(glm::vec2(arg(0), arg(1)));
            }
        } else if (c.op == "key" || c.op == "ktap" || c.op == "mbutton" || c.op == "mtap") {
            KeyCommand(c.op, c.words, c.args, frame);
        } else if (c.op == "stap" || c.op == "sbutton" || c.op == "saxis" || c.op == "stouch" || c.op == "sexpect" || c.op == "ssound" ||
                   c.op == "smenu" || c.op == "spaused" || c.op == "sfocus" || c.op == "skey" || c.op == "sktap" || c.op == "smbutton" ||
                   c.op == "smtap" || c.op == "sattach" || c.op == "sdetach") {
            Waypoint item{glm::vec3(0.0f), false, false, c.op, 0, c.text};
            item.words = c.words;
            item.args = c.args;
            waypoints_.push_back(std::move(item));
        } else if (c.op == "move") {
            move_ = glm::vec2(arg(0), arg(1));
        } else if (c.op == "look") {
            input.mouse_look += glm::vec2(arg(0), arg(1));
        } else if (c.op == "action") {
            action_ = true;
        } else if (c.op == "zoom") {
            zoom_ = arg(0) != 0.0f;
        } else if (c.op == "yaw") {
            game.GetPlayer().yaw = game.GetPlayer().target_yaw = arg(0);
        } else if (c.op == "pitch") {
            game.GetPlayer().SetScriptPitch(arg(0));
        } else if (c.op == "teleport") {
            game.GetPlayer().Warp(glm::vec3(arg(0), arg(1), arg(2)), game.GetPlayer().BodyFoxYaw());
            LogInfo("input script frame {}: teleport to ({:.3f} {:.3f} {:.3f}) camera ({:.3f} {:.3f} {:.3f})", frame, arg(0), arg(1), arg(2),
                    game.GetCamera().position.x, game.GetCamera().position.y, game.GetCamera().position.z);
        } else if (c.op == "face") {
            Face(game, glm::vec3(arg(0), arg(1), arg(2)));
        } else if (c.op == "shold") {
            hold_until_ = static_cast<uint64_t>(std::max(0.0f, arg(0)));
        } else if (c.op == "step") {
            game.Controller().ChangeGameStep(c.text);
        } else if (c.op == "voice") {
            game.OnVoiceKeyword(c.text.empty() ? "jack" : c.text);
        } else if (c.op == "anchor") {
            if (const Stage* stage = NearestHallway(game)) {
                anchor_inverse_ = glm::inverse(stage->file_to_world);
                anchored_ = true;
            }
        } else if (c.op == "sfeet") {
            waypoints_.push_back({glm::vec3(arg(0), 0.0f, arg(1)), false, false, "feet", 0});
            waypoints_.back().extra.x = c.args.size() > 2 ? arg(2) : 0.3f;
        } else if (c.op == "goto") {
            waypoints_.push_back({glm::vec3(arg(0), arg(1), arg(2)), false, false, "goto", static_cast<int>(arg(3))});
            waypoints_.back().extra.x = arg(4);
        } else if (c.op == "gfile" || c.op == "ffile") {
            Waypoint item{glm::vec3(arg(0), arg(1), arg(2)), false, false, c.op == "gfile" ? "goto" : "face", static_cast<int>(arg(3))};
            item.file_space = true;
            waypoints_.push_back(item);
        } else if (c.op == "sanchor" || c.op == "fent" || c.op == "fdir" || c.op == "sfree" || c.op == "sfloor" || c.op == "sdemo") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, c.op[0] == 's' ? c.op.substr(1) : c.op, 0, c.text});
        } else if (c.op == "glisa") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "goto_lisa", static_cast<int>(arg(0))});
        } else if (c.op == "slisa" || c.op == "flisa") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, c.op == "slisa" ? "lisa" : "face_lisa", static_cast<int>(arg(0))});
        } else if (c.op == "sdframe") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "dframe", static_cast<int>(arg(0)), c.text});
            waypoints_.back().extra.x = arg(1);
        } else if (c.op == "sarchive") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "archive", 0, c.text});
        } else if (c.op == "sarchived") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "archived", c.args.empty() ? 7200 : static_cast<int>(arg(0))});
        } else if (c.op == "sarchiveshown") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "archiveshown", c.args.empty() ? 3600 : static_cast<int>(arg(0))});
        } else if (c.op == "sloop") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "browse_loop", static_cast<int>(arg(0))});
        } else if (c.op == "sbrowsed" || c.op == "sfade") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, c.op.substr(1), c.args.empty() ? 3600 : static_cast<int>(arg(0))});
        } else if (c.op == "sat") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "at", static_cast<int>(arg(0)), c.text});
        } else if (c.op == "sstep") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "step", static_cast<int>(arg(0))});
        } else if (c.op == "steleport") {
            Waypoint item{glm::vec3(arg(0), arg(1), arg(2)), false, false, "teleport"};
            item.args = c.args;
            waypoints_.push_back(std::move(item));
        } else if (c.op == "sgamestep") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "gamestep", 0, c.text});
        } else if (c.op == "sstreet") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "street"});
        } else if (c.op == "sbody") {
            Waypoint item{glm::vec3(0.0f), false, false, "body", static_cast<int>(arg(0))};
            item.text = c.words.empty() ? std::string() : c.words.front();
            waypoints_.push_back(item);
        } else if (c.op == "smodel") {
            Waypoint item{glm::vec3(arg(0), arg(1), arg(2)), false, false, "model"};
            item.text = c.words.empty() ? std::string() : c.words.front();
            item.extra = glm::vec4(arg(3), arg(4), arg(5), c.args.size() > 6 ? arg(6) : 1.0f);
            waypoints_.push_back(item);
        } else if (c.op == "splaydemo") {
            Waypoint item{glm::vec3(0.0f), false, false, "playdemo", 0, c.words.empty() ? std::string() : c.words.front()};
            item.words = c.words;
            waypoints_.push_back(item);
        } else if (c.op == "sgameplus") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "gameplus"});
        } else if (c.op == "soffer") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "offer"});
        } else if (c.op == "scredits") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "credits"});
        } else if (c.op == "squit") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "quit"});
        } else if (c.op == "sev") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "ev", 0, c.text});
            waypoints_.back().point.x = arg(0);
        } else if (c.op == "sshot" || c.op == "srate") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, c.op.substr(1), 0, c.text});
            waypoints_.back().point.x = arg(0);
        } else if (c.op == "sburst") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "burst", static_cast<int>(arg(0)), c.text});
        } else if (c.op == "ssteps") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "steps", static_cast<int>(arg(0))});
        } else if (c.op == "sstick" || c.op == "srstick" || c.op == "slook") {
            Waypoint item{glm::vec3(0.0f), false, false, c.op.substr(1), static_cast<int>(arg(2))};
            item.stick = glm::vec2(arg(0), arg(1));
            waypoints_.push_back(item);
        } else if (c.op == "strace") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "trace", static_cast<int>(arg(0))});
        } else if (c.op == "goto_rel") {
            waypoints_.push_back({glm::vec3(arg(0), arg(1), arg(2)), true, false, "goto"});
        } else if (c.op == "goto_loop") {
            waypoints_.push_back({glm::vec3(arg(0), arg(1), arg(2)), true, true, "goto"});
        } else if (c.op == "scamera" || c.op == "scamfile") {
            Waypoint item{glm::vec3(arg(0), arg(1), arg(2)), false, false, "camera", 0, c.text};
            item.extra = glm::vec4(arg(3), arg(4), arg(5), arg(6));
            item.up = glm::vec3(arg(7), arg(8), arg(9));
            item.file_space = c.op == "scamfile";
            waypoints_.push_back(item);
        } else if (c.op == "sface" || c.op == "sface_rel") {
            waypoints_.push_back({glm::vec3(arg(0), arg(1), arg(2)), c.op == "sface_rel", false, "face"});
        } else if (c.op == "sphotoset") {
            Waypoint item{glm::vec3(arg(0), arg(1), arg(2)), false, false, "photoset", static_cast<int>(arg(4))};
            item.extra = glm::vec4(arg(3), arg(5), c.args.size() > 6 ? arg(6) : 1.0f, 0.0f);
            item.up.x = c.args.size() > 7 ? arg(7) : -1.0f;
            waypoints_.push_back(item);
        } else if (c.op == "sphotocam") {
            Waypoint item{glm::vec3(arg(0), arg(1), arg(2)), false, false, "photocam", 0};
            item.extra = glm::vec4(arg(3), arg(4), arg(5), 0.0f);
            waypoints_.push_back(item);
        } else if (c.op == "sphotoview" || c.op == "sphotofile") {
            Waypoint item{glm::vec3(arg(0), arg(1), arg(2)), false, false, "photoview", 0};
            item.extra = glm::vec4(arg(3), arg(4), arg(5), 0.0f);
            item.file_space = c.op == "sphotofile";
            waypoints_.push_back(item);
        } else if (c.op == "sphoto") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "photo", static_cast<int>(c.args.empty() ? 1.0f : arg(0))});
        } else if (c.op == "sfreeze") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "freeze", static_cast<int>(arg(0))});
        } else if (c.op == "sfreecam") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "freecam", static_cast<int>(c.args.empty() ? 1.0f : arg(0))});
        } else if (c.op == "sthird") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "third", static_cast<int>(c.args.empty() ? 1.0f : arg(0))});
        } else if (c.op == "sstate") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "state", 0, c.text});
        } else if (c.op == "sreset") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, "reset"});
        } else if (c.op == "saction" || c.op == "slog" || c.op == "svoice") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, c.op.substr(1), 0, c.text});
        } else if (c.op == "sui") {
            Waypoint item{glm::vec3(0.0f), false, false, "ui"};
            item.words = c.words;
            item.args = c.args;
            waypoints_.push_back(item);
        } else if (c.op == "swait" || c.op == "szoom" || c.op == "shandylight") {
            waypoints_.push_back({glm::vec3(0.0f), false, false, c.op.substr(1), static_cast<int>(arg(0))});
        } else if (c.op == "mmove" || c.op == "mclick" || c.op == "mright") {
            input.pointer = glm::vec2(arg(0), arg(1));
            input.pointer_valid = true;
            input.click = c.op == "mclick";
            input.right_click = c.op == "mright";
        } else if (GameUi::ScriptCommand(game, c.op, c.text, c.args)) {
        } else if (c.op == "shot") {
            game.RequestScreenshot(c.text);
        } else if (c.op == "camera") {
            if (c.text == "off") {
                game.SetCameraOverride(std::nullopt);
            } else {
                SetScriptCamera(game, glm::vec3(arg(0), arg(1), arg(2)), glm::vec4(arg(3), arg(4), arg(5), arg(6)), glm::vec3(arg(7), arg(8), arg(9)),
                                true);
            }
        } else if (c.op == "trace") {
            trace_frames_ = static_cast<int>(arg(0));
        } else if (c.op == "ev") {
            game.Effects().ev_pinned = c.text != "auto";
            game.Effects().pinned_ev = arg(0);
        } else if (c.op == "handylight") {
            game.GetPlayer().handy_light.enable = arg(0) != 0.0f;
            LogInfo("input script frame {}: handy light {}", frame, game.GetPlayer().handy_light.enable ? 1 : 0);
        } else if (c.op == "log") {
            const glm::vec3 p = game.GetPlayer().Feet();
            LogInfo("input script frame {}: step {} floor {} loop {} feet ({:.2f} {:.2f} {:.2f}) yaw {:.2f}", frame, game.Controller().Step(),
                    game.Floor().CurrentFloorName(), game.Floor().LoopCount(), p.x, p.y, p.z, game.GetPlayer().yaw);
        }
    }
    if (trace_frames_ > 0) {
        --trace_frames_;
        const Camera camera = game.GetCamera();
        const glm::vec3 feet = game.GetPlayer().Feet();
        std::string demos;
        for (const PlayingDemo& demo : game.Demos().Playing()) {
            demos += std::format(" {}@{:.3f}", demo.demo_id, demo.frame);
            if (demo.audio_clock) {
                demos += std::format(" audio {:.3f}", demo.audio_frame);
            }
            for (const DemoModel& m : demo.models) {
                const int head = m.skeleton && m.drawn && m.visible ? m.skeleton->FindName("SKL_004_HEAD") : -1;
                if (head >= 0 && static_cast<size_t>(head) < m.bone_world.size()) {
                    const glm::vec3 p = glm::vec3(m.world * m.bone_world[static_cast<size_t>(head)][3]);
                    demos += std::format(" {}.head ({:.4f} {:.4f} {:.4f})", m.name, p.x, p.y, p.z);
                }
            }
        }
        const glm::vec3 aim = game.HandyAim();
        glm::vec3 handy_position(0.0f);
        glm::vec3 handy_direction(0.0f);
        HandyLightPose(camera, aim, game.HandyPickupHold(), game.HandyDemoPose(), handy_position, handy_direction);
        const glm::vec2 stick = game.GetPlayer().LightStick();
        LogInfo("input script trace {}: camera ({:.4f} {:.4f} {:.4f}) yaw {:.3f} pitch {:.3f} roll {:.3f} feet ({:.4f} {:.4f} {:.4f}) vfov {:.3f} "
                "light aim ({:.4f} {:.4f} {:.4f}) origin ({:.4f} {:.4f} {:.4f}) dir ({:.5f} {:.5f} {:.5f}) stick ({:.3f} {:.3f}) demos{}",
                frame, camera.position.x, camera.position.y, camera.position.z, glm::degrees(camera.yaw), glm::degrees(camera.pitch),
                glm::degrees(camera.roll), feet.x, feet.y, feet.z, glm::degrees(camera.fov_y), aim.x, aim.y, aim.z, handy_position.x, handy_position.y,
                handy_position.z, handy_direction.x, handy_direction.y, handy_direction.z, stick.x, stick.y, demos);
    }
    if (bot_slot_ >= 0 && pads_) {
        ApplyBot(frame, game, input);
        return;
    }
    bool moved = false;
    while (!waypoints_.empty()) {
        Waypoint& item = waypoints_.front();
        Resolve(game, item);
        if (item.op == "goto") {
            moved = ApplyGoto(frame, game, input);
            break;
        }
        if (item.op == "smenu" || item.op == "spaused") {
            const bool open = item.op == "smenu" ? GameUi::Active() && GameUi::Active()->MenuOpen() : game.Paused();
            if (open != (!item.args.empty() && item.args[0] != 0.0f)) {
                break;
            }
        } else if (item.op == "sfocus") {
            seq_focus_ = !item.args.empty() && item.args[0] != 0.0f;
            seq_focus_frame_ = frame;
            seq_focus_until_ = item.args.size() > 1 && item.args[1] > 0.0f ? frame + static_cast<uint64_t>(item.args[1]) : 0;
        } else if (item.op == "stap" || item.op == "sbutton" || item.op == "saxis" || item.op == "stouch" || item.op == "sexpect" ||
                   item.op == "ssound") {
            PadCommand("p" + item.op.substr(1), item.words, item.args, frame, game, input);
        } else if (item.op == "sattach" || item.op == "sdetach") {
            PadCommand(item.op == "sattach" ? "pattach" : "pdetach", item.words, item.args, frame, game, input);
        } else if (item.op == "skey" || item.op == "sktap" || item.op == "smbutton" || item.op == "smtap") {
            KeyCommand(item.op.substr(1), item.words, item.args, frame);
        } else if (item.op == "wait") {
            if (--item.frames > 0) {
                break;
            }
        } else if (item.op == "stick") {
            if (item.frames-- > 0) {
                input.left_stick = item.stick;
                input.left_stick_from_pad = true;
                moved = true;
                break;
            }
        } else if (item.op == "rstick") {
            if (item.frames-- > 0) {
                input.right_stick = item.stick;
                input.from_gamepad = true;
                break;
            }
        } else if (item.op == "look") {
            if (item.frames-- > 0) {
                input.mouse_look += item.stick;
                break;
            }
        } else if (item.op == "trace") {
            trace_frames_ = item.frames;
        } else if (item.op == "burst") {
            burst_left_ = item.frames;
            burst_index_ = 0;
            burst_prefix_ = item.text;
        } else if (item.op == "steps") {
            if (item.text.empty()) {
                item.text = std::to_string(game.GetPlayer().foot_steps);
            }
            if (game.GetPlayer().foot_steps - std::stoi(item.text) < item.frames) {
                input.left_stick = glm::vec2(0.0f, 1.0f);
                input.left_stick_from_pad = true;
                moved = true;
                break;
            }
            LogInfo("input script: walked {} footsteps", item.frames);
        } else if (item.op == "feet") {
            Player& player = game.GetPlayer();
            const glm::vec3 feet = player.Feet();
            const bool still = glm::length(feet - eye_last_feet_) < 0.0005f;
            eye_last_feet_ = feet;
            eye_still_ = still ? eye_still_ + 1 : 0;
            if (eye_still_ < 4 && ++eye_wait_ < 120) {
                break;
            }
            eye_wait_ = 0;
            eye_still_ = 0;
            const glm::vec2 d(item.point.x - feet.x, item.point.z - feet.z);
            if (glm::length(d) <= item.extra.x) {
                player.controller.position += glm::vec3(d.x, 0.0f, d.y);
                LogInfo("input script: feet moved {:.3f} m to ({:.3f} {:.3f}) at frame {}", glm::length(d), item.point.x, item.point.z, frame);
            } else {
                LogWarn("input script: feet {:.3f} m from ({:.3f} {:.3f}) at frame {}, not moved", glm::length(d), item.point.x, item.point.z, frame);
            }
        } else if (item.op == "street") {
            game.StartStreetWalk();
        } else if (item.op == "offer") {
            game.OfferStreetWalk();
        } else if (item.op == "credits") {
            game.StartPortCredits();
            game.Controller().SetStep(33);
        } else if (item.op == "free") {
            if (game.Demos().ControlsPlayer()) {
                break;
            }
        } else if (item.op == "lisa") {
            if (!game.Objects().Ocho().Visible() && (item.frames <= 0 || --item.frames > 0)) {
                break;
            }
            const glm::vec3 lisa(game.Objects().Ocho().World()[3]);
            LogInfo("input script: Lisa {} (state {}, spawn {}) at ({:.2f} {:.2f} {:.2f})", game.Objects().Ocho().Visible() ? "visible" : "not seen",
                    game.Objects().Ocho().State(), game.Objects().Ocho().SpawnIndex(), lisa.x, lisa.y, lisa.z);
        } else if (item.op == "goto_lisa") {
            item.op = "goto";
            item.point = glm::vec3(game.Objects().Ocho().World()[3]);
            continue;
        } else if (item.op == "face_lisa") {
            Face(game, glm::vec3(game.Objects().Ocho().World()[3]) + glm::vec3(0.0f, 1.5f, 0.0f));
        } else if (item.op == "browse_loop" || item.op == "browsed" || item.op == "fade" || item.op == "at") {
            if (BrowseWait(game, item)) {
                break;
            }
        } else if (item.op == "archive" || item.op == "archived" || item.op == "archiveshown") {
            if (ArchiveWait(game, item)) {
                break;
            }
        } else if (item.op == "step") {
            if (game.Controller().Step() != item.frames) {
                break;
            }
        } else if (item.op == "teleport") {
            Player& player = game.GetPlayer();
            const glm::vec3 to = item.args.size() == 2 ? glm::vec3(item.args[0], player.Feet().y, item.args[1]) : item.point;
            player.Warp(to, player.BodyFoxYaw());
            LogInfo("input script frame {}: teleport to ({:.3f} {:.3f} {:.3f})", frame, to.x, to.y, to.z);
        } else if (item.op == "gamestep") {
            LogInfo("input script frame {}: ChangeGameStep({})", frame, item.text);
            game.Controller().ChangeGameStep(item.text);
        } else if (item.op == "photoset") {
            std::vector<int> values{static_cast<int>(item.point.x), static_cast<int>(item.point.y), static_cast<int>(item.point.z),
                                    static_cast<int>(item.extra.x), item.frames, static_cast<int>(item.extra.y),
                                    static_cast<int>(item.extra.z)};
            if (item.up.x >= 0.0f) values.push_back(static_cast<int>(item.up.x));
            game.RequestPhotoSettings(values);
        } else if (item.op == "photocam") {
            game.RequestPhotoCamera(glm::vec4(item.point, item.extra.x));
            game.RequestPhotoTarget(glm::vec2(item.extra.y, item.extra.z));
        } else if (item.op == "photoview") {
            game.RequestPhotoView(item.point, glm::vec3(item.extra));
        } else if (item.op == "photo") {
            game.RequestPhotoMode(item.frames);
        } else if (item.op == "freeze") {
            game.SetPaused(item.frames != 0);
            LogInfo("input script: game clock {}", item.frames != 0 ? "frozen" : "running");
        } else if (item.op == "freecam") {
            game.RequestFreeCamera(item.frames != 0);
        } else if (item.op == "third") {
            game.SetThirdPerson(item.frames != 0);
        } else if (item.op == "state") {
            SessionState(game, item.text, frame);
        } else if (item.op == "reset") {
            LogInfo("input script: reset progress {}", game.ResetProgress() ? "accepted" : "refused (no save store)");
        } else if (item.op == "gameplus") {
            game.EnableGamePlus();
        } else if (item.op == "playdemo") {
            if (item.text.starts_with("?")) {
                for (const PlayingDemo& demo : game.Demos().Playing()) {
                    if (demo.demo_id != item.text.substr(1)) continue;
                    for (const DemoModel& m : demo.models) {
                        const glm::vec3 w(m.world[3]);
                        const glm::vec3 file(glm::inverse(file_transform_) * glm::vec4(w, 1.0f));
                        LogInfo("input script: demo {} model {} drawn {} world ({:.2f} {:.2f} {:.2f}) file ({:.2f} {:.2f} {:.2f})", demo.demo_id,
                                m.name, m.drawn, w.x, w.y, w.z, file.x, file.y, file.z);
                    }
                }
            } else {
                const bool started = game.Demos().Play(item.text);
                LogInfo("input script frame {}: demo {} {}", frame, item.text, started ? "started" : "refused");
                if (started && item.words.size() > 1) {
                    glm::mat4 world(1.0f);
                    if (EntityTransform(game, item.words[1], world)) {
                        game.Demos().SetDemoTransform(item.text, glm::quat_cast(glm::mat3(world)), glm::vec3(world[3]));
                        LogInfo("input script: demo {} placed at {} ({:.2f} {:.2f} {:.2f})", item.text, item.words[1], world[3].x, world[3].y, world[3].z);
                    }
                }
            }
        } else if (item.op == "body") {
            int changed = 0;
            game.Stages().ForEachStage([&](Stage& stage) {
                for (const auto& file : stage.files) {
                    auto set = [&](const fox2::Entity* e) {
                        BodyState& body = stage.Body(e);
                        body.visible = body.enable = item.frames != 0;
                        if (++changed <= 3) {
                            const glm::vec3 at(file->file->WorldTransform(*e)[3]);
                            LogInfo("input script: body {} of {} at file ({:.2f} {:.2f} {:.2f})", file->file->EntityName(*e), stage.label, at.x, at.y, at.z);
                        }
                    };
                    if (item.text.ends_with('*')) {
                        const std::string_view part = std::string_view(item.text).substr(0, item.text.size() - 1);
                        for (const fox2::Entity& e : file->file->Entities()) {
                            if (file->file->EntityName(e).find(part) != std::string::npos) set(&e);
                        }
                    } else if (const fox2::Entity* e = file->file->ByShortName(item.text)) {
                        set(e);
                    }
                }
            });
            game.Stages().MarkVisualsDirty();
            LogInfo("input script frame {}: body {} {} in {} stage(s)", frame, item.text, item.frames != 0 ? "shown" : "hidden", changed);
        } else if (item.op == "model") {
            if (item.text == "off") {
                game.ClearMockupDraws();
                LogInfo("input script frame {}: script models removed", frame);
            } else if (Stage* stage = game.Stages().FindById(file_stage_); stage && !stage->files.empty()) {
                const ModelEntry* model = game.Models().Get(item.text);
                if (model && model->mesh) {
                    glm::mat4 m = glm::translate(glm::mat4(1.0f), item.point);
                    m = glm::rotate(m, glm::radians(item.extra.x), glm::vec3(0.0f, 1.0f, 0.0f));
                    m = glm::rotate(m, glm::radians(item.extra.y), glm::vec3(1.0f, 0.0f, 0.0f));
                    m = glm::rotate(m, glm::radians(item.extra.z), glm::vec3(0.0f, 0.0f, 1.0f));
                    m = glm::scale(m, glm::vec3(item.extra.w));
                    game.AddMockupDraw(model->mesh.get(), stage->file_to_world * m);
                    LogInfo("input script frame {}: model {} at file ({:.2f} {:.2f} {:.2f}) in stage {}", frame, item.text, item.point.x,
                            item.point.y, item.point.z, stage->label);
                } else {
                    LogWarn("input script frame {}: model {} not loaded", frame, item.text);
                }
            } else {
                LogWarn("input script frame {}: smodel needs an anchored stage (sanchor)", frame);
            }
        } else if (item.op == "quit") {
            game.RequestQuit();
        } else if (item.op == "shot" && std::exchange(skip_next_shot_, false)) {
            LogWarn("input script: shot {} dropped, loop not reached", item.text);
        } else if (item.op == "shot") {
            std::string path = item.text;
            for (const auto& [key, value] : {std::pair<std::string, std::string>{"{floor}", game.Floor().CurrentFloorName()},
                                             {"{loop}", std::to_string(game.Floor().LoopCount())}}) {
                for (size_t at = path.find(key); at != std::string::npos; at = path.find(key)) {
                    path.replace(at, key.size(), value);
                }
            }
            game.RequestScreenshot(path);
        } else if (item.op == "camera") {
            if (item.text == "off") {
                game.SetCameraOverride(std::nullopt);
            } else {
                SetScriptCamera(game, item.point, item.extra, item.up);
            }
        } else if (item.op == "rate") {
            game.Demos().time_scale = item.point.x;
        } else if (item.op == "ev") {
            game.Effects().ev_pinned = item.text != "auto";
            game.Effects().pinned_ev = item.point.x;
            LogInfo("input script: ev {}", item.text == "auto" ? std::string("auto") : std::format("{:.3f}", item.point.x));
        } else if (item.op == "demo") {
            if (game.Demos().IsPlaying(item.text)) {
                break;
            }
        } else if (item.op == "dframe") {
            if (!game.Demos().IsPlaying(item.text) || game.Demos().PlayTime(item.text) * 59.94 < item.frames) {
                if (item.extra.x <= 0.0f || --item.extra.x > 0.0f) {
                    break;
                }
                LogWarn("input script: sdframe {} {} gave up at controller step {} floor {}", item.text, item.frames, game.Controller().Step(),
                        game.Floor().CurrentFloorName());
                skip_next_shot_ = true;
            }
        } else if (item.op == "floor") {
            if (!game.Floor().IsCurrentFloorName(item.text)) {
                break;
            }
        } else if (item.op == "anchor") {
            const Stage* stage = item.text.empty() ? NearestHallway(game) : game.Stages().Find(item.text);
            if (stage) {
                file_transform_ = stage->file_to_world;
                file_stage_ = stage->id;
                LogInfo("input script: file frame anchored to stage {} ({})", stage->id, stage->label);
            } else {
                LogWarn("input script: no stage to anchor ({})", item.text);
            }
        } else if (item.op == "fent" || item.op == "fdir") {
            glm::mat4 world(1.0f);
            if (!EntityTransform(game, item.text, world)) {
                LogWarn("input script: entity {} not found", item.text);
            } else if (item.op == "fent") {
                Face(game, glm::vec3(world[3]));
                const glm::vec3 file(glm::inverse(file_transform_) * world[3]);
                LogInfo("input script: facing {} at ({:.2f} {:.2f} {:.2f}), file ({:.2f} {:.2f} {:.2f})", item.text, world[3][0], world[3][1],
                        world[3][2], file.x, file.y, file.z);
            } else {
                glm::vec3 d(world[2][0], 0.0f, world[2][2]);
                if (glm::length(d) > 1e-4f) {
                    d = glm::normalize(d);
                    Face(game, game.GetPlayer().Eye() + d * 5.0f);
                }
            }
        } else if (item.op == "face") {
            Face(game, item.point);
        } else if (item.op == "action") {
            action_ = true;
        } else if (item.op == "zoom") {
            zoom_ = item.frames != 0;
        } else if (item.op == "ui") {
            if (item.words.empty() ||
                !GameUi::ScriptCommand(game, item.words[0], item.words.size() > 1 ? std::string_view(item.words[1]) : std::string_view(), item.args)) {
                LogWarn("input script: sui {} is not a UI op", item.words.empty() ? std::string() : item.words[0]);
            }
        } else if (item.op == "handylight") {
            game.GetPlayer().handy_light.enable = item.frames != 0;
            LogInfo("input script: handy light {}", item.frames != 0 ? 1 : 0);
        } else if (item.op == "voice") {
            game.OnVoiceKeyword(item.text.empty() ? "jack" : item.text);
        } else if (item.op == "log") {
            const glm::vec3 p = game.GetPlayer().Feet();
            LogInfo("input script seq frame {}: step {} floor {} loop {} feet ({:.2f} {:.2f} {:.2f}) footsteps {} (sounded {})", frame,
                    game.Controller().Step(), game.Floor().CurrentFloorName(), game.Floor().LoopCount(), p.x, p.y, p.z, game.GetPlayer().foot_steps,
                    game.GetPlayer().sounding_steps);
            const Camera camera = game.GetCamera();
            const glm::vec3 f = camera.Forward();
            const glm::vec3 u = camera.Up();
            LogInfo("input script camera: pos ({:.4f} {:.4f} {:.4f}) fwd ({:.4f} {:.4f} {:.4f}) up ({:.4f} {:.4f} {:.4f}) vfov {:.3f}", camera.position.x,
                    camera.position.y, camera.position.z, f.x, f.y, f.z, u.x, u.y, u.z, glm::degrees(camera.fov_y));
        }
        waypoints_.erase(waypoints_.begin());
        if (item.op == "action" || item.op == "face" || item.op == "fent" || item.op == "fdir" || item.op == "face_lisa") {
            break;
        }
    }
    if (!moved && glm::length(move_) > 0.0f) {
        input.left_stick = move_;
        input.left_stick_from_pad = true;
    }
    uint32_t held = input.held;
    if (zoom_) {
        held |= kPadZoom;
    }
    bool gouge = false;
    if (action_) {
        held |= kPadAction;
        action_ = false;
        gouge = (previous_held_ & kPadAction) == 0;
    }
    input.pressed |= held & ~previous_held_ & ~input.held;
    if (gouge) {
        input.pressed |= kPadGouge;
    }
    input.held = held;
    previous_held_ = held;
}

void InputScript::SetScriptCamera(Game& game, const glm::vec3& position, const glm::vec4& forward_fov, const glm::vec3& requested_up,
                                  bool follows_view) {
    game.SetCameraOverride(std::nullopt);
    Camera camera = game.GetCamera();
    const glm::vec3 forward = glm::normalize(glm::vec3(forward_fov));
    camera.position = position;
    camera.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    camera.yaw = std::atan2(-forward.x, -forward.z);
    camera.roll = 0.0f;
    if (glm::length(requested_up) > 0.5f) {
        const glm::vec3 level = camera.Up();
        const glm::vec3 up = glm::normalize(requested_up - forward * glm::dot(requested_up, forward));
        camera.roll = std::atan2(glm::dot(forward, glm::cross(level, up)), glm::dot(level, up));
    }
    if (forward_fov.w > 0.0f) {
        camera.fov_y = glm::radians(forward_fov.w);
    }
    game.SetCameraOverride(camera, follows_view);
    LogInfo("input script: camera at ({:.3f} {:.3f} {:.3f}) yaw {:.2f} pitch {:.2f} roll {:.2f} fov {:.1f}", camera.position.x, camera.position.y,
            camera.position.z, glm::degrees(camera.yaw), glm::degrees(camera.pitch), glm::degrees(camera.roll), glm::degrees(camera.fov_y));
}

void InputScript::Face(Game& game, const glm::vec3& point) {
    Player& player = game.GetPlayer();
    const glm::vec3 d = point - player.Eye();
    player.yaw = player.target_yaw = std::atan2(-d.x, -d.z);
    player.SetScriptPitch(std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z)));
}

bool InputScript::EntityTransform(Game& game, const std::string& name, glm::mat4& out) const {
    const Stage* anchored = game.Stages().FindById(file_stage_);
    auto search = [&](const Stage& stage) {
        for (const auto& file : stage.files) {
            if (const fox2::Entity* e = file->file->ByShortName(name)) {
                out = stage.ToWorld(file->file->WorldTransform(*e));
                return true;
            }
        }
        return false;
    };
    if (anchored && search(*anchored)) {
        return true;
    }
    for (const auto& [label, stage] : game.Stages().Stages()) {
        if (search(*stage)) {
            return true;
        }
    }
    if (glm::vec3 p; game.Nazo().ControlAssetPosition(name, p)) {
        out = glm::mat4(1.0f);
        out[3] = glm::vec4(p, 1.0f);
        return true;
    }
    return false;
}

void InputScript::Resolve(Game& game, Waypoint& item) {
    if (item.reanchor) {
        if (const Stage* stage = NearestHallway(game)) {
            loop_transform_ = stage->file_to_world;
            LogInfo("input script: loop anchored to stage {} ({})", stage->id, stage->label);
        }
        item.reanchor = false;
    }
    if (item.relative) {
        item.point = glm::vec3(loop_transform_ * anchor_inverse_ * glm::vec4(item.point, 1.0f));
        item.relative = false;
    }
    if (item.file_space) {
        item.point = glm::vec3(file_transform_ * glm::vec4(item.point, 1.0f));
        if (item.op == "camera") {
            const glm::mat3 rotation(file_transform_);
            item.extra = glm::vec4(rotation * glm::vec3(item.extra), item.extra.w);
            item.up = rotation * item.up;
        } else if (item.op == "photoview") {
            item.extra = glm::vec4(glm::vec3(file_transform_ * glm::vec4(glm::vec3(item.extra), 1.0f)), 0.0f);
        }
        item.file_space = false;
    }
}

bool InputScript::ApplyGoto(uint64_t frame, Game& game, InputState& input) {
    Player& player = game.GetPlayer();
    const glm::vec3 feet = player.Feet();
    const float tolerance = waypoints_.front().extra.x;
    const glm::vec3 from = tolerance > 0.0f ? player.Eye() : feet;
    glm::vec3 d = waypoints_.front().point - from;
    d.y = 0.0f;
    if (glm::length(d) < (tolerance > 0.0f ? tolerance : 0.35f)) {
        LogInfo("input script: reached ({:.2f} {:.2f} {:.2f}) at frame {}", feet.x, feet.y, feet.z, frame);
        waypoints_.erase(waypoints_.begin());
        stuck_frames_ = 0;
        return false;
    }
    Waypoint& item = waypoints_.front();
    if (item.frames > 0 && --item.frames == 0) {
        LogInfo("input script: gave up on ({:.2f} {:.2f} {:.2f}) at ({:.2f} {:.2f} {:.2f}), frame {}", item.point.x, item.point.y, item.point.z, feet.x,
                feet.y, feet.z, frame);
        waypoints_.erase(waypoints_.begin());
        return false;
    }
    player.yaw = player.target_yaw = std::atan2(-d.x, -d.z);
    input.left_stick = glm::vec2(0.0f, 1.0f);
    input.left_stick_from_pad = true;
    stuck_frames_ = glm::length(feet - last_position_) < 0.001f ? stuck_frames_ + 1 : 0;
    if (stuck_frames_ == 120) {
        LogWarn("input script: stuck at ({:.2f} {:.2f} {:.2f}) going to ({:.2f} {:.2f} {:.2f})", feet.x, feet.y, feet.z, waypoints_.front().point.x,
                waypoints_.front().point.y, waypoints_.front().point.z);
    }
    last_position_ = feet;
    return true;
}

std::optional<bool> InputScript::ForcedFocus(uint64_t frame) const {
    std::optional<bool> focus;
    uint64_t focus_frame = 0;
    for (const Command& c : commands_) {
        if (c.frame > frame) {
            break;
        }
        if (c.op == "focus") {
            focus = !c.args.empty() && c.args[0] != 0.0f;
            focus_frame = c.frame;
        }
    }
    if (seq_focus_ && (!focus || seq_focus_frame_ >= focus_frame)) {
        return seq_focus_until_ != 0 && frame >= seq_focus_until_ ? std::optional<bool>(true) : seq_focus_;
    }
    return focus;
}

bool InputScript::UsesPads() const {
    return std::any_of(commands_.begin(), commands_.end(), [](const Command& c) { return c.op == "pattach"; });
}

namespace {

constexpr float kBotFaceYaw = 0.02f;
constexpr float kBotFacePitch = 0.03f;
constexpr float kBotMinStick = 0.2f;

float WrapAngle(float a) {
    constexpr float kPi = 3.14159265f;
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0.0f) {
        a += 2.0f * kPi;
    }
    return a - kPi;
}

float TurnStick(float error, float tolerance) {
    const float a = std::abs(error);
    if (a < tolerance) {
        return 0.0f;
    }
    const float magnitude = std::clamp(a * 2.5f, kBotMinStick, 1.0f);
    return error > 0.0f ? -magnitude : magnitude;
}

uint32_t NamedBit(const std::string& name, bool raw) {
    static const std::pair<const char*, uint32_t> kPlayer[] = {
        {"zoom", kPadZoom},     {"l1", kPadL1},         {"r1", kPadR1},         {"triangle", kPadTriangle}, {"l3", kPadL3},
        {"action", kPadAction}, {"select", kPadSelect}, {"lookup", kPadLookUp}, {"lookdown", kPadLookDown},  {"lookleft", kPadLookLeft},
        {"lookright", kPadLookRight}};
    static const std::pair<const char*, uint32_t> kRaw[] = {
        {"square", kRawSquare}, {"cross", kRawCross}, {"circle", kRawCircle}, {"triangle", kRawTriangle}, {"l2", kRawL2}, {"r2", kRawR2},
        {"l1", kRawL1},         {"r1", kRawR1},       {"start", kRawStart},   {"select", kRawSelect},     {"l3", kRawL3}, {"r3", kRawR3},
        {"up", kRawUp},         {"down", kRawDown},   {"left", kRawLeft},     {"right", kRawRight}};
    if (raw) {
        for (const auto& [key, bit] : kRaw) {
            if (name == key) {
                return bit;
            }
        }
    } else {
        for (const auto& [key, bit] : kPlayer) {
            if (name == key) {
                return bit;
            }
        }
    }
    return 0;
}

}

void InputScript::PadCommand(const std::string& op, const std::vector<std::string>& words, const std::vector<float>& args, uint64_t frame, Game& game,
                             const InputState& input) {
    auto arg = [&](size_t i, float fallback = 0.0f) { return i < args.size() ? args[i] : fallback; };
    auto word = [&](size_t i) { return i < words.size() ? words[i] : std::string(); };
    const int slot = static_cast<int>(arg(0));
    if (op == "expect" || op == "pexpect") {
        Expect(words, args, frame, game, input);
        return;
    }
    if (op == "sound" || op == "psound") {
        if (game.Audio()) {
            const std::string name = word(0);
            const uint32_t id = name.starts_with("#") ? game.Audio()->PostEventId(static_cast<uint32_t>(std::stoul(name.substr(1))), nullptr)
                                                       : game.Audio()->PostEvent(name, nullptr);
            LogInfo("input script frame {}: sound {} posted as {}", frame, name, id);
        }
        return;
    }
    if (!pads_) {
        LogWarn("input script frame {}: {} needs virtual pads (--virtual-pads)", frame, op);
        return;
    }
    if (op == "pattach") {
        pads_->Attach(slot, word(0));
    } else if (op == "pdetach") {
        pads_->Detach(slot);
    } else if (op == "pbutton") {
        pads_->SetButton(slot, word(0), arg(1) != 0.0f);
    } else if (op == "ptap") {
        if (pads_->SetButton(slot, word(0), true)) {
            releases_.push_back({frame + static_cast<uint64_t>(std::max(1.0f, arg(1, 2.0f))), slot, word(0)});
        }
    } else if (op == "paxis") {
        pads_->SetAxis(slot, word(0), arg(1));
    } else if (op == "ptouch") {
        pads_->SetTouch(slot, arg(3) != 0.0f, arg(1), arg(2));
    } else if (op == "pbot") {
        if (bot_slot_ >= 0 && slot < 0) {
            SendBot(BotOutput{});
        }
        bot_slot_ = slot;
        bot_sent_ = false;
        LogInfo("input script frame {}: pad bot {}", frame, slot >= 0 ? std::format("drives virtual pad {}", slot) : std::string("off"));
    }
}

bool InputScript::KeyCommand(const std::string& op, const std::vector<std::string>& words, const std::vector<float>& args, uint64_t frame) {
    const std::string name = words.empty() ? std::string() : words.front();
    const bool mouse = op == "mbutton" || op == "mtap";
    const uint32_t code = mouse ? MouseButtonFromName(name) : ScancodeFromName(name);
    if (!device_ || code == 0) {
        LogWarn("input script frame {}: {} '{}': {}", frame, op, name, device_ ? "unknown name" : "no input device (--virtual-pads)");
        return false;
    }
    const bool tap = op == "ktap" || op == "mtap";
    const bool down = tap || (!args.empty() && args.front() != 0.0f);
    if (mouse) {
        device_->InjectMouseButton(static_cast<uint8_t>(code), down);
    } else {
        device_->InjectKey(code, down);
    }
    if (tap) {
        const uint64_t hold = static_cast<uint64_t>(std::max(1.0f, args.empty() ? 2.0f : args.front()));
        key_releases_.emplace_back(frame + hold, mouse ? 0x10000u + code : code);
    }
    LogInfo("input script frame {}: {} {} {}", frame, mouse ? "mouse button" : "key", name, tap ? "tapped" : down ? "down" : "up");
    return true;
}

bool InputScript::Expect(const std::vector<std::string>& words, const std::vector<float>& args, uint64_t frame, Game& game, const InputState& input) {
    auto arg = [&](size_t i, float fallback = 0.0f) { return i < args.size() ? args[i] : fallback; };
    auto word = [&](size_t i) { return i < words.size() ? words[i] : std::string(); };
    const std::string what = word(0);
    std::string got;
    bool ok = false;
    if (what == "held" || what == "pressed" || what == "raw" || what == "rawpressed") {
        const bool raw = what.starts_with("raw");
        const uint32_t bit = NamedBit(word(1), raw);
        const uint32_t value = what == "held" ? input.held : what == "pressed" ? input.pressed : what == "raw" ? input.raw_held : input.raw_pressed;
        const bool on = bit != 0 && (value & bit) != 0;
        ok = bit != 0 && on == (arg(0) != 0.0f);
        got = std::format("{} {} is {} (word {:#x})", what, word(1), on ? 1 : 0, value);
    } else if (what == "gimmickmotion") {
        const Gimmick* gimmick = game.Objects().FindGimmick(word(1));
        ok = gimmick && gimmick->motion == word(2);
        got = std::format("gimmick {} motion {}", word(1), gimmick ? gimmick->motion : "missing");
    } else if (what == "gimmick") {
        const Gimmick* g = game.Objects().FindGimmick(word(1));
        const bool drawn = game.Objects().GimmickDrawn(word(1));
        ok = g && drawn == (arg(0) != 0.0f);
        got = !g ? std::format("gimmick {} missing", word(1))
                 : std::format("gimmick {} drawn {} (enabled {} shown {} placed {} active {} mesh {} views {} hidden meshes {} motion {})",
                               word(1), drawn ? 1 : 0, g->enabled ? 1 : 0, g->shown ? 1 : 0, g->placed ? 1 : 0, g->active ? 1 : 0,
                               g->mesh ? 1 : 0, g->hidden_views, g->hidden_meshes.size(), g->motion);
    } else if (what == "ocho") {
        ok = game.Objects().Ocho().State() == static_cast<int>(arg(0));
        got = std::format("ocho state {} visible {} killed {}", game.Objects().Ocho().State(), game.Objects().Ocho().Visible(),
                          game.Objects().Ocho().HasKilled());
    } else if (what == "ocholook") {
        ok = game.Objects().Ocho().LookPhase() == static_cast<int>(arg(0));
        got = std::format("ocho look-back phase {}", game.Objects().Ocho().LookPhase());
    } else if (what == "gameplus") {
        const bool shown = game.GamePlusTubShown();
        ok = shown == (arg(0) != 0.0f);
        got = std::format("game+ tub lisa {}", shown ? "shown" : "not shown");
    } else if (what == "body") {
        int found = -1;
        game.Stages().ForEachStage([&](Stage& stage) {
            for (const auto& file : stage.files) {
                if (const fox2::Entity* e = file->file->ByShortName(word(1)); e && found < 0) {
                    found = stage.Body(e).visible ? 1 : 0;
                }
            }
        });
        ok = found == static_cast<int>(arg(0));
        got = std::format("body {} {}", word(1), found < 0 ? std::string("missing") : found ? std::string("visible") : std::string("hidden"));
    } else if (what == "hello") {
        const uint32_t value = game.Nazo().Word(NazoId::Hello);
        ok = value == static_cast<uint32_t>(arg(0));
        got = std::format("hello word {}", value);
    } else if (what == "speech") {
        const bool shown = GameUi::Active() && GameUi::Active()->SpeechShown();
        ok = shown == (arg(0) != 0.0f);
        got = std::format("speech {}", shown ? 1 : 0);
    } else if (what == "handylight") {
        ok = game.GetPlayer().handy_light.enable == (arg(0) != 0.0f);
        got = std::format("handy light {}", game.GetPlayer().handy_light.enable ? 1 : 0);
    } else if (what == "pause") {
        ok = input.pause == (arg(0) != 0.0f);
        got = std::format("pause {}", input.pause ? 1 : 0);
    } else if (what == "lstick" || what == "rstick") {
        const glm::vec2 v = what == "lstick" ? input.left_stick : input.right_stick;
        ok = glm::length(v - glm::vec2(arg(0), arg(1))) <= arg(2, 0.02f);
        got = std::format("{} ({:.3f} {:.3f})", what, v.x, v.y);
    } else if (what == "pads") {
        const size_t count = device_ ? device_->GamepadCount() : 0;
        ok = count == static_cast<size_t>(arg(0));
        got = std::format("{} gamepads open", count);
    } else if (what == "rumble" || what == "norumble") {
        const int slot = static_cast<int>(arg(0));
        const VirtualRumble r = pads_ ? pads_->TakeRumble(slot) : VirtualRumble{};
        if (what == "rumble") {
            ok = r.peak_low / 257 >= static_cast<int>(arg(1)) && r.peak_high / 257 >= static_cast<int>(arg(2));
        } else {
            ok = r.peak_low == 0 && r.peak_high == 0 && r.low == 0 && r.high == 0;
        }
        got = std::format("pad {} rumble now ({} {}), peak ({} {}), {} calls", slot, r.low / 257, r.high / 257, r.peak_low / 257, r.peak_high / 257,
                          r.calls);
    } else if (what == "menu") {
        const bool open = GameUi::Active() && GameUi::Active()->MenuOpen();
        ok = open == (arg(0) != 0.0f);
        got = std::format("menu {}", open ? "open" : "closed");
    } else if (what == "paused") {
        ok = game.Paused() == (arg(0) != 0.0f);
        got = std::format("paused {}", game.Paused() ? 1 : 0);
    } else if (what == "page") {
        const int page = GameUi::Active() && GameUi::Active()->Menu().CurrentPage() == OptionsMenu::Page::Pc ? 1 : 0;
        ok = page == static_cast<int>(arg(0));
        got = std::format("menu page {}", page);
    } else if (what == "archive") {
        const ArchiveEntry* entry = FindArchiveEntry(word(1));
        const bool open = entry && game.ArchiveUnlocked(*entry);
        ok = entry && open == (arg(0) != 0.0f);
        got = std::format("archive entry {} {}", word(1), !entry ? "missing" : open ? "open" : "locked");
    } else if (what == "viewer") {
        const bool active = game.ArchiveTheaterActive();
        ok = active == (arg(0) != 0.0f);
        got = std::format("archive viewer {}", active ? "open" : "closed");
    } else if (what == "unlocked") {
        const int index = static_cast<int>(arg(0));
        ok = game.BrowseUnlocked(index) == (arg(1) != 0.0f);
        got = std::format("loop browser entry {} {}", index, game.BrowseUnlocked(index) ? "unlocked" : "locked");
    } else if (what == "pcenabled") {
        const PcSettingRow* row = GameUi::Active() ? GameUi::Active()->Menu().PcPage().FindRow(word(1)) : nullptr;
        ok = row && row->enabled == (arg(0) != 0.0f);
        got = std::format("PC setting {} {}", word(1), !row ? "missing" : row->enabled ? "enabled" : "disabled");
    } else if (what == "pcrow" || what == "pcvalue" || what == "pcnote" || what == "pcshown") {
        const PcSettingsPage* page = GameUi::Active() ? &GameUi::Active()->Menu().PcPage() : nullptr;
        const PcSettingRow* row = !page ? nullptr : what == "pcrow" ? page->CurrentRow() : page->FindRow(word(1));
        if (what == "pcshown") {
            const int shown = row ? page->ShownValue(*row) : -1;
            const std::string note = row ? std::string(page->ShownNote(*row)) : std::string("missing");
            ok = row && shown == static_cast<int>(arg(0)) && note == word(2);
            got = std::format("PC setting {} shows {} ({}), greyed {}, note {}", word(1), shown,
                              row && shown >= 0 && shown < static_cast<int>(row->values.size()) ? row->values[shown] : std::string(),
                              row && row->ValueDisabled(shown), note);
        } else if(what == "pcnote") {
            ok=row && row->note==word(2);
            got=std::format("PC setting {} note {}",word(1),row?row->note:std::string("missing"));
        } else if (what == "pcrow") {
            ok = row && row->label == word(1);
            got = std::format("PC settings cursor on {}", row ? row->label : std::string("nothing"));
        } else {
            ok = row && row->value == static_cast<int>(arg(0));
            const int shown = row ? std::clamp(row->value, 0, std::max(0, static_cast<int>(row->values.size()) - 1)) : 0;
            got = std::format("PC setting {} = {} ({})", word(1), row ? row->value : -1,
                              row && !row->values.empty() ? row->values[shown] : std::string());
        }
    } else if (what == "prompts") {
        const PromptDevice shown = GameUi::Active() ? GameUi::Active()->Menu().Prompts() : PromptDevice::Keyboard;
        std::string name = PromptDeviceName(shown);
        std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        ok = name == word(1);
        got = std::format("option menu prompts {}", name);
    } else if (what == "invert") {
        const bool on = word(1) == "x" ? game.Options().invert_x : game.Options().invert_y;
        ok = on == (arg(0) != 0.0f);
        got = std::format("invert {} {}", word(1), on ? 1 : 0);
    } else if (what == "mirrorbits") {
        ok = static_cast<int>(game.MirrorViewportBits()) == static_cast<int>(arg(0));
        got = std::format("mirror bits {:#x}", game.MirrorViewportBits());
    } else if (what == "brightness") {
        ok = game.Options().brightness == static_cast<int>(arg(0));
        got = std::format("brightness {}", game.Options().brightness);
    } else if (what == "subtitles") {
        const int value = game.Options().subtitles ? game.Options().subtitle_language : -1;
        ok = value == static_cast<int>(arg(0));
        got = std::format("subtitle language {}", value);
    } else {
        got = std::format("unknown expectation '{}'", what);
    }
    ++expectations_;
    if (ok) {
        LogInfo("expect ok frame {}: {}", frame, got);
    } else {
        ++failures_;
        LogError("expect FAILED frame {}: {} (wanted {})", frame, got, args.empty() ? std::string() : std::format("{}", args.front()));
    }
    return ok;
}

void InputScript::SessionState(Game& game, const std::string& mode, uint64_t frame) {
    std::string now = game.DescribeSessionState();
    if (const GameUi* ui = GameUi::Active()) {
        now += std::format("ui speech: {}\n", ui->SpeechShown());
    }
    if (mode == "save") {
        saved_state_ = now;
        LogInfo("input script frame {}: session state saved\n{}", frame, now);
        return;
    }
    auto split = [](const std::string& text) {
        std::vector<std::string> lines;
        size_t start = 0;
        for (size_t end = text.find('\n'); end != std::string::npos; start = end + 1, end = text.find('\n', start)) {
            lines.push_back(text.substr(start, end - start));
        }
        return lines;
    };
    const std::vector<std::string> was = split(saved_state_);
    const std::vector<std::string> is = split(now);
    int differences = 0;
    for (size_t i = 0; i < std::max(was.size(), is.size()); ++i) {
        const std::string a = i < was.size() ? was[i] : std::string("(none)");
        const std::string b = i < is.size() ? is[i] : std::string("(none)");
        if (a != b) {
            ++differences;
            LogError("session state differs: saved '{}' now '{}'", a, b);
        }
    }
    ++expectations_;
    if (differences == 0 && !saved_state_.empty()) {
        LogInfo("expect ok frame {}: session state as saved ({} lines)", frame, is.size());
    } else {
        ++failures_;
        LogError("expect FAILED frame {}: session state, {} lines differ{}", frame, differences, saved_state_.empty() ? " (nothing saved)" : "");
    }
}

bool InputScript::FacePoint(Game& game, Waypoint& item, glm::vec3& point) {
    if (item.op == "face") {
        point = item.point;
        return true;
    }
    if (item.op == "face_lisa") {
        point = glm::vec3(game.Objects().Ocho().World()[3]) + glm::vec3(0.0f, 1.5f, 0.0f);
        return true;
    }
    glm::mat4 world(1.0f);
    if (!EntityTransform(game, item.text, world)) {
        LogWarn("input script: entity {} not found", item.text);
        return false;
    }
    if (item.op == "fent") {
        point = glm::vec3(world[3]);
        return true;
    }
    glm::vec3 d(world[2][0], 0.0f, world[2][2]);
    if (glm::length(d) < 1e-4f) {
        return false;
    }
    point = game.GetPlayer().Eye() + glm::normalize(d) * 5.0f;
    return true;
}

bool InputScript::BotGoto(uint64_t frame, Game& game, Waypoint& item, BotOutput& out) {
    Player& player = game.GetPlayer();
    const glm::vec3 feet = player.Feet();
    glm::vec3 d = item.point - feet;
    d.y = 0.0f;
    const float distance = glm::length(d);
    if (distance < 0.35f) {
        LogInfo("input script: pad reached ({:.2f} {:.2f} {:.2f}) at frame {}", feet.x, feet.y, feet.z, frame);
        stuck_frames_ = 0;
        return false;
    }
    if (item.frames > 0 && --item.frames == 0) {
        LogInfo("input script: pad gave up on ({:.2f} {:.2f} {:.2f}) at ({:.2f} {:.2f} {:.2f}), frame {}", item.point.x, item.point.y, item.point.z,
                feet.x, feet.y, feet.z, frame);
        return false;
    }
    const float error = WrapAngle(std::atan2(-d.x, -d.z) - player.yaw);
    out.right.x = TurnStick(error, 0.05f) * (game.Options().invert_x ? -1.0f : 1.0f);
    const glm::vec3 forward = player.BodyForward();
    const glm::vec3 right(-forward.z, 0.0f, forward.x);
    const glm::vec3 dir = d / distance;
    out.left = glm::vec2(glm::dot(dir, right), glm::dot(dir, forward));
    stuck_frames_ = glm::length(feet - last_position_) < 0.001f ? stuck_frames_ + 1 : 0;
    if (stuck_frames_ == 120) {
        LogWarn("input script: pad stuck at ({:.2f} {:.2f} {:.2f}) going to ({:.2f} {:.2f} {:.2f})", feet.x, feet.y, feet.z, item.point.x,
                item.point.y, item.point.z);
    }
    last_position_ = feet;
    return true;
}

bool InputScript::BotFace(Game& game, Waypoint& item, BotOutput& out) {
    glm::vec3 point(0.0f);
    if (!FacePoint(game, item, point)) {
        return false;
    }
    Player& player = game.GetPlayer();
    const glm::vec3 d = point - player.Eye();
    const float yaw_error = WrapAngle(std::atan2(-d.x, -d.z) - player.yaw);
    const float pitch_error = Player::ClampPitch(std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z))) - player.pitch;
    const bool still = std::abs(player.yaw - bot_last_yaw_) < 0.002f;
    bot_last_yaw_ = player.yaw;
    if (std::abs(yaw_error) < kBotFaceYaw && std::abs(pitch_error) < kBotFacePitch) {
        if (still && ++bot_settle_ >= 3) {
            bot_settle_ = 0;
            LogInfo("input script: pad faces {} (yaw error {:.2f}, pitch error {:.2f} degrees)", item.op == "face" ? std::string("point") : item.text,
                    glm::degrees(yaw_error), glm::degrees(pitch_error));
            return false;
        }
        return true;
    }
    bot_settle_ = 0;
    out.right.x = TurnStick(yaw_error, kBotFaceYaw * 0.5f) * (game.Options().invert_x ? -1.0f : 1.0f);
    out.right.y = TurnStick(pitch_error, kBotFacePitch * 0.5f) * (game.Options().invert_y ? -1.0f : 1.0f);
    return true;
}

bool InputScript::ArchiveWait(Game& game, Waypoint& item) {
    if (item.op == "archive") {
        game.RequestArchive(item.text);
        LogInfo("input script: archive entry {} requested", item.text);
        return false;
    }
    if (item.op == "archiveshown") {
        if (game.ArchiveTheaterShowing()) {
            LogInfo("input script: archive viewer showing");
            return false;
        }
        if (!game.ArchiveRequestPending() && !game.ArchiveTheaterActive()) {
            LogWarn("input script: sarchiveshown: no viewer");
            return false;
        }
    } else if (!game.ArchiveRequestPending() && !game.ArchiveTheaterActive()) {
        LogInfo("input script: archive viewer ended");
        return false;
    }
    if (--item.frames > 0) {
        return true;
    }
    LogWarn("input script: {} gave up", item.op == "archiveshown" ? "sarchiveshown" : "sarchived");
    return false;
}

bool InputScript::BrowseWait(Game& game, Waypoint& item) {
    if (item.op == "browse_loop") {
        if (item.text.empty() || item.text == "refused") {
            const bool picked = game.BrowseLoop(item.frames);
            if (!picked && ++item.extra.x < 1200.0f) {
                item.text = "refused";
                return true;
            }
            item.text = picked ? "picked" : "given up";
            if (!picked) {
                LogWarn("input script: loop {} refused at controller step {}", item.frames, game.Controller().Step());
                skip_next_shot_ = true;
            }
        }
        return game.LoopReloadPending();
    }
    if (item.op == "at") {
        const int step = game.Controller().Step();
        const std::string& floor = game.Floor().CurrentFloorName();
        if (step != 15 || floor != item.text || game.Floor().LoopCount() != item.frames) {
            LogWarn("input script: at step {} floor {} loop {}, expected {} loop {}, next shot dropped", step, floor, game.Floor().LoopCount(),
                    item.text, item.frames);
            skip_next_shot_ = true;
        } else {
            LogInfo("input script: at {} loop {}", floor, item.frames);
        }
        return false;
    }
    const bool done = item.op == "browsed" ? game.BrowseArrived() : !game.Effects().IsFadeProcessing() && game.Effects().FadeShown().a < 0.004f;
    if (done) {
        return false;
    }
    if (--item.frames > 0) {
        return true;
    }
    LogWarn("input script: s{} gave up at controller step {} floor {}", item.op, game.Controller().Step(), game.Floor().CurrentFloorName());
    skip_next_shot_ = true;
    return false;
}

void InputScript::SendBot(const BotOutput& out) {
    if (!pads_ || bot_slot_ < 0) {
        return;
    }
    const BotOutput& p = bot_previous_;
    if (!bot_sent_ || out.left != p.left) {
        pads_->SetAxis(bot_slot_, "lx", out.left.x);
        pads_->SetAxis(bot_slot_, "ly", -out.left.y);
    }
    if (!bot_sent_ || out.right != p.right) {
        pads_->SetAxis(bot_slot_, "rx", out.right.x);
        pads_->SetAxis(bot_slot_, "ry", out.right.y);
    }
    if (!bot_sent_ || out.cross != p.cross) {
        pads_->SetButton(bot_slot_, "cross", out.cross);
    }
    if (!bot_sent_ || out.r3 != p.r3) {
        pads_->SetButton(bot_slot_, "r3", out.r3);
    }
    bot_previous_ = out;
    bot_sent_ = true;
}

void InputScript::ApplyBot(uint64_t frame, Game& game, const InputState& input) {
    BotOutput out;
    bool steering = false;
    while (!waypoints_.empty()) {
        Waypoint& item = waypoints_.front();
        Resolve(game, item);
        const std::string op = item.op;
        if (op == "goto_lisa") {
            item.op = "goto";
            item.point = glm::vec3(game.Objects().Ocho().World()[3]);
            continue;
        }
        if (op == "goto") {
            if (BotGoto(frame, game, item, out)) {
                steering = true;
                break;
            }
        } else if (op == "face" || op == "fent" || op == "fdir" || op == "face_lisa") {
            if (BotFace(game, item, out)) {
                steering = true;
                break;
            }
        } else if (op == "stick") {
            if (item.frames-- > 0) {
                out.left = item.stick;
                steering = true;
                break;
            }
        } else if (op == "rstick") {
            if (item.frames-- > 0) {
                out.right = item.stick;
                steering = true;
                break;
            }
        } else if (op == "trace") {
            trace_frames_ = item.frames;
        } else if (op == "steps") {
            if (item.text.empty()) {
                item.text = std::to_string(game.GetPlayer().foot_steps);
            }
            if (game.GetPlayer().foot_steps - std::stoi(item.text) < item.frames) {
                out.left = glm::vec2(0.0f, 1.0f);
                steering = true;
                break;
            }
            LogInfo("input script: pad walked {} footsteps", item.frames);
        } else if (op == "wait") {
            if (--item.frames > 0) {
                break;
            }
        } else if (op == "free") {
            if (game.Demos().ControlsPlayer()) {
                break;
            }
        } else if (op == "smenu" || op == "spaused") {
            const bool open = op == "smenu" ? GameUi::Active() && GameUi::Active()->MenuOpen() : game.Paused();
            if (open != (!item.args.empty() && item.args[0] != 0.0f)) {
                break;
            }
        } else if (op == "sfocus") {
            seq_focus_ = !item.args.empty() && item.args[0] != 0.0f;
            seq_focus_frame_ = frame;
            seq_focus_until_ = item.args.size() > 1 && item.args[1] > 0.0f ? frame + static_cast<uint64_t>(item.args[1]) : 0;
        } else if (op == "stap" || op == "sbutton" || op == "saxis" || op == "stouch" || op == "sexpect" || op == "ssound") {
            PadCommand("p" + op.substr(1), item.words, item.args, frame, game, input);
        } else if (op == "lisa") {
            if (!game.Objects().Ocho().Visible() && (item.frames <= 0 || --item.frames > 0)) {
                break;
            }
            const glm::vec3 lisa(game.Objects().Ocho().World()[3]);
            LogInfo("input script: Lisa {} (state {}, spawn {}) at ({:.2f} {:.2f} {:.2f})", game.Objects().Ocho().Visible() ? "visible" : "not seen",
                    game.Objects().Ocho().State(), game.Objects().Ocho().SpawnIndex(), lisa.x, lisa.y, lisa.z);
        } else if (op == "browse_loop" || op == "browsed" || op == "fade" || op == "at") {
            if (BrowseWait(game, item)) {
                break;
            }
        } else if (op == "archive" || op == "archived" || op == "archiveshown") {
            if (ArchiveWait(game, item)) {
                break;
            }
        } else if (op == "step") {
            if (game.Controller().Step() != item.frames) {
                break;
            }
        } else if (op == "demo") {
            if (game.Demos().IsPlaying(item.text)) {
                break;
            }
        } else if (op == "dframe") {
            if (!game.Demos().IsPlaying(item.text) || game.Demos().PlayTime(item.text) * 59.94 < item.frames) {
                break;
            }
        } else if (op == "floor") {
            if (!game.Floor().IsCurrentFloorName(item.text)) {
                break;
            }
        } else if (op == "quit") {
            game.RequestQuit();
        } else if (op == "shot" && std::exchange(skip_next_shot_, false)) {
            LogWarn("input script: shot {} dropped, loop not reached", item.text);
        } else if (op == "shot") {
            std::string path = item.text;
            for (const auto& [key, value] : {std::pair<std::string, std::string>{"{floor}", game.Floor().CurrentFloorName()},
                                             {"{loop}", std::to_string(game.Floor().LoopCount())}}) {
                for (size_t at = path.find(key); at != std::string::npos; at = path.find(key)) {
                    path.replace(at, key.size(), value);
                }
            }
            game.RequestScreenshot(path);
        } else if (op == "anchor") {
            const Stage* stage = item.text.empty() ? NearestHallway(game) : game.Stages().Find(item.text);
            if (stage) {
                file_transform_ = stage->file_to_world;
                file_stage_ = stage->id;
                LogInfo("input script: file frame anchored to stage {} ({})", stage->id, stage->label);
            } else {
                LogWarn("input script: no stage to anchor ({})", item.text);
            }
        } else if (op == "action") {
            bot_cross_ = 3;
        } else if (op == "zoom") {
            bot_zoom_ = item.frames != 0;
        } else if (op == "voice") {
            game.OnVoiceKeyword(item.text.empty() ? "jack" : item.text);
        } else if (op == "log") {
            const glm::vec3 p = game.GetPlayer().Feet();
            LogInfo("input script pad frame {}: step {} floor {} loop {} feet ({:.2f} {:.2f} {:.2f}) yaw {:.1f} pitch {:.1f} footsteps {}", frame,
                    game.Controller().Step(), game.Floor().CurrentFloorName(), game.Floor().LoopCount(), p.x, p.y, p.z,
                    glm::degrees(game.GetPlayer().yaw), glm::degrees(game.GetPlayer().pitch), game.GetPlayer().foot_steps);
            const Camera camera = game.GetCamera();
            const glm::vec3 forward = camera.Forward();
            LogInfo("input script camera: pos ({:.4f} {:.4f} {:.4f}) fwd ({:.4f} {:.4f} {:.4f})", camera.position.x,
                    camera.position.y, camera.position.z, forward.x, forward.y, forward.z);
        } else {
            LogWarn("input script: pad bot skips {}, would bypass the pad", op);
        }
        waypoints_.erase(waypoints_.begin());
        if (op == "action") {
            break;
        }
    }
    if (!steering && glm::length(move_) > 0.0f) {
        out.left = move_;
    }
    out.r3 = bot_zoom_;
    if (bot_cross_ > 0) {
        out.cross = true;
        --bot_cross_;
    }
    SendBot(out);
}

}
