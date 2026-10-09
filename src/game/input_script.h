#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/platform/input.h"

namespace pt {
class VirtualPads;
}

namespace pt::game {

class Game;
struct Stage;

class InputScript {
public:
    bool Load(const std::filesystem::path& path);
    bool Parse(const std::string& text);
    void Apply(uint64_t frame, Game& game, InputState& input);
    bool Empty() const { return commands_.empty(); }
    // every command was applied and no sequenced item waits (the Archive's theater runs its own walks with a script)
    bool Idle() const { return next_ >= commands_.size() && waypoints_.empty(); }
    bool UsesPads() const;
    std::optional<bool> ForcedFocus(uint64_t frame) const;
    void SetPads(VirtualPads* pads, InputDevice* device) {
        pads_ = pads;
        device_ = device;
    }
    int Expectations() const { return expectations_; }
    int Failures() const { return failures_; }

private:
    const Stage* NearestHallway(Game& game) const;
    void Face(Game& game, const glm::vec3& point);
    void SetScriptCamera(Game& game, const glm::vec3& position, const glm::vec4& forward_fov, const glm::vec3& requested_up,
                         bool follows_view = false);
    bool EntityTransform(Game& game, const std::string& name, glm::mat4& out) const;

    struct Command {
        uint64_t frame = 0;
        std::string op;
        std::vector<float> args;
        std::string text;
        std::vector<std::string> words;
    };

    std::vector<Command> commands_;
    size_t next_ = 0;
    glm::vec2 move_{0.0f};
    bool zoom_ = false;
    bool action_ = false;
    uint32_t previous_held_ = 0;
    struct Waypoint {
        glm::vec3 point{0.0f};
        bool relative = false;
        bool reanchor = false;
        std::string op = "goto";
        int frames = 0;
        std::string text;
        bool file_space = false;
        glm::vec2 stick{0.0f};
        glm::vec4 extra{0.0f};
        glm::vec3 up{0.0f};
        std::vector<std::string> words;
        std::vector<float> args;
    };
    std::vector<Waypoint> waypoints_;
    // the loop browser's queue items (sloop, sbrowsed, sfade): true while the item still waits
    bool BrowseWait(Game& game, Waypoint& item);
    // sarchive and sarchived (the Archive's viewers)
    bool ArchiveWait(Game& game, Waypoint& item);
    // a pick that was refused or a wait that gave up: the next sshot is dropped, so no picture of another place is kept under its name
    bool skip_next_shot_ = false;
    // `sstate save`: the session state (Game::DescribeSessionState) a later `sstate compare` must find again
    std::string saved_state_;
    void Resolve(Game& game, Waypoint& item);
    bool ApplyGoto(uint64_t frame, Game& game, InputState& input);
    glm::mat4 anchor_inverse_{1.0f};
    glm::mat4 loop_transform_{1.0f};
    glm::mat4 file_transform_{1.0f};
    uint32_t file_stage_ = 0;
    bool anchored_ = false;
    uint64_t stuck_frames_ = 0;
    glm::vec3 last_position_{0.0f};
    // sfeet: the feet of the previous tick, the ticks the player has stood still since and the ticks waited
    glm::vec3 eye_last_feet_{0.0f};
    int eye_still_ = 0;
    int eye_wait_ = 0;
    int trace_frames_ = 0;
    // shold: the frame until which the game is held (paused) once it gives control (StartGame ran, controller step 15); a later
    // control moves the script's remaining lines by the difference
    uint64_t hold_until_ = 0;
    bool holding_ = false;
    bool hold_done_ = false;
    std::optional<bool> seq_focus_;
    uint64_t seq_focus_frame_ = 0;
    uint64_t seq_focus_until_ = 0;

    struct BotOutput {
        glm::vec2 left{0.0f};
        glm::vec2 right{0.0f};
        bool cross = false;
        bool r3 = false;
    };
    void PadCommand(const std::string& op, const std::vector<std::string>& words, const std::vector<float>& args, uint64_t frame, Game& game,
                    const InputState& input);
    bool Expect(const std::vector<std::string>& words, const std::vector<float>& args, uint64_t frame, Game& game, const InputState& input);
    void ApplyBot(uint64_t frame, Game& game, const InputState& input);
    bool BotGoto(uint64_t frame, Game& game, Waypoint& item, BotOutput& out);
    bool BotFace(Game& game, Waypoint& item, BotOutput& out);
    void SessionState(Game& game, const std::string& mode, uint64_t frame);
    bool FacePoint(Game& game, Waypoint& item, glm::vec3& point);
    void SendBot(const BotOutput& out);

    VirtualPads* pads_ = nullptr;
    InputDevice* device_ = nullptr;
    int bot_slot_ = -1;
    bool bot_zoom_ = false;
    int bot_cross_ = 0;
    int bot_settle_ = 0;
    float bot_last_yaw_ = 0.0f;
    bool bot_sent_ = false;
    BotOutput bot_previous_;
    struct Release {
        uint64_t frame = 0;
        int slot = 0;
        std::string button;
    };
    std::vector<Release> releases_;
    // sburst: screenshots left, the next one's index, the path prefix
    int burst_left_ = 0;
    int burst_index_ = 0;
    std::string burst_prefix_;
    // key and mouse taps (ktap, sktap): scancode, or 0x10000 + mouse button
    std::vector<std::pair<uint64_t, uint32_t>> key_releases_;
    bool KeyCommand(const std::string& op, const std::vector<std::string>& words, const std::vector<float>& args, uint64_t frame);
    int expectations_ = 0;
    int failures_ = 0;
};

}
